// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// Tests for the bounded retry helper used by registry resolution. Exercises the
// helper in isolation with a counter-based fake so we don't need a live HTTP
// server or real wall-clock delays.
#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <vector>

#include "exception.h"
#include "http/http_client.h"
#include "logger.h"

using namespace fl;
using namespace fl::http;

namespace {

/// Captures log output so tests can assert the helper logged the expected lines.
class RecordingLogger : public ILogger {
 public:
  std::vector<std::pair<LogLevel, std::string>> entries;

  void Log(LogLevel level, std::string_view message) override {
    entries.emplace_back(level, std::string(message));
  }
};

/// Test config: zero base_delay so jitter is also 0, plus an injected sleep_fn.
RetryConfig FastConfig() {
  RetryConfig cfg;
  cfg.max_retries = 3;
  cfg.base_delay = std::chrono::milliseconds(0);
  cfg.max_total = std::chrono::milliseconds(1000);
  return cfg;
}

}  // namespace

TEST(RetryWithBackoffTest, SucceedsImmediately) {
  RecordingLogger logger;
  int call_count = 0;

  auto op = [&]() -> RetryAttempt {
    ++call_count;
    return RetryAttempt{RetryDecision::Success, "ok-body", ""};
  };

  std::string body = RetryWithBackoff(op, FastConfig(), logger);
  EXPECT_EQ(body, "ok-body");
  EXPECT_EQ(call_count, 1);
}

TEST(RetryWithBackoffTest, RetriesTransientThenSucceeds) {
  RecordingLogger logger;
  int call_count = 0;
  std::vector<std::chrono::milliseconds> sleeps;

  auto op = [&]() -> RetryAttempt {
    ++call_count;
    if (call_count < 3) {
      return RetryAttempt{RetryDecision::RetryTransient, "", "503 Service Unavailable"};
    }
    return RetryAttempt{RetryDecision::Success, "finally-ok", ""};
  };

  auto sleep_fn = [&](std::chrono::milliseconds d) { sleeps.push_back(d); };

  std::string body = RetryWithBackoff(op, FastConfig(), logger, sleep_fn);

  EXPECT_EQ(body, "finally-ok");
  EXPECT_EQ(call_count, 3);
  // Slept twice (between attempt 1→2 and 2→3); did not sleep after success.
  EXPECT_EQ(sleeps.size(), 2u);
}

TEST(RetryWithBackoffTest, GivesUpAfterMaxRetries) {
  RecordingLogger logger;
  int call_count = 0;

  auto op = [&]() -> RetryAttempt {
    ++call_count;
    return RetryAttempt{RetryDecision::RetryTransient, "", "503 Service Unavailable"};
  };

  auto noop_sleep = [](std::chrono::milliseconds) {};

  EXPECT_THROW(RetryWithBackoff(op, FastConfig(), logger, noop_sleep), fl::Exception);
  // 1 initial + 3 retries = 4 calls
  EXPECT_EQ(call_count, 4);
}

TEST(RetryWithBackoffTest, ThrowsNetworkCodeOnExhaustion) {
  RecordingLogger logger;
  auto op = []() -> RetryAttempt {
    return RetryAttempt{RetryDecision::RetryTransient, "", "503 Service Unavailable"};
  };
  auto noop_sleep = [](std::chrono::milliseconds) {};

  try {
    RetryWithBackoff(op, FastConfig(), logger, noop_sleep);
    FAIL() << "expected fl::Exception";
  } catch (const fl::Exception& e) {
    EXPECT_EQ(e.code(), FOUNDRY_LOCAL_ERROR_NETWORK);
  }
}

TEST(RetryWithBackoffTest, DoesNotRetryPermanentFailure) {
  RecordingLogger logger;
  int call_count = 0;

  auto op = [&]() -> RetryAttempt {
    ++call_count;
    return RetryAttempt{RetryDecision::FailPermanent, "", "404 Not Found"};
  };

  auto noop_sleep = [](std::chrono::milliseconds) {};

  EXPECT_THROW(RetryWithBackoff(op, FastConfig(), logger, noop_sleep), fl::Exception);
  EXPECT_EQ(call_count, 1);
}

TEST(RetryWithBackoffTest, LogsRetryAndFinalFailureAtExpectedLevels) {
  RecordingLogger logger;
  auto op = []() -> RetryAttempt {
    return RetryAttempt{RetryDecision::RetryTransient, "", "503"};
  };
  auto noop_sleep = [](std::chrono::milliseconds) {};

  EXPECT_THROW(RetryWithBackoff(op, FastConfig(), logger, noop_sleep), fl::Exception);

  // Should log Information for each retry attempt and Warning for the final give-up.
  int info_count = 0;
  int warn_count = 0;
  for (const auto& [level, _msg] : logger.entries) {
    if (level == LogLevel::Information) {
      ++info_count;
    } else if (level == LogLevel::Warning) {
      ++warn_count;
    }
  }
  EXPECT_EQ(info_count, 3) << "Expected one Information line per retry attempt before giving up";
  EXPECT_EQ(warn_count, 1) << "Expected one Warning line on final failure";
}

TEST(RetryWithBackoffTest, RespectsMaxTotalBudget) {
  RecordingLogger logger;
  RetryConfig cfg;
  cfg.max_retries = 5;
  cfg.base_delay = std::chrono::milliseconds(100);
  cfg.max_total = std::chrono::milliseconds(50);  // smaller than first sleep

  std::vector<std::chrono::milliseconds> sleeps;
  auto sleep_fn = [&](std::chrono::milliseconds d) { sleeps.push_back(d); };

  int call_count = 0;
  auto op = [&]() -> RetryAttempt {
    ++call_count;
    return RetryAttempt{RetryDecision::RetryTransient, "", "503"};
  };

  EXPECT_THROW(RetryWithBackoff(op, cfg, logger, sleep_fn), fl::Exception);

  // Each requested sleep must be capped to the remaining budget (≤ 50ms).
  for (auto d : sleeps) {
    EXPECT_LE(d.count(), 50);
  }
}
