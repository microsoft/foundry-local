// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
// Global test environment for SDK integration tests.
// Creates the Manager singleton ONCE for the entire test process, avoiding
// the "Manager already created" error when multiple fixture classes inherit
// from a common base that each call SetUpTestSuite/TearDownTestSuite.
#pragma once

#include <foundry_local/foundry_local_cpp.h>
#include <gtest/gtest.h>

#include "internal_api/test_model_cache.h"
#include "utils/safe_getenv.h"
#include "utils/string_utils.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

/// Find the smallest model whose Task() matches the given task string and
/// DeviceType() matches the given device_type.  Checks all variants of each
/// alias group and selects the matching variant when found.
/// Returns nullptr if none found.
inline foundry_local::IModel* FindSmallestModelByTask(foundry_local::ModelList& models, const char* task,
                                                      flDeviceType device_type = FOUNDRY_LOCAL_DEVICE_CPU) {
  foundry_local::IModel* best = nullptr;
  int64_t best_size = std::numeric_limits<int64_t>::max();

  for (const auto& m : models) {
    // Check the selected variant first.
    auto info = m->GetInfo();
    bool selected_matches = (info.DeviceType() == device_type) &&
                            info.Task().has_value() && (*info.Task() == task);

    if (!selected_matches) {
      // The selected variant doesn't match — scan all variants of this alias
      // group to find one that does, and select it.
      bool found_variant = false;
      auto variants = m->GetVariants();
      for (const auto& v : variants) {
        auto vi = v->GetInfo();
        if (vi.DeviceType() == device_type && vi.Task().has_value() && *vi.Task() == task) {
          m->SelectVariant(*v);
          info = m->GetInfo();
          found_variant = true;
          break;
        }
      }

      if (!found_variant) {
        continue;
      }
    }

    int64_t size = info.FilesizeMb().value_or(0);
    if (size > 0 && size < best_size) {
      best_size = size;
      best = m.get();
    }
  }

  return best;
}

/// Find the smallest model whose Name() contains the given substring (case-insensitive)
/// and DeviceType() matches.  Optionally also requires a matching task.
/// Returns nullptr if none found.
/// Helper: check whether a ModelInfo matches a name substring, optional task,
/// and device type.
inline bool MatchesNameTaskDevice(const foundry_local::ModelInfo& info, const char* name_substring,
                                  const char* task, flDeviceType device_type) {
  if (info.DeviceType() != device_type) {
    return false;
  }

  if (task) {
    auto t = info.Task();
    if (!t.has_value() || *t != task) {
      return false;
    }
  }

  std::string name = fl::test::ToLower(std::string(info.Name()));
  std::string needle = fl::test::ToLower(std::string(name_substring));
  return name.find(needle) != std::string::npos;
}

inline foundry_local::IModel* FindSmallestModelByName(foundry_local::ModelList& models, const char* name_substring,
                                                      const char* task = nullptr,
                                                      flDeviceType device_type = FOUNDRY_LOCAL_DEVICE_CPU) {
  foundry_local::IModel* best = nullptr;
  int64_t best_size = std::numeric_limits<int64_t>::max();

  for (const auto& m : models) {
    auto info = m->GetInfo();

    if (!MatchesNameTaskDevice(info, name_substring, task, device_type)) {
      // Scan variants for a match.
      bool found_variant = false;
      auto variants = m->GetVariants();
      for (const auto& v : variants) {
        if (MatchesNameTaskDevice(v->GetInfo(), name_substring, task, device_type)) {
          m->SelectVariant(*v);
          info = m->GetInfo();
          found_variant = true;
          break;
        }
      }

      if (!found_variant) {
        continue;
      }
    }

    int64_t size = info.FilesizeMb().value_or(0);
    if (size > 0 && size < best_size) {
      best_size = size;
      best = m.get();
    }
  }

  return best;
}

/// Find the smallest model that reliably supports tool calling for the given
/// device type.  Returns nullptr if none found.
/// Helper: check whether a ModelInfo is a chat-completion model that
/// supports tool calling on the given device type.
inline bool MatchesToolCalling(const foundry_local::ModelInfo& info, flDeviceType device_type) {
  if (info.DeviceType() != device_type) {
    return false;
  }

  auto t = info.Task();
  if (!t.has_value() || *t != "chat-completion") {
    return false;
  }

  return info.SupportsToolCalling().value_or(false);
}

inline foundry_local::IModel* FindSmallestToolCallingModel(
    foundry_local::ModelList& models,
    flDeviceType device_type = FOUNDRY_LOCAL_DEVICE_CPU) {
  foundry_local::IModel* best = nullptr;
  int64_t best_size = std::numeric_limits<int64_t>::max();

  for (const auto& m : models) {
    auto info = m->GetInfo();

    if (!MatchesToolCalling(info, device_type)) {
      bool found_variant = false;
      auto variants = m->GetVariants();
      for (const auto& v : variants) {
        if (MatchesToolCalling(v->GetInfo(), device_type)) {
          m->SelectVariant(*v);
          info = m->GetInfo();
          found_variant = true;
          break;
        }
      }

      if (!found_variant) {
        continue;
      }
    }

    int64_t size = info.FilesizeMb().value_or(0);
    if (size > 0 && size < best_size) {
      best_size = size;
      best = m.get();
    }
  }

  return best;
}

/// Find the smallest reasoning model (supports_reasoning=1) on the given device type. Returns nullptr if none found.
/// Reasoning models emit <think>...</think> blocks and are exercised by dedicated reasoning tests.
inline bool MatchesReasoning(const foundry_local::ModelInfo& info, flDeviceType device_type) {
  if (info.DeviceType() != device_type) {
    return false;
  }

  auto t = info.Task();
  if (!t.has_value() || *t != "chat-completion") {
    return false;
  }

  return info.GetIntProperty(FOUNDRY_LOCAL_MODEL_PROP_SUPPORTS_REASONING_INT, -1) == 1;
}

inline foundry_local::IModel* FindReasoningModel(foundry_local::ModelList& models,
                                                 flDeviceType device_type = FOUNDRY_LOCAL_DEVICE_CPU) {
  foundry_local::IModel* best = nullptr;
  int64_t best_size = std::numeric_limits<int64_t>::max();

  for (const auto& m : models) {
    auto info = m->GetInfo();

    if (!MatchesReasoning(info, device_type)) {
      bool found_variant = false;
      auto variants = m->GetVariants();
      for (const auto& v : variants) {
        if (MatchesReasoning(v->GetInfo(), device_type)) {
          m->SelectVariant(*v);
          info = m->GetInfo();
          found_variant = true;
          break;
        }
      }

      if (!found_variant) {
        continue;
      }
    }

    int64_t size = info.FilesizeMb().value_or(0);
    if (size > 0 && size < best_size) {
      best_size = size;
      best = m.get();
    }
  }

  return best;
}

/// Find a model by exact name prefix (case-insensitive).
/// Returns nullptr if not found.
inline foundry_local::IModel* FindModelByName(foundry_local::ModelList& models, const char* name_prefix) {
  std::string needle = fl::test::ToLower(std::string(name_prefix));

  for (const auto& m : models) {
    std::string name = fl::test::ToLower(std::string(m->GetInfo().Name()));

    if (name.find(needle) == 0) {
      return m.get();
    }
  }

  return nullptr;
}

/// Download and load a model. Returns true on success, false on failure.
inline bool TryDownloadAndLoad(foundry_local::IModel& model) {
  try {
    model.Download();
    model.Load();
    return true;
  } catch (const std::exception& ex) {
    std::cout << "  failed to load " << model.GetInfo().Id()
              << ": " << ex.what() << "\n";
    return false;
  }
}

/// Process-global test environment.
/// Owns the Manager, Catalog, ModelList, and pointers to loaded models.
/// Registered via ::testing::AddGlobalTestEnvironment in a static initializer.
class SharedTestEnv : public ::testing::Environment {
 public:
  static SharedTestEnv& Get() { return *instance_; }

  void SetUp() override {
    foundry_local::Configuration config("foundry_local_sdk_test");

    // Bump default log level so EP registration / runtime version messages
    // surface during test runs and end up in the rotating log file.
    config.SetDefaultLogLevel(FOUNDRY_LOCAL_LOG_INFO);

    config.AddCatalogUrl("https://ai.azure.com/api/centralus/ux/v1.0");

    // Point the model cache at the shared test data directory when available.
    auto cache_dir = fl::test::SafeGetEnv("FOUNDRY_TEST_DATA_DIR");
    if (!cache_dir.empty()) {
      config.SetModelCacheDir(cache_dir);
    }

    manager_ = std::make_unique<foundry_local::Manager>(std::move(config));

    // CI gate: surface diagnostics so a missing FOUNDRY_TEST_DATA_DIR is obvious
    // in build logs (every model-using test will GTEST_SKIP in this state).
    if (fl::test::IsRunningInCI()) {
      auto ci_cache_dir = fl::test::SafeGetEnv("FOUNDRY_TEST_DATA_DIR");
      std::cout << "SharedTestEnv: CI detected -- model downloads disabled. "
                << "FOUNDRY_TEST_DATA_DIR="
                << (ci_cache_dir.empty() ? "(unset; all model-using tests will skip)" : ci_cache_dir)
                << "\n";
    }

    // ---- Detect and register EPs ----
    has_cuda_ = EnsureEpRegistered("CUDAExecutionProvider");
    std::cout << "SharedTestEnv: device="
              << (has_cuda_ ? "GPU (CUDA)" : "CPU") << "\n";

    has_webgpu_ = EnsureEpRegistered("WebGpuExecutionProvider");
    std::cout << "SharedTestEnv: webgpu="
              << (has_webgpu_ ? "registered" : "unavailable") << "\n";

    model_list_ = std::make_unique<foundry_local::ModelList>(manager_->GetCatalog().GetModels());

    // ---- Chat model ----
    chat_model_ = FindModelByName(*model_list_, "qwen2.5-0.5b-instruct-generic-cpu");

    if (!chat_model_) {
      chat_model_ = FindSmallestModelByTask(*model_list_, "chat-completion",
                                            FOUNDRY_LOCAL_DEVICE_CPU);
    }

    if (chat_model_) {
      // Populate IDs from selection — they're available without loading.
      chat_model_id_ = std::string(chat_model_->GetInfo().Id());
      chat_model_alias_ = std::string(chat_model_->GetInfo().Alias());
      std::cout << "SharedTestEnv: chat_model selected="
                << chat_model_->GetInfo().Name() << "\n";
    } else {
      std::cout << "SharedTestEnv: no chat-completion model in catalog\n";
    }

    // ---- Reasoning model ----
    // A separate slot for reasoning models (qwen3, etc.) that emit <think>...</think> blocks. Reasoning behavior is
    // exercised by dedicated tests; the default chat_model stays a non-reasoning instruct model so existing tests
    // assert on direct answers.
    reasoning_model_ = FindModelByName(*model_list_, "qwen3-0.6b-generic-cpu");

    if (!reasoning_model_) {
      reasoning_model_ = FindReasoningModel(*model_list_, FOUNDRY_LOCAL_DEVICE_CPU);
    }

    if (reasoning_model_) {
      reasoning_model_id_ = std::string(reasoning_model_->GetInfo().Id());
      std::cout << "SharedTestEnv: reasoning_model selected="
                << reasoning_model_->GetInfo().Name() << "\n";
    } else {
      std::cout << "SharedTestEnv: no reasoning model in catalog\n";
    }

    // ---- Tool-calling model ----
    // Use the same preferred model. It supports tool calling reliably.
    // Fall back to any model with supports_tool_calling=1.
    // SELECTION ONLY — actual download+load is deferred to first
    // tool_calling_model() call. See LazyLoad note above.
    tool_calling_model_ = chat_model_;
    if (!tool_calling_model_ || !tool_calling_model_->GetInfo().SupportsToolCalling().value_or(false)) {
      tool_calling_model_ = FindSmallestToolCallingModel(*model_list_);
    }

    if (!tool_calling_model_) {
      std::cout << "SharedTestEnv: no tool-calling model in catalog\n";
    } else if (tool_calling_model_ == chat_model_) {
      std::cout << "SharedTestEnv: tool_calling_model="
                << tool_calling_model_->GetInfo().Name() << " (same as chat_model)\n";
    } else {
      std::cout << "SharedTestEnv: tool_calling_model selected="
                << tool_calling_model_->GetInfo().Name() << "\n";
    }

    // ---- Audio / streaming-audio / vision / embeddings ----
    // SELECTION ONLY. AcquireModels() (called from each fixture's
    // SetUpTestSuite) loads the set the suite needs and unloads anything
    // it doesn't, so peak memory stays bounded by max(suite_needs)
    // instead of sum(everything). Vision and nemotron in particular are
    // multi-GB and used to push the resident set above 13 GB when
    // eager-loaded.
    // Non-streaming audio transcription uses OnnxAudioGenerator which is
    // whisper-specific (BuildWhisperPrompt, ProcessAudios). Nemotron speech
    // has a different architecture and is covered by streaming_audio_model_.
    audio_model_ = FindSmallestModelByName(*model_list_, "whisper", "automatic-speech-recognition",
                                           FOUNDRY_LOCAL_DEVICE_CPU);

    if (audio_model_) {
      audio_model_id_ = std::string(audio_model_->GetInfo().Id());
      std::cout << "SharedTestEnv: audio_model selected="
                << audio_model_->GetInfo().Name() << "\n";
    } else {
      std::cout << "SharedTestEnv: no whisper model in catalog\n";
    }

    streaming_audio_model_ = FindSmallestModelByName(*model_list_, "nemotron", "automatic-speech-recognition",
                                                     FOUNDRY_LOCAL_DEVICE_CPU);

    if (streaming_audio_model_) {
      std::cout << "SharedTestEnv: streaming_audio_model selected="
                << streaming_audio_model_->GetInfo().Name() << "\n";
    } else {
      std::cout << "SharedTestEnv: no nemotron model in catalog\n";
    }

    // ---- Vision model ----
    vision_model_ = FindSmallestModelByTask(*model_list_, "vision-language-chat", FOUNDRY_LOCAL_DEVICE_CPU);

    if (vision_model_) {
      vision_model_id_ = std::string(vision_model_->GetInfo().Id());
      std::cout << "SharedTestEnv: vision_model selected="
                << vision_model_->GetInfo().Name() << "\n";
    } else {
      std::cout << "SharedTestEnv: no vision-language-chat model in catalog\n";
    }

    // ---- Embeddings model ----
    // Always use CPU; embeddings are not compute-intensive and CPU avoids
    // a GPU dependency in CI.
    embeddings_model_ = FindSmallestModelByTask(*model_list_, "embeddings", FOUNDRY_LOCAL_DEVICE_CPU);

    if (embeddings_model_) {
      embeddings_model_id_ = std::string(embeddings_model_->GetInfo().Id());
      std::cout << "SharedTestEnv: embeddings_model selected="
                << embeddings_model_->GetInfo().Name() << "\n";
    } else {
      std::cout << "SharedTestEnv: no CPU embeddings model in catalog\n";
    }

    // Resolve test audio file path.
#ifdef FOUNDRY_LOCAL_TEST_DATA_DIR
    auto audio_path = fs::path(FOUNDRY_LOCAL_TEST_DATA_DIR) / "Recording.mp3";
#else
    auto audio_path = fs::current_path() / "testdata" / "Recording.mp3";
#endif
    if (fs::exists(audio_path)) {
      audio_file_path_ = fs::canonical(audio_path).string();
    }

    // ---- Web service ----
    // The HTTP service is a thin dispatch layer above the Manager; it
    // doesn't care which models are loaded and routes by model id at
    // request time. Start it once for the whole process so suites don't
    // pay startup/shutdown cost per modality, and so any suite (chat,
    // audio, embeddings, vision) can hit it without coordinating
    // service lifecycle.
    try {
      manager_->StartWebService();
      auto endpoints = manager_->GetWebServiceEndpoints();
      if (!endpoints.empty()) {
        web_service_url_ = endpoints[0];
        std::cout << "SharedTestEnv: web_service url=" << web_service_url_ << "\n";
      }
    } catch (const std::exception& ex) {
      std::cout << "SharedTestEnv: web service start failed: " << ex.what() << "\n";
    }
  }

  void TearDown() override {
    if (manager_ && !web_service_url_.empty()) {
      try {
        manager_->StopWebService();
      } catch (...) {
      }
    }

    // Do not force-unload here. Responses API may intentionally keep idle
    // sessions cached for follow-up requests, which pins model session refs.
    // Manager shutdown performs coordinated session drain + unload.

    chat_model_ = nullptr;
    tool_calling_model_ = nullptr;
    audio_model_ = nullptr;
    streaming_audio_model_ = nullptr;
    vision_model_ = nullptr;
    embeddings_model_ = nullptr;
    reasoning_model_ = nullptr;
    acquired_.clear();
    model_list_.reset();
    manager_.reset();
  }

  foundry_local::Manager* manager() { return manager_.get(); }
  foundry_local::ICatalog& catalog() { return manager_->GetCatalog(); }
  foundry_local::ModelList* model_list() { return model_list_.get(); }
  const std::string& web_service_url() const { return web_service_url_; }

  // ----------------------------------------------------------------
  // Modality-based suite-level model acquisition.
  //
  // Memory policy: each test suite declares its needs via AcquireModels()
  // in its SetUpTestSuite(). AcquireModels diffs against the currently
  // resident set: it unloads anything not wanted and downloads+loads
  // anything wanted but not yet loaded. Consecutive suites that share a
  // model keep it warm; transitions across modality boundaries pay one
  // unload + one load. Peak memory = max(any single suite's needs)
  // instead of sum(everything).
  //
  // Accessors return the selected `IModel*` only when the suite
  // acquired it (selection found a candidate AND AcquireModels
  // successfully loaded it). Tests may Unload()/Load() the model
  // freely without invalidating the accessor — the gate is
  // suite-level acquisition, not live load state. Returns nullptr
  // when no suite asked for it, or when load failed.
  // ----------------------------------------------------------------

  enum class Modality {
    Chat,
    Tool,
    Audio,
    StreamingAudio,
    Embeddings,
    Vision,
    Reasoning,
  };

  void AcquireModels(std::initializer_list<Modality> modalities) {
    // Resolve modalities → distinct model pointers (Chat and Tool may
    // resolve to the same instance; the set keeps it once).
    std::set<foundry_local::IModel*> wanted;
    for (auto m : modalities) {
      if (auto* p = ResolveModality(m)) {
        wanted.insert(p);
      }
    }

    // Unload anything currently loaded that the new suite doesn't want.
    // Drop it from acquired_ so accessors return nullptr.
    std::set<foundry_local::IModel*> seen;
    for (auto* p : AllSelectedModels()) {
      if (!p || !p->IsLoaded() || !seen.insert(p).second) {
        continue;
      }

      if (wanted.count(p) == 0) {
        // Responses API may keep chat sessions cached for follow-up turns.
        // Those idle sessions pin model refs, so eager unload attempts can
        // fail between suites. Keep chat/tool models warm and let manager
        // shutdown perform coordinated drain + unload.
        if (!web_service_url_.empty() && (p == chat_model_ || p == tool_calling_model_)) {
          std::cout << "SharedTestEnv: keeping " << p->GetInfo().Name()
                    << " loaded for web-service session reuse\n";
          acquired_.erase(p);
          continue;
        }

        std::cout << "SharedTestEnv: unloading " << p->GetInfo().Name() << "\n";
        acquired_.erase(p);

        try {
          p->Unload();
        } catch (const std::exception& ex) {
          std::cout << "SharedTestEnv: deferring unload for "
                    << p->GetInfo().Name() << " (" << ex.what() << ")\n";
        }
      }
    }

    // Load anything wanted but not yet loaded. On success, mark acquired
    // so the accessor returns the pointer even if the test transiently
    // unloads/reloads the model. On failure, leave it out of acquired_
    // so the per-test SetUp can skip.
    for (auto* p : wanted) {
      if (p->IsLoaded()) {
        acquired_.insert(p);
        continue;
      }

      // CI gate: never pull multi-GB models from the network during CI runs.
      // Leaving p out of acquired_ causes the fixture's per-test SetUp to
      // GTEST_SKIP via the modality accessor returning nullptr.
      if (fl::test::IsRunningInCI() && !p->IsCached()) {
        std::cout << "SharedTestEnv: skipping " << p->GetInfo().Name()
                  << " in CI -- not pre-cached in FOUNDRY_TEST_DATA_DIR\n";
        continue;
      }

      std::cout << "SharedTestEnv: loading " << p->GetInfo().Name() << "\n";
      if (TryDownloadAndLoad(*p)) {
        acquired_.insert(p);
      }
    }
  }

  foundry_local::IModel* chat_model() { return Loaded(chat_model_); }
  const std::string& chat_model_id() { return chat_model_id_; }
  const std::string& chat_model_alias() { return chat_model_alias_; }

  foundry_local::IModel* audio_model() { return Loaded(audio_model_); }
  const std::string& audio_model_id() { return audio_model_id_; }
  const std::string& audio_file_path() { return audio_file_path_; }

  foundry_local::IModel* tool_calling_model() { return Loaded(tool_calling_model_); }
  foundry_local::IModel* streaming_audio_model() { return Loaded(streaming_audio_model_); }

  foundry_local::IModel* embeddings_model() { return Loaded(embeddings_model_); }
  const std::string& embeddings_model_id() { return embeddings_model_id_; }

  foundry_local::IModel* vision_model() { return Loaded(vision_model_); }
  const std::string& vision_model_id() { return vision_model_id_; }

  foundry_local::IModel* reasoning_model() { return Loaded(reasoning_model_); }
  const std::string& reasoning_model_id() { return reasoning_model_id_; }

  bool has_cuda() const { return has_cuda_; }
  bool has_webgpu() const { return has_webgpu_; }

 private:
  // Return `m` only if the suite acquired it. Reflects "did
  // SetUpTestSuite ask for this and did the load succeed?", NOT the
  // live IsLoaded() state — tests are free to Unload() and Load()
  // through the same handle without invalidating the accessor.
  foundry_local::IModel* Loaded(foundry_local::IModel* m) const {
    return (m && acquired_.count(m)) ? m : nullptr;
  }

  foundry_local::IModel* ResolveModality(Modality m) const {
    switch (m) {
      case Modality::Chat:
        return chat_model_;
      case Modality::Tool:
        return tool_calling_model_;
      case Modality::Audio:
        return audio_model_;
      case Modality::StreamingAudio:
        return streaming_audio_model_;
      case Modality::Embeddings:
        return embeddings_model_;
      case Modality::Vision:
        return vision_model_;
      case Modality::Reasoning:
        return reasoning_model_;
    }
    return nullptr;
  }

  std::array<foundry_local::IModel*, 7> AllSelectedModels() const {
    return {chat_model_, tool_calling_model_, audio_model_,
            streaming_audio_model_, embeddings_model_, vision_model_,
            reasoning_model_};
  }

  friend SharedTestEnv* RegisterSharedTestEnv();

  /// Ensure the EP with the given name is registered, downloading it if
  /// necessary. Returns true if the EP is registered after this call, false
  /// if it was not discoverable or registration failed.
  bool EnsureEpRegistered(const std::string& ep_name) {
    try {
      auto eps = manager_->GetDiscoverableEps();
      for (const auto& ep : eps) {
        if (ep.name != ep_name) {
          continue;
        }

        if (ep.is_registered) {
          return true;
        }

        std::cout << "SharedTestEnv: downloading and registering " << ep_name
                  << " (first run may take several minutes)...\n";
        int last_ten = -1;
        manager_->DownloadAndRegisterEps(
            {ep_name},
            [&last_ten](std::string_view name, float pct) {
              // Log every 10% to show progress.
              int cur = static_cast<int>(pct / 10.0f);
              if (cur != last_ten) {
                last_ten = cur;
                std::cout << "  " << name << ": " << static_cast<int>(pct) << "%"
                          << std::endl;
              }
              return true;
            });
        return true;
      }
    } catch (const std::exception& ex) {
      std::cout << "SharedTestEnv: " << ep_name
                << " registration failed: " << ex.what() << "\n";
    }

    return false;
  }

  std::unique_ptr<foundry_local::Manager> manager_;
  std::unique_ptr<foundry_local::ModelList> model_list_;

  foundry_local::IModel* chat_model_ = nullptr;
  std::set<foundry_local::IModel*> acquired_;
  std::string chat_model_id_;
  std::string chat_model_alias_;

  foundry_local::IModel* tool_calling_model_ = nullptr;

  bool has_cuda_ = false;
  bool has_webgpu_ = false;

  foundry_local::IModel* audio_model_ = nullptr;
  std::string audio_model_id_;
  std::string audio_file_path_;

  foundry_local::IModel* streaming_audio_model_ = nullptr;

  foundry_local::IModel* embeddings_model_ = nullptr;
  std::string embeddings_model_id_;

  foundry_local::IModel* vision_model_ = nullptr;
  std::string vision_model_id_;

  foundry_local::IModel* reasoning_model_ = nullptr;
  std::string reasoning_model_id_;

  std::string web_service_url_;

  static SharedTestEnv* instance_;
};

// Factory + registration.
inline SharedTestEnv* RegisterSharedTestEnv() {
  auto* env = new SharedTestEnv();
  SharedTestEnv::instance_ = env;
  ::testing::AddGlobalTestEnvironment(env);  // GTest takes ownership
  return env;
}

// Static initializer triggers registration before main().
inline SharedTestEnv* SharedTestEnv::instance_ =
    RegisterSharedTestEnv();
