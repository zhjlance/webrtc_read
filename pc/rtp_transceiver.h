/*
 *  Copyright 2017 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef PC_RTP_TRANSCEIVER_H_
#define PC_RTP_TRANSCEIVER_H_

#include <string>
#include <vector>

#include "api/rtp_transceiver_interface.h"
#include "pc/channel_interface.h"
#include "pc/channel_manager.h"
#include "pc/rtp_receiver.h"
#include "pc/rtp_sender.h"

namespace webrtc {

// Implementation of the public RtpTransceiverInterface.
//
// The RtpTransceiverInterface is only intended to be used with a PeerConnection
// that enables Unified Plan SDP. Thus, the methods that only need to implement
// public API features and are not used internally can assume exactly one sender
// and receiver.
//
// Since the RtpTransceiver is used internally by PeerConnection for tracking
// RtpSenders, RtpReceivers, and BaseChannels, and PeerConnection needs to be
// backwards compatible with Plan B SDP, this implementation is more flexible
// than that required by the WebRTC specification.
//
// With Plan B SDP, an RtpTransceiver can have any number of senders and
// receivers which map to a=ssrc lines in the m= section.
// With Unified Plan SDP, an RtpTransceiver will have exactly one sender and one
// receiver which are encapsulated by the m= section.
//
// This class manages the RtpSenders, RtpReceivers, and BaseChannel associated
// with this m= section. Since the transceiver, senders, and receivers are
// reference counted and can be referenced from JavaScript (in Chromium), these
// objects must be ready to live for an arbitrary amount of time. The
// BaseChannel is not reference counted and is owned by the ChannelManager, so
// the PeerConnection must take care of creating/deleting the BaseChannel and
// setting the channel reference in the transceiver to null when it has been
// deleted.
//
// The RtpTransceiver is specialized to either audio or video according to the
// MediaType specified in the constructor. Audio RtpTransceivers will have
// AudioRtpSenders, AudioRtpReceivers, and a VoiceChannel. Video RtpTransceivers
// will have VideoRtpSenders, VideoRtpReceivers, and a VideoChannel.
/**
 * RtpTransceiver 类是 WebRTC 架构中核心的 媒体流传输控制单元，直接关联到 SDP 协商、媒体轨道管理和网络传输。
 * 
 * 和其他组件之间的关系：
 * Stream 中的 Track 会通过 Transceiver 的 Sender 和 Receiver 进行传输和接收。
 * 多个 Stream 可以在同一个 PeerConnection 中进行处理，
 * 每个 Stream 内的 Track 都有对应的 Transceiver 负责其传输。
 */
class RtpTransceiver final
    : public rtc::RefCountedObject<RtpTransceiverInterface>,
      public sigslot::has_slots<> {
 public:
  // Construct a Plan B-style RtpTransceiver with no senders, receivers, or
  // channel set.
  // |media_type| specifies the type of RtpTransceiver (and, by transitivity,
  // the type of senders, receivers, and channel). Can either by audio or video.
  // 此构造函数用于创建一个 Plan B 风格的RtpTransceiver，初始时没有设置发送器、接收器或通道。
  // media_type参数指定了收发器的类型（音频或视频）。
  explicit RtpTransceiver(cricket::MediaType media_type);
  // Construct a Unified Plan-style RtpTransceiver with the given sender and
  // receiver. The media type will be derived from the media types of the sender
  // and receiver. The sender and receiver should have the same media type.
  // |HeaderExtensionsToOffer| is used for initializing the return value of
  // HeaderExtensionsToOffer().
  // 该构造函数用于创建一个 Unified Plan 风格的RtpTransceiver，接收一个发送器、一个接收器、
  // 一个通道管理器和一组 RTP 头扩展能力作为参数。媒体类型将从发送器和接收器的媒体类型推导得出。
  // Unified Plan（现代标准），每个transceiver严格对应一个m=section，包含1个发送端（Sender)+1个接收端（Receiver)
  RtpTransceiver(
      rtc::scoped_refptr<RtpSenderProxyWithInternal<RtpSenderInternal>> sender,
      rtc::scoped_refptr<RtpReceiverProxyWithInternal<RtpReceiverInternal>>
          receiver,
      cricket::ChannelManager* channel_manager,
      std::vector<RtpHeaderExtensionCapability> HeaderExtensionsToOffer);
  // 析构函数用于清理RtpTransceiver对象所占用的资源。
  ~RtpTransceiver() override;

  // Returns the Voice/VideoChannel set for this transceiver. May be null if
  // the transceiver is not in the currently set local/remote description.
  // 1. 通道管理
  // 责任：关联底层传输通道（如 VoiceChannel），但不拥有其生命周期
  // 调用时机：在 SetLocalDescription/SetRemoteDescription 中由 PeerConnection 触发
  cricket::ChannelInterface* channel() const { return channel_; }

  // Sets the Voice/VideoChannel. The caller must pass in the correct channel
  // implementation based on the type of the transceiver.
  void SetChannel(cricket::ChannelInterface* channel);

  // Adds an RtpSender of the appropriate type to be owned by this transceiver.
  // Must not be null.
  // 发送器管理：
  // AddSender()函数用于添加一个发送器到收发器中，sender参数不能为nullptr。
  // RemoveSender()函数用于移除指定的发送器，如果发送器不属于该收发器，则返回false。
  // senders()函数返回当前收发器所拥有的发送器列表。
  void AddSender(
      rtc::scoped_refptr<RtpSenderProxyWithInternal<RtpSenderInternal>> sender);

  // Removes the given RtpSender. Returns false if the sender is not owned by
  // this transceiver.
  bool RemoveSender(RtpSenderInterface* sender);

  // Returns a vector of the senders owned by this transceiver.
  std::vector<rtc::scoped_refptr<RtpSenderProxyWithInternal<RtpSenderInternal>>>
  senders() const {
    return senders_;
  }

  // Adds an RtpReceiver of the appropriate type to be owned by this
  // transceiver. Must not be null.
  // 接收器管理：
  // 这些函数与发送器管理函数类似，分别用于添加、移除和获取接收器。
  void AddReceiver(
      rtc::scoped_refptr<RtpReceiverProxyWithInternal<RtpReceiverInternal>>
          receiver);

  // Removes the given RtpReceiver. Returns false if the sender is not owned by
  // this transceiver.
  bool RemoveReceiver(RtpReceiverInterface* receiver);

  // Returns a vector of the receivers owned by this transceiver.
  std::vector<
      rtc::scoped_refptr<RtpReceiverProxyWithInternal<RtpReceiverInternal>>>
  receivers() const {
    return receivers_;
  }

  // 获取 Unified Plan 发送器和接收器的内部对象：
  // 这两个函数分别返回 Unified Plan 发送器和接收器的内部对象。
  // Returns the backing object for the transceiver's Unified Plan sender.
  rtc::scoped_refptr<RtpSenderInternal> sender_internal() const;

  // Returns the backing object for the transceiver's Unified Plan receiver.
  rtc::scoped_refptr<RtpReceiverInternal> receiver_internal() const;

  // mline 索引和 MID 管理（没啥屌用）
  // mline_index()和set_mline_index()函数用于获取和设置 mline 索引，set_mid()函数用于设置 MID（媒体标识符）。
  // 当 MID 不为nullptr时，收发器被认为与具有相同 MID 的媒体部分相关联。
  // RtpTransceivers are not associated until they have a corresponding media
  // section set in SetLocalDescription or SetRemoteDescription. Therefore,
  // when setting a local offer we need a way to remember which transceiver was
  // used to create which media section in the offer. Storing the mline index
  // in CreateOffer is specified in JSEP to allow us to do that.
  absl::optional<size_t> mline_index() const { return mline_index_; }
  void set_mline_index(absl::optional<size_t> mline_index) {
    mline_index_ = mline_index;
  }

  // Sets the MID for this transceiver. If the MID is not null, then the
  // transceiver is considered "associated" with the media section that has the
  // same MID.
  void set_mid(const absl::optional<std::string>& mid) { mid_ = mid; }

  // 6.方向管理：
  // 这些函数分别用于设置收发器的预期方向、当前协商的方向和触发的方向。
  // Sets the intended direction for this transceiver. Intended to be used
  // internally over SetDirection since this does not trigger a negotiation
  // needed callback.
  void set_direction(RtpTransceiverDirection direction) {
    direction_ = direction;
  }

  // Sets the current direction for this transceiver as negotiated in an offer/
  // answer exchange. The current direction is null before an answer with this
  // transceiver has been set.
  void set_current_direction(RtpTransceiverDirection direction);

  // Sets the fired direction for this transceiver. The fired direction is null
  // until SetRemoteDescription is called or an answer is set (either local or
  // remote).
  void set_fired_direction(RtpTransceiverDirection direction);

  // 7.其他设置和查询函数：
  // 这些函数用于设置和查询与AddTrack相关的标志，以及判断收发器是否曾经用于发送数据。
  // According to JSEP rules for SetRemoteDescription, RtpTransceivers can be
  // reused only if they were added by AddTrack.
  void set_created_by_addtrack(bool created_by_addtrack) {
    created_by_addtrack_ = created_by_addtrack;
  }
  // If AddTrack has been called then transceiver can't be removed during
  // rollback.
  void set_reused_for_addtrack(bool reused_for_addtrack) {
    reused_for_addtrack_ = reused_for_addtrack;
  }

  bool created_by_addtrack() const { return created_by_addtrack_; }

  bool reused_for_addtrack() const { return reused_for_addtrack_; }

  // Returns true if this transceiver has ever had the current direction set to
  // sendonly or sendrecv.
  bool has_ever_been_used_to_send() const {
    return has_ever_been_used_to_send_;
  }

  // Fired when the RtpTransceiver state changes such that negotiation is now
  // needed (e.g., in response to a direction change).
  // 8.信号槽：事件触发，方向变化时触发重协商
  // 通过 SignalNegotiationNeeded 通知 PC 发起新一轮协商
  sigslot::signal0<> SignalNegotiationNeeded;

  // RtpTransceiverInterface implementation.
  cricket::MediaType media_type() const override;
  absl::optional<std::string> mid() const override;
  rtc::scoped_refptr<RtpSenderInterface> sender() const override;
  rtc::scoped_refptr<RtpReceiverInterface> receiver() const override;
  bool stopped() const override;
  RtpTransceiverDirection direction() const override;
  void SetDirection(RtpTransceiverDirection new_direction) override;
  absl::optional<RtpTransceiverDirection> current_direction() const override;
  absl::optional<RtpTransceiverDirection> fired_direction() const override;
  // 3. 停止与回收
  // 行为：
  //  1) 关闭所有发送/接收流
  //  2) 标记 stopped_ = true
  //  3) 释放 Channel 引用
  // 注意：对象仍可能被 JavaScript 持有而存活
  void Stop() override;
  // 2. 编码器控制
  // 作用：限定可选的编解码器（影响 SDP 生成）
  // 实现：存储到 codec_preferences_ 并在 SDP 协商阶段应用
  RTCError SetCodecPreferences(
      rtc::ArrayView<RtpCodecCapability> codecs) override;
  std::vector<RtpCodecCapability> codec_preferences() const override {
    return codec_preferences_;
  }
  std::vector<RtpHeaderExtensionCapability> HeaderExtensionsToOffer()
      const override;

 private:
  void OnFirstPacketReceived(cricket::ChannelInterface* channel);

  const bool unified_plan_;
  const cricket::MediaType media_type_;
  // PlanB(老版本，允许一个transceiver关联多个发送/接收端（通过ssrc区分），用senders和receivers当容器)
  std::vector<rtc::scoped_refptr<RtpSenderProxyWithInternal<RtpSenderInternal>>>
      senders_;
  std::vector<
      rtc::scoped_refptr<RtpReceiverProxyWithInternal<RtpReceiverInternal>>>
      receivers_;

  bool stopped_ = false;
  // direction_：本地期望方向（通过 SetDirection() 修改）
  RtpTransceiverDirection direction_ = RtpTransceiverDirection::kInactive;
  // current_direction_：最后一次协商后的实际方向
  absl::optional<RtpTransceiverDirection> current_direction_;
  // fired_direction_：远程描述的临时方向
  absl::optional<RtpTransceiverDirection> fired_direction_;
  // 唯一标识一个 m= section
  absl::optional<std::string> mid_;
  // 关联 SDP 中的媒体行索引（在创建 Offer 时记录）
  absl::optional<size_t> mline_index_;
  // 标记 transceiver 是否由 AddTrack 创建（影响重用规则）
  bool created_by_addtrack_ = false; 
  bool reused_for_addtrack_ = false;
  bool has_ever_been_used_to_send_ = false;

  cricket::ChannelInterface* channel_ = nullptr;
  cricket::ChannelManager* channel_manager_ = nullptr;
  std::vector<RtpCodecCapability> codec_preferences_;
  std::vector<RtpHeaderExtensionCapability> HeaderExtensionsToOffer_;
};

BEGIN_SIGNALING_PROXY_MAP(RtpTransceiver)
PROXY_SIGNALING_THREAD_DESTRUCTOR()
PROXY_CONSTMETHOD0(cricket::MediaType, media_type)
PROXY_CONSTMETHOD0(absl::optional<std::string>, mid)
PROXY_CONSTMETHOD0(rtc::scoped_refptr<RtpSenderInterface>, sender)
PROXY_CONSTMETHOD0(rtc::scoped_refptr<RtpReceiverInterface>, receiver)
PROXY_CONSTMETHOD0(bool, stopped)
PROXY_CONSTMETHOD0(RtpTransceiverDirection, direction)
PROXY_METHOD1(void, SetDirection, RtpTransceiverDirection)
PROXY_CONSTMETHOD0(absl::optional<RtpTransceiverDirection>, current_direction)
PROXY_CONSTMETHOD0(absl::optional<RtpTransceiverDirection>, fired_direction)
PROXY_METHOD0(void, Stop)
PROXY_METHOD1(webrtc::RTCError,
              SetCodecPreferences,
              rtc::ArrayView<RtpCodecCapability>)
PROXY_CONSTMETHOD0(std::vector<RtpCodecCapability>, codec_preferences)
PROXY_CONSTMETHOD0(std::vector<RtpHeaderExtensionCapability>,
                   HeaderExtensionsToOffer)
END_PROXY_MAP()

}  // namespace webrtc

#endif  // PC_RTP_TRANSCEIVER_H_
