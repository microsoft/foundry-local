// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "util/region_fallback.h"

#include "exception.h"
#include "utils.h"

#include <foundry_local/foundry_local_c.h>

#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <utility>

namespace fl {

namespace {

/// Build the proximal-region map once.
std::map<std::string, std::vector<std::string>> BuildProximalRegions() {
  std::map<std::string, std::vector<std::string>> map = {
      // Americas: United States
      {"eastus", {"eastus2", "centralus", "southcentralus", "westus2", "westeurope"}},
      {"eastus2", {"eastus", "centralus", "southcentralus", "westus2", "westeurope"}},
      {"centralus", {"southcentralus", "northcentralus", "eastus", "westus2", "westeurope"}},
      {"northcentralus", {"centralus", "southcentralus", "eastus", "westus2", "westeurope"}},
      {"southcentralus", {"centralus", "northcentralus", "eastus", "westus2", "westeurope"}},
      {"westus", {"westus2", "westus3", "westcentralus", "centralus", "eastus", "westeurope"}},
      {"westus2", {"westus3", "westus", "westcentralus", "centralus", "eastus", "westeurope"}},
      {"westus3", {"westus2", "westus", "westcentralus", "centralus", "eastus", "westeurope"}},
      {"westcentralus", {"westus2", "westus3", "westus", "centralus", "eastus", "westeurope"}},

      // Americas: Canada
      {"canadacentral", {"canadaeast", "eastus", "eastus2", "westeurope"}},
      {"canadaeast", {"canadacentral", "eastus", "eastus2", "westeurope"}},

      // Americas: Brazil
      {"brazilsouth", {"southcentralus", "eastus", "westeurope"}},

      // Europe: Western
      {"westeurope",
       {"northeurope", "francecentral", "germanywestcentral", "uksouth", "swedencentral", "eastus"}},
      {"northeurope", {"westeurope", "uksouth", "francecentral", "swedencentral", "eastus"}},
      {"francecentral",
       {"westeurope", "northeurope", "germanywestcentral", "switzerlandnorth", "eastus"}},
      {"germanywestcentral",
       {"westeurope", "francecentral", "switzerlandnorth", "polandcentral", "northeurope", "eastus"}},
      {"switzerlandnorth",
       {"switzerlandwest", "germanywestcentral", "francecentral", "westeurope", "italynorth", "eastus"}},
      {"switzerlandwest",
       {"switzerlandnorth", "francecentral", "italynorth", "germanywestcentral", "westeurope", "eastus"}},
      {"italynorth", {"switzerlandnorth", "francecentral", "westeurope", "spaincentral", "eastus"}},
      {"spaincentral", {"francecentral", "westeurope", "italynorth", "eastus"}},

      // Europe: UK
      {"uksouth", {"ukwest", "northeurope", "westeurope", "francecentral", "eastus"}},
      {"ukwest", {"uksouth", "northeurope", "westeurope", "francecentral", "eastus"}},

      // Europe: Nordics
      {"swedencentral", {"norwayeast", "northeurope", "westeurope", "eastus"}},
      {"norwayeast", {"swedencentral", "northeurope", "westeurope", "eastus"}},

      // Europe: Eastern
      {"polandcentral",
       {"germanywestcentral", "swedencentral", "westeurope", "northeurope", "eastus"}},

      // Middle East
      {"uaenorth", {"qatarcentral", "israelcentral", "westeurope", "northeurope", "eastus"}},
      {"qatarcentral", {"uaenorth", "israelcentral", "westeurope", "northeurope", "eastus"}},
      {"israelcentral", {"uaenorth", "qatarcentral", "westeurope", "northeurope", "eastus"}},

      // Africa
      {"southafricanorth", {"westeurope", "northeurope", "uaenorth", "eastus"}},

      // Asia Pacific: East Asia
      {"japaneast", {"japanwest", "koreacentral", "taiwannorth", "eastasia", "southeastasia", "westus2"}},
      {"japanwest", {"japaneast", "koreacentral", "taiwannorth", "eastasia", "southeastasia", "westus2"}},
      {"koreacentral", {"japaneast", "japanwest", "taiwannorth", "eastasia", "southeastasia", "westus2"}},
      {"eastasia", {"taiwannorth", "southeastasia", "japaneast", "koreacentral", "australiaeast", "westus2"}},
      {"taiwannorth", {"eastasia", "japaneast", "koreacentral", "southeastasia", "westus2"}},

      // Asia Pacific: Southeast Asia
      {"southeastasia", {"malaysiawest", "eastasia", "japaneast", "australiaeast", "centralindia", "westus2"}},
      {"malaysiawest", {"southeastasia", "eastasia", "centralindia", "japaneast", "westus2"}},

      // Asia Pacific: India
      {"centralindia", {"southindia", "jioindiawest", "southeastasia", "uaenorth", "westeurope", "eastus"}},
      {"southindia", {"centralindia", "jioindiawest", "southeastasia", "uaenorth", "westeurope", "eastus"}},
      {"jioindiawest", {"centralindia", "southindia", "southeastasia", "uaenorth", "westeurope", "eastus"}},

      // Oceania
      {"australiaeast", {"australiasoutheast", "southeastasia", "japaneast", "westus2"}},
      {"australiasoutheast", {"australiaeast", "southeastasia", "japaneast", "westus2"}},
  };

  return map;
}

/// Proximal-region lookup table, built once on first use.
const std::map<std::string, std::vector<std::string>>& ProximalRegions() {
  static const std::map<std::string, std::vector<std::string>> kMap = BuildProximalRegions();
  return kMap;
}

std::string DescribeStatus(int status) {
  return status == 0 ? "transport failure" : ("HTTP " + std::to_string(status));
}

}  // namespace

bool IsRegionRetryableStatus(int status) {
  return status == 0 || status == 408 || status == 429 || status == 500 || status == 502 ||
         status == 503 || status == 504;
}

RegionFallback::RegionFallback(ILogger& logger, bool enabled, RandomPicker random_picker)
    : logger_(logger), enabled_(enabled), random_picker_(std::move(random_picker)) {
  if (random_picker_) {
    return;
  }

  // Default RNG: per-instance, seeded from std::random_device. Jitter need not be strong.
  auto rng = std::make_shared<std::mt19937_64>(
      static_cast<uint64_t>(std::random_device{}()));
  random_picker_ = [rng](std::size_t count) -> std::size_t {
    if (count == 0) {
      return 0;
    }

    return static_cast<std::size_t>((*rng)() % count);
  };
}

std::vector<std::string> BuildRegionFallbackChain(const std::string& start_region,
                                                  const RegionFallback::RandomPicker& picker) {
  std::vector<std::string> chain;
  std::set<std::string> seen;

  auto add = [&](const std::string& region) {
    if (region.empty()) {
      return;
    }

    auto normalized = ToLower(region);
    if (seen.insert(normalized).second) {
      chain.push_back(normalized);
    }
  };

  const auto start = ToLower(start_region);
  add(start);

  const auto& proximal_map = ProximalRegions();
  auto it = proximal_map.find(start);
  if (it != proximal_map.end()) {
    for (const auto& p : it->second) {
      add(p);
    }
  }

  // Last-ditch: one random known public region not already in the chain.
  std::vector<std::string> remaining;
  for (const auto& [region, _] : proximal_map) {
    if (seen.find(region) == seen.end()) {
      remaining.push_back(region);
    }
  }

  if (!remaining.empty() && picker) {
    add(remaining[picker(remaining.size())]);
  }

  return chain;
}

FallbackResult RegionFallback::Execute(const std::string& start_region, const AttemptFn& attempt) {
  const auto normalized_start = ToLower(start_region);

  if (!enabled_) {
    return FallbackResult{attempt(normalized_start), normalized_start};
  }

  const auto chain = BuildRegionFallbackChain(normalized_start, random_picker_);
  std::string failure_summary;

  for (const auto& region : chain) {
    http::HttpResponse response = attempt(region);

    if (!IsRegionRetryableStatus(response.status)) {
      // Non-retryable: a 2xx success OR a permanent error (e.g. 404). Either way, stop here and let the caller
      // inspect the status. Record the sticky region only on success so a permanent error doesn't pin a bad region.
      if (response.status >= 200 && response.status < 300) {
        sticky_ = region;
      }

      return FallbackResult{std::move(response), region};
    }

    if (!failure_summary.empty()) {
      failure_summary += ", ";
    }

    failure_summary += region + "(" + DescribeStatus(response.status) + ")";
    logger_.Log(LogLevel::Warning,
                "RegionFallback: region '" + region + "' unhealthy (" +
                    DescribeStatus(response.status) + "); trying next candidate.");
  }

  FL_THROW(FOUNDRY_LOCAL_ERROR_NETWORK,
           "region fallback exhausted all " + std::to_string(chain.size()) +
               " candidate region(s): " + failure_summary);
}

}  // namespace fl
