// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "inferencing/execution_provider.h"
#include "inferencing/generative/genai_config.h"
#include "inferencing/generative/tokenizer.h"
#include "logger.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>

// Forward declarations for ORT GenAI types (defined in ort_genai.h)
struct OgaModel;
struct OgaTokenizer;
struct OgaMultiModalProcessor;

namespace fl {

/// A model that has been loaded into the ORT GenAI runtime.
/// Owns the OgaModel, OgaTokenizer, and optional OgaMultiModalProcessor.
/// Non-copyable, non-movable. Owned by ModelLoadManager via std::shared_ptr.
///
/// The instance doubles as the *shared lease entry*: it carries the live-session count together with the
/// mutex and condition variable that signal its release. A ModelSessionLease holds a strong shared_ptr to
/// it, so a lease that outlives the ModelLoadManager still releases correctly against storage it owns a
/// reference to, with no manager back-pointer involved.
class GenAIModelInstance : public std::enable_shared_from_this<GenAIModelInstance> {
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

  /// Access the underlying OGA objects (for future chat generation work).
  OgaModel& GetOgaModel();

  /// Tool calling needs a tokenizer that does not skip special tokens.
  OgaTokenizer& GetOgaTokenizerWithSpecial();

  /// The model's tokenizer, shared across all concurrent sessions of this model. Encode operations are
  /// synchronized internally; callers use it without needing to know it is shared. See fl::Tokenizer.
  fl::Tokenizer& Tokenizer();

  /// Cached EOS token IDs for the tokenizer. Avoids re-fetching from OGA on every Decode() call.
  const std::vector<int32_t>& GetEosTokenIds();

  /// Returns nullptr if the model is not multimodal.
  OgaMultiModalProcessor* GetProcessor();

  /// Get the last-activity timestamp.
  std::chrono::steady_clock::time_point LastActivity() const { return last_activity_; }

  /// Live-session reference counting. Taken and released exclusively through ModelSessionLease;
  /// ModelLoadManager::UnloadModel refuses to unload while the count is > 0 to prevent use-after-free of the
  /// OGA objects.
  ///
  /// Guarded by this instance's own mutex rather than being a bare atomic, so a release can wake
  /// WaitForNoSessions() with no lost-wakeup window and the shutdown drain can block on a condition variable
  /// instead of polling. The mutex and CV live here, on the shared entry, precisely so a lease that outlives
  /// the ModelLoadManager never has to reach back through a manager pointer.
  void AcquireSession() {
    std::lock_guard<std::mutex> lock(session_mu_);
    ++session_ref_count_;
  }

  void ReleaseSession() {
    {
      std::lock_guard<std::mutex> lock(session_mu_);
      if (session_ref_count_ > 0) {
        --session_ref_count_;
      }
    }

    session_idle_cv_.notify_all();
  }

  int SessionRefCount() const {
    std::lock_guard<std::mutex> lock(session_mu_);
    return session_ref_count_;
  }

  /// Block until no lease references this model, or `deadline` passes. Returns true if it drained.
  bool WaitForNoSessions(std::chrono::steady_clock::time_point deadline) const {
    std::unique_lock<std::mutex> lock(session_mu_);
    return session_idle_cv_.wait_until(lock, deadline, [this] { return session_ref_count_ == 0; });
  }

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
  std::chrono::steady_clock::time_point last_activity_;
  mutable std::mutex session_mu_;
  mutable std::condition_variable session_idle_cv_;
  int session_ref_count_ = 0;
};

}  // namespace fl
