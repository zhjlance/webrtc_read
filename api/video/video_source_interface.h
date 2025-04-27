/*
 *  Copyright (c) 2016 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef API_VIDEO_VIDEO_SOURCE_INTERFACE_H_
#define API_VIDEO_VIDEO_SOURCE_INTERFACE_H_

#include <limits>

#include "absl/types/optional.h"
#include "api/video/video_sink_interface.h"
#include "rtc_base/system/rtc_export.h"

namespace rtc {

// VideoSinkWants is used for notifying the source of properties a video frame
// should have when it is delivered to a certain sink.
/**
 * 消费者需求说明书：
 * 告诉视频源（如摄像头、屏幕采集）如何预处理帧数据，以满足特定消费者（如编码器、渲染器）
 * 的需求，避免不必要的计算或内存拷贝。
 * 
 * - **按需供给**：消费者声明需求，源端动态调整输出，避免资源浪费。
 * - **格式协商**：解决生产者（如摄像头输出RGBA）与消费者（如编码器需要I420）的格式冲突。
 */
/**
 * 应用场景：
 * 消费者1：需要720p@30fps
 * VideoSinkWants wants1;
 * wants1.max_pixel_count = 1280*720;
 * wants1.max_framerate_fps = 30;
 * 
 * 消费者2：需要1080p@15fps 
 * VideoSinkWants wants2;
 * wants2.max_pixel_count = 1920*1080;
 * wants2.max_framerate_fps = 15;
 * 
 * 源端实际输出：720p@15fps（取各维度的最小值）
 */
struct RTC_EXPORT VideoSinkWants {
  VideoSinkWants();
  VideoSinkWants(const VideoSinkWants&);
  ~VideoSinkWants();
  // Tells the source whether the sink wants frames with rotation applied.
  // By default, any rotation must be applied by the sink.
  // 是否要求源端处理旋转（否则消费者自行旋转），应用场景：移动端摄像头竖屏画面矫正
  bool rotation_applied = false;

  // Tells the source that the sink only wants black frames.
  // 是否仅需要黑帧（节省带宽），应用场景：用户主动关闭摄像头时的虚拟背景
  bool black_frames = false;

  // Tells the source the maximum number of pixels the sink wants.
  // 帧的最大像素数（超出需降分辨率），应用场景：弱网环境下降低分辨率
  int max_pixel_count = std::numeric_limits<int>::max();
  // Tells the source the desired number of pixels the sinks wants. This will
  // typically be used when stepping the resolution up again when conditions
  // have improved after an earlier downgrade. The source should select the
  // closest resolution to this pixel count, but if max_pixel_count is set, it
  // still sets the absolute upper bound.
  // 期望像素数（源端尽量接近该值），应用场景：网络恢复时渐进式提升分辨率
  absl::optional<int> target_pixel_count;
  // Tells the source the maximum framerate the sink wants.
  // 最大帧率限制，应用场景：CPU过热时降帧率
  int max_framerate_fps = std::numeric_limits<int>::max();

  // Tells the source that the sink wants width and height of the video frames
  // to be divisible by |resolution_alignment|.
  // For example: With I420, this value would be a multiple of 2.
  // Note that this field is unrelated to any horizontal or vertical stride
  // requirements the encoder has on the incoming video frame buffers.
  // 要求分辨率宽高是其整倍数（如2的倍数满足I420格式），应用场景：编码器输入尺寸对其要求
  int resolution_alignment = 1;
};

/**
 * 视频源控制器：
 * 管理所有消费者（VideoSink）的注册与需求汇总，充当多路视频分发的中枢。
 */
/**
 * 应用场景：
 * 初始：编码器和预览都需要1080p@30fps
 * camera_source->AddOrUpdateSink(encoder_sink, {.max_pixel_count=1920*1080, .max_framerate_fps=30});
 * camera_source->AddOrUpdateSink(preview_sink, {.max_pixel_count=1920*1080, .max_framerate_fps=30});
 * 
 * 网络变差：编码器降级到720p@15fps
 * camera_source->AddOrUpdateSink(encoder_sink, {.max_pixel_count=1280*720, .max_framerate_fps=15});
 * 源端自动调整输出（预览Sink也受影响）
 * 
 */
template <typename VideoFrameT>
class VideoSourceInterface {
 public:
  virtual ~VideoSourceInterface() = default;

  // 添加/更新消费者，并指定其需求（可能触发源端配置变更）
  virtual void AddOrUpdateSink(VideoSinkInterface<VideoFrameT>* sink,
                               const VideoSinkWants& wants) = 0;
  // RemoveSink must guarantee that at the time the method returns,
  // there is no current and no future calls to VideoSinkInterface::OnFrame.
  // 移除消费者，并确保不再向其推送帧（线程安全保证）
  virtual void RemoveSink(VideoSinkInterface<VideoFrameT>* sink) = 0;
};

}  // namespace rtc
#endif  // API_VIDEO_VIDEO_SOURCE_INTERFACE_H_
