// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
// C API implementation — maps the sub-API vtable functions declared in foundry_local_c.h
// to internal C++ types.
#include <foundry_local/foundry_local_c.h>
#include <foundry_local/foundry_local_cpp.h>

#include "c_api_types.h"
#include "catalog.h"
#include "exception.h"
#include "version.h"
#include "items/audio_item.h"
#include "items/bytes_item.h"
#include "items/image_item.h"
#include "items/message_item.h"
#include "items/speech_result_item.h"
#include "items/speech_segment_item.h"
#include "items/tensor_item.h"
#include "items/text_item.h"
#include "items/tool_call_item.h"
#include "items/tool_result_item.h"
#include "manager.h"
#include "ep_detection/ep_bootstrapper.h"

#include <functional>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <vector>

// ========================================================================
// Helpers
// ========================================================================

/// Create an flStatus on the heap. Caller owns and must release via Status_Release.
static flStatus* MakeStatus(flErrorCode code, std::string msg);

/// Catch any C++ exception and return it as an flStatus*.
/// Usage: try { ... } catch (...) { return HandleException(); }
static flStatus* HandleException() FL_NO_EXCEPTION;

// DuplicateString available for Phase 4 when wiring real implementations.
// static char* DuplicateString(const std::string& s);

/// Stub status for unimplemented functions.
#define STUB_NOT_IMPLEMENTED() return MakeStatus(FOUNDRY_LOCAL_ERROR_NOT_IMPLEMENTED, "not implemented")

// ========================================================================
// Opaque type definitions — types with state that stay in c_api.cc
// Empty pass-through types are in c_api_types.h.
// ========================================================================

// --- Status ---
struct flStatus {
  flErrorCode code;
  std::string message;
};

// --- ModelList ---
struct flModelList {
  std::vector<flModel*> items;  // non-owning pointers into catalog
};

// --- Catalog ---
struct flCatalog {
  fl::ICatalog& impl;
};

// --- Manager ---
struct flManager {
  fl::Manager& impl;
  std::unique_ptr<flCatalog> catalog;  // stores the flCatalog wrapper around impl.GetCatalog()
  mutable std::vector<const char*> urls_cache;
};

// ========================================================================
// ========================================================================
// Helper implementations
// ========================================================================

// Singleton OOM status returned when we cannot allocate a new flStatus on the heap.
// The "out of memory" message fits in std::string SSO on all supported toolchains, so the
// constructor does not allocate. Status_ReleaseImpl checks against this pointer and skips delete.
static flStatus* GetOomStatus() FL_NO_EXCEPTION {
  static flStatus s{FOUNDRY_LOCAL_ERROR_INTERNAL, std::string("out of memory")};
  return &s;
}

static flStatus* MakeStatus(flErrorCode code, std::string msg) {
  auto* s = new (std::nothrow) flStatus();
  if (!s) {
    return GetOomStatus();
  }
  s->code = code;
  s->message = std::move(msg);
  return s;
}

static flStatus* HandleException() FL_NO_EXCEPTION {
  try {
    try {
      throw;
    } catch (const std::bad_alloc&) {
      return GetOomStatus();
    } catch (const fl::Exception& ex) {
      return MakeStatus(ex.code(), ex.what());
    } catch (const std::exception& ex) {
      return MakeStatus(FOUNDRY_LOCAL_ERROR_INTERNAL, ex.what());
    } catch (...) {
      return MakeStatus(FOUNDRY_LOCAL_ERROR_INTERNAL, "unknown error");
    }
  } catch (...) {
    // Any allocation inside the catch arms (e.g., constructing the message std::string) may itself
    // throw bad_alloc. Funnel that to the pre-allocated OOM singleton so no exception escapes the C ABI.
    return GetOomStatus();
  }
}

// Returns FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT when data_size > 0 but both data pointers are null.
// (data == null, mutable_data == null, data_size == 0) is valid (empty payload / URI-based).
static flStatus* ValidateDataPointers(const void* data, const void* mutable_data, size_t data_size,
                                      const char* field_name) {
  if (data_size > 0 && data == nullptr && mutable_data == nullptr) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
                      std::string(field_name) + ": data_size > 0 but both data and mutable_data are null");
  }
  return nullptr;
}

#define API_IMPL_BEGIN try {
#define API_IMPL_END          \
  }                           \
  catch (...) {               \
    return HandleException(); \
  }

// DuplicateString — reserved for Phase 4 when string ownership transfer is needed.
// static char* DuplicateString(const std::string& s) {
//   char* result = static_cast<char*>(std::malloc(s.size() + 1));
//   if (result) {
//     std::memcpy(result, s.c_str(), s.size() + 1);
//   }
//   return result;
// }

// ========================================================================
// Status API
// ========================================================================

FL_API_STATUS_IMPL(Status_CreateImpl, flErrorCode error_code, const char* error_msg) {
  API_IMPL_BEGIN
  return MakeStatus(error_code, error_msg ? std::string(error_msg) : std::string());
  API_IMPL_END
}

static void FL_API_CALL Status_ReleaseImpl(flStatus* status) FL_NO_EXCEPTION {
  if (status && status != GetOomStatus()) {
    delete status;
  }
}

static flErrorCode FL_API_CALL Status_GetErrorCodeImpl(const flStatus* status) FL_NO_EXCEPTION {
  return status ? status->code : FOUNDRY_LOCAL_OK;
}

static const char* FL_API_CALL Status_GetErrorMessageImpl(const flStatus* status) FL_NO_EXCEPTION {
  return status ? status->message.c_str() : "";
}

// ========================================================================
// Configuration API
// ========================================================================

FL_API_STATUS_IMPL(Configuration_CreateImpl, const char* app_name, flConfiguration** out_config) {
  API_IMPL_BEGIN
  if (!app_name || !out_config) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "app_name and out_config must not be null");
  }

  auto cfg = std::make_unique<fl::Configuration>();
  cfg->app_name = app_name;
  *out_config = AsHandle<flConfiguration>(cfg.release());
  return nullptr;
  API_IMPL_END
}

static void FL_API_CALL Configuration_ReleaseImpl(flConfiguration* config) FL_NO_EXCEPTION {
  delete AsImpl(config);
}

FL_API_STATUS_IMPL(SetAppDataDirImpl, flConfiguration* config, const char* dir) {
  API_IMPL_BEGIN
  if (!config || !dir) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "null argument");
  }

  AsImpl(config)->app_data_dir = dir;
  return nullptr;
  API_IMPL_END
}

FL_API_STATUS_IMPL(SetLogsDirImpl, flConfiguration* config, const char* dir) {
  API_IMPL_BEGIN
  if (!config || !dir) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "null argument");
  }

  AsImpl(config)->logs_dir = dir;
  return nullptr;
  API_IMPL_END
}

FL_API_STATUS_IMPL(SetModelCacheDirImpl, flConfiguration* config, const char* dir) {
  API_IMPL_BEGIN
  if (!config || !dir) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "null argument");
  }

  AsImpl(config)->model_cache_dir = dir;
  return nullptr;
  API_IMPL_END
}

FL_API_STATUS_IMPL(SetDefaultLogLevelImpl, flConfiguration* config, flLogLevel level) {
  API_IMPL_BEGIN
  if (!config) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "null config");
  }

  AsImpl(config)->log_level = static_cast<fl::LogLevel>(level);
  return nullptr;
  API_IMPL_END
}

FL_API_STATUS_IMPL(AddCatalogUrlImpl, flConfiguration* config, const char* url,
                   const char* filter_override) {
  API_IMPL_BEGIN
  if (!config || !url) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "null argument");
  }

  AsImpl(config)->catalog_urls.emplace_back(
      url,
      filter_override ? std::optional<std::string>{filter_override} : std::nullopt);
  return nullptr;
  API_IMPL_END
}

FL_API_STATUS_IMPL(AddWebServiceEndpointImpl, flConfiguration* config, const char* url) {
  API_IMPL_BEGIN
  if (!config || !url) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "null argument");
  }

  AsImpl(config)->web_service_endpoints.emplace_back(url);
  return nullptr;
  API_IMPL_END
}

FL_API_STATUS_IMPL(SetAdditionalOptionsImpl, flConfiguration* config, const flKeyValuePairs* options) {
  API_IMPL_BEGIN
  if (!config || !options) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "null argument");
  }

  auto* cfg = AsImpl(config);
  const auto& kvps = *AsImpl(options);
  for (const auto& [key, value] : kvps.Entries()) {
    cfg->additional_options[key] = value;
  }

  return nullptr;
  API_IMPL_END
}

FL_API_STATUS_IMPL(SetExternalServiceUrlImpl, flConfiguration* config, const char* url) {
  API_IMPL_BEGIN
  if (!config || !url) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "null argument");
  }

  AsImpl(config)->external_service_url = url;
  return nullptr;
  API_IMPL_END
}

FL_API_STATUS_IMPL(SetCatalogRegionImpl, flConfiguration* config, const char* region) {
  API_IMPL_BEGIN
  if (!config || !region) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "null argument");
  }

  AsImpl(config)->catalog_region = region;
  return nullptr;
  API_IMPL_END
}

static const flConfigurationApi g_configuration_api = {
    Configuration_CreateImpl,
    Configuration_ReleaseImpl,
    SetDefaultLogLevelImpl,
    SetAppDataDirImpl,
    SetLogsDirImpl,
    SetModelCacheDirImpl,
    AddCatalogUrlImpl,
    SetCatalogRegionImpl,
    AddWebServiceEndpointImpl,
    SetExternalServiceUrlImpl,
    SetAdditionalOptionsImpl,
};

// ========================================================================
// Manager API
// ========================================================================

FL_API_STATUS_IMPL(Manager_CreateImpl, const flConfiguration* config, flManager** out_manager) {
  API_IMPL_BEGIN
  if (!config || !out_manager) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "config and out_manager must not be null");
  }

  const auto* cfg = AsImpl(config);
  if (cfg->app_name.empty()) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "app_name must not be empty");
  }

  auto& mgr = fl::Manager::Create(*cfg);
  auto wrapper = std::make_unique<flManager>(flManager{mgr, nullptr, {}});
  wrapper->catalog = std::make_unique<flCatalog>(flCatalog{mgr.GetCatalog()});
  *out_manager = wrapper.release();
  return nullptr;
  API_IMPL_END
}

static void FL_API_CALL Manager_ReleaseImpl(flManager* manager) FL_NO_EXCEPTION {
  if (!manager) {
    return;
  }

  fl::Manager::Destroy();
  delete manager;
}

FL_API_STATUS_IMPL(Manager_GetCatalogImpl, const flManager* manager, flCatalog** out_catalog) {
  API_IMPL_BEGIN
  if (!manager || !out_catalog) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "null argument");
  }

  *out_catalog = manager->catalog.get();
  return nullptr;
  API_IMPL_END
}

FL_API_STATUS_IMPL(Manager_WebServiceStartImpl, flManager* manager) {
  API_IMPL_BEGIN
  if (!manager) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "null manager");
  }

  manager->impl.StartWebService();
  return nullptr;
  API_IMPL_END
}

FL_API_STATUS_IMPL(Manager_WebServiceUrlsImpl, const flManager* manager,
                   const char* const** out_urls, size_t* out_num_urls) {
  API_IMPL_BEGIN
  if (!manager || !out_urls || !out_num_urls) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "null argument");
  }

  const auto& urls = manager->impl.GetWebServiceUrls();
  manager->urls_cache.clear();
  manager->urls_cache.reserve(urls.size());
  for (const auto& u : urls) {
    manager->urls_cache.push_back(u.c_str());
  }

  *out_urls = manager->urls_cache.data();
  *out_num_urls = manager->urls_cache.size();
  return nullptr;
  API_IMPL_END
}

FL_API_STATUS_IMPL(Manager_WebServiceStopImpl, flManager* manager) {
  API_IMPL_BEGIN
  if (!manager) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "null manager");
  }

  manager->impl.StopWebService();
  return nullptr;
  API_IMPL_END
}

// ========================================================================
// KeyValuePairs API
// ========================================================================

static void FL_API_CALL CreateKeyValuePairsImpl(flKeyValuePairs** out) FL_NO_EXCEPTION {
  if (out)
    *out = AsHandle<flKeyValuePairs>(new (std::nothrow) fl::KeyValuePairs());
}

static void FL_API_CALL AddKeyValuePairImpl(flKeyValuePairs* kvps,
                                            const char* key, const char* value) FL_NO_EXCEPTION {
  if (!kvps || !key) {
    return;
  }

  AsImpl(kvps)->Add(key, value ? value : "");
}

static const char* FL_API_CALL GetKeyValueImpl(const flKeyValuePairs* kvps,
                                               const char* key) FL_NO_EXCEPTION {
  if (!kvps || !key) {
    return nullptr;
  }

  return AsImpl(kvps)->Find(key);
}

static void FL_API_CALL GetKeyValuePairsImpl(const flKeyValuePairs* kvps,
                                             const char* const** keys,
                                             const char* const** values,
                                             size_t* num_entries) FL_NO_EXCEPTION {
  if (!kvps || !keys || !values || !num_entries) {
    return;
  }

  const auto* impl = AsImpl(kvps);
  *keys = impl->Keys().data();
  *values = impl->Values().data();
  *num_entries = impl->Keys().size();
}

static void FL_API_CALL RemoveKeyValuePairImpl(flKeyValuePairs* kvps, const char* key) FL_NO_EXCEPTION {
  if (!kvps || !key) {
    return;
  }

  AsImpl(kvps)->Remove(key);
}

static void FL_API_CALL KeyValuePairs_ReleaseImpl(flKeyValuePairs* kvps) FL_NO_EXCEPTION {
  delete AsImpl(kvps);
}

// ========================================================================
// ModelList API
// ========================================================================

static void FL_API_CALL ModelList_ReleaseImpl(flModelList* models) FL_NO_EXCEPTION {
  delete models;
}

static size_t FL_API_CALL ModelList_SizeImpl(const flModelList* models) FL_NO_EXCEPTION {
  return models ? models->items.size() : 0;
}

static flModel* FL_API_CALL ModelList_GetAtImpl(const flModelList* models, size_t idx) FL_NO_EXCEPTION {
  if (!models || idx >= models->items.size()) {
    return nullptr;
  }

  return models->items[idx];
}

// ========================================================================
// EP Detection API
// ========================================================================

FL_API_STATUS_IMPL(Manager_GetDiscoverableEpsImpl, const flManager* manager,
                   const flEpInfo** out_eps,
                   size_t* out_count) {
  API_IMPL_BEGIN
  if (!manager || !out_eps || !out_count) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "null argument");
  }

  auto view = manager->impl.GetEpDetector().GetDiscoverableEpsCApi();
  *out_eps = view.data();
  *out_count = view.size();
  return nullptr;
  API_IMPL_END
}

FL_API_STATUS_IMPL(Manager_DownloadAndRegisterEpsImpl, flManager* manager,
                   const char* const* ep_names,
                   size_t num_ep_names,
                   flEpProgressCallback callback,
                   void* user_data) {
  API_IMPL_BEGIN
  if (!manager) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "null manager");
  }

  // Convert C name array to std::vector, nullptr means all EPs
  const std::vector<std::string>* names_ptr = nullptr;
  std::vector<std::string> names;
  if (ep_names != nullptr && num_ep_names > 0) {
    names.reserve(num_ep_names);
    for (size_t i = 0; i < num_ep_names; ++i) {
      if (ep_names[i]) {
        names.emplace_back(ep_names[i]);
      }
    }
    names_ptr = &names;
  }

  // Wire the C callback to the C++ progress callback.
  // The C callback returns 0 to continue, non-zero to cancel.
  fl::IEpBootstrapper::ProgressCallback progress_cb;

  if (callback) {
    progress_cb = [callback, user_data](const std::string& ep_name, float percent) -> bool {
      return callback(ep_name.c_str(), percent, user_data) == 0;
    };
  }

  auto result = manager->impl.DownloadAndRegisterEps(names_ptr, progress_cb);

  if (result.cancelled) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_OPERATION_CANCELLED, "EP download cancelled by user");
  }

  if (!result.success) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INTERNAL, result.status);
  }

  return nullptr;
  API_IMPL_END
}

static bool FL_API_CALL Manager_IsEpDownloadInProgressImpl(const flManager* manager) FL_NO_EXCEPTION {
  if (!manager) {
    return false;
  }

  return manager->impl.GetEpDetector().IsDownloadInProgress();
}

FL_API_STATUS_IMPL(Manager_ShutdownImpl, flManager* manager) {
  API_IMPL_BEGIN
  if (!manager) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "null manager");
  }

  manager->impl.Shutdown();
  return nullptr;
  API_IMPL_END
}

static bool FL_API_CALL Manager_IsShutdownRequestedImpl(const flManager* manager) FL_NO_EXCEPTION {
  if (!manager) {
    return false;
  }

  return manager->impl.IsShutdownRequested();
}

// ========================================================================
// Catalog API
// ========================================================================

FL_API_STATUS_IMPL(Catalog_GetModelsImpl, const flCatalog* catalog, flModelList** out_models) {
  API_IMPL_BEGIN
  if (!catalog || !out_models) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "null argument");
  }

  auto models = catalog->impl.ListModels();
  auto list = std::make_unique<flModelList>();
  list->items.reserve(models.size());

  for (auto* m : models) {
    list->items.push_back(AsHandle<flModel>(m));
  }

  *out_models = list.release();
  return nullptr;
  API_IMPL_END
}

FL_API_STATUS_IMPL(Catalog_GetModelImpl, const flCatalog* catalog, const char* alias,
                   flModel** out_model) {
  API_IMPL_BEGIN
  if (!catalog || !alias || !out_model) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "null argument");
  }

  auto* model = catalog->impl.GetModel(alias);
  if (!model) {
    *out_model = nullptr;
    return nullptr;
  }

  *out_model = AsHandle<flModel>(model);
  return nullptr;
  API_IMPL_END
}

FL_API_STATUS_IMPL(Catalog_GetModelVariantImpl, const flCatalog* catalog, const char* model_id,
                   flModel** out_model) {
  API_IMPL_BEGIN
  if (!catalog || !model_id || !out_model) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "null argument");
  }

  auto* model = catalog->impl.GetModelVariant(model_id);
  if (!model) {
    *out_model = nullptr;
    return nullptr;
  }

  *out_model = AsHandle<flModel>(model);
  return nullptr;
  API_IMPL_END
}

FL_API_STATUS_IMPL(Catalog_GetLatestVersionImpl, const flCatalog* catalog, const flModel* model,
                   flModel** out_model) {
  API_IMPL_BEGIN
  if (!catalog || !model || !out_model) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "null argument");
  }

  auto* latest = catalog->impl.GetLatestVersion(AsImpl(model));
  if (!latest) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "latest version not found");
  }

  *out_model = AsHandle<flModel>(latest);
  return nullptr;
  API_IMPL_END
}

FL_API_STATUS_IMPL(Catalog_GetCachedModelsImpl, const flCatalog* catalog, flModelList** out_models) {
  API_IMPL_BEGIN
  if (!catalog || !out_models) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "null argument");
  }

  auto models = catalog->impl.GetCachedModels();
  auto list = std::make_unique<flModelList>();
  list->items.reserve(models.size());

  for (auto* m : models) {
    list->items.push_back(AsHandle<flModel>(m));
  }

  *out_models = list.release();
  return nullptr;
  API_IMPL_END
}

FL_API_STATUS_IMPL(Catalog_GetLoadedModelsImpl, const flCatalog* catalog, flModelList** out_models) {
  API_IMPL_BEGIN
  if (!catalog || !out_models) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "null argument");
  }

  auto models = catalog->impl.GetLoadedModels();
  auto list = std::make_unique<flModelList>();
  list->items.reserve(models.size());

  for (auto* m : models) {
    list->items.push_back(AsHandle<flModel>(m));
  }

  *out_models = list.release();
  return nullptr;
  API_IMPL_END
}

FL_API_STATUS_IMPL(Catalog_GetNameImpl, const flCatalog* catalog, const char** out_name) {
  API_IMPL_BEGIN
  if (!catalog || !out_name) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "null argument");
  }

  *out_name = catalog->impl.GetName().c_str();
  return nullptr;
  API_IMPL_END
}

FL_API_STATUS_IMPL(Catalog_GetModelVersionsImpl, const flCatalog* catalog,
                   const char* model_alias, const char* model_name,
                   int32_t max_versions, flModelList** out_models) {
  API_IMPL_BEGIN
  if (!catalog || !model_alias || !out_models) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "null argument");
  }

  if (*model_alias == '\0') {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "model_alias must not be empty");
  }

  std::string alias = model_alias;
  std::string model_name_filter = model_name ? model_name : std::string{};

  auto models = catalog->impl.GetModelVersions(alias, model_name_filter, max_versions);
  auto list = std::make_unique<flModelList>();
  list->items.reserve(models.size());

  for (auto* m : models) {
    list->items.push_back(AsHandle<flModel>(m));
  }

  *out_models = list.release();
  return nullptr;
  API_IMPL_END
}

static const flCatalogApi g_catalog_api = {
    Catalog_GetNameImpl,
    Catalog_GetModelsImpl,
    Catalog_GetModelImpl,
    Catalog_GetModelVariantImpl,
    Catalog_GetLatestVersionImpl,
    Catalog_GetCachedModelsImpl,
    Catalog_GetLoadedModelsImpl,
    Catalog_GetModelVersionsImpl,
};

// ========================================================================
// Model API
// ========================================================================

FL_API_STATUS_IMPL(Model_GetInputOutputInfoImpl, const flModel* model,
                   const flItem* const** out_inputs, size_t* out_num_inputs,
                   const flItem* const** out_outputs, size_t* out_num_outputs) {
  API_IMPL_BEGIN
  if (!model || !out_inputs || !out_num_inputs || !out_outputs || !out_num_outputs) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "null argument");
  }

  auto io = AsImpl(model)->GetInputOutputInfo();

  // The internal API returns arrays of fl::Item* (pointing to concrete
  // subclasses). The flItem* and fl::Item* bit patterns are interchangeable
  // (see c_api_types.h), so reinterpret_cast on the array pointer is safe.
  *out_inputs = reinterpret_cast<const flItem* const*>(io.inputs);
  *out_num_inputs = io.num_inputs;
  *out_outputs = reinterpret_cast<const flItem* const*>(io.outputs);
  *out_num_outputs = io.num_outputs;
  return nullptr;
  API_IMPL_END
}

FL_API_STATUS_IMPL(Model_IsCachedImpl, const flModel* model, int* out_cached) {
  API_IMPL_BEGIN
  if (!model || !out_cached) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "null argument");
  }

  *out_cached = AsImpl(model)->IsCached() ? 1 : 0;
  return nullptr;
  API_IMPL_END
}

FL_API_STATUS_IMPL(Model_IsLoadedImpl, const flModel* model, int* out_loaded) {
  API_IMPL_BEGIN
  if (!model || !out_loaded) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "null argument");
  }

  *out_loaded = AsImpl(model)->IsLoaded() ? 1 : 0;
  return nullptr;
  API_IMPL_END
}

FL_API_STATUS_IMPL(Model_GetPathImpl, const flModel* model, const char** out_path) {
  API_IMPL_BEGIN
  if (!model || !out_path) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "null argument");
  }

  const auto* impl = AsImpl(model);
  if (!impl->IsCached()) {
    *out_path = nullptr;
    return nullptr;
  }

  *out_path = impl->GetPath().c_str();
  return nullptr;
  API_IMPL_END
}

FL_API_STATUS_IMPL(Model_DownloadImpl, flModel* model, flProgressCallback callback,
                   void* user_data) {
  API_IMPL_BEGIN
  if (!model) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "null model");
  }

  std::function<int(float)> progress_fn;
  if (callback) {
    progress_fn = [callback, user_data](float percent) -> int {
      return callback(percent, user_data);
    };
  }

  AsImpl(model)->Download(std::move(progress_fn));
  return nullptr;
  API_IMPL_END
}

FL_API_STATUS_IMPL(Model_LoadImpl, flModel* model) {
  API_IMPL_BEGIN
  if (!model) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "null model");
  }

  AsImpl(model)->Load();
  return nullptr;
  API_IMPL_END
}

FL_API_STATUS_IMPL(Model_UnloadImpl, flModel* model) {
  API_IMPL_BEGIN
  if (!model) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "null model");
  }

  AsImpl(model)->Unload();
  return nullptr;
  API_IMPL_END
}

FL_API_STATUS_IMPL(Model_RemoveFromCacheImpl, flModel* model) {
  API_IMPL_BEGIN
  if (!model) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "null model");
  }

  auto* impl = AsImpl(model);
  if (!impl->IsCached()) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "model is not cached locally");
  }

  if (impl->IsLoaded()) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
                      "cannot remove a loaded model from cache; unload it first");
  }

  impl->RemoveFromCache();
  return nullptr;
  API_IMPL_END
}

FL_API_STATUS_IMPL(Model_GetVariantsImpl, const flModel* model, flModelList** out_variants) {
  API_IMPL_BEGIN
  if (!model || !out_variants) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "null argument");
  }

  auto variants = AsImpl(model)->Variants();
  auto list = std::make_unique<flModelList>();
  list->items.reserve(variants.size());

  for (auto* m : variants) {
    list->items.push_back(AsHandle<flModel>(m));
  }

  *out_variants = list.release();
  return nullptr;
  API_IMPL_END
}

FL_API_STATUS_IMPL(Model_SelectVariantImpl, flModel* model, const flModel* variant) {
  API_IMPL_BEGIN
  if (!model || !variant) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "null argument");
  }

  AsImpl(model)->SelectVariant(*AsImpl(variant));
  return nullptr;
  API_IMPL_END
}

FL_API_STATUS_IMPL(Model_GetInfoImpl, const flModel* model, const flModelInfo** out_info) {
  API_IMPL_BEGIN
  if (!model || !out_info) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "null argument");
  }

  *out_info = AsHandle<flModelInfo>(&AsImpl(model)->Info());
  return nullptr;
  API_IMPL_END
}

static const char* FL_API_CALL Info_GetIdImpl(const flModelInfo* info) FL_NO_EXCEPTION {
  return info ? AsImpl(info)->model_id.c_str() : nullptr;
}

static const char* FL_API_CALL Info_GetNameImpl(const flModelInfo* info) FL_NO_EXCEPTION {
  return info ? AsImpl(info)->name.c_str() : nullptr;
}

static int FL_API_CALL Info_GetVersionImpl(const flModelInfo* info) FL_NO_EXCEPTION {
  return info ? AsImpl(info)->version : 0;
}

static const char* FL_API_CALL Info_GetAliasImpl(const flModelInfo* info) FL_NO_EXCEPTION {
  return info ? AsImpl(info)->alias.c_str() : nullptr;
}

static const char* FL_API_CALL Info_GetUriImpl(const flModelInfo* info) FL_NO_EXCEPTION {
  return info ? AsImpl(info)->uri.c_str() : nullptr;
}

static flDeviceType FL_API_CALL Info_GetDeviceTypeImpl(const flModelInfo* info) FL_NO_EXCEPTION {
  return info ? static_cast<flDeviceType>(AsImpl(info)->device_type) : FOUNDRY_LOCAL_DEVICE_NOTSET;
}

static const char* FL_API_CALL Info_GetExecutionProviderImpl(const flModelInfo* info) FL_NO_EXCEPTION {
  if (!info || AsImpl(info)->execution_provider.empty()) {
    return nullptr;
  }

  return AsImpl(info)->execution_provider.c_str();
}

static const char* FL_API_CALL Info_GetTaskImpl(const flModelInfo* info) FL_NO_EXCEPTION {
  if (!info || AsImpl(info)->task.empty()) {
    return nullptr;
  }

  return AsImpl(info)->task.c_str();
}

static const flKeyValuePairs* FL_API_CALL Info_GetPromptTemplatesImpl(const flModelInfo* info) FL_NO_EXCEPTION {
  if (!info || AsImpl(info)->prompt_templates.empty()) {
    return nullptr;
  }

  return AsHandle<flKeyValuePairs>(&AsImpl(info)->prompt_templates);
}

static const flKeyValuePairs* FL_API_CALL Info_GetModelSettingsImpl(const flModelInfo* info) FL_NO_EXCEPTION {
  if (!info || AsImpl(info)->model_settings.empty()) {
    return nullptr;
  }

  return AsHandle<flKeyValuePairs>(&AsImpl(info)->model_settings);
}

static const char* FL_API_CALL Info_GetStringPropertyImpl(const flModelInfo* info,
                                                          const char* key) FL_NO_EXCEPTION {
  if (!info || !key) {
    return nullptr;
  }

  const auto* impl = AsImpl(info);
  auto it = impl->string_properties.find(key);
  return (it != impl->string_properties.end()) ? it->second.c_str() : nullptr;
}

static int64_t FL_API_CALL Info_GetIntPropertyImpl(const flModelInfo* info,
                                                   const char* key,
                                                   int64_t default_value) FL_NO_EXCEPTION {
  if (!info || !key) {
    return default_value;
  }

  return AsImpl(info)->GetPropertyWithDefault(key, default_value);
}

static const flModelApi g_model_api = {
    Model_GetInfoImpl,
    Model_GetInputOutputInfoImpl,
    Model_IsCachedImpl,
    Model_GetPathImpl,
    Model_DownloadImpl,
    Model_IsLoadedImpl,
    Model_LoadImpl,
    Model_UnloadImpl,
    Model_RemoveFromCacheImpl,
    Model_GetVariantsImpl,
    Model_SelectVariantImpl,
    Info_GetIdImpl,
    Info_GetNameImpl,
    Info_GetVersionImpl,
    Info_GetAliasImpl,
    Info_GetUriImpl,
    Info_GetDeviceTypeImpl,
    Info_GetExecutionProviderImpl,
    Info_GetTaskImpl,
    Info_GetPromptTemplatesImpl,
    Info_GetModelSettingsImpl,
    Info_GetStringPropertyImpl,
    Info_GetIntPropertyImpl,
};

// ========================================================================
// Item API
// ========================================================================

// AsItemType — thin wrapper around AsImpl that downcasts to a concrete
// fl::Item subclass. Caller must already have validated the discriminator
// (e.g. checked item->type) before downcasting.
template <typename T = fl::Item>
static T* AsItemType(flItem* item) {
  return AsImpl<T>(item);
}

template <typename T = fl::Item>
static const T* AsItemType(const flItem* item) {
  return AsImpl<T>(item);
}

FL_API_STATUS_IMPL(Item_CreateImpl, flItemType type, flItem** out_item) {
  API_IMPL_BEGIN
  if (!out_item) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "null out_item");
  }

  if (type == FOUNDRY_LOCAL_ITEM_SPEECH_SEGMENT || type == FOUNDRY_LOCAL_ITEM_SPEECH_RESULT) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
                      "SPEECH_SEGMENT / SPEECH_RESULT are output-only and cannot be created by callers");
  }

  auto item = fl::Item::Create(type);
  if (!item) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "unknown item type");
  }

  *out_item = AsHandle<flItem>(item.release());
  return nullptr;
  API_IMPL_END
}

static void FL_API_CALL Item_ReleaseImpl(flItem* item) FL_NO_EXCEPTION {
  delete AsImpl(item);  // virtual destructor handles deleter and derived cleanup
}

static flItemType FL_API_CALL Item_GetTypeImpl(const flItem* item) FL_NO_EXCEPTION {
  if (!item) {
    return FOUNDRY_LOCAL_ITEM_UNKNOWN;
  }

  return AsImpl(item)->type;
}

// --- ItemQueue ---

FL_API_STATUS_IMPL(ItemQueue_CreateImpl, flItemQueue** out_queue) {
  API_IMPL_BEGIN
  if (!out_queue) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "null out_queue");
  }

  *out_queue = AsHandle<flItemQueue>(new fl::ItemQueue());
  return nullptr;
  API_IMPL_END
}

static void FL_API_CALL ItemQueue_ReleaseImpl(flItemQueue* queue) FL_NO_EXCEPTION {
  delete AsImpl(queue);
}

FL_API_STATUS_IMPL(ItemQueue_PushImpl, flItemQueue* queue, flItem* item) {
  API_IMPL_BEGIN
  if (!queue || !item) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "null argument");
  }

  // Transfer ownership: the raw flItem* becomes a unique_ptr<fl::Item> in the queue.
  AsImpl(queue)->Push(std::unique_ptr<fl::Item>(AsImpl(item)));
  return nullptr;
  API_IMPL_END
}

static bool FL_API_CALL ItemQueue_TryPopImpl(flItemQueue* queue, flItem** out_item) FL_NO_EXCEPTION {
  if (!queue || !out_item) {
    return false;
  }

  auto item = AsImpl(queue)->TryPop();
  if (item) {
    *out_item = AsHandle<flItem>(item.release());
    return true;
  }

  return false;
}

static size_t FL_API_CALL ItemQueue_SizeImpl(const flItemQueue* queue) FL_NO_EXCEPTION {
  return queue ? AsImpl(queue)->Size() : 0;
}

static void FL_API_CALL ItemQueue_MarkFinishedImpl(flItemQueue* queue) FL_NO_EXCEPTION {
  if (queue) {
    AsImpl(queue)->MarkFinished();
  }
}

static bool FL_API_CALL ItemQueue_IsFinishedImpl(const flItemQueue* queue) FL_NO_EXCEPTION {
  return queue ? AsImpl(queue)->IsFinished() : true;
}

// --- Text ---

FL_API_STATUS_IMPL(Item_SetTextImpl, flItem* item, const flTextData* text_data) {
  API_IMPL_BEGIN
  if (!item || !text_data || !text_data->text) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "null argument");
  }

  if (text_data->version == 0) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "text_data->version must be set");
  }

  if (AsImpl(item)->type != FOUNDRY_LOCAL_ITEM_TEXT) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "item is not a TEXT item");
  }

  auto* t = AsItemType<fl::TextItem>(item);
  t->text = text_data->text;
  t->text_type = text_data->type;

  return nullptr;
  API_IMPL_END
}

FL_API_STATUS_IMPL(Item_GetTextImpl, const flItem* item, flTextData* out_text_data) {
  API_IMPL_BEGIN
  if (!item || !out_text_data) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "null argument");
  }

  if (out_text_data->version == 0) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "out_text_data->version must be set");
  }

  if (AsImpl(item)->type != FOUNDRY_LOCAL_ITEM_TEXT) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "item is not a TEXT item");
  }

  const auto* t = AsItemType<fl::TextItem>(item);
  out_text_data->version = FOUNDRY_LOCAL_API_VERSION;
  out_text_data->text = t->text.c_str();
  out_text_data->type = t->text_type;

  return nullptr;
  API_IMPL_END
}

// --- Tensor ---

FL_API_STATUS_IMPL(Item_SetTensorImpl, flItem* item, const flTensorData* tensor) {
  API_IMPL_BEGIN
  if (!item || !tensor) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "null argument");
  }

  if (AsImpl(item)->type != FOUNDRY_LOCAL_ITEM_TENSOR) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "item is not a TENSOR item");
  }

  if (tensor->deleter && !tensor->mutable_data) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "deleter requires mutable_data to be set");
  }

  if (tensor->data && tensor->mutable_data && tensor->data != tensor->mutable_data) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "data and mutable_data must be equal when both are set");
  }

  auto* t = AsItemType<fl::TensorItem>(item);
  t->SetTensorData(*tensor);

  return nullptr;
  API_IMPL_END
}

FL_API_STATUS_IMPL(Item_GetTensorImpl, const flItem* item, flTensorData* out_tensor) {
  API_IMPL_BEGIN
  if (!item || !out_tensor) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "null argument");
  }

  if (AsImpl(item)->type != FOUNDRY_LOCAL_ITEM_TENSOR) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "item is not a TENSOR item");
  }

  AsItemType<fl::TensorItem>(item)->GetApiData(*out_tensor);
  return nullptr;
  API_IMPL_END
}

// --- Image ---

FL_API_STATUS_IMPL(Item_SetImageImpl, flItem* item, const flImageData* image) {
  API_IMPL_BEGIN
  if (!item || !image) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "null argument");
  }

  if (AsImpl(item)->type != FOUNDRY_LOCAL_ITEM_IMAGE) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "item is not an IMAGE item");
  }

  if (image->deleter && !image->mutable_data) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "deleter requires mutable_data to be set");
  }

  if (image->data && image->mutable_data && image->data != image->mutable_data) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "data and mutable_data must be equal when both are set");
  }

  if (auto* err = ValidateDataPointers(image->data, image->mutable_data, image->data_size, "image")) {
    return err;
  }

  auto* img = AsItemType<fl::ImageItem>(item);
  img->SetImageData(*image);

  return nullptr;
  API_IMPL_END
}

FL_API_STATUS_IMPL(Item_GetImageImpl, const flItem* item, flImageData* out_image) {
  API_IMPL_BEGIN
  if (!item || !out_image) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "null argument");
  }

  if (AsImpl(item)->type != FOUNDRY_LOCAL_ITEM_IMAGE) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "item is not an IMAGE item");
  }

  AsItemType<fl::ImageItem>(item)->GetApiData(*out_image);
  return nullptr;
  API_IMPL_END
}

// --- Message ---

FL_API_STATUS_IMPL(Item_SetMessageImpl, flItem* item, const flMessageData* message) {
  API_IMPL_BEGIN
  if (!item || !message) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "null argument");
  }

  if (AsImpl(item)->type != FOUNDRY_LOCAL_ITEM_MESSAGE) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "item is not a MESSAGE item");
  }

  auto* msg = AsItemType<fl::MessageItem>(item);
  msg->SetMessageData(*message);

  return nullptr;
  API_IMPL_END
}

FL_API_STATUS_IMPL(Item_GetMessageImpl, const flItem* item, flMessageData* out_message) {
  API_IMPL_BEGIN
  if (!item || !out_message) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "null argument");
  }

  if (AsImpl(item)->type != FOUNDRY_LOCAL_ITEM_MESSAGE) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "item is not a MESSAGE item");
  }

  AsItemType<fl::MessageItem>(item)->GetApiData(*out_message);
  return nullptr;
  API_IMPL_END
}

// --- ToolCall ---

FL_API_STATUS_IMPL(Item_SetToolCallImpl, flItem* item, const flToolCallData* tool_call) {
  API_IMPL_BEGIN
  if (!item || !tool_call || !tool_call->call_id || !tool_call->name || !tool_call->arguments) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "null argument");
  }

  if (AsImpl(item)->type != FOUNDRY_LOCAL_ITEM_TOOL_CALL) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "item is not a TOOL_CALL item");
  }

  auto* tc = AsItemType<fl::ToolCallItem>(item);
  tc->SetToolCallData(*tool_call);

  return nullptr;
  API_IMPL_END
}

FL_API_STATUS_IMPL(Item_GetToolCallImpl, const flItem* item, flToolCallData* out_tool_call) {
  API_IMPL_BEGIN
  if (!item || !out_tool_call) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "null argument");
  }

  if (AsImpl(item)->type != FOUNDRY_LOCAL_ITEM_TOOL_CALL) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "item is not a TOOL_CALL item");
  }

  AsItemType<fl::ToolCallItem>(item)->GetApiData(*out_tool_call);
  return nullptr;
  API_IMPL_END
}

// --- ToolResult ---

FL_API_STATUS_IMPL(Item_SetToolResultImpl, flItem* item, const flToolResultData* tool_result) {
  API_IMPL_BEGIN
  if (!item || !tool_result || !tool_result->call_id || !tool_result->result) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "null argument");
  }

  if (AsImpl(item)->type != FOUNDRY_LOCAL_ITEM_TOOL_RESULT) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "item is not a TOOL_RESULT item");
  }

  auto* tr = AsItemType<fl::ToolResultItem>(item);
  tr->SetToolResultData(*tool_result);

  return nullptr;
  API_IMPL_END
}

FL_API_STATUS_IMPL(Item_GetToolResultImpl, const flItem* item, flToolResultData* out_tool_result) {
  API_IMPL_BEGIN
  if (!item || !out_tool_result) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "null argument");
  }

  if (AsImpl(item)->type != FOUNDRY_LOCAL_ITEM_TOOL_RESULT) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "item is not a TOOL_RESULT item");
  }

  AsItemType<fl::ToolResultItem>(item)->GetApiData(*out_tool_result);
  return nullptr;
  API_IMPL_END
}

// --- Speech (output-only) ---

FL_API_STATUS_IMPL(Item_GetSpeechSegmentImpl, const flItem* item, flSpeechSegmentData* out_segment) {
  API_IMPL_BEGIN
  if (!item || !out_segment) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "null argument");
  }

  if (AsImpl(item)->type != FOUNDRY_LOCAL_ITEM_SPEECH_SEGMENT) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "item is not a SPEECH_SEGMENT item");
  }

  AsItemType<fl::SpeechSegmentItem>(item)->GetApiData(*out_segment);
  return nullptr;
  API_IMPL_END
}

FL_API_STATUS_IMPL(Item_GetSpeechResultImpl, const flItem* item, flSpeechResultData* out_result) {
  API_IMPL_BEGIN
  if (!item || !out_result) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "null argument");
  }

  if (AsImpl(item)->type != FOUNDRY_LOCAL_ITEM_SPEECH_RESULT) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "item is not a SPEECH_RESULT item");
  }

  AsItemType<fl::SpeechResultItem>(item)->GetApiData(*out_result);
  return nullptr;
  API_IMPL_END
}

// --- Bytes ---

FL_API_STATUS_IMPL(Item_SetBytesImpl, flItem* item, const flBytesData* bytes) {
  API_IMPL_BEGIN
  if (!item || !bytes) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "null argument");
  }

  if (AsImpl(item)->type != FOUNDRY_LOCAL_ITEM_BYTES) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "item is not a BYTES item");
  }

  if (bytes->deleter && !bytes->mutable_data) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "deleter requires mutable_data to be set");
  }

  if (bytes->data && bytes->mutable_data && bytes->data != bytes->mutable_data) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "data and mutable_data must be equal when both are set");
  }

  if (auto* err = ValidateDataPointers(bytes->data, bytes->mutable_data, bytes->data_size, "bytes")) {
    return err;
  }

  auto* b = AsItemType<fl::BytesItem>(item);
  b->SetBytesData(*bytes);

  return nullptr;
  API_IMPL_END
}

FL_API_STATUS_IMPL(Item_GetBytesImpl, const flItem* item, flBytesData* out_bytes) {
  API_IMPL_BEGIN
  if (!item || !out_bytes) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "null argument");
  }

  if (AsImpl(item)->type != FOUNDRY_LOCAL_ITEM_BYTES) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "item is not a BYTES item");
  }

  AsItemType<fl::BytesItem>(item)->GetApiData(*out_bytes);
  return nullptr;
  API_IMPL_END
}

// --- Audio ---

FL_API_STATUS_IMPL(Item_SetAudioImpl, flItem* item, const flAudioData* audio) {
  API_IMPL_BEGIN
  if (!item || !audio) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "null argument");
  }

  if (AsImpl(item)->type != FOUNDRY_LOCAL_ITEM_AUDIO) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "item is not an AUDIO item");
  }

  if (audio->deleter && !audio->mutable_data) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "deleter requires mutable_data to be set");
  }

  if (audio->data && audio->mutable_data && audio->data != audio->mutable_data) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "data and mutable_data must be equal when both are set");
  }

  if (auto* err = ValidateDataPointers(audio->data, audio->mutable_data, audio->data_size, "audio")) {
    return err;
  }

  auto* a = AsItemType<fl::AudioItem>(item);
  a->SetAudioData(*audio);

  return nullptr;
  API_IMPL_END
}

FL_API_STATUS_IMPL(Item_GetAudioImpl, const flItem* item, flAudioData* out_audio) {
  API_IMPL_BEGIN
  if (!item || !out_audio) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "null argument");
  }

  if (AsImpl(item)->type != FOUNDRY_LOCAL_ITEM_AUDIO) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "item is not an AUDIO item");
  }

  AsItemType<fl::AudioItem>(item)->GetApiData(*out_audio);
  return nullptr;
  API_IMPL_END
}

// --- Metadata ---

FL_API_STATUS_IMPL(Item_GetMetadataImpl, const flItem* item, const flKeyValuePairs** out_metadata) {
  API_IMPL_BEGIN
  if (!item || !out_metadata) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "null argument");
  }

  const auto* base = AsImpl(item);
  *out_metadata = AsHandle<flKeyValuePairs>(base->GetMetadata());  // nullptr if no metadata exists
  return nullptr;
  API_IMPL_END
}

FL_API_STATUS_IMPL(Item_GetMutableMetadataImpl, flItem* item, flKeyValuePairs** out_metadata) {
  API_IMPL_BEGIN
  if (!item || !out_metadata) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "null argument");
  }

  auto* base = AsImpl(item);
  *out_metadata = AsHandle<flKeyValuePairs>(&base->GetMetadata());  // creates if it doesn't exist

  return nullptr;
  API_IMPL_END
}

// --- Queue item ---
FL_API_STATUS_IMPL(Item_GetQueueImpl, flItem* item, flItemQueue** out_queue) {
  API_IMPL_BEGIN
  if (!item || !out_queue) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "null argument");
  }

  if (AsImpl(item)->type != FOUNDRY_LOCAL_ITEM_QUEUE) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "item is not a QUEUE item");
  }

  // For QUEUE items, the item itself IS the queue (fl::ItemQueue derives from fl::Item).
  *out_queue = AsHandle<flItemQueue>(AsImpl<fl::ItemQueue>(item));
  return nullptr;
  API_IMPL_END
}

static const flItemApi g_item_api = {
    Item_CreateImpl,
    Item_ReleaseImpl,
    Item_GetTypeImpl,
    Item_SetBytesImpl,
    Item_SetTensorImpl,
    Item_SetTextImpl,
    Item_SetMessageImpl,
    Item_SetImageImpl,
    Item_SetAudioImpl,
    Item_SetToolCallImpl,
    Item_SetToolResultImpl,
    Item_GetBytesImpl,
    Item_GetTextImpl,
    Item_GetMessageImpl,
    Item_GetTensorImpl,
    Item_GetImageImpl,
    Item_GetAudioImpl,
    Item_GetToolCallImpl,
    Item_GetToolResultImpl,
    Item_GetSpeechSegmentImpl,
    Item_GetSpeechResultImpl,
    Item_GetMetadataImpl,
    Item_GetMutableMetadataImpl,
    Item_GetQueueImpl,
    ItemQueue_CreateImpl,
    ItemQueue_ReleaseImpl,
    ItemQueue_PushImpl,
    ItemQueue_TryPopImpl,
    ItemQueue_SizeImpl,
    ItemQueue_MarkFinishedImpl,
    ItemQueue_IsFinishedImpl,
};

// ========================================================================
// Inference API (Request / Response / Session)
// ========================================================================

// --- Request ---

FL_API_STATUS_IMPL(Request_CreateImpl, flRequest** out_request) {
  API_IMPL_BEGIN
  if (!out_request) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "null out_request");
  }

  *out_request = AsHandle<flRequest>(new fl::Request());
  return nullptr;
  API_IMPL_END
}

static void FL_API_CALL Request_ReleaseImpl(flRequest* request) FL_NO_EXCEPTION {
  delete AsImpl(request);
}

FL_API_STATUS_IMPL(Request_AddItemImpl, flRequest* request, flItem* item, bool take_ownership) {
  API_IMPL_BEGIN
  if (!request || !item) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "null argument");
  }

  auto* req = AsImpl(request);
  if (take_ownership) {
    req->AddOwnedItem(std::unique_ptr<fl::Item>(AsImpl(item)));
  } else {
    req->AddBorrowedItem(AsImpl(item));
  }

  return nullptr;
  API_IMPL_END
}

static size_t FL_API_CALL Request_GetItemCountImpl(const flRequest* request) FL_NO_EXCEPTION {
  return request ? AsImpl(request)->items.size() : 0;
}

FL_API_STATUS_IMPL(Request_GetItemImpl, const flRequest* request, size_t idx,
                   const flItem** out_item) {
  API_IMPL_BEGIN
  if (!request || !out_item) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "null argument");
  }

  const auto* req = AsImpl(request);
  if (idx >= req->items.size()) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "index out of range");
  }

  *out_item = AsHandle<flItem>(req->items[idx]);
  return nullptr;
  API_IMPL_END
}

FL_API_STATUS_IMPL(Request_SetOptionsImpl, flRequest* request, const flKeyValuePairs* options) {
  API_IMPL_BEGIN
  if (!request || !options) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "null argument");
  }

  auto* req = AsImpl(request);
  for (const auto& [k, v] : *AsImpl(options)) {
    req->options[k] = v;
  }

  return nullptr;
  API_IMPL_END
}

FL_API_STATUS_IMPL(Request_CancelImpl, flRequest* request) {
  API_IMPL_BEGIN
  if (!request) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "null argument");
  }
  AsImpl(request)->canceled = true;
  return nullptr;
  API_IMPL_END
}

// --- Response ---

FL_API_STATUS_IMPL(Response_CreateImpl, flResponse** out_response) {
  API_IMPL_BEGIN
  if (!out_response) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "null out_response");
  }

  *out_response = AsHandle<flResponse>(new fl::Response());
  return nullptr;
  API_IMPL_END
}

static void FL_API_CALL Response_ReleaseImpl(flResponse* response) FL_NO_EXCEPTION {
  delete AsImpl(response);
}

static size_t FL_API_CALL Response_GetItemCountImpl(const flResponse* response) FL_NO_EXCEPTION {
  return response ? AsImpl(response)->items.size() : 0;
}

FL_API_STATUS_IMPL(Response_GetItemImpl, const flResponse* response, size_t idx,
                   const flItem** out_item) {
  API_IMPL_BEGIN
  if (!response || !out_item) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "null argument");
  }

  const auto* resp = AsImpl(response);
  if (idx >= resp->items.size()) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "index out of range");
  }

  *out_item = AsHandle<flItem>(resp->items[idx].get());
  return nullptr;
  API_IMPL_END
}

static flFinishReason FL_API_CALL Response_GetFinishReasonImpl(const flResponse* response) FL_NO_EXCEPTION {
  return response ? AsImpl(response)->finish_reason : FOUNDRY_LOCAL_FINISH_NONE;
}

FL_API_STATUS_IMPL(Response_GetUsageImpl, const flResponse* response, flUsage* out_usage) {
  API_IMPL_BEGIN
  if (!response || !out_usage) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "null argument");
  }

  const auto& usage = AsImpl(response)->usage;
  out_usage->version = FOUNDRY_LOCAL_API_VERSION;
  out_usage->prompt_tokens = usage.prompt_tokens;
  out_usage->completion_tokens = usage.completion_tokens;
  out_usage->total_tokens = usage.total_tokens;
  return nullptr;
  API_IMPL_END
}

// --- Session ---

FL_API_STATUS_IMPL(Session_CreateImpl, const flModel* model, flSession** out_session) {
  API_IMPL_BEGIN
  if (!model || !out_session) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "null argument");
  }

  auto session = fl::Session::Create(*AsImpl(model));
  *out_session = AsHandle<flSession>(session.release());
  return nullptr;
  API_IMPL_END
}

static void FL_API_CALL Session_ReleaseImpl(flSession* session) FL_NO_EXCEPTION {
  delete AsImpl(session);
}

FL_API_STATUS_IMPL(Session_AddToolDefinitionImpl, flSession* session, const flToolDefinition* tool_def) {
  API_IMPL_BEGIN
  if (!session || !tool_def || !tool_def->name || !tool_def->description || !tool_def->json_schema) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "null argument");
  }

  AsImpl(session)->AddToolDefinition({tool_def->name, tool_def->description, tool_def->json_schema});
  return nullptr;
  API_IMPL_END
}

FL_API_STATUS_IMPL(Session_RemoveToolDefinitionImpl, flSession* session, const char* tool_name, bool* out_removed) {
  API_IMPL_BEGIN
  if (!session || !tool_name || !out_removed) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "null argument");
  }

  *out_removed = AsImpl(session)->RemoveToolDefinition(tool_name);
  return nullptr;
  API_IMPL_END
}

FL_API_STATUS_IMPL(Session_SetStreamingCallbackImpl, flSession* session,
                   flStreamingCallback callback, void* user_data) {
  API_IMPL_BEGIN
  if (!session) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "null session");
  }

  AsImpl(session)->SetStreamingCallback(callback, user_data);
  return nullptr;
  API_IMPL_END
}

FL_API_STATUS_IMPL(Session_SetOptionsImpl, flSession* session, const flKeyValuePairs* options) {
  API_IMPL_BEGIN
  if (!session || !options) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "null argument");
  }

  AsImpl(session)->SetSessionOptions(*AsImpl(options));

  return nullptr;
  API_IMPL_END
}

FL_API_STATUS_IMPL(Session_ProcessRequestImpl, flSession* session, const flRequest* request,
                   flResponse** response) {
  API_IMPL_BEGIN
  if (!session || !request || !response) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "null argument");
  }

  // Allocate response locally so a throw from ProcessRequest does not leak it.
  std::unique_ptr<fl::Response> owned;
  fl::Response* target = nullptr;

  if (*response) {
    target = AsImpl(*response);
  } else {
    owned = std::make_unique<fl::Response>();
    target = owned.get();
  }

  // ProcessRequest handles session option overlay and streaming callback wiring.
  AsImpl(session)->ProcessRequest(*AsImpl(request), *target);

  if (owned) {
    *response = AsHandle<flResponse>(owned.release());
  }
  return nullptr;
  API_IMPL_END
}

static size_t FL_API_CALL Session_GetTurnCountImpl(const flSession* session) FL_NO_EXCEPTION {
  if (!session) {
    return 0;
  }

  return AsImpl(session)->TurnCount();
}

FL_API_STATUS_IMPL(Session_UndoTurnsImpl, flSession* session, size_t count) {
  API_IMPL_BEGIN
  if (!session) {
    return MakeStatus(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "null session");
  }

  AsImpl(session)->UndoTurns(count);
  return nullptr;
  API_IMPL_END
}

static const flInferenceApi g_inference_api = {
    Request_CreateImpl,
    Request_ReleaseImpl,
    Request_AddItemImpl,
    Request_GetItemCountImpl,
    Request_GetItemImpl,
    Request_SetOptionsImpl,
    Request_CancelImpl,
    Response_CreateImpl,
    Response_ReleaseImpl,
    Response_GetItemCountImpl,
    Response_GetItemImpl,
    Response_GetFinishReasonImpl,
    Response_GetUsageImpl,
    Session_CreateImpl,
    Session_ReleaseImpl,
    Session_SetStreamingCallbackImpl,
    Session_SetOptionsImpl,
    Session_ProcessRequestImpl,
    Session_AddToolDefinitionImpl,
    Session_RemoveToolDefinitionImpl,
    Session_GetTurnCountImpl,
    Session_UndoTurnsImpl,
};

// ========================================================================
// Sub-API accessors
// ========================================================================

static const flCatalogApi* FL_API_CALL GetCatalogApiImpl() FL_NO_EXCEPTION {
  return &g_catalog_api;
}

static const flConfigurationApi* FL_API_CALL GetConfigurationApiImpl() FL_NO_EXCEPTION {
  return &g_configuration_api;
}

static const flItemApi* FL_API_CALL GetItemApiImpl() FL_NO_EXCEPTION {
  return &g_item_api;
}

static const flInferenceApi* FL_API_CALL GetInferenceApiImpl() FL_NO_EXCEPTION {
  return &g_inference_api;
}

static const flModelApi* FL_API_CALL GetModelApiImpl() FL_NO_EXCEPTION {
  return &g_model_api;
}

// ========================================================================
// Root API function table (version 1)
// ========================================================================

static const flApi g_api_v1 = {
    /* Status */
    Status_CreateImpl,
    Status_ReleaseImpl,
    Status_GetErrorCodeImpl,
    Status_GetErrorMessageImpl,

    /* Manager lifecycle */
    Manager_CreateImpl,
    Manager_ReleaseImpl,
    Manager_GetCatalogImpl,
    Manager_WebServiceStartImpl,
    Manager_WebServiceUrlsImpl,
    Manager_WebServiceStopImpl,

    /* Sub-API accessors */
    GetCatalogApiImpl,
    GetConfigurationApiImpl,
    GetItemApiImpl,
    GetInferenceApiImpl,
    GetModelApiImpl,

    /* KeyValuePairs */
    CreateKeyValuePairsImpl,
    AddKeyValuePairImpl,
    GetKeyValueImpl,
    GetKeyValuePairsImpl,
    RemoveKeyValuePairImpl,
    KeyValuePairs_ReleaseImpl,

    /* ModelList */
    ModelList_ReleaseImpl,
    ModelList_SizeImpl,
    ModelList_GetAtImpl,

    /* EP detection */
    Manager_GetDiscoverableEpsImpl,
    Manager_DownloadAndRegisterEpsImpl,
    Manager_IsEpDownloadInProgressImpl,
    Manager_ShutdownImpl,
    Manager_IsShutdownRequestedImpl,
};

// ========================================================================
// Exported symbols — the ONLY symbols the library exports
// ========================================================================

extern "C" {

FL_EXPORT const flApi* FL_API_CALL FoundryLocalGetApi(uint32_t version) FL_NO_EXCEPTION {
  if (version == 0 || version <= FOUNDRY_LOCAL_API_VERSION) {
    return &g_api_v1;
  }

  return nullptr;
}

FL_EXPORT const char* FL_API_CALL FoundryLocalGetVersionString(void) FL_NO_EXCEPTION {
  return FOUNDRY_LOCAL_VERSION;
}

}  // extern "C"
