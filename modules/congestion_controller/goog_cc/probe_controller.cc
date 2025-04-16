/*
 *  Copyright (c) 2016 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "modules/congestion_controller/goog_cc/probe_controller.h"

#include <algorithm>
#include <initializer_list>
#include <memory>
#include <string>

#include "api/units/data_rate.h"
#include "api/units/time_delta.h"
#include "api/units/timestamp.h"
#include "logging/rtc_event_log/events/rtc_event_probe_cluster_created.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"
#include "rtc_base/numerics/safe_conversions.h"
#include "system_wrappers/include/metrics.h"

namespace webrtc {

namespace {
// The minimum number probing packets used.
constexpr int kMinProbePacketsSent = 5;

// The minimum probing duration in ms.
constexpr int kMinProbeDurationMs = 15;

// Maximum waiting time from the time of initiating probing to getting
// the measured results back.
constexpr int64_t kMaxWaitingTimeForProbingResultMs = 1000;

// Value of |min_bitrate_to_probe_further_bps_| that indicates
// further probing is disabled.
constexpr int kExponentialProbingDisabled = 0;

// Default probing bitrate limit. Applied only when the application didn't
// specify max bitrate.
constexpr int64_t kDefaultMaxProbingBitrateBps = 5000000;

// If the bitrate drops to a factor |kBitrateDropThreshold| or lower
// and we recover within |kBitrateDropTimeoutMs|, then we'll send
// a probe at a fraction |kProbeFractionAfterDrop| of the original bitrate.
constexpr double kBitrateDropThreshold = 0.66;
constexpr int kBitrateDropTimeoutMs = 5000;
constexpr double kProbeFractionAfterDrop = 0.85;

// Timeout for probing after leaving ALR. If the bitrate drops significantly,
// (as determined by the delay based estimator) and we leave ALR, then we will
// send a probe if we recover within |kLeftAlrTimeoutMs| ms.
constexpr int kAlrEndedTimeoutMs = 3000;

// The expected uncertainty of probe result (as a fraction of the target probe
// This is a limit on how often probing can be done when there is a BW
// drop detected in ALR.
constexpr int64_t kMinTimeBetweenAlrProbesMs = 5000;

// bitrate). Used to avoid probing if the probe bitrate is close to our current
// estimate.
constexpr double kProbeUncertainty = 0.05;

// Use probing to recover faster after large bitrate estimate drops.
constexpr char kBweRapidRecoveryExperiment[] =
    "WebRTC-BweRapidRecoveryExperiment";

// Never probe higher than configured by OnMaxTotalAllocatedBitrate().
constexpr char kCappedProbingFieldTrialName[] = "WebRTC-BweCappedProbing";

void MaybeLogProbeClusterCreated(RtcEventLog* event_log,
                                 const ProbeClusterConfig& probe) {
  RTC_DCHECK(event_log);
  if (!event_log) {
    return;
  }

  size_t min_bytes = static_cast<int32_t>(probe.target_data_rate.bps() *
                                          probe.target_duration.ms() / 8000);
  event_log->Log(std::make_unique<RtcEventProbeClusterCreated>(
      probe.id, probe.target_data_rate.bps(), probe.target_probe_count,
      min_bytes));
}

}  // namespace

ProbeControllerConfig::ProbeControllerConfig(
    const WebRtcKeyValueConfig* key_value_config)
    : first_exponential_probe_scale("p1", 3.0),
      second_exponential_probe_scale("p2", 6.0),
      further_exponential_probe_scale("step_size", 2),
      further_probe_threshold("further_probe_threshold", 0.7),
      alr_probing_interval("alr_interval", TimeDelta::Seconds(5)),
      alr_probe_scale("alr_scale", 2),
      first_allocation_probe_scale("alloc_p1", 1),
      second_allocation_probe_scale("alloc_p2", 2),
      allocation_allow_further_probing("alloc_probe_further", false),
      allocation_probe_max("alloc_probe_max", DataRate::PlusInfinity()) {
  ParseFieldTrial(
      {&first_exponential_probe_scale, &second_exponential_probe_scale,
       &further_exponential_probe_scale, &further_probe_threshold,
       &alr_probing_interval, &alr_probe_scale, &first_allocation_probe_scale,
       &second_allocation_probe_scale, &allocation_allow_further_probing},
      key_value_config->Lookup("WebRTC-Bwe-ProbingConfiguration"));

  // Specialized keys overriding subsets of WebRTC-Bwe-ProbingConfiguration
  ParseFieldTrial(
      {&first_exponential_probe_scale, &second_exponential_probe_scale},
      key_value_config->Lookup("WebRTC-Bwe-InitialProbing"));
  ParseFieldTrial({&further_exponential_probe_scale, &further_probe_threshold},
                  key_value_config->Lookup("WebRTC-Bwe-ExponentialProbing"));
  ParseFieldTrial({&alr_probing_interval, &alr_probe_scale},
                  key_value_config->Lookup("WebRTC-Bwe-AlrProbing"));
  ParseFieldTrial(
      {&first_allocation_probe_scale, &second_allocation_probe_scale,
       &allocation_allow_further_probing, &allocation_probe_max},
      key_value_config->Lookup("WebRTC-Bwe-AllocationProbing"));
}

ProbeControllerConfig::ProbeControllerConfig(const ProbeControllerConfig&) =
    default;
ProbeControllerConfig::~ProbeControllerConfig() = default;

ProbeController::ProbeController(const WebRtcKeyValueConfig* key_value_config,
                                 RtcEventLog* event_log)
    : enable_periodic_alr_probing_(false),
      in_rapid_recovery_experiment_(
          key_value_config->Lookup(kBweRapidRecoveryExperiment)
              .find("Enabled") == 0),
      limit_probes_with_allocateable_rate_(
          key_value_config->Lookup(kCappedProbingFieldTrialName)
              .find("Disabled") != 0),
      event_log_(event_log),
      config_(ProbeControllerConfig(key_value_config)) {
  Reset(0);
}

ProbeController::~ProbeController() {}
/**
 * 该方法用于设置带宽范围并触发探测:
 * @param min_bitrate_bps: 最小带宽，保障音视频流的最低需求
 * @param start_bitrate_bps: 初始带宽，起点估计值（若未提供则用min_bitrate_bps）
 * @param max_bitrate_bps：探测上限，动态调整的关键依据
 */
std::vector<ProbeClusterConfig> ProbeController::SetBitrates(
    int64_t min_bitrate_bps,
    int64_t start_bitrate_bps,
    int64_t max_bitrate_bps,
    int64_t at_time_ms) {
  // 设置初始带宽（码率）
  if (start_bitrate_bps > 0) {
    start_bitrate_bps_ = start_bitrate_bps; // 优先使用外部指定的初始带宽
    estimated_bitrate_bps_ = start_bitrate_bps; // 同步更新当前估计值
  } else if (start_bitrate_bps_ == 0) {
    start_bitrate_bps_ = min_bitrate_bps; // 默认回退到最小带宽
  }

  // The reason we use the variable |old_max_bitrate_pbs| is because we
  // need to set |max_bitrate_bps_| before we call InitiateProbing.
  // 2.更新最大带宽，并记录旧值
  int64_t old_max_bitrate_bps = max_bitrate_bps_;
  max_bitrate_bps_ = max_bitrate_bps; // 更新为新的最大带宽
  // 3.状态机处理
  switch (state_) {
    // 1）初始状态：快速逼近真实可用带宽
    case State::kInit:
      if (network_available_)
        return InitiateExponentialProbing(at_time_ms); // 从初始状态发起指数级增长的探测（如从500kbps→1Mbps→2Mbps等）
      break;
    // 2）探测中状态：不执行任何操作，等待当前探测完成，避免重叠探测导致网络过载
    case State::kWaitingForProbingResult:
      break;
    // 3）探测完成状态
    case State::kProbingComplete:
      // If the new max bitrate is higher than both the old max bitrate and the
      // estimate then initiate probing.
      // 这里的条件是当前带宽估计码率不为0，并且当前的估计码率比用户预设的要小，则进行探测，这里并没有进行指数探测
      // 并且是不进行更多探测further为false.
      if (estimated_bitrate_bps_ != 0 &&
          old_max_bitrate_bps < max_bitrate_bps_ &&
          estimated_bitrate_bps_ < max_bitrate_bps_) {
        // The assumption is that if we jump more than 20% in the bandwidth
        // estimate or if the bandwidth estimate is within 90% of the new
        // max bitrate then the probing attempt was successful.
        // 后续实际带宽达到mid_call_probing_succcess_threshold_（当前估计值的1.2倍或最大带宽的90%）
        // 设置中途探测成功阈值：
        // i）最大带宽更新时，通过中途探测验证是否可实际利用新增带宽
        // ii）中途探测采用单次探测（非指数），直接测试新上限是否可达。
        mid_call_probing_succcess_threshold_ =
            std::min(estimated_bitrate_bps_ * 1.2, max_bitrate_bps_ * 0.9);
        // 标记探测进行中，并记录目标带宽
        mid_call_probing_waiting_for_result_ = true;
        mid_call_probing_bitrate_bps_ = max_bitrate_bps_;

        RTC_HISTOGRAM_COUNTS_10000("WebRTC.BWE.MidCallProbing.Initiated",
                                   max_bitrate_bps_ / 1000);
        // 发起单次探测（目标值为新的 max_bitrate_bps_ ）
        return InitiateProbing(at_time_ms, {max_bitrate_bps_}, false);
      }
      break;
  }
  return std::vector<ProbeClusterConfig>();
}
/**
 * 该方法在网络总分配带宽（max_total_allocated_bitrate）更新时被调用，用于决定是否需要发起带宽探测，
 * 以验证是否可以利用新增的带宽资源。核心应用场景包括：
 * 1.多流竞争场景：例如视频会议中有多个参与者共享带宽时，某条流的带宽分配变化。
 * 2.网络容量动态调整：当底层网络（如蜂窝网络）通知可用带宽变化时。
 * 3.用户切换画质，对编码器流级别的码率进行更新的时候会用到
 * 
 * @param max_total_allocated_bitrate：新的总分配的带宽，由带宽分配模块（如 BitrateAllocator）提供
 * @param at_time_ms：当前时间戳，用于标记探测触发时间
 */
std::vector<ProbeClusterConfig> ProbeController::OnMaxTotalAllocatedBitrate(
    int64_t max_total_allocated_bitrate,
    int64_t at_time_ms) {
  // 1.探测条件检查
  // in_alr：是否处于应用限制区域（Application Limited Region，ALR），表示发送端有数据积压
  const bool in_alr = alr_start_time_ms_.has_value();
  // 仅当 in_alr 为 true 时允许探测，避免在无数据发送时浪费资源
  const bool allow_allocation_probe = in_alr;
  /**
   * kProbingComplete：避免与其他探测（如初始指数探测）冲突。
   * 带宽增长：只有新分配带宽（max_total_allocated_bitrate）高于当前估计值（estimated_bitrate_bps_）时才探测。
   * ALR限制：确保发送端有足够数据可以立即利用探测到的带宽（避免空探测）。
   */
  if (state_ == State::kProbingComplete && // 仅在探测完成状态下触发
      max_total_allocated_bitrate != max_total_allocated_bitrate_ && // 总分配带宽确实变化
      estimated_bitrate_bps_ != 0 && // 当前带宽估计值有效
      (max_bitrate_bps_ <= 0 || estimated_bitrate_bps_ < max_bitrate_bps_) && // 未达到单流上限
      estimated_bitrate_bps_ < max_total_allocated_bitrate && // 当前估计低于最新分配带宽
      allow_allocation_probe) { // 处于ALR状态（有数据可发）
    max_total_allocated_bitrate_ = max_total_allocated_bitrate;
    // 首次探测的带宽比例（如1.5倍），用于保守试探
    if (!config_.first_allocation_probe_scale)
      return std::vector<ProbeClusterConfig>();
    // 2.计算探测带宽目标：
    DataRate first_probe_rate =
        DataRate::BitsPerSec(max_total_allocated_bitrate) *
        config_.first_allocation_probe_scale.Value();
    // 探测带宽的绝对上限（如3Mbps），防止过度占用
    DataRate probe_cap = config_.allocation_probe_max.Get();
    first_probe_rate = std::min(first_probe_rate, probe_cap); // 取比例值与上限的较小者
    std::vector<int64_t> probes = {first_probe_rate.bps()};
    // 3.可选二次探测
    // 作用：如果首次探测成功（如实际带宽接近 3Mbps），则尝试更高目标（如 4Mbps）。
    // 保守设计：二次探测仅在配置了 second_allocation_probe_scale 且目标值更高时触发。
    if (config_.second_allocation_probe_scale) {
      DataRate second_probe_rate =
          DataRate::BitsPerSec(max_total_allocated_bitrate) *
          config_.second_allocation_probe_scale.Value();
      second_probe_rate = std::min(second_probe_rate, probe_cap);
      if (second_probe_rate > first_probe_rate)
        probes.push_back(second_probe_rate.bps()); // 第二次探测目标更高
    }
    // 4.发起探测
    // probes：待探测的带宽目标列表（如 [3Mbps, 4Mbps]）。
    // allocation_allow_further_probing：是否允许基于探测结果继续试探更高带宽（受全局配置控制）
    return InitiateProbing(at_time_ms, probes,
                           config_.allocation_allow_further_probing);
  }
  max_total_allocated_bitrate_ = max_total_allocated_bitrate;
  return std::vector<ProbeClusterConfig>();
}
/**
 * 响应网络可用性变化事件：
 * 网络不可用时：立即终止正在进行的带宽探测（避免无效发包）
 * 网络恢复可用时：在合适的条件下触发初始探测（快速建立可用带宽基线）
 */
std::vector<ProbeClusterConfig> ProbeController::OnNetworkAvailability(
    NetworkAvailability msg) {
  // 1.更新网络状态（network_available_网络状态可用标志（true可用，false不可用））
  network_available_ = msg.network_available;
  // 2.处理网络不可用情况，速失败（Fail Fast）原则，及时释放资源
  // （state_：探测控制器（ProbeController）当前的状态）
  if (!network_available_ && state_ == State::kWaitingForProbingResult) {
    state_ = State::kProbingComplete; // 强制终止探测状态
    min_bitrate_to_probe_further_bps_ = kExponentialProbingDisabled; // 禁用后续探测
  }
  // 3.处理网络恢复可用情况
  // start_bitrate_bps_：初始比特率配置值
  if (network_available_ && state_ == State::kInit && start_bitrate_bps_ > 0)
    return InitiateExponentialProbing(msg.at_time.ms()); // 启动带宽探测
  return std::vector<ProbeClusterConfig>();
}
/**
 * 初始状态，触发探测会走这儿（会进行指数探测）
 * 
 * 1.第一个为3.0倍start_bitrate_，第二个为6.0倍start_bitrate_。
 * 2.由此可看出，webrtc的start_bitrate不宜设置太高，如果设置太高很容易出现探测带宽太高，造成丢包现象，
 *   应该根据实际业务，当前目标画质进行配置。
 * 3.最终调用InitiateProbing生成ProbeClusterConfig集合，这里会生成两个探测族源数据。
 */
std::vector<ProbeClusterConfig> ProbeController::InitiateExponentialProbing(
    int64_t at_time_ms) {
  RTC_DCHECK(network_available_);
  RTC_DCHECK(state_ == State::kInit);
  RTC_DCHECK_GT(start_bitrate_bps_, 0);

  // When probing at 1.8 Mbps ( 6x 300), this represents a threshold of
  // 1.2 Mbps to continue probing.
  // 默认 first_exponential_probe_scale 为 3.0
  std::vector<int64_t> probes = {static_cast<int64_t>(
      config_.first_exponential_probe_scale * start_bitrate_bps_)};
  // 默认 second_exponential_probe_scale 为6.0
  // 初始阶段使用指数探测
  if (config_.second_exponential_probe_scale) {
    probes.push_back(config_.second_exponential_probe_scale.Value() *
                     start_bitrate_bps_);
  }
  return InitiateProbing(at_time_ms, probes, true);
}

std::vector<ProbeClusterConfig> ProbeController::SetEstimatedBitrate(
    int64_t bitrate_bps,
    int64_t at_time_ms) {
  if (mid_call_probing_waiting_for_result_ &&
      bitrate_bps >= mid_call_probing_succcess_threshold_) {
    RTC_HISTOGRAM_COUNTS_10000("WebRTC.BWE.MidCallProbing.Success",
                               mid_call_probing_bitrate_bps_ / 1000);
    RTC_HISTOGRAM_COUNTS_10000("WebRTC.BWE.MidCallProbing.ProbedKbps",
                               bitrate_bps / 1000);
    mid_call_probing_waiting_for_result_ = false;
  }
  std::vector<ProbeClusterConfig> pending_probes;
  if (state_ == State::kWaitingForProbingResult) {
    // Continue probing if probing results indicate channel has greater
    // capacity.
    RTC_LOG(LS_INFO) << "Measured bitrate: " << bitrate_bps
                     << " Minimum to probe further: "
                     << min_bitrate_to_probe_further_bps_;

    if (min_bitrate_to_probe_further_bps_ != kExponentialProbingDisabled &&
        bitrate_bps > min_bitrate_to_probe_further_bps_) {
      pending_probes = InitiateProbing(
          at_time_ms,
          {static_cast<int64_t>(config_.further_exponential_probe_scale *
                                bitrate_bps)},
          true);
    }
  }

  if (bitrate_bps < kBitrateDropThreshold * estimated_bitrate_bps_) {
    time_of_last_large_drop_ms_ = at_time_ms;
    bitrate_before_last_large_drop_bps_ = estimated_bitrate_bps_;
  }

  estimated_bitrate_bps_ = bitrate_bps;
  return pending_probes;
}

void ProbeController::EnablePeriodicAlrProbing(bool enable) {
  enable_periodic_alr_probing_ = enable;
}

void ProbeController::SetAlrStartTimeMs(
    absl::optional<int64_t> alr_start_time_ms) {
  alr_start_time_ms_ = alr_start_time_ms;
}
void ProbeController::SetAlrEndedTimeMs(int64_t alr_end_time_ms) {
  alr_end_time_ms_.emplace(alr_end_time_ms);
}

std::vector<ProbeClusterConfig> ProbeController::RequestProbe(
    int64_t at_time_ms) {
  // Called once we have returned to normal state after a large drop in
  // estimated bandwidth. The current response is to initiate a single probe
  // session (if not already probing) at the previous bitrate.
  //
  // If the probe session fails, the assumption is that this drop was a
  // real one from a competing flow or a network change.
  bool in_alr = alr_start_time_ms_.has_value();
  bool alr_ended_recently =
      (alr_end_time_ms_.has_value() &&
       at_time_ms - alr_end_time_ms_.value() < kAlrEndedTimeoutMs);
  if (in_alr || alr_ended_recently || in_rapid_recovery_experiment_) {
    if (state_ == State::kProbingComplete) {
      uint32_t suggested_probe_bps =
          kProbeFractionAfterDrop * bitrate_before_last_large_drop_bps_;
      uint32_t min_expected_probe_result_bps =
          (1 - kProbeUncertainty) * suggested_probe_bps;
      int64_t time_since_drop_ms = at_time_ms - time_of_last_large_drop_ms_;
      int64_t time_since_probe_ms = at_time_ms - last_bwe_drop_probing_time_ms_;
      if (min_expected_probe_result_bps > estimated_bitrate_bps_ &&
          time_since_drop_ms < kBitrateDropTimeoutMs &&
          time_since_probe_ms > kMinTimeBetweenAlrProbesMs) {
        RTC_LOG(LS_INFO) << "Detected big bandwidth drop, start probing.";
        // Track how often we probe in response to bandwidth drop in ALR.
        RTC_HISTOGRAM_COUNTS_10000(
            "WebRTC.BWE.BweDropProbingIntervalInS",
            (at_time_ms - last_bwe_drop_probing_time_ms_) / 1000);
        last_bwe_drop_probing_time_ms_ = at_time_ms;
        return InitiateProbing(at_time_ms, {suggested_probe_bps}, false);
      }
    }
  }
  return std::vector<ProbeClusterConfig>();
}

void ProbeController::SetMaxBitrate(int64_t max_bitrate_bps) {
  max_bitrate_bps_ = max_bitrate_bps;
}

void ProbeController::Reset(int64_t at_time_ms) {
  network_available_ = true;
  state_ = State::kInit;
  min_bitrate_to_probe_further_bps_ = kExponentialProbingDisabled;
  time_last_probing_initiated_ms_ = 0;
  estimated_bitrate_bps_ = 0;
  start_bitrate_bps_ = 0;
  max_bitrate_bps_ = 0;
  int64_t now_ms = at_time_ms;
  last_bwe_drop_probing_time_ms_ = now_ms;
  alr_end_time_ms_.reset();
  mid_call_probing_waiting_for_result_ = false;
  time_of_last_large_drop_ms_ = now_ms;
  bitrate_before_last_large_drop_bps_ = 0;
  max_total_allocated_bitrate_ = 0;
}

std::vector<ProbeClusterConfig> ProbeController::Process(int64_t at_time_ms) {
  if (at_time_ms - time_last_probing_initiated_ms_ >
      kMaxWaitingTimeForProbingResultMs) {
    mid_call_probing_waiting_for_result_ = false;

    if (state_ == State::kWaitingForProbingResult) {
      RTC_LOG(LS_INFO) << "kWaitingForProbingResult: timeout";
      state_ = State::kProbingComplete;
      min_bitrate_to_probe_further_bps_ = kExponentialProbingDisabled;
    }
  }

  if (enable_periodic_alr_probing_ && state_ == State::kProbingComplete) {
    // Probe bandwidth periodically when in ALR state.
    if (alr_start_time_ms_ && estimated_bitrate_bps_ > 0) {
      int64_t next_probe_time_ms =
          std::max(*alr_start_time_ms_, time_last_probing_initiated_ms_) +
          config_.alr_probing_interval->ms();
      if (at_time_ms >= next_probe_time_ms) {
        return InitiateProbing(at_time_ms,
                               {static_cast<int64_t>(estimated_bitrate_bps_ *
                                                     config_.alr_probe_scale)},
                               true);
      }
    }
  }
  return std::vector<ProbeClusterConfig>();
}
/**
 * 初始化主动带宽探测，生成一组探测配置（ProbeClusterConfig），
 * 目的是：通过短期发送高强度的探测数据包，快速测量当前网络的最大可用带宽。
 * @param now_ms: 当前时间戳，用于标记探测启动时间
 * @param bitrates_to_probe：待探测的目标比特率列表（探测的比特率都是输入的）
 * @param probe_further: 是否在首次探测后继续发起更高带宽的探测
 * 
 * 在新版本的webrtc中引入了几个新特性：
 * 1) 属于保守探测，判断当前的估计带宽是否已经大于最大探测码率的某个比例，如果是则表示探测完成，
 *    这样的好处有，其一是避免带宽浪费，其二是也可以降低一些不必要的丢包。
 * 3) 引入拥塞控制状态进行调整和控制，当识别到强求延迟增加、处于过载降码率的情况下直接放弃探测。
 */
std::vector<ProbeClusterConfig> ProbeController::InitiateProbing(
    int64_t now_ms,
    std::vector<int64_t> bitrates_to_probe,
    bool probe_further) {
  // 1. 确定最大探测带宽上限
  // 逻辑：如果用户设置了最大带宽限制（max_bitrate_bps_），则使用该值，否则使用默认值kDefaultMaxProbingBitrateBps（5Mbps）
  int64_t max_probe_bitrate_bps =
      max_bitrate_bps_ > 0 ? max_bitrate_bps_ : kDefaultMaxProbingBitrateBps;
  if (limit_probes_with_allocateable_rate_ &&
      max_total_allocated_bitrate_ > 0) {
    // If a max allocated bitrate has been configured, allow probing up to 2x
    // that rate. This allows some overhead to account for bursty streams,
    // which otherwise would have to ramp up when the overshoot is already in
    // progress.
    // It also avoids minor quality reduction caused by probes often being
    // received at slightly less than the target probe bitrate.
    // 2.考虑已分配的带宽限制
    // 作用：如果启用了动态带宽分配（如多流场景），探测带宽上限不超过已分配带宽的2被，避免过度抢占其他流的资源
    // 设计意图：允许一定突发（bursty）空间，同时防止因探测导致其他流质量下降
    max_probe_bitrate_bps =
        std::min(max_probe_bitrate_bps, max_total_allocated_bitrate_ * 2);
  }
  // 3.生成探测配置
  std::vector<ProbeClusterConfig> pending_probes;
  for (int64_t bitrate : bitrates_to_probe) {
    RTC_DCHECK_GT(bitrate, 0);

    // 超过上限值时候，停止进一步探测
    if (bitrate > max_probe_bitrate_bps) {
      bitrate = max_probe_bitrate_bps;
      probe_further = false;
    }
    // 生成探测配置
    ProbeClusterConfig config;
    config.at_time = Timestamp::Millis(now_ms); // 探测开始时间
    config.target_data_rate =
        DataRate::BitsPerSec(rtc::dchecked_cast<int>(bitrate)); // 目标比特率
    config.target_duration = TimeDelta::Millis(kMinProbeDurationMs); // 最小探测时长
    config.target_probe_count = kMinProbePacketsSent; // 最少探测包数量
    config.id = next_probe_cluster_id_; // 唯一ID
    next_probe_cluster_id_++; 
    MaybeLogProbeClusterCreated(event_log_, config);
    pending_probes.push_back(config);
  }
  time_last_probing_initiated_ms_ = now_ms;
  // 4.状态机更新
  if (probe_further) {
    // 等待首次探测结果
    state_ = State::kWaitingForProbingResult;
    min_bitrate_to_probe_further_bps_ =
        (*(bitrates_to_probe.end() - 1)) * config_.further_probe_threshold;
  } else {
    // 探测结束
    state_ = State::kProbingComplete;
    min_bitrate_to_probe_further_bps_ = kExponentialProbingDisabled;
  }
  return pending_probes;
}

}  // namespace webrtc
