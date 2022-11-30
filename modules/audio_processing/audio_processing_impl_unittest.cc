/*
 *  Copyright (c) 2014 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "modules/audio_processing/audio_processing_impl.h"

#include <array>
#include <memory>
#include <tuple>

#include "absl/types/optional.h"
#include "api/make_ref_counted.h"
#include "api/scoped_refptr.h"
#include "modules/audio_processing/include/audio_processing.h"
#include "modules/audio_processing/optionally_built_submodule_creators.h"
#include "modules/audio_processing/test/audio_processing_builder_for_testing.h"
#include "modules/audio_processing/test/echo_canceller_test_tools.h"
#include "modules/audio_processing/test/echo_control_mock.h"
#include "modules/audio_processing/test/test_utils.h"
#include "rtc_base/checks.h"
#include "rtc_base/random.h"
#include "rtc_base/strings/string_builder.h"
#include "test/field_trial.h"
#include "test/gmock.h"
#include "test/gtest.h"

namespace webrtc {
namespace {

using ::testing::Invoke;
using ::testing::NotNull;

class MockInitialize : public AudioProcessingImpl {
 public:
  MockInitialize() : AudioProcessingImpl() {}

  MOCK_METHOD(void, InitializeLocked, (), (override));
  void RealInitializeLocked() {
    AssertLockedForTest();
    AudioProcessingImpl::InitializeLocked();
  }

  MOCK_METHOD(void, AddRef, (), (const, override));
  MOCK_METHOD(rtc::RefCountReleaseStatus, Release, (), (const, override));
};

// Creates MockEchoControl instances and provides a raw pointer access to
// the next created one. The raw pointer is meant to be used with gmock.
// Returning a pointer of the next created MockEchoControl instance is necessary
// for the following reasons: (i) gmock expectations must be set before any call
// occurs, (ii) APM is initialized the first time that
// AudioProcessingImpl::ProcessStream() is called and the initialization leads
// to the creation of a new EchoControl object.
class MockEchoControlFactory : public EchoControlFactory {
 public:
  MockEchoControlFactory() : next_mock_(std::make_unique<MockEchoControl>()) {}
  // Returns a pointer to the next MockEchoControl that this factory creates.
  MockEchoControl* GetNext() const { return next_mock_.get(); }
  std::unique_ptr<EchoControl> Create(int sample_rate_hz,
                                      int num_render_channels,
                                      int num_capture_channels) override {
    std::unique_ptr<EchoControl> mock = std::move(next_mock_);
    next_mock_ = std::make_unique<MockEchoControl>();
    return mock;
  }

 private:
  std::unique_ptr<MockEchoControl> next_mock_;
};

// Mocks EchoDetector and records the first samples of the last analyzed render
// stream frame. Used to check what data is read by an EchoDetector
// implementation injected into an APM.
class TestEchoDetector : public EchoDetector {
 public:
  TestEchoDetector()
      : analyze_render_audio_called_(false),
        last_render_audio_first_sample_(0.f) {}
  ~TestEchoDetector() override = default;
  void AnalyzeRenderAudio(rtc::ArrayView<const float> render_audio) override {
    last_render_audio_first_sample_ = render_audio[0];
    analyze_render_audio_called_ = true;
  }
  void AnalyzeCaptureAudio(rtc::ArrayView<const float> capture_audio) override {
  }
  void Initialize(int capture_sample_rate_hz,
                  int num_capture_channels,
                  int render_sample_rate_hz,
                  int num_render_channels) override {}
  EchoDetector::Metrics GetMetrics() const override { return {}; }
  // Returns true if AnalyzeRenderAudio() has been called at least once.
  bool analyze_render_audio_called() const {
    return analyze_render_audio_called_;
  }
  // Returns the first sample of the last analyzed render frame.
  float last_render_audio_first_sample() const {
    return last_render_audio_first_sample_;
  }

 private:
  bool analyze_render_audio_called_;
  float last_render_audio_first_sample_;
};

// Mocks CustomProcessing and applies ProcessSample() to all the samples.
// Meant to be injected into an APM to modify samples in a known and detectable
// way.
class TestRenderPreProcessor : public CustomProcessing {
 public:
  TestRenderPreProcessor() = default;
  ~TestRenderPreProcessor() = default;
  void Initialize(int sample_rate_hz, int num_channels) override {}
  void Process(AudioBuffer* audio) override {
    for (size_t k = 0; k < audio->num_channels(); ++k) {
      rtc::ArrayView<float> channel_view(audio->channels()[k],
                                         audio->num_frames());
      std::transform(channel_view.begin(), channel_view.end(),
                     channel_view.begin(), ProcessSample);
    }
  }
  std::string ToString() const override { return "TestRenderPreProcessor"; }
  void SetRuntimeSetting(AudioProcessing::RuntimeSetting setting) override {}
  // Modifies a sample. This member is used in Process() to modify a frame and
  // it is publicly visible to enable tests.
  static constexpr float ProcessSample(float x) { return 2.f * x; }
};

// Creates a simple `AudioProcessing` instance for APM input volume testing
// with AGC1 analog and/or AGC2 input volume controller enabled and AGC2
// digital controller enabled.
rtc::scoped_refptr<AudioProcessing> CreateApmForInputVolumeTest(
    bool agc1_analog_gain_controller_enabled,
    bool agc2_input_volume_controller_enabled) {
  webrtc::AudioProcessing::Config config;
  // Enable AGC1 analog controller.
  config.gain_controller1.enabled = agc1_analog_gain_controller_enabled;
  config.gain_controller1.analog_gain_controller.enabled =
      agc1_analog_gain_controller_enabled;
  // Enable AG2 input volume controller
  config.gain_controller2.input_volume_controller.enabled =
      agc2_input_volume_controller_enabled;
  // Enable AGC2 adaptive digital controller.
  config.gain_controller1.analog_gain_controller.enable_digital_adaptive =
      false;
  config.gain_controller2.enabled = true;
  config.gain_controller2.adaptive_digital.enabled = true;

  auto apm(AudioProcessingBuilder().Create());
  apm->ApplyConfig(config);

  return apm;
}

// Runs `apm` input processing for volume adjustments for `num_frames` random
// frames starting from the volume `initial_volume`. This includes three steps:
// 1) Set the input volume 2) Process the stream 3) Set the new recommended
// input volume. Returns the new recommended input volume.
int ProcessInputVolume(AudioProcessing& apm,
                       int num_frames,
                       int initial_volume) {
  constexpr int kSampleRateHz = 48000;
  constexpr int kNumChannels = 1;
  std::array<float, kSampleRateHz / 100> buffer;
  float* channel_pointers[] = {buffer.data()};
  StreamConfig stream_config(/*sample_rate_hz=*/kSampleRateHz,
                             /*num_channels=*/kNumChannels);
  int recommended_input_volume = initial_volume;
  for (int i = 0; i < num_frames; ++i) {
    Random random_generator(2341U);
    RandomizeSampleVector(&random_generator, buffer);

    apm.set_stream_analog_level(recommended_input_volume);
    apm.ProcessStream(channel_pointers, stream_config, stream_config,
                      channel_pointers);
    recommended_input_volume = apm.recommended_stream_analog_level();
  }
  return recommended_input_volume;
}

constexpr char kMinMicLevelFieldTrial[] =
    "WebRTC-Audio-2ndAgcMinMicLevelExperiment";
constexpr char kMinInputVolumeFieldTrial[] = "WebRTC-Audio-Agc2-MinInputVolume";
constexpr int kMinInputVolume = 12;

std::string GetMinMicLevelExperimentFieldTrial(absl::optional<int> value) {
  char field_trial_buffer[128];
  rtc::SimpleStringBuilder builder(field_trial_buffer);
  if (value.has_value()) {
    RTC_DCHECK_GE(*value, 0);
    RTC_DCHECK_LE(*value, 255);
    builder << kMinMicLevelFieldTrial << "/Enabled-" << *value << "/";
    builder << kMinInputVolumeFieldTrial << "/Enabled-" << *value << "/";
  } else {
    builder << kMinMicLevelFieldTrial << "/Disabled/";
    builder << kMinInputVolumeFieldTrial << "/Disabled/";
  }
  return builder.str();
}

// TODO(webrtc:7494): Remove the fieldtrial from the input volume tests when
// "WebRTC-Audio-2ndAgcMinMicLevelExperiment" and
// "WebRTC-Audio-Agc2-MinInputVolume" are removed.
class InputVolumeStartupParameterizedTest
    : public ::testing::TestWithParam<
          std::tuple<int, absl::optional<int>, bool, bool>> {
 protected:
  InputVolumeStartupParameterizedTest()
      : field_trials_(
            GetMinMicLevelExperimentFieldTrial(std::get<1>(GetParam()))) {}
  int GetStartupVolume() const { return std::get<0>(GetParam()); }
  int GetMinVolume() const {
    return std::get<1>(GetParam()).value_or(kMinInputVolume);
  }
  bool GetAgc1AnalogControllerEnabled() const {
    return std::get<2>(GetParam());
  }
  bool GetAgc2InputVolumeControllerEnabled() const {
    return std::get<3>(GetParam());
  }

 private:
  test::ScopedFieldTrials field_trials_;
};

class InputVolumeNotZeroParameterizedTest
    : public ::testing::TestWithParam<
          std::tuple<int, int, absl::optional<int>, bool, bool>> {
 protected:
  InputVolumeNotZeroParameterizedTest()
      : field_trials_(
            GetMinMicLevelExperimentFieldTrial(std::get<2>(GetParam()))) {}
  int GetStartupVolume() const { return std::get<0>(GetParam()); }
  int GetVolume() const { return std::get<1>(GetParam()); }
  int GetMinVolume() const {
    return std::get<2>(GetParam()).value_or(kMinInputVolume);
  }
  bool GetMinMicLevelExperimentEnabled() {
    return std::get<2>(GetParam()).has_value();
  }
  bool GetAgc1AnalogControllerEnabled() const {
    return std::get<3>(GetParam());
  }
  bool GetAgc2InputVolumeControllerEnabled() const {
    return std::get<4>(GetParam());
  }

 private:
  test::ScopedFieldTrials field_trials_;
};

class InputVolumeZeroParameterizedTest
    : public ::testing::TestWithParam<
          std::tuple<int, absl::optional<int>, bool, bool>> {
 protected:
  InputVolumeZeroParameterizedTest()
      : field_trials_(
            GetMinMicLevelExperimentFieldTrial(std::get<1>(GetParam()))) {}
  int GetStartupVolume() const { return std::get<0>(GetParam()); }
  int GetMinVolume() const {
    return std::get<1>(GetParam()).value_or(kMinInputVolume);
  }
  bool GetAgc1AnalogControllerEnabled() const {
    return std::get<2>(GetParam());
  }
  bool GetAgc2InputVolumeControllerEnabled() const {
    return std::get<3>(GetParam());
  }

 private:
  test::ScopedFieldTrials field_trials_;
};

}  // namespace

TEST(AudioProcessingImplTest, AudioParameterChangeTriggersInit) {
  MockInitialize mock;
  ON_CALL(mock, InitializeLocked)
      .WillByDefault(Invoke(&mock, &MockInitialize::RealInitializeLocked));

  EXPECT_CALL(mock, InitializeLocked).Times(1);
  mock.Initialize();

  constexpr size_t kMaxSampleRateHz = 32000;
  constexpr size_t kMaxNumChannels = 2;
  std::array<int16_t, kMaxNumChannels * kMaxSampleRateHz / 100> frame;
  frame.fill(0);
  StreamConfig config(16000, 1);
  // Call with the default parameters; there should be an init.
  EXPECT_CALL(mock, InitializeLocked).Times(0);
  EXPECT_NOERR(mock.ProcessStream(frame.data(), config, config, frame.data()));
  EXPECT_NOERR(
      mock.ProcessReverseStream(frame.data(), config, config, frame.data()));

  // New sample rate. (Only impacts ProcessStream).
  config = StreamConfig(32000, 1);
  EXPECT_CALL(mock, InitializeLocked).Times(1);
  EXPECT_NOERR(mock.ProcessStream(frame.data(), config, config, frame.data()));

  // New number of channels.
  config = StreamConfig(32000, 2);
  EXPECT_CALL(mock, InitializeLocked).Times(2);
  EXPECT_NOERR(mock.ProcessStream(frame.data(), config, config, frame.data()));
  EXPECT_NOERR(
      mock.ProcessReverseStream(frame.data(), config, config, frame.data()));

  // A new sample rate passed to ProcessReverseStream should cause an init.
  config = StreamConfig(16000, 2);
  EXPECT_CALL(mock, InitializeLocked).Times(1);
  EXPECT_NOERR(
      mock.ProcessReverseStream(frame.data(), config, config, frame.data()));
}

TEST(AudioProcessingImplTest, UpdateCapturePreGainRuntimeSetting) {
  rtc::scoped_refptr<AudioProcessing> apm =
      AudioProcessingBuilderForTesting().Create();
  webrtc::AudioProcessing::Config apm_config;
  apm_config.pre_amplifier.enabled = true;
  apm_config.pre_amplifier.fixed_gain_factor = 1.f;
  apm->ApplyConfig(apm_config);

  constexpr int kSampleRateHz = 48000;
  constexpr int16_t kAudioLevel = 10000;
  constexpr size_t kNumChannels = 2;

  std::array<int16_t, kNumChannels * kSampleRateHz / 100> frame;
  StreamConfig config(kSampleRateHz, kNumChannels);
  frame.fill(kAudioLevel);
  apm->ProcessStream(frame.data(), config, config, frame.data());
  EXPECT_EQ(frame[100], kAudioLevel)
      << "With factor 1, frame shouldn't be modified.";

  constexpr float kGainFactor = 2.f;
  apm->SetRuntimeSetting(
      AudioProcessing::RuntimeSetting::CreateCapturePreGain(kGainFactor));

  // Process for two frames to have time to ramp up gain.
  for (int i = 0; i < 2; ++i) {
    frame.fill(kAudioLevel);
    apm->ProcessStream(frame.data(), config, config, frame.data());
  }
  EXPECT_EQ(frame[100], kGainFactor * kAudioLevel)
      << "Frame should be amplified.";
}

TEST(AudioProcessingImplTest,
     LevelAdjustmentUpdateCapturePreGainRuntimeSetting) {
  rtc::scoped_refptr<AudioProcessing> apm =
      AudioProcessingBuilderForTesting().Create();
  webrtc::AudioProcessing::Config apm_config;
  apm_config.capture_level_adjustment.enabled = true;
  apm_config.capture_level_adjustment.pre_gain_factor = 1.f;
  apm->ApplyConfig(apm_config);

  constexpr int kSampleRateHz = 48000;
  constexpr int16_t kAudioLevel = 10000;
  constexpr size_t kNumChannels = 2;

  std::array<int16_t, kNumChannels * kSampleRateHz / 100> frame;
  StreamConfig config(kSampleRateHz, kNumChannels);
  frame.fill(kAudioLevel);
  apm->ProcessStream(frame.data(), config, config, frame.data());
  EXPECT_EQ(frame[100], kAudioLevel)
      << "With factor 1, frame shouldn't be modified.";

  constexpr float kGainFactor = 2.f;
  apm->SetRuntimeSetting(
      AudioProcessing::RuntimeSetting::CreateCapturePreGain(kGainFactor));

  // Process for two frames to have time to ramp up gain.
  for (int i = 0; i < 2; ++i) {
    frame.fill(kAudioLevel);
    apm->ProcessStream(frame.data(), config, config, frame.data());
  }
  EXPECT_EQ(frame[100], kGainFactor * kAudioLevel)
      << "Frame should be amplified.";
}

TEST(AudioProcessingImplTest,
     LevelAdjustmentUpdateCapturePostGainRuntimeSetting) {
  rtc::scoped_refptr<AudioProcessing> apm =
      AudioProcessingBuilderForTesting().Create();
  webrtc::AudioProcessing::Config apm_config;
  apm_config.capture_level_adjustment.enabled = true;
  apm_config.capture_level_adjustment.post_gain_factor = 1.f;
  apm->ApplyConfig(apm_config);

  constexpr int kSampleRateHz = 48000;
  constexpr int16_t kAudioLevel = 10000;
  constexpr size_t kNumChannels = 2;

  std::array<int16_t, kNumChannels * kSampleRateHz / 100> frame;
  StreamConfig config(kSampleRateHz, kNumChannels);
  frame.fill(kAudioLevel);
  apm->ProcessStream(frame.data(), config, config, frame.data());
  EXPECT_EQ(frame[100], kAudioLevel)
      << "With factor 1, frame shouldn't be modified.";

  constexpr float kGainFactor = 2.f;
  apm->SetRuntimeSetting(
      AudioProcessing::RuntimeSetting::CreateCapturePostGain(kGainFactor));

  // Process for two frames to have time to ramp up gain.
  for (int i = 0; i < 2; ++i) {
    frame.fill(kAudioLevel);
    apm->ProcessStream(frame.data(), config, config, frame.data());
  }
  EXPECT_EQ(frame[100], kGainFactor * kAudioLevel)
      << "Frame should be amplified.";
}

TEST(AudioProcessingImplTest, EchoControllerObservesSetCaptureUsageChange) {
  // Tests that the echo controller observes that the capture usage has been
  // updated.
  auto echo_control_factory = std::make_unique<MockEchoControlFactory>();
  const MockEchoControlFactory* echo_control_factory_ptr =
      echo_control_factory.get();

  rtc::scoped_refptr<AudioProcessing> apm =
      AudioProcessingBuilderForTesting()
          .SetEchoControlFactory(std::move(echo_control_factory))
          .Create();

  constexpr int16_t kAudioLevel = 10000;
  constexpr int kSampleRateHz = 48000;
  constexpr int kNumChannels = 2;
  std::array<int16_t, kNumChannels * kSampleRateHz / 100> frame;
  StreamConfig config(kSampleRateHz, kNumChannels);
  frame.fill(kAudioLevel);

  MockEchoControl* echo_control_mock = echo_control_factory_ptr->GetNext();

  // Ensure that SetCaptureOutputUsage is not called when no runtime settings
  // are passed.
  EXPECT_CALL(*echo_control_mock, SetCaptureOutputUsage(testing::_)).Times(0);
  apm->ProcessStream(frame.data(), config, config, frame.data());

  // Ensure that SetCaptureOutputUsage is called with the right information when
  // a runtime setting is passed.
  EXPECT_CALL(*echo_control_mock,
              SetCaptureOutputUsage(/*capture_output_used=*/false))
      .Times(1);
  EXPECT_TRUE(apm->PostRuntimeSetting(
      AudioProcessing::RuntimeSetting::CreateCaptureOutputUsedSetting(
          /*capture_output_used=*/false)));
  apm->ProcessStream(frame.data(), config, config, frame.data());

  EXPECT_CALL(*echo_control_mock,
              SetCaptureOutputUsage(/*capture_output_used=*/true))
      .Times(1);
  EXPECT_TRUE(apm->PostRuntimeSetting(
      AudioProcessing::RuntimeSetting::CreateCaptureOutputUsedSetting(
          /*capture_output_used=*/true)));
  apm->ProcessStream(frame.data(), config, config, frame.data());

  // The number of positions to place items in the queue is equal to the queue
  // size minus 1.
  constexpr int kNumSlotsInQueue = RuntimeSettingQueueSize();

  // Ensure that SetCaptureOutputUsage is called with the right information when
  // many runtime settings are passed.
  for (int k = 0; k < kNumSlotsInQueue - 1; ++k) {
    EXPECT_TRUE(apm->PostRuntimeSetting(
        AudioProcessing::RuntimeSetting::CreateCaptureOutputUsedSetting(
            /*capture_output_used=*/false)));
  }
  EXPECT_CALL(*echo_control_mock,
              SetCaptureOutputUsage(/*capture_output_used=*/false))
      .Times(kNumSlotsInQueue - 1);
  apm->ProcessStream(frame.data(), config, config, frame.data());

  // Ensure that SetCaptureOutputUsage is properly called with the fallback
  // value when the runtime settings queue becomes full.
  for (int k = 0; k < kNumSlotsInQueue; ++k) {
    EXPECT_TRUE(apm->PostRuntimeSetting(
        AudioProcessing::RuntimeSetting::CreateCaptureOutputUsedSetting(
            /*capture_output_used=*/false)));
  }
  EXPECT_FALSE(apm->PostRuntimeSetting(
      AudioProcessing::RuntimeSetting::CreateCaptureOutputUsedSetting(
          /*capture_output_used=*/false)));
  EXPECT_FALSE(apm->PostRuntimeSetting(
      AudioProcessing::RuntimeSetting::CreateCaptureOutputUsedSetting(
          /*capture_output_used=*/false)));
  EXPECT_CALL(*echo_control_mock,
              SetCaptureOutputUsage(/*capture_output_used=*/false))
      .Times(kNumSlotsInQueue);
  EXPECT_CALL(*echo_control_mock,
              SetCaptureOutputUsage(/*capture_output_used=*/true))
      .Times(1);
  apm->ProcessStream(frame.data(), config, config, frame.data());
}

TEST(AudioProcessingImplTest,
     EchoControllerObservesPreAmplifierEchoPathGainChange) {
  // Tests that the echo controller observes an echo path gain change when the
  // pre-amplifier submodule changes the gain.
  auto echo_control_factory = std::make_unique<MockEchoControlFactory>();
  const auto* echo_control_factory_ptr = echo_control_factory.get();

  rtc::scoped_refptr<AudioProcessing> apm =
      AudioProcessingBuilderForTesting()
          .SetEchoControlFactory(std::move(echo_control_factory))
          .Create();
  // Disable AGC.
  webrtc::AudioProcessing::Config apm_config;
  apm_config.gain_controller1.enabled = false;
  apm_config.gain_controller2.enabled = false;
  apm_config.pre_amplifier.enabled = true;
  apm_config.pre_amplifier.fixed_gain_factor = 1.f;
  apm->ApplyConfig(apm_config);

  constexpr int16_t kAudioLevel = 10000;
  constexpr size_t kSampleRateHz = 48000;
  constexpr size_t kNumChannels = 2;
  std::array<int16_t, kNumChannels * kSampleRateHz / 100> frame;
  StreamConfig config(kSampleRateHz, kNumChannels);
  frame.fill(kAudioLevel);

  MockEchoControl* echo_control_mock = echo_control_factory_ptr->GetNext();

  EXPECT_CALL(*echo_control_mock, AnalyzeCapture(testing::_)).Times(1);
  EXPECT_CALL(*echo_control_mock,
              ProcessCapture(NotNull(), testing::_, /*echo_path_change=*/false))
      .Times(1);
  apm->ProcessStream(frame.data(), config, config, frame.data());

  EXPECT_CALL(*echo_control_mock, AnalyzeCapture(testing::_)).Times(1);
  EXPECT_CALL(*echo_control_mock,
              ProcessCapture(NotNull(), testing::_, /*echo_path_change=*/true))
      .Times(1);
  apm->SetRuntimeSetting(
      AudioProcessing::RuntimeSetting::CreateCapturePreGain(2.f));
  apm->ProcessStream(frame.data(), config, config, frame.data());
}

TEST(AudioProcessingImplTest,
     EchoControllerObservesLevelAdjustmentPreGainEchoPathGainChange) {
  // Tests that the echo controller observes an echo path gain change when the
  // pre-amplifier submodule changes the gain.
  auto echo_control_factory = std::make_unique<MockEchoControlFactory>();
  const auto* echo_control_factory_ptr = echo_control_factory.get();

  rtc::scoped_refptr<AudioProcessing> apm =
      AudioProcessingBuilderForTesting()
          .SetEchoControlFactory(std::move(echo_control_factory))
          .Create();
  // Disable AGC.
  webrtc::AudioProcessing::Config apm_config;
  apm_config.gain_controller1.enabled = false;
  apm_config.gain_controller2.enabled = false;
  apm_config.capture_level_adjustment.enabled = true;
  apm_config.capture_level_adjustment.pre_gain_factor = 1.f;
  apm->ApplyConfig(apm_config);

  constexpr int16_t kAudioLevel = 10000;
  constexpr size_t kSampleRateHz = 48000;
  constexpr size_t kNumChannels = 2;
  std::array<int16_t, kNumChannels * kSampleRateHz / 100> frame;
  StreamConfig config(kSampleRateHz, kNumChannels);
  frame.fill(kAudioLevel);

  MockEchoControl* echo_control_mock = echo_control_factory_ptr->GetNext();

  EXPECT_CALL(*echo_control_mock, AnalyzeCapture(testing::_)).Times(1);
  EXPECT_CALL(*echo_control_mock,
              ProcessCapture(NotNull(), testing::_, /*echo_path_change=*/false))
      .Times(1);
  apm->ProcessStream(frame.data(), config, config, frame.data());

  EXPECT_CALL(*echo_control_mock, AnalyzeCapture(testing::_)).Times(1);
  EXPECT_CALL(*echo_control_mock,
              ProcessCapture(NotNull(), testing::_, /*echo_path_change=*/true))
      .Times(1);
  apm->SetRuntimeSetting(
      AudioProcessing::RuntimeSetting::CreateCapturePreGain(2.f));
  apm->ProcessStream(frame.data(), config, config, frame.data());
}

TEST(AudioProcessingImplTest,
     EchoControllerObservesAnalogAgc1EchoPathGainChange) {
  // Tests that the echo controller observes an echo path gain change when the
  // AGC1 analog adaptive submodule changes the analog gain.
  auto echo_control_factory = std::make_unique<MockEchoControlFactory>();
  const auto* echo_control_factory_ptr = echo_control_factory.get();

  rtc::scoped_refptr<AudioProcessing> apm =
      AudioProcessingBuilderForTesting()
          .SetEchoControlFactory(std::move(echo_control_factory))
          .Create();
  webrtc::AudioProcessing::Config apm_config;
  // Enable AGC1.
  apm_config.gain_controller1.enabled = true;
  apm_config.gain_controller1.analog_gain_controller.enabled = true;
  apm_config.gain_controller2.enabled = false;
  apm_config.pre_amplifier.enabled = false;
  apm->ApplyConfig(apm_config);

  constexpr int16_t kAudioLevel = 1000;
  constexpr size_t kSampleRateHz = 48000;
  constexpr size_t kNumChannels = 2;
  std::array<int16_t, kNumChannels * kSampleRateHz / 100> frame;
  StreamConfig stream_config(kSampleRateHz, kNumChannels);
  frame.fill(kAudioLevel);

  MockEchoControl* echo_control_mock = echo_control_factory_ptr->GetNext();

  constexpr int kInitialStreamAnalogLevel = 123;
  apm->set_stream_analog_level(kInitialStreamAnalogLevel);

  // When the first fame is processed, no echo path gain change must be
  // detected.
  EXPECT_CALL(*echo_control_mock, AnalyzeCapture(testing::_)).Times(1);
  EXPECT_CALL(*echo_control_mock,
              ProcessCapture(NotNull(), testing::_, /*echo_path_change=*/false))
      .Times(1);
  apm->ProcessStream(frame.data(), stream_config, stream_config, frame.data());

  // Simulate the application of the recommended analog level.
  int recommended_analog_level = apm->recommended_stream_analog_level();
  if (recommended_analog_level == kInitialStreamAnalogLevel) {
    // Force an analog gain change if it did not happen.
    recommended_analog_level++;
  }
  apm->set_stream_analog_level(recommended_analog_level);

  // After the first fame and with a stream analog level change, the echo path
  // gain change must be detected.
  EXPECT_CALL(*echo_control_mock, AnalyzeCapture(testing::_)).Times(1);
  EXPECT_CALL(*echo_control_mock,
              ProcessCapture(NotNull(), testing::_, /*echo_path_change=*/true))
      .Times(1);
  apm->ProcessStream(frame.data(), stream_config, stream_config, frame.data());
}

// Tests that a stream is successfully processed when AGC2 adaptive digital is
// used and when the field trial
// `WebRTC-Audio-TransientSuppressorVadMode/Enabled-Default/` is set.
TEST(AudioProcessingImplTest,
     ProcessWithAgc2AndTransientSuppressorVadModeDefault) {
  webrtc::test::ScopedFieldTrials field_trials(
      "WebRTC-Audio-TransientSuppressorVadMode/Enabled-Default/");
  rtc::scoped_refptr<AudioProcessing> apm = AudioProcessingBuilder().Create();
  ASSERT_EQ(apm->Initialize(), AudioProcessing::kNoError);
  webrtc::AudioProcessing::Config apm_config;
  // Disable AGC1 analog.
  apm_config.gain_controller1.enabled = false;
  // Enable AGC2 digital.
  apm_config.gain_controller2.enabled = true;
  apm_config.gain_controller2.adaptive_digital.enabled = true;
  apm->ApplyConfig(apm_config);
  constexpr int kSampleRateHz = 48000;
  constexpr int kNumChannels = 1;
  std::array<float, kSampleRateHz / 100> buffer;
  float* channel_pointers[] = {buffer.data()};
  StreamConfig stream_config(/*sample_rate_hz=*/kSampleRateHz,
                             /*num_channels=*/kNumChannels);
  Random random_generator(2341U);
  constexpr int kFramesToProcess = 10;
  for (int i = 0; i < kFramesToProcess; ++i) {
    RandomizeSampleVector(&random_generator, buffer);
    ASSERT_EQ(apm->ProcessStream(channel_pointers, stream_config, stream_config,
                                 channel_pointers),
              kNoErr);
  }
}

// Tests that a stream is successfully processed when AGC2 adaptive digital is
// used and when the field trial
// `WebRTC-Audio-TransientSuppressorVadMode/Enabled-RnnVad/` is set.
TEST(AudioProcessingImplTest,
     ProcessWithAgc2AndTransientSuppressorVadModeRnnVad) {
  webrtc::test::ScopedFieldTrials field_trials(
      "WebRTC-Audio-TransientSuppressorVadMode/Enabled-RnnVad/");
  rtc::scoped_refptr<AudioProcessing> apm = AudioProcessingBuilder().Create();
  ASSERT_EQ(apm->Initialize(), AudioProcessing::kNoError);
  webrtc::AudioProcessing::Config apm_config;
  // Disable AGC1 analog.
  apm_config.gain_controller1.enabled = false;
  // Enable AGC2 digital.
  apm_config.gain_controller2.enabled = true;
  apm_config.gain_controller2.adaptive_digital.enabled = true;
  apm->ApplyConfig(apm_config);
  constexpr int kSampleRateHz = 48000;
  constexpr int kNumChannels = 1;
  std::array<float, kSampleRateHz / 100> buffer;
  float* channel_pointers[] = {buffer.data()};
  StreamConfig stream_config(/*sample_rate_hz=*/kSampleRateHz,
                             /*num_channels=*/kNumChannels);
  Random random_generator(2341U);
  constexpr int kFramesToProcess = 10;
  for (int i = 0; i < kFramesToProcess; ++i) {
    RandomizeSampleVector(&random_generator, buffer);
    ASSERT_EQ(apm->ProcessStream(channel_pointers, stream_config, stream_config,
                                 channel_pointers),
              kNoErr);
  }
}

TEST(AudioProcessingImplTest, EchoControllerObservesPlayoutVolumeChange) {
  // Tests that the echo controller observes an echo path gain change when a
  // playout volume change is reported.
  auto echo_control_factory = std::make_unique<MockEchoControlFactory>();
  const auto* echo_control_factory_ptr = echo_control_factory.get();

  rtc::scoped_refptr<AudioProcessing> apm =
      AudioProcessingBuilderForTesting()
          .SetEchoControlFactory(std::move(echo_control_factory))
          .Create();
  // Disable AGC.
  webrtc::AudioProcessing::Config apm_config;
  apm_config.gain_controller1.enabled = false;
  apm_config.gain_controller2.enabled = false;
  apm->ApplyConfig(apm_config);

  constexpr int16_t kAudioLevel = 10000;
  constexpr size_t kSampleRateHz = 48000;
  constexpr size_t kNumChannels = 2;
  std::array<int16_t, kNumChannels * kSampleRateHz / 100> frame;
  StreamConfig stream_config(kSampleRateHz, kNumChannels);
  frame.fill(kAudioLevel);

  MockEchoControl* echo_control_mock = echo_control_factory_ptr->GetNext();

  EXPECT_CALL(*echo_control_mock, AnalyzeCapture(testing::_)).Times(1);
  EXPECT_CALL(*echo_control_mock,
              ProcessCapture(NotNull(), testing::_, /*echo_path_change=*/false))
      .Times(1);
  apm->ProcessStream(frame.data(), stream_config, stream_config, frame.data());

  EXPECT_CALL(*echo_control_mock, AnalyzeCapture(testing::_)).Times(1);
  EXPECT_CALL(*echo_control_mock,
              ProcessCapture(NotNull(), testing::_, /*echo_path_change=*/false))
      .Times(1);
  apm->SetRuntimeSetting(
      AudioProcessing::RuntimeSetting::CreatePlayoutVolumeChange(50));
  apm->ProcessStream(frame.data(), stream_config, stream_config, frame.data());

  EXPECT_CALL(*echo_control_mock, AnalyzeCapture(testing::_)).Times(1);
  EXPECT_CALL(*echo_control_mock,
              ProcessCapture(NotNull(), testing::_, /*echo_path_change=*/false))
      .Times(1);
  apm->SetRuntimeSetting(
      AudioProcessing::RuntimeSetting::CreatePlayoutVolumeChange(50));
  apm->ProcessStream(frame.data(), stream_config, stream_config, frame.data());

  EXPECT_CALL(*echo_control_mock, AnalyzeCapture(testing::_)).Times(1);
  EXPECT_CALL(*echo_control_mock,
              ProcessCapture(NotNull(), testing::_, /*echo_path_change=*/true))
      .Times(1);
  apm->SetRuntimeSetting(
      AudioProcessing::RuntimeSetting::CreatePlayoutVolumeChange(100));
  apm->ProcessStream(frame.data(), stream_config, stream_config, frame.data());
}

TEST(AudioProcessingImplTest, RenderPreProcessorBeforeEchoDetector) {
  // Make sure that signal changes caused by a render pre-processing sub-module
  // take place before any echo detector analysis.
  auto test_echo_detector = rtc::make_ref_counted<TestEchoDetector>();
  std::unique_ptr<CustomProcessing> test_render_pre_processor(
      new TestRenderPreProcessor());
  // Create APM injecting the test echo detector and render pre-processor.
  rtc::scoped_refptr<AudioProcessing> apm =
      AudioProcessingBuilderForTesting()
          .SetEchoDetector(test_echo_detector)
          .SetRenderPreProcessing(std::move(test_render_pre_processor))
          .Create();
  webrtc::AudioProcessing::Config apm_config;
  apm_config.pre_amplifier.enabled = true;
  apm->ApplyConfig(apm_config);

  constexpr int16_t kAudioLevel = 1000;
  constexpr int kSampleRateHz = 16000;
  constexpr size_t kNumChannels = 1;
  // Explicitly initialize APM to ensure no render frames are discarded.
  const ProcessingConfig processing_config = {{
      {kSampleRateHz, kNumChannels},
      {kSampleRateHz, kNumChannels},
      {kSampleRateHz, kNumChannels},
      {kSampleRateHz, kNumChannels},
  }};
  apm->Initialize(processing_config);

  std::array<int16_t, kNumChannels * kSampleRateHz / 100> frame;
  StreamConfig stream_config(kSampleRateHz, kNumChannels);

  constexpr float kAudioLevelFloat = static_cast<float>(kAudioLevel);
  constexpr float kExpectedPreprocessedAudioLevel =
      TestRenderPreProcessor::ProcessSample(kAudioLevelFloat);
  ASSERT_NE(kAudioLevelFloat, kExpectedPreprocessedAudioLevel);

  // Analyze a render stream frame.
  frame.fill(kAudioLevel);
  ASSERT_EQ(AudioProcessing::Error::kNoError,
            apm->ProcessReverseStream(frame.data(), stream_config,
                                      stream_config, frame.data()));
  // Trigger a call to in EchoDetector::AnalyzeRenderAudio() via
  // ProcessStream().
  frame.fill(kAudioLevel);
  ASSERT_EQ(AudioProcessing::Error::kNoError,
            apm->ProcessStream(frame.data(), stream_config, stream_config,
                               frame.data()));
  // Regardless of how the call to in EchoDetector::AnalyzeRenderAudio() is
  // triggered, the line below checks that the call has occurred. If not, the
  // APM implementation may have changed and this test might need to be adapted.
  ASSERT_TRUE(test_echo_detector->analyze_render_audio_called());
  // Check that the data read in EchoDetector::AnalyzeRenderAudio() is that
  // produced by the render pre-processor.
  EXPECT_EQ(kExpectedPreprocessedAudioLevel,
            test_echo_detector->last_render_audio_first_sample());
}

// Disabling build-optional submodules and trying to enable them via the APM
// config should be bit-exact with running APM with said submodules disabled.
// This mainly tests that SetCreateOptionalSubmodulesForTesting has an effect.
TEST(ApmWithSubmodulesExcludedTest, BitexactWithDisabledModules) {
  auto apm = rtc::make_ref_counted<AudioProcessingImpl>();
  ASSERT_EQ(apm->Initialize(), AudioProcessing::kNoError);

  ApmSubmoduleCreationOverrides overrides;
  overrides.transient_suppression = true;
  apm->OverrideSubmoduleCreationForTesting(overrides);

  AudioProcessing::Config apm_config = apm->GetConfig();
  apm_config.transient_suppression.enabled = true;
  apm->ApplyConfig(apm_config);

  rtc::scoped_refptr<AudioProcessing> apm_reference =
      AudioProcessingBuilder().Create();
  apm_config = apm_reference->GetConfig();
  apm_config.transient_suppression.enabled = false;
  apm_reference->ApplyConfig(apm_config);

  constexpr int kSampleRateHz = 16000;
  constexpr int kNumChannels = 1;
  std::array<float, kSampleRateHz / 100> buffer;
  std::array<float, kSampleRateHz / 100> buffer_reference;
  float* channel_pointers[] = {buffer.data()};
  float* channel_pointers_reference[] = {buffer_reference.data()};
  StreamConfig stream_config(/*sample_rate_hz=*/kSampleRateHz,
                             /*num_channels=*/kNumChannels);
  Random random_generator(2341U);
  constexpr int kFramesToProcessPerConfiguration = 10;

  for (int i = 0; i < kFramesToProcessPerConfiguration; ++i) {
    RandomizeSampleVector(&random_generator, buffer);
    std::copy(buffer.begin(), buffer.end(), buffer_reference.begin());
    ASSERT_EQ(apm->ProcessStream(channel_pointers, stream_config, stream_config,
                                 channel_pointers),
              kNoErr);
    ASSERT_EQ(
        apm_reference->ProcessStream(channel_pointers_reference, stream_config,
                                     stream_config, channel_pointers_reference),
        kNoErr);
    for (int j = 0; j < kSampleRateHz / 100; ++j) {
      EXPECT_EQ(buffer[j], buffer_reference[j]);
    }
  }
}

// Disable transient suppressor creation and run APM in ways that should trigger
// calls to the transient suppressor API.
TEST(ApmWithSubmodulesExcludedTest, ReinitializeTransientSuppressor) {
  auto apm = rtc::make_ref_counted<AudioProcessingImpl>();
  ASSERT_EQ(apm->Initialize(), kNoErr);

  ApmSubmoduleCreationOverrides overrides;
  overrides.transient_suppression = true;
  apm->OverrideSubmoduleCreationForTesting(overrides);

  AudioProcessing::Config config = apm->GetConfig();
  config.transient_suppression.enabled = true;
  apm->ApplyConfig(config);
  // 960 samples per frame: 10 ms of <= 48 kHz audio with <= 2 channels.
  float buffer[960];
  float* channel_pointers[] = {&buffer[0], &buffer[480]};
  Random random_generator(2341U);
  constexpr int kFramesToProcessPerConfiguration = 3;

  StreamConfig initial_stream_config(/*sample_rate_hz=*/16000,
                                     /*num_channels=*/1);
  for (int i = 0; i < kFramesToProcessPerConfiguration; ++i) {
    RandomizeSampleVector(&random_generator, buffer);
    EXPECT_EQ(apm->ProcessStream(channel_pointers, initial_stream_config,
                                 initial_stream_config, channel_pointers),
              kNoErr);
  }

  StreamConfig stereo_stream_config(/*sample_rate_hz=*/16000,
                                    /*num_channels=*/2);
  for (int i = 0; i < kFramesToProcessPerConfiguration; ++i) {
    RandomizeSampleVector(&random_generator, buffer);
    EXPECT_EQ(apm->ProcessStream(channel_pointers, stereo_stream_config,
                                 stereo_stream_config, channel_pointers),
              kNoErr);
  }

  StreamConfig high_sample_rate_stream_config(/*sample_rate_hz=*/48000,
                                              /*num_channels=*/2);
  for (int i = 0; i < kFramesToProcessPerConfiguration; ++i) {
    RandomizeSampleVector(&random_generator, buffer);
    EXPECT_EQ(
        apm->ProcessStream(channel_pointers, high_sample_rate_stream_config,
                           high_sample_rate_stream_config, channel_pointers),
        kNoErr);
  }
}

// Disable transient suppressor creation and run APM in ways that should trigger
// calls to the transient suppressor API.
TEST(ApmWithSubmodulesExcludedTest, ToggleTransientSuppressor) {
  auto apm = rtc::make_ref_counted<AudioProcessingImpl>();
  ASSERT_EQ(apm->Initialize(), AudioProcessing::kNoError);

  ApmSubmoduleCreationOverrides overrides;
  overrides.transient_suppression = true;
  apm->OverrideSubmoduleCreationForTesting(overrides);

  //  960 samples per frame: 10 ms of <= 48 kHz audio with <= 2 channels.
  float buffer[960];
  float* channel_pointers[] = {&buffer[0], &buffer[480]};
  Random random_generator(2341U);
  constexpr int kFramesToProcessPerConfiguration = 3;
  StreamConfig stream_config(/*sample_rate_hz=*/16000,
                             /*num_channels=*/1);

  AudioProcessing::Config config = apm->GetConfig();
  config.transient_suppression.enabled = true;
  apm->ApplyConfig(config);
  for (int i = 0; i < kFramesToProcessPerConfiguration; ++i) {
    RandomizeSampleVector(&random_generator, buffer);
    EXPECT_EQ(apm->ProcessStream(channel_pointers, stream_config, stream_config,
                                 channel_pointers),
              kNoErr);
  }

  config = apm->GetConfig();
  config.transient_suppression.enabled = false;
  apm->ApplyConfig(config);
  for (int i = 0; i < kFramesToProcessPerConfiguration; ++i) {
    RandomizeSampleVector(&random_generator, buffer);
    EXPECT_EQ(apm->ProcessStream(channel_pointers, stream_config, stream_config,
                                 channel_pointers),
              kNoErr);
  }

  config = apm->GetConfig();
  config.transient_suppression.enabled = true;
  apm->ApplyConfig(config);
  for (int i = 0; i < kFramesToProcessPerConfiguration; ++i) {
    RandomizeSampleVector(&random_generator, buffer);
    EXPECT_EQ(apm->ProcessStream(channel_pointers, stream_config, stream_config,
                                 channel_pointers),
              kNoErr);
  }
}

// Tests that the minimum startup volume is applied at the startup.
TEST_P(InputVolumeStartupParameterizedTest,
       VerifyStartupMinVolumeAppliedAtStartup) {
  const int applied_startup_input_volume = GetStartupVolume();
  const int expected_volume =
      std::max(applied_startup_input_volume, GetMinVolume());
  const bool agc1_analog_controller_enabled = GetAgc1AnalogControllerEnabled();
  const bool agc2_input_volume_controller_enabled =
      GetAgc2InputVolumeControllerEnabled();
  auto apm = CreateApmForInputVolumeTest(agc1_analog_controller_enabled,
                                         agc2_input_volume_controller_enabled);

  const int recommended_input_volume =
      ProcessInputVolume(*apm, /*num_frames=*/1, applied_startup_input_volume);

  if (!agc1_analog_controller_enabled &&
      !agc2_input_volume_controller_enabled) {
    // No input volume changes if none of the analog controllers is enabled.
    ASSERT_EQ(recommended_input_volume, applied_startup_input_volume);
  } else {
    ASSERT_EQ(recommended_input_volume, expected_volume);
  }
}

// Tests that the minimum input volume is applied if the volume is manually
// adjusted to a non-zero value 1) always for AGC2 input volume controller and
// 2) only if "WebRTC-Audio-2ndAgcMinMicLevelExperiment" is enabled for AGC1
// analog controller.
TEST_P(InputVolumeNotZeroParameterizedTest,
       VerifyMinVolumeMaybeAppliedAfterManualVolumeAdjustments) {
  const int applied_startup_input_volume = GetStartupVolume();
  const int applied_input_volume = GetVolume();
  const int expected_volume = std::max(applied_input_volume, GetMinVolume());
  const bool agc1_analog_controller_enabled = GetAgc1AnalogControllerEnabled();
  const bool agc2_input_volume_controller_enabled =
      GetAgc2InputVolumeControllerEnabled();
  auto apm = CreateApmForInputVolumeTest(agc1_analog_controller_enabled,
                                         agc2_input_volume_controller_enabled);

  ProcessInputVolume(*apm, /*num_frames=*/1, applied_startup_input_volume);
  const int recommended_input_volume =
      ProcessInputVolume(*apm, /*num_frames=*/1, applied_input_volume);

  ASSERT_NE(applied_input_volume, 0);

  if (!agc1_analog_controller_enabled &&
      !agc2_input_volume_controller_enabled) {
    // No input volume changes if none of the analog controllers is enabled.
    ASSERT_EQ(recommended_input_volume, applied_input_volume);
  } else {
    if (GetMinMicLevelExperimentEnabled() ||
        (!agc1_analog_controller_enabled &&
         agc2_input_volume_controller_enabled)) {
      ASSERT_EQ(recommended_input_volume, expected_volume);
    } else {
      ASSERT_EQ(recommended_input_volume, applied_input_volume);
    }
  }
}

// Tests that the minimum input volume is not applied if the volume is manually
// adjusted to zero.
TEST_P(InputVolumeZeroParameterizedTest,
       VerifyMinVolumeNotAppliedAfterManualVolumeAdjustments) {
  constexpr int kZeroVolume = 0;
  const int applied_startup_input_volume = GetStartupVolume();
  const bool agc1_analog_controller_enabled = GetAgc1AnalogControllerEnabled();
  const bool agc2_input_volume_controller_enabled =
      GetAgc2InputVolumeControllerEnabled();
  auto apm = CreateApmForInputVolumeTest(agc1_analog_controller_enabled,
                                         agc2_input_volume_controller_enabled);

  const int recommended_input_volume_after_startup =
      ProcessInputVolume(*apm, /*num_frames=*/1, applied_startup_input_volume);
  const int recommended_input_volume =
      ProcessInputVolume(*apm, /*num_frames=*/1, kZeroVolume);

  if (!agc1_analog_controller_enabled &&
      !agc2_input_volume_controller_enabled) {
    // No input volume changes if none of the analog controllers is enabled.
    ASSERT_EQ(recommended_input_volume, kZeroVolume);
  } else {
    ASSERT_NE(recommended_input_volume, recommended_input_volume_after_startup);
    ASSERT_EQ(recommended_input_volume, kZeroVolume);
  }
}

// Tests that the minimum input volume is applied if the volume is not zero
// before it is automatically adjusted.
TEST_P(InputVolumeNotZeroParameterizedTest,
       VerifyMinVolumeAppliedAfterAutomaticVolumeAdjustments) {
  const int applied_startup_input_volume = GetStartupVolume();
  const int applied_input_volume = GetVolume();
  const bool agc1_analog_controller_enabled = GetAgc1AnalogControllerEnabled();
  const bool agc2_input_volume_controller_enabled =
      GetAgc2InputVolumeControllerEnabled();
  auto apm = CreateApmForInputVolumeTest(agc1_analog_controller_enabled,
                                         agc2_input_volume_controller_enabled);

  ProcessInputVolume(*apm, /*num_frames=*/1, applied_startup_input_volume);
  const int recommended_input_volume =
      ProcessInputVolume(*apm, /*num_frames=*/400, applied_input_volume);

  ASSERT_NE(applied_input_volume, 0);

  if (!agc1_analog_controller_enabled &&
      !agc2_input_volume_controller_enabled) {
    // No input volume changes if none of the analog controllers is enabled.
    ASSERT_EQ(recommended_input_volume, applied_input_volume);
  } else {
    if (recommended_input_volume != applied_input_volume) {
      ASSERT_GE(recommended_input_volume, GetMinVolume());
    }
  }
}

// Tests that the minimum input volume is not applied if the volume is zero
// before it is automatically adjusted.
TEST_P(InputVolumeZeroParameterizedTest,
       VerifyMinVolumeNotAppliedAfterAutomaticVolumeAdjustments) {
  constexpr int kZeroVolume = 0;
  const int applied_startup_input_volume = GetStartupVolume();
  const bool agc1_analog_controller_enabled = GetAgc1AnalogControllerEnabled();
  const bool agc2_input_volume_controller_enabled =
      GetAgc2InputVolumeControllerEnabled();
  auto apm = CreateApmForInputVolumeTest(agc1_analog_controller_enabled,
                                         agc2_input_volume_controller_enabled);

  const int recommended_input_volume_after_startup =
      ProcessInputVolume(*apm, /*num_frames=*/1, applied_startup_input_volume);
  const int recommended_input_volume =
      ProcessInputVolume(*apm, /*num_frames=*/400, kZeroVolume);

  if (!agc1_analog_controller_enabled &&
      !agc2_input_volume_controller_enabled) {
    // No input volume changes if none of the analog controllers is enabled.
    ASSERT_EQ(recommended_input_volume, kZeroVolume);
  } else {
    ASSERT_NE(recommended_input_volume, recommended_input_volume_after_startup);
    ASSERT_EQ(recommended_input_volume, kZeroVolume);
  }
}

INSTANTIATE_TEST_SUITE_P(AudioProcessingImplTest,
                         InputVolumeStartupParameterizedTest,
                         ::testing::Combine(::testing::Values(0, 5, 30),
                                            ::testing::Values(absl::nullopt,
                                                              20),
                                            ::testing::Bool(),
                                            ::testing::Bool()));

INSTANTIATE_TEST_SUITE_P(AudioProcessingImplTest,
                         InputVolumeNotZeroParameterizedTest,
                         ::testing::Combine(::testing::Values(0, 5, 15),
                                            ::testing::Values(1, 5, 30),
                                            ::testing::Values(absl::nullopt,
                                                              20),
                                            ::testing::Bool(),
                                            ::testing::Bool()));

INSTANTIATE_TEST_SUITE_P(AudioProcessingImplTest,
                         InputVolumeZeroParameterizedTest,
                         ::testing::Combine(::testing::Values(0, 5, 15),
                                            ::testing::Values(absl::nullopt,
                                                              20),
                                            ::testing::Bool(),
                                            ::testing::Bool()));

// When the input volume is not emulated and no input volume controller is
// active, the recommended volume must always be the applied volume.
TEST(AudioProcessingImplTest,
     RecommendAppliedInputVolumeWithNoAgcWithNoEmulation) {
  auto apm = AudioProcessingBuilder()
                 .SetConfig({.capture_level_adjustment = {.enabled = false},
                             .gain_controller1 = {.enabled = false}})
                 .Create();

  constexpr int kOneFrame = 1;
  EXPECT_EQ(ProcessInputVolume(*apm, kOneFrame, /*initial_volume=*/123), 123);
  EXPECT_EQ(ProcessInputVolume(*apm, kOneFrame, /*initial_volume=*/59), 59);
  EXPECT_EQ(ProcessInputVolume(*apm, kOneFrame, /*initial_volume=*/135), 135);
}

// When the input volume is emulated, the recommended volume must always be the
// applied volume and at any time it must not be that set in the input volume
// emulator.
// TODO(bugs.webrtc.org/14581): Enable when APM fixed to let this test pass.
TEST(AudioProcessingImplTest,
     DISABLED_RecommendAppliedInputVolumeWithNoAgcWithEmulation) {
  auto apm =
      AudioProcessingBuilder()
          .SetConfig({.capture_level_adjustment = {.enabled = true,
                                                   .analog_mic_gain_emulation{
                                                       .enabled = true,
                                                       .initial_level = 255}},
                      .gain_controller1 = {.enabled = false}})
          .Create();

  constexpr int kOneFrame = 1;
  EXPECT_EQ(ProcessInputVolume(*apm, kOneFrame, /*initial_volume=*/123), 123);
  EXPECT_EQ(ProcessInputVolume(*apm, kOneFrame, /*initial_volume=*/59), 59);
  EXPECT_EQ(ProcessInputVolume(*apm, kOneFrame, /*initial_volume=*/135), 135);
}

// Even if there is an enabled input volume controller, when the input volume is
// emulated, the recommended volume is always the applied volume because the
// active controller must only adjust the internally emulated volume and leave
// the externally applied volume unchanged.
// TODO(bugs.webrtc.org/14581): Enable when APM fixed to let this test pass.
TEST(AudioProcessingImplTest,
     DISABLED_RecommendAppliedInputVolumeWithAgcWithEmulation) {
  auto apm =
      AudioProcessingBuilder()
          .SetConfig({.capture_level_adjustment = {.enabled = true,
                                                   .analog_mic_gain_emulation{
                                                       .enabled = true}},
                      .gain_controller1 = {.enabled = true,
                                           .analog_gain_controller{
                                               .enabled = true,
                                           }}})
          .Create();

  constexpr int kOneFrame = 1;
  EXPECT_EQ(ProcessInputVolume(*apm, kOneFrame, /*initial_volume=*/123), 123);
  EXPECT_EQ(ProcessInputVolume(*apm, kOneFrame, /*initial_volume=*/59), 59);
  EXPECT_EQ(ProcessInputVolume(*apm, kOneFrame, /*initial_volume=*/135), 135);
}

TEST(AudioProcessingImplInputVolumeControllerExperimentTest,
     ConfigAdjustedWhenExperimentEnabledAndAgc1AnalogEnabled) {
  webrtc::test::ScopedFieldTrials field_trials(
      "WebRTC-Audio-InputVolumeControllerExperiment/"
      "Enabled,"
      "enable_clipping_predictor:true,"
      "clipped_level_min:20,"
      "clipped_level_step:30,"
      "clipped_ratio_threshold:0.4,"
      "clipped_wait_frames:50,"
      "target_range_max_dbfs:-6,"
      "target_range_min_dbfs:-70,"
      "update_input_volume_wait_frames:80,"
      "speech_probability_threshold:0.9,"
      "speech_ratio_threshold:1.0/");

  AudioProcessingBuilderForTesting apm_builder;

  // Set a config with analog AGC1 enabled.
  AudioProcessing::Config config;
  config.gain_controller1.enabled = true;
  config.gain_controller1.analog_gain_controller.enabled = true;
  config.gain_controller1.analog_gain_controller.enable_digital_adaptive = true;
  config.gain_controller2.enabled = false;
  config.gain_controller1.mode =
      AudioProcessing::Config::GainController1::kAdaptiveAnalog;

  EXPECT_FALSE(config.gain_controller2.input_volume_controller.enabled);

  apm_builder.SetConfig(config);

  auto apm = apm_builder.Create();
  auto adjusted_config = apm->GetConfig();

  // Expect the config to be adjusted.
  EXPECT_FALSE(adjusted_config.gain_controller1.enabled);
  EXPECT_FALSE(adjusted_config.gain_controller1.analog_gain_controller.enabled);
  EXPECT_TRUE(adjusted_config.gain_controller2.enabled);
  EXPECT_TRUE(adjusted_config.gain_controller2.adaptive_digital.enabled);
  EXPECT_TRUE(adjusted_config.gain_controller2.input_volume_controller.enabled);

  // Change config back and compare.
  adjusted_config.gain_controller1.enabled = config.gain_controller1.enabled;
  adjusted_config.gain_controller1.analog_gain_controller.enabled =
      config.gain_controller1.analog_gain_controller.enabled;
  adjusted_config.gain_controller2.enabled = config.gain_controller2.enabled;
  adjusted_config.gain_controller2.adaptive_digital.enabled =
      config.gain_controller2.adaptive_digital.enabled;
  adjusted_config.gain_controller2.input_volume_controller.enabled =
      config.gain_controller2.input_volume_controller.enabled;

  EXPECT_THAT(adjusted_config.ToString(), ::testing::StrEq(config.ToString()));
}

TEST(AudioProcessingImplInputVolumeControllerExperimentTest,
     ConfigAdjustedWhenExperimentEnabledAndHybridAgcEnabled) {
  webrtc::test::ScopedFieldTrials field_trials(
      "WebRTC-Audio-InputVolumeControllerExperiment/"
      "Enabled,"
      "enable_clipping_predictor:true,"
      "clipped_level_min:20,"
      "clipped_level_step:30,"
      "clipped_ratio_threshold:0.4,"
      "clipped_wait_frames:50,"
      "target_range_max_dbfs:-6,"
      "target_range_min_dbfs:-70,"
      "update_input_volume_wait_frames:80,"
      "speech_probability_threshold:0.9,"
      "speech_ratio_threshold:1.0/");

  AudioProcessingBuilderForTesting apm_builder;

  // Set a config with hybrid AGC enabled.
  AudioProcessing::Config config;
  config.gain_controller1.enabled = true;
  config.gain_controller1.analog_gain_controller.enabled = true;
  config.gain_controller1.analog_gain_controller.enable_digital_adaptive =
      false;
  config.gain_controller2.enabled = true;
  config.gain_controller2.adaptive_digital.enabled = true;
  config.gain_controller1.mode =
      AudioProcessing::Config::GainController1::kAdaptiveAnalog;

  EXPECT_FALSE(config.gain_controller2.input_volume_controller.enabled);

  apm_builder.SetConfig(config);

  auto apm = apm_builder.Create();
  auto adjusted_config = apm->GetConfig();

  // Expect the config to be adjusted.
  EXPECT_FALSE(adjusted_config.gain_controller1.enabled);
  EXPECT_FALSE(adjusted_config.gain_controller1.analog_gain_controller.enabled);
  EXPECT_TRUE(adjusted_config.gain_controller2.enabled);
  EXPECT_TRUE(adjusted_config.gain_controller2.adaptive_digital.enabled);
  EXPECT_TRUE(adjusted_config.gain_controller2.input_volume_controller.enabled);

  // Change config back and compare.
  adjusted_config.gain_controller1.enabled = config.gain_controller1.enabled;
  adjusted_config.gain_controller1.analog_gain_controller.enabled =
      config.gain_controller1.analog_gain_controller.enabled;
  adjusted_config.gain_controller2.enabled = config.gain_controller2.enabled;
  adjusted_config.gain_controller2.adaptive_digital.enabled =
      config.gain_controller2.adaptive_digital.enabled;
  adjusted_config.gain_controller2.input_volume_controller.enabled =
      config.gain_controller2.input_volume_controller.enabled;

  EXPECT_THAT(adjusted_config.ToString(), ::testing::StrEq(config.ToString()));
}

TEST(AudioProcessingImplInputVolumeControllerExperimentTest,
     ConfigNotAdjustedWhenExperimentEnabledAndAgc1AnalogNotEnabled) {
  webrtc::test::ScopedFieldTrials field_trials(
      "WebRTC-Audio-InputVolumeControllerExperiment/"
      "Enabled,"
      "enable_clipping_predictor:true,"
      "clipped_level_min:20,"
      "clipped_level_step:30,"
      "clipped_ratio_threshold:0.4,"
      "clipped_wait_frames:50,"
      "target_range_max_dbfs:-6,"
      "target_range_min_dbfs:-70,"
      "update_input_volume_wait_frames:80,"
      "speech_probability_threshold:0.9,"
      "speech_ratio_threshold:1.0/");

  AudioProcessingBuilderForTesting apm_builder;

  // Set a config with analog AGC1 not enabled.
  AudioProcessing::Config config;
  config.gain_controller1.enabled = false;
  config.gain_controller1.analog_gain_controller.enabled = true;
  config.gain_controller1.analog_gain_controller.enable_digital_adaptive = true;
  config.gain_controller2.enabled = false;
  config.gain_controller1.mode =
      AudioProcessing::Config::GainController1::kAdaptiveAnalog;

  EXPECT_FALSE(config.gain_controller2.input_volume_controller.enabled);

  apm_builder.SetConfig(config);

  auto apm = apm_builder.Create();
  auto adjusted_config = apm->GetConfig();

  EXPECT_EQ(config.gain_controller1.enabled,
            adjusted_config.gain_controller1.enabled);
  EXPECT_EQ(config.gain_controller1.analog_gain_controller.enabled,
            adjusted_config.gain_controller1.analog_gain_controller.enabled);
  EXPECT_EQ(config.gain_controller2.enabled,
            adjusted_config.gain_controller2.enabled);
  EXPECT_EQ(config.gain_controller2.adaptive_digital.enabled,
            adjusted_config.gain_controller2.adaptive_digital.enabled);
  EXPECT_FALSE(
      adjusted_config.gain_controller2.input_volume_controller.enabled);

  EXPECT_THAT(adjusted_config.ToString(), ::testing::StrEq(config.ToString()));
}

TEST(AudioProcessingImplInputVolumeControllerExperimentTest,
     ConfigNotAdjustedWhenExperimentEnabledAndHybridAgcNotEnabled) {
  webrtc::test::ScopedFieldTrials field_trials(
      "WebRTC-Audio-InputVolumeControllerExperiment/"
      "Enabled,"
      "enable_clipping_predictor:true,"
      "clipped_level_min:20,"
      "clipped_level_step:30,"
      "clipped_ratio_threshold:0.4,"
      "clipped_wait_frames:50,"
      "target_range_max_dbfs:-6,"
      "target_range_min_dbfs:-70,"
      "update_input_volume_wait_frames:80,"
      "speech_probability_threshold:0.9,"
      "speech_ratio_threshold:1.0/");

  AudioProcessingBuilderForTesting apm_builder;

  // Set a config with hybrid AGC analog not enabled.
  AudioProcessing::Config config;
  config.gain_controller1.enabled = false;
  config.gain_controller1.analog_gain_controller.enabled = true;
  config.gain_controller1.analog_gain_controller.enable_digital_adaptive =
      false;
  config.gain_controller2.enabled = true;
  config.gain_controller2.adaptive_digital.enabled = true;
  config.gain_controller1.mode =
      AudioProcessing::Config::GainController1::kAdaptiveAnalog;

  EXPECT_FALSE(config.gain_controller2.input_volume_controller.enabled);

  apm_builder.SetConfig(config);

  auto apm = apm_builder.Create();
  auto adjusted_config = apm->GetConfig();

  EXPECT_EQ(config.gain_controller1.enabled,
            adjusted_config.gain_controller1.enabled);
  EXPECT_EQ(config.gain_controller1.analog_gain_controller.enabled,
            adjusted_config.gain_controller1.analog_gain_controller.enabled);
  EXPECT_EQ(config.gain_controller2.enabled,
            adjusted_config.gain_controller2.enabled);
  EXPECT_EQ(config.gain_controller2.adaptive_digital.enabled,
            adjusted_config.gain_controller2.adaptive_digital.enabled);
  EXPECT_FALSE(
      adjusted_config.gain_controller2.input_volume_controller.enabled);

  EXPECT_THAT(adjusted_config.ToString(), ::testing::StrEq(config.ToString()));
}

TEST(AudioProcessingImplInputVolumeControllerExperimentTest,
     ConfigNotAdjustedWhenExperimentNotEnabledAndAgc1AnalogEnabled) {
  AudioProcessingBuilderForTesting apm_builder;

  // Set a config with analog AGC1 analog enabled.
  AudioProcessing::Config config;
  config.gain_controller1.enabled = true;
  config.gain_controller1.analog_gain_controller.enabled = true;
  config.gain_controller1.analog_gain_controller.enable_digital_adaptive = true;
  config.gain_controller2.enabled = false;
  config.gain_controller1.mode =
      AudioProcessing::Config::GainController1::kAdaptiveAnalog;

  EXPECT_FALSE(config.gain_controller2.input_volume_controller.enabled);

  apm_builder.SetConfig(config);

  auto apm = apm_builder.Create();
  auto adjusted_config = apm->GetConfig();

  EXPECT_EQ(config.gain_controller1.enabled,
            adjusted_config.gain_controller1.enabled);
  EXPECT_EQ(config.gain_controller1.analog_gain_controller.enabled,
            adjusted_config.gain_controller1.analog_gain_controller.enabled);
  EXPECT_EQ(config.gain_controller2.enabled,
            adjusted_config.gain_controller2.enabled);
  EXPECT_EQ(config.gain_controller2.adaptive_digital.enabled,
            adjusted_config.gain_controller2.adaptive_digital.enabled);
  EXPECT_FALSE(
      adjusted_config.gain_controller2.input_volume_controller.enabled);

  EXPECT_THAT(adjusted_config.ToString(), ::testing::StrEq(config.ToString()));
}

TEST(AudioProcessingImplInputVolumeControllerExperimentTest,
     ConfigNotAdjustedWhenExperimentNotEnabledAndHybridAgcEnabled) {
  AudioProcessingBuilderForTesting apm_builder;

  // Set a config with hybrid AGC enabled.
  AudioProcessing::Config config;
  config.gain_controller1.enabled = true;
  config.gain_controller1.analog_gain_controller.enabled = true;
  config.gain_controller1.analog_gain_controller.enable_digital_adaptive =
      false;
  config.gain_controller2.enabled = true;
  config.gain_controller2.adaptive_digital.enabled = true;
  config.gain_controller1.mode =
      AudioProcessing::Config::GainController1::kAdaptiveAnalog;

  EXPECT_FALSE(config.gain_controller2.input_volume_controller.enabled);

  apm_builder.SetConfig(config);

  auto apm = apm_builder.Create();
  auto adjusted_config = apm->GetConfig();

  EXPECT_EQ(config.gain_controller1.enabled,
            adjusted_config.gain_controller1.enabled);
  EXPECT_EQ(config.gain_controller1.analog_gain_controller.enabled,
            adjusted_config.gain_controller1.analog_gain_controller.enabled);
  EXPECT_EQ(config.gain_controller2.enabled,
            adjusted_config.gain_controller2.enabled);
  EXPECT_EQ(config.gain_controller2.adaptive_digital.enabled,
            adjusted_config.gain_controller2.adaptive_digital.enabled);
  EXPECT_FALSE(
      adjusted_config.gain_controller2.input_volume_controller.enabled);

  EXPECT_THAT(adjusted_config.ToString(), ::testing::StrEq(config.ToString()));
}

}  // namespace webrtc
