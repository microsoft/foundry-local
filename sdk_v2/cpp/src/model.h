// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <atomic>
#include <forward_list>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "inferencing/execution_provider.h"
#include "model_info.h"

namespace fl {

class NonGenerativeAdmissionCoordinator;

class DownloadManager;
struct Item;
class ModelLoadManager;
class LoadedSelectionReservation;

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
// The C API's embedded backend adapts this object behind its opaque flModel handles.
// -----------------------------------------------------------------------

class Model {
 public:
  // Every successfully constructed Model is either a leaf with immutable metadata or a container with a selected,
  // metadata-bearing leaf. A moved-from Model may only be destroyed or assigned a new value.
  Model() = delete;
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

  /// Create an in-place externally registered model. Assets are never deleted by this Model.
  static Model FromLocalRegistration(ModelInfo info,
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

  /// Reconcile with a fresh authoritative container while preserving leaves with matching metadata and local path.
  /// Returns false without changing either container when unregister is in progress.
  /// Removed/replaced leaves are deactivated but retained in container-owned storage for outstanding handle safety.
  bool TryReconcileVariants(Model& incoming);

  /// Deactivate this container for an authoritative refresh unless unregister currently owns its lifecycle lock.
  bool TryDeactivateForRefresh();

  /// Reserve storage for retiring one variant. Call before persisting an unregister operation so the later
  /// in-memory commit cannot fail while transferring ownership of an outstanding model handle.
  bool PrepareRetireVariant(const std::string& model_id);

  /// Retire one variant after PrepareRetireVariant succeeds. Returns true when active variants remain.
  bool RetireVariant(const std::string& model_id);

  /// Choose the default selected variant from the current sorted variant list:
  /// first eligible cached variant if any, else the best eligible variant.
  /// If none are eligible, retain the best variant for catalog display and explicit access.
  /// Refreshes preserve explicit selections and the artifact currently loaded through an implicit selection.
  /// Requires IsContainer() to be true.
  /// Returns false when an existing explicit or loaded selection is preserved.
  bool SelectDefaultVariant(const std::function<bool(const Model&)>& is_eligible = {},
                            bool preserve_explicit_selection = false) const;
  bool HasEligibleDefaultVariant() const;
  void SetDefaultVariantRefresh(std::function<void()> refresh);
  void UpdateVariantVisibility(const std::function<bool(const Model&)>& is_visible);
  /// Hosted loads pin the selected artifact without loading weights into the metadata manager.
  /// Matching ID/path handles share the pin so any of them can release it.
  bool PreserveLoadedSelection(bool preserve);

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
  /// For a container, returns catalog-visible variants. Internal inventory and reconciliation callers
  /// may include hidden leaves to preserve cached/loaded access and stable handles across refreshes.
  ///
  /// Returned by value because the underlying variants_ vector could change during catalog refresh.
  ///
  /// `const` reflects that the *set* of variants is fixed by this call (no AddVariant /
  /// SelectVariant). The returned variants are themselves mutable (Download, Load, etc.) —
  /// the const_cast in the implementation is intentional and mirrors the
  /// `std::unique_ptr<T>::get() const → T*` idiom.
  std::vector<Model*> Variants(bool include_hidden = false) const;

  // --- Query methods ---

  bool IsCached() const;
  bool IsLoaded() const;
  Model* SelectedLeaf() const;

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
  bool IsContainer() const { return selected_variant_.load(std::memory_order_acquire) != nullptr; }

  // --- Mutation methods ---

  /// Download the model to local cache.
  /// `progress_cb` (optional) is called with percent in [0, 100].
  /// Return 0 to continue, non-zero to cancel — cancellation surfaces as
  /// FOUNDRY_LOCAL_ERROR_OPERATION_CANCELLED.
  void Download(std::function<int(float)> progress_cb = nullptr);
  const std::string& GetPath() const;
  /// Own the selected active leaf's execution metadata/path while admitted work uses it.
  /// An optional reservation pins exactly that artifact before default selection can move.
  Model CaptureExecutionModel(std::unique_ptr<LoadedSelectionReservation>* reservation = nullptr) const;
  /// Observe a completed shared-cache download without downloading or changing an already selected artifact.
  void RefreshCachedState();
  /// Publish an authoritative catalog cache path without replacing this handle or changing a loaded artifact.
  void UpdateCachedState(std::string local_path);

  /// Forget this client's cache publication after an authoritative remote removal; never delete files.
  void ForgetCachedState();
  void Load(ExecutionProvider ep = ExecutionProvider::kDefault,
            std::optional<std::string_view> expected_path = std::nullopt);
  void Unload();
  std::shared_ptr<void> AcquireNonGenerativeRuntime();
  std::shared_ptr<void> AcquireLoadedNonGenerativeRuntime();
  void RemoveFromCache(std::optional<std::string_view> expected_path = std::nullopt);

  /// Mark this model and its variants inactive while retaining pointer validity.
  void Deactivate();

  /// Serialize unregister with Load(); EndUnregister releases the lock after success or rollback.
  void BeginUnregister();
  void EndUnregister();

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
  /// Published paths are immutable and retained for this model's lifetime, including across catalog refresh.
  const std::string& LocalPath() const;
  std::string LocalPathSnapshot() const;

 private:
  struct ContainerTag {};

  Model(ModelInfo info,
        std::string local_path,
        DownloadManager& download_manager,
        ModelLoadManager& model_load_manager,
        bool external_registration);
  Model(ContainerTag, Model first_variant);

  void RefreshDefaultVariant() const;
  bool HasPreservedLoadedSelection() const;
  bool SelectDefaultVariantLocked(const std::function<bool(const Model&)>& is_eligible,
                                  bool preserve_explicit_selection) const;

  // Leaf data (empty for containers). Construction guarantees this is non-null for every leaf.
  // cached_ is atomic — flipped concurrently by the download path.
  // Loaded state is NOT stored here; it is queried from ModelLoadManager so the load
  // manager remains the single source of truth (Manager::Shutdown clears its map without
  // having to walk every Model and reset a local flag).
  // Mutable paths and immutable borrowed-path publications are accessed under state_mutex_.
  std::unique_ptr<const ModelInfo> info_;
  std::atomic<bool> cached_{false};
  // Logical tombstone state. Retired models remain allocated so outstanding catalog-owned handles stay address-valid,
  // but operations that would use the registration reject them.
  std::atomic<bool> active_{true};
  std::string local_path_;
  mutable std::forward_list<std::string> published_paths_;
  bool external_registration_ = false;

  // Non-owning service bindings for leaf operations. Set once at construction and never
  // reassigned; guaranteed non-null because FromModelInfo takes them by reference.
  DownloadManager* download_manager_ = nullptr;
  ModelLoadManager* model_load_manager_ = nullptr;

  // Container data (empty/null for leaves). unique_ptr keeps Model addresses
  // stable across vector growth/reordering.
  std::vector<std::unique_ptr<Model>> variants_;
  std::vector<std::unique_ptr<Model>> retired_variants_;
  std::atomic<bool> catalog_visible_{true};
  mutable std::atomic<Model*> selected_variant_{nullptr};                  // non-null = this is a container
  mutable bool selection_is_explicit_ = false;                             // guarded by state_mutex_
  mutable bool has_eligible_default_variant_ = true;                       // guarded by state_mutex_
  mutable Model* loading_variant_ = nullptr;                               // guarded by state_mutex_
  mutable std::function<bool(const Model&)> default_variant_eligibility_;  // guarded by state_mutex_
  std::function<void()> default_variant_refresh_;                          // guarded by state_mutex_

  // Guards variants_ across reader/writer threads (catalog refresh adding variants
  // while another thread enumerates via Variants()).
  mutable std::mutex state_mutex_;

  // Lifecycle state. Containers acquire this before a leaf's lifecycle lock, never while holding state_mutex_.
  // A Model must not be moved while a download, load, unload, or unregister operation is active.
  mutable std::mutex lifecycle_mutex_;
  std::atomic<size_t> active_downloads_{0};
  bool unregistering_ = false;
  std::vector<Model*> unregistering_variants_;  // exact container snapshot locked by BeginUnregister
};

}  // namespace fl
