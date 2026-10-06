// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "exception.h"
#include "logger.h"
#include "platform/telemetry_device_id.h"
#include "telemetry/telemetry_action_tracker.h"
#include "telemetry/download_tracker.h"
#include "telemetry/ep_download_tracker.h"
#include "telemetry/device_id.h"
#include "telemetry/telemetry_context.h"
#include "telemetry/telemetry_event_properties_sanitizer.h"
#include "telemetry/telemetry_environment.h"
#include "telemetry/telemetry_logger.h"
#include "telemetry/telemetry_metadata.h"
#include "telemetry/one_ds_telemetry.h"
#include "telemetry/telemetry_redaction.h"
#include "telemetry/telemetry_request_metrics.h"
#include "telemetry/telemetry_sampling.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
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
#ifdef _WIN32
    ::SetLastError(ERROR_SUCCESS);
    const auto needed = ::GetEnvironmentVariableA(name, nullptr, 0);
    had_original_ = needed != 0 || ::GetLastError() != ERROR_ENVVAR_NOT_FOUND;
    original_ = TelemetryEnvironment::GetEnv(name);
    ::SetEnvironmentVariableA(name, value);
#else
    if (const auto* original = std::getenv(name); original != nullptr) {
      had_original_ = true;
      original_ = original;
    }

    if (value == nullptr) {
      unsetenv(name);
    } else {
      setenv(name, value, 1);
    }
#endif
  }

  ScopedEnvVar(const ScopedEnvVar&) = delete;
  ScopedEnvVar& operator=(const ScopedEnvVar&) = delete;

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

  void RecordEpDownloadAndRegister(const EpDownloadAndRegisterInfo& info) override { ep_calls.push_back(info); }

  void RecordDownload(const DownloadInfo& info) override { download_calls.push_back(info); }

  void RecordCatalogFetch(const CatalogFetchInfo&) override {}

  std::vector<ActionCall> action_calls;
  std::vector<std::pair<Action, std::string>> exception_calls;
  std::vector<DownloadInfo> download_calls;
  std::vector<EpDownloadAndRegisterInfo> ep_calls;
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

TEST(TelemetryEnvironmentTest, ClassifiesContainersAndVirtualMachines) {
  using TelemetryInternal::ClassifyHostEnvironment;
  using TelemetryInternal::HostEnvironmentEvidence;

  {
    HostEnvironmentEvidence evidence;
    evidence.kubernetes = true;
    const auto info = ClassifyHostEnvironment(evidence);
    EXPECT_TRUE(info.is_container);
    EXPECT_STREQ(info.container_type, "kubernetes");
    EXPECT_STREQ(info.environment_class, "container");
    EXPECT_STREQ(info.detection_confidence, "high");
    EXPECT_STREQ(info.device_id_scope, "container");
  }
  {
    HostEnvironmentEvidence evidence;
    evidence.podman_marker = true;
    evidence.dmi = "Amazon EC2";
    const auto info = ClassifyHostEnvironment(evidence);
    EXPECT_TRUE(info.is_container);
    EXPECT_TRUE(info.is_virtual_machine);
    EXPECT_STREQ(info.container_type, "podman");
    EXPECT_STREQ(info.virtualization_type, "amazonEC2");
    EXPECT_STREQ(info.environment_class, "containerOnVirtualMachine");
  }
  {
    HostEnvironmentEvidence evidence;
    evidence.dmi = "Microsoft Corporation Virtual Machine";
    const auto info = ClassifyHostEnvironment(evidence);
    EXPECT_TRUE(info.is_virtual_machine);
    EXPECT_STREQ(info.virtualization_type, "hyperV");
    EXPECT_STREQ(info.environment_class, "virtualMachine");
    EXPECT_STREQ(info.device_id_scope, "virtualMachine");
  }
  {
    HostEnvironmentEvidence evidence;
    evidence.kernel_release = "6.6.87.2-microsoft-standard-WSL2";
    const auto info = ClassifyHostEnvironment(evidence);
    EXPECT_TRUE(info.is_virtual_machine);
    EXPECT_STREQ(info.virtualization_type, "wsl");
  }
}

TEST(TelemetryEnvironmentTest, ClassifiesEmulatorAndUndetectedHostWithoutClaimingPhysicalDevice) {
  TelemetryInternal::HostEnvironmentEvidence evidence;
  evidence.android_emulator = true;
  const auto emulator = TelemetryInternal::ClassifyHostEnvironment(evidence);
  EXPECT_TRUE(emulator.is_virtual_machine);
  EXPECT_TRUE(emulator.is_emulator);
  EXPECT_STREQ(emulator.virtualization_type, "androidEmulator");
  EXPECT_STREQ(emulator.environment_class, "emulator");

  const auto undetected =
      TelemetryInternal::ClassifyHostEnvironment(TelemetryInternal::HostEnvironmentEvidence{});
  EXPECT_FALSE(undetected.is_container);
  EXPECT_FALSE(undetected.is_virtual_machine);
  EXPECT_FALSE(undetected.is_emulator);
  EXPECT_STREQ(undetected.environment_class, "undetected");
  EXPECT_STREQ(undetected.detection_confidence, "none");
  EXPECT_STREQ(undetected.device_id_scope, "installation");
}

TEST(OneDsTelemetryTest, DisableNonessentialTelemetrySuppressesUpload) {
  constexpr std::array<const char*, 14> environment_variables = {
      "ORT_TELEMETRY_DISABLED",
      "CI",
      "TF_BUILD",
      "GITHUB_ACTIONS",
      "GITLAB_CI",
      "CIRCLECI",
      "TRAVIS",
      "JENKINS_URL",
      "CODEBUILD_BUILD_ID",
      "BUILDKITE",
      "TEAMCITY_VERSION",
      "APPVEYOR",
      "BITBUCKET_BUILD_NUMBER",
      "SYSTEM_TEAMFOUNDATIONCOLLECTIONURI",
  };
  std::vector<std::unique_ptr<ScopedEnvVar>> unset_variables;
  unset_variables.reserve(environment_variables.size());
  for (const auto* name : environment_variables) {
    unset_variables.push_back(std::make_unique<ScopedEnvVar>(name, nullptr));
  }

  ASSERT_FALSE(TelemetryEnvironment::IsTelemetryDisabledByEnvVar());
  ASSERT_FALSE(TelemetryEnvironment::IsCiEnvironment());

  RecordingLogger logger;
  OneDsTelemetry telemetry("TestApp", logger, /*disable_nonessential_telemetry=*/true);

  constexpr std::string_view expected_diagnostic =
      "[Telemetry] Disabled via configuration; non-essential 1DS upload disabled (ProcessInfo still uploads)";
  const auto diagnostic =
      std::find_if(logger.entries.begin(), logger.entries.end(), [expected_diagnostic](const auto& entry) {
        return entry.level == LogLevel::Information && entry.message == expected_diagnostic;
      });
  EXPECT_NE(diagnostic, logger.entries.end());
  EXPECT_FALSE(telemetry.IsUploadEnabled());
}

TEST(OneDsTelemetryTest, OrtEnvironmentVariableSuppressesOnlyNonessentialUpload) {
  constexpr std::array<const char*, 13> ci_environment_variables = {
      "CI",
      "TF_BUILD",
      "GITHUB_ACTIONS",
      "GITLAB_CI",
      "CIRCLECI",
      "TRAVIS",
      "JENKINS_URL",
      "CODEBUILD_BUILD_ID",
      "BUILDKITE",
      "TEAMCITY_VERSION",
      "APPVEYOR",
      "BITBUCKET_BUILD_NUMBER",
      "SYSTEM_TEAMFOUNDATIONCOLLECTIONURI",
  };
  std::vector<std::unique_ptr<ScopedEnvVar>> unset_variables;
  for (const auto* name : ci_environment_variables) {
    unset_variables.push_back(std::make_unique<ScopedEnvVar>(name, nullptr));
  }
  ScopedEnvVar disabled("ORT_TELEMETRY_DISABLED", "true");

  RecordingLogger logger;
  OneDsTelemetry telemetry("TestApp", logger);

  constexpr std::string_view expected_diagnostic =
      "[Telemetry] Disabled via ORT_TELEMETRY_DISABLED; non-essential 1DS upload disabled "
      "(ProcessInfo still uploads)";
  const auto diagnostic =
      std::find_if(logger.entries.begin(), logger.entries.end(), [expected_diagnostic](const auto& entry) {
        return entry.level == LogLevel::Information && entry.message == expected_diagnostic;
      });
  EXPECT_NE(diagnostic, logger.entries.end());
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

TEST(TelemetryActionTest, ClassifiesInternalTimeoutExceptionWithoutChangingPublicErrorCode) {
  TimeoutException timeout(FL_WHERE, "timed out", FOUNDRY_LOCAL_ERROR_NETWORK);
  EXPECT_EQ(timeout.code(), FOUNDRY_LOCAL_ERROR_NETWORK);
  EXPECT_EQ(ActionStatusFromException(timeout), ActionStatus::kTimeout);
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
  info.is_container = true;
  info.is_virtual_machine = true;
  info.container_type = "kubernetes";
  info.virtualization_type = "hyperV";
  info.host_environment = "containerOnVirtualMachine";
  info.environment_detection_confidence = "high";
  info.device_id_scope = "container";
  info.cpu_count = 8;
  info.total_memory_mb = 32768;

  telemetry.RecordProcessInfo(info);

  ASSERT_EQ(logger.entries.size(), 1u);
  EXPECT_NE(logger.entries[0].message.find("ProcessInfo"), std::string::npos);
  EXPECT_NE(logger.entries[0].message.find("AppVersion=4.5.6"), std::string::npos);
  EXPECT_NE(logger.entries[0].message.find("ProcessName=foundry_local_test.exe"), std::string::npos);
  EXPECT_NE(logger.entries[0].message.find("DeviceIdStatus=Existing"), std::string::npos);
  EXPECT_NE(logger.entries[0].message.find("ContainerType=kubernetes"), std::string::npos);
  EXPECT_NE(logger.entries[0].message.find("VirtualizationType=hyperV"), std::string::npos);
  EXPECT_NE(logger.entries[0].message.find("HostEnvironment=containerOnVirtualMachine"), std::string::npos);
  EXPECT_NE(logger.entries[0].message.find("EnvironmentDetectionConfidence=high"), std::string::npos);
  EXPECT_NE(logger.entries[0].message.find("DeviceIdScope=container"), std::string::npos);
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

TEST(TelemetryMetadataTest, HardwareInventoryPreservesExactCountsWithinBudget) {
  const std::map<std::string, std::vector<std::string>> devices{
      {"CPU", {"CPUExecutionProvider"}}, {"GPU", {"CUDAExecutionProvider", "CPUExecutionProvider"}}};
  const auto info = BuildHardwareInfo(devices);
  EXPECT_TRUE(info.has_cpu);
  EXPECT_TRUE(info.has_gpu);
  EXPECT_FALSE(info.has_npu);
  EXPECT_EQ(info.device_type_count, 2);
  EXPECT_EQ(info.execution_provider_count, 2);
  EXPECT_EQ(info.device_types, "CPU,GPU");
  EXPECT_EQ(info.execution_providers, "CPUExecutionProvider,CUDAExecutionProvider");
}

TEST(TelemetryMetadataTest, HardwareInventoryBoundsRepeatedAndOversizedEntries) {
  std::map<std::string, std::vector<std::string>> devices{{"CPU", {}}};
  devices.at("CPU").assign(kMaxTelemetryInventoryEntries, "CPUExecutionProvider");
  EXPECT_EQ(BuildHardwareInfo(devices).execution_provider_count, 1);
  devices.at("CPU").push_back("CPUExecutionProvider");
  auto info = BuildHardwareInfo(devices);
  EXPECT_EQ(info.device_type_count, 1);
  EXPECT_EQ(info.execution_provider_count, -1);
  EXPECT_EQ(info.execution_providers, "[oversized inventory]");

  devices.at("CPU") = {std::string(1024 * 1024, 'x')};
  info = BuildHardwareInfo(devices);
  EXPECT_EQ(info.execution_provider_count, -1);
  EXPECT_EQ(info.execution_providers, "[oversized inventory]");
  EXPECT_EQ(devices.at("CPU").front().size(), 1024u * 1024u);

  devices = {{std::string(1024 * 1024, 'x'), {}}};
  info = BuildHardwareInfo(devices);
  EXPECT_EQ(info.device_type_count, -1);
  EXPECT_EQ(info.device_types, "[oversized inventory]");
  EXPECT_EQ(info.execution_provider_count, 0);
}

TEST(TelemetryMetadataTest, HardwareInventoryBoundsDeviceIterationWithoutLosingKnownClasses) {
  std::map<std::string, std::vector<std::string>> devices;
  for (size_t i = 0; i < kMaxTelemetryInventoryEntries; ++i) {
    devices.emplace(std::to_string(i), std::vector<std::string>{});
  }
  EXPECT_EQ(BuildHardwareInfo(devices).device_type_count, kMaxTelemetryInventoryEntries);

  devices.emplace("CPU", std::vector<std::string>{"CPUExecutionProvider"});
  devices.emplace("GPU", std::vector<std::string>{});
  devices.emplace("NPU", std::vector<std::string>{});
  const auto info = BuildHardwareInfo(devices);
  EXPECT_TRUE(info.has_cpu);
  EXPECT_TRUE(info.has_gpu);
  EXPECT_TRUE(info.has_npu);
  EXPECT_EQ(info.device_type_count, -1);
  EXPECT_EQ(info.execution_provider_count, -1);
  EXPECT_EQ(info.device_types, "[oversized inventory]");
  EXPECT_EQ(info.execution_providers, "[oversized inventory]");
}

TEST(TelemetryMetadataTest, HardwareInventoryBoundsJoinedOutputWithoutChangingExactCounts) {
  const std::map<std::string, std::vector<std::string>> devices{
      {"CPU", {std::string(kMaxTelemetryStringLength, 'a'), std::string(kMaxTelemetryStringLength, 'b')}}};
  const auto info = BuildHardwareInfo(devices);
  EXPECT_EQ(info.execution_provider_count, 2);
  EXPECT_EQ(info.execution_providers, "[oversized inventory]");
}

TEST(TelemetryMetadataTest, HostAppVersionIsAlwaysPopulated) {
  auto metadata = BuildTelemetryMetadata("foundry-local-test");

  EXPECT_FALSE(metadata.app_version.empty());
  EXPECT_FALSE(metadata.version.empty());
}

#ifdef _WIN32
TEST(TelemetryDeviceIdPlatformTest, UsesLocalAppDataForCacheDirectory) {
  ScopedEnvVar local_app_data("LOCALAPPDATA", "C:\\telemetry-cache-test");
  EXPECT_EQ(TelemetryDeviceIdPlatform::GetCacheDirectory(),
            std::filesystem::path("C:\\telemetry-cache-test\\Microsoft\\DeveloperTools\\.onnxruntime"));
  EXPECT_FALSE(TelemetryDeviceIdPlatform::UsesPlatformProvidedId());
}
#elif !defined(__ANDROID__) && !defined(__APPLE__)
TEST(TelemetryDeviceIdPlatformTest, UsesXdgCacheHomeForStorageDirectory) {
  ScopedEnvVar xdg_cache_home("XDG_CACHE_HOME", "/tmp/telemetry-cache-test");
  EXPECT_EQ(TelemetryDeviceIdPlatform::GetStorageDirectory(),
            std::filesystem::path("/tmp/telemetry-cache-test/Microsoft/DeveloperTools/.onnxruntime"));
  EXPECT_FALSE(TelemetryDeviceIdPlatform::UsesPlatformProvidedId());
}

TEST(TelemetryDeviceIdPlatformTest, ConcurrentCorruptionRecoveryUsesOneStableId) {
  const auto root = std::filesystem::temp_directory_path() / ("foundry-device-id-" + GenerateGuidV4());
  ScopedEnvVar xdg_cache_home("XDG_CACHE_HOME", root.string().c_str());
  const auto directory = TelemetryDeviceIdPlatform::EnsureStorageDirectory();
  ASSERT_FALSE(directory.empty());

  {
    std::ofstream corrupt_file(directory / "deviceid");
    corrupt_file << "corrupt";
  }

  std::array<TelemetryDeviceIdPlatform::LoadResult, 4> results;
  std::vector<std::thread> threads;
  for (size_t i = 0; i < results.size(); ++i) {
    threads.emplace_back([&, i] { results[i] = TelemetryDeviceIdPlatform::LoadOrCreate(); });
  }
  for (auto& thread : threads) {
    thread.join();
  }

  ASSERT_TRUE(TelemetryDeviceId::IsValidGuid(results[0].value));
  for (const auto& result : results) {
    EXPECT_EQ(result.value, results[0].value);
  }

  std::error_code error;
  std::filesystem::remove_all(root, error);
}

TEST(TelemetryDeviceIdPlatformTest, ExistingDeviceIdIsHardenedBeforeReuse) {
  const auto root = std::filesystem::temp_directory_path() / ("foundry-device-id-" + GenerateGuidV4());
  ScopedEnvVar xdg_cache_home("XDG_CACHE_HOME", root.string().c_str());
  const auto directory = TelemetryDeviceIdPlatform::EnsureStorageDirectory();
  ASSERT_FALSE(directory.empty());
  const auto file = directory / "deviceid";
  const std::string existing_id = "01234567-89ab-4def-8123-456789abcdef";
  {
    std::ofstream output(file);
    output << existing_id;
  }

  const auto loose_directory_permissions = std::filesystem::perms::owner_all |
                                           std::filesystem::perms::group_read |
                                           std::filesystem::perms::group_exec;
  const auto loose_file_permissions = std::filesystem::perms::owner_read |
                                      std::filesystem::perms::owner_write |
                                      std::filesystem::perms::group_read |
                                      std::filesystem::perms::others_read;
  std::filesystem::permissions(directory, loose_directory_permissions, std::filesystem::perm_options::replace);
  std::filesystem::permissions(file, loose_file_permissions, std::filesystem::perm_options::replace);

  const auto result = TelemetryDeviceIdPlatform::LoadOrCreate();
  EXPECT_EQ(result.value, existing_id);
  EXPECT_EQ(result.status, TelemetryDeviceIdStatus::kExisting);
  const auto directory_permissions = std::filesystem::status(directory).permissions();
  const auto file_permissions = std::filesystem::status(file).permissions();
  EXPECT_EQ(directory_permissions & (std::filesystem::perms::group_all | std::filesystem::perms::others_all),
            std::filesystem::perms::none);
  EXPECT_EQ(file_permissions & (std::filesystem::perms::group_all | std::filesystem::perms::others_all),
            std::filesystem::perms::none);

  std::error_code error;
  std::filesystem::remove_all(root, error);
}
#endif

#ifdef _WIN32
TEST(TelemetryMetadataTest, ProcessNamePreservesExecutableExtensionOnWindows) {
  auto info = BuildProcessInfo(BuildTelemetryMetadata("foundry-local-test"), /*include_device_id_status=*/false);

  EXPECT_TRUE(info.process_name.ends_with(".exe")) << info.process_name;
}
#endif

TEST(TelemetryGuidTest, GeneratesRfc4122VersionFourValues) {
  const auto first = GenerateGuidV4();
  const auto second = GenerateGuidV4();

  ASSERT_EQ(first.size(), 36u);
  EXPECT_EQ(first[8], '-');
  EXPECT_EQ(first[13], '-');
  EXPECT_EQ(first[18], '-');
  EXPECT_EQ(first[23], '-');
  EXPECT_EQ(first[14], '4');
  EXPECT_NE(std::string_view{"89ab"}.find(first[19]), std::string_view::npos);
  EXPECT_NE(first, second);
}

TEST(TelemetryContextTest, SuppressesUnneededCommonContextWithoutChangingExplicitIdentity) {
  struct RecordingContext {
    std::map<std::string, std::string> fields;
    void SetCommonField(const std::string& name, const std::string& value) { fields[name] = value; }
  } context;
  context.fields["AppInfo.Id"] = "application-id";
  context.fields["DeviceInfo.Id"] = "device-id";

  TelemetryInternal::SuppressUnneededCommonContext(context);
  TelemetryInternal::SetApplicationNameFromProcessName(context, "foundry_local_test");

  ASSERT_EQ(context.fields.size(), 7u);
  EXPECT_EQ(context.fields.at("AppInfo.Id"), "application-id");
  EXPECT_EQ(context.fields.at("AppInfo.Name"), "foundry_local_test");
  EXPECT_EQ(context.fields.at("DeviceInfo.Id"), "device-id");
  for (const auto* field : TelemetryInternal::kSuppressedCommonContextFields) {
    EXPECT_TRUE(context.fields.at(field).empty()) << field;
  }
}

TEST(TelemetryDeviceIdTest, ValidatesGuidShapeAndHashesForUpload) {
  EXPECT_TRUE(TelemetryDeviceId::IsValidGuid("01234567-89ab-4def-8123-456789abcdef"));
  EXPECT_FALSE(TelemetryDeviceId::IsValidGuid("0123456789ab4def8123456789abcdef"));
  EXPECT_FALSE(TelemetryDeviceId::IsValidGuid("zzzzzzzz-89ab-4def-8123-456789abcdef"));

  auto hashed = TelemetryDeviceId::HashForTelemetry("01234567-89ab-4def-8123-456789abcdef");
  EXPECT_EQ(hashed, "c:6225BD190D6CCF87766A49C9986D174DEF3391FE175A61525E49A1D2334D6A43");
}

TEST(TelemetryRedactionTest, MatchesOnnxRuntimePathAnchors) {
  EXPECT_EQ(ScrubStringForTelemetry(""), "");
  EXPECT_EQ(ScrubStringForTelemetry("no path here"), "no path here");
  EXPECT_EQ(ScrubStringForTelemetry("/home/alice/model.onnx"), "[path]");
  EXPECT_EQ(ScrubStringForTelemetry("~/.config/app/x"), "[path]");
  EXPECT_EQ(ScrubStringForTelemetry("Load C:\\Users\\First Last\\model.onnx failed"), "Load [path]");
  EXPECT_EQ(ScrubStringForTelemetry("from \\\\server\\share\\dir\\weights.bin done"), "from [path]");
  EXPECT_EQ(ScrubStringForTelemetry("alice/models/phi3.onnx"), "[path]");
  EXPECT_EQ(ScrubStringForTelemetry("Users\\alice\\model.onnx"), "[path]");
}

TEST(TelemetryRedactionTest, PreservesSingleSeparatorTokens) {
  EXPECT_EQ(ScrubStringForTelemetry("/secret"), "/secret");
  EXPECT_EQ(ScrubStringForTelemetry("models/foo.onnx"), "models/foo.onnx");
  EXPECT_EQ(ScrubStringForTelemetry("ratio 3/4 and/or"), "ratio 3/4 and/or");
  EXPECT_EQ(ScrubStringForTelemetry("domain\\user"), "domain\\user");
  EXPECT_EQ(ScrubStringForTelemetry("read\\write access"), "read\\write access");
}
TEST(TelemetryRedactionTest, CapsAsciiAndMultibyteStringsAtUtf8Boundary) {
  const std::string long_msg(kMaxTelemetryStringLength + 100, 'x');
  EXPECT_EQ(ScrubStringForTelemetry(long_msg).size(), kMaxTelemetryStringLength);

  const auto path_after_limit = std::string(kMaxTelemetryStringLength - 5, 'x') + "/profiles/sample-user/model.onnx";
  const auto scrubbed_path_after_limit = ScrubStringForTelemetry(path_after_limit);
  EXPECT_EQ(scrubbed_path_after_limit.find("sample-user"), std::string::npos);
  EXPECT_LE(scrubbed_path_after_limit.size(), kMaxTelemetryStringLength);

  const std::string crossing_tail = " sample-user/models";
  const std::string crossing_padding(kMaxTelemetryStringLength - crossing_tail.size(), 'x');
  EXPECT_EQ(ScrubStringForTelemetry(crossing_padding + crossing_tail + "/private"),
            crossing_padding + " [path]");

  const std::string euro = "\xE2\x82\xAC";
  std::string long_utf8;
  long_utf8.reserve(kMaxTelemetryStringLength + euro.size());
  while (long_utf8.size() <= kMaxTelemetryStringLength) {
    long_utf8 += euro;
  }

  const auto scrubbed_utf8 = ScrubStringForTelemetry(long_utf8);
  EXPECT_EQ(scrubbed_utf8.size(), kMaxTelemetryStringLength - 1);
  EXPECT_EQ(scrubbed_utf8.size() % euro.size(), 0);

  const std::string exact_boundary = std::string(kMaxTelemetryStringLength - euro.size(), 'x') + euro;
  EXPECT_EQ(ScrubStringForTelemetry(exact_boundary), exact_boundary);
}

TEST(TelemetryRedactionTest, EnforcesOneKiBAtEveryMultibyteBoundary) {
  ASSERT_EQ(kMaxTelemetryStringLength, 1024u);
  const std::string codepoints[] = {"\xC2\xA2", "\xE2\x82\xAC", "\xF0\x9F\x98\x80"};
  for (const auto& codepoint : codepoints) {
    for (size_t offset = 1; offset < codepoint.size(); ++offset) {
      const std::string prefix(kMaxTelemetryStringLength - offset, 'x');
      const auto input = prefix + codepoint;
      EXPECT_EQ(ScrubStringForTelemetry(input), prefix);
      EXPECT_EQ(TelemetryInternal::SanitizeCommonContextValue(input), prefix);
    }
    const auto exact = std::string(kMaxTelemetryStringLength - codepoint.size(), 'x') + codepoint;
    EXPECT_EQ(TelemetryInternal::SanitizeCommonContextValue(exact), exact);
    EXPECT_EQ(ScrubStringForTelemetry(exact + "x"), exact);
  }
}

TEST(TelemetryRedactionTest, ReplacesMalformedUtf8WithoutExceedingTheByteLimit) {
  const std::string malformed[] = {
      "\x80", "\xC0\xAF", "\xED\xA0\x80", "\xF4\x90\x80\x80", "\xE2\x82", "\xF0\x9F\x98", "\xFF"};
  for (const auto& value : malformed) {
    const std::string expected(value.size(), '?');
    EXPECT_EQ(ScrubStringForTelemetry(value), expected);
    EXPECT_EQ(TelemetryInternal::SanitizeTelemetryValue(value), expected);
    EXPECT_EQ(ScrubStringForTelemetry(std::string(kMaxTelemetryStringLength - 1, 'x') + value),
              std::string(kMaxTelemetryStringLength - 1, 'x') + "?");
  }
  EXPECT_EQ(ScrubStringForTelemetry("valid \xE2\x82\xAC then \xFF"), "valid \xE2\x82\xAC then ?");
  EXPECT_EQ(ScrubStringForTelemetry("\xE2x"), "?x");
}

TEST(TelemetryRedactionTest, BoundsInspectionAndFailsClosedForUninspectableSuffixes) {
  const std::string huge(1024 * 1024, 'x');
  EXPECT_EQ(ScrubStringForTelemetry(huge), "[oversized]");
  EXPECT_EQ(TelemetryInternal::SanitizeCommonContextValue(huge + "/private/path"), "[oversized]");
  EXPECT_EQ(TelemetryInternal::SanitizeCommonContextValue(huge + " token=private"), "[oversized]");
  EXPECT_EQ(TelemetryInternal::SanitizeCommonContextValue(huge + " https://private.invalid"), "[oversized]");

  const std::string prefix(kMaxTelemetryStringLength - 20, 'x');
  EXPECT_EQ(TelemetryInternal::SanitizeCommonContextValue(prefix + " sample-user/models/private"),
            prefix + " [path]");
  EXPECT_EQ(TelemetryInternal::SanitizeCommonContextValue(prefix + " sample-user/models/private token=value"),
            prefix + " [path]");
  EXPECT_EQ(TelemetryInternal::SanitizeCommonContextValue(prefix + " token=private"),
            prefix + " token=[secret]");
  EXPECT_EQ(TelemetryInternal::SanitizeCommonContextValue(prefix + " https://private.invalid"),
            prefix + " [url]");

  std::array<char, kMaxTelemetryInspectionLength + 1> unterminated{};
  unterminated.fill('x');
  const auto bounded = BoundedTelemetryCString(unterminated.data());
  EXPECT_EQ(bounded.size(), unterminated.size());
  EXPECT_EQ(TelemetryInternal::SanitizeTelemetryValue(bounded), "[oversized]");
  EXPECT_TRUE(BoundedTelemetryCString(nullptr).empty());
}

TEST(OneDsTelemetryTest, BoundsEveryStringDimensionBeforeEventPropertyCopies) {
  using namespace ::Microsoft::Applications::Events;
  TelemetryInternal::BoundedEventProperties event("Limits");
  const std::string input(kMaxTelemetryStringLength + 1, 'x');
  const char* dimensions[] = {
      "Action",
      "Status",
      "UserAgent",
      "AudioSource",
      "Language",
      "InitReadyState",
      "DownloadReadyState",
      "RegisterReadyState",
      "DownloadWaitResult",
      "Operation",
      "Endpoint",
      "Region",
      "Format",
      "ExceptionMessage",
      "ErrorMessage",
      "appVersion",
      "appName",
      "osName",
      "osVersion",
      "architecture",
      "processName",
      "DeviceInfo.Status",
      "containerType",
      "virtualizationType",
      "hostEnvironment",
      "environmentDetectionConfidence",
      "deviceIdScope",
      "DeviceTypes",
      "ExecutionProviders",
  };
  for (const char* dimension : dimensions) {
    event.SetProperty(dimension, input);
    EXPECT_EQ(std::string_view(event.GetProperties(DataCategory_PartC).at(dimension).as_string).size(), 1024u)
        << dimension;
  }

  const char* identifiers[] = {"ModelId", "ExecutionProvider", "CorrelationId", "ProviderName", "AppSessionGuid"};
  for (const char* identifier : identifiers) {
    event.SetProperty(identifier, input);
    EXPECT_STREQ(event.GetProperties(DataCategory_PartC).at(identifier).as_string, "[oversized]");
    event.SetProperty(identifier, "valid-id");
    EXPECT_STREQ(event.GetProperties(DataCategory_PartC).at(identifier).as_string, "valid-id");
  }

  event.SetProperty("UserAgent", input.c_str());
  event.SetProperty("TotalTokens", int64_t{42});
  event.SetProperty("Stream", true);
  EXPECT_EQ(std::string_view(event.GetProperties(DataCategory_PartC).at("UserAgent").as_string).size(), 1024u);
  EXPECT_EQ(event.GetProperties(DataCategory_PartC).at("TotalTokens").as_int64, 42);
  EXPECT_TRUE(event.GetProperties(DataCategory_PartC).at("Stream").as_bool);
  event.SetProperty("TotalTokens", int64_t{0});
  event.SetProperty("Stream", false);
  EXPECT_EQ(event.GetProperties(DataCategory_PartC).at("TotalTokens").as_int64, 0);
  EXPECT_FALSE(event.GetProperties(DataCategory_PartC).at("Stream").as_bool);
  std::vector<std::string> values(TelemetryInternal::kMaxTelemetryStringArrayElements + 1, input);
  event.SetProperty("Metadata", values, PiiKind_GenericData);
  const auto& array = event.GetProperties(DataCategory_PartC).at("Metadata");
  ASSERT_NE(array.as_stringArray, nullptr);
  ASSERT_EQ(array.as_stringArray->size(), TelemetryInternal::kMaxTelemetryStringArrayElements);
  EXPECT_EQ(array.as_stringArray->front().size(), 1024u);
  EXPECT_EQ(array.as_stringArray->back(), "[oversized array]");
  EXPECT_EQ(array.piiKind, PiiKind_GenericData);
  EXPECT_EQ(input.size(), 1025u);
}

TEST(OneDsTelemetryTest, FinalSanitizerBoundsArrayCountAndOversizedIdentifiers) {
  using namespace ::Microsoft::Applications::Events;
  EventProperties event("Limits");
  const std::string huge(1024 * 1024, 'x');
  event.SetProperty("ModelId", std::string(kMaxTelemetryStringLength + 1, 'i'), PiiKind_GenericData);
  event.SetProperty("Huge", huge);
  std::vector<std::string> values(TelemetryInternal::kMaxTelemetryStringArrayElements + 1,
                                  std::string(kMaxTelemetryInspectionLength + 1, 'x'));
  event.SetProperty("Metadata", values, PiiKind_GenericData);
  TelemetryInternal::SanitizeEventProperties(event);

  const auto& properties = event.GetProperties(DataCategory_PartC);
  EXPECT_STREQ(properties.at("ModelId").as_string, "[oversized]");
  EXPECT_STREQ(properties.at("Huge").as_string, "[oversized]");
  const auto& array = properties.at("Metadata");
  ASSERT_NE(array.as_stringArray, nullptr);
  ASSERT_EQ(array.as_stringArray->size(), TelemetryInternal::kMaxTelemetryStringArrayElements);
  EXPECT_EQ(array.as_stringArray->front(), "[oversized]");
  EXPECT_EQ(array.as_stringArray->back(), "[oversized array]");
  EXPECT_EQ(array.piiKind, PiiKind_GenericData);
}

TEST(OneDsTelemetryTest, EventPropertiesSanitizerRedactsNonErrorStringsWithoutChangingSchema) {
  using namespace ::Microsoft::Applications::Events;

  EventProperties event("Model");
  event.SetProperty("ModelId", "metadata from /profiles/sample-user/model.onnx", PiiKind_GenericData,
                    DataCategory_PartB);
  event.SetProperty("ExecutionProvider", "CPUExecutionProvider");
  event.SetProperty("auth.token", "placeholder");
  event.SetProperty("RequestError", "failed at https://account@example.invalid?query=value");
  event.SetProperty("SimplePath", "failed at /private");
  event.SetProperty("TotalTokens", int64_t{42});

  const auto before = event.GetProperties(DataCategory_PartC);
  TelemetryInternal::SanitizeEventProperties(event);
  const auto& after = event.GetProperties(DataCategory_PartC);

  EXPECT_EQ(event.GetName(), "Model");
  ASSERT_EQ(after.size(), before.size());
  for (const auto& [name, property] : before) {
    const auto sanitized = after.find(name);
    ASSERT_NE(sanitized, after.end());
    EXPECT_EQ(sanitized->second.type, property.type);
  }

  EXPECT_STREQ(after.at("ModelId").as_string, "metadata from [path]");
  EXPECT_STREQ(after.at("ExecutionProvider").as_string, "CPUExecutionProvider");
  EXPECT_STREQ(after.at("auth.token").as_string, "[secret]");
  EXPECT_STREQ(after.at("RequestError").as_string, "failed at [url]");
  EXPECT_STREQ(after.at("SimplePath").as_string, "failed at [path]");
  EXPECT_EQ(after.at("ModelId").piiKind, PiiKind_GenericData);
  EXPECT_EQ(after.at("ModelId").dataCategory, DataCategory_PartB);
  EXPECT_EQ(after.at("TotalTokens").as_int64, 42);
}

TEST(OneDsTelemetryTest, CommonContextSanitizerRedactsUrlsAndSingleComponentPaths) {
  EXPECT_EQ(TelemetryInternal::SanitizeCommonContextValue("app https://demo:placeholder@example.invalid?sig=x"),
            "app [url]");
  EXPECT_EQ(TelemetryInternal::SanitizeCommonContextValue("app /private"), "app [path]");
  EXPECT_EQ(TelemetryInternal::SanitizeCommonContextValue("apiKey:private-value"), "apiKey:[secret]");
  EXPECT_EQ(TelemetryInternal::SanitizeCommonContextValue("foundry-local"), "foundry-local");
  EXPECT_EQ(TelemetryInternal::SanitizeCommonContextValue(std::string(kMaxTelemetryStringLength + 1, 'x')).size(),
            kMaxTelemetryStringLength);
}

TEST(OneDsTelemetryTest, EventPropertiesSanitizerRedactsEmbeddedCredentials) {
  using namespace ::Microsoft::Applications::Events;

  EventProperties event("Error");
  event.SetProperty("ExceptionMessage", "request failed: token=placeholder; endpoint unavailable");
  std::vector<std::string> metadata{"trace id: 42; apiKey : placeholder"};
  event.SetProperty("Metadata", metadata);

  TelemetryInternal::SanitizeEventProperties(event);
  const auto& properties = event.GetProperties(DataCategory_PartC);
  EXPECT_STREQ(properties.at("ExceptionMessage").as_string, "request failed: token=[secret]");
  ASSERT_NE(properties.at("Metadata").as_stringArray, nullptr);
  EXPECT_EQ(properties.at("Metadata").as_stringArray->at(0), "trace id: 42; apiKey :[secret]");
  EXPECT_EQ(TelemetryInternal::SanitizeCommonContextValue("application: password=placeholder"),
            "application: password=[secret]");
}

TEST(OneDsTelemetryTest, EventPropertiesSanitizerCapsEveryStringValue) {
  using namespace ::Microsoft::Applications::Events;

  EventProperties event("CatalogFetch");
  event.SetProperty("Endpoint", "catalog endpoint");
  event.SetProperty("Ascii", std::string(kMaxTelemetryStringLength + 100, 'x'));
  const std::string euro = "\xE2\x82\xAC";
  std::string long_utf8;
  while (long_utf8.size() <= kMaxTelemetryStringLength) {
    long_utf8 += euro;
  }
  event.SetProperty("Utf8", long_utf8);

  TelemetryInternal::SanitizeEventProperties(event);
  const auto& properties = event.GetProperties(DataCategory_PartC);

  EXPECT_STREQ(properties.at("Endpoint").as_string, "catalog endpoint");
  EXPECT_EQ(std::string_view(properties.at("Ascii").as_string).size(), kMaxTelemetryStringLength);
  EXPECT_EQ(std::string_view(properties.at("Utf8").as_string).size(), kMaxTelemetryStringLength - 1);
}

TEST(OneDsTelemetryTest, EventPropertiesSanitizerRecursesIntoStringArraysInOrder) {
  using namespace ::Microsoft::Applications::Events;

  EventProperties event("Metadata");
  std::vector<std::string> aliases = {
      "microsoft/phi-3-mini",
      "load /profiles/sample-user/config.json",
      "models/foo.onnx",
      "/private",
      std::string(kMaxTelemetryStringLength + 1, 'z'),
  };
  event.SetProperty("Aliases", aliases, PiiKind_GenericData);

  TelemetryInternal::SanitizeEventProperties(event);
  const auto& property = event.GetProperties(DataCategory_PartC).at("Aliases");

  ASSERT_EQ(property.type, EventProperty::TYPE_STRING_ARRAY);
  ASSERT_NE(property.as_stringArray, nullptr);
  ASSERT_EQ(property.as_stringArray->size(), aliases.size());
  EXPECT_EQ(property.as_stringArray->at(0), "microsoft/phi-3-mini");
  EXPECT_EQ(property.as_stringArray->at(1), "load [path]");
  EXPECT_EQ(property.as_stringArray->at(2), "models/foo.onnx");
  EXPECT_EQ(property.as_stringArray->at(3), "[path]");
  EXPECT_EQ(property.as_stringArray->at(4).size(), kMaxTelemetryStringLength);
  EXPECT_EQ(property.piiKind, PiiKind_GenericData);
}

TEST(OneDsTelemetryTest, EventPropertiesSanitizerPreservesSecretPropertyProtection) {
  using namespace ::Microsoft::Applications::Events;

  EventProperties event("ProviderMetadata");
  std::vector<std::string> provider_options = {
      "device_id:0", "cache_dir:/profiles/sample-user/cache", "dbPassword:placeholder",
      "apiKey=another-placeholder", " Authorization : Bearer private-value"};
  std::vector<std::string> secret_placeholders = {"first-placeholder", "second-placeholder"};
  event.SetProperty("ProviderOptions", provider_options, PiiKind_GenericData);
  event.SetProperty("clientApiKey", secret_placeholders);
  event.SetProperty("ProviderMetadata", "access_token=private-value");

  TelemetryInternal::SanitizeEventProperties(event);
  const auto& properties = event.GetProperties(DataCategory_PartC);
  const auto& options = properties.at("ProviderOptions");
  const auto& secret_values = properties.at("clientApiKey");

  ASSERT_NE(options.as_stringArray, nullptr);
  EXPECT_EQ(*options.as_stringArray,
            (std::vector<std::string>{"device_id:0", "cache_dir:[path]", "dbPassword:[secret]",
                                      "apiKey=[secret]", " Authorization :[secret]"}));
  ASSERT_NE(secret_values.as_stringArray, nullptr);
  EXPECT_EQ(*secret_values.as_stringArray, (std::vector<std::string>{"[secret]", "[secret]"}));
  EXPECT_STREQ(properties.at("ProviderMetadata").as_string, "access_token=[secret]");
}
TEST(TelemetrySamplingTest, RetainsAllNonAudioEvents) {
  EXPECT_DOUBLE_EQ(TelemetryInternal::kTelemetrySampleRatePercent, 100.0);
}

TEST(TelemetrySamplingTest, HonorsZeroAndHundredPercentRates) {
  EXPECT_FALSE(TelemetryInternal::ShouldSampleTelemetryEvent("app-session", "corr-1", 0.0));
  EXPECT_TRUE(TelemetryInternal::ShouldSampleTelemetryEvent("app-session", "corr-1", 100.0));
}

TEST(TelemetrySamplingTest, SamplesOnlyCorrelatedAudioEventsAtOnePercent) {
  EXPECT_DOUBLE_EQ(TelemetryInternal::SampleRateForAction("OpenAIAudioTranscribe"), 1.0);
  EXPECT_DOUBLE_EQ(TelemetryInternal::kAudioSampleRatePercent, 1.0);
  EXPECT_DOUBLE_EQ(TelemetryInternal::SampleRateForAction("ModelList"), 100.0);

  bool retained = false;
  bool dropped = false;
  for (int i = 0; i < 100'000 && (!retained || !dropped); ++i) {
    const bool sampled = TelemetryInternal::ShouldSampleTelemetryEvent(
        "app-session", "audio-correlation-" + std::to_string(i),
        TelemetryInternal::SampleRateForAction("OpenAIAudioTranscribe"));
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

TEST(TelemetryContextTest, BoundsRetainedContextsAndDefaultUserAgentWithoutChangingSource) {
  const std::string input(kMaxTelemetryStringLength + 1, 'x');
  SetDefaultUserAgent(input);
  EXPECT_EQ(DefaultUserAgent(), std::string(1024, 'x'));
  EXPECT_EQ(InvocationContext::Direct().user_agent.size(), 1024u);
  EXPECT_EQ(InvocationContext::Direct(input).user_agent.size(), 1024u);

  const InvocationContext source{input, input, false};
  const auto bounded = source.BoundedCopy();
  const auto indirect = source.AsIndirect();
  EXPECT_EQ(bounded.user_agent.size(), 1024u);
  EXPECT_EQ(bounded.correlation_id, "[oversized]");
  EXPECT_FALSE(bounded.indirect);
  EXPECT_EQ(indirect.user_agent.size(), 1024u);
  EXPECT_EQ(indirect.correlation_id, "[oversized]");
  EXPECT_TRUE(indirect.indirect);
  EXPECT_EQ(source.user_agent, input);
  EXPECT_EQ(source.correlation_id, input);
  SetDefaultUserAgent({});
}

TEST(ActionTrackerTest, BoundsRetainedModelContextAndDownloadProviderState) {
  CapturingTelemetry telemetry;
  const std::string input(kMaxTelemetryStringLength + 1, 'x');
  const InvocationContext context{input, input, false};
  {
    ActionTracker action(Action::kModelLoad, telemetry, context);
    action.SetModelId(input);
    EXPECT_EQ(action.Context().user_agent.size(), 1024u);
    EXPECT_EQ(action.Context().correlation_id, "[oversized]");
    DownloadTracker download(input, input, telemetry);
    download.SetDownloadWaitResult(input);
    EpDownloadTracker provider(input, input, input, telemetry);
    provider.Done();
  }

  ASSERT_EQ(telemetry.action_calls.size(), 1u);
  EXPECT_EQ(telemetry.action_calls[0].model_id, "[oversized]");
  EXPECT_EQ(telemetry.action_calls[0].user_agent.size(), 1024u);
  ASSERT_EQ(telemetry.download_calls.size(), 1u);
  EXPECT_EQ(telemetry.download_calls[0].model_id, "[oversized]");
  EXPECT_EQ(telemetry.download_calls[0].user_agent.size(), 1024u);
  EXPECT_EQ(telemetry.download_calls[0].download_wait_result.size(), 1024u);
  ASSERT_EQ(telemetry.ep_calls.size(), 1u);
  EXPECT_EQ(telemetry.ep_calls[0].provider_name, "[oversized]");
  EXPECT_EQ(telemetry.ep_calls[0].correlation_id, "[oversized]");
  EXPECT_EQ(telemetry.ep_calls[0].user_agent.size(), 1024u);
  EXPECT_EQ(context.user_agent, input);
  EXPECT_EQ(context.correlation_id, input);
}

TEST(TelemetryMetadataTest, BoundsMetadataBeforeRetentionAndProcessInfoCopies) {
  const std::string input(kMaxTelemetryStringLength + 1, 'x');
  auto metadata = BuildTelemetryMetadata(input);
  EXPECT_EQ(metadata.app_name.size(), 1024u);
  metadata.app_name = input;
  metadata.app_version = input;
  metadata.os_name = input;
  metadata.os_version = input;
  metadata.cpu_arch = input;
  const auto process = BuildProcessInfo(metadata, false);
  EXPECT_EQ(process.app_name.size(), 1024u);
  EXPECT_EQ(process.app_version.size(), 1024u);
  EXPECT_EQ(process.os_name.size(), 1024u);
  EXPECT_EQ(process.os_version.size(), 1024u);
  EXPECT_EQ(process.cpu_arch.size(), 1024u);
  EXPECT_EQ(metadata.app_name, input);
}

TEST(TelemetryEnvironmentTest, PreservesCompleteFunctionalValuesBeyondEventLimit) {
  const std::string input(kMaxTelemetryStringLength + 100, 'x');
  ScopedEnvVar variable("FOUNDRY_TELEMETRY_LIMIT_TEST", input.c_str());
  EXPECT_EQ(TelemetryEnvironment::GetEnv("FOUNDRY_TELEMETRY_LIMIT_TEST"), input);
  EXPECT_TRUE(TelemetryEnvironment::IsTruthyValue(std::string(kMaxTelemetryEnvironmentLength + 1, ' ')));
}

TEST(TelemetryEnvironmentTest, DistinguishesUnsetAndEmptyFromRejectedValues) {
  ScopedEnvVar variable("FOUNDRY_TELEMETRY_LIMIT_TEST", nullptr);
  auto result = TelemetryEnvironment::TryGetEnv("FOUNDRY_TELEMETRY_LIMIT_TEST");
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(result->empty());
  {
    ScopedEnvVar empty("FOUNDRY_TELEMETRY_LIMIT_TEST", "");
    result = TelemetryEnvironment::TryGetEnv("FOUNDRY_TELEMETRY_LIMIT_TEST");
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->empty());
  }
}

#ifndef _WIN32
// Windows cannot create an ASCII environment value this long; POSIX permits it.
TEST(TelemetryEnvironmentTest, RejectsOversizedFunctionalValuesAndFailsClosedForGatingFlags) {
  const std::string input(kMaxTelemetryEnvironmentLength + 1, 'x');
  ScopedEnvVar variable("FOUNDRY_TELEMETRY_LIMIT_TEST", input.c_str());
  ScopedEnvVar disabled("ORT_TELEMETRY_DISABLED", input.c_str());
  ScopedEnvVar ci("CI", input.c_str());
  EXPECT_TRUE(TelemetryEnvironment::GetEnv("FOUNDRY_TELEMETRY_LIMIT_TEST").empty());
  EXPECT_FALSE(TelemetryEnvironment::TryGetEnv("FOUNDRY_TELEMETRY_LIMIT_TEST").has_value());
  EXPECT_TRUE(TelemetryEnvironment::IsTelemetryDisabledByEnvVar());
  EXPECT_TRUE(TelemetryEnvironment::IsCiEnvironment());
  EXPECT_STREQ(std::getenv("FOUNDRY_TELEMETRY_LIMIT_TEST"), input.c_str());
}

#if !defined(__APPLE__) && !defined(__ANDROID__)
TEST(TelemetryDeviceIdPlatformTest, RejectedCacheOverrideDoesNotFallBackToHome) {
  const std::string input(kMaxTelemetryEnvironmentLength + 1, 'x');
  ScopedEnvVar variable("XDG_CACHE_HOME", input.c_str());
  ScopedEnvVar home("HOME", "/tmp/must-not-use-telemetry-fallback");
  EXPECT_TRUE(TelemetryDeviceIdPlatform::GetStorageDirectory().empty());
  EXPECT_TRUE(TelemetryDeviceIdPlatform::GetCacheDirectory().empty());
}
#endif
#else
TEST(TelemetryEnvironmentTest, WindowsReadPreservesTheExactByteLimit) {
  const std::string input(kMaxTelemetryEnvironmentLength, 'x');
  int calls = 0;
  const auto result = TelemetryInternal::ReadWindowsEnvironment(
      "test", [&](const char*, char* buffer, uint32_t size) -> std::optional<uint32_t> {
        ++calls;
        if (buffer == nullptr) {
          return static_cast<uint32_t>(input.size() + 1);
        }
        EXPECT_EQ(size, input.size() + 1);
        std::copy(input.begin(), input.end(), buffer);
        return static_cast<uint32_t>(input.size());
      });
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, input);
  EXPECT_EQ(calls, 2);
}

TEST(TelemetryEnvironmentTest, WindowsReadRetriesBoundedGrowthAndAcceptsShrinkage) {
  int calls = 0;
  const auto result = TelemetryInternal::ReadWindowsEnvironment(
      "test", [&](const char*, char* buffer, uint32_t size) -> std::optional<uint32_t> {
        ++calls;
        if (calls == 1) {
          EXPECT_EQ(buffer, nullptr);
          return 2;
        }
        if (calls == 2) {
          EXPECT_EQ(size, 2u);
          return 6;
        }
        EXPECT_EQ(size, 6u);
        std::copy_n("true", 4, buffer);
        return 4;
      });
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, "true");
  EXPECT_EQ(calls, 3);
}

TEST(TelemetryEnvironmentTest, WindowsReadRejectsUnstableAndFailedReads) {
  const std::vector<std::vector<std::optional<uint32_t>>> scenarios{
      {std::nullopt},
      {std::numeric_limits<uint32_t>::max()},
      {static_cast<uint32_t>(kMaxTelemetryEnvironmentLength + 2)},
      {2, 0},
      {2, std::nullopt},
      {2, static_cast<uint32_t>(kMaxTelemetryEnvironmentLength + 2)},
      {2, 3, 4, 5},
  };
  for (const auto& scenario : scenarios) {
    size_t calls = 0;
    const auto result = TelemetryInternal::ReadWindowsEnvironment(
        "test", [&](const char*, char*, uint32_t) -> std::optional<uint32_t> {
          return scenario.at(calls++);
        });
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(calls, scenario.size());
  }
}

TEST(TelemetryEnvironmentTest, WindowsReadAcceptsInitiallyUnsetOrEmptyValues) {
  for (uint32_t size : {0u, 1u}) {
    int calls = 0;
    const auto result = TelemetryInternal::ReadWindowsEnvironment(
        "test", [&](const char*, char*, uint32_t) -> std::optional<uint32_t> {
          ++calls;
          return size;
        });
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->empty());
    EXPECT_EQ(calls, 1);
  }
}

TEST(TelemetryEnvironmentTest, AppliesByteLimitToWindowsMultibyteEnvironmentValues) {
  ScopedEnvVar variable("FOUNDRY_TELEMETRY_LIMIT_TEST", nullptr);
  const std::wstring input(20'000, L'\u4e00');
  ASSERT_TRUE(::SetEnvironmentVariableW(L"FOUNDRY_TELEMETRY_LIMIT_TEST", input.c_str()));
  const auto needed = ::GetEnvironmentVariableA("FOUNDRY_TELEMETRY_LIMIT_TEST", nullptr, 0);
  ASSERT_GT(needed, 0u);

  const auto value = TelemetryEnvironment::GetEnv("FOUNDRY_TELEMETRY_LIMIT_TEST");
  if (needed > kMaxTelemetryEnvironmentLength + 1) {
    EXPECT_TRUE(value.empty());
  } else {
    std::vector<char> expected(needed);
    const auto written = ::GetEnvironmentVariableA("FOUNDRY_TELEMETRY_LIMIT_TEST", expected.data(), needed);
    ASSERT_EQ(written + 1, needed);
    EXPECT_EQ(value, std::string(expected.data(), written));
  }
}
#endif

TEST(TelemetryEnvironmentTest, BoundsHostEvidenceBeforeCopyingOrClassification) {
  const std::string huge(1024 * 1024, 'x');
  EXPECT_EQ(TelemetryInternal::ToLowerAscii(huge).size(), 3 * kMaxTelemetryProbeLength);
  TelemetryInternal::HostEnvironmentEvidence evidence;
  evidence.dmi = "VMware " + huge;
  evidence.cpu_info = huge;
  evidence.cgroup = "docker " + huge;
  const auto result = TelemetryInternal::ClassifyHostEnvironment(evidence);
  EXPECT_STREQ(result.container_type, "docker");
  EXPECT_STREQ(result.virtualization_type, "vmware");
}

TEST(TelemetryDeviceIdTest, RejectsInvalidOrOversizedDeviceIdsWithoutHashingThem) {
  EXPECT_TRUE(TelemetryDeviceId::HashForTelemetry("").empty());
  EXPECT_TRUE(TelemetryDeviceId::HashForTelemetry("not-a-guid").empty());
  EXPECT_TRUE(TelemetryDeviceId::HashForTelemetry(std::string(1024 * 1024, 'x')).empty());
}

TEST(TelemetrySamplingTest, BoundsOversizedSamplingInputsWithoutMutatingIdentifiers) {
  const std::string huge(1024 * 1024, 'x');
  EXPECT_EQ(TelemetryInternal::HashSamplingKey("session", huge),
            TelemetryInternal::HashSamplingKey("session", "[oversized]"));
  EXPECT_EQ(TelemetryInternal::ShouldSampleTelemetryEvent("session", huge, 1.0),
            TelemetryInternal::ShouldSampleTelemetryEvent("session", "[oversized]", 1.0));
  EXPECT_EQ(TelemetryInternal::ShouldSampleTelemetryEvent(huge, "event", 1.0),
            TelemetryInternal::ShouldSampleTelemetryEvent("[oversized]", "event", 1.0));
  EXPECT_EQ(huge.size(), 1024u * 1024u);
}

TEST(TelemetryLoggerTest, BoundsEveryEventStringBeforeFormatting) {
  RecordingLogger logger;
  const std::string input(kMaxTelemetryStringLength + 1, 'x');
  TelemetryLogger telemetry(input, logger);
  const InvocationContext context{input, input, false};
  telemetry.RecordAction(Action::kModelLoad, ActionStatus::kSuccess, context, 1, input);
  telemetry.RecordException(Action::kModelLoad, std::runtime_error(input), context);

  ModelUsageInfo model;
  model.model_id = model.execution_provider = model.user_agent = model.correlation_id = input;
  telemetry.RecordModelUsage(model);
  AudioUsageInfo audio;
  audio.model_id = audio.execution_provider = audio.user_agent = audio.correlation_id = input;
  audio.audio_source = audio.language = input;
  telemetry.RecordAudioUsage(audio);
  EpDownloadAttemptInfo attempt;
  attempt.user_agent = attempt.correlation_id = input;
  telemetry.RecordEpDownloadAttempt(attempt);
  EpDownloadAndRegisterInfo provider;
  provider.user_agent = provider.correlation_id = provider.provider_name = input;
  provider.init_ready_state = provider.download_ready_state = provider.register_ready_state = input;
  telemetry.RecordEpDownloadAndRegister(provider);
  DownloadInfo download;
  download.model_id = download.user_agent = download.correlation_id = download.download_wait_result = input;
  telemetry.RecordDownload(download);
  CatalogFetchInfo catalog;
  catalog.operation = catalog.endpoint = catalog.region = catalog.format = input;
  catalog.error_message = catalog.user_agent = catalog.correlation_id = input;
  telemetry.RecordCatalogFetch(catalog);
  ProcessInfo process;
  process.app_name = process.app_version = process.os_name = process.os_version = process.cpu_arch = input;
  process.process_name = process.device_id_status = process.container_type = process.virtualization_type = input;
  process.host_environment = process.environment_detection_confidence = process.device_id_scope = input;
  telemetry.RecordProcessInfo(process);
  HardwareInfo hardware;
  hardware.device_types = hardware.execution_providers = input;
  telemetry.RecordHardwareInfo(hardware);
  telemetry.StartSession();
  telemetry.EndSession();

  ASSERT_EQ(logger.entries.size(), 12u);
  for (const auto& entry : logger.entries) {
    EXPECT_EQ(entry.message.find(input), std::string::npos) << entry.message.substr(0, 64);
    EXPECT_LE(entry.message.size(), 16 * kMaxTelemetryStringLength);
  }
  EXPECT_EQ(model.model_id, input);
  EXPECT_EQ(context.correlation_id, input);
}

TEST(TelemetryRequestMetricsTest, ReadsOnlyCountsFromAlreadyParsedLargeInputs) {
  const auto request = nlohmann::json{
      {"messages", nlohmann::json::array({
                       {{"role", "user"}, {"content", std::string(1024 * 1024, 'x')}},
                       {{"role", "assistant"}, {"content", "reply"}},
                   })}};
  EXPECT_EQ(TelemetryInternal::CountParsedJsonMessages(request), 2u);
  EXPECT_EQ(request.at("messages").at(0).at("content").get_ref<const std::string&>().size(), 1024u * 1024u);
  EXPECT_EQ(TelemetryInternal::CountParsedJsonMessages(nlohmann::json::object()), 0u);
  EXPECT_EQ(TelemetryInternal::CountParsedJsonMessages({{"messages", nlohmann::json::array()}}), 0u);
  EXPECT_FALSE(TelemetryInternal::CountParsedJsonMessages({{"messages", "invalid"}}).has_value());
}