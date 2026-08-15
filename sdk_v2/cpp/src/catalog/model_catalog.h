// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "catalog.h"
#include "catalog/model_source.h"
#include "logger.h"

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace fl {

/// ModelCatalog — the single aggregating store behind ICatalog.
///
/// Owns the ModelFactory and every Model instance, plus a list of fetch-only IModelSources
/// (one per catalog type). It merges ModelInfo across sources into alias containers, keeps
/// same-model_id copies from different sources as shadow variants (ordered preferred-source
/// first), and serves a filtered preferred-only public API view. Fetch lives in the sources;
/// store / query / index / create / cache / refresh live here.
///
/// Model ownership: the catalog owns all Model instances via unique_ptr in models_. These
/// pointers are stable for the catalog's lifetime — external code can hold raw Model* safely.
/// The sole exception is Unregister(), an explicit user-initiated removal that frees a variant
/// (see RemoveVariant). Indices (id_index, alias_index, name_index) are rebuilt on refresh but
/// always point into stable storage; id_index resolves each model_id to its preferred leaf via
/// the source-aware variant ordering (first-wins over preferred-first variants_).
///
/// Was BaseModelCatalog + AzureModelCatalog; the fetch guts moved into AzureModelSource.
class ModelCatalog : public ICatalog {
 public:
  /// The store owns this factory: sources return ModelInfo (carrying local_path when cached),
  /// and the store builds Model leaves from it.
  using ModelFactory = std::function<Model(ModelInfo info)>;

  ModelCatalog(std::string name,
               std::vector<std::unique_ptr<IModelSource>> sources,
               ModelFactory model_factory,
               ILogger& logger);
  ~ModelCatalog() override;

  const std::string& GetName() const override { return name_; }

  // ICatalog implementations — query/lookup layer
  std::vector<Model*> ListModels() const override;
  Model* GetModel(const std::string& alias) const override;
  Model* GetModelVariant(const std::string& model_id) const override;
  Model* GetLatestVersion(const Model* model) const override;
  std::vector<Model*> GetCachedModels() const override;
  std::vector<Model*> GetLoadedModels() const override;
  std::vector<Model*> GetModelVersions(const std::string& model_alias,
                                       const std::string& variant_name,
                                       int max_versions = 0) override;
  void InvalidateCache() override;

  /// Remove a local-BYO model (foundation for the future BYOM Unregister). Locates the alias
  /// container, removes the matching variant, drops the container if it became empty, and
  /// rebuilds indices — re-selecting a surviving shadow (e.g. a cloud copy) if any. Not safe to
  /// call concurrently with enumeration/model ops on the affected alias.
  void Unregister(const std::string& model_id);

 private:
  /// Gather ModelInfo from every source (each stamped by Source()) and build Model leaves via
  /// the owned factory. Same-model_id shadows from different sources are all built and later
  /// grouped into the alias container.
  std::vector<Model> FetchModels() const;

  /// Gather all versions of an alias from every source and build leaves.
  std::vector<Model> FetchModelVersions(const std::string& model_alias,
                                        const std::string& model_name = "") const;

  /// Look up specific model IDs across every source and build leaves.
  std::vector<Model> FetchModelsByIds(const std::vector<std::string>& model_ids) const;

  /// Build a single Model leaf from a fetched ModelInfo. The info carries local_path when the
  /// model is cached locally, which marks the constructed leaf cached.
  Model BuildLeaf(ModelInfo info) const;

  /// Lookup indices into the stable models_ storage.
  /// Rebuilt on refresh. Does not own any Model instances.
  struct ModelIndex {
    std::unordered_map<std::string, Model*> id_index;     // model_id -> preferred Model* (specific variant)
    std::unordered_map<std::string, Model*> alias_index;  // alias -> Model* (grouped container)
    std::unordered_map<std::string, Model*> name_index;   // name -> latest version Model*
  };

  /// Stable model storage. unique_ptr ensures addresses never change.
  /// Models are appended on refresh; only Unregister() removes an entry.
  mutable std::vector<std::unique_ptr<Model>> models_;

  /// Lookup indices, rebuilt on each populate/refresh.
  /// Guarded by std::atomic_load/store free functions so readers get a consistent
  /// snapshot — the swap after rebuild is atomic, so a concurrent reader never sees
  /// a partially-built index.
  /// Can't use std::atomic<std::shared_ptr<>> due to lack of implemention in XCode (macOS)
  mutable std::shared_ptr<const ModelIndex> index_;

  /// Atomically grab the current index snapshot. Callers hold the returned shared_ptr
  /// for the duration of their lookup, keeping the index alive if a refresh swaps it out.
  std::shared_ptr<const ModelIndex> GetIndex() const;

  mutable bool populated_ = false;
  mutable std::mutex mutex_;
  mutable std::chrono::steady_clock::time_point next_refresh_at_{};

  static constexpr std::chrono::hours kCacheDuration{4};

  /// Populate or refresh the catalog (under lock). Groups variants, builds indices.
  void PopulateModels(std::vector<Model> variants) const;

  /// Merge new variants into the catalog's stable storage. For an existing alias container,
  /// appends any variants whose (model_id, catalog_source) isn't already present — allowing
  /// same-model_id shadows from different sources. For new aliases, creates a new container.
  /// Rebuilds the lookup index when the model set actually changed.
  void IntegrateVariants(std::vector<Model> variants) const;

  /// Build lookup indices from the current models_ collection.
  /// Builds a complete new ModelIndex locally, then atomically swaps it into index_.
  void RebuildIndex() const;

  /// Thread-safe access: ensures catalog is populated, refreshes if allowed and stale.
  void EnsurePopulated(bool allow_refresh = false) const;

  /// Append-only storage for GetModelVersions query results. Each call appends a new
  /// container, so all previously returned Model* pointers remain valid for the catalog's
  /// lifetime. These models are intentionally not integrated into the main lookup indices.
  mutable std::vector<std::unique_ptr<Model>> version_query_models_;

  std::string name_;
  std::vector<std::unique_ptr<IModelSource>> sources_;
  ModelFactory model_factory_;
  ILogger& logger_;
};

}  // namespace fl
