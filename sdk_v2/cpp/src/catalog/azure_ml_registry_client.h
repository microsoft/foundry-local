// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "download/model_registry_client.h"

#include <azure/core/credentials/credentials.hpp>
#include <azure/core/internal/http/pipeline.hpp>
#include <nlohmann/json.hpp>

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace fl {

class AzureMlRegistryClient {
 public:
  AzureMlRegistryClient(std::string registry,
                        std::shared_ptr<Azure::Core::Credentials::TokenCredential> credential,
                        std::shared_ptr<Azure::Core::Http::HttpTransport> transport = nullptr);

  std::vector<nlohmann::json> ListModelVersions(const std::string& name = "");
  std::optional<nlohmann::json> GetModelVersion(const std::string& name, const std::string& version);
  ModelContainer ResolveArtifacts(const std::string& name, int version);

  const std::string& ResourceId();
  static std::string ValidateRegistryName(std::string name);

 private:
  void Discover();
  std::optional<nlohmann::json> Request(const Azure::Core::Http::HttpMethod& method, const std::string& url,
                                       bool discovery = false, const nlohmann::json* body = nullptr,
                                       bool allow_not_found = false);
  std::vector<nlohmann::json> List(const std::string& url, bool allow_not_found = false);

  std::string registry_;
  std::string base_url_;
  std::string resource_id_;
  bool is_test_endpoint_ = false;
  std::mutex discovery_mutex_;
  std::unique_ptr<Azure::Core::Http::_internal::HttpPipeline> discovery_pipeline_;
  std::unique_ptr<Azure::Core::Http::_internal::HttpPipeline> registry_pipeline_;
};

}  // namespace fl
