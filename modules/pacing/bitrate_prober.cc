/*
 *  Copyright (c) 2014 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "modules/pacing/bitrate_prober.h"

#include <algorithm>

#include "absl/memory/memory.h"
#include "api/rtc_event_log/rtc_event.h"
#include "api/rtc_event_log/rtc_event_log.h"
#include "logging/rtc_event_log/events/rtc_event_probe_cluster_created.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"
#include "system_wrappers/include/metrics.h"

namespace webrtc {

namespace {
// The min probe packet size is scaled with the bitrate we're probing at.
// This defines the max min probe packet size, meaning that on high bitrates
// we have a min probe packet size of 200 bytes.
constexpr size_t kMinProbePacketSize = 200;

constexpr TimeDelta kProbeClusterTimeout = TimeDelta::Seconds(5);

}  // namespace

BitrateProberConfig::BitrateProberConfig(
    const WebRtcKeyValueConfig* key_value_config)
    : min_probe_packets_sent("min_probe_packets_sent", 5),
      min_probe_delta("min_probe_delta", TimeDelta::Millis(1)),
      min_probe_duration("min_probe_duration", TimeDelta::Millis(15)),
      max_probe_delay("max_probe_delay", TimeDelta::Millis(3)) {
  ParseFieldTrial({&min_probe_packets_sent, &min_probe_delta,
                   &min_probe_duration, &max_probe_delay},
                  key_value_config->Lookup("WebRTC-Bwe-ProbingConfiguration"));
  ParseFieldTrial({&min_probe_packets_sent, &min_probe_delta,
                   &min_probe_duration, &max_probe_delay},
                  key_value_config->Lookup("WebRTC-Bwe-ProbingBehavior"));
}

BitrateProber::~BitrateProber() {
  RTC_HISTOGRAM_COUNTS_1000("WebRTC.BWE.Probing.TotalProbeClustersRequested",
                            total_probe_count_);
  RTC_HISTOGRAM_COUNTS_1000("WebRTC.BWE.Probing.TotalFailedProbeClusters",
                            total_failed_probe_count_);
}

BitrateProber::BitrateProber(const WebRtcKeyValueConfig& field_trials)
    : probing_state_(ProbingState::kDisabled),
      next_probe_time_(Timestamp::PlusInfinity()),
      total_probe_count_(0),
      total_failed_probe_count_(0),
      config_(&field_trials) {
  SetEnabled(true);
}

void BitrateProber::SetEnabled(bool enable) {
  if (enable) {
    if (probing_state_ == ProbingState::kDisabled) {
      probing_state_ = ProbingState::kInactive;
      RTC_LOG(LS_INFO) << "Bandwidth probing enabled, set to inactive";
    }
  } else {
    probing_state_ = ProbingState::kDisabled;
    RTC_LOG(LS_INFO) << "Bandwidth probing disabled";
  }
}

bool BitrateProber::IsProbing() const {
  return probing_state_ == ProbingState::kActive;
}
/**
 * BitrateProber::OnIncomingPacket 函数是 BitrateProber（比特率探测器）类的成员函数，
 * 主要作用是根据接收到的数据包大小和当前的探测状态，决定是否启动带宽探测。
 * 
 * 使用场景：
 * 在 WebRTC 的带宽探测机制中，当有数据包进入系统时，此函数会被调用，以判断是否满足启动带宽探测的条件。
 * 它将数据包的相关信息纳入带宽探测的决策逻辑，有助于动态适应网络状况并及时进行带宽评估。
 * @param packet_size: 入参为当前Rtp包的大小。
 */
void BitrateProber::OnIncomingPacket(size_t packet_size) {
  // Don't initialize probing unless we have something large enough to start
  // probing.
  if (probing_state_ == ProbingState::kInactive && !clusters_.empty() &&
      packet_size >=
          std::min<size_t>(RecommendedMinProbeSize(), kMinProbePacketSize)) {
    // Send next probe right away.
    next_probe_time_ = Timestamp::MinusInfinity();
    probing_state_ = ProbingState::kActive;
  }
}
/**
 * 1.函数概述：
 * 函数是 WebRTC 中用于创建带宽探测簇（probe cluster）的方法。
 * 带宽探测簇是一组具有相同探测比特率的探测数据包集合，用于测量网络在该比特率下的传输性能
 * 
 * 2.使用场景：
 * 在 WebRTC 的带宽探测机制中，当需要以特定比特率进行带宽探测时，会调用此函数创建相应的探测簇。
 * 例如，在初始带宽探测阶段，或者在网络状况发生变化后需要重新评估带宽时，都会根据设定的比特率来创建探测簇。
 */
void BitrateProber::CreateProbeCluster(DataRate bitrate,
                                       Timestamp now,
                                       int cluster_id) {
  // 首先确保探测状态不是禁用状态，并且传入的探测比特率大于零。
  // 这是合理的前提条件，因为如果探测被禁用或者比特率为零，创建探测簇就没有意义。
  RTC_DCHECK(probing_state_ != ProbingState::kDisabled);
  RTC_DCHECK_GT(bitrate, DataRate::Zero());

  total_probe_count_++;
  // 1.过期探测簇清理
  // 每次创建新的探测簇时，total_probe_count_ 计数增加。同时，检查并清理已经过期的探测簇。
  // clusters_ 是一个存储探测簇的队列，这里遍历队列头部的探测簇，如果其创建时间距离当前时间 now 超过了
  //  kProbeClusterTimeout（预设的探测簇超时时间），则将其从队列中移除，并增加失败探测簇计数 total_failed_probe_count_。
  // 这一步保证了队列中只保留有效的、未过期的探测簇。
  while (!clusters_.empty() &&
         now - clusters_.front().created_at > kProbeClusterTimeout) {
    clusters_.pop();
    total_failed_probe_count_++;
  }
  // 2.创建新的探测簇
  // 创建一个新的 ProbeCluster 对象 cluster。记录其创建时间为当前时间 now。
  // 根据配置信息设置最小探测数据包数量 probe_cluster_min_probes，以及根据传入的比特率 bitrate 和
  // 最小探测时长 config_.min_probe_duration 计算出最小探测字节数 probe_cluster_min_bytes，并确保其不小于零。
  // 设置探测簇的发送比特率 send_bitrate_bps 和唯一的探测簇 ID probe_cluster_id。
  // 最后将新创建的探测簇加入到 clusters_ 队列中。
  ProbeCluster cluster;
  cluster.created_at = now;
  cluster.pace_info.probe_cluster_min_probes = config_.min_probe_packets_sent; // 每组探测的最小包数（保证统计显著性）
  cluster.pace_info.probe_cluster_min_bytes =
      (bitrate * config_.min_probe_duration.Get()).bytes(); // 每组探测的最小持续时间（计算结果：bitrate * duration = min_bytes）
  RTC_DCHECK_GE(cluster.pace_info.probe_cluster_min_bytes, 0);
  cluster.pace_info.send_bitrate_bps = bitrate.bps(); // 探测目标码率（激进但可控）
  cluster.pace_info.probe_cluster_id = cluster_id;
  clusters_.push(cluster);

  RTC_LOG(LS_INFO) << "Probe cluster (bitrate:min bytes:min packets): ("
                   << cluster.pace_info.send_bitrate_bps << ":"
                   << cluster.pace_info.probe_cluster_min_bytes << ":"
                   << cluster.pace_info.probe_cluster_min_probes << ")";
  // If we are already probing, continue to do so. Otherwise set it to
  // kInactive and wait for OnIncomingPacket to start the probing.
  // 3.更新探测状态:
  // 如果当前探测状态不是活跃状态 kActive，则将其设置为非活跃状态 kInactive。
  // 这意味着探测尚未真正开始，等待 OnIncomingPacket 函数（可能是接收到某个特定数据包的回调函数）
  // 来触发实际的探测操作。这种设计使得探测操作可以根据数据包的接收情况灵活启动，
  // 提高了带宽探测与数据传输流程的协同性。
  if (probing_state_ != ProbingState::kActive)
    probing_state_ = ProbingState::kInactive;
}

Timestamp BitrateProber::NextProbeTime(Timestamp now) const {
  // Probing is not active or probing is already complete.
  if (probing_state_ != ProbingState::kActive || clusters_.empty()) {
    return Timestamp::PlusInfinity();
  }

  if (next_probe_time_.IsFinite() &&
      now - next_probe_time_ > config_.max_probe_delay.Get()) {
    RTC_DLOG(LS_WARNING) << "Probe delay too high"
                            " (next_ms:"
                         << next_probe_time_.ms() << ", now_ms: " << now.ms()
                         << ")";
    return Timestamp::PlusInfinity();
  }

  return next_probe_time_;
}

PacedPacketInfo BitrateProber::CurrentCluster() const {
  RTC_DCHECK(!clusters_.empty());
  RTC_DCHECK(probing_state_ == ProbingState::kActive);
  PacedPacketInfo info = clusters_.front().pace_info;
  info.probe_cluster_bytes_sent = clusters_.front().sent_bytes;
  return info;
}

// Probe size is recommended based on the probe bitrate required. We choose
// a minimum of twice |kMinProbeDeltaMs| interval to allow scheduling to be
// feasible.
size_t BitrateProber::RecommendedMinProbeSize() const {
  RTC_DCHECK(!clusters_.empty());
  return clusters_.front().pace_info.send_bitrate_bps * 2 *
         config_.min_probe_delta->ms() / (8 * 1000);
}

void BitrateProber::ProbeSent(Timestamp now, size_t bytes) {
  RTC_DCHECK(probing_state_ == ProbingState::kActive);
  RTC_DCHECK_GT(bytes, 0);

  if (!clusters_.empty()) {
    ProbeCluster* cluster = &clusters_.front();
    if (cluster->sent_probes == 0) {
      RTC_DCHECK(cluster->started_at.IsInfinite());
      cluster->started_at = now;
    }
    cluster->sent_bytes += static_cast<int>(bytes);
    cluster->sent_probes += 1;
    next_probe_time_ = CalculateNextProbeTime(*cluster);
    if (cluster->sent_bytes >= cluster->pace_info.probe_cluster_min_bytes &&
        cluster->sent_probes >= cluster->pace_info.probe_cluster_min_probes) {
      RTC_HISTOGRAM_COUNTS_100000("WebRTC.BWE.Probing.ProbeClusterSizeInBytes",
                                  cluster->sent_bytes);
      RTC_HISTOGRAM_COUNTS_100("WebRTC.BWE.Probing.ProbesPerCluster",
                               cluster->sent_probes);
      RTC_HISTOGRAM_COUNTS_10000("WebRTC.BWE.Probing.TimePerProbeCluster",
                                 (now - cluster->started_at).ms());

      clusters_.pop();
    }
    if (clusters_.empty())
      probing_state_ = ProbingState::kSuspended;
  }
}

Timestamp BitrateProber::CalculateNextProbeTime(
    const ProbeCluster& cluster) const {
  RTC_CHECK_GT(cluster.pace_info.send_bitrate_bps, 0);
  RTC_CHECK(cluster.started_at.IsFinite());

  // Compute the time delta from the cluster start to ensure probe bitrate stays
  // close to the target bitrate. Result is in milliseconds.
  DataSize sent_bytes = DataSize::Bytes(cluster.sent_bytes);
  DataRate send_bitrate =
      DataRate::BitsPerSec(cluster.pace_info.send_bitrate_bps);
  TimeDelta delta = sent_bytes / send_bitrate;
  return cluster.started_at + delta;
}

}  // namespace webrtc
