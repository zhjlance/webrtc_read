/*
 *  Copyright (c) 2016 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "modules/video_coding/nack_module.h"

#include <algorithm>
#include <limits>

#include "api/units/timestamp.h"
#include "modules/utility/include/process_thread.h"
#include "rtc_base/checks.h"
#include "rtc_base/experiments/field_trial_parser.h"
#include "rtc_base/logging.h"
#include "system_wrappers/include/field_trial.h"

namespace webrtc {

namespace {
const int kMaxPacketAge = 10000;
const int kMaxNackPackets = 1000;
const int kDefaultRttMs = 100;
const int kMaxNackRetries = 10;
const int kProcessFrequency = 50;
const int kProcessIntervalMs = 1000 / kProcessFrequency;
const int kMaxReorderedPackets = 128;
const int kNumReorderingBuckets = 10;
const int kDefaultSendNackDelayMs = 0;

int64_t GetSendNackDelay() {
  int64_t delay_ms = strtol(
      webrtc::field_trial::FindFullName("WebRTC-SendNackDelayMs").c_str(),
      nullptr, 10);
  if (delay_ms > 0 && delay_ms <= 20) {
    RTC_LOG(LS_INFO) << "SendNackDelay is set to " << delay_ms;
    return delay_ms;
  }
  return kDefaultSendNackDelayMs;
}
}  // namespace

NackModule::NackInfo::NackInfo()
    : seq_num(0), send_at_seq_num(0), sent_at_time(-1), retries(0) {}

NackModule::NackInfo::NackInfo(uint16_t seq_num,
                               uint16_t send_at_seq_num,
                               int64_t created_at_time)
    : seq_num(seq_num),
      send_at_seq_num(send_at_seq_num),
      created_at_time(created_at_time),
      sent_at_time(-1),
      retries(0) {}

NackModule::BackoffSettings::BackoffSettings(TimeDelta min_retry,
                                             TimeDelta max_rtt,
                                             double base)
    : min_retry_interval(min_retry), max_rtt(max_rtt), base(base) {}

absl::optional<NackModule::BackoffSettings>
NackModule::BackoffSettings::ParseFromFieldTrials() {
  // Matches magic number in RTPSender::OnReceivedNack().
  const TimeDelta kDefaultMinRetryInterval = TimeDelta::Millis(5);
  // Upper bound on link-delay considered for exponential backoff.
  // Selected so that cumulative delay with 1.25 base and 10 retries ends up
  // below 3s, since above that there will be a FIR generated instead.
  const TimeDelta kDefaultMaxRtt = TimeDelta::Millis(160);
  // Default base for exponential backoff, adds 25% RTT delay for each retry.
  const double kDefaultBase = 1.25;

  FieldTrialParameter<bool> enabled("enabled", false);
  FieldTrialParameter<TimeDelta> min_retry("min_retry",
                                           kDefaultMinRetryInterval);
  FieldTrialParameter<TimeDelta> max_rtt("max_rtt", kDefaultMaxRtt);
  FieldTrialParameter<double> base("base", kDefaultBase);
  ParseFieldTrial({&enabled, &min_retry, &max_rtt, &base},
                  field_trial::FindFullName("WebRTC-ExponentialNackBackoff"));

  if (enabled) {
    return NackModule::BackoffSettings(min_retry.Get(), max_rtt.Get(),
                                       base.Get());
  }
  return absl::nullopt;
}

NackModule::NackModule(Clock* clock,
                       NackSender* nack_sender,
                       KeyFrameRequestSender* keyframe_request_sender)
    : clock_(clock),
      nack_sender_(nack_sender),
      keyframe_request_sender_(keyframe_request_sender),
      reordering_histogram_(kNumReorderingBuckets, kMaxReorderedPackets),
      initialized_(false),
      rtt_ms_(kDefaultRttMs),
      newest_seq_num_(0),
      next_process_time_ms_(-1),
      send_nack_delay_ms_(GetSendNackDelay()),
      backoff_settings_(BackoffSettings::ParseFromFieldTrials()) {
  RTC_DCHECK(clock_);
  RTC_DCHECK(nack_sender_);
  RTC_DCHECK(keyframe_request_sender_);
}

int NackModule::OnReceivedPacket(uint16_t seq_num, bool is_keyframe) {
  return OnReceivedPacket(seq_num, is_keyframe, false);
}
/**
 * 里面会判断是否有丢包：
 * 
 * 1.只在接收到的第一个包时执行。初始化 newest_seq_num_ 为当前包的序列号，
 *   同时保存第一个关键帧（如果该包是关键帧）
 * 2.第一次初始化函数，只需要简化处理，本次包被记录后直接退出，不做其它操作。
 */
int NackModule::OnReceivedPacket(uint16_t seq_num,
                                 bool is_keyframe,
                                 bool is_recovered) {
  rtc::CritScope lock(&crit_);
  // TODO(philipel): When the packet includes information whether it is
  //                 retransmitted or not, use that value instead. For
  //                 now set it to true, which will cause the reordering
  //                 statistics to never be updated.
  bool is_retransmitted = true;

  if (!initialized_) {
    newest_seq_num_ = seq_num;
    if (is_keyframe)
      keyframe_list_.insert(seq_num);
    initialized_ = true;
    return 0;
  }

  // Since the |newest_seq_num_| is a packet we have actually received we know
  // that packet has never been Nacked.
  if (seq_num == newest_seq_num_)
    return 0;
  /**
   * 1.检查收到的包是否为重复包（seq_num == newest_seq_num_）或者乱序包（AheadOf 函数判断包是否比最新序列号旧）
   * 2.乱序包的行动：如果乱序包在 nack_list_ 中，说明之前被判断为丢失，已请求重传，此时从 nack_list_ 中删除，
   *   因为该丢包实际上已经被恢复。（防止无意义的重传）
   */
  if (AheadOf(newest_seq_num_, seq_num)) {
    // An out of order packet has been received.
    auto nack_list_it = nack_list_.find(seq_num);
    int nacks_sent_for_packet = 0;
    if (nack_list_it != nack_list_.end()) {
      nacks_sent_for_packet = nack_list_it->second.retries;
      nack_list_.erase(nack_list_it);
    }
    if (!is_retransmitted)
      UpdateReorderingStatistics(seq_num);
    return nacks_sent_for_packet;
  }

  // Keep track of new keyframes.
  // 新包到达模块
  // 1.当新包到达时，如果是关键帧，则记录关键帧的序号；
  // 2.同时清理超出 kMaxPacketAge （值是10000）范围的历史关键帧序号，避免列表的无限增长。
  if (is_keyframe)
    keyframe_list_.insert(seq_num);

  // And remove old ones so we don't accumulate keyframes.
  auto it = keyframe_list_.lower_bound(seq_num - kMaxPacketAge);
  if (it != keyframe_list_.begin())
    keyframe_list_.erase(keyframe_list_.begin(), it);

  if (is_recovered) {
    recovered_list_.insert(seq_num);

    // Remove old ones so we don't accumulate recovered packets.
    auto it = recovered_list_.lower_bound(seq_num - kMaxPacketAge);
    if (it != recovered_list_.begin())
      recovered_list_.erase(recovered_list_.begin(), it);

    // Do not send nack for packets recovered by FEC or RTX.
    return 0;
  }
  // 丢包列表管理：
  // 1.检查当前包与上次接收的包之间是否存在包丢失（通过包序号差距判断）。
  //   调用 AddPacketsToNack 将中间的丢包插入到 nack_list_ 中；
  // 2.更新 newest_seq_num_，确保下次处理时以最新接收的包为基准。
  AddPacketsToNack(newest_seq_num_ + 1, seq_num);
  newest_seq_num_ = seq_num;

  // Are there any nacks that are waiting for this seq_num.
  // NACK 批量发送：
  // 1.构造一个丢失包序号的批量列表（nack_batch），并将这些序号通过 NACK 消息发送给远端。
  // 2.GetNackBatch 函数会筛选出真正需要 NACK（仍未恢复）的丢失包。
  std::vector<uint16_t> nack_batch = GetNackBatch(kSeqNumOnly);
  if (!nack_batch.empty()) {
    // This batch of NACKs is triggered externally; the initiator can
    // batch them with other feedback messages.
    nack_sender_->SendNack(nack_batch, /*buffering_allowed=*/true);
  }

  return 0;
}

void NackModule::ClearUpTo(uint16_t seq_num) {
  rtc::CritScope lock(&crit_);
  nack_list_.erase(nack_list_.begin(), nack_list_.lower_bound(seq_num));
  keyframe_list_.erase(keyframe_list_.begin(),
                       keyframe_list_.lower_bound(seq_num));
  recovered_list_.erase(recovered_list_.begin(),
                        recovered_list_.lower_bound(seq_num));
}

void NackModule::UpdateRtt(int64_t rtt_ms) {
  rtc::CritScope lock(&crit_);
  rtt_ms_ = rtt_ms;
}

void NackModule::Clear() {
  rtc::CritScope lock(&crit_);
  nack_list_.clear();
  keyframe_list_.clear();
  recovered_list_.clear();
}

int64_t NackModule::TimeUntilNextProcess() {
  return std::max<int64_t>(next_process_time_ms_ - clock_->TimeInMilliseconds(),
                           0);
}

void NackModule::Process() {
  if (nack_sender_) {
    std::vector<uint16_t> nack_batch;
    {
      rtc::CritScope lock(&crit_);
      nack_batch = GetNackBatch(kTimeOnly);
    }

    if (!nack_batch.empty()) {
      // This batch of NACKs is triggered externally; there is no external
      // initiator who can batch them with other feedback messages.
      nack_sender_->SendNack(nack_batch, /*buffering_allowed=*/false);
    }
  }

  // Update the next_process_time_ms_ in intervals to achieve
  // the targeted frequency over time. Also add multiple intervals
  // in case of a skip in time as to not make uneccessary
  // calls to Process in order to catch up.
  int64_t now_ms = clock_->TimeInMilliseconds();
  if (next_process_time_ms_ == -1) {
    next_process_time_ms_ = now_ms + kProcessIntervalMs;
  } else {
    next_process_time_ms_ = next_process_time_ms_ + kProcessIntervalMs +
                            (now_ms - next_process_time_ms_) /
                                kProcessIntervalMs * kProcessIntervalMs;
  }
}

bool NackModule::RemovePacketsUntilKeyFrame() {
  while (!keyframe_list_.empty()) {
    auto it = nack_list_.lower_bound(*keyframe_list_.begin());

    if (it != nack_list_.begin()) {
      // We have found a keyframe that actually is newer than at least one
      // packet in the nack list.
      nack_list_.erase(nack_list_.begin(), it);
      return true;
    }

    // If this keyframe is so old it does not remove any packets from the list,
    // remove it from the list of keyframes and try the next keyframe.
    keyframe_list_.erase(keyframe_list_.begin());
  }
  return false;
}
/**
 * 这个是根据包序号判断哪些包丢了，归纳功能如下：
 * 1.记录丢包： 当检测到某些数据包丢失时，将这些丢包的序号记录进 nack_list_，等待后续判断和处理。
 * 2.限制管理： 控制 nack_list_ 的尺寸，防止超出最大容量，同时根据策略清理不必要的条目。
 * 3.识别特殊包: 对于通过其他方式（例如 FEC/RTX）找回的包无需生成 NACK 条目。
 * 
 * 拿到这个包到上一次的包newest_seq_num_中间所有丢失的包;
 * 这些包有可能是乱序（假丢包），也有可能是真丢包，就可以用 GetNackBatch 判断
 */
void NackModule::AddPacketsToNack(uint16_t seq_num_start,
                                  uint16_t seq_num_end) {
  // Remove old packets.
  // 清理老旧记录（防止 nack_list 过大）
  // 使用 lower_bound 找到 seq_num_end - kMaxPacketAge 的边界，将过于久远的丢包条目从 nack_list_ 中删除
  // (查找第一个键值不小于(seq_num_end - kMaxPacketAge)的元素)
  auto it = nack_list_.lower_bound(seq_num_end - kMaxPacketAge);
  nack_list_.erase(nack_list_.begin(), it);

  // If the nack list is too large, remove packets from the nack list until
  // the latest first packet of a keyframe. If the list is still too large,
  // clear it and request a keyframe.
  // 限制丢包列表大小
  uint16_t num_new_nacks = ForwardDiff(seq_num_start, seq_num_end);
  // 检查 nack_list_ 的当前大小和即将添加的条目是否会超过最大容量。
  if (nack_list_.size() + num_new_nacks > kMaxNackPackets) {
    // 优先通过 RemovePacketsUntilKeyFrame() 清理到最新关键帧之前的 NACK 条目
    while (RemovePacketsUntilKeyFrame() &&
           nack_list_.size() + num_new_nacks > kMaxNackPackets) {
    }
    // 如果清理后仍超上限，则完全清空 nack_list_ 并请求关键帧
    if (nack_list_.size() + num_new_nacks > kMaxNackPackets) {
      nack_list_.clear();
      RTC_LOG(LS_WARNING) << "NACK list full, clearing NACK"
                             " list and requesting keyframe.";
      keyframe_request_sender_->RequestKeyFrame();
      return;
    }
  }
  // 接下来就是将可疑丢包找到(记录到NackInfo当中)
  for (uint16_t seq_num = seq_num_start; seq_num != seq_num_end; ++seq_num) {
    // Do not send nack for packets that are already recovered by FEC or RTX
    // 根据 seq_num 逐个检查这些包是否已通过 FEC 或 RTX 恢复。如果恢复过，则跳过
    if (recovered_list_.find(seq_num) != recovered_list_.end())
      continue;
    // 创建 NackInfo 记录丢包的序号和请求重传的时间等信息。
    NackInfo nack_info(seq_num, seq_num + WaitNumberOfPackets(0.5),
                       clock_->TimeInMilliseconds());
    RTC_DCHECK(nack_list_.find(seq_num) == nack_list_.end());
    nack_list_[seq_num] = nack_info;
  }
}

std::vector<uint16_t> NackModule::GetNackBatch(NackFilterOptions options) {
  bool consider_seq_num = options != kTimeOnly;
  bool consider_timestamp = options != kSeqNumOnly;
  Timestamp now = clock_->CurrentTime();
  std::vector<uint16_t> nack_batch;
  auto it = nack_list_.begin();
  while (it != nack_list_.end()) {
    TimeDelta resend_delay = TimeDelta::Millis(rtt_ms_);
    if (backoff_settings_) {
      resend_delay =
          std::max(resend_delay, backoff_settings_->min_retry_interval);
      if (it->second.retries > 1) {
        TimeDelta exponential_backoff =
            std::min(TimeDelta::Millis(rtt_ms_), backoff_settings_->max_rtt) *
            std::pow(backoff_settings_->base, it->second.retries - 1);
        resend_delay = std::max(resend_delay, exponential_backoff);
      }
    }

    bool delay_timed_out =
        now.ms() - it->second.created_at_time >= send_nack_delay_ms_;
    bool nack_on_rtt_passed =
        now.ms() - it->second.sent_at_time >= resend_delay.ms();
    bool nack_on_seq_num_passed =
        it->second.sent_at_time == -1 &&
        AheadOrAt(newest_seq_num_, it->second.send_at_seq_num);
    if (delay_timed_out && ((consider_seq_num && nack_on_seq_num_passed) ||
                            (consider_timestamp && nack_on_rtt_passed))) {
      nack_batch.emplace_back(it->second.seq_num);
      ++it->second.retries;
      it->second.sent_at_time = now.ms();
      if (it->second.retries >= kMaxNackRetries) {
        RTC_LOG(LS_WARNING) << "Sequence number " << it->second.seq_num
                            << " removed from NACK list due to max retries.";
        it = nack_list_.erase(it);
      } else {
        ++it;
      }
      continue;
    }
    ++it;
  }
  return nack_batch;
}

void NackModule::UpdateReorderingStatistics(uint16_t seq_num) {
  RTC_DCHECK(AheadOf(newest_seq_num_, seq_num));
  uint16_t diff = ReverseDiff(newest_seq_num_, seq_num);
  reordering_histogram_.Add(diff);
}

int NackModule::WaitNumberOfPackets(float probability) const {
  if (reordering_histogram_.NumValues() == 0)
    return 0;
  return reordering_histogram_.InverseCdf(probability);
}

}  // namespace webrtc
