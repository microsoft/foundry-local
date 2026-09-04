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
#include <filesystem>

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

Model::Model(ModelInfo info,
             std::string local_path,
             DownloadManager& download_manager,
             ModelLoadManager& model_load_manager,
             bool external_registration)
    : info_(std::make_unique<const ModelInfo>(std::move(info))),
      cached_(!local_path.empty()),
      local_path_(std::move(local_path)),
      external_registration_(external_registration),
      download_manager_(&download_manager),
      model_load_manager_(&model_load_manager) {}

Model::Model(ContainerTag, Model first_variant) {
  if (!first_variant.info_) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "MakeContainer requires an initialized leaf Model");
  }

  variants_.push_back(std::make_unique<Model>(std::move(first_variant)));
  selected_variant_.store(variants_.back().get(), std::memory_order_release);
}

Model::Model(Model&& other) noexcept
    : info_(std::move(other.info_)),
      cached_(other.cached_.load()),
      active_(other.active_.load()),
      local_path_(std::move(other.local_path_)),
      external_registration_(other.external_registration_),
      download_manager_(other.download_manager_),
      model_load_manager_(other.model_load_manager_),
      variants_(std::move(other.variants_)),
      retired_variants_(std::move(other.retired_variants_)),
      selected_variant_(other.selected_variant_.load(std::memory_order_relaxed)),
      selection_is_explicit_(other.selection_is_explicit_) {
  // The mutex and unregistering state are intentionally not moved; moving while unregistering is invalid.
  // After vector move, selected_variant_ still points into the transferred buffer.
  other.download_manager_ = nullptr;
  other.model_load_manager_ = nullptr;
  other.selected_variant_.store(nullptr, std::memory_order_relaxed);
}

Model& Model::operator=(Model&& other) noexcept {
  if (this != &other) {
    info_ = std::move(other.info_);
    cached_.store(other.cached_.load());
    active_.store(other.active_.load());
    local_path_ = std::move(other.local_path_);
    external_registration_ = other.external_registration_;
    download_manager_ = other.download_manager_;
    model_load_manager_ = other.model_load_manager_;
    variants_ = std::move(other.variants_);
    retired_variants_ = std::move(other.retired_variants_);
    selected_variant_.store(other.selected_variant_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    selection_is_explicit_ = other.selection_is_explicit_;
    unregistering_ = false;
    other.download_manager_ = nullptr;
    other.model_load_manager_ = nullptr;
    other.selected_variant_.store(nullptr, std::memory_order_relaxed);
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
  return Model(std::move(info), std::move(local_path), download_manager, model_load_manager, false);
}

Model Model::FromLocalRegistration(ModelInfo info,
                                   std::string local_path,
                                   DownloadManager& download_manager,
                                   ModelLoadManager& model_load_manager) {
  return Model(std::move(info), std::move(local_path), download_manager, model_load_manager, true);
}

// ---------------------------------------------------------------------------
// Container operations
// ---------------------------------------------------------------------------

Model Model::MakeContainer(Model first_variant) {
  return Model(ContainerTag{}, std::move(first_variant));
}

void Model::AddVariant(Model variant) {
  if (!IsContainer()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "AddVariant called on a non-container Model; use MakeContainer first");
  }
  if (!variant.info_) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "AddVariant requires an initialized leaf Model");
  }

  std::lock_guard<std::mutex> lock(state_mutex_);

  auto pos = std::upper_bound(variants_.begin(), variants_.end(), variant,
                              [](const Model& value, const std::unique_ptr<Model>& element) {
                                return CompareModelsForSort(value, *element);
                              });

  variants_.insert(pos, std::make_unique<Model>(std::move(variant)));
}

bool Model::TryReconcileVariants(Model& incoming) {
  if (!IsContainer() || !incoming.IsContainer() || Alias() != incoming.Alias()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "TryReconcileVariants requires containers with the same alias");
  }

  std::unique_lock<std::mutex> lifecycle_lock(lifecycle_mutex_, std::try_to_lock);
  if (!lifecycle_lock.owns_lock()) {
    return false;
  }

  std::scoped_lock lock(state_mutex_, incoming.state_mutex_);
  variants_.reserve(variants_.size() + incoming.variants_.size());
  retired_variants_.reserve(retired_variants_.size() + variants_.size());

  auto* previous_selection = selected_variant_.load(std::memory_order_acquire);
  const auto preserve_selection = selection_is_explicit_;

  for (auto current = variants_.begin(); current != variants_.end();) {
    const auto match = std::find_if(incoming.variants_.begin(), incoming.variants_.end(), [&](const auto& candidate) {
      return (*current)->Info().model_id == candidate->Info().model_id &&
             (*current)->LocalPath() == candidate->LocalPath();
    });
    if (match != incoming.variants_.end()) {
      incoming.variants_.erase(match);
      ++current;
      continue;
    }

    (*current)->Deactivate();
    retired_variants_.push_back(std::move(*current));
    current = variants_.erase(current);
  }

  for (auto& variant : incoming.variants_) {
    variants_.push_back(std::move(variant));
  }
  incoming.variants_.clear();

  std::sort(variants_.begin(), variants_.end(), [](const auto& left, const auto& right) {
    return CompareModelsForSort(*left, *right);
  });

  const auto retained_selection = std::find_if(variants_.begin(), variants_.end(), [&](const auto& variant) {
    return variant.get() == previous_selection;
  });
  const auto cached_selection = std::find_if(variants_.begin(), variants_.end(), [](const auto& variant) {
    return variant->IsCached();
  });
  const auto selected = preserve_selection && retained_selection != variants_.end()
                            ? retained_selection
                            : (cached_selection != variants_.end() ? cached_selection : variants_.begin());
  selection_is_explicit_ = preserve_selection && retained_selection != variants_.end();
  selected_variant_.store(selected->get(), std::memory_order_release);
  return true;
}

bool Model::TryDeactivateForRefresh() {
  std::unique_lock<std::mutex> lifecycle_lock(lifecycle_mutex_, std::try_to_lock);
  if (!lifecycle_lock.owns_lock()) {
    return false;
  }

  Deactivate();
  return true;
}

bool Model::PrepareRetireVariant(const std::string& model_id) {
  if (!IsContainer()) {
    return false;
  }

  std::lock_guard<std::mutex> lock(state_mutex_);
  const auto variant = std::find_if(variants_.begin(), variants_.end(), [&](const auto& candidate) {
    return candidate->Info().model_id == model_id;
  });
  if (variant == variants_.end()) {
    return false;
  }

  retired_variants_.reserve(retired_variants_.size() + 1);
  return true;
}

bool Model::RetireVariant(const std::string& model_id) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  const auto variant = std::find_if(variants_.begin(), variants_.end(), [&](const auto& candidate) {
    return candidate->Info().model_id == model_id;
  });
  if (variant == variants_.end()) {
    return !variants_.empty();
  }

  auto* retired = variant->get();
  retired->Deactivate();
  retired_variants_.push_back(std::move(*variant));
  variants_.erase(variant);

  if (variants_.empty()) {
    selection_is_explicit_ = false;
    selected_variant_.store(retired, std::memory_order_release);
    return false;
  }

  if (selected_variant_.load(std::memory_order_acquire) == retired) {
    const auto cached = std::find_if(variants_.begin(), variants_.end(), [](const auto& candidate) {
      return candidate->IsCached();
    });
    const auto selected = cached != variants_.end() ? cached : variants_.begin();
    selection_is_explicit_ = false;
    selected_variant_.store(selected->get(), std::memory_order_release);
  }

  return true;
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
      selection_is_explicit_ = false;
      selected_variant_.store(v.get(), std::memory_order_release);
      return;
    }
  }

  selection_is_explicit_ = false;
  selected_variant_.store(variants_.front().get(), std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Properties (delegate to selected_variant_ when container)
// ---------------------------------------------------------------------------

const std::string& Model::Id() const {
  if (Model* sv = selected_variant_.load(std::memory_order_acquire)) {
    return sv->Id();
  }

  return Info().model_id;
}

const std::string& Model::Alias() const {
  if (Model* sv = selected_variant_.load(std::memory_order_acquire)) {
    return sv->Alias();
  }

  return Info().alias;
}

const ModelInfo& Model::Info() const {
  if (Model* sv = selected_variant_.load(std::memory_order_acquire)) {
    return sv->Info();
  }

  if (!info_) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "cannot access metadata on a moved-from Model");
  }

  return *info_;
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
  if (Model* sv = selected_variant_.load(std::memory_order_acquire)) {
    return sv->IsCached();
  }

  if (external_registration_) {
    std::error_code ec;
    return std::filesystem::is_directory(local_path_, ec) &&
           std::filesystem::is_regular_file(std::filesystem::path(local_path_) / "genai_config.json", ec);
  }

  return active_ && cached_;
}

bool Model::IsLoaded() const {
  if (Model* sv = selected_variant_.load(std::memory_order_acquire)) {
    return sv->IsLoaded();
  }

  if (!active_) {
    return false;
  }

  // ModelLoadManager owns the authoritative loaded-instance map. The pointer is set at
  // construction and never reassigned, so querying it here stays in sync with paths that
  // bypass Model::Load/Unload (e.g., Manager::Shutdown -> ModelLoadManager::UnloadAll).
  return model_load_manager_->GetLoadedModel(Info().model_id, local_path_) != nullptr;
}

// ---------------------------------------------------------------------------
// Mutation (delegate to selected_variant_ when container)
// ---------------------------------------------------------------------------

void Model::Download(std::function<int(float)> progress_cb) {
  if (Model* sv = selected_variant_.load(std::memory_order_acquire)) {
    sv->Download(std::move(progress_cb));
    return;
  }

  if (!active_) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "model is no longer registered");
  }

  if (external_registration_) {
    if (progress_cb) {
      progress_cb(100.0f);
    }
    return;
  }

  // Already cached (scanner found the model on disk during catalog construction).
  // No need to re-derive the path via DownloadManager — local_path_ is authoritative.
  bool already_cached;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    already_cached = cached_.load() && !local_path_.empty();
  }

  if (already_cached) {
    if (progress_cb) {
      // No work remains; cancellation request is meaningless here, so the
      // return value is intentionally ignored.
      progress_cb(100.0f);
    }
    return;
  }

  auto path = download_manager_->DownloadModel(Info(), std::move(progress_cb));
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    local_path_ = std::move(path);
  }
  // local_path_ must be published before cached_ flips true: readers gate on IsCached()
  // and this release store guarantees they observe the completed path. Do not reorder.
  cached_.store(true);
}

const std::string& Model::GetPath() const {
  if (Model* sv = selected_variant_.load(std::memory_order_acquire)) {
    return sv->GetPath();
  }

  return local_path_;
}

void Model::Load(ExecutionProvider ep) {
  if (Model* sv = selected_variant_.load(std::memory_order_acquire)) {
    sv->Load(ep);
    return;
  }

  std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);

  if (!active_ || unregistering_) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "model is no longer registered");
  }

  const auto& info = Info();
  if (external_registration_ && ep == ExecutionProvider::kDefault && !info.execution_provider.empty()) {
    ep = EPUtils::StringtoEP(info.execution_provider);
    if (ep == ExecutionProvider::kUnknown) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
               "unknown execution provider for local model: " + info.execution_provider);
    }
  }

  // LoadModel is idempotent — it returns kModelAlreadyLoaded if the id is already
  // in the load manager's map, so no need for a local short-circuit.
  auto result = model_load_manager_->LoadModel(local_path_, Info().model_id, ep);

  if (result.status == ModelLoadManager::LoadStatus::kModelNotFound) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "model not found at path: " + local_path_);
  }

}

void Model::Unload() {
  if (Model* sv = selected_variant_.load(std::memory_order_acquire)) {
    sv->Unload();
    return;
  }

  std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);

  // Path qualification lets a retired handle clean up its own instance without unloading a later registration that
  // reused the same model ID at a different path. UnloadModel is idempotent when no matching instance is loaded.
  model_load_manager_->UnloadModel(Info().model_id, local_path_);
}

void Model::RemoveFromCache() {
  if (Model* sv = selected_variant_.load(std::memory_order_acquire)) {
    sv->RemoveFromCache();
    return;
  }

  if (external_registration_) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
             "local registrations are not cache entries; call Catalog::UnregisterModel instead");
  }

  std::string path;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!cached_ || local_path_.empty()) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "model is not cached locally");
    }
    path = local_path_;
  }

  if (IsLoaded()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "cannot remove a loaded model from cache; unload it first");
  }

  if (!Utils::RemoveDirectoryRecursive(path)) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "failed to remove model cache directory: " + path);
  }

  // Turn off the published state before tearing down the backing string (mirror of Download's ordering).
  cached_.store(false);
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    local_path_.clear();
  }
}

void Model::Deactivate() {
  active_.store(false);
  if (IsContainer()) {
    for (auto* variant : Variants()) {
      variant->Deactivate();
    }
  }
}

void Model::BeginUnregister() {
  if (IsContainer()) {
    lifecycle_mutex_.lock();
    if (!active_ || unregistering_) {
      lifecycle_mutex_.unlock();
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "model is no longer registered");
    }
    unregistering_ = true;

    try {
      std::lock_guard<std::mutex> state_lock(state_mutex_);
      unregistering_variants_.clear();
      unregistering_variants_.reserve(variants_.size());
      for (const auto& variant : variants_) {
        unregistering_variants_.push_back(variant.get());
      }
    } catch (...) {
      unregistering_ = false;
      lifecycle_mutex_.unlock();
      throw;
    }

    size_t locked_count = 0;
    try {
      for (; locked_count < unregistering_variants_.size(); ++locked_count) {
        unregistering_variants_[locked_count]->BeginUnregister();
      }
    } catch (...) {
      while (locked_count > 0) {
        unregistering_variants_[--locked_count]->EndUnregister();
      }
      unregistering_variants_.clear();
      unregistering_ = false;
      lifecycle_mutex_.unlock();
      throw;
    }
    return;
  }

  lifecycle_mutex_.lock();
  if (!active_ || unregistering_) {
    lifecycle_mutex_.unlock();
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "model is no longer registered");
  }

  unregistering_ = true;
}

void Model::EndUnregister() {
  if (!unregistering_variants_.empty()) {
    for (auto* variant : unregistering_variants_) {
      variant->EndUnregister();
    }
    unregistering_variants_.clear();
    unregistering_ = false;
    lifecycle_mutex_.unlock();
    return;
  }

  unregistering_ = false;
  lifecycle_mutex_.unlock();
}

void Model::SelectVariant(const Model& variant) {
  if (!IsContainer()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
             "Not supported on a model variant. Fetch a model by alias from the catalog to get a model "
             "with all variants available.");
  }

  std::lock_guard<std::mutex> lock(state_mutex_);
  for (auto& v : variants_) {
    if (v.get() == &variant) {
      selection_is_explicit_ = true;
      selected_variant_.store(v.get(), std::memory_order_release);
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

const StaticIOCache& VisionLanguageChatIO() {
  static const StaticIOCache cache = [] {
    StaticIOCache c;
    c.input_items.push_back(Item::Create(FOUNDRY_LOCAL_ITEM_MESSAGE));
    c.input_items.push_back(std::make_unique<TextItem>("", FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON));
    c.input_items.push_back(Item::Create(FOUNDRY_LOCAL_ITEM_IMAGE));
    c.input_items.push_back(Item::Create(FOUNDRY_LOCAL_ITEM_AUDIO));
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
  if (Model* sv = selected_variant_.load(std::memory_order_acquire)) {
    return sv->GetInputOutputInfo();
  }

  const auto& task = Info().task;

  if (task == "chat-completion") {
    return IOInfoFromCache(ChatCompletionIO());
  }

  if (task == "vision-language-chat") {
    return IOInfoFromCache(VisionLanguageChatIO());
  }

  if (task == "automatic-speech-recognition") {
    return IOInfoFromCache(AutomaticSpeechRecognitionIO());
  }

  FL_THROW(FOUNDRY_LOCAL_ERROR_NOT_IMPLEMENTED,
           "input/output info not defined for task: " + task);
}

}  // namespace fl
