/*
 *  Copyright (c) 2012 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "modules/pacing/paced_sender.h"

#include <algorithm>
#include <utility>
#include <vector>

#include "absl/memory/memory.h"
#include "api/rtc_event_log/rtc_event_log.h"
#include "modules/utility/include/process_thread.h"
#include "rtc_base/checks.h"
#include "rtc_base/location.h"
#include "rtc_base/logging.h"
#include "rtc_base/time_utils.h"
#include "system_wrappers/include/clock.h"

namespace webrtc {
const int64_t PacedSender::kMaxQueueLengthMs = 2000;
const float PacedSender::kDefaultPaceMultiplier = 2.5f;

PacedSender::PacedSender(Clock* clock,
                         PacketRouter* packet_router,
                         RtcEventLog* event_log,
                         const WebRtcKeyValueConfig* field_trials,
                         ProcessThread* process_thread)
    : process_mode_((field_trials != nullptr &&
                     field_trials->Lookup("WebRTC-Pacer-DynamicProcess")
                             .find("Enabled") == 0)
                        ? PacingController::ProcessMode::kDynamic
                        : PacingController::ProcessMode::kPeriodic),
      pacing_controller_(clock, // 构造 PacingController
                         static_cast<PacingController::PacketSender*>(this),
                         event_log,
                         field_trials, // 设置PacedSender 处理模式为周期模式ProcessMode::kPeriodic
                         process_mode_),
      clock_(clock),
      packet_router_(packet_router),
      process_thread_(process_thread) {
  if (process_thread_)
    process_thread_->RegisterModule(&module_proxy_, RTC_FROM_HERE);
}

PacedSender::~PacedSender() {
  if (process_thread_) {
    process_thread_->DeRegisterModule(&module_proxy_);
  }
}

void PacedSender::CreateProbeCluster(DataRate bitrate, int cluster_id) {
  rtc::CritScope cs(&critsect_);
  return pacing_controller_.CreateProbeCluster(bitrate, cluster_id);
}

void PacedSender::Pause() {
  {
    rtc::CritScope cs(&critsect_);
    pacing_controller_.Pause();
  }

  // Tell the process thread to call our TimeUntilNextProcess() method to get
  // a new (longer) estimate for when to call Process().
  if (process_thread_) {
    process_thread_->WakeUp(&module_proxy_);
  }
}

void PacedSender::Resume() {
  {
    rtc::CritScope cs(&critsect_);
    pacing_controller_.Resume();
  }

  // Tell the process thread to call our TimeUntilNextProcess() method to
  // refresh the estimate for when to call Process().
  if (process_thread_) {
    process_thread_->WakeUp(&module_proxy_);
  }
}

void PacedSender::SetCongestionWindow(DataSize congestion_window_size) {
  {
    rtc::CritScope cs(&critsect_);
    pacing_controller_.SetCongestionWindow(congestion_window_size);
  }
  MaybeWakupProcessThread();
}

void PacedSender::UpdateOutstandingData(DataSize outstanding_data) {
  {
    rtc::CritScope cs(&critsect_);
    pacing_controller_.UpdateOutstandingData(outstanding_data);
  }
  MaybeWakupProcessThread();
}

void PacedSender::SetPacingRates(DataRate pacing_rate, DataRate padding_rate) {
  {
    rtc::CritScope cs(&critsect_);
    pacing_controller_.SetPacingRates(pacing_rate, padding_rate);
  }
  MaybeWakupProcessThread();
}

void PacedSender::EnqueuePackets(
    std::vector<std::unique_ptr<RtpPacketToSend>> packets) {
  {
    rtc::CritScope cs(&critsect_);
    for (auto& packet : packets) {
      pacing_controller_.EnqueuePacket(std::move(packet));
    }
  }
  MaybeWakupProcessThread();
}

void PacedSender::SetAccountForAudioPackets(bool account_for_audio) {
  rtc::CritScope cs(&critsect_);
  pacing_controller_.SetAccountForAudioPackets(account_for_audio);
}

void PacedSender::SetIncludeOverhead() {
  rtc::CritScope cs(&critsect_);
  pacing_controller_.SetIncludeOverhead();
}

void PacedSender::SetTransportOverhead(DataSize overhead_per_packet) {
  rtc::CritScope cs(&critsect_);
  pacing_controller_.SetTransportOverhead(overhead_per_packet);
}

TimeDelta PacedSender::ExpectedQueueTime() const {
  rtc::CritScope cs(&critsect_);
  return pacing_controller_.ExpectedQueueTime();
}

DataSize PacedSender::QueueSizeData() const {
  rtc::CritScope cs(&critsect_);
  return pacing_controller_.QueueSizeData();
}

absl::optional<Timestamp> PacedSender::FirstSentPacketTime() const {
  rtc::CritScope cs(&critsect_);
  return pacing_controller_.FirstSentPacketTime();
}

TimeDelta PacedSender::OldestPacketWaitTime() const {
  rtc::CritScope cs(&critsect_);
  return pacing_controller_.OldestPacketWaitTime();
}

int64_t PacedSender::TimeUntilNextProcess() {
  rtc::CritScope cs(&critsect_);

  Timestamp next_send_time = pacing_controller_.NextSendTime();
  TimeDelta sleep_time =
      std::max(TimeDelta::Zero(), next_send_time - clock_->CurrentTime());
  if (process_mode_ == PacingController::ProcessMode::kDynamic) {
    return std::max(sleep_time, PacingController::kMinSleepTime).ms();
  }
  return sleep_time.ms();
}

void PacedSender::Process() {
  rtc::CritScope cs(&critsect_);
  pacing_controller_.ProcessPackets();
}

void PacedSender::ProcessThreadAttached(ProcessThread* process_thread) {
  RTC_LOG(LS_INFO) << "ProcessThreadAttached 0x" << process_thread;
  RTC_DCHECK(!process_thread || process_thread == process_thread_);
}

void PacedSender::MaybeWakupProcessThread() {
  // Tell the process thread to call our TimeUntilNextProcess() method to get
  // a new time for when to call Process().
  if (process_thread_ &&
      process_mode_ == PacingController::ProcessMode::kDynamic) {
    process_thread_->WakeUp(&module_proxy_);
  }
}

void PacedSender::SetQueueTimeLimit(TimeDelta limit) {
  {
    rtc::CritScope cs(&critsect_);
    pacing_controller_.SetQueueTimeLimit(limit);
  }
  MaybeWakupProcessThread();
}

void PacedSender::SendRtpPacket(std::unique_ptr<RtpPacketToSend> packet,
                                const PacedPacketInfo& cluster_info) {
  critsect_.Leave();
  packet_router_->SendPacket(std::move(packet), cluster_info);
  critsect_.Enter();
}

std::vector<std::unique_ptr<RtpPacketToSend>> PacedSender::GeneratePadding(
    DataSize size) {
  std::vector<std::unique_ptr<RtpPacketToSend>> padding_packets;
  critsect_.Leave();
  padding_packets = packet_router_->GeneratePadding(size.bytes());
  critsect_.Enter();
  return padding_packets;
}
}  // namespace webrtc
