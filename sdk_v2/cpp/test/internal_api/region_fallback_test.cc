// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// Tests for the region-fallback engine:
//   - candidate-chain construction (start + proximal + random tail, deduped)
//   - retryable vs permanent status classification
//   - sticky-region recording per endpoint
//   - fallback to the next healthy region, and exhaustion behavior
//
#include "util/region_fallback.h"

#include "exception.h"
#include "http/http_client.h"
#include "logger.h"

#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

using namespace fl;

namespace {

http::HttpResponse Resp(int status) {
  http::HttpResponse r;
  r.status = status;
  r.body = "{}";
  return r;
}

// Deterministic picker: always selects the first remaining region.
RegionFallback::RandomPicker FirstPicker() {
  return [](std::size_t) -> std::size_t { return 0; };
}

}  // namespace

// ========================================================================
// Candidate chain
// ========================================================================

TEST(RegionFallbackTest, ChainStartsWithStartThenProximalThenRandom) {
  auto chain = BuildRegionFallbackChain("eastus", FirstPicker());

  ASSERT_FALSE(chain.empty());
  EXPECT_EQ(chain.front(), "eastus");
  // eastus proximal: eastus2, centralus, southcentralus, westus2, westeurope
  EXPECT_EQ(chain[1], "eastus2");
  EXPECT_EQ(chain[2], "centralus");
  // The last element is the random tail (a known public region not already present).
  EXPECT_GE(chain.size(), 7u);
}

TEST(RegionFallbackTest, ChainIsDedupedAndLowercased) {
  auto chain = BuildRegionFallbackChain("EastUS", FirstPicker());
  EXPECT_EQ(chain.front(), "eastus");

  std::set<std::string> unique(chain.begin(), chain.end());
  EXPECT_EQ(unique.size(), chain.size()) << "chain must contain no duplicates";
}

TEST(RegionFallbackTest, UnknownStartRegionStillProducesChain) {
  auto chain = BuildRegionFallbackChain("madeupregion", FirstPicker());
  ASSERT_FALSE(chain.empty());
  EXPECT_EQ(chain.front(), "madeupregion");
  // No proximal entry, but the random tail still appends one known region.
  EXPECT_GE(chain.size(), 2u);
}

// ========================================================================
// Status classification
// ========================================================================

TEST(RegionFallbackTest, RetryableStatusClassification) {
  for (int s : {0, 408, 429, 500, 502, 503, 504}) {
    EXPECT_TRUE(IsRegionRetryableStatus(s)) << "status " << s << " should be retryable";
  }

  for (int s : {200, 201, 400, 401, 403, 404}) {
    EXPECT_FALSE(IsRegionRetryableStatus(s)) << "status " << s << " should be permanent";
  }
}

// ========================================================================
// Execute
// ========================================================================

TEST(RegionFallbackTest, SuccessOnFirstRegionRecordsSticky) {
  StderrLogger logger;
  RegionFallback fallback(logger, true, FirstPicker());

  std::vector<std::string> attempted;
  auto result = fallback.Execute("eastus",
                                 [&](const std::string& region) {
                                   attempted.push_back(region);
                                   return Resp(200);
                                 });

  EXPECT_EQ(result.region, "eastus");
  EXPECT_EQ(result.response.status, 200);
  ASSERT_EQ(attempted.size(), 1u);
  EXPECT_EQ(attempted[0], "eastus");

  auto sticky = fallback.StickyRegion();
  ASSERT_TRUE(sticky.has_value());
  EXPECT_EQ(*sticky, "eastus");
}

TEST(RegionFallbackTest, FallsThroughToNextHealthyRegion) {
  StderrLogger logger;
  RegionFallback fallback(logger, true, FirstPicker());

  std::vector<std::string> attempted;
  auto result = fallback.Execute("eastus",
                                 [&](const std::string& region) {
                                   attempted.push_back(region);
                                   // First region 503, second succeeds.
                                   return Resp(attempted.size() == 1 ? 503 : 200);
                                 });

  EXPECT_EQ(result.response.status, 200);
  EXPECT_EQ(result.region, "eastus2");  // second candidate in eastus chain
  EXPECT_EQ(attempted[0], "eastus");
  EXPECT_EQ(attempted[1], "eastus2");

  auto sticky = fallback.StickyRegion();
  ASSERT_TRUE(sticky.has_value());
  EXPECT_EQ(*sticky, "eastus2");
}

TEST(RegionFallbackTest, PermanentErrorStopsImmediatelyAndDoesNotPinSticky) {
  StderrLogger logger;
  RegionFallback fallback(logger, true, FirstPicker());

  int calls = 0;
  auto result = fallback.Execute("eastus",
                                 [&](const std::string&) {
                                   ++calls;
                                   return Resp(404);
                                 });

  EXPECT_EQ(result.response.status, 404);
  EXPECT_EQ(calls, 1) << "a permanent error must not try other regions";
  EXPECT_FALSE(fallback.StickyRegion().has_value());
}

TEST(RegionFallbackTest, ExhaustingAllRegionsThrows) {
  StderrLogger logger;
  RegionFallback fallback(logger, true, FirstPicker());

  int calls = 0;
  try {
    fallback.Execute("eastus",
                     [&](const std::string&) {
                       ++calls;
                       return Resp(503);
                     });
    FAIL() << "expected fl::Exception";
  } catch (const fl::Exception& e) {
    EXPECT_EQ(e.code(), FOUNDRY_LOCAL_ERROR_NETWORK);
  }

  EXPECT_GE(calls, 7) << "every candidate region should have been attempted";
}

TEST(RegionFallbackTest, DisabledRunsSingleAttemptNoFallback) {
  StderrLogger logger;
  RegionFallback fallback(logger, /*enabled=*/false);

  int calls = 0;
  auto result = fallback.Execute("eastus",
                                 [&](const std::string& region) {
                                   ++calls;
                                   EXPECT_EQ(region, "eastus");
                                   return Resp(503);  // even a retryable failure isn't retried
                                 });

  EXPECT_EQ(calls, 1);
  EXPECT_EQ(result.response.status, 503);
  EXPECT_EQ(result.region, "eastus");
}

TEST(RegionFallbackTest, StickyUpdatesToLastSuccessfulRegion) {
  StderrLogger logger;
  RegionFallback fallback(logger, true, FirstPicker());

  fallback.Execute("eastus", [&](const std::string&) { return Resp(200); });
  ASSERT_TRUE(fallback.StickyRegion().has_value());
  EXPECT_EQ(*fallback.StickyRegion(), "eastus");

  fallback.Execute("westus2", [&](const std::string&) { return Resp(200); });
  EXPECT_EQ(*fallback.StickyRegion(), "westus2");
}
