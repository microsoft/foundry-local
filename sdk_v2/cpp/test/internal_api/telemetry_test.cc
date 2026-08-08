// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "logger.h"
#include "telemetry/telemetry_action_tracker.h"
#include "telemetry/device_id.h"
#include "telemetry/telemetry_environment.h"
#include "telemetry/telemetry_logger.h"
#include "telemetry/telemetry_metadata.h"
#include "telemetry/one_ds_telemetry.h"
#include "telemetry/telemetry_redaction.h"
#include "telemetry/telemetry_sampling.h"
#include "test_helpers.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace fl;

namespace {

class ScopedEnvVar {
 public:
  ScopedEnvVar(const char* name, const char* value) : name_(name) {
    original_ = TelemetryEnvironment::GetEnv(name);
    had_original_ = !original_.empty();
#ifdef _WIN32
    ::SetEnvironmentVariableA(name, value);
#else
    setenv(name, value, 1);
#endif
  }

  ~ScopedEnvVar() {
#ifdef _WIN32
    if (had_original_) {
      ::SetEnvironmentVariableA(name_.c_str(), original_.c_str());
    } else {
      ::SetEnvironmentVariableA(name_.c_str(), nullptr);
    }
#else
    if (had_original_) {
      setenv(name_.c_str(), original_.c_str(), 1);
    } else {
      unsetenv(name_.c_str());
    }
#endif
  }

 private:
  std::string name_;
  std::string original_;
  bool had_original_ = false;
};

struct LogEntry {
  LogLevel level;
  std::string message;
};

class RecordingLogger : public ILogger {
 public:
  void Log(LogLevel level, std::string_view message) override {
    entries.push_back(LogEntry{level, std::string(message)});
  }

  std::vector<LogEntry> entries;
};

struct ActionCall {
  Action action;
  ActionStatus status;
  std::string user_agent;
  bool indirect;
  int64_t duration_ms;
  std::string model_id;
};

class CapturingTelemetry : public ITelemetry {
 public:
  using ITelemetry::RecordAction;

  void RecordAction(Action action, ActionStatus status,
                    const InvocationContext& context, int64_t duration_ms,
                    const std::string& model_id) override {
    action_calls.push_back(
        ActionCall{action, status, context.user_agent, context.indirect, duration_ms, model_id});
  }

  void RecordException(Action action, const std::exception& exception,
                       const InvocationContext& /*context*/) override {
    exception_calls.emplace_back(action, exception.what());
  }

  void RecordModelUsage(const ModelUsageInfo&) override {}

  void RecordEpDownloadAttempt(const EpDownloadAttemptInfo&) override {}

  void RecordEpDownloadAndRegister(const EpDownloadAndRegisterInfo&) override {}

  void RecordDownload(const DownloadInfo&) override {}

  void RecordCatalogFetch(const CatalogFetchInfo&) override {}

  std::vector<ActionCall> action_calls;
  std::vector<std::pair<Action, std::string>> exception_calls;
};

}  // namespace

TEST(TelemetryEnvironmentTest, TruthyValueParsing) {
  EXPECT_FALSE(TelemetryEnvironment::IsTruthyValue(""));
  EXPECT_FALSE(TelemetryEnvironment::IsTruthyValue("   "));
  EXPECT_FALSE(TelemetryEnvironment::IsTruthyValue("0"));
  EXPECT_FALSE(TelemetryEnvironment::IsTruthyValue(" false "));
  EXPECT_FALSE(TelemetryEnvironment::IsTruthyValue("NO"));
  EXPECT_FALSE(TelemetryEnvironment::IsTruthyValue("off"));
  EXPECT_TRUE(TelemetryEnvironment::IsTruthyValue("1"));
  EXPECT_TRUE(TelemetryEnvironment::IsTruthyValue("true"));
  EXPECT_TRUE(TelemetryEnvironment::IsTruthyValue("yes"));
  EXPECT_TRUE(TelemetryEnvironment::IsTruthyValue("anything"));
}

TEST(TelemetryEnvironmentTest, DetectsCiEnvironmentFlag) {
  ScopedEnvVar ci("CI", "true");
  EXPECT_TRUE(TelemetryEnvironment::IsCiEnvironment());
}

TEST(TelemetryEnvironmentTest, DetectsSharedOrtTelemetryOptOut) {
  ScopedEnvVar disabled("ORT_TELEMETRY_DISABLED", "true");
  EXPECT_TRUE(TelemetryEnvironment::IsTelemetryDisabledByEnvVar());
}

TEST(OneDsTelemetryTest, DisableNonessentialTelemetrySuppressesUpload) {
  // In test processes, hard suppression prevents 1DS upload entirely. Outside tests/CI,
  // manager disable_nonessential_telemetry initializes 1DS but suppresses non-ProcessInfo uploads.
  OneDsTelemetry telemetry("TestApp", fl::test::NullLog(), /*disable_nonessential_telemetry=*/true);
  EXPECT_FALSE(telemetry.IsUploadEnabled());
}

TEST(TelemetryActionTest, EpActionNamesMatchEventNames) {
  EXPECT_EQ(ActionToString(Action::kEpDownloadAttempt), "EPDownloadAttempt");
  EXPECT_EQ(ActionToString(Action::kEpDownloadAndRegister), "EPDownloadAndRegister");
}

TEST(TelemetryActionTest, StatusNamesIncludeDetailedFailures) {
  EXPECT_EQ(ActionStatusToString(ActionStatus::kClientError), "ClientError");
  EXPECT_EQ(ActionStatusToString(ActionStatus::kCanceled), "Canceled");
  EXPECT_EQ(ActionStatusToString(ActionStatus::kDependencyFailure), "DependencyFailure");
  EXPECT_EQ(ActionStatusToString(ActionStatus::kTimeout), "Timeout");
}

TEST(TelemetryActionTest, DirectContextUsesDefaultUserAgent) {
  SetDefaultUserAgent("foundry-local-test/1.0");
  auto context = InvocationContext::Direct();
  EXPECT_EQ(context.user_agent, "foundry-local-test/1.0");
  EXPECT_FALSE(context.correlation_id.empty());
  EXPECT_FALSE(context.indirect);
}

TEST(TelemetryActionTest, CompatibilityActionOverloadCreatesCorrelationId) {
  CapturingTelemetry telemetry;

  telemetry.RecordAction(Action::kCoreInitialize, ActionStatus::kSuccess, "test-agent", true, 42);

  ASSERT_EQ(telemetry.action_calls.size(), 1u);
  EXPECT_EQ(telemetry.action_calls.front().user_agent, "test-agent");
  EXPECT_TRUE(telemetry.action_calls.front().indirect);
  EXPECT_EQ(telemetry.action_calls.front().duration_ms, 42);
  SetDefaultUserAgent({});
}

TEST(TelemetryLoggerTest, RecordActionIncludesConcreteFields) {
  RecordingLogger logger;
  TelemetryLogger telemetry("foundry-local", logger);

  telemetry.RecordAction(Action::kModelFileDownload, ActionStatus::kSuccess,
                         InvocationContext{"cli/1.0", "corr-1", false}, 1234);

  ASSERT_EQ(logger.entries.size(), 1u);
  EXPECT_EQ(logger.entries[0].level, LogLevel::Debug);
  EXPECT_NE(logger.entries[0].message.find("AppName=foundry-local"), std::string::npos);
  EXPECT_NE(logger.entries[0].message.find("UserAgent=cli/1.0"), std::string::npos);
  EXPECT_NE(logger.entries[0].message.find("CorrelationId=corr-1"), std::string::npos);
  EXPECT_NE(logger.entries[0].message.find("Action=ModelFileDownload"), std::string::npos);
  EXPECT_NE(logger.entries[0].message.find("Status=Success"), std::string::npos);
  EXPECT_NE(logger.entries[0].message.find("Direct=true"), std::string::npos);
  EXPECT_NE(logger.entries[0].message.find("TimeMs=1234"), std::string::npos);
}

TEST(TelemetryLoggerTest, RecordExceptionAndModelEventsIncludeSpecificValues) {
  RecordingLogger logger;
  TelemetryLogger telemetry("foundry-local", logger);

  telemetry.RecordException(Action::kModelLoad, std::runtime_error("config missing"), InvocationContext{});

  ModelUsageInfo usage;
  usage.model_id = "phi-3-mini";
  usage.execution_provider = "CPU";
  usage.user_agent = "cli/1.0";
  usage.total_tokens = 31;
  usage.input_token_count = 17;
  usage.total_time_ms = 250;
  telemetry.RecordModelUsage(usage);

  telemetry.RecordAction(Action::kModelLoad, ActionStatus::kSuccess,
                         InvocationContext{"cli/1.0", "", false}, 250, "phi-3-mini");

  ASSERT_EQ(logger.entries.size(), 3u);
  EXPECT_NE(logger.entries[0].message.find("Action=ModelLoad"), std::string::npos);
  EXPECT_NE(logger.entries[0].message.find("Exception=config missing"), std::string::npos);

  EXPECT_NE(logger.entries[1].message.find("Model "), std::string::npos);
  EXPECT_NE(logger.entries[1].message.find("ModelId=phi-3-mini"), std::string::npos);
  EXPECT_NE(logger.entries[1].message.find("InputTokenCount=17"), std::string::npos);
  EXPECT_NE(logger.entries[1].message.find("TotalTokens=31"), std::string::npos);
  EXPECT_NE(logger.entries[1].message.find("TotalTimeMs=250"), std::string::npos);

  EXPECT_NE(logger.entries[2].message.find("Action=ModelLoad"), std::string::npos);
  EXPECT_NE(logger.entries[2].message.find("ModelId=phi-3-mini"), std::string::npos);
}

TEST(TelemetryLoggerTest, RecordAudioUsageIncludesAudioSpecificFields) {
  RecordingLogger logger;
  TelemetryLogger telemetry("foundry-local", logger);

  AudioUsageInfo info;
  info.model_id = "whisper-tiny";
  info.execution_provider = "CPUExecutionProvider";
  info.user_agent = "cli/1.0";
  info.correlation_id = "corr-audio";
  info.audio_source = "streaming_pcm";
  info.language = "en";
  info.stream = true;
  info.indirect = true;
  info.total_time_ms = 1234;
  info.total_tokens = 42;
  info.input_token_count = 0;
  info.completion_token_count = 42;
  info.audio_duration_ms = 5000;
  info.sample_rate = 16000;
  info.channels = 1;

  telemetry.RecordAudioUsage(info);

  ASSERT_EQ(logger.entries.size(), 1u);
  EXPECT_NE(logger.entries[0].message.find("AudioModel"), std::string::npos);
  EXPECT_NE(logger.entries[0].message.find("ModelId=whisper-tiny"), std::string::npos);
  EXPECT_NE(logger.entries[0].message.find("AudioSource=streaming_pcm"), std::string::npos);
  EXPECT_NE(logger.entries[0].message.find("Language=en"), std::string::npos);
  EXPECT_NE(logger.entries[0].message.find("AudioDurationMs=5000"), std::string::npos);
  EXPECT_NE(logger.entries[0].message.find("SampleRate=16000"), std::string::npos);
  EXPECT_NE(logger.entries[0].message.find("Channels=1"), std::string::npos);
  EXPECT_NE(logger.entries[0].message.find("Direct=false"), std::string::npos);
}

TEST(TelemetryLoggerTest, RecordProcessInfoIncludesStartupMetadata) {
  RecordingLogger logger;
  TelemetryLogger telemetry("foundry-local", logger);

  ProcessInfo info;
  info.app_name = "foundry-local";
  info.app_version = "4.5.6";
  info.os_name = "Windows";
  info.os_version = "10.0.26100";
  info.cpu_arch = "amd64";
  info.process_name = "foundry_local_test.exe";
  info.device_id_status = "Existing";
  info.cpu_count = 8;
  info.total_memory_mb = 32768;

  telemetry.RecordProcessInfo(info);

  ASSERT_EQ(logger.entries.size(), 1u);
  EXPECT_NE(logger.entries[0].message.find("ProcessInfo"), std::string::npos);
  EXPECT_NE(logger.entries[0].message.find("AppVersion=4.5.6"), std::string::npos);
  EXPECT_NE(logger.entries[0].message.find("ProcessName=foundry_local_test.exe"), std::string::npos);
  EXPECT_NE(logger.entries[0].message.find("DeviceIdStatus=Existing"), std::string::npos);
  EXPECT_NE(logger.entries[0].message.find("CpuCount=8"), std::string::npos);
  EXPECT_NE(logger.entries[0].message.find("TotalMemoryMB=32768"), std::string::npos);
}

TEST(TelemetryLoggerTest, RecordExceptionRedactsPaths) {
  RecordingLogger logger;
  TelemetryLogger telemetry("foundry-local", logger);

  telemetry.RecordException(Action::kModelLoad, std::runtime_error("failed at C:\\Users\\Alice\\model.onnx"),
                            InvocationContext{"cli/1.0", "corr-error", false});

  ASSERT_EQ(logger.entries.size(), 1u);
  EXPECT_NE(logger.entries[0].message.find("[Telemetry] Error"), std::string::npos);
  EXPECT_NE(logger.entries[0].message.find("Exception=failed at [path]"), std::string::npos);
  EXPECT_EQ(logger.entries[0].message.find("Alice"), std::string::npos);
}

TEST(TelemetryLoggerTest, RecordHardwareInfoIncludesCoarseAcceleratorInventory) {
  RecordingLogger logger;
  TelemetryLogger telemetry("foundry-local", logger);

  HardwareInfo info;
  info.has_cpu = true;
  info.has_gpu = true;
  info.device_type_count = 2;
  info.execution_provider_count = 3;
  info.device_types = "CPU,GPU";
  info.execution_providers = "CPUExecutionProvider,CUDAExecutionProvider,WebGpuExecutionProvider";

  telemetry.RecordHardwareInfo(info);

  ASSERT_EQ(logger.entries.size(), 1u);
  EXPECT_NE(logger.entries[0].message.find("HardwareInfo"), std::string::npos);
  EXPECT_NE(logger.entries[0].message.find("DeviceTypes=CPU,GPU"), std::string::npos);
  EXPECT_NE(logger.entries[0].message.find("ExecutionProviderCount=3"), std::string::npos);
  EXPECT_NE(logger.entries[0].message.find("HasGPU=true"), std::string::npos);
}

TEST(TelemetryMetadataTest, HostAppVersionIsAlwaysPopulated) {
  auto metadata = BuildTelemetryMetadata("foundry-local-test");

  EXPECT_FALSE(metadata.app_version.empty());
  EXPECT_FALSE(metadata.version.empty());
}

TEST(TelemetryDeviceIdTest, ValidatesGuidShapeAndHashesForUpload) {
  EXPECT_TRUE(TelemetryDeviceId::IsValidGuid("01234567-89ab-4def-8123-456789abcdef"));
  EXPECT_FALSE(TelemetryDeviceId::IsValidGuid("0123456789ab4def8123456789abcdef"));
  EXPECT_FALSE(TelemetryDeviceId::IsValidGuid("zzzzzzzz-89ab-4def-8123-456789abcdef"));

  auto hashed = TelemetryDeviceId::HashForTelemetry("01234567-89ab-4def-8123-456789abcdef");
  EXPECT_EQ(hashed, "c:6225BD190D6CCF87766A49C9986D174DEF3391FE175A61525E49A1D2334D6A43");
}

TEST(TelemetryRedactionTest, ScrubsPathsKeepsNonPathTextAndCapsLength) {
  EXPECT_EQ(ScrubStringForTelemetry("config missing"), "config missing");
  EXPECT_EQ(ScrubStringForTelemetry("/secret"), "[path]");
  EXPECT_EQ(ScrubStringForTelemetry("failed at /secret"), "failed at [path]");
  EXPECT_EQ(ScrubStringForTelemetry("Load C:\\Users\\First Last\\model.onnx failed"), "Load [path]");
  EXPECT_EQ(ScrubStringForTelemetry("open /home/alice/model.onnx failed"), "open [path]");
  EXPECT_EQ(ScrubStringForTelemetry("failed at models/alice.onnx"), "failed at [path]");
  EXPECT_EQ(ScrubStringForTelemetry("ratio 3/4 and and/or"), "ratio 3/4 and and/or");

  const std::string long_msg(kMaxTelemetryStringLength + 100, 'x');
  EXPECT_EQ(ScrubStringForTelemetry(long_msg).size(), kMaxTelemetryStringLength);

  const std::string euro = "\xE2\x82\xAC";
  const std::string partial_tail = std::string(kMaxTelemetryStringLength - 1, 'x') + euro;
  EXPECT_EQ(ScrubStringForTelemetry(partial_tail), std::string(kMaxTelemetryStringLength - 1, 'x'));
}

TEST(TelemetrySamplingTest, SamplesAllEventsAtCurrentDefaultRate) {
  EXPECT_TRUE(TelemetryInternal::ShouldSampleTelemetryEvent("app-session", "corr-1"));
}

TEST(TelemetrySamplingTest, HonorsZeroAndHundredPercentRates) {
  EXPECT_FALSE(TelemetryInternal::ShouldSampleTelemetryEvent("app-session", "corr-1", 0.0));
  EXPECT_TRUE(TelemetryInternal::ShouldSampleTelemetryEvent("app-session", "corr-1", 100.0));
}

TEST(TelemetrySamplingTest, SamplesCoreAudioTranscribeAtTwoPercent) {
  EXPECT_DOUBLE_EQ(TelemetryInternal::SampleRateForAction("OpenAIAudioTranscribe"), 2.0);
  EXPECT_DOUBLE_EQ(TelemetryInternal::SampleRateForAction("ModelList"), 100.0);

  bool retained = false;
  bool dropped = false;
  for (int i = 0; i < 10'000 && (!retained || !dropped); ++i) {
    const bool sampled = TelemetryInternal::ShouldSampleTelemetryEvent(
        "app-session", "audio-correlation-" + std::to_string(i), 2.0);
    retained = retained || sampled;
    dropped = dropped || !sampled;
  }
  EXPECT_TRUE(retained);
  EXPECT_TRUE(dropped);
}

TEST(ActionTrackerTest, DestructorRecordsFailureByDefaultWithoutModelId) {
  CapturingTelemetry telemetry;
  SetDefaultUserAgent("foundry-local-test/2.0");

  {
    ActionTracker tracker(Action::kModelFileDownload, telemetry);
  }

  ASSERT_EQ(telemetry.action_calls.size(), 1u);
  EXPECT_EQ(telemetry.action_calls[0].action, Action::kModelFileDownload);
  EXPECT_EQ(telemetry.action_calls[0].status, ActionStatus::kFailure);
  EXPECT_EQ(telemetry.action_calls[0].user_agent, "foundry-local-test/2.0");
  EXPECT_FALSE(telemetry.action_calls[0].indirect);
  EXPECT_GE(telemetry.action_calls[0].duration_ms, 0);
  EXPECT_TRUE(telemetry.action_calls[0].model_id.empty());
  SetDefaultUserAgent({});
}

TEST(ActionTrackerTest, RecordsExceptionSuccessAndModelIdOnAction) {
  CapturingTelemetry telemetry;

  {
    ActionTracker tracker(Action::kModelLoad, telemetry, InvocationContext{"cli/3.0", "", false});
    tracker.RecordException(std::runtime_error("failed to load"));
    tracker.SetModelId("phi-3-mini");
    tracker.SetStatus(ActionStatus::kSuccess);
  }

  ASSERT_EQ(telemetry.exception_calls.size(), 1u);
  EXPECT_EQ(telemetry.exception_calls[0].first, Action::kModelLoad);
  EXPECT_EQ(telemetry.exception_calls[0].second, "failed to load");

  ASSERT_EQ(telemetry.action_calls.size(), 1u);
  EXPECT_EQ(telemetry.action_calls[0].action, Action::kModelLoad);
  EXPECT_EQ(telemetry.action_calls[0].status, ActionStatus::kSuccess);
  EXPECT_EQ(telemetry.action_calls[0].user_agent, "cli/3.0");
  EXPECT_FALSE(telemetry.action_calls[0].indirect);
  EXPECT_GE(telemetry.action_calls[0].duration_ms, 0);

  EXPECT_EQ(telemetry.action_calls[0].model_id, "phi-3-mini");
}