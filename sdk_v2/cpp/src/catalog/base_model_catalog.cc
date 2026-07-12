// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "catalog/base_model_catalog.h"

#include <foundry_local/foundry_local_c.h>

#include "exception.h"

#include <algorithm>
#include <atomic>
#include <fmt/format.h>
#include <map>
#include <unordered_map>
#include <unordered_set>

namespace fl {

BaseModelCatalog::BaseModelCatalog(std::string name, ILogger& logger)
    : name_(std::move(name)), logger_(logger) {}
BaseModelCatalog::~BaseModelCatalog() = default;

void BaseModelCatalog::PopulateModels(std::vector<Model> variants) const {
  // Group variants by alias into Model containers.
  // Matches C# Catalog.UpdateModels() pattern:
  //   foreach (modelInfo) { find or create Model by alias, add variant }
  std::map<std::string, Model> alias_to_model;

  for (auto& v : variants) {
    const auto& info = v.Info();
    if (info.model_id.empty() || info.alias.empty() || info.name.empty()) {
      logger_.Log(LogLevel::Debug,
                  fmt::format("Skipping model with missing required fields: id='{}', name='{}', alias='{}'.",
                              info.model_id, info.name, info.alias));
      continue;  // skip invalid entries
    }

    std::string alias = info.alias;  // copy before potential move of v
    auto it = alias_to_model.find(alias);
    if (it != alias_to_model.end()) {
      it->second.AddVariant(std::move(v));
    } else {
      alias_to_model.emplace(std::move(alias), Model::MakeContainer(std::move(v)));
    }
  }

  for (auto& [alias, model] : alias_to_model) {
    model.SelectDefaultVariant();
  }

  // On refresh: merge new models into stable storage. Existing models keep their addresses.
  // New aliases are appended. Existing aliases are left unchanged (their Model* stays valid).
  if (populated_) {
    // Build a set of existing aliases for fast lookup.
    std::unordered_map<std::string, Model*> existing_aliases;
    for (auto& m : models_) {
      existing_aliases[m->Alias()] = m.get();
    }

    size_t new_count = 0;
    for (auto& [alias, model] : alias_to_model) {
      if (!existing_aliases.contains(alias)) {
        models_.push_back(std::make_unique<Model>(std::move(model)));
        ++new_count;
      }
    }

    if (new_count > 0) {
      logger_.Log(LogLevel::Information,
                  fmt::format("Catalog '{}' refresh: {} new model(s) added, {} total.",
                              name_, new_count, models_.size()));
    } else {
      logger_.Log(LogLevel::Debug,
                  fmt::format("Catalog '{}' refresh: no new models. {} total.",
                              name_, models_.size()));
    }
  } else {
    // Initial population: move all models into stable storage.
    models_.reserve(alias_to_model.size());
    for (auto& [alias, model] : alias_to_model) {
      models_.push_back(std::make_unique<Model>(std::move(model)));
    }

    logger_.Log(LogLevel::Debug,
                fmt::format("Catalog '{}' populated with {} model(s).", name_, models_.size()));
  }

  RebuildIndex();
  populated_ = true;
}

void BaseModelCatalog::IntegrateVariants(std::vector<Model> variants) const {
  std::lock_guard<std::mutex> lock(mutex_);

  if (variants.empty()) {
    return;
  }

  // Build a lookup of existing aliases -> containers so we can merge new
  // variants in O(1) per incoming variant.
  std::unordered_map<std::string, Model*> alias_to_existing;
  for (auto& m : models_) {
    alias_to_existing[m->Alias()] = m.get();
  }

  // Track existing model_ids in a single set so the dedup check is O(1) and
  // doesn't require walking each container's variants per incoming variant.
  std::unordered_set<std::string> existing_ids;
  for (auto& m : models_) {
    for (auto* v : m->Variants()) {
      existing_ids.insert(v->Info().model_id);
    }
  }

  size_t added_variants = 0;
  size_t added_aliases = 0;

  // Collect-by-alias the variants that are actually new (not already known).
  std::map<std::string, std::vector<Model>> new_by_alias;
  for (auto& v : variants) {
    const auto& info = v.Info();
    if (info.model_id.empty() || info.alias.empty() || info.name.empty()) {
      logger_.Log(LogLevel::Debug,
                  fmt::format("IntegrateVariants: skipping model with missing required fields: "
                              "id='{}', name='{}', alias='{}'.",
                              info.model_id, info.name, info.alias));
      continue;
    }

    if (existing_ids.count(info.model_id) > 0) {
      continue;
    }

    existing_ids.insert(info.model_id);
    new_by_alias[info.alias].push_back(std::move(v));
  }

  for (auto& [alias, alias_variants] : new_by_alias) {
    auto it = alias_to_existing.find(alias);
    if (it != alias_to_existing.end()) {
      for (auto& v : alias_variants) {
        it->second->AddVariant(std::move(v));
        ++added_variants;
      }
    } else {
      // New alias: build a container and choose default after all variants are added.
      auto first = std::move(alias_variants.front());
      auto container = Model::MakeContainer(std::move(first));
      for (size_t i = 1; i < alias_variants.size(); ++i) {
        container.AddVariant(std::move(alias_variants[i]));
      }

      container.SelectDefaultVariant();

      models_.push_back(std::make_unique<Model>(std::move(container)));
      ++added_aliases;
      added_variants += alias_variants.size();
    }
  }

  if (added_variants > 0 || added_aliases > 0) {
    logger_.Log(LogLevel::Information,
                fmt::format("Catalog '{}' integrated {} new variant(s) across {} new alias(es). "
                            "{} total alias container(s).",
                            name_, added_variants, added_aliases, models_.size()));
    RebuildIndex();
  }
}

void BaseModelCatalog::RebuildIndex() const {
  auto new_index = std::make_shared<ModelIndex>();

  for (auto& m : models_) {
    new_index->alias_index[m->Alias()] = m.get();

    for (auto* variant : m->Variants()) {
      const auto& info = variant->Info();

      if (!new_index->id_index.contains(info.model_id)) {
        new_index->id_index[info.model_id] = variant;
      }

      if (!new_index->name_index.contains(info.name)) {
        new_index->name_index[info.name] = m.get();
      }
    }
  }

  // Atomic swap — readers holding the old shared_ptr keep it alive until they're done.
  // Suppress C++20 deprecation warning for std::atomic_store/load free functions.
  // We can't use std::atomic<std::shared_ptr<>> due to missing XCode support (see header).
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
  std::atomic_store(&index_, std::shared_ptr<const ModelIndex>(std::move(new_index)));
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}

std::shared_ptr<const BaseModelCatalog::ModelIndex> BaseModelCatalog::GetIndex() const {
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
  return std::atomic_load(&index_);
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}

void BaseModelCatalog::InvalidateCache() {
  // Reset the refresh timer so the next query triggers a re-fetch.
  // This is called after EP registration changes — the catalog needs to
  // re-query with updated device/EP filters.
  std::lock_guard<std::mutex> lock(mutex_);
  next_refresh_at_ = std::chrono::steady_clock::time_point{};
}

void BaseModelCatalog::EnsurePopulated(bool allow_refresh) const {
  // Catalog access is never performance-critical: always take the lock so the
  // populated_/next_refresh_at_ check and the populate/refresh below are a single
  // critical section. (No fast path — races on next_refresh_at_ and populated_ are
  // not worth the complexity to optimise.)
  std::lock_guard<std::mutex> lock(mutex_);

  bool needs_refresh = allow_refresh &&
                       std::chrono::steady_clock::now() >= next_refresh_at_;

  if (populated_ && !needs_refresh) {
    return;
  }

  if (populated_) {
    logger_.Log(LogLevel::Information,
                fmt::format("Catalog '{}' refreshing (cache expired).", name_));
  }

  auto variants = FetchModels();
  PopulateModels(std::move(variants));
  next_refresh_at_ = std::chrono::steady_clock::now() + kCacheDuration;
}

std::vector<Model*> BaseModelCatalog::ListModels() const {
  EnsurePopulated(/*allow_refresh=*/true);

  // PopulateModels appends to models_ under mutex_; iterate under the same lock so a
  // concurrent EnsurePopulated cannot reallocate the vector mid-iteration.
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<Model*> result;
  result.reserve(models_.size());
  for (auto& m : models_) {
    result.push_back(m.get());
  }

  return result;
}

Model* BaseModelCatalog::GetModel(const std::string& alias) const {
  EnsurePopulated();

  auto idx = GetIndex();
  auto alias_it = idx->alias_index.find(alias);
  if (alias_it != idx->alias_index.end()) {
    return alias_it->second;
  }

  logger_.Log(LogLevel::Information,
              fmt::format("GetModel: '{}' not found in the catalog.", alias));

  return nullptr;
}

Model* BaseModelCatalog::GetModelVariant(const std::string& model_id) const {
  EnsurePopulated();

  auto idx = GetIndex();
  auto id_it = idx->id_index.find(model_id);
  if (id_it != idx->id_index.end()) {
    return id_it->second;
  }

  // Not in cached indices — try a direct catalog lookup. Only attempt this when
  // the input looks like a Model Id (Name + ":" + Version). Plain names and
  // aliases would not succeed via FetchModelsByIds and just cost a network call.
  // Mirrors C# BaseModelCatalog.GetModelInfoAsync direct-fetch fallback.
  if (model_id.find(':') != std::string::npos) {
    logger_.Log(LogLevel::Information,
                fmt::format("GetModelVariant: '{}' not in cache, fetching from catalog source.",
                            model_id));

    std::vector<Model> fetched;
    try {
      fetched = FetchModelsByIds({model_id});
    } catch (const std::exception& ex) {
      logger_.Log(LogLevel::Warning,
                  fmt::format("GetModelVariant: direct fetch for '{}' failed — {}",
                              model_id, ex.what()));
      return nullptr;
    } catch (...) {
      logger_.Log(LogLevel::Warning,
                  fmt::format("GetModelVariant: direct fetch for '{}' failed — unknown error",
                              model_id));
      return nullptr;
    }

    if (!fetched.empty()) {
      IntegrateVariants(std::move(fetched));

      // Look up again from the refreshed index.
      idx = GetIndex();
      auto id_it2 = idx->id_index.find(model_id);
      if (id_it2 != idx->id_index.end()) {
        return id_it2->second;
      }
    }
  }

  logger_.Log(LogLevel::Information,
              fmt::format("GetModelVariant: '{}' not found in the catalog.", model_id));

  return nullptr;
}

Model* BaseModelCatalog::GetLatestVersion(const Model* model) const {
  if (!model) {
    return nullptr;
  }

  EnsurePopulated();

  auto idx = GetIndex();
  auto alias_it = idx->alias_index.find(std::string(model->Info().alias));
  if (alias_it != idx->alias_index.end()) {
    auto variants = alias_it->second->Variants();
    if (!variants.empty()) {
      return variants.front();
    }
  }

  return nullptr;
}

std::vector<Model*> BaseModelCatalog::GetCachedModels() const {
  EnsurePopulated();

  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<Model*> result;
  for (auto& m : models_) {
    if (m->IsCached()) {
      result.push_back(m.get());
    }
  }

  return result;
}

std::vector<Model*> BaseModelCatalog::GetLoadedModels() const {
  EnsurePopulated();

  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<Model*> result;
  for (auto& m : models_) {
    if (m->IsLoaded()) {
      result.push_back(m.get());
    }
  }

  return result;
}

std::vector<Model*> BaseModelCatalog::GetModelVersions(const std::string& model_alias,
                                                       const std::string& variant_name,
                                                       int max_versions) {
  if (model_alias.empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "GetModelVersions requires a non-empty model_alias.");
  }

  // Make sure the regular "latest only" catalog is populated first so the
  // existing alias set is available for logging/validation.
  EnsurePopulated();

  std::vector<Model> fetched;
  try {
    fetched = FetchModelVersions(model_alias, variant_name);  // variant_name is used as model_name filter
  } catch (const std::exception& ex) {
    logger_.Log(LogLevel::Warning,
                fmt::format("GetModelVersions: fetch for alias '{}' failed — {}",
                            model_alias, ex.what()));
    return {};
  } catch (...) {
    logger_.Log(LogLevel::Warning,
                fmt::format("GetModelVersions: fetch for alias '{}' failed — unknown error",
                            model_alias));
    return {};
  }

  if (fetched.empty()) {
    return {};
  }

  std::vector<Model*> result;
  auto idx = GetIndex();

  {
    std::lock_guard<std::mutex> lock(mutex_);

    // Build a container Model for this alias, mirroring the structure used by
    // PopulateModels / IntegrateVariants. AddVariant maintains best-first order.
    auto container = Model::MakeContainer(std::move(fetched.front()));
    for (size_t i = 1; i < fetched.size(); ++i) {
      container.AddVariant(std::move(fetched[i]));
    }

    container.SelectDefaultVariant();
    version_query_models_.push_back(std::make_unique<Model>(std::move(container)));

    // Return variant pointers from the container (like Model_GetVariantsImpl).
    auto variants = version_query_models_.back()->Variants();
    result.reserve(variants.size());
    for (auto* v : variants) {
      if (!variant_name.empty() && v->Info().name != variant_name) {
        continue;
      }

      result.push_back(v);
    }
  }

  if (result.empty()) {
    // Source returned nothing — log when the alias was unknown.
    auto alias_it = idx->alias_index.find(model_alias);
    if (alias_it == idx->alias_index.end()) {
      logger_.Log(LogLevel::Information,
                  fmt::format("GetModelVersions: alias '{}' not found in catalog.", model_alias));
    }
  }

  if (max_versions > 0 && !result.empty()) {
    // Enforce latest N per variant name from the best-first ordering.
    std::unordered_map<std::string, int> selected_per_variant;
    std::vector<Model*> limited;
    limited.reserve(result.size());

    for (Model* model : result) {
      const std::string& variant = model->Info().name;
      int& count = selected_per_variant[variant];
      if (count >= max_versions) {
        continue;
      }

      ++count;
      limited.push_back(model);
    }

    result = std::move(limited);
  }

  return result;
}

}  // namespace fl
