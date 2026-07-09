// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "spdlog_logger.h"

#include <spdlog/async.h>
#include <spdlog/sinks/daily_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#ifdef __ANDROID__
#include <spdlog/sinks/android_sink.h>
#endif

#include <filesystem>
#include <vector>

namespace fl {

namespace {

spdlog::level::level_enum ToSpdlogLevel(LogLevel level) {
  switch (level) {
    case LogLevel::Verbose:
      return spdlog::level::trace;
    case LogLevel::Debug:
      return spdlog::level::debug;
    case LogLevel::Information:
      return spdlog::level::info;
    case LogLevel::Warning:
      return spdlog::level::warn;
    case LogLevel::Error:
      return spdlog::level::err;
    case LogLevel::Fatal:
      return spdlog::level::critical;
    default:
      return spdlog::level::info;
  }
}

}  // namespace

SpdlogLogger::SpdlogLogger(LogLevel min_level, const std::string& logs_dir) {
  // Use a logger-owned async thread pool so logger lifetime is independent of
  // spdlog's global thread-pool teardown order.
  thread_pool_ = std::make_shared<spdlog::details::thread_pool>(8192, 1);

  std::vector<spdlog::sink_ptr> sinks;

#ifdef __ANDROID__
  // On Android, route logs to logcat (stderr is not visible).
  auto console_sink = std::make_shared<spdlog::sinks::android_sink_mt>("foundry_local");
  console_sink->set_pattern("%v");
#else
  // Colored stderr sink for desktop platforms.
  auto console_sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
  console_sink->set_pattern("[%l] %v");
#endif
  sinks.push_back(console_sink);

  // Daily rotating file sink — added when logs_dir is specified
  if (!logs_dir.empty()) {
    std::filesystem::create_directories(logs_dir);

    auto log_path = (std::filesystem::path(logs_dir) / "foundry_local.log").string();
    // Rotate daily at midnight, keep unlimited history (let the caller manage disk)
    auto file_sink = std::make_shared<spdlog::sinks::daily_file_sink_mt>(log_path, 0, 0);
    file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
    sinks.push_back(file_sink);
  }

  logger_ = std::make_shared<spdlog::async_logger>("foundry_local",
                                                   sinks.begin(), sinks.end(),
                                                   thread_pool_,
                                                   spdlog::async_overflow_policy::block);

  logger_->set_level(ToSpdlogLevel(min_level));

  // Flush policy: by default we buffer low-severity logs and only force a flush at warning and
  // above (plus on destruction), avoiding a per-message flush on the hot path. We flush on every
  // message only when running in a debug build, or when the caller has explicitly opted into
  // Debug/Verbose diagnostics -- in those cases prompt, crash-safe output is worth the cost.
#ifndef NDEBUG
  constexpr bool debug_build = true;
#else
  constexpr bool debug_build = false;
#endif

  const bool flush_every_message = debug_build || min_level <= LogLevel::Debug;

  logger_->flush_on(flush_every_message ? spdlog::level::trace : spdlog::level::warn);

  spdlog::register_logger(logger_);
}

SpdlogLogger::~SpdlogLogger() {
  if (logger_) {
    logger_->flush();
    spdlog::drop(logger_->name());
    logger_.reset();
    thread_pool_.reset();
  }
}

void SpdlogLogger::Log(LogLevel level, std::string_view message) {
  logger_->log(ToSpdlogLevel(level), "{}", message);
}

}  // namespace fl
