/*
 *  Copyright (c) 2020 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "modules/rtp_rtcp/source/rtp_sender_video_frame_transformer_delegate.h"

#include <utility>
#include <vector>

#include "api/sequence_checker.h"
#include "api/task_queue/task_queue_factory.h"
#include "modules/rtp_rtcp/source/rtp_descriptor_authentication.h"
#include "modules/rtp_rtcp/source/rtp_sender_video.h"
#include "rtc_base/checks.h"

namespace webrtc {
namespace {

class TransformableVideoSenderFrame : public TransformableVideoFrameInterface {
 public:
  TransformableVideoSenderFrame(
      const EncodedImage& encoded_image,
      const RTPVideoHeader& video_header,
      int payload_type,
      absl::optional<VideoCodecType> codec_type,
      uint32_t rtp_timestamp,
      absl::optional<int64_t> expected_retransmission_time_ms,
      uint32_t ssrc)
      : encoded_data_(encoded_image.GetEncodedData()),
        header_(video_header),
        metadata_(header_.GetAsMetadata()),
        frame_type_(encoded_image._frameType),
        payload_type_(payload_type),
        codec_type_(codec_type),
        timestamp_(rtp_timestamp),
        capture_time_ms_(encoded_image.capture_time_ms_),
        expected_retransmission_time_ms_(expected_retransmission_time_ms),
        ssrc_(ssrc) {
    RTC_DCHECK_GE(payload_type_, 0);
    RTC_DCHECK_LE(payload_type_, 127);
  }

  ~TransformableVideoSenderFrame() override = default;

  // Implements TransformableVideoFrameInterface.
  rtc::ArrayView<const uint8_t> GetData() const override {
    return *encoded_data_;
  }

  void SetData(rtc::ArrayView<const uint8_t> data) override {
    encoded_data_ = EncodedImageBuffer::Create(data.data(), data.size());
  }

  uint32_t GetTimestamp() const override { return timestamp_; }
  uint32_t GetSsrc() const override { return ssrc_; }

  bool IsKeyFrame() const override {
    return frame_type_ == VideoFrameType::kVideoFrameKey;
  }

  std::vector<uint8_t> GetAdditionalData() const override {
    return RtpDescriptorAuthentication(header_);
  }

  const VideoFrameMetadata& GetMetadata() const override { return metadata_; }

  const RTPVideoHeader& GetHeader() const { return header_; }
  uint8_t GetPayloadType() const override { return payload_type_; }
  absl::optional<VideoCodecType> GetCodecType() const { return codec_type_; }
  int64_t GetCaptureTimeMs() const { return capture_time_ms_; }

  const absl::optional<int64_t>& GetExpectedRetransmissionTimeMs() const {
    return expected_retransmission_time_ms_;
  }

  Direction GetDirection() const override { return Direction::kSender; }

 private:
  rtc::scoped_refptr<EncodedImageBufferInterface> encoded_data_;
  const RTPVideoHeader header_;
  const VideoFrameMetadata metadata_;
  const VideoFrameType frame_type_;
  const uint8_t payload_type_;
  const absl::optional<VideoCodecType> codec_type_ = absl::nullopt;
  const uint32_t timestamp_;
  const int64_t capture_time_ms_;
  const absl::optional<int64_t> expected_retransmission_time_ms_;
  const uint32_t ssrc_;
};
}  // namespace

RTPSenderVideoFrameTransformerDelegate::RTPSenderVideoFrameTransformerDelegate(
    RTPSenderVideo* sender,
    rtc::scoped_refptr<FrameTransformerInterface> frame_transformer,
    uint32_t ssrc,
    TaskQueueFactory* task_queue_factory)
    : sender_(sender),
      frame_transformer_(std::move(frame_transformer)),
      ssrc_(ssrc),
      transformation_queue_(task_queue_factory->CreateTaskQueue(
          "video_frame_transformer",
          TaskQueueFactory::Priority::NORMAL)) {}

void RTPSenderVideoFrameTransformerDelegate::Init() {
  frame_transformer_->RegisterTransformedFrameSinkCallback(
      rtc::scoped_refptr<TransformedFrameCallback>(this), ssrc_);
}

bool RTPSenderVideoFrameTransformerDelegate::TransformFrame(
    int payload_type,
    absl::optional<VideoCodecType> codec_type,
    uint32_t rtp_timestamp,
    const EncodedImage& encoded_image,
    RTPVideoHeader video_header,
    absl::optional<int64_t> expected_retransmission_time_ms) {
  frame_transformer_->Transform(std::make_unique<TransformableVideoSenderFrame>(
      encoded_image, video_header, payload_type, codec_type, rtp_timestamp,
      expected_retransmission_time_ms, ssrc_));
  return true;
}

void RTPSenderVideoFrameTransformerDelegate::OnTransformedFrame(
    std::unique_ptr<TransformableFrameInterface> frame) {
  MutexLock lock(&sender_lock_);

  if (!sender_) {
    return;
  }
  rtc::scoped_refptr<RTPSenderVideoFrameTransformerDelegate> delegate(this);
  transformation_queue_->PostTask(
      [delegate = std::move(delegate), frame = std::move(frame)]() mutable {
        RTC_DCHECK_RUN_ON(delegate->transformation_queue_.get());
        delegate->SendVideo(std::move(frame));
      });
}

void RTPSenderVideoFrameTransformerDelegate::SendVideo(
    std::unique_ptr<TransformableFrameInterface> transformed_frame) const {
  RTC_DCHECK_RUN_ON(transformation_queue_.get());
  RTC_CHECK_EQ(transformed_frame->GetDirection(),
               TransformableFrameInterface::Direction::kSender);
  MutexLock lock(&sender_lock_);
  if (!sender_)
    return;
  auto* transformed_video_frame =
      static_cast<TransformableVideoSenderFrame*>(transformed_frame.get());
  sender_->SendVideo(
      transformed_video_frame->GetPayloadType(),
      transformed_video_frame->GetCodecType(),
      transformed_video_frame->GetTimestamp(),
      transformed_video_frame->GetCaptureTimeMs(),
      transformed_video_frame->GetData(), transformed_video_frame->GetHeader(),
      transformed_video_frame->GetExpectedRetransmissionTimeMs());
}

void RTPSenderVideoFrameTransformerDelegate::SetVideoStructureUnderLock(
    const FrameDependencyStructure* video_structure) {
  MutexLock lock(&sender_lock_);
  RTC_CHECK(sender_);
  sender_->SetVideoStructureAfterTransformation(video_structure);
}

void RTPSenderVideoFrameTransformerDelegate::SetVideoLayersAllocationUnderLock(
    VideoLayersAllocation allocation) {
  MutexLock lock(&sender_lock_);
  RTC_CHECK(sender_);
  sender_->SetVideoLayersAllocationAfterTransformation(std::move(allocation));
}

void RTPSenderVideoFrameTransformerDelegate::Reset() {
  frame_transformer_->UnregisterTransformedFrameSinkCallback(ssrc_);
  frame_transformer_ = nullptr;
  {
    MutexLock lock(&sender_lock_);
    sender_ = nullptr;
  }
}

std::unique_ptr<TransformableVideoFrameInterface> CloneSenderVideoFrame(
    TransformableVideoFrameInterface* original) {
  auto encoded_image_buffer = EncodedImageBuffer::Create(
      original->GetData().data(), original->GetData().size());
  EncodedImage encoded_image;
  encoded_image.SetEncodedData(encoded_image_buffer);
  RTPVideoHeader new_header;
  absl::optional<VideoCodecType> new_codec_type;
  // TODO(bugs.webrtc.org/14708): Figure out a way to get the header information
  // without casting to TransformableVideoSenderFrame.
  if (original->GetDirection() ==
      TransformableFrameInterface::Direction::kSender) {
    // TODO(bugs.webrtc.org/14708): Figure out a way to bulletproof this cast.
    auto original_as_sender =
        static_cast<TransformableVideoSenderFrame*>(original);
    new_header = original_as_sender->GetHeader();
    new_codec_type = original_as_sender->GetCodecType();
  } else {
    // TODO(bugs.webrtc.org/14708): Make this codec dependent
    new_header.video_type_header.emplace<RTPVideoHeaderVP8>();
    new_codec_type = kVideoCodecVP8;
    // TODO(bugs.webrtc.org/14708): Fill in the new_header when it's not
    // `Direction::kSender`
  }
  // TODO(bugs.webrtc.org/14708): Fill in other EncodedImage parameters
  return std::make_unique<TransformableVideoSenderFrame>(
      encoded_image, new_header, original->GetPayloadType(), new_codec_type,
      original->GetTimestamp(),
      absl::nullopt,  // expected_retransmission_time_ms
      original->GetSsrc());
}

}  // namespace webrtc
