/*
 *  Copyright 2023 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#import "RTCRtpCodecCapability.h"

#include "api/rtp_parameters.h"

NS_ASSUME_NONNULL_BEGIN

@interface RTC_OBJC_TYPE (RTCRtpCodecCapability)
()

    @property(nonatomic, readonly) webrtc::RtpCodecCapability nativeCodecCapability;

- (instancetype)initWithNativeCodecCapability:
    (const webrtc::RtpCodecCapability &)nativeCodecCapability NS_DESIGNATED_INITIALIZER;

@end

NS_ASSUME_NONNULL_END
