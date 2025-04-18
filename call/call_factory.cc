/*
 *  Copyright 2017 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "call/call_factory.h"

#include <stdio.h>

#include <memory>
#include <string>

#include "absl/types/optional.h"
#include "api/test/simulated_network.h"
#include "call/call.h"
#include "call/degraded_call.h"
#include "rtc_base/checks.h"
#include "system_wrappers/include/field_trial.h"

namespace webrtc {
namespace {
bool ParseConfigParam(std::string exp_name, int* field) {
  std::string group = field_trial::FindFullName(exp_name);
  if (group.empty())
    return false;

  return (sscanf(group.c_str(), "%d", field) == 1);
}

absl::optional<webrtc::BuiltInNetworkBehaviorConfig> ParseDegradationConfig(
    bool send) {
  std::string exp_prefix = "WebRTCFakeNetwork";
  if (send) {
    exp_prefix += "Send";
  } else {
    exp_prefix += "Receive";
  }

  webrtc::BuiltInNetworkBehaviorConfig config;
  bool configured = false;
  configured |=
      ParseConfigParam(exp_prefix + "DelayMs", &config.queue_delay_ms);
  configured |= ParseConfigParam(exp_prefix + "DelayStdDevMs",
                                 &config.delay_standard_deviation_ms);
  int queue_length = 0;
  if (ParseConfigParam(exp_prefix + "QueueLength", &queue_length)) {
    RTC_CHECK_GE(queue_length, 0);
    config.queue_length_packets = queue_length;
    configured = true;
  }
  configured |=
      ParseConfigParam(exp_prefix + "CapacityKbps", &config.link_capacity_kbps);
  configured |=
      ParseConfigParam(exp_prefix + "LossPercent", &config.loss_percent);
  int allow_reordering = 0;
  if (ParseConfigParam(exp_prefix + "AllowReordering", &allow_reordering)) {
    config.allow_reordering = true;
    configured = true;
  }
  configured |= ParseConfigParam(exp_prefix + "AvgBurstLossLength",
                                 &config.avg_burst_loss_length);
  return configured
             ? absl::optional<webrtc::BuiltInNetworkBehaviorConfig>(config)
             : absl::nullopt;
}
}  // namespace
// 创建Call对象
Call* CallFactory::CreateCall(const Call::Config& config) {
  absl::optional<webrtc::BuiltInNetworkBehaviorConfig> send_degradation_config =
      ParseDegradationConfig(true);
  absl::optional<webrtc::BuiltInNetworkBehaviorConfig>
      receive_degradation_config = ParseDegradationConfig(false);

  if (send_degradation_config || receive_degradation_config) {
    return new DegradedCall(std::unique_ptr<Call>(Call::Create(config)),
                            send_degradation_config, receive_degradation_config,
                            config.task_queue_factory);
  }
  // 调用Call的静态Create方法创建
  return Call::Create(config);
}

std::unique_ptr<CallFactoryInterface> CreateCallFactory() {
  return std::unique_ptr<CallFactoryInterface>(new CallFactory());
}

}  // namespace webrtc
