// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "inferencing/execution_provider.h"
#include "inferencing/generative/genai_config.h"
#include "inferencing/generative/tokenizer.h"
#include "logger.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

// Forward declarations for ORT GenAI types (defined in ort_genai.h)
struct OgaModel;
struct OgaTokenizer;
struct OgaMultiModalProcessor;

namespace fl {

/// A model that has been loaded into the ORT GenAI runtime.
/// Owns the OgaModel, OgaTokenizer, and optional OgaMultiModalProcessor.
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

  /// Cached tag token IDs and their decoded strings for efficient detection.
  /// IDs are used for integer comparison in the decode loop (fast path).
  /// Strings are used by ToolCallContext/Accumulator for text-based processing.
  /// Populated once at first access via OgaTokenizer getters + tokenizer decode.
  /// Naming follows bos/eos/pad convention:
  ///   bot = beginning of tool (call), eot = end of tool (call)
  ///   bor = beginning of reasoning,   eor = end of reasoning
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

  /// Access the underlying OGA objects (for future chat generation work).
  OgaModel& GetOgaModel();
  OgaTokenizer& GetOgaTokenizerWithSpecial();  // For tool calling, we need a tokenizer that does not skip special tokens.

  /// The model's tokenizer, shared across all concurrent sessions of this model. Encode operations are
  /// synchronized internally; callers use it without needing to know it is shared. See fl::Tokenizer.
  fl::Tokenizer& Tokenizer();

  /// Cached EOS token IDs for the tokenizer. Avoids re-fetching from OGA on every Decode() call.
  const std::vector<int32_t>& GetEosTokenIds();

  /// Returns nullptr if the model is not multimodal.
  OgaMultiModalProcessor* GetProcessor();

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
  std::unique_ptr<fl::Tokenizer> tokenizer_;
  std::unique_ptr<OgaTokenizer> tokenizer_with_special_;
  std::unique_ptr<OgaMultiModalProcessor> processor_;  // nullptr if not multimodal
  std::vector<int32_t> eos_token_ids_;                 // cached; populated on first GetEosTokenIds() call
  std::once_flag eos_token_ids_init_flag_;
  TagInfo tag_info_;                                    // cached; populated on first GetTagInfo() call
  std::once_flag tag_info_init_flag_;
  std::chrono::steady_clock::time_point last_activity_;
  mutable std::atomic<int> session_ref_count_{0};
};

}  // namespace fl
