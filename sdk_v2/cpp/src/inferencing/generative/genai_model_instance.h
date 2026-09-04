// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "inferencing/execution_provider.h"
#include "inferencing/generative/genai_config.h"
#include "inferencing/generative/preprocessor.h"
#include "logger.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

// Forward declarations for ORT GenAI types (defined in ort_genai.h)
struct OgaModel;

namespace fl {

/// A model that has been loaded into the ORT GenAI runtime.
/// Owns the OgaModel and its preprocessing resources.
/// Non-copyable, non-movable. Owned by ModelLoadManager via std::unique_ptr.
class GenAIModelInstance {
 public:
  ~GenAIModelInstance();
  GenAIModelInstance(const GenAIModelInstance&) = delete;
  GenAIModelInstance& operator=(const GenAIModelInstance&) = delete;
  GenAIModelInstance(GenAIModelInstance&&) = delete;
  GenAIModelInstance& operator=(GenAIModelInstance&&) = delete;

  const std::string& ModelId() const { return model_id_; }
  const std::string& ModelPath() const { return model_path_; }
  const GenAIConfig& GetGenAIConfig() const { return genai_config_; }
  ExecutionProvider EP() const { return ep_; }
  bool IsMultiModal() const;

  /// Cached tag token IDs and decoded strings for tool/reasoning detection.
  /// Populated once on first access using OGA tokenizer APIs.
  struct TagInfo {
    std::optional<int32_t> bot_id;
    std::optional<int32_t> eot_id;
    std::optional<int32_t> bor_id;
    std::optional<int32_t> eor_id;
    std::string bot_str;
    std::string eot_str;
    std::string bor_str;
    std::string eor_str;
  };
  const TagInfo& GetTagInfo();

  /// Access the underlying OGA objects.
  OgaModel& GetOgaModel();
  Preprocessor& GetPreprocessor();

  /// Get the last-activity timestamp.
  std::chrono::steady_clock::time_point LastActivity() const { return last_activity_; }

  /// Live-session reference counting. Sessions call AcquireSession() on construction and
  /// ReleaseSession() on destruction; ModelLoadManager::UnloadModel refuses to unload
  /// while the count is > 0 to prevent use-after-free of the OGA objects.
  void AcquireSession() { session_ref_count_.fetch_add(1, std::memory_order_acq_rel); }
  void ReleaseSession() { session_ref_count_.fetch_sub(1, std::memory_order_acq_rel); }
  int SessionRefCount() const { return session_ref_count_.load(std::memory_order_acquire); }

 private:
  friend class ModelLoadManager;

  GenAIModelInstance(std::string model_id,
                     std::string effective_model_path,
                     GenAIConfig genai_config,
                     ExecutionProvider resolved_ep,
                     ILogger& logger);

  std::string model_id_;
  std::string model_path_;
  GenAIConfig genai_config_;
  ExecutionProvider ep_;
  std::unique_ptr<OgaModel> oga_model_;
  std::unique_ptr<Preprocessor> preprocessor_;
  TagInfo tag_info_;
  std::once_flag tag_info_init_flag_;
  std::chrono::steady_clock::time_point last_activity_;
  mutable std::atomic<int> session_ref_count_{0};
};

}  // namespace fl
