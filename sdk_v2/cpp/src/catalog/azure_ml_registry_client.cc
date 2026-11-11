// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "catalog/azure_ml_registry_client.h"

#include "exception.h"
#include "http/curl_transport.h"
#include "util/string_utils.h"

#include <azure/core/exception.hpp>
#include <azure/core/io/body_stream.hpp>
#include <azure/core/url.hpp>
#if defined(FOUNDRY_LOCAL_USE_WINHTTP_TRANSPORT)
#include <azure/core/http/win_http_transport.hpp>
#else
#include <azure/core/http/curl_transport.hpp>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <set>

namespace fl {
namespace {

using Json = nlohmann::json;
using Azure::Core::Url;
using Azure::Core::Http::HttpMethod;
constexpr auto kApiQuery = "?api-version=2021-10-01-dataplanepreview";

std::string RequiredString(const Json& value, const char* key) {
  const auto it = value.find(key);
  if (it == value.end() || !it->is_string() || it->get_ref<const std::string&>().empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_NETWORK, "Azure ML response missing string: ", key);
  }
  return it->get<std::string>();
}

void ValidateSegment(const std::string& value) {
  if (value.empty() || value == "." || value == ".." ||
      !std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
               (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.';
      })) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "invalid Azure ML resource path segment");
  }
}

void ValidateResourceGroup(const std::string& value) {
  // ARM discovery owns Unicode category validation; preserve its names without locale-dependent classification.
  if (value.empty() || !IsValidUtf8(value) || value.back() == '.' ||
      std::count_if(value.begin(), value.end(), [](unsigned char ch) { return (ch & 0xC0) != 0x80; }) > 90 ||
      !std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return ch >= 0x80 || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
               (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == '(' || ch == ')';
      })) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "invalid Azure ML resource group name");
  }
}

void ValidateHost(const Url& url, const std::string& suffix) {
  const auto host = ToLower(url.GetHost());
  if (url.GetScheme() != "https" || host.size() <= suffix.size() ||
      !host.ends_with(suffix) || (url.GetPort() != 0 && url.GetPort() != 443)) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_NETWORK, "Azure ML returned an endpoint outside the allowed Azure ML domains");
  }
}

std::string CleanPath(const Url& url) {
  auto path = Url::Decode(url.GetPath());
  while (!path.empty() && path.front() == '/') path.erase(0, 1);
  while (!path.empty() && path.back() == '/') path.pop_back();
  return path;
}

std::unique_ptr<Azure::Core::Http::_internal::HttpPipeline> MakePipeline(
    const std::shared_ptr<Azure::Core::Credentials::TokenCredential>& credential, const std::string& scope,
    const std::shared_ptr<Azure::Core::Http::HttpTransport>& http_transport) {
  using namespace Azure::Core::Http::Policies;
  Azure::Core::Credentials::TokenRequestContext tokens;
  tokens.Scopes = {scope};
  RetryOptions retry;
  retry.MaxRetries = 2;
  retry.MaxRetryDelay = std::chrono::seconds(5);
  retry.StatusCodes.insert(Azure::Core::Http::HttpStatusCode::TooManyRequests);
  TransportOptions transport;
  transport.Transport = http_transport;
  if (!transport.Transport) {
#if defined(FOUNDRY_LOCAL_USE_WINHTTP_TRANSPORT)
    transport.Transport = std::make_shared<Azure::Core::Http::WinHttpTransport>();
#else
    transport.Transport = std::make_shared<Azure::Core::Http::CurlTransport>(http::MakeCurlTransportOptions());
#endif
  }
  std::vector<std::unique_ptr<HttpPolicy>> policies;
  policies.push_back(std::make_unique<_internal::RetryPolicy>(retry));
  policies.push_back(std::make_unique<_internal::BearerTokenAuthenticationPolicy>(credential, tokens));
  policies.push_back(std::make_unique<_internal::TransportPolicy>(transport));
  return std::make_unique<Azure::Core::Http::_internal::HttpPipeline>(policies);
}

}  // namespace

std::string AzureMlRegistryClient::ValidateRegistryName(std::string name) {
  ValidateSegment(name);
  if (name.size() < 2 || name.size() > 32 ||
      !std::isalpha(static_cast<unsigned char>(name.front())) ||
      !std::isalnum(static_cast<unsigned char>(name.back())) ||
      name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != std::string::npos) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "invalid Azure ML registry name");
  }
  return ToLower(std::move(name));
}

AzureMlRegistryClient::AzureMlRegistryClient(
    std::string registry, std::shared_ptr<Azure::Core::Credentials::TokenCredential> credential,
    std::shared_ptr<Azure::Core::Http::HttpTransport> transport)
    : registry_(ValidateRegistryName(std::move(registry))) {
  if (!credential) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "test catalog requires an Azure credential");
  }
  discovery_pipeline_ = MakePipeline(credential, "https://ml.azure.com/.default", transport);
  registry_pipeline_ = MakePipeline(credential, "https://management.azure.com/.default", transport);
}

std::optional<Json> AzureMlRegistryClient::Request(const HttpMethod& method, const std::string& url,
                                                  bool discovery, const Json* body, bool allow_not_found) {
  const auto context = Azure::Core::Context{}.WithDeadline(
      Azure::DateTime(std::chrono::system_clock::now() + std::chrono::seconds(90)));
  const auto text = body ? body->dump() : std::string{};
  const std::vector<uint8_t> bytes(text.begin(), text.end());
  Azure::Core::IO::MemoryBodyStream stream(bytes);
  Azure::Core::Http::Request request(method, Url(url), &stream);
  request.SetHeader("Accept", "application/json");
  request.SetHeader("User-Agent", "FoundryLocalTestCatalog/1.0");
  if (body) request.SetHeader("Content-Type", "application/json");
  try {
    auto response = (discovery ? discovery_pipeline_ : registry_pipeline_)->Send(request, context);
    const auto status = static_cast<int>(response->GetStatusCode());
    if (allow_not_found && response->GetStatusCode() == Azure::Core::Http::HttpStatusCode::NotFound) {
      return std::nullopt;
    }
    if (status < 200 || status >= 300) {
      std::string request_id;
      for (const auto* header : {"x-ms-request-id", "x-ms-correlation-request-id"}) {
        const auto it = response->GetHeaders().find(header);
        if (it != response->GetHeaders().end()) request_id += std::string("; ") + header + "=" + it->second;
      }
      FL_THROW(FOUNDRY_LOCAL_ERROR_NETWORK, "Azure ML ", discovery ? "registry discovery" : "model/artifact request",
               " failed: HTTP ", status, request_id,
               ". Check registry name, login/federation, model-read and artifact-access permissions.");
    }
    constexpr size_t kMaxResponseBytes = 16 * 1024 * 1024;
    std::string contents;
    if (auto input = response->ExtractBodyStream()) {
      std::array<uint8_t, 8192> buffer{};
      for (;;) {
        const auto count = input->Read(buffer.data(), buffer.size(), context);
        if (!count) break;
        if (contents.size() + count > kMaxResponseBytes) {
          FL_THROW(FOUNDRY_LOCAL_ERROR_NETWORK, "Azure ML response exceeds 16 MiB");
        }
        contents.append(reinterpret_cast<const char*>(buffer.data()), count);
      }
    } else {
      if (response->GetBody().size() > kMaxResponseBytes) {
        FL_THROW(FOUNDRY_LOCAL_ERROR_NETWORK, "Azure ML response exceeds 16 MiB");
      }
      contents.assign(response->GetBody().begin(), response->GetBody().end());
    }
    return Json::parse(contents);
  } catch (const Azure::Core::Credentials::AuthenticationException&) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_NETWORK,
             "Test catalog authentication failed. Run az login for azure_cli, or check "
             "AZURE_TENANT_ID, AZURE_CLIENT_ID and AZURE_FEDERATED_TOKEN_FILE for workload_identity.");
  } catch (const Azure::Core::Http::TransportException&) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_NETWORK, "Azure ML transport failed; check network, proxy and TLS configuration");
  } catch (const Azure::Core::RequestFailedException& error) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_NETWORK, "Azure ML request failed: HTTP ",
             static_cast<int>(error.StatusCode), "; request ID: ", error.RequestId);
  } catch (const Json::exception&) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_NETWORK, "Azure ML returned malformed JSON");
  }
}

void AzureMlRegistryClient::Discover() {
  std::lock_guard lock(discovery_mutex_);
  if (!base_url_.empty()) return;
  const auto discovery = Request(HttpMethod::Get,
      "https://eastus.api.azureml.ms/registrymanagement/v1.0/registries/" + registry_ + "/discovery", true).value();
  const auto subscription = RequiredString(discovery, "subscriptionId");
  const auto group = RequiredString(discovery, "resourceGroup");
  ValidateSegment(subscription);
  ValidateResourceGroup(group);
  Url endpoint(RequiredString(discovery, "primaryRegionResourceProviderUri"));
  is_test_endpoint_ = ToLower(endpoint.GetHost()).ends_with(".experiments.azureml-test.net");
  ValidateHost(endpoint, is_test_endpoint_ ? ".experiments.azureml-test.net" : ".experiments.azureml.net");
  if (!endpoint.GetQueryParameters().empty() || !CleanPath(endpoint).empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_NETWORK, "Azure ML returned an unexpected registry endpoint path/query");
  }
  resource_id_ = "/subscriptions/" + subscription + "/resourceGroups/" + group +
      "/providers/Microsoft.MachineLearningServices/registries/" + registry_;
  endpoint.AppendPath("mferp/managementfrontend/subscriptions/" + subscription + "/resourceGroups/" +
      Url::Encode(group) + "/providers/Microsoft.MachineLearningServices/registries/" + registry_);
  base_url_ = endpoint.GetAbsoluteUrl();
}

const std::string& AzureMlRegistryClient::ResourceId() {
  Discover();
  return resource_id_;
}

std::vector<Json> AzureMlRegistryClient::List(const std::string& url) {
  std::vector<Json> items;
  std::set<std::string> visited;
  const Url base(base_url_);
  auto next = url;
  while (!next.empty()) {
    const Url page(next);
    if (page.GetScheme() != base.GetScheme() || page.GetHost() != base.GetHost() ||
        page.GetPort() != base.GetPort() || !page.GetPath().starts_with(base.GetPath() + "/") ||
        !visited.insert(next).second) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_NETWORK, "Azure ML returned an invalid pagination link");
    }
    auto response = Request(HttpMethod::Get, next).value();
    if (!response.contains("value") || !response["value"].is_array()) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_NETWORK, "Azure ML list response missing value array");
    }
    for (auto& item : response["value"]) items.push_back(std::move(item));
    next = response.contains("nextLink") && !response["nextLink"].is_null()
        ? RequiredString(response, "nextLink") : std::string{};
  }
  return items;
}

std::vector<Json> AzureMlRegistryClient::ListModelVersions() {
  Discover();
  std::vector<Json> versions;
  for (const auto& model : List(base_url_ + "/models" + kApiQuery)) {
    const auto name = RequiredString(model, "name");
    ValidateSegment(name);
    for (auto& version : List(base_url_ + "/models/" + name + "/versions" + kApiQuery)) {
      version["foundryVariantName"] = name;
      versions.push_back(std::move(version));
    }
  }
  return versions;
}

std::optional<Json> AzureMlRegistryClient::GetModelVersion(const std::string& name, const std::string& version) {
  ValidateSegment(name);
  ValidateSegment(version);
  Discover();
  auto model = Request(HttpMethod::Get, base_url_ + "/models/" + name + "/versions/" + version + kApiQuery,
                       false, nullptr, true);
  if (model) (*model)["foundryVariantName"] = name;
  return model;
}

ModelContainer AzureMlRegistryClient::ResolveArtifacts(const std::string& name, int version) {
  const auto model = GetModelVersion(name, std::to_string(version));
  if (!model) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_NETWORK, "Azure ML model/artifact request failed: HTTP 404; model version not found: ",
             name, ":", version);
  }
  const auto artifact_uri = RequiredString(model->at("properties"), "modelUri");
  const Url artifact(artifact_uri);
  ValidateHost(artifact, ".blob.core.windows.net");
  const auto path = CleanPath(artifact);
  const auto slash = path.find('/');
  if (!artifact.GetQueryParameters().empty() || path.empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_NETWORK, "Azure ML model must reference a credential-free Blob container or prefix");
  }
  const Json body{{"assetId", "azureml://registries/" + registry_ + "/models/" + name +
                             "/versions/" + std::to_string(version)}, {"blobUri", artifact_uri}};
  const auto access = Request(HttpMethod::Post,
      base_url_ + "/datarefs/" + name + "/versions/" + std::to_string(version) + kApiQuery, false, &body).value();
  const auto& grant = access.at("blobReferenceForConsumption").at("credential");
  if (RequiredString(grant, "credentialType") != "SAS") {
    FL_THROW(FOUNDRY_LOCAL_ERROR_NETWORK, "Azure ML test catalog requires SAS artifact access");
  }
  const auto sas = RequiredString(grant, "sasUri");
  const Url container(sas);
  const auto query = container.GetQueryParameters();
  const auto artifact_host = ToLower(artifact.GetHost());
  const auto proxy_host = artifact_host.substr(0, artifact_host.find('.')) +
      "-blob.dataproxy.int.api.azureml-test.ms";
  const auto container_host = ToLower(container.GetHost());
  const bool test_proxy = is_test_endpoint_ && container_host == proxy_host;
  if (container.GetScheme() != "https" || (container_host != artifact_host && !test_proxy) ||
      (container.GetPort() != 0 && container.GetPort() != 443) ||
      CleanPath(container) != path.substr(0, slash) ||
      !query.contains("sig") || query.at("sig").empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_NETWORK, "Azure ML returned artifact access for an unexpected storage container");
  }
  ModelContainer result;
  result.blob_sas_uri = sas;
  if (slash != std::string::npos) {
    result.path_prefix = path.substr(slash + 1) + "/";
  } else {
    const auto& tags = model->at("properties").at("tags");
    if (tags.contains("directoryPath")) {
      const auto directory = RequiredString(tags, "directoryPath");
      ValidateSegment(directory);
      result.path_prefix = directory + "/";
    }
  }
  return result;
}

}  // namespace fl
