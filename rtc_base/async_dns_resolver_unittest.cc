/*
 *  Copyright 2023 The WebRTC Project Authors. All rights reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "rtc_base/async_dns_resolver.h"

#include "rtc_base/gunit.h"
#include "test/gtest.h"
#include "test/run_loop.h"

namespace webrtc {
namespace {
const int kDefaultTimeout = 1000;
const int kPortNumber = 3027;

TEST(AsyncDnsResolver, ConstructorWorks) {
  AsyncDnsResolver resolver;
}

TEST(AsyncDnsResolver, ResolvingLocalhostWorks) {
  test::RunLoop loop;  // Ensure that posting back to main thread works
  AsyncDnsResolver resolver;
  rtc::SocketAddress address("localhost",
                             kPortNumber);  // Port number does not matter
  rtc::SocketAddress resolved_address;
  bool done = false;
  resolver.Start(address, [&done] { done = true; });
  ASSERT_TRUE_WAIT(done, kDefaultTimeout);
  EXPECT_EQ(resolver.result().GetError(), 0);
  EXPECT_TRUE(resolver.result().GetResolvedAddress(AF_INET, &resolved_address));
  EXPECT_EQ(resolved_address, rtc::SocketAddress("127.0.0.1", kPortNumber));
}

}  // namespace
}  // namespace webrtc
