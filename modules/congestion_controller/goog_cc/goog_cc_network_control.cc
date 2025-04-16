/*
 *  Copyright (c) 2018 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "modules/congestion_controller/goog_cc/goog_cc_network_control.h"

#include <inttypes.h>
#include <stdio.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

#include "api/units/time_delta.h"
#include "logging/rtc_event_log/events/rtc_event_remote_estimate.h"
#include "modules/congestion_controller/goog_cc/alr_detector.h"
#include "modules/congestion_controller/goog_cc/probe_controller.h"
#include "modules/remote_bitrate_estimator/include/bwe_defines.h"
#include "modules/remote_bitrate_estimator/test/bwe_test_logging.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"

namespace webrtc {

namespace {
// From RTCPSender video report interval.
constexpr TimeDelta kLossUpdateInterval = TimeDelta::Millis(1000);

// Pacing-rate relative to our target send rate.
// Multiplicative factor that is applied to the target bitrate to calculate
// the number of bytes that can be transmitted per interval.
// Increasing this factor will result in lower delays in cases of bitrate
// overshoots from the encoder.
const float kDefaultPaceMultiplier = 2.5f;

int64_t GetBpsOrDefault(const absl::optional<DataRate>& rate,
                        int64_t fallback_bps) {
  if (rate && rate->IsFinite()) {
    return rate->bps();
  } else {
    return fallback_bps;
  }
}

bool IsEnabled(const WebRtcKeyValueConfig* config, absl::string_view key) {
  return config->Lookup(key).find("Enabled") == 0;
}

bool IsNotDisabled(const WebRtcKeyValueConfig* config, absl::string_view key) {
  return config->Lookup(key).find("Disabled") != 0;
}
}  // namespace

GoogCcNetworkController::GoogCcNetworkController(NetworkControllerConfig config,
                                                 GoogCcConfig goog_cc_config)
    : key_value_config_(config.key_value_config ? config.key_value_config
                                                : &trial_based_config_),
      event_log_(config.event_log),
      packet_feedback_only_(goog_cc_config.feedback_only),
      safe_reset_on_route_change_("Enabled"),
      safe_reset_acknowledged_rate_("ack"),
      use_min_allocatable_as_lower_bound_(
          IsNotDisabled(key_value_config_, "WebRTC-Bwe-MinAllocAsLowerBound")),
      ignore_probes_lower_than_network_estimate_(IsNotDisabled(
          key_value_config_,
          "WebRTC-Bwe-IgnoreProbesLowerThanNetworkStateEstimate")),
      rate_control_settings_(
          RateControlSettings::ParseFromKeyValueConfig(key_value_config_)),
      loss_based_stable_rate_(
          IsEnabled(key_value_config_, "WebRTC-Bwe-LossBasedStableRate")),
      probe_controller_(
          new ProbeController(key_value_config_, config.event_log)),
      congestion_window_pushback_controller_(
          rate_control_settings_.UseCongestionWindowPushback()
              ? std::make_unique<CongestionWindowPushbackController>(
                    key_value_config_)
              : nullptr),
      bandwidth_estimation_(
          std::make_unique<SendSideBandwidthEstimation>(event_log_)),
      alr_detector_(
          std::make_unique<AlrDetector>(key_value_config_, config.event_log)),
      probe_bitrate_estimator_(new ProbeBitrateEstimator(config.event_log)),
      network_estimator_(std::move(goog_cc_config.network_state_estimator)),
      network_state_predictor_(
          std::move(goog_cc_config.network_state_predictor)),
      delay_based_bwe_(new DelayBasedBwe(key_value_config_,
                                         event_log_,
                                         network_state_predictor_.get())),
      acknowledged_bitrate_estimator_(
          AcknowledgedBitrateEstimatorInterface::Create(key_value_config_)),
      initial_config_(config),
      last_loss_based_target_rate_(*config.constraints.starting_rate),
      last_pushback_target_rate_(last_loss_based_target_rate_),
      last_stable_target_rate_(last_loss_based_target_rate_),
      pacing_factor_(config.stream_based_config.pacing_factor.value_or(
          kDefaultPaceMultiplier)),
      min_total_allocated_bitrate_(
          config.stream_based_config.min_total_allocated_bitrate.value_or(
              DataRate::Zero())),
      max_padding_rate_(config.stream_based_config.max_padding_rate.value_or(
          DataRate::Zero())),
      max_total_allocated_bitrate_(DataRate::Zero()) {
  RTC_DCHECK(config.constraints.at_time.IsFinite());
  ParseFieldTrial(
      {&safe_reset_on_route_change_, &safe_reset_acknowledged_rate_},
      key_value_config_->Lookup("WebRTC-Bwe-SafeResetOnRouteChange"));
  if (delay_based_bwe_)
    delay_based_bwe_->SetMinBitrate(congestion_controller::GetMinBitrate());
}

GoogCcNetworkController::~GoogCcNetworkController() {}

NetworkControlUpdate GoogCcNetworkController::OnNetworkAvailability(
    NetworkAvailability msg) {
  NetworkControlUpdate update;
  update.probe_cluster_configs = probe_controller_->OnNetworkAvailability(msg);
  return update;
}

NetworkControlUpdate GoogCcNetworkController::OnNetworkRouteChange(
    NetworkRouteChange msg) {
  if (safe_reset_on_route_change_) {
    // 当前估计带宽
    absl::optional<DataRate> estimated_bitrate;
    if (safe_reset_acknowledged_rate_) {
      estimated_bitrate = acknowledged_bitrate_estimator_->bitrate();
      if (!estimated_bitrate)
        estimated_bitrate = acknowledged_bitrate_estimator_->PeekRate();
    } else {
      estimated_bitrate = bandwidth_estimation_->target_rate();
    }
    if (estimated_bitrate) {
      if (msg.constraints.starting_rate) {
        msg.constraints.starting_rate =
            std::min(*msg.constraints.starting_rate, *estimated_bitrate);
      } else {
        msg.constraints.starting_rate = estimated_bitrate;
      }
    }
  }

  acknowledged_bitrate_estimator_ =
      AcknowledgedBitrateEstimatorInterface::Create(key_value_config_);
  probe_bitrate_estimator_.reset(new ProbeBitrateEstimator(event_log_));
  if (network_estimator_)
    network_estimator_->OnRouteChange(msg);
  delay_based_bwe_.reset(new DelayBasedBwe(key_value_config_, event_log_,
                                           network_state_predictor_.get()));
  bandwidth_estimation_->OnRouteChange();
  probe_controller_->Reset(msg.at_time.ms());
  NetworkControlUpdate update;
  update.probe_cluster_configs = ResetConstraints(msg.constraints);
  MaybeTriggerOnNetworkChanged(&update, msg.at_time);
  return update;
}
/**
 * 1. 核心目标:
 *    周期性探测带宽：通过主动发送探测包动态测量网络可用带宽。
 *    状态维护：更新ALR（应用受限区域）状态、拥塞窗口和码率估计。
 *    初始配置处理：首次运行时触发初始化探测。
 * 
 * 2. 设计原则:
 *    主动性：不等拥塞发生，定期探测网络容量变化。
 *    适应性：根据当前网络状态（如ALR）调整探测策略。
 *    保守性：避免过度探测造成网络冲击。
 */
NetworkControlUpdate GoogCcNetworkController::OnProcessInterval(
    ProcessInterval msg) {
  NetworkControlUpdate update;
  // 1. 初始探测配置（首次运行）
  //    场景：媒体流启动时，未知网络状态。
  //    动作：
  //      1）发送初始探测包集群（如2组，每组5个包）。
  //      2）若配置ALR探测，后续在应用受限时自动触发。
  if (initial_config_) {
    // 在这里触发初始探测
    update.probe_cluster_configs =
        ResetConstraints(initial_config_->constraints);
    update.pacer_config = GetPacingRates(msg.at_time);

    if (initial_config_->stream_based_config.requests_alr_probing) {
      probe_controller_->EnablePeriodicAlrProbing(
          *initial_config_->stream_based_config.requests_alr_probing); // 启用ALR探测
    }
    absl::optional<DataRate> total_bitrate =
        initial_config_->stream_based_config.max_total_allocated_bitrate;
    if (total_bitrate) {
      auto probes = probe_controller_->OnMaxTotalAllocatedBitrate(
          total_bitrate->bps(), msg.at_time.ms()); // 基于初始带宽请求探测
      update.probe_cluster_configs.insert(update.probe_cluster_configs.end(),
                                          probes.begin(), probes.end());

      max_total_allocated_bitrate_ = *total_bitrate;
    }
    initial_config_.reset();
  }
  // 2. 拥塞窗口动态调整：
  // 输入：当前队列积压数据量（pacer_queue）。
  // 策略：
  //    1）队列过长→缩小拥塞窗口（防止缓冲区膨胀）。
  //    2）队列空闲→扩大窗口（提高利用率）。
  if (congestion_window_pushback_controller_ && msg.pacer_queue) {
    congestion_window_pushback_controller_->UpdatePacingQueue(
        msg.pacer_queue->bytes());
  }
  // 3. 带宽估计更新
  // 内部行为：
  //  1）结合延迟、丢包、历史数据更新带宽预测。
  //  2）若探测包返回，优先使用探测结果。
  bandwidth_estimation_->UpdateEstimate(msg.at_time);
  // 4. ALR状态同步与探测触发
  //   ALR（Application Limited Region）：
  //   定义：发送码率低于估计带宽（如摄像头帧率不足）。
  absl::optional<int64_t> start_time_ms =
      alr_detector_->GetApplicationLimitedRegionStartTime();
  probe_controller_->SetAlrStartTimeMs(start_time_ms);
  auto probes = probe_controller_->Process(msg.at_time.ms()); // 生成探测计划
  update.probe_cluster_configs.insert(update.probe_cluster_configs.end(),
                                      probes.begin(), probes.end());
  // 5. 拥塞窗口最终决策
  if (rate_control_settings_.UseCongestionWindow() &&
      last_packet_received_time_.IsFinite() && !feedback_max_rtts_.empty()) {
    UpdateCongestionWindowSize(); // 基于最新RTT和码率计算窗口
  }
  if (congestion_window_pushback_controller_ && current_data_window_) {
    congestion_window_pushback_controller_->SetDataWindow(
        *current_data_window_);
  } else {
    update.congestion_window = current_data_window_; // 输出窗口值
  }
  MaybeTriggerOnNetworkChanged(&update, msg.at_time);
  return update;
}

NetworkControlUpdate GoogCcNetworkController::OnRemoteBitrateReport(
    RemoteBitrateReport msg) {
  if (packet_feedback_only_) {
    RTC_LOG(LS_ERROR) << "Received REMB for packet feedback only GoogCC";
    return NetworkControlUpdate();
  }
  bandwidth_estimation_->UpdateReceiverEstimate(msg.receive_time,
                                                msg.bandwidth);
  BWE_TEST_LOGGING_PLOT(1, "REMB_kbps", msg.receive_time.ms(),
                        msg.bandwidth.bps() / 1000);
  return NetworkControlUpdate();
}

NetworkControlUpdate GoogCcNetworkController::OnRoundTripTimeUpdate(
    RoundTripTimeUpdate msg) {
  if (packet_feedback_only_ || msg.smoothed)
    return NetworkControlUpdate();
  RTC_DCHECK(!msg.round_trip_time.IsZero());
  if (delay_based_bwe_)
    delay_based_bwe_->OnRttUpdate(msg.round_trip_time);
  bandwidth_estimation_->UpdateRtt(msg.round_trip_time, msg.receive_time);
  return NetworkControlUpdate();
}

NetworkControlUpdate GoogCcNetworkController::OnSentPacket(
    SentPacket sent_packet) {
  alr_detector_->OnBytesSent(sent_packet.size.bytes(),
                             sent_packet.send_time.ms());
  acknowledged_bitrate_estimator_->SetAlr(
      alr_detector_->GetApplicationLimitedRegionStartTime().has_value());

  if (!first_packet_sent_) {
    first_packet_sent_ = true;
    // Initialize feedback time to send time to allow estimation of RTT until
    // first feedback is received.
    bandwidth_estimation_->UpdatePropagationRtt(sent_packet.send_time,
                                                TimeDelta::Zero());
  }
  bandwidth_estimation_->OnSentPacket(sent_packet);

  if (congestion_window_pushback_controller_) {
    congestion_window_pushback_controller_->UpdateOutstandingData(
        sent_packet.data_in_flight.bytes());
    NetworkControlUpdate update;
    MaybeTriggerOnNetworkChanged(&update, sent_packet.send_time);
    return update;
  } else {
    return NetworkControlUpdate();
  }
}

NetworkControlUpdate GoogCcNetworkController::OnReceivedPacket(
    ReceivedPacket received_packet) {
  last_packet_received_time_ = received_packet.receive_time;
  return NetworkControlUpdate();
}

NetworkControlUpdate GoogCcNetworkController::OnStreamsConfig(
    StreamsConfig msg) {
  NetworkControlUpdate update;
  if (msg.requests_alr_probing) {
    probe_controller_->EnablePeriodicAlrProbing(*msg.requests_alr_probing);
  }
  if (msg.max_total_allocated_bitrate &&
      *msg.max_total_allocated_bitrate != max_total_allocated_bitrate_) {
    if (rate_control_settings_.TriggerProbeOnMaxAllocatedBitrateChange()) {
      update.probe_cluster_configs =
          probe_controller_->OnMaxTotalAllocatedBitrate(
              msg.max_total_allocated_bitrate->bps(), msg.at_time.ms());
    } else {
      probe_controller_->SetMaxBitrate(msg.max_total_allocated_bitrate->bps());
    }
    max_total_allocated_bitrate_ = *msg.max_total_allocated_bitrate;
  }
  bool pacing_changed = false;
  if (msg.pacing_factor && *msg.pacing_factor != pacing_factor_) {
    pacing_factor_ = *msg.pacing_factor;
    pacing_changed = true;
  }
  if (msg.min_total_allocated_bitrate &&
      *msg.min_total_allocated_bitrate != min_total_allocated_bitrate_) {
    min_total_allocated_bitrate_ = *msg.min_total_allocated_bitrate;
    pacing_changed = true;

    if (use_min_allocatable_as_lower_bound_) {
      ClampConstraints();
      delay_based_bwe_->SetMinBitrate(min_data_rate_);
      bandwidth_estimation_->SetMinMaxBitrate(min_data_rate_, max_data_rate_);
    }
  }
  if (msg.max_padding_rate && *msg.max_padding_rate != max_padding_rate_) {
    max_padding_rate_ = *msg.max_padding_rate;
    pacing_changed = true;
  }

  if (pacing_changed)
    update.pacer_config = GetPacingRates(msg.at_time);
  return update;
}

NetworkControlUpdate GoogCcNetworkController::OnTargetRateConstraints(
    TargetRateConstraints constraints) {
  NetworkControlUpdate update;
  update.probe_cluster_configs = ResetConstraints(constraints);
  MaybeTriggerOnNetworkChanged(&update, constraints.at_time);
  return update;
}

void GoogCcNetworkController::ClampConstraints() {
  // TODO(holmer): We should make sure the default bitrates are set to 10 kbps,
  // and that we don't try to set the min bitrate to 0 from any applications.
  // The congestion controller should allow a min bitrate of 0.
  min_data_rate_ =
      std::max(min_target_rate_, congestion_controller::GetMinBitrate());
  if (use_min_allocatable_as_lower_bound_) {
    min_data_rate_ = std::max(min_data_rate_, min_total_allocated_bitrate_);
  }
  if (max_data_rate_ < min_data_rate_) {
    RTC_LOG(LS_WARNING) << "max bitrate smaller than min bitrate";
    max_data_rate_ = min_data_rate_;
  }
  if (starting_rate_ && starting_rate_ < min_data_rate_) {
    RTC_LOG(LS_WARNING) << "start bitrate smaller than min bitrate";
    starting_rate_ = min_data_rate_;
  }
}
/**
 * 
 */
std::vector<ProbeClusterConfig> GoogCcNetworkController::ResetConstraints(
    TargetRateConstraints new_constraints) {
  min_target_rate_ = new_constraints.min_data_rate.value_or(DataRate::Zero());
  max_data_rate_ =
      new_constraints.max_data_rate.value_or(DataRate::PlusInfinity());
  starting_rate_ = new_constraints.starting_rate;
  ClampConstraints();
  // TODO: 这儿是啥？
  bandwidth_estimation_->SetBitrates(starting_rate_, min_data_rate_,
                                     max_data_rate_, new_constraints.at_time);

  if (starting_rate_)
    delay_based_bwe_->SetStartBitrate(*starting_rate_);
  delay_based_bwe_->SetMinBitrate(min_data_rate_);
  // 这儿是带宽探测的最终实现
  return probe_controller_->SetBitrates(
      min_data_rate_.bps(), GetBpsOrDefault(starting_rate_, -1),
      max_data_rate_.bps_or(-1), new_constraints.at_time.ms());
}

NetworkControlUpdate GoogCcNetworkController::OnTransportLossReport(
    TransportLossReport msg) {
  if (packet_feedback_only_)
    return NetworkControlUpdate();
  int64_t total_packets_delta =
      msg.packets_received_delta + msg.packets_lost_delta;
  bandwidth_estimation_->UpdatePacketsLost(
      msg.packets_lost_delta, total_packets_delta, msg.receive_time);
  return NetworkControlUpdate();
}

void GoogCcNetworkController::UpdateCongestionWindowSize() {
  TimeDelta min_feedback_max_rtt = TimeDelta::Millis(
      *std::min_element(feedback_max_rtts_.begin(), feedback_max_rtts_.end()));

  const DataSize kMinCwnd = DataSize::Bytes(2 * 1500);
  TimeDelta time_window =
      min_feedback_max_rtt +
      TimeDelta::Millis(
          rate_control_settings_.GetCongestionWindowAdditionalTimeMs());

  DataSize data_window = last_loss_based_target_rate_ * time_window;
  if (current_data_window_) {
    data_window =
        std::max(kMinCwnd, (data_window + current_data_window_.value()) / 2);
  } else {
    data_window = std::max(kMinCwnd, data_window);
  }
  current_data_window_ = data_window;
}
/**
 * 过载恢复进行探测：
 * 主要负责处理传输层数据包反馈信息，进行带宽估计和过载恢复探测等操作
 */
NetworkControlUpdate GoogCcNetworkController::OnTransportPacketsFeedback(
    TransportPacketsFeedback report) {
  // 1.首先检查数据包反馈列表是否为空，如果为空，直接返回一个空的 NetworkControlUpdate，
  //   因为没有反馈信息就无法进行后续处理。
  //   这里还提到了一个 TODO 事项，需要设计更好的机制来防止网络队列过大。
  if (report.packet_feedbacks.empty()) {
    // TODO(bugs.webrtc.org/10125): Design a better mechanism to safe-guard
    // against building very large network queues.
    return NetworkControlUpdate();
  }
  // 2.拥塞窗口控制（Cwnd Pushback）
  // 如果存在拥塞窗口回退控制器 congestion_window_pushback_controller_，则更新其未完成的数据量
  // 功能：动态限制发送速率防止拥塞崩溃（基于Cubic/BBR算法）。
  // 关键参数：data_in_flight（网络中未确认的数据量）。
  if (congestion_window_pushback_controller_) {
    congestion_window_pushback_controller_->UpdateOutstandingData(
        report.data_in_flight.bytes()); // 更新传输中的数据量
  }
  TimeDelta max_feedback_rtt = TimeDelta::MinusInfinity(); // 初始化最大反馈往返时间 max_feedback_rtt
  TimeDelta min_propagation_rtt = TimeDelta::PlusInfinity(); // 最小传播往返时间 min_propagation_rtt
  Timestamp max_recv_time = Timestamp::MinusInfinity(); // 最大接收时间 max_recv_time
  // 获取包含发送信息的数据包反馈列表 feedbacks，并找出其中的最大接收时间 max_recv_time
  std::vector<PacketResult> feedbacks = report.ReceivedWithSendInfo();
  for (const auto& feedback : feedbacks)
    max_recv_time = std::max(max_recv_time, feedback.receive_time);

  // 3.RTT（往返时延）计算
  // 原理：
  // feedback_rtt：总RTT（发送→反馈到达时间）。
  // propagation_rtt：真实网络传播延迟（总RTT - 接收端排队时间）。
  // 目的：区分网络延迟和拥塞延迟。
  for (const auto& feedback : feedbacks) {
    // 遍历每个反馈信息，计算反馈往返时间 feedback_rtt、最小等待时间 min_pending_time 和传播往返时间 propagation_rtt。
    // 并更新最大反馈往返时间 max_feedback_rtt 和最小传播往返时间 min_propagation_rtt。
    TimeDelta feedback_rtt =
        report.feedback_time - feedback.sent_packet.send_time;
    TimeDelta min_pending_time = feedback.receive_time - max_recv_time;
    TimeDelta propagation_rtt = feedback_rtt - min_pending_time;
    max_feedback_rtt = std::max(max_feedback_rtt, feedback_rtt);
    min_propagation_rtt = std::min(min_propagation_rtt, propagation_rtt);
  }
  // 4.带宽预测更新
  // 动作：更新基础RTT用于后续码率计算（如Google Congestion Control）。
  if (max_feedback_rtt.IsFinite()) {
    // 如果rtt是有限值，将其加入到 feedback_max_rtts_ 队列中，
    // 并确保队列大小不超过 kMaxFeedbackRttWindow（32），
    // 超出则弹出队首元素。然后使用最小传播往返时间更新带宽估计器的传播往返时间。
    feedback_max_rtts_.push_back(max_feedback_rtt.ms());
    const size_t kMaxFeedbackRttWindow = 32;
    if (feedback_max_rtts_.size() > kMaxFeedbackRttWindow)
      feedback_max_rtts_.pop_front();
    // TODO(srte): Use time since last unacknowledged packet.
    bandwidth_estimation_->UpdatePropagationRtt(report.feedback_time,
                                                min_propagation_rtt);
  }
  // 5.基于数据包反馈的带宽估计，相关计算
  // 如果仅基于数据包反馈（packet_feedback_only_ 为真）
  if (packet_feedback_only_) {
    if (!feedback_max_rtts_.empty()) {
      int64_t sum_rtt_ms = std::accumulate(feedback_max_rtts_.begin(),
                                           feedback_max_rtts_.end(), 0);
      // 计算平均反馈往返时间 mean_rtt_ms，并使用它更新基于延迟的带宽估计器 delay_based_bwe_ 的往返时间。
      int64_t mean_rtt_ms = sum_rtt_ms / feedback_max_rtts_.size();
      if (delay_based_bwe_)
        delay_based_bwe_->OnRttUpdate(TimeDelta::Millis(mean_rtt_ms));
    }
    // 计算最小反馈往返时间 feedback_min_rtt，并使用它更新带宽估计器的往返时间。
    TimeDelta feedback_min_rtt = TimeDelta::PlusInfinity();
    for (const auto& packet_feedback : feedbacks) {
      TimeDelta pending_time = packet_feedback.receive_time - max_recv_time;
      TimeDelta rtt = report.feedback_time -
                      packet_feedback.sent_packet.send_time - pending_time;
      // Value used for predicting NACK round trip time in FEC controller.
      feedback_min_rtt = std::min(rtt, feedback_min_rtt);
    }
    if (feedback_min_rtt.IsFinite()) {
      bandwidth_estimation_->UpdateRtt(feedback_min_rtt, report.feedback_time);
    }

    expected_packets_since_last_loss_update_ +=
        report.PacketsWithFeedback().size();
    // 6. 丢包率统计
    // 丢包率超阈值触发降速（基于Loss-Based Controller）。
    // 定期重置统计窗口（kLossUpdateInterval）
    for (const auto& packet_feedback : report.PacketsWithFeedback()) {
      if (packet_feedback.receive_time.IsInfinite())
        lost_packets_since_last_loss_update_ += 1; // 记录丢包
    }
    // 统计预期接收的数据包数量和实际丢失的数据包数量。
    // 当反馈时间超过下一次丢失更新时间时，更新带宽估计器的丢包信息，并重置计数
    if (report.feedback_time > next_loss_update_) {
      next_loss_update_ = report.feedback_time + kLossUpdateInterval;
      bandwidth_estimation_->UpdatePacketsLost(
          lost_packets_since_last_loss_update_,
          expected_packets_since_last_loss_update_, report.feedback_time);
      expected_packets_since_last_loss_update_ = 0;
      lost_packets_since_last_loss_update_ = 0;
    }
  }
  // 7. 应用层速率限制（ALR）相关处理
  // ALR定义：发送速率低于可用带宽（如应用无数据可发）。
  // 行为：退出ALR时主动发送探测包测试剩余带宽。
  absl::optional<int64_t> alr_start_time =
      alr_detector_->GetApplicationLimitedRegionStartTime();
  // 获取应用层速率限制区域的开始时间 alr_start_time。
  // 如果之前处于 ALR 状态但现在不再处于该状态，更新确认比特率估计器和探测控制器的 ALR 结束时间。
  if (previously_in_alr_ && !alr_start_time.has_value()) {
    int64_t now_ms = report.feedback_time.ms();
    acknowledged_bitrate_estimator_->SetAlrEndedTime(report.feedback_time);
    probe_controller_->SetAlrEndedTimeMs(now_ms); // 退出ALR时触发探测
  }
  previously_in_alr_ = alr_start_time.has_value();
  // 更新确认比特率估计器，并获取确认比特率，然后使用它更新带宽估计器的确认速率。
  acknowledged_bitrate_estimator_->IncomingPacketFeedbackVector(
      report.SortedByReceiveTime());
  auto acknowledged_bitrate = acknowledged_bitrate_estimator_->bitrate();
  // 同时，将数据包反馈信息提供给带宽估计器。
  bandwidth_estimation_->SetAcknowledgedRate(acknowledged_bitrate,
                                             report.feedback_time);
  bandwidth_estimation_->IncomingPacketFeedbackVector(report);
  // 遍历排序后的反馈信息，如果数据包是探测包，则使用探测比特率估计器处理并估计比特率。
  for (const auto& feedback : report.SortedByReceiveTime()) {
    if (feedback.sent_packet.pacing_info.probe_cluster_id !=
        PacedPacketInfo::kNotAProbe) {
      probe_bitrate_estimator_->HandleProbeAndEstimateBitrate(feedback);
    }
  }
  // 8.网络估计器相关处理
  // 如果存在网络估计器 network_estimator_，将数据包反馈信息提供给它，
  // 并更新当前估计值 estimate_。如果估计值发生变化，记录到事件日志中。
  if (network_estimator_) {
    network_estimator_->OnTransportPacketsFeedback(report);
    auto prev_estimate = estimate_;
    estimate_ = network_estimator_->GetCurrentEstimate();
    // TODO(srte): Make OnTransportPacketsFeedback signal wether the state
    // changed to avoid the need for this check.
    if (estimate_ && (!prev_estimate || estimate_->last_feed_time !=
                                            prev_estimate->last_feed_time)) {
      event_log_->Log(std::make_unique<RtcEventRemoteEstimate>(
          estimate_->link_capacity_lower, estimate_->link_capacity_upper));
    }
  }
  // 获取探测比特率估计器的最后估计比特率 probe_bitrate，如果设置了忽略低于网络估计值的探测，
  // 并且探测比特率低于基于延迟的带宽估计器的最后估计值以及网络估计的下限，则忽略该探测比特率。
  absl::optional<DataRate> probe_bitrate =
      probe_bitrate_estimator_->FetchAndResetLastEstimatedBitrate();
  if (ignore_probes_lower_than_network_estimate_ && probe_bitrate &&
      estimate_ && *probe_bitrate < delay_based_bwe_->last_estimate() &&
      *probe_bitrate < estimate_->link_capacity_lower) {
    probe_bitrate.reset();
  }
  // 9.延时与带宽联合控制
  // 初始化一个 NetworkControlUpdate 对象 update，以及两个布尔变量 recovered_from_overuse 和 backoff_in_alr
  // 用于记录过载恢复和 ALR 中的回退状态。
  NetworkControlUpdate update;
  bool recovered_from_overuse = false;
  bool backoff_in_alr = false;
  // 7. 延时与带宽联合控制
  // 使用基于延迟的带宽估计器 delay_based_bwe_ 处理数据包反馈信息，得到结果 result。
  DelayBasedBwe::Result result;
  result = delay_based_bwe_->IncomingPacketFeedbackVector(
      report, acknowledged_bitrate, probe_bitrate, estimate_,
      alr_start_time.has_value());
  // 如果结果表明需要更新：
  // 如果是探测结果，设置带宽估计器的发送比特率，并更新基于延迟的估计值。
  // 调用 MaybeTriggerOnNetworkChanged 函数，可能会触发网络变化相关操作。
  if (result.updated) {
    if (result.probe) {
      bandwidth_estimation_->SetSendBitrate(result.target_bitrate,
                                            report.feedback_time);
    }
    // Since SetSendBitrate now resets the delay-based estimate, we have to
    // call UpdateDelayBasedEstimate after SetSendBitrate.
    bandwidth_estimation_->UpdateDelayBasedEstimate(report.feedback_time,
                                                    result.target_bitrate);
    // Update the estimate in the ProbeController, in case we want to probe.
    MaybeTriggerOnNetworkChanged(&update, report.feedback_time);
  }
  // 更新 recovered_from_overuse 和 backoff_in_alr 的值。
  recovered_from_overuse = result.recovered_from_overuse;
  backoff_in_alr = result.backoff_in_alr;
  // 8. 主动探测触发
  // 如果从过载中恢复或者在 ALR 中进行了回退，请求探测控制器进行新的探测，并将探测配置加入到 update 中。
  if (recovered_from_overuse) {
    probe_controller_->SetAlrStartTimeMs(alr_start_time);
    auto probes = probe_controller_->RequestProbe(report.feedback_time.ms()); // 请求带宽探测
    update.probe_cluster_configs.insert(update.probe_cluster_configs.end(),
                                        probes.begin(), probes.end());
  } else if (backoff_in_alr) {
    // If we just backed off during ALR, request a new probe.
    auto probes = probe_controller_->RequestProbe(report.feedback_time.ms());
    update.probe_cluster_configs.insert(update.probe_cluster_configs.end(),
                                        probes.begin(), probes.end());
  }

  // No valid RTT could be because send-side BWE isn't used, in which case
  // we don't try to limit the outstanding packets.
  // 9. 拥塞窗口动态调整
  // 如果启用了拥塞窗口并且RTT是有限值，更新拥塞窗口大小。
  if (rate_control_settings_.UseCongestionWindow() &&
      max_feedback_rtt.IsFinite()) {
    UpdateCongestionWindowSize(); // 根据RTT和码率调整窗口
  }
  // 如果存在拥塞窗口回退控制器和当前数据窗口，设置拥塞窗口回退控制器的数据窗口；
  // 否则，将当前数据窗口设置到 update 的拥塞窗口字段中。
  if (congestion_window_pushback_controller_ && current_data_window_) {
    congestion_window_pushback_controller_->SetDataWindow(
        *current_data_window_);
  } else {
    update.congestion_window = current_data_window_;
  }
  // 最后返回 update，其中包含了带宽估计、探测配置和拥塞窗口等相关信息。
  return update;
}

NetworkControlUpdate GoogCcNetworkController::OnNetworkStateEstimate(
    NetworkStateEstimate msg) {
  estimate_ = msg;
  return NetworkControlUpdate();
}

NetworkControlUpdate GoogCcNetworkController::GetNetworkState(
    Timestamp at_time) const {
  NetworkControlUpdate update;
  update.target_rate = TargetTransferRate();
  update.target_rate->network_estimate.at_time = at_time;
  update.target_rate->network_estimate.loss_rate_ratio =
      last_estimated_fraction_loss_.value_or(0) / 255.0;
  update.target_rate->network_estimate.round_trip_time =
      last_estimated_round_trip_time_;
  update.target_rate->network_estimate.bwe_period =
      delay_based_bwe_->GetExpectedBwePeriod();

  update.target_rate->at_time = at_time;
  update.target_rate->target_rate = last_pushback_target_rate_;
  update.target_rate->stable_target_rate =
      bandwidth_estimation_->GetEstimatedLinkCapacity();
  update.pacer_config = GetPacingRates(at_time);
  update.congestion_window = current_data_window_;
  return update;
}
/**
 * 该函数是 Google Congestion Control (GoogCc) 算法的核心决策模块，负责动态调整发送速率:
 * 原始估计（loss_based） → 拥塞窗口修正（pushback） → 链路容量限制（stable）
 * 
 * 基于网络状态变化计算目标码率，综合以下输入决定发送策略：
 * 1.当前带宽估计值（loss_based_target_rate）
 * 2.网络丢包率（fraction_loss）
 * 3.往返时延（round_trip_time）
 * 4.拥塞窗口反馈（pushback_target_rate）
 * 5.链路容量估计（stable_target_rate）
 * 
 * 最终输出封装在 NetworkControlUpdate 中，包含：
 * 1.目标码率（target_rate_msg）
 * 2.探测配置（probe_cluster_configs）
 * 3.pacing速率（pacer_config）
 */
void GoogCcNetworkController::MaybeTriggerOnNetworkChanged(
    NetworkControlUpdate* update,
    Timestamp at_time) {
  // 1.收集实时网络指标（数据来源是：bandwidth_estimation_ 模块（整合延迟/丢包指标）、接收端的 RTCP 反馈（如 Transport-CC））
  // 丢包率（0=无丢包，255=100%丢包）
  uint8_t fraction_loss = bandwidth_estimation_->fraction_loss();
  TimeDelta round_trip_time = bandwidth_estimation_->round_trip_time();
  // 基于丢包的带宽估计（主控值）
  DataRate loss_based_target_rate = bandwidth_estimation_->target_rate();
  // 考虑拥塞窗口反馈后的保守码率
  DataRate pushback_target_rate = loss_based_target_rate;

  BWE_TEST_LOGGING_PLOT(1, "fraction_loss_%", at_time.ms(),
                        (fraction_loss * 100) / 256);
  BWE_TEST_LOGGING_PLOT(1, "rtt_ms", at_time.ms(), round_trip_time.ms());
  BWE_TEST_LOGGING_PLOT(1, "Target_bitrate_kbps", at_time.ms(),
                        loss_based_target_rate.kbps());
  // 拥塞窗口缩减比例（仅帧丢弃模式使用）
  double cwnd_reduce_ratio = 0.0;
  // 2.拥塞窗口反馈处理
  // 1）拥塞窗口控制器作用：
  //    1.1）当网络缓存队列过长时，主动降低目标码率（基于 BBR 或 PCC 算法）
  //    1.2）输出 pushback_rate 确保不低于最小码率（GetMinBitrate）
  // 2）帧丢弃模式：
  //    2.1）计算码率缩减比例（cwnd_reduce_ratio），用于视频编码层选择性丢帧
  if (congestion_window_pushback_controller_) {
    int64_t pushback_rate =
        congestion_window_pushback_controller_->UpdateTargetBitrate(
            loss_based_target_rate.bps());
    pushback_rate = std::max<int64_t>(bandwidth_estimation_->GetMinBitrate(),
                                      pushback_rate);
    pushback_target_rate = DataRate::BitsPerSec(pushback_rate);
    if (rate_control_settings_.UseCongestionWindowDropFrameOnly()) {
      cwnd_reduce_ratio = static_cast<double>(loss_based_target_rate.bps() -
                                              pushback_target_rate.bps()) /
                          loss_based_target_rate.bps();
    }
  }
  // 3.计算稳定目标码率（综合链路容量和稳定性要求的安全码率）
  //   策略选择：
  //    loss_based_stable_rate_=true：取链路容量和丢包估计的较小值（激进模式）
  //    false：取链路容量和拥塞反馈的较小值（保守模式）
  DataRate stable_target_rate =
      bandwidth_estimation_->GetEstimatedLinkCapacity();
  if (loss_based_stable_rate_) {
    stable_target_rate = std::min(stable_target_rate, loss_based_target_rate);
  } else {
    stable_target_rate = std::min(stable_target_rate, pushback_target_rate);
  }
  // 4.状态变化检测与响应
  //   触发条件：任一网络指标变化（码率/丢包/RTT）
  //   防抖动设计：仅当变化超过阈值时才触发更新（隐式比较）
  if ((loss_based_target_rate != last_loss_based_target_rate_) ||
      (fraction_loss != last_estimated_fraction_loss_) ||
      (round_trip_time != last_estimated_round_trip_time_) ||
      (pushback_target_rate != last_pushback_target_rate_) ||
      (stable_target_rate != last_stable_target_rate_)) {
    last_loss_based_target_rate_ = loss_based_target_rate;
    last_pushback_target_rate_ = pushback_target_rate;
    last_estimated_fraction_loss_ = fraction_loss;
    last_estimated_round_trip_time_ = round_trip_time;
    last_stable_target_rate_ = stable_target_rate;

    alr_detector_->SetEstimatedBitrate(loss_based_target_rate.bps());

    TimeDelta bwe_period = delay_based_bwe_->GetExpectedBwePeriod();
    // 更新状态并构造控制消息
    TargetTransferRate target_rate_msg;
    target_rate_msg.at_time = at_time;
    // 控制算法行为（如是否启用拥塞窗口/帧丢弃模式）
    if (rate_control_settings_.UseCongestionWindowDropFrameOnly()) {
      target_rate_msg.target_rate = loss_based_target_rate;
      target_rate_msg.cwnd_reduce_ratio = cwnd_reduce_ratio;
    } else {
      target_rate_msg.target_rate = pushback_target_rate; // 或 loss_based_target_rate
    }
    // 5.构造控制消息
    target_rate_msg.stable_target_rate = stable_target_rate; 
    target_rate_msg.network_estimate.at_time = at_time;
    target_rate_msg.network_estimate.round_trip_time = round_trip_time;
    target_rate_msg.network_estimate.loss_rate_ratio = fraction_loss / 255.0f; // 标准化丢包率，供视频编码器参考（归一化到[0,1]）
    target_rate_msg.network_estimate.bwe_period = bwe_period; // 指示下次带宽更新的预期时间（用于发送端调度）

    update->target_rate = target_rate_msg;
    // 6.触发带宽探测
    // 当 loss_based_target_rate 显著增长时，发起探测验证实际可用带宽
    // 探测结果将反馈回 bandwidth_estimation_ 模块
    auto probes = probe_controller_->SetEstimatedBitrate(
        loss_based_target_rate.bps(), at_time.ms());
    update->probe_cluster_configs.insert(update->probe_cluster_configs.end(),
                                         probes.begin(), probes.end());
    update->pacer_config = GetPacingRates(at_time);

    RTC_LOG(LS_VERBOSE) << "bwe " << at_time.ms() << " pushback_target_bps="
                        << last_pushback_target_rate_.bps()
                        << " estimate_bps=" << loss_based_target_rate.bps();
  }
}

PacerConfig GoogCcNetworkController::GetPacingRates(Timestamp at_time) const {
  // Pacing rate is based on target rate before congestion window pushback,
  // because we don't want to build queues in the pacer when pushback occurs.
  DataRate pacing_rate =
      std::max(min_total_allocated_bitrate_, last_loss_based_target_rate_) *
      pacing_factor_;
  DataRate padding_rate =
      std::min(max_padding_rate_, last_pushback_target_rate_);
  PacerConfig msg;
  msg.at_time = at_time;
  msg.time_window = TimeDelta::Seconds(1);
  msg.data_window = pacing_rate * msg.time_window;
  msg.pad_window = padding_rate * msg.time_window;
  return msg;
}

}  // namespace webrtc
