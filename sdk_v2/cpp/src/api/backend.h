// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <foundry_local/foundry_local_c.h>

#include "inferencing/session/streaming_callback.h"

#include <functional>
#include <memory>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fl {
enum class CatalogType;
struct Configuration;
struct EpDownloadResult;
struct Item;
class KeyValuePairs;
struct ModelInfo;
struct Request;
struct RequestBudget;
struct Response;
class Session;
struct ToolDefinition;
}  // namespace fl

namespace fl::api {

enum class BackendKind { Embedded,
                         Hosted };

struct InputOutputInfo {
  const Item* const* inputs;
  size_t num_inputs;
  const Item* const* outputs;
  size_t num_outputs;
};

using EpProgressCallback = std::function<bool(const std::string&, float)>;
using DeviceProviders = std::map<std::string, std::vector<std::string>>;

class BackendObject {
 public:
  BackendKind Kind() const noexcept { return kind_; }
  virtual ~BackendObject() = default;

  BackendObject(const BackendObject&) = delete;
  BackendObject& operator=(const BackendObject&) = delete;

 protected:
  explicit BackendObject(BackendKind kind) : kind_(kind) {}

 private:
  const BackendKind kind_;
};

class PreflightBackend : public BackendObject {
 public:
  using BackendObject::BackendObject;
  virtual RequestBudget Execute() = 0;
};

class SessionBackend : public BackendObject {
 public:
  using BackendObject::BackendObject;
  virtual void SetStreamingCallback(flStreamingCallback callback, void* user_data) = 0;
  virtual void SetSessionOptions(const KeyValuePairs& options) = 0;
  virtual void ProcessRequest(const Request& request, Response& response,
                              std::optional<StreamingCallback> callback = std::nullopt) = 0;
  virtual void AddToolDefinition(ToolDefinition definition) = 0;
  virtual bool RemoveToolDefinition(const std::string& name) = 0;
  virtual size_t TurnCount() const = 0;
  virtual void UndoTurns(size_t count) = 0;
  virtual std::unique_ptr<PreflightBackend> CreateRequestPreflight(const Request& request) const = 0;
};

class ModelBackend : public BackendObject {
 public:
  using BackendObject::BackendObject;
  virtual const ModelInfo& Info() const = 0;
  virtual InputOutputInfo GetInputOutputInfo() const = 0;
  virtual bool IsCached() const = 0;
  virtual void RefreshCachedState() = 0;
  virtual void ForgetCachedState() = 0;
  virtual const std::string& GetPath() const = 0;
  virtual void Download(std::function<int(float)> progress) = 0;
  virtual bool IsLoaded() const = 0;
  virtual void Load(std::optional<std::string_view> expected_path = std::nullopt) = 0;
  virtual void Unload() = 0;
  virtual void RemoveFromCache(std::optional<std::string_view> expected_path = std::nullopt) = 0;
  virtual std::vector<ModelBackend*> Variants() const = 0;
  virtual void SelectVariant(const ModelBackend& variant) = 0;
  virtual std::unique_ptr<SessionBackend> CreateSession() const = 0;
};

class CatalogBackend : public BackendObject {
 public:
  using BackendObject::BackendObject;
  virtual const std::string& GetName() const = 0;
  virtual std::vector<ModelBackend*> ListModels() const = 0;
  virtual ModelBackend* GetModel(const std::string& alias) const = 0;
  virtual ModelBackend* GetModelVariant(const std::string& model_id) const = 0;
  virtual ModelBackend* GetLatestVersion(const ModelBackend& model) const = 0;
  virtual std::vector<ModelBackend*> GetCachedModels() const = 0;
  virtual std::vector<ModelBackend*> GetLoadedModels() const = 0;
  virtual std::vector<ModelBackend*> GetModelVersions(const std::string& alias, const std::string& name,
                                                      int max_versions) = 0;
  virtual ModelBackend* RegisterModel(const std::string& path, const std::string& model_id,
                                      const ModelInfo& metadata) = 0;
  virtual void UnregisterModel(const std::string& alias_or_model_id) = 0;
};

class ManagerBackend : public BackendObject {
 public:
  using BackendObject::BackendObject;
  virtual CatalogBackend& GetCatalog(CatalogType type) = 0;
  // Capture an active selection for admitted execution, not for retaining catalog/discovery handles.
  virtual std::unique_ptr<ModelBackend> CaptureModel(const ModelBackend& model) = 0;
  // Loaded artifacts can outlive their catalog registrations. These operations remain exactly path-qualified.
  virtual bool IsModelArtifactLoaded(const std::string& model_id, const std::string& path) const = 0;
  virtual void UnloadModelArtifact(const std::string& model_id, const std::string& path) = 0;
  virtual void StartWebService() = 0;
  virtual const std::vector<std::string>& GetWebServiceUrls() const = 0;
  virtual void StopWebService() = 0;
  virtual std::span<const flEpInfo> GetDiscoverableEps() const = 0;
  virtual DeviceProviders GetRuntimeDevices() const = 0;
  virtual void SetCatalogRuntimeDevices(DeviceProviders devices) = 0;
  virtual EpDownloadResult DownloadAndRegisterEps(const std::vector<std::string>* names,
                                                  const EpProgressCallback& progress) = 0;
  virtual bool IsEpDownloadInProgress() const = 0;
  virtual void Shutdown() = 0;
  virtual bool IsShutdownRequested() const = 0;
};

std::unique_ptr<ManagerBackend> CreateEmbeddedBackend(const Configuration& configuration);
std::unique_ptr<SessionBackend> CreateEmbeddedSession(std::unique_ptr<Session> session);

// Internal callers may borrow a session only while its native owner remains alive.
std::unique_ptr<SessionBackend> BorrowEmbeddedSession(Session& session);

}  // namespace fl::api
