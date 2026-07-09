// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "http/http_client.h"
#include "logger.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace fl {

/// Outcome of a fallback run: the response and the region that produced it.
struct FallbackResult {
  http::HttpResponse response;
  std::string region;
};

/// True for statuses that indicate a region-health failure (retryable):
/// 0 (transport), 408, 429, 500, 502, 503, 504.
bool IsRegionRetryableStatus(int status);

/// Build the ordered fallback chain: [start] + proximal[start] + one random unused public region.
/// Deduplicated in order; all regions lowercased.
std::vector<std::string> BuildRegionFallbackChain(const std::string& start_region,
                                                  const std::function<std::size_t(std::size_t count)>& picker);

/// Executes a regional HTTP operation against an ordered chain of candidate Azure
/// regions when the preferred region is unhealthy. Classifies failures by HTTP
/// status; status==0 means a transport failure before any HTTP response arrived.
///
/// The sticky region (last success) and the RNG are per-instance state (no global mutable state).
class RegionFallback {
 public:
  /// Performs one HTTP attempt against `region`, returning the full response
  /// (status==0 means transport failure).
  using AttemptFn = std::function<http::HttpResponse(const std::string& region)>;

  /// Picks an index in [0, count) — injected for deterministic tests.
  using RandomPicker = std::function<std::size_t(std::size_t count)>;

  /// @param logger Diagnostics for fallback decisions.
  /// @param enabled When false, runs a single attempt against the start region with no fallback.
  /// @param random_picker Picks the last-ditch random region. The default uses a per-instance RNG.
  explicit RegionFallback(ILogger& logger, bool enabled = true, RandomPicker random_picker = {});

  /// Execute `attempt` across the candidate chain for `start_region`.
  /// - First non-retryable response (2xx or a permanent error like 404) is returned
  ///   immediately along with its region; the caller inspects the status.
  /// - Retryable responses (status 0/408/429/5xx) advance to the next candidate.
  /// - If every candidate is retryable-failed, throws fl::Exception with the chain.
  FallbackResult Execute(const std::string& start_region, const AttemptFn& attempt);

  /// Last region that successfully served a request, if any.
  std::optional<std::string> StickyRegion() const { return sticky_; }

 private:
  ILogger& logger_;
  bool enabled_;
  RandomPicker random_picker_;
  std::optional<std::string> sticky_;
};

}  // namespace fl
