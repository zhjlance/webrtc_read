/*
 *  Copyright (c) 2019 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "modules/pacing/pacing_controller.h"

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include "modules/pacing/bitrate_prober.h"
#include "modules/pacing/interval_budget.h"
#include "modules/utility/include/process_thread.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"
#include "rtc_base/time_utils.h"
#include "system_wrappers/include/clock.h"

namespace webrtc {
namespace {
// Time limit in milliseconds between packet bursts.
constexpr TimeDelta kDefaultMinPacketLimit = TimeDelta::Millis(5);
constexpr TimeDelta kCongestedPacketInterval = TimeDelta::Millis(500);
// TODO(sprang): Consider dropping this limit.
// The maximum debt level, in terms of time, capped when sending packets.
constexpr TimeDelta kMaxDebtInTime = TimeDelta::Millis(500);
constexpr TimeDelta kMaxElapsedTime = TimeDelta::Seconds(2);
constexpr DataSize kDefaultPaddingTarget = DataSize::Bytes(50);

// Upper cap on process interval, in case process has not been called in a long
// time.
constexpr TimeDelta kMaxProcessingInterval = TimeDelta::Millis(30);

constexpr int kFirstPriority = 0;

bool IsDisabled(const WebRtcKeyValueConfig& field_trials,
                absl::string_view key) {
  return field_trials.Lookup(key).find("Disabled") == 0;
}

bool IsEnabled(const WebRtcKeyValueConfig& field_trials,
               absl::string_view key) {
  return field_trials.Lookup(key).find("Enabled") == 0;
}

int GetPriorityForType(RtpPacketMediaType type) {
  // Lower number takes priority over higher.
  switch (type) {
    case RtpPacketMediaType::kAudio:
      // Audio is always prioritized over other packet types.
      return kFirstPriority + 1;
    case RtpPacketMediaType::kRetransmission:
      // Send retransmissions before new media.
      return kFirstPriority + 2;
    case RtpPacketMediaType::kVideo:
    case RtpPacketMediaType::kForwardErrorCorrection:
      // Video has "normal" priority, in the old speak.
      // Send redundancy concurrently to video. If it is delayed it might have a
      // lower chance of being useful.
      return kFirstPriority + 3;
    case RtpPacketMediaType::kPadding:
      // Packets that are in themselves likely useless, only sent to keep the
      // BWE high.
      return kFirstPriority + 4;
  }
}

}  // namespace

const TimeDelta PacingController::kMaxExpectedQueueLength =
    TimeDelta::Millis(2000);
const float PacingController::kDefaultPaceMultiplier = 2.5f;
const TimeDelta PacingController::kPausedProcessInterval =
    kCongestedPacketInterval;
const TimeDelta PacingController::kMinSleepTime = TimeDelta::Millis(1);

PacingController::PacingController(Clock* clock,
                                   PacketSender* packet_sender,
                                   RtcEventLog* event_log,
                                   const WebRtcKeyValueConfig* field_trials,
                                   ProcessMode mode)
    : mode_(mode),
      clock_(clock),
      packet_sender_(packet_sender),
      fallback_field_trials_(
          !field_trials ? std::make_unique<FieldTrialBasedConfig>() : nullptr),
      field_trials_(field_trials ? field_trials : fallback_field_trials_.get()),
      drain_large_queues_(
          !IsDisabled(*field_trials_, "WebRTC-Pacer-DrainQueue")),
      send_padding_if_silent_(
          IsEnabled(*field_trials_, "WebRTC-Pacer-PadInSilence")),
      pace_audio_(IsEnabled(*field_trials_, "WebRTC-Pacer-BlockAudio")),
      small_first_probe_packet_(
          IsEnabled(*field_trials_, "WebRTC-Pacer-SmallFirstProbePacket")),
      ignore_transport_overhead_(
          IsEnabled(*field_trials_, "WebRTC-Pacer-IgnoreTransportOverhead")),
      min_packet_limit_(kDefaultMinPacketLimit),
      transport_overhead_per_packet_(DataSize::Zero()),
      last_timestamp_(clock_->CurrentTime()),
      paused_(false),
      media_budget_(0),
      padding_budget_(0),
      media_debt_(DataSize::Zero()),
      padding_debt_(DataSize::Zero()),
      media_rate_(DataRate::Zero()),
      padding_rate_(DataRate::Zero()),
      prober_(*field_trials_),
      probing_send_failure_(false),
      pacing_bitrate_(DataRate::Zero()),
      last_process_time_(clock->CurrentTime()),
      last_send_time_(last_process_time_),
      packet_queue_(last_process_time_, field_trials_),
      packet_counter_(0),
      congestion_window_size_(DataSize::PlusInfinity()),
      outstanding_data_(DataSize::Zero()),
      queue_time_limit(kMaxExpectedQueueLength),
      account_for_audio_(false),
      include_overhead_(false) {
  if (!drain_large_queues_) {
    RTC_LOG(LS_WARNING) << "Pacer queues will not be drained,"
                           "pushback experiment must be enabled.";
  }
  FieldTrialParameter<int> min_packet_limit_ms("", min_packet_limit_.ms());
  ParseFieldTrial({&min_packet_limit_ms},
                  field_trials_->Lookup("WebRTC-Pacer-MinPacketLimitMs"));
  min_packet_limit_ = TimeDelta::Millis(min_packet_limit_ms.Get());
  UpdateBudgetWithElapsedTime(min_packet_limit_);
}

PacingController::~PacingController() = default;

void PacingController::CreateProbeCluster(DataRate bitrate, int cluster_id) {
  prober_.CreateProbeCluster(bitrate, CurrentTime(), cluster_id);
}

void PacingController::Pause() {
  if (!paused_)
    RTC_LOG(LS_INFO) << "PacedSender paused.";
  paused_ = true;
  packet_queue_.SetPauseState(true, CurrentTime());
}

void PacingController::Resume() {
  if (paused_)
    RTC_LOG(LS_INFO) << "PacedSender resumed.";
  paused_ = false;
  packet_queue_.SetPauseState(false, CurrentTime());
}

bool PacingController::IsPaused() const {
  return paused_;
}

void PacingController::SetCongestionWindow(DataSize congestion_window_size) {
  const bool was_congested = Congested();
  congestion_window_size_ = congestion_window_size;
  if (was_congested && !Congested()) {
    TimeDelta elapsed_time = UpdateTimeAndGetElapsed(CurrentTime());
    UpdateBudgetWithElapsedTime(elapsed_time);
  }
}

void PacingController::UpdateOutstandingData(DataSize outstanding_data) {
  const bool was_congested = Congested();
  outstanding_data_ = outstanding_data;
  if (was_congested && !Congested()) {
    TimeDelta elapsed_time = UpdateTimeAndGetElapsed(CurrentTime());
    UpdateBudgetWithElapsedTime(elapsed_time);
  }
}

bool PacingController::Congested() const {
  if (congestion_window_size_.IsFinite()) {
    return outstanding_data_ >= congestion_window_size_;
  }
  return false;
}

Timestamp PacingController::CurrentTime() const {
  Timestamp time = clock_->CurrentTime();
  if (time < last_timestamp_) {
    RTC_LOG(LS_WARNING)
        << "Non-monotonic clock behavior observed. Previous timestamp: "
        << last_timestamp_.ms() << ", new timestamp: " << time.ms();
    RTC_DCHECK_GE(time, last_timestamp_);
    time = last_timestamp_;
  }
  last_timestamp_ = time;
  return time;
}

void PacingController::SetProbingEnabled(bool enabled) {
  RTC_CHECK_EQ(0, packet_counter_);
  prober_.SetEnabled(enabled);
}

void PacingController::SetPacingRates(DataRate pacing_rate,
                                      DataRate padding_rate) {
  RTC_DCHECK_GT(pacing_rate, DataRate::Zero());
  media_rate_ = pacing_rate;
  padding_rate_ = padding_rate;
  pacing_bitrate_ = pacing_rate;
  padding_budget_.set_target_rate_kbps(padding_rate.kbps());

  RTC_LOG(LS_VERBOSE) << "bwe:pacer_updated pacing_kbps="
                      << pacing_bitrate_.kbps()
                      << " padding_budget_kbps=" << padding_rate.kbps();
}

void PacingController::EnqueuePacket(std::unique_ptr<RtpPacketToSend> packet) {
  RTC_DCHECK(pacing_bitrate_ > DataRate::Zero())
      << "SetPacingRate must be called before InsertPacket.";
  RTC_CHECK(packet->packet_type());
  // Get priority first and store in temporary, to avoid chance of object being
  // moved before GetPriorityForType() being called.
  const int priority = GetPriorityForType(*packet->packet_type());
  EnqueuePacketInternal(std::move(packet), priority);
}

void PacingController::SetAccountForAudioPackets(bool account_for_audio) {
  account_for_audio_ = account_for_audio;
}

void PacingController::SetIncludeOverhead() {
  include_overhead_ = true;
  packet_queue_.SetIncludeOverhead();
}

void PacingController::SetTransportOverhead(DataSize overhead_per_packet) {
  if (ignore_transport_overhead_)
    return;
  transport_overhead_per_packet_ = overhead_per_packet;
  packet_queue_.SetTransportOverhead(overhead_per_packet);
}

TimeDelta PacingController::ExpectedQueueTime() const {
  RTC_DCHECK_GT(pacing_bitrate_, DataRate::Zero());
  return TimeDelta::Millis(
      (QueueSizeData().bytes() * 8 * rtc::kNumMillisecsPerSec) /
      pacing_bitrate_.bps());
}

size_t PacingController::QueueSizePackets() const {
  return packet_queue_.SizeInPackets();
}

DataSize PacingController::QueueSizeData() const {
  return packet_queue_.Size();
}

DataSize PacingController::CurrentBufferLevel() const {
  return std::max(media_debt_, padding_debt_);
}

absl::optional<Timestamp> PacingController::FirstSentPacketTime() const {
  return first_sent_packet_time_;
}

TimeDelta PacingController::OldestPacketWaitTime() const {
  Timestamp oldest_packet = packet_queue_.OldestEnqueueTime();
  if (oldest_packet.IsInfinite()) {
    return TimeDelta::Zero();
  }

  return CurrentTime() - oldest_packet;
}
/**
 * 函数概述：
 *  1.PacingController::EnqueuePacketInternal 函数是 PacingController（速率控制控制器）类的内部函数，
 *    主要用于将数据包排入队列，并执行与速率控制相关的辅助操作。它处理数据包的入队逻辑，同时与带宽探测功能进行交互。
 * 
 * 使用场景：
 * 1.在 WebRTC 数据传输过程中，当需要将实时传输协议（RTP）数据包发送到网络之前，
 * 会调用此函数将数据包加入到发送队列中。这个过程会根据当前的速率控制模式以及队列状态进行处理，
 * 以确保数据包能够按照合适的节奏发送，避免网络拥塞，并配合带宽探测机制优化传输性能。
 */
void PacingController::EnqueuePacketInternal(
    std::unique_ptr<RtpPacketToSend> packet,
    int priority) {
  // 1.带宽探测交互：
  // 调用 prober_（带宽探测器）的 OnIncomingPacket 方法，并传入当前数据包的有效载荷大小。
  // 这一步将数据包的信息提供给带宽探测器，以便带宽探测器基于接收到的数据包大小等信息进行带宽估计和相关的探测逻辑。
  // 例如，带宽探测器可能会根据数据包大小和到达时间间隔来调整对网络带宽的估计。
  prober_.OnIncomingPacket(packet->payload_size());

  // TODO(sprang): Make sure tests respect this, replace with DCHECK.
  // 2.时间戳设置：
  // 获取当前时间 now。如果数据包的捕获时间 capture_time_ms 为负数（表示未设置或设置错误），则将其设置为当前时间。
  // 数据包的捕获时间在 WebRTC 的传输过程中有重要意义，它可以用于时间戳排序、抖动计算以及与接收端的同步等操作。
  Timestamp now = CurrentTime();
  if (packet->capture_time_ms() < 0) {
    packet->set_capture_time_ms(now.ms());
  }
  // 3.动态模式下的时间记录：
  // 当速率控制模式为动态模式（ProcessMode::kDynamic），并且数据包队列 packet_queue_ 为空，
  // 同时媒体数据负债（media_debt_，可能表示之前未发送完的媒体数据量）为零时，
  // 记录当前时间到 last_process_time_。这一步主要用于动态速率控制模式下跟踪数据包处理的时间点，以便后续计算发送间隔和速率调整。
  // 例如，根据 last_process_time_ 和当前时间的差值，以及队列中的数据包大小，
  // 可以计算出合适的发送速率，以避免网络拥塞并充分利用带宽。
  if (mode_ == ProcessMode::kDynamic && packet_queue_.Empty() &&
      media_debt_ == DataSize::Zero()) {
    last_process_time_ = CurrentTime();
  }
  // 4.数据包入队：
  // 将数据包 packet 排入 packet_queue_ 队列中。入队操作不仅将数据包本身放入队列，
  // 还附带了数据包的优先级 priority、当前时间 now 以及一个递增的数据包计数器 packet_counter_。
  // 通过这种方式，队列可以根据优先级等信息对数据包进行排序和管理，确保高优先级的数据包能够优先发送，
  // 从而满足不同类型数据（如关键控制信息、音频或视频数据等）在传输上的不同需求。
  // std::move 操作将 packet 的所有权转移到队列中，避免不必要的拷贝。
  packet_queue_.Push(priority, now, packet_counter_++, std::move(packet));
}

TimeDelta PacingController::UpdateTimeAndGetElapsed(Timestamp now) {
  if (last_process_time_.IsMinusInfinity()) {
    return TimeDelta::Zero();
  }
  RTC_DCHECK_GE(now, last_process_time_);
  TimeDelta elapsed_time = now - last_process_time_;
  last_process_time_ = now;
  if (elapsed_time > kMaxElapsedTime) {
    RTC_LOG(LS_WARNING) << "Elapsed time (" << elapsed_time.ms()
                        << " ms) longer than expected, limiting to "
                        << kMaxElapsedTime.ms();
    elapsed_time = kMaxElapsedTime;
  }
  return elapsed_time;
}
/**
 * 使用ShouldSendKeepalive()检查是否需要发送keepalive包, 
 * 判定的依据如下，如果需要则构造一个1Bytes的包发送。
 * 
 * 1.根据发送队列大小和packet能在队列存放的时间计算一个目标发送码率target_rate；
 * 2.从带宽探测器prober_中获取当前的探测码率，具体的prober原理可参考此篇
 * （https://blog.csdn.net/qq_22658119/article/details/119242152）, 
 *  通过发送包簇的发送码率和接受码率是否出现差值判断是否达到链路最大容量；
 * 3.循环扫发送队列，将队列的包通过packet_sender->SendPacket()进行发送，
 *   并通过packet_sender->FetchFec()获取已发送包的fec包，进行发送
 */
bool PacingController::ShouldSendKeepalive(Timestamp now) const {
  if (send_padding_if_silent_ || paused_ || Congested() ||
      packet_counter_ == 0) {
    // We send a padding packet every 500 ms to ensure we won't get stuck in
    // congested state due to no feedback being received.
    // 没有feedback过来就处于congested状态，则每500ms就会有一个keepalive探测包
    TimeDelta elapsed_since_last_send = now - last_send_time_;
    if (elapsed_since_last_send >= kCongestedPacketInterval) {
      return true;
    }
  }
  return false;
}
/**
 * Pacing模块当中，获取每次执行的时间(5ms)
 */
Timestamp PacingController::NextSendTime() const {
  Timestamp now = CurrentTime();

  if (paused_) {
    return last_send_time_ + kPausedProcessInterval;
  }

  // If probing is active, that always takes priority.
  if (prober_.IsProbing()) {
    Timestamp probe_time = prober_.NextProbeTime(now);
    // |probe_time| == PlusInfinity indicates no probe scheduled.
    if (probe_time != Timestamp::PlusInfinity() && !probing_send_failure_) {
      return probe_time;
    }
  }
  // kPeriodic模式（其实目前也仅仅支持这种周期模式）
  if (mode_ == ProcessMode::kPeriodic) {
    // In periodic non-probing mode, we just have a fixed interval.
    // 在周期性非探测模式下，我们只有一个固定间隔。就是使用上次处理的时间，加上min_packet_limit_（默认是5ms）
    return last_process_time_ + min_packet_limit_;
  }

  // In dynamic mode, figure out when the next packet should be sent,
  // given the current conditions.

  if (Congested() || packet_counter_ == 0) {
    // If congested, we only send keep-alive or audio (if audio is
    // configured in pass-through mode).
    if (!pace_audio_ && packet_queue_.NextPacketIsAudio()) {
      return now;
    }

    // We need to at least send keep-alive packets with some interval.
    return last_send_time_ + kCongestedPacketInterval;
  }

  // Check how long until media buffer has drained. We schedule a call
  // for when the last packet in the queue drains as otherwise we may
  // be late in starting padding.
  if (media_rate_ > DataRate::Zero() &&
      (!packet_queue_.Empty() || !media_debt_.IsZero())) {
    return std::min(last_send_time_ + kPausedProcessInterval,
                    last_process_time_ + media_debt_ / media_rate_);
  }

  // If we _don't_ have pending packets, check how long until we have
  // bandwidth for padding packets.
  if (padding_rate_ > DataRate::Zero() && packet_queue_.Empty()) {
    return std::min(last_send_time_ + kPausedProcessInterval,
                    last_process_time_ + padding_debt_ / padding_rate_);
  }

  if (send_padding_if_silent_) {
    return last_send_time_ + kPausedProcessInterval;
  }
  return last_process_time_ + kPausedProcessInterval;
}
/**
 * 周期发送数据包:
 * ProcessPackets() 中会从发送队列取包进行转发，其中增添了很多逻辑，用于带宽探测和发送速率控制,
 * 
 * 1.使用budget进行发送码率控制，budget会随着时间流逝而增长，随着发送而减小，budget在内部的维护上，
 *   又根据ProcessPackets()是被固定周期调用(ProcessMode::kPeriodic)使用media_budget_，还是
 *   被动态调用(ProcessMode::kDynamic) 则使用media_debt_维护;(动态目前没有人用)
 * 2.在入口使用UpdateBudgetWithElapsedTime()函数通过流逝的时间更新发送budget
 * */
void PacingController::ProcessPackets() {
  Timestamp now = CurrentTime();
  Timestamp target_send_time = now;
  // 目前只支持周期模式，这个动态模式用不到
  if (mode_ == ProcessMode::kDynamic) {
    target_send_time = NextSendTime();
    if (target_send_time.IsMinusInfinity()) {
      target_send_time = now;
    } else if (now < target_send_time) {
      // We are too early, abort and regroup!
      return;
    }

    if (target_send_time < last_process_time_) {
      // After the last process call, at time X, the target send time
      // shifted to be earlier than X. This should normally not happen
      // but we want to make sure rounding errors or erratic behavior
      // of NextSendTime() does not cause issue. In particular, if the
      // buffer reduction of
      // rate * (target_send_time - previous_process_time)
      // in the main loop doesn't clean up the existing debt we may not
      // be able to send again. We don't want to check this reordering
      // there as it is the normal exit condtion when the buffer is
      // exhausted and there are packets in the queue.
      UpdateBudgetWithElapsedTime(last_process_time_ - target_send_time);
      target_send_time = last_process_time_;
    }
  }

  Timestamp previous_process_time = last_process_time_;
  TimeDelta elapsed_time = UpdateTimeAndGetElapsed(now);
  // 检查是否需要发送keepalive包
  if (ShouldSendKeepalive(now)) {
    // We can not send padding unless a normal packet has first been sent. If
    // we do, timestamps get messed up.
    if (packet_counter_ == 0) {
      last_send_time_ = now;
    } else {
      DataSize keepalive_data_sent = DataSize::Zero();
      std::vector<std::unique_ptr<RtpPacketToSend>> keepalive_packets =
          packet_sender_->GeneratePadding(DataSize::Bytes(1));
      for (auto& packet : keepalive_packets) {
        keepalive_data_sent +=
            DataSize::Bytes(packet->payload_size() + packet->padding_size());
        packet_sender_->SendRtpPacket(std::move(packet), PacedPacketInfo());
      }
      OnPaddingSent(keepalive_data_sent);
    }
  }
  // 如果处于暂停状态（重新进行媒体协商的时候，需要置为暂停状态），不能发送音视频数据包
  if (paused_) {
    return;
  }
  // 如果逝去的时间大于0（说明已经过了一段时间了），为media budget设置目标码率
  // webrtc是通过media budget设置目标码率的，ProcessPackets 正是根据media budget控制码流大小的
  // 如果发现media budget是500kbps，那么每次处理就是按照500kbps进行发送
  if (elapsed_time > TimeDelta::Zero()) {
    // 开始更新目标码率
    DataRate target_rate = pacing_bitrate_;
    DataSize queue_size_data = packet_queue_.Size();
    if (queue_size_data > DataSize::Zero()) {
      // Assuming equal size packets and input/output rate, the average packet
      // has avg_time_left_ms left to get queue_size_bytes out of the queue, if
      // time constraint shall be met. Determine bitrate needed for that.
      packet_queue_.UpdateQueueTime(now);
      if (drain_large_queues_) {
        TimeDelta avg_time_left =
            std::max(TimeDelta::Millis(1),
                     queue_time_limit - packet_queue_.AverageQueueTime());
        DataRate min_rate_needed = queue_size_data / avg_time_left;
        if (min_rate_needed > target_rate) {
          target_rate = min_rate_needed;
          RTC_LOG(LS_VERBOSE) << "bwe:large_pacing_queue pacing_rate_kbps="
                              << target_rate.kbps();
        }
      }
    }

    if (mode_ == ProcessMode::kPeriodic) {
      // In periodic processing mode, the IntevalBudget allows positive budget
      // up to (process interval duration) * (target rate), so we only need to
      // update it once before the packet sending loop.
      // 将前面设置给pacer的目标码率设置给media_budget
      media_budget_.set_target_rate_kbps(target_rate.kbps());
      UpdateBudgetWithElapsedTime(elapsed_time);
    } else {
      media_rate_ = target_rate;
    }
  }

  bool first_packet_in_probe = false;
  bool is_probing = prober_.IsProbing();
  PacedPacketInfo pacing_info;
  absl::optional<DataSize> recommended_probe_size;
  // 是否需要探测带宽码率，这块主要根据拥塞控制来
  if (is_probing) {
    pacing_info = prober_.CurrentCluster();
    first_packet_in_probe = pacing_info.probe_cluster_bytes_sent == 0;
    recommended_probe_size = DataSize::Bytes(prober_.RecommendedMinProbeSize());
  }

  DataSize data_sent = DataSize::Zero();

  // The paused state is checked in the loop since it leaves the critical
  // section allowing the paused state to be changed from other code.
  // 核心逻辑（是否处于暂停状态）
  while (!paused_) {
    if (small_first_probe_packet_ && first_packet_in_probe) {
      // If first packet in probe, insert a small padding packet so we have a
      // more reliable start window for the rate estimation.
      auto padding = packet_sender_->GeneratePadding(DataSize::Bytes(1));
      // If no RTP modules sending media are registered, we may not get a
      // padding packet back.
      if (!padding.empty()) {
        // Insert with high priority so larger media packets don't preempt it.
        EnqueuePacketInternal(std::move(padding[0]), kFirstPriority);
        // We should never get more than one padding packets with a requested
        // size of 1 byte.
        RTC_DCHECK_EQ(padding.size(), 1u);
      }
      first_packet_in_probe = false;
    }
    // 动态模式不管
    if (mode_ == ProcessMode::kDynamic &&
        previous_process_time < target_send_time) {
      // Reduce buffer levels with amount corresponding to time between last
      // process and target send time for the next packet.
      // If the process call is late, that may be the time between the optimal
      // send times for two packets we should already have sent.
      UpdateBudgetWithElapsedTime(target_send_time - previous_process_time);
      previous_process_time = target_send_time;
    }

    // Fetch the next packet, so long as queue is not empty or budget is not exhausted.
    // 如果队列不为空，或者预算未耗尽，就获取下一个数据包：
    std::unique_ptr<RtpPacketToSend> rtp_packet =
    GetPendingPacket(pacing_info, target_send_time, now);
    
    // 如果我们获取到数据，就发送出去，如果没有获取到packet（rtp_packet为null）又分为两种情况：
    // 1.packet队列的确没有音视频数据,这个时候,我们应该发送padding(让底层网络为我们将带宽留着,
    //   否则,底层网络会将带宽进行回收,等到真正发送数据的时候,没有带宽,会造成延迟)
    // 2.packet队列有数据,但是已经发送的数据达到了设定的目标码率,也不能发送
    if (rtp_packet == nullptr) {
      // No packet available to send, check if we should send padding.
      DataSize padding_to_add = PaddingToAdd(recommended_probe_size, data_sent);
      // 如果需要发送padding包,将padding包插入队列,并重新执行while
      if (padding_to_add > DataSize::Zero()) {
        std::vector<std::unique_ptr<RtpPacketToSend>> padding_packets =
            packet_sender_->GeneratePadding(padding_to_add);
        if (padding_packets.empty()) {
          // No padding packets were generated, quite send loop.
          break;
        }
        for (auto& packet : padding_packets) {
          EnqueuePacket(std::move(packet));
        }
        // Continue loop to send the padding that was just added.
        continue;
      }

      // Can't fetch new packet and no padding to send, exit send loop.
      // 不需要发送padding, 说明这次不能再发送数据了, 退出while循环
      break;
    }

    RTC_DCHECK(rtp_packet);
    RTC_DCHECK(rtp_packet->packet_type().has_value());
    // 否则，说明我们获取了一个packet，那么计算packet的payload 大小是多少字节
    const RtpPacketMediaType packet_type = *rtp_packet->packet_type();
    DataSize packet_size = DataSize::Bytes(rtp_packet->payload_size() +
                                           rtp_packet->padding_size());

    if (include_overhead_) {
      packet_size += DataSize::Bytes(rtp_packet->headers_size()) +
                     transport_overhead_per_packet_;
    }
    // 将Packet发送出去
    packet_sender_->SendRtpPacket(std::move(rtp_packet), pacing_info);

    data_sent += packet_size;

    // Send done, update send/process time to the target send time.
    // 记录发送的packet大小和时间（下次的发送时间就是根据我们这次更新的时间计算出来的）
    OnPacketSent(packet_type, packet_size, target_send_time);
    if (recommended_probe_size && data_sent > *recommended_probe_size)
      break;

    if (mode_ == ProcessMode::kDynamic) {
      // Update target send time in case that are more packets that we are late
      // in processing.
      Timestamp next_send_time = NextSendTime();
      if (next_send_time.IsMinusInfinity()) {
        target_send_time = now;
      } else {
        target_send_time = std::min(now, next_send_time);
      }
    }
  }

  if (is_probing) {
    // 发送探测包
    probing_send_failure_ = data_sent == DataSize::Zero();
    if (!probing_send_failure_) {
      prober_.ProbeSent(CurrentTime(), data_sent.bytes());
    }
  }
}

DataSize PacingController::PaddingToAdd(
    absl::optional<DataSize> recommended_probe_size,
    DataSize data_sent) const {
  if (!packet_queue_.Empty()) {
    // Actual payload available, no need to add padding.
    return DataSize::Zero();
  }

  if (Congested()) {
    // Don't add padding if congested, even if requested for probing.
    return DataSize::Zero();
  }

  if (packet_counter_ == 0) {
    // We can not send padding unless a normal packet has first been sent. If we
    // do, timestamps get messed up.
    return DataSize::Zero();
  }

  if (recommended_probe_size) {
    if (*recommended_probe_size > data_sent) {
      return *recommended_probe_size - data_sent;
    }
    return DataSize::Zero();
  }

  if (mode_ == ProcessMode::kPeriodic) {
    return DataSize::Bytes(padding_budget_.bytes_remaining());
  } else if (padding_rate_ > DataRate::Zero() &&
             padding_debt_ == DataSize::Zero()) {
    return kDefaultPaddingTarget;
  }
  return DataSize::Zero();
}

std::unique_ptr<RtpPacketToSend> PacingController::GetPendingPacket(
    const PacedPacketInfo& pacing_info,
    Timestamp target_send_time,
    Timestamp now) {
  if (packet_queue_.Empty()) {
    return nullptr;
  }

  // First, check if there is any reason _not_ to send the next queued packet.

  // Unpaced audio packets and probes are exempted from send checks.
  // 不需要平滑处理的特殊情况：
  // pace_audio_ 为false，说明音频包不需要平滑处理，并且，
  // packet_queue_.LeadingAudioPacketEnqueueTime().has_value() 说明队列中下一个数据包是音频包
  bool unpaced_audio_packet = !pace_audio_ && packet_queue_.NextPacketIsAudio();
  bool is_probe = pacing_info.probe_cluster_id != PacedPacketInfo::kNotAProbe;
  if (!unpaced_audio_packet && !is_probe) {
    // 网络拥塞状况下不能发送
    if (Congested()) {
      // Don't send anything if congested.
      return nullptr;
    }
    // 没有拥塞，会检测模式是否为周期模式
    if (mode_ == ProcessMode::kPeriodic) {
      // media_budget_ 中可供我们使用的字节数不大于0，表示我们已经达到了目标码率，我们也不发送
      if (media_budget_.bytes_remaining() <= 0) {
        // Not enough budget.
        return nullptr;
      }
    } else {
      // Dynamic processing mode.
      // 动态模式现在还不成熟，不关心
      if (now <= target_send_time) {
        // We allow sending slightly early if we think that we would actually
        // had been able to, had we been right on time - i.e. the current debt
        // is not more than would be reduced to zero at the target sent time.
        TimeDelta flush_time = media_debt_ / media_rate_;
        if (now + flush_time > target_send_time) {
          return nullptr;
        }
      }
    }
  }
  // 如果不是上面的情况，那么，我们需要从队列中pop出一个packet，让ProcessPackets函数将其发送出去
  return packet_queue_.Pop();
}

void PacingController::OnPacketSent(RtpPacketMediaType packet_type,
                                    DataSize packet_size,
                                    Timestamp send_time) {
  if (!first_sent_packet_time_) {
    first_sent_packet_time_ = send_time;
  }
  bool audio_packet = packet_type == RtpPacketMediaType::kAudio;
  if (!audio_packet || account_for_audio_) {
    // Update media bytes sent.
    UpdateBudgetWithSentData(packet_size);
  }
  last_send_time_ = send_time;
  last_process_time_ = send_time;
}

void PacingController::OnPaddingSent(DataSize data_sent) {
  if (data_sent > DataSize::Zero()) {
    UpdateBudgetWithSentData(data_sent);
  }
  last_send_time_ = CurrentTime();
  last_process_time_ = CurrentTime();
}

void PacingController::UpdateBudgetWithElapsedTime(TimeDelta delta) {
  if (mode_ == ProcessMode::kPeriodic) {
    delta = std::min(kMaxProcessingInterval, delta);
    media_budget_.IncreaseBudget(delta.ms());
    padding_budget_.IncreaseBudget(delta.ms());
  } else {
    media_debt_ -= std::min(media_debt_, media_rate_ * delta);
    padding_debt_ -= std::min(padding_debt_, padding_rate_ * delta);
  }
}

void PacingController::UpdateBudgetWithSentData(DataSize size) {
  outstanding_data_ += size;
  if (mode_ == ProcessMode::kPeriodic) {
    media_budget_.UseBudget(size.bytes());
    padding_budget_.UseBudget(size.bytes());
  } else {
    media_debt_ += size;
    media_debt_ = std::min(media_debt_, media_rate_ * kMaxDebtInTime);
    padding_debt_ += size;
    padding_debt_ = std::min(padding_debt_, padding_rate_ * kMaxDebtInTime);
  }
}

void PacingController::SetQueueTimeLimit(TimeDelta limit) {
  queue_time_limit = limit;
}

}  // namespace webrtc
