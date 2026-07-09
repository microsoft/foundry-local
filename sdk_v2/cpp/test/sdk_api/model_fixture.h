// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
// Shared fixture and helpers for SDK API integration tests.
// All tests use ONLY the public C++ API (foundry_local_cpp.h).
//
// The Manager singleton is owned by SharedTestEnv (a GTest global environment).
// Fixtures just reference the shared state — no per-suite create/destroy.
#pragma once

#include "shared_test_env.h"

#include <foundry_local/foundry_local_cpp.h>
#include <nlohmann/json.hpp>
#include <httplib.h>
#ifdef _WIN32
#undef StartService
#undef GetMessage
#endif
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using json = nlohmann::json;

inline std::string ReadFileContents(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

/// Collect text from a single message's parts, optionally filtering by text type.
/// Pass std::nullopt to include every TEXT part regardless of type.
inline std::string CollectMessageText(const foundry_local::MessageContent& msg,
                                      std::optional<flTextItemType> filter = std::nullopt) {
  std::string text;
  for (const auto& part : msg.parts) {
    if (part.GetType() != FOUNDRY_LOCAL_ITEM_TEXT) {
      continue;
    }
    auto tc = part.GetText();
    if (filter && tc.type != *filter) {
      continue;
    }
    text += tc.text;
  }
  return text;
}

/// Collect all text (visible + reasoning) from TEXT and MESSAGE output items.
inline std::string CollectResponseText(const foundry_local::Response& response) {
  std::string text;

  for (const auto& item : response.GetItems()) {
    if (item.GetType() == FOUNDRY_LOCAL_ITEM_TEXT) {
      text += item.GetText().text;
    } else if (item.GetType() == FOUNDRY_LOCAL_ITEM_MESSAGE) {
      text += CollectMessageText(item.GetMessage());
    } else if (item.GetType() == FOUNDRY_LOCAL_ITEM_SPEECH_RESULT) {
      auto sr = item.GetSpeechResult();
      text.append(sr.text.data(), sr.text.size());
    }
  }

  return text;
}

/// Collect only visible (DEFAULT) text from TEXT and MESSAGE output items.
/// Used by reasoning-model tests to assert the user-visible response excludes <think> content.
inline std::string CollectResponseVisibleText(const foundry_local::Response& response) {
  std::string text;

  for (const auto& item : response.GetItems()) {
    if (item.GetType() == FOUNDRY_LOCAL_ITEM_TEXT) {
      auto tc = item.GetText();
      if (tc.type == FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT) {
        text += tc.text;
      }
    } else if (item.GetType() == FOUNDRY_LOCAL_ITEM_MESSAGE) {
      text += CollectMessageText(item.GetMessage(), FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT);
    }
  }

  return text;
}

/// Collect only REASONING text from TEXT and MESSAGE output items.
inline std::string CollectResponseReasoningText(const foundry_local::Response& response) {
  std::string text;

  for (const auto& item : response.GetItems()) {
    if (item.GetType() == FOUNDRY_LOCAL_ITEM_TEXT) {
      auto tc = item.GetText();
      if (tc.type == FOUNDRY_LOCAL_TEXT_ITEM_TYPE_REASONING) {
        text += tc.text;
      }
    } else if (item.GetType() == FOUNDRY_LOCAL_ITEM_MESSAGE) {
      text += CollectMessageText(item.GetMessage(), FOUNDRY_LOCAL_TEXT_ITEM_TYPE_REASONING);
    }
  }

  return text;
}

// ========================================================================
// ModelFixture — base for chat-using suites.
//
// Acquires the chat model in SetUpTestSuite. Per-test SetUp skips when
// the chat model isn't currently loaded (selection or load failed).
// ========================================================================

class ModelFixture : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    SharedTestEnv::Get().AcquireModels({SharedTestEnv::Modality::Chat});
  }

  void SetUp() override {
    auto& env = SharedTestEnv::Get();
    if (!env.chat_model()) {
      GTEST_SKIP() << "No chat-completion model available";
    }
  }

  // Convenience accessors — delegate to the global environment.
  static foundry_local::Manager& manager() { return *SharedTestEnv::Get().manager(); }
  static foundry_local::ICatalog& catalog() { return SharedTestEnv::Get().catalog(); }
  static foundry_local::ModelList& model_list() { return *SharedTestEnv::Get().model_list(); }
  static foundry_local::IModel& chat_model() { return *SharedTestEnv::Get().chat_model(); }
  static const std::string& model_id() { return SharedTestEnv::Get().chat_model_id(); }
  static const std::string& model_alias() { return SharedTestEnv::Get().chat_model_alias(); }
};

// ========================================================================
// ToolCallFixture — uses a model that supports tool calling.
// May be a different model than chat_model_ if the smallest chat model
// doesn't support tool calling (e.g. qwen3-0.6b vs qwen2.5-0.5b-instruct).
// ========================================================================

class ToolCallFixture : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    SharedTestEnv::Get().AcquireModels({SharedTestEnv::Modality::Tool});
  }

  void SetUp() override {
    auto& env = SharedTestEnv::Get();
    if (!env.tool_calling_model()) {
      GTEST_SKIP() << "No tool-calling model available";
    }
  }

  static foundry_local::IModel& tool_model() {
    return *SharedTestEnv::Get().tool_calling_model();
  }
};

// ========================================================================
// VisionFixture — uses a vision-language-chat model.
// Skips when no vision model is available in the catalog (CI machines
// without large-model storage, or when the catalog server is unreachable).
// ========================================================================

class VisionFixture : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    SharedTestEnv::Get().AcquireModels({SharedTestEnv::Modality::Vision});
  }

  void SetUp() override {
    auto& env = SharedTestEnv::Get();
    if (!env.vision_model()) {
      GTEST_SKIP() << "No vision-language-chat model available";
    }
  }

  static foundry_local::IModel& vision_model() {
    return *SharedTestEnv::Get().vision_model();
  }

  static const std::string& vision_model_id() {
    return SharedTestEnv::Get().vision_model_id();
  }
};

// ========================================================================
// ReasoningFixture — uses a model that supports reasoning (e.g. qwen3) and emits <think>...</think> blocks. Tests
// using this fixture expect that the visible response text excludes reasoning content but may otherwise be indirect
// (the model often summarizes rather than echoes its input). Skips when no reasoning model is available.
// ========================================================================

class ReasoningFixture : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    SharedTestEnv::Get().AcquireModels({SharedTestEnv::Modality::Reasoning});
  }

  void SetUp() override {
    auto& env = SharedTestEnv::Get();
    if (!env.reasoning_model()) {
      GTEST_SKIP() << "No reasoning model available";
    }
  }

  static foundry_local::IModel& reasoning_model() {
    return *SharedTestEnv::Get().reasoning_model();
  }

  static const std::string& reasoning_model_id() {
    return SharedTestEnv::Get().reasoning_model_id();
  }
};
