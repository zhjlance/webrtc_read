/*
 *  Copyright 2018 The WebRTC Project Authors. All rights reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef API_ASYNC_RESOLVER_FACTORY_H_
#define API_ASYNC_RESOLVER_FACTORY_H_

#include "rtc_base/async_resolver_interface.h"

namespace webrtc {

/**
 * AsyncResolverFactory类是一个抽象工厂，用于创建异步DNS解析器(AsyncResolverInterface)。
 * 核心功能：
 * 1、解耦DNS解析实现：
 *    1）允许客户端应用自定义DNS解析机制替代WebRTC默认实现；
 *    2）适用于需要特殊网络配置的场景（如代理、自定义DNS服务器等）；
 * 
 * 2、工厂模式应用：
 *    1）通过纯虚函数Create()强制子类实现具体的解析器创建逻辑；
 *    2）生命周期由调用者控制（需显式调用Destroy销毁对象）；
 */
// An abstract factory for creating AsyncResolverInterfaces. This allows
// client applications to provide WebRTC with their own mechanism for
// performing DNS resolution.
class AsyncResolverFactory {
 public:
  AsyncResolverFactory() = default;
  virtual ~AsyncResolverFactory() = default;

  // The caller should call Destroy on the returned object to delete it.
  virtual rtc::AsyncResolverInterface* Create() = 0;
};

}  // namespace webrtc

#endif  // API_ASYNC_RESOLVER_FACTORY_H_
