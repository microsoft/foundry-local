// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "inferencing/execution_provider.h"
#include "model_info.h"

namespace fl {

class DownloadManager;
struct Item;
class ModelLoadManager;

// -----------------------------------------------------------------------
// Model — a single model entry or a multi-variant container.
//
// In "leaf" mode (created via FromModelInfo), it represents a single model
// variant with its own info, cached/loaded state, and local path.
//
// In "container" mode (created via MakeContainer), it groups multiple
// Model leaves that share the same alias and delegates all operations
// to a selected variant. Additional variants are added via AddVariant.
//
// The C API type (flModel) inherits from this with zero extra members,
// following the same pattern as flItem : fl::Item.
// -----------------------------------------------------------------------

class Model {
 public:
  Model() = default;
  ~Model();
  Model(Model&& other) noexcept;
  Model& operator=(Model&& other) noexcept;
  Model(const Model&) = delete;
  Model& operator=(const Model&) = delete;

  /// Create a leaf Model from a ModelInfo (used when populating the catalog).
  /// If local_path is non-empty the model is marked as cached at that location.
  /// The managers are non-owning bindings used by Download/Load/Unload. In production
  /// Manager owns both via unique_ptr. Tests can use `fl::test::FakeServiceBindings`
  /// (test_helpers.h) for a one-line construction with cheap fakes.
  static Model FromModelInfo(ModelInfo info,
                             std::string local_path,
                             DownloadManager& download_manager,
                             ModelLoadManager& model_load_manager);

  // --- Container construction ---

  /// Create a container Model wrapping the given variant as its first (and selected) variant.
  /// The container's own leaf data stays default/empty — all property access delegates to
  /// the selected variant.
  static Model MakeContainer(Model first_variant);

  /// Add a variant to this container. Requires IsContainer() to be true.
  /// The variant is inserted to keep the container's variant order best-first
  /// (device priority asc, version desc, created-at desc).
  ///
  /// Does not change the current selection. Call SelectDefaultVariant once the
  /// container has its full variant set.
  void AddVariant(Model variant);

  /// Choose the default selected variant from the current sorted variant list:
  /// first cached variant if any, else the best variant.
  /// Requires IsContainer() to be true.
  void SelectDefaultVariant();

  /// Best-first comparator: device priority asc, version desc, created-at desc, model_id asc.
  /// Exposed so callers that return Model* lists can produce consistent ordering with Variants().
  static bool CompareBestFirst(const Model& a, const Model& b);

  // --- Properties ---

  /// Unique model identifier. For containers, delegates to the selected variant.
  const std::string& Id() const;

  /// Alias shared across variants. For containers, delegates to the selected variant.
  const std::string& Alias() const;

  /// Full model metadata. For containers, delegates to the selected variant.
  const ModelInfo& Info() const;

  /// Returns the variants within this model. For a leaf, returns {this}.
  /// For a container, returns pointers to all contained variants.
  ///
  /// Returned by value because the underlying variants_ vector could change during catalog refresh.
  ///
  /// `const` reflects that the *set* of variants is fixed by this call (no AddVariant /
  /// SelectVariant). The returned variants are themselves mutable (Download, Load, etc.) —
  /// the const_cast in the implementation is intentional and mirrors the
  /// `std::unique_ptr<T>::get() const → T*` idiom.
  std::vector<Model*> Variants() const;

  // --- Query methods ---

  bool IsCached() const;
  bool IsLoaded() const;

  /// Get the supported input and output item types for this model, based on its task.
  /// Returns arrays of Item pointers (type-tag-only descriptors) from static storage.
  /// Pointers are stable for the lifetime of the process.
  /// Throws if the model's task doesn't have defined input/output info.
  struct IOInfo {
    const Item* const* inputs;
    size_t num_inputs;
    const Item* const* outputs;
    size_t num_outputs;
  };

  IOInfo GetInputOutputInfo() const;

  /// True if this is a multi-variant container.
  bool IsContainer() const { return selected_variant_ != nullptr; }

  // --- Mutation methods ---

  /// Download the model to local cache.
  /// `progress_cb` (optional) is called with percent in [0, 100].
  /// Return 0 to continue, non-zero to cancel — cancellation surfaces as
  /// FOUNDRY_LOCAL_ERROR_OPERATION_CANCELLED.
  void Download(std::function<int(float)> progress_cb = nullptr);
  const std::string& GetPath() const;
  void Load(ExecutionProvider ep = ExecutionProvider::kDefault);
  void Unload();
  void RemoveFromCache();

  /// Select a specific variant within this container. Throws if the variant is
  /// not part of this model, or if this is a leaf.
  ///
  /// `variant` is taken by const reference because only its address is used as a
  /// lookup key against the container's owned variants — the variant itself is not
  /// mutated by this call.
  void SelectVariant(const Model& variant);

  // --- Non-delegating accessors ---

  /// Get the local path if cached, or empty string if not.
  ///
  /// The returned reference is valid until either Download() or RemoveFromCache()
  /// is invoked on this Model. In practice these mutations are user-initiated
  /// one-shot operations, so callers reading the path concurrently with download
  /// or removal of the same Model are out of contract.
  const std::string& LocalPath() const { return local_path_; }

 private:
  // Leaf data (default/empty for containers).
  // cached_ is atomic — flipped concurrently by the download path.
  // Loaded state is NOT stored here; it is queried from ModelLoadManager so the load
  // manager remains the single source of truth (Manager::Shutdown clears its map without
  // having to walk every Model and reset a local flag).
  // local_path_ is set once during Download() (or at construction for already-cached models)
  // and cleared by RemoveFromCache(). It is intentionally NOT mutex-protected: concurrent
  // mutation alongside reads on the same Model* is not a supported pattern.
  ModelInfo info_;
  std::atomic<bool> cached_{false};
  std::string local_path_;

  // Non-owning service bindings for leaf operations. Set once at construction and never
  // reassigned; guaranteed non-null because FromModelInfo takes them by reference.
  DownloadManager* download_manager_ = nullptr;
  ModelLoadManager* model_load_manager_ = nullptr;

  // Container data (empty/null for leaves). unique_ptr keeps Model addresses
  // stable across vector growth/reordering.
  std::vector<std::unique_ptr<Model>> variants_;
  Model* selected_variant_ = nullptr;  // non-null = this is a container

  // Guards variants_ across reader/writer threads (catalog refresh adding variants
  // while another thread enumerates via Variants()).
  mutable std::mutex state_mutex_;
};

}  // namespace fl
