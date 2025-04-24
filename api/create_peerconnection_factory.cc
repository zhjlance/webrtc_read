/*
 * @Author: zhjlance zhanghj0904@163.com
 * @Date: 2025-04-14 17:07:03
 * @LastEditors: zhjlance zhanghj0904@163.com
 * @LastEditTime: 2025-04-22 21:03:45
 * @FilePath: \src\api\create_peerconnection_factory.cc
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
/*
 *  Copyright 2018 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "api/create_peerconnection_factory.h"

#include <memory>
#include <utility>

#include "api/call/call_factory_interface.h"
#include "api/peer_connection_interface.h"
#include "api/rtc_event_log/rtc_event_log_factory.h"
#include "api/scoped_refptr.h"
#include "api/task_queue/default_task_queue_factory.h"
#include "media/base/media_engine.h"
#include "media/engine/webrtc_media_engine.h"
#include "modules/audio_device/include/audio_device.h"
#include "modules/audio_processing/include/audio_processing.h"
#include "rtc_base/thread.h"

namespace webrtc {

rtc::scoped_refptr<PeerConnectionFactoryInterface> CreatePeerConnectionFactory(
    rtc::Thread* network_thread,
    rtc::Thread* worker_thread,
    rtc::Thread* signaling_thread,
    rtc::scoped_refptr<AudioDeviceModule> default_adm,
    rtc::scoped_refptr<AudioEncoderFactory> audio_encoder_factory,
    rtc::scoped_refptr<AudioDecoderFactory> audio_decoder_factory,
    std::unique_ptr<VideoEncoderFactory> video_encoder_factory,
    std::unique_ptr<VideoDecoderFactory> video_decoder_factory,
    rtc::scoped_refptr<AudioMixer> audio_mixer,
    rtc::scoped_refptr<AudioProcessing> audio_processing) {
  PeerConnectionFactoryDependencies dependencies;
  dependencies.network_thread = network_thread;
  dependencies.worker_thread = worker_thread;
  dependencies.signaling_thread = signaling_thread;
  dependencies.task_queue_factory = CreateDefaultTaskQueueFactory();
  dependencies.call_factory = CreateCallFactory();
  dependencies.event_log_factory = std::make_unique<RtcEventLogFactory>(
      dependencies.task_queue_factory.get());

  cricket::MediaEngineDependencies media_dependencies;
  media_dependencies.task_queue_factory = dependencies.task_queue_factory.get();
  media_dependencies.adm = std::move(default_adm);
  media_dependencies.audio_encoder_factory = std::move(audio_encoder_factory);
  media_dependencies.audio_decoder_factory = std::move(audio_decoder_factory);
  if (audio_processing) {
    media_dependencies.audio_processing = std::move(audio_processing);
  } else {
    media_dependencies.audio_processing = AudioProcessingBuilder().Create();
  }
  media_dependencies.audio_mixer = std::move(audio_mixer);
  media_dependencies.video_encoder_factory = std::move(video_encoder_factory);
  media_dependencies.video_decoder_factory = std::move(video_decoder_factory);
  // 创建媒体引擎
  dependencies.media_engine =
      cricket::CreateMediaEngine(std::move(media_dependencies));

  return CreateModularPeerConnectionFactory(std::move(dependencies));
}

}  // namespace webrtc
