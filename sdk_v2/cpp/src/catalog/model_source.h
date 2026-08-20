// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "model_info.h"

#include <string>
#include <vector>

namespace fl {

/// IModelSource — a pure, fetch-only contributor to the aggregating ModelCatalog.
///
/// A source returns `ModelInfo` (never `Model`): it does not own a ModelFactory and never
/// touches DownloadManager / ModelLoadManager. The store creates the `Model` leaves. Each
/// source stamps `info.catalog_source` on every `ModelInfo` it produces so the store can
/// dedup / order shadow variants by source preference, and conveys a cached model's local
/// path via the transient `ModelInfo::local_path` field so the store marks the leaf cached.
///
/// One source exists per catalog type. Initial scope ships the Public (Azure) source only;
/// Private and BYOM-local sources are future follow-ups that slot in without a store redesign.
class IModelSource {
 public:
  virtual ~IModelSource() = default;

  /// The catalog source this fetcher serves. Stamped onto every ModelInfo it returns.
  virtual CatalogSource Source() const = 0;

  /// Human-readable name (e.g. the catalog URL) for logging / GetName.
  virtual std::string Name() const = 0;

  /// Fetch the latest model infos this source offers. Each ModelInfo is stamped with
  /// `Source()` (or kLocal for synthesized local stubs) and carries `local_path` when cached.
  virtual std::vector<ModelInfo> FetchModels() const = 0;

  /// Look up specific model IDs (e.g. older versions not in the latest catalog).
  /// Default returns `{}` (sources without a by-id lookup).
  virtual std::vector<ModelInfo> FetchModelsByIds(const std::vector<std::string>& /*model_ids*/) const {
    return {};
  }

  /// Fetch all known versions of a model (by alias), bypassing the "latest only" filter.
  /// Default returns `{}` (sources that cannot list older versions).
  virtual std::vector<ModelInfo> FetchModelVersions(const std::string& /*model_alias*/,
                                                    const std::string& /*model_name*/ = "") const {
    return {};
  }
};

}  // namespace fl
