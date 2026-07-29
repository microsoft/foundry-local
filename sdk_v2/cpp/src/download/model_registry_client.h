// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "http/http_client.h"

#include <functional>
#include <memory>
#include <string>

namespace fl {

class ILogger;
class RegionFallback;

/// Response-aware HTTP GET (status + headers + body) — lets the region-fallback
/// engine classify region-health failures by status code.
using HttpGetResponseFn = std::function<http::HttpResponse(const std::string& url)>;

/// Result from resolving a model's asset ID against the Azure model registry.
struct ModelContainer {
  std::string blob_sas_uri;
  std::string description;
};

/// Client for the Azure model registry API.
/// Resolves a model's asset_id to a Blob Storage SAS URI for downloading.
class ModelRegistryClient {
 public:
  /// @param region Default Azure region for the model registry endpoint (e.g. "centralus").
  ///               Used when ResolveModelContainer is called without a per-call region.
  /// @param logger Logger used for diagnostics. Tests that override the HTTP seam with a
  ///               synchronous fake can pass a sink logger.
  /// @param fallback Region-fallback engine. It can be constructed as disabled when only one region should be tried.
  /// @param http_get HTTP GET implementation. The default uses `http::HttpGetWithResponse`.
  ModelRegistryClient(std::string region,
                      ILogger& logger,
                      std::unique_ptr<RegionFallback> fallback,
                      HttpGetResponseFn http_get = {});

  /// Resolve a model's asset_id (URI) to a blob storage SAS URI.
  /// Uses anonymous access (no token) for FoundryLocal models. When `region` is
  /// non-empty it selects the regional registry endpoint for this call; otherwise
  /// the default region from construction is used.
  /// Throws fl::Exception on failure.
  ModelContainer ResolveModelContainer(const std::string& asset_id, const std::string& region = "");

 private:
  /// Parse a registry response body into a ModelContainer. Throws on empty/invalid input.
  ModelContainer ParseContainer(const std::string& body, const std::string& asset_id) const;

  std::string default_region_;
  HttpGetResponseFn http_get_;
  std::unique_ptr<RegionFallback> fallback_;
};

}  // namespace fl
