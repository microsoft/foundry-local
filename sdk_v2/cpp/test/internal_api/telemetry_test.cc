// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "logger.h"
#include "telemetry/telemetry_action_tracker.h"
#include "telemetry/telemetry_logger.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace fl;

namespace {

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
};

class RecordingTelemetry : public ITelemetry {
 public:
  void RecordAction(Action action, ActionStatus status,
                    const std::string& user_agent,
                    bool indirect, int64_t duration_ms) override {
    action_calls.push_back(ActionCall{action, status, user_agent, indirect, duration_ms});
  }

  void RecordException(Action action, const std::exception& exception) override {
    exception_calls.emplace_back(action, exception.what());
  }

  void RecordModelUsage(const ModelUsageInfo& info) override {
    model_usage_calls.push_back(info);
  }

  void RecordModelId(Action action, const std::string& model_id,
                     ActionStatus /*status*/ = ActionStatus::kSuccess,
                     const std::string& /*user_agent*/ = "") override {
    model_id_calls.emplace_back(action, model_id);
  }

  void RecordEpDownloadAttempt(const EpDownloadAttemptInfo& info) override {
    ep_attempt_calls.push_back(info);
  }

  void RecordEpDownloadAndRegister(const EpDownloadAndRegisterInfo& info) override {
    ep_register_calls.push_back(info);
  }

  void RecordDownload(const DownloadInfo& info) override {
    download_calls.push_back(info);
  }

  std::vector<ActionCall> action_calls;
  std::vector<std::pair<Action, std::string>> exception_calls;
  std::vector<ModelUsageInfo> model_usage_calls;
  std::vector<std::pair<Action, std::string>> model_id_calls;
  std::vector<EpDownloadAttemptInfo> ep_attempt_calls;
  std::vector<EpDownloadAndRegisterInfo> ep_register_calls;
  std::vector<DownloadInfo> download_calls;
};

}  // namespace

TEST(TelemetryLoggerTest, RecordActionIncludesConcreteFields) {
  RecordingLogger logger;
  TelemetryLogger telemetry("foundry-local", logger);

  telemetry.RecordAction(Action::kModelDownload, ActionStatus::kSuccess,
                         "cli/1.0", false, 1234);

  ASSERT_EQ(logger.entries.size(), 1u);
  EXPECT_EQ(logger.entries[0].level, LogLevel::Debug);
  EXPECT_NE(logger.entries[0].message.find("AppName=foundry-local"), std::string::npos);
  EXPECT_NE(logger.entries[0].message.find("UserAgent=cli/1.0"), std::string::npos);
  EXPECT_NE(logger.entries[0].message.find("Action=ModelDownload"), std::string::npos);
  EXPECT_NE(logger.entries[0].message.find("Status=Success"), std::string::npos);
  EXPECT_NE(logger.entries[0].message.find("Direct=true"), std::string::npos);
  EXPECT_NE(logger.entries[0].message.find("TimeMs=1234"), std::string::npos);
}

TEST(TelemetryLoggerTest, RecordExceptionAndModelEventsIncludeSpecificValues) {
  RecordingLogger logger;
  TelemetryLogger telemetry("foundry-local", logger);

  telemetry.RecordException(Action::kModelLoad, std::runtime_error("config missing"));

  ModelUsageInfo usage;
  usage.model_id = "phi-3-mini";
  usage.execution_provider = "CPU";
  usage.user_agent = "cli/1.0";
  usage.total_tokens = 31;
  usage.input_token_count = 17;
  usage.total_time_ms = 250;
  telemetry.RecordModelUsage(usage);

  telemetry.RecordModelId(Action::kModelLoad, "phi-3-mini", ActionStatus::kSuccess, "cli/1.0");

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

TEST(ActionTrackerTest, DestructorRecordsFailureByDefaultWithoutModelId) {
  RecordingTelemetry telemetry;

  {
    ActionTracker tracker(Action::kModelDelete, telemetry, "cli/2.0", true);
  }

  ASSERT_EQ(telemetry.action_calls.size(), 1u);
  EXPECT_EQ(telemetry.action_calls[0].action, Action::kModelDelete);
  EXPECT_EQ(telemetry.action_calls[0].status, ActionStatus::kFailure);
  EXPECT_EQ(telemetry.action_calls[0].user_agent, "cli/2.0");
  EXPECT_TRUE(telemetry.action_calls[0].indirect);
  EXPECT_GE(telemetry.action_calls[0].duration_ms, 0);
  EXPECT_TRUE(telemetry.model_id_calls.empty());
}

TEST(ActionTrackerTest, RecordsExceptionSuccessAndModelId) {
  RecordingTelemetry telemetry;

  {
    ActionTracker tracker(Action::kModelLoad, telemetry, "cli/3.0", false);
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

  ASSERT_EQ(telemetry.model_id_calls.size(), 1u);
  EXPECT_EQ(telemetry.model_id_calls[0].first, Action::kModelLoad);
  EXPECT_EQ(telemetry.model_id_calls[0].second, "phi-3-mini");
}