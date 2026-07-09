// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "model.h"

#include "download/download_manager.h"
#include "exception.h"
#include "inferencing/model_load_manager.h"
#include "items/item.h"
#include "items/text_item.h"
#include "util/string_utils.h"
#include "utils.h"

#include <foundry_local/foundry_local_c.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>

namespace fl {

namespace {

// ---------------------------------------------------------------------------
// Model priority sort — ports C# AzureFoundryService.CompareModelsForSort.
// ---------------------------------------------------------------------------

bool ContainsCaseInsensitive(const std::string& text, const std::string& pattern) {
  auto it = std::search(text.begin(), text.end(),
                        pattern.begin(), pattern.end(),
                        [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) ==
                                                    std::tolower(static_cast<unsigned char>(b)); });
  return it != text.end();
}

/// Extract device-type priority from model_id.
/// Format: <model_name>-<device>:<version>
/// Returns: 0(NPU) < 1(vendor-GPU) < 2(CUDA-GPU) < 3(generic-GPU)
///        < 4(vendor-CPU) < 5(generic-CPU) < 6(unknown)
int GetModelDevicePriority(const std::string& model_id) {
  if (ContainsCaseInsensitive(model_id, "-npu:")) {
    return 0;
  }

  // Check generic-gpu before -gpu: so "-generic-gpu:" isn't caught by the broader "-gpu:" check.
  if (ContainsCaseInsensitive(model_id, "-generic-gpu:")) {
    return 3;
  }

  if (ContainsCaseInsensitive(model_id, "-cuda-gpu:")) {
    return 2;
  }

  if (ContainsCaseInsensitive(model_id, "-gpu:")) {
    return 1;
  }

  if (ContainsCaseInsensitive(model_id, "-generic-cpu:")) {
    return 5;
  }

  if (ContainsCaseInsensitive(model_id, "-cpu:")) {
    return 4;
  }

  return 6;
}

/// Comparator for sorting variants by priority within a container.
/// Criteria (matching C# CompareModelsForSort):
///   1. Device-type priority (ascending — lower number = better)
///   2. Version number (descending — higher version first)
///   3. CreatedAtUnix timestamp (descending — newer first)
///   4. model_id (ascending) as final tie-break
bool CompareModelsForSort(const Model& m1, const Model& m2) {
  const auto& info1 = m1.Info();
  const auto& info2 = m2.Info();

  int p1 = GetModelDevicePriority(info1.model_id);
  int p2 = GetModelDevicePriority(info2.model_id);

  if (p1 != p2) {
    return p1 < p2;
  }

  if (info1.version != info2.version) {
    return info1.version > info2.version;
  }

  int64_t created1 = info1.GetPropertyWithDefault(FOUNDRY_LOCAL_MODEL_PROP_CREATED_AT_UNIX_INT, int64_t{0});
  int64_t created2 = info2.GetPropertyWithDefault(FOUNDRY_LOCAL_MODEL_PROP_CREATED_AT_UNIX_INT, int64_t{0});

  if (created1 != created2) {
    return created1 > created2;
  }

  return info1.model_id < info2.model_id;
}

}  // namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

Model::~Model() = default;

Model::Model(Model&& other) noexcept
    : info_(std::move(other.info_)),
      cached_(other.cached_.load()),
      local_path_(std::move(other.local_path_)),
      download_manager_(other.download_manager_),
      model_load_manager_(other.model_load_manager_),
      variants_(std::move(other.variants_)),
      selected_variant_(other.selected_variant_) {
  // After vector move, selected_variant_ still points into the transferred buffer.
  other.download_manager_ = nullptr;
  other.model_load_manager_ = nullptr;
  other.selected_variant_ = nullptr;
}

Model& Model::operator=(Model&& other) noexcept {
  if (this != &other) {
    info_ = std::move(other.info_);
    cached_.store(other.cached_.load());
    local_path_ = std::move(other.local_path_);
    download_manager_ = other.download_manager_;
    model_load_manager_ = other.model_load_manager_;
    variants_ = std::move(other.variants_);
    selected_variant_ = other.selected_variant_;
    other.download_manager_ = nullptr;
    other.model_load_manager_ = nullptr;
    other.selected_variant_ = nullptr;
  }

  return *this;
}

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

Model Model::FromModelInfo(ModelInfo info,
                           std::string local_path,
                           DownloadManager& download_manager,
                           ModelLoadManager& model_load_manager) {
  Model model;
  model.info_ = std::move(info);
  model.download_manager_ = &download_manager;
  model.model_load_manager_ = &model_load_manager;

  if (!local_path.empty()) {
    model.cached_ = true;
    model.local_path_ = std::move(local_path);
  }

  return model;
}

// ---------------------------------------------------------------------------
// Container operations
// ---------------------------------------------------------------------------

Model Model::MakeContainer(Model first_variant) {
  Model container;
  container.variants_.push_back(std::make_unique<Model>(std::move(first_variant)));
  container.selected_variant_ = container.variants_.back().get();
  return container;
}

void Model::AddVariant(Model variant) {
  if (!IsContainer()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "AddVariant called on a non-container Model; use MakeContainer first");
  }

  std::lock_guard<std::mutex> lock(state_mutex_);

  auto pos = std::upper_bound(variants_.begin(), variants_.end(), variant,
                              [](const Model& value, const std::unique_ptr<Model>& element) {
                                return CompareModelsForSort(value, *element);
                              });

  variants_.insert(pos, std::make_unique<Model>(std::move(variant)));
}

bool Model::CompareBestFirst(const Model& a, const Model& b) {
  return CompareModelsForSort(a, b);
}

void Model::SelectDefaultVariant() {
  if (!IsContainer()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL,
             "SelectDefaultVariant called on a non-container Model; use MakeContainer first");
  }

  std::lock_guard<std::mutex> lock(state_mutex_);

  for (auto& v : variants_) {
    if (v->IsCached()) {
      selected_variant_ = v.get();
      return;
    }
  }

  selected_variant_ = variants_.front().get();
}

// ---------------------------------------------------------------------------
// Properties (delegate to selected_variant_ when container)
// ---------------------------------------------------------------------------

const std::string& Model::Id() const {
  if (selected_variant_) {
    return selected_variant_->Id();
  }

  return info_.model_id;
}

const std::string& Model::Alias() const {
  if (selected_variant_) {
    return selected_variant_->Alias();
  }

  return info_.alias;
}

const ModelInfo& Model::Info() const {
  if (selected_variant_) {
    return selected_variant_->Info();
  }

  return info_;
}

std::vector<Model*> Model::Variants() const {
  std::lock_guard<std::mutex> lock(state_mutex_);

  std::vector<Model*> result;
  if (IsContainer()) {
    result.reserve(variants_.size());
    for (auto& v : variants_) {
      // const_cast: the *set* of variants is fixed (this method is const), but each
      // variant is independently mutable. See header.
      result.push_back(const_cast<Model*>(v.get()));
    }
  } else {
    result.push_back(const_cast<Model*>(this));
  }

  return result;
}

bool Model::IsCached() const {
  if (selected_variant_) {
    return selected_variant_->IsCached();
  }

  return cached_;
}

bool Model::IsLoaded() const {
  if (selected_variant_) {
    return selected_variant_->IsLoaded();
  }

  // ModelLoadManager owns the authoritative loaded-instance map. The pointer is set at
  // construction and never reassigned, so querying it here stays in sync with paths that
  // bypass Model::Load/Unload (e.g., Manager::Shutdown -> ModelLoadManager::UnloadAll).
  return model_load_manager_->GetLoadedModel(info_.model_id) != nullptr;
}

// ---------------------------------------------------------------------------
// Mutation (delegate to selected_variant_ when container)
// ---------------------------------------------------------------------------

void Model::Download(std::function<int(float)> progress_cb) {
  if (selected_variant_) {
    selected_variant_->Download(std::move(progress_cb));
    return;
  }

  // Already cached (scanner found the model on disk during catalog construction).
  // No need to re-derive the path via DownloadManager — local_path_ is authoritative.
  if (cached_ && !local_path_.empty()) {
    if (progress_cb) {
      // No work remains; cancellation request is meaningless here, so the
      // return value is intentionally ignored.
      progress_cb(100.0f);
    }
    return;
  }

  auto path = download_manager_->DownloadModel(info_, std::move(progress_cb));
  local_path_ = std::move(path);
  cached_.store(true);
}

const std::string& Model::GetPath() const {
  if (selected_variant_) {
    return selected_variant_->GetPath();
  }

  return local_path_;
}

void Model::Load(ExecutionProvider ep) {
  if (selected_variant_) {
    selected_variant_->Load(ep);
    return;
  }

  // LoadModel is idempotent — it returns kModelAlreadyLoaded if the id is already
  // in the load manager's map, so no need for a local short-circuit.
  auto result = model_load_manager_->LoadModel(local_path_, info_.model_id, ep);

  if (result.status == ModelLoadManager::LoadStatus::kModelNotFound) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "model not found at path: " + local_path_);
  }
}

void Model::Unload() {
  if (selected_variant_) {
    selected_variant_->Unload();
    return;
  }

  // UnloadModel is idempotent — returns false if the id isn't loaded.
  model_load_manager_->UnloadModel(info_.model_id);
}

void Model::RemoveFromCache() {
  if (selected_variant_) {
    selected_variant_->RemoveFromCache();
    return;
  }

  if (!cached_ || local_path_.empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "model is not cached locally");
  }

  if (IsLoaded()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "cannot remove a loaded model from cache; unload it first");
  }

  if (!Utils::RemoveDirectoryRecursive(local_path_)) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "failed to remove model cache directory: " + local_path_);
  }

  cached_.store(false);
  local_path_.clear();
}

void Model::SelectVariant(const Model& variant) {
  if (!IsContainer()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
             "Not supported on a model variant. Fetch a model by alias from the catalog to get a model "
             "with all variants available.");
  }

  for (auto& v : variants_) {
    if (v.get() == &variant) {
      selected_variant_ = v.get();
      return;
    }
  }

  FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "variant not found in this model");
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

// Static IO type descriptors, shared across all Model instances with the same task.
// Built once per task on first access (C++11 guarantees thread-safe function-local static init).
namespace {

struct StaticIOCache {
  std::vector<std::unique_ptr<Item>> input_items;
  std::vector<std::unique_ptr<Item>> output_items;
  std::vector<const Item*> input_ptrs;
  std::vector<const Item*> output_ptrs;
};

const StaticIOCache& ChatCompletionIO() {
  static const StaticIOCache cache = [] {
    StaticIOCache c;
    c.input_items.push_back(Item::Create(FOUNDRY_LOCAL_ITEM_MESSAGE));
    c.input_items.push_back(std::make_unique<TextItem>("", FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON));
    c.output_items.push_back(Item::Create(FOUNDRY_LOCAL_ITEM_MESSAGE));
    c.output_items.push_back(std::make_unique<TextItem>("", FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON));

    for (const auto& item : c.input_items) {
      c.input_ptrs.push_back(item.get());
    }

    for (const auto& item : c.output_items) {
      c.output_ptrs.push_back(item.get());
    }

    return c;
  }();

  return cache;
}

const StaticIOCache& AutomaticSpeechRecognitionIO() {
  static const StaticIOCache cache = [] {
    StaticIOCache c;
    c.input_items.push_back(Item::Create(FOUNDRY_LOCAL_ITEM_AUDIO));
    c.input_items.push_back(std::make_unique<TextItem>("", FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON));
    c.output_items.push_back(Item::Create(FOUNDRY_LOCAL_ITEM_TEXT));
    c.output_items.push_back(std::make_unique<TextItem>("", FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON));

    for (const auto& item : c.input_items) {
      c.input_ptrs.push_back(item.get());
    }

    for (const auto& item : c.output_items) {
      c.output_ptrs.push_back(item.get());
    }

    return c;
  }();

  return cache;
}

Model::IOInfo IOInfoFromCache(const StaticIOCache& cache) {
  return {cache.input_ptrs.data(), cache.input_ptrs.size(),
          cache.output_ptrs.data(), cache.output_ptrs.size()};
}

}  // namespace

Model::IOInfo Model::GetInputOutputInfo() const {
  if (selected_variant_) {
    return selected_variant_->GetInputOutputInfo();
  }

  const auto& task = Info().task;

  if (task == "chat-completion") {
    return IOInfoFromCache(ChatCompletionIO());
  }

  if (task == "automatic-speech-recognition") {
    return IOInfoFromCache(AutomaticSpeechRecognitionIO());
  }

  FL_THROW(FOUNDRY_LOCAL_ERROR_NOT_IMPLEMENTED,
           "input/output info not defined for task: " + task);
}

}  // namespace fl
