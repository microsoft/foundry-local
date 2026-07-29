// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

// Method implementations for foundry_local_cpp.h.
// Included at the bottom of foundry_local_cpp.h — do not include directly.

namespace foundry_local {

namespace detail {

// Factory functions — keep class declarations purely "shape".
inline flConfiguration* CreateConfiguration(const std::string& app_name);
inline flManager* CreateManager(const Configuration& config);
inline flItem* CreateItem(flItemType type);
inline flRequest* CreateRequest();
inline flResponse* CreateResponse();
inline flSession* CreateSession(IModel& model);

/// Build a KeyValuePairs from a RequestOptions. Defined further down in this file —
/// forward-declared here so Session::SetOptions / Request::SetOptions can call it.
inline KeyValuePairs ToKeyValuePairs(const RequestOptions& opts);

/// Checked downcast from IModel& to Model&. IModel is documented as not
/// user-implementable; this helper enforces that contract at runtime via
/// a virtual identity hook (no RTTI / dynamic_cast required).
inline Model& AsModel(IModel& model) {
  auto* concrete = model.AsConcreteModelHook();
  if (!concrete) {
    throw Error("IModel argument must be a concrete Model instance",
                FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT);
  }
  return *concrete;
}

inline const Model& AsModel(const IModel& model) {
  const auto* concrete = model.AsConcreteModelHook();
  if (!concrete) {
    throw Error("IModel argument must be a concrete Model instance",
                FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT);
  }
  return *concrete;
}

}  // namespace detail

// ===========================================================================
// Error
// ===========================================================================

inline Error::Error(flStatus& status)
    : std::runtime_error(GetErrorMessage(status)),
      code_(detail::api()->Status_GetErrorCode(&status)) {}

inline Error::Error(std::string_view message, flErrorCode code)
    : std::runtime_error(std::string(message)), code_(code) {}

inline const char* Error::GetErrorMessage(flStatus& status) {
  const char* message = detail::api()->Status_GetErrorMessage(&status);
  return message ? message : "Unknown Foundry Local error";
}

inline void Check(flStatus* status) {
  if (status) {
    Error err(*status);
    detail::api()->Status_Release(status);
    throw err;
  }
}

inline const char* Version() noexcept {
  return FoundryLocalGetVersionString();
}

// ===========================================================================
// KeyValuePairs
// ===========================================================================

inline KeyValuePairs::KeyValuePairs()
    : handle_([] {
        flKeyValuePairs* p = nullptr;
        detail::api()->CreateKeyValuePairs(&p);
        return p;
      }(),
              detail::api()->KeyValuePairs_Release) {}

inline KeyValuePairs::KeyValuePairs(
    std::initializer_list<std::pair<const char*, const char*>> pairs)
    : KeyValuePairs() {
  for (const auto& [k, v] : pairs) {
    detail::api()->AddKeyValuePair(handle_.get_mutable(), k, v);
  }
}

inline std::optional<std::string_view> KeyValuePairs::Get(const char* key) const noexcept {
  if (!handle_.get()) {
    return std::nullopt;
  }
  const char* v = detail::api()->GetKeyValue(handle_.get(), key);
  return v ? std::optional<std::string_view>{v} : std::nullopt;
}

inline std::vector<KeyValueEntry> KeyValuePairs::GetAll() const {
  std::vector<KeyValueEntry> result;
  if (!handle_.get()) {
    return result;
  }

  const char* const* keys = nullptr;
  const char* const* values = nullptr;
  size_t count = 0;
  detail::api()->GetKeyValuePairs(handle_.get(), &keys, &values, &count);
  result.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    result.push_back({keys[i] ? keys[i] : "", values[i] ? values[i] : ""});
  }
  return result;
}

inline KeyValuePairs& KeyValuePairs::Set(const char* key, const char* value) {
  detail::api()->AddKeyValuePair(handle_.get_mutable(), key, value);
  return *this;
}

inline KeyValuePairs& KeyValuePairs::Remove(const char* key) {
  detail::api()->RemoveKeyValuePair(handle_.get_mutable(), key);
  return *this;
}

// ===========================================================================
// Configuration
// ===========================================================================

inline Configuration::Configuration(const std::string& app_name)
    : handle_(detail::CreateConfiguration(app_name), detail::config_api()->Configuration_Release) {}

inline Configuration& Configuration::SetAppDataDir(const std::string& value) {
  Check(detail::config_api()->SetAppDataDir(handle_.get_mutable(), value.c_str()));
  return *this;
}

inline Configuration& Configuration::SetLogsDir(const std::string& value) {
  Check(detail::config_api()->SetLogsDir(handle_.get_mutable(), value.c_str()));
  return *this;
}

inline Configuration& Configuration::SetModelCacheDir(const std::string& value) {
  Check(detail::config_api()->SetModelCacheDir(handle_.get_mutable(), value.c_str()));
  return *this;
}

inline Configuration& Configuration::SetDefaultLogLevel(flLogLevel level) {
  Check(detail::config_api()->SetDefaultLogLevel(handle_.get_mutable(), level));
  return *this;
}

inline Configuration& Configuration::AddCatalogUrl(
    const std::string& url, const std::optional<std::string>& filter_override) {
  Check(detail::config_api()->AddCatalogUrl(
      handle_.get_mutable(), url.c_str(), filter_override ? filter_override->c_str() : nullptr));
  return *this;
}

inline Configuration& Configuration::AddWebServiceEndpoint(const std::string& url) {
  Check(detail::config_api()->AddWebServiceEndpoint(handle_.get_mutable(), url.c_str()));
  return *this;
}

inline Configuration& Configuration::SetAdditionalOptions(const KeyValuePairs& options) {
  Check(detail::config_api()->SetAdditionalOptions(handle_.get_mutable(), options.native_handle()));
  return *this;
}

inline Configuration& Configuration::SetExternalServiceUrl(const std::string& url) {
  Check(detail::config_api()->SetExternalServiceUrl(handle_.get_mutable(), url.c_str()));
  return *this;
}

inline Configuration& Configuration::SetCatalogRegion(const std::string& region) {
  Check(detail::config_api()->SetCatalogRegion(handle_.get_mutable(), region.c_str()));
  return *this;
}

inline flConfiguration* detail::CreateConfiguration(const std::string& app_name) {
  flConfiguration* config = nullptr;
  Check(detail::config_api()->Create(app_name.c_str(), &config));
  return config;
}

// ===========================================================================
// Manager
// ===========================================================================

inline Manager::Manager(Configuration&& config)
    : handle_(detail::CreateManager(config), detail::api()->Manager_Release),
      config_(std::move(config)) {}

inline ICatalog& Manager::GetCatalog() const {
  std::call_once(*catalog_once_, [this]() {
    flCatalog* cat = nullptr;
    Check(detail::api()->Manager_GetCatalog(handle_.get(), &cat));
    catalog_ = std::unique_ptr<Catalog>(new Catalog(*cat));
  });
  return *catalog_;
}

inline void Manager::StartWebService() {
  Check(detail::api()->Manager_WebServiceStart(handle_.get_mutable()));
}

inline std::vector<std::string> Manager::GetWebServiceEndpoints() const {
  const char* const* urls = nullptr;
  size_t num_urls = 0;
  Check(detail::api()->Manager_WebServiceUrls(handle_.get(), &urls, &num_urls));

  std::vector<std::string> service_endpoints;
  service_endpoints.reserve(num_urls);
  for (size_t i = 0; i < num_urls; ++i) {
    service_endpoints.emplace_back(urls[i]);
  }
  return service_endpoints;
}

inline void Manager::StopWebService() {
  Check(detail::api()->Manager_WebServiceStop(handle_.get_mutable()));
}

inline std::vector<EpInfo> Manager::GetDiscoverableEps() const {
  const flEpInfo* eps = nullptr;
  size_t count = 0;
  Check(detail::api()->Manager_GetDiscoverableEps(handle_.get(), &eps, &count));

  std::vector<EpInfo> result;
  result.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    result.push_back(EpInfo{std::string(eps[i].name), eps[i].is_registered});
  }
  return result;
}

inline void Manager::DownloadAndRegisterEps(
    const std::vector<std::string>& ep_names,
    std::function<bool(std::string_view ep_name, float percent)> progress) {
  std::vector<const char*> c_names;
  const char* const* names_ptr = nullptr;
  size_t names_count = 0;

  if (!ep_names.empty()) {
    c_names.reserve(ep_names.size());
    for (const auto& name : ep_names) {
      c_names.push_back(name.c_str());
    }
    names_ptr = c_names.data();
    names_count = c_names.size();
  }

  if (progress) {
    Check(detail::api()->Manager_DownloadAndRegisterEps(
        handle_.get_mutable(), names_ptr, names_count,
        [](const char* ep_name, float value, void* ctx) -> int {
          auto* fn = static_cast<std::function<bool(std::string_view, float)>*>(ctx);
          return (*fn)(ep_name, value) ? 0 : 1;
        },
        &progress));
  } else {
    Check(detail::api()->Manager_DownloadAndRegisterEps(
        handle_.get_mutable(), names_ptr, names_count, nullptr, nullptr));
  }
}

inline bool Manager::IsEpDownloadInProgress() const {
  return detail::api()->Manager_IsEpDownloadInProgress(handle_.get());
}

inline void Manager::Shutdown() {
  Check(detail::api()->Manager_Shutdown(handle_.get_mutable()));
}

inline bool Manager::IsShutdownRequested() const {
  return detail::api()->Manager_IsShutdownRequested(handle_.get());
}

inline flManager* detail::CreateManager(const Configuration& config) {
  flManager* mgr = nullptr;
  Check(detail::api()->Manager_Create(config.native_handle(), &mgr));
  return mgr;
}

// ===========================================================================
// ModelInfo
// ===========================================================================

inline std::string_view ModelInfo::Id() const noexcept {
  return safe(detail::model_api()->Info_GetId(info_));
}

inline std::string_view ModelInfo::Name() const noexcept {
  return safe(detail::model_api()->Info_GetName(info_));
}

inline int ModelInfo::Version() const noexcept {
  return detail::model_api()->Info_GetVersion(info_);
}

inline std::string_view ModelInfo::Alias() const noexcept {
  return safe(detail::model_api()->Info_GetAlias(info_));
}

inline std::string_view ModelInfo::Uri() const noexcept {
  return safe(detail::model_api()->Info_GetUri(info_));
}

inline flDeviceType ModelInfo::DeviceType() const noexcept {
  return detail::model_api()->Info_GetDeviceType(info_);
}

inline std::optional<std::string_view> ModelInfo::ExecutionProvider() const noexcept {
  const char* v = detail::model_api()->Info_GetExecutionProvider(info_);
  return v ? std::optional<std::string_view>{v} : std::nullopt;
}

inline std::optional<Runtime> ModelInfo::GetRuntime() const noexcept {
  flDeviceType dt = DeviceType();
  if (dt == FOUNDRY_LOCAL_DEVICE_NOTSET) {
    return std::nullopt;
  }
  return Runtime{dt, ExecutionProvider()};
}

inline std::optional<std::string_view> ModelInfo::GetPromptTemplate(const char* key) const noexcept {
  const flKeyValuePairs* kvps = detail::model_api()->Info_GetPromptTemplates(info_);
  if (!kvps) {
    return std::nullopt;
  }
  const char* v = detail::api()->GetKeyValue(kvps, key);
  return v ? std::optional<std::string_view>{v} : std::nullopt;
}

inline std::optional<std::string_view> ModelInfo::GetModelSetting(const char* key) const noexcept {
  const flKeyValuePairs* kvps = detail::model_api()->Info_GetModelSettings(info_);
  if (!kvps) {
    return std::nullopt;
  }
  const char* v = detail::api()->GetKeyValue(kvps, key);
  return v ? std::optional<std::string_view>{v} : std::nullopt;
}

inline std::optional<KeyValuePairs> ModelInfo::GetModelSettings() const noexcept {
  const flKeyValuePairs* kvps = detail::model_api()->Info_GetModelSettings(info_);
  if (!kvps) {
    return std::nullopt;
  }
  return KeyValuePairs(*kvps);
}

inline std::optional<std::string_view> ModelInfo::GetStringProperty(const char* key) const noexcept {
  const char* v = detail::model_api()->Info_GetStringProperty(info_, key);
  return v ? std::optional<std::string_view>{v} : std::nullopt;
}

inline int64_t ModelInfo::GetIntProperty(const char* key, int64_t default_value) const noexcept {
  return detail::model_api()->Info_GetIntProperty(info_, key, default_value);
}

// --- Typed property accessors ---

inline std::optional<std::string_view> ModelInfo::DisplayName() const noexcept {
  return GetStringProperty(FOUNDRY_LOCAL_MODEL_PROP_DISPLAY_NAME_STR);
}

inline std::optional<std::string_view> ModelInfo::ModelType() const noexcept {
  return GetStringProperty(FOUNDRY_LOCAL_MODEL_PROP_MODEL_TYPE_STR);
}

inline std::optional<std::string_view> ModelInfo::Publisher() const noexcept {
  return GetStringProperty(FOUNDRY_LOCAL_MODEL_PROP_PUBLISHER_STR);
}

inline std::optional<std::string_view> ModelInfo::License() const noexcept {
  return GetStringProperty(FOUNDRY_LOCAL_MODEL_PROP_LICENSE_STR);
}

inline std::optional<std::string_view> ModelInfo::LicenseDescription() const noexcept {
  return GetStringProperty(FOUNDRY_LOCAL_MODEL_PROP_LICENSE_DESCRIPTION_STR);
}

inline std::optional<std::string_view> ModelInfo::Task() const noexcept {
  return GetStringProperty(FOUNDRY_LOCAL_MODEL_PROP_TASK_STR);
}

inline std::optional<std::string_view> ModelInfo::ModelProvider() const noexcept {
  return GetStringProperty(FOUNDRY_LOCAL_MODEL_PROP_MODEL_PROVIDER_STR);
}

inline std::optional<std::string_view> ModelInfo::MinFlVersion() const noexcept {
  return GetStringProperty(FOUNDRY_LOCAL_MODEL_PROP_MIN_FL_VERSION_STR);
}

inline std::optional<std::string_view> ModelInfo::ParentUri() const noexcept {
  return GetStringProperty(FOUNDRY_LOCAL_MODEL_PROP_PARENT_URI_STR);
}

inline std::optional<bool> ModelInfo::SupportsToolCalling() const noexcept {
  int64_t v = GetIntProperty(FOUNDRY_LOCAL_MODEL_PROP_SUPPORTS_TOOL_CALLING_INT);
  if (v < 0) {
    return std::nullopt;
  }
  return v != 0;
}

inline std::optional<int64_t> ModelInfo::FilesizeMb() const noexcept {
  int64_t v = GetIntProperty(FOUNDRY_LOCAL_MODEL_PROP_FILESIZE_MB_INT);
  if (v < 0) {
    return std::nullopt;
  }
  return v;
}

inline std::optional<int64_t> ModelInfo::MaxOutputTokens() const noexcept {
  int64_t v = GetIntProperty(FOUNDRY_LOCAL_MODEL_PROP_MAX_OUTPUT_TOKENS_INT);
  if (v < 0) {
    return std::nullopt;
  }
  return v;
}

inline int64_t ModelInfo::CreatedAtUnix() const noexcept {
  return GetIntProperty(FOUNDRY_LOCAL_MODEL_PROP_CREATED_AT_UNIX_INT, 0);
}

inline bool ModelInfo::IsTestModel() const noexcept {
  return GetIntProperty(FOUNDRY_LOCAL_MODEL_PROP_IS_TEST_MODEL_INT, 0) != 0;
}

inline std::optional<int64_t> ModelInfo::ContextLength() const noexcept {
  int64_t v = GetIntProperty(FOUNDRY_LOCAL_MODEL_PROP_CONTEXT_LENGTH_INT, -1);
  if (v < 0) {
    return std::nullopt;
  }
  return v;
}

inline std::optional<std::string_view> ModelInfo::InputModalities() const noexcept {
  return GetStringProperty(FOUNDRY_LOCAL_MODEL_PROP_INPUT_MODALITIES_STR);
}

inline std::optional<std::string_view> ModelInfo::OutputModalities() const noexcept {
  return GetStringProperty(FOUNDRY_LOCAL_MODEL_PROP_OUTPUT_MODALITIES_STR);
}

inline std::optional<std::string_view> ModelInfo::Capabilities() const noexcept {
  return GetStringProperty(FOUNDRY_LOCAL_MODEL_PROP_CAPABILITIES_STR);
}

// ===========================================================================
// Model
// ===========================================================================

inline ModelInfo Model::GetInfo() const {
  const flModelInfo* info = nullptr;
  Check(detail::model_api()->GetInfo(handle_.get(), &info));
  return ModelInfo(*info);
}

inline bool Model::IsCached() const {
  int cached = 0;
  Check(detail::model_api()->IsCached(handle_.get(), &cached));
  return cached != 0;
}

inline bool Model::IsLoaded() const {
  int loaded = 0;
  Check(detail::model_api()->IsLoaded(handle_.get(), &loaded));
  return loaded != 0;
}

inline std::string_view Model::GetPath() const {
  const char* path = nullptr;
  Check(detail::model_api()->GetPath(handle_.get(), &path));
  return path ? path : "";
}

inline InputOutputInfo Model::GetInputOutputInfo() const {
  const flItem* const* inputs = nullptr;
  size_t num_inputs = 0;
  const flItem* const* outputs = nullptr;
  size_t num_outputs = 0;
  Check(detail::model_api()->GetInputOutputInfo(handle_.get(), &inputs, &num_inputs, &outputs, &num_outputs));

  InputOutputInfo info;
  info.inputs.reserve(num_inputs);
  for (size_t i = 0; i < num_inputs; ++i) {
    info.inputs.emplace_back(*inputs[i]);
  }
  info.outputs.reserve(num_outputs);
  for (size_t i = 0; i < num_outputs; ++i) {
    info.outputs.emplace_back(*outputs[i]);
  }
  return info;
}

inline ModelList Model::GetVariants() const {
  flModelList* variants = nullptr;
  Check(detail::model_api()->GetVariants(handle_.get(), &variants));
  return ModelList(*variants);
}

inline void Model::SelectVariant(const IModel& variant) {
  Check(detail::model_api()->SelectVariant(
      handle_.get_mutable(), detail::AsModel(variant).native_handle()));
}

inline void Model::Download(std::function<int(float)> progress) {
  if (progress) {
    Check(detail::model_api()->Download(
        handle_.get_mutable(),
        [](float value, void* ctx) -> int {
          return (*static_cast<std::function<int(float)>*>(ctx))(value);
        },
        &progress));
  } else {
    Check(detail::model_api()->Download(handle_.get_mutable(), nullptr, nullptr));
  }
}

inline void Model::Load() {
  Check(detail::model_api()->Load(handle_.get_mutable()));
}

inline void Model::Unload() {
  Check(detail::model_api()->Unload(handle_.get_mutable()));
}

inline void Model::RemoveFromCache() {
  Check(detail::model_api()->RemoveFromCache(handle_.get_mutable()));
}

// ===========================================================================
// ModelList
// ===========================================================================

inline ModelList::ModelList(flModelList& model_list)
    : handle_(&model_list, detail::api()->ModelList_Release) {
  size_t count = detail::api()->ModelList_Size(handle_.get());
  models_.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    models_.push_back(std::make_unique<Model>(*detail::api()->ModelList_GetAt(handle_.get(), i)));
  }
}

inline gsl::span<const std::unique_ptr<IModel>> ModelList::Models() const noexcept {
  return {models_.data(), models_.size()};
}

inline size_t ModelList::size() const noexcept {
  return models_.size();
}

// ===========================================================================
// Catalog
// ===========================================================================

inline std::string_view Catalog::GetName() const {
  const char* name = nullptr;
  Check(detail::catalog_api()->GetName(handle_.get(), &name));
  return name ? name : "";
}

inline ModelList Catalog::GetModels() const {
  flModelList* models = nullptr;
  Check(detail::catalog_api()->GetModels(handle_.get(), &models));
  return ModelList(*models);
}

inline std::unique_ptr<IModel> Catalog::GetModel(const std::string& alias) const {
  flModel* m = nullptr;
  Check(detail::catalog_api()->GetModel(handle_.get(), alias.c_str(), &m));
  if (!m) {
    return nullptr;
  }
  return std::make_unique<Model>(*m);
}

inline std::unique_ptr<IModel> Catalog::GetModelVariant(const std::string& model_id) const {
  flModel* m = nullptr;
  Check(detail::catalog_api()->GetModelVariant(handle_.get(), model_id.c_str(), &m));
  if (!m) {
    return nullptr;
  }
  return std::make_unique<Model>(*m);
}

inline ModelList Catalog::GetCachedModels() const {
  flModelList* models = nullptr;
  Check(detail::catalog_api()->GetCachedModels(handle_.get(), &models));
  return ModelList(*models);
}

inline ModelList Catalog::GetLoadedModels() const {
  flModelList* models = nullptr;
  Check(detail::catalog_api()->GetLoadedModels(handle_.get(), &models));
  return ModelList(*models);
}

inline std::unique_ptr<IModel> Catalog::GetLatestVersion(const IModel& model) const {
  flModel* m = nullptr;
  Check(detail::catalog_api()->GetLatestVersion(
      handle_.get(), detail::AsModel(model).native_handle(), &m));
  if (!m) {
    return nullptr;
  }
  return std::make_unique<Model>(*m);
}

inline ModelList Catalog::GetModelVersions(const std::string& model_alias,
                                           const std::string& variant_name,
                                           int max_versions) {
  flModelList* models = nullptr;
  Check(detail::catalog_api()->GetModelVersions(
      handle_.get(),
      model_alias.c_str(),
      variant_name.empty() ? nullptr : variant_name.c_str(),
      max_versions,
      &models));
  return ModelList(*models);
}

// ===========================================================================
// Item
// ===========================================================================

inline Item::Item(flItem& raw)
    : handle_(&raw, detail::item_api()->Item_Release) {}

inline Item::Item(flItemType type)
    : handle_(detail::CreateItem(type), detail::item_api()->Item_Release) {}

inline flItemType Item::GetType() const noexcept {
  return detail::item_api()->GetType(handle_.get());
}

inline BytesContent Item::GetBytes() const {
  flBytesData bytes{};
  bytes.version = FOUNDRY_LOCAL_API_VERSION;
  Check(detail::item_api()->GetBytes(handle_.get(), &bytes));
  return {bytes.item_type, bytes.data, bytes.data_size};
}

inline TextContent Item::GetText() const {
  flTextData data{};
  data.version = FOUNDRY_LOCAL_API_VERSION;
  Check(detail::item_api()->GetText(handle_.get(), &data));
  return {data.text ? std::string_view{data.text} : std::string_view{}, data.type};
}

inline TensorContent Item::GetTensor() const {
  flTensorData tensor{};
  tensor.version = FOUNDRY_LOCAL_API_VERSION;
  Check(detail::item_api()->GetTensor(handle_.get(), &tensor));
  return {tensor.data_type, tensor.data, {tensor.shape, tensor.shape + tensor.rank}};
}

inline ImageContent Item::GetImage() const {
  flImageData image{};
  image.version = FOUNDRY_LOCAL_API_VERSION;
  Check(detail::item_api()->GetImage(handle_.get(), &image));
  return {image.data, image.data_size,
          image.format ? std::optional<std::string_view>{image.format} : std::nullopt,
          image.uri ? std::optional<std::string_view>{image.uri} : std::nullopt};
}

inline AudioContent Item::GetAudio() const {
  flAudioData audio{};
  audio.version = FOUNDRY_LOCAL_API_VERSION;
  Check(detail::item_api()->GetAudio(handle_.get(), &audio));
  return {audio.data, audio.data_size,
          audio.format ? std::optional<std::string_view>{audio.format} : std::nullopt,
          audio.uri ? std::optional<std::string_view>{audio.uri} : std::nullopt,
          audio.sample_rate, audio.channels};
}

inline MessageContent Item::GetMessage() const {
  flMessageData msg{};
  msg.version = FOUNDRY_LOCAL_API_VERSION;
  Check(detail::item_api()->GetMessage(handle_.get(), &msg));

  MessageContent out;
  out.role = msg.role;
  out.name = msg.name ? std::optional<std::string_view>{msg.name} : std::nullopt;
  out.parts.reserve(msg.content_items_count);
  for (size_t i = 0; i < msg.content_items_count; ++i) {
    const flItem* part = msg.content_items[i];
    if (!part) continue;
    out.parts.emplace_back(*part);
  }
  return out;
}

inline bool MessageContent::IsSimpleText() const {
  return parts.size() == 1 && parts.front().GetType() == FOUNDRY_LOCAL_ITEM_TEXT;
}

inline std::string MessageContent::GetSimpleText() const {
  if (!IsSimpleText()) {
    throw Error("MessageContent is not a single TEXT part", FOUNDRY_LOCAL_ERROR_INVALID_USAGE);
  }
  return std::string(parts.front().GetText().text);
}

inline ToolCallContent Item::GetToolCall() const {
  flToolCallData tc{};
  tc.version = FOUNDRY_LOCAL_API_VERSION;
  Check(detail::item_api()->GetToolCall(handle_.get(), &tc));
  return {tc.call_id ? std::string_view{tc.call_id} : std::string_view{},
          tc.name ? std::string_view{tc.name} : std::string_view{},
          tc.arguments ? std::string_view{tc.arguments} : std::string_view{}};
}

inline ToolResultContent Item::GetToolResult() const {
  flToolResultData tr{};
  tr.version = FOUNDRY_LOCAL_API_VERSION;
  Check(detail::item_api()->GetToolResult(handle_.get(), &tr));
  return {tr.call_id ? std::string_view{tr.call_id} : std::string_view{},
          tr.result ? std::string_view{tr.result} : std::string_view{}};
}

namespace detail {

inline std::optional<int64_t> SpeechDuration(int64_t v) {
  return v == FOUNDRY_LOCAL_DURATION_UNSET ? std::optional<int64_t>{} : std::optional<int64_t>{v};
}

inline std::optional<float> SpeechConfidence(float v) {
  return v == FOUNDRY_LOCAL_CONFIDENCE_UNSET ? std::optional<float>{} : std::optional<float>{v};
}

inline std::optional<std::string_view> SpeechOptStr(const char* s) {
  return s ? std::optional<std::string_view>{s} : std::optional<std::string_view>{};
}

}  // namespace detail

inline SpeechSegmentContent Item::GetSpeechSegment() const {
  flSpeechSegmentData s{};
  s.version = FOUNDRY_LOCAL_API_VERSION;
  Check(detail::item_api()->GetSpeechSegment(handle_.get(), &s));

  SpeechSegmentContent out;
  out.kind = s.kind;
  out.text = s.text ? std::string_view{s.text} : std::string_view{};
  out.start_time_ms = detail::SpeechDuration(s.start_time_ms);
  out.end_time_ms = detail::SpeechDuration(s.end_time_ms);
  out.utterance_start = s.utterance_start;
  out.language = detail::SpeechOptStr(s.language);
  out.words.reserve(s.words_count);
  for (size_t i = 0; i < s.words_count; ++i) {
    const flSpeechWord& w = s.words[i];
    SpeechWord sw;
    sw.text = w.text ? std::string_view{w.text} : std::string_view{};
    sw.start_time_ms = detail::SpeechDuration(w.start_time_ms);
    sw.end_time_ms = detail::SpeechDuration(w.end_time_ms);
    sw.confidence = detail::SpeechConfidence(w.confidence);
    sw.speaker_id = detail::SpeechOptStr(w.speaker_id);
    out.words.push_back(std::move(sw));
  }
  return out;
}

inline SpeechResultContent Item::GetSpeechResult() const {
  flSpeechResultData r{};
  r.version = FOUNDRY_LOCAL_API_VERSION;
  Check(detail::item_api()->GetSpeechResult(handle_.get(), &r));

  SpeechResultContent out;
  out.text = r.text ? std::string_view{r.text} : std::string_view{};
  out.language = detail::SpeechOptStr(r.language);
  out.duration_ms = detail::SpeechDuration(r.duration_ms);
  out.segments.reserve(r.segments_count);
  for (size_t i = 0; i < r.segments_count; ++i) {
    if (r.segments[i]) {
      out.segments.emplace_back(*r.segments[i]);
    }
  }
  return out;
}

inline flItem* detail::CreateItem(flItemType type) {
  flItem* item = nullptr;
  Check(detail::item_api()->Create(type, &item));
  return item;
}

// ===========================================================================
// Item static factories
// ===========================================================================

inline Item Item::Text(const std::string& text, flTextItemType type) {
  Item item(FOUNDRY_LOCAL_ITEM_TEXT);
  flTextData data{};
  data.version = FOUNDRY_LOCAL_API_VERSION;
  data.text = text.c_str();
  data.type = type;
  Check(detail::item_api()->SetText(item.handle_.get_mutable(), &data));
  return item;
}

inline MessageItem::MessageItem(flMessageRole role, const std::string& content,
                                const std::optional<std::string>& name)
    : MessageItem(role, [&]() {
        std::vector<Item> v;
        v.push_back(Item::Text(content));
        return v; }(), name) {}

inline MessageItem::MessageItem(flMessageRole role, std::vector<Item> parts,
                                const std::optional<std::string>& name)
    : Item(FOUNDRY_LOCAL_ITEM_MESSAGE) {
  std::vector<const flItem*> ptrs;
  ptrs.reserve(parts.size());
  for (auto& p : parts) {
    ptrs.push_back(p.native_handle());
  }

  flMessageData msg{};
  msg.version = FOUNDRY_LOCAL_API_VERSION;
  msg.role = role;
  msg.content_items = ptrs.empty() ? nullptr : ptrs.data();
  msg.content_items_count = ptrs.size();
  msg.name = name ? name->c_str() : nullptr;
  // SetMessage deep-clones each part, so `parts` can safely be dropped after
  // this call returns.
  Check(detail::item_api()->SetMessage(handle_.get_mutable(), &msg));
}

inline Item Item::Bytes(flItemType item_type, const void* data, size_t data_size) {
  Item item(FOUNDRY_LOCAL_ITEM_BYTES);
  flBytesData bytes{};
  bytes.version = FOUNDRY_LOCAL_API_VERSION;
  bytes.item_type = item_type;
  bytes.data = data;
  bytes.mutable_data = nullptr;
  bytes.data_size = data_size;
  bytes.deleter = nullptr;
  bytes.deleter_user_data = nullptr;
  Check(detail::item_api()->SetBytes(item.handle_.get_mutable(), &bytes));
  return item;
}

inline Item Item::Bytes(flItemType item_type, void* data, size_t data_size,
                        std::function<void(const flBytesData*)> deleter) {
  auto helper = std::make_unique<detail::DataDeleterHelper<flBytesData>>(std::move(deleter));
  Item item(FOUNDRY_LOCAL_ITEM_BYTES);
  flBytesData bytes{};
  bytes.version = FOUNDRY_LOCAL_API_VERSION;
  bytes.item_type = item_type;
  bytes.data = data;
  bytes.mutable_data = data;
  bytes.data_size = data_size;
  bytes.deleter = &detail::DataDeleterHelper<flBytesData>::CDeleter;
  bytes.deleter_user_data = helper.get();
  // Ownership of `helper` only transfers to the C API on success; release after Check passes.
  Check(detail::item_api()->SetBytes(item.handle_.get_mutable(), &bytes));
  helper.release();
  return item;
}

inline Item Item::Tensor(flTensorDataType data_type, const void* data, const int64_t* shape, size_t rank) {
  Item item(FOUNDRY_LOCAL_ITEM_TENSOR);
  flTensorData tensor{};
  tensor.version = FOUNDRY_LOCAL_API_VERSION;
  tensor.data_type = data_type;
  tensor.data = data;
  tensor.mutable_data = nullptr;
  tensor.shape = shape;
  tensor.rank = rank;
  tensor.deleter = nullptr;
  tensor.deleter_user_data = nullptr;
  Check(detail::item_api()->SetTensor(item.handle_.get_mutable(), &tensor));
  return item;
}

inline Item Item::Tensor(flTensorDataType data_type, void* data, const int64_t* shape, size_t rank,
                         std::function<void(const flTensorData*)> deleter) {
  auto helper = std::make_unique<detail::DataDeleterHelper<flTensorData>>(std::move(deleter));
  Item item(FOUNDRY_LOCAL_ITEM_TENSOR);
  flTensorData tensor{};
  tensor.version = FOUNDRY_LOCAL_API_VERSION;
  tensor.data_type = data_type;
  tensor.data = data;
  tensor.mutable_data = data;
  tensor.shape = shape;
  tensor.rank = rank;
  tensor.deleter = &detail::DataDeleterHelper<flTensorData>::CDeleter;
  tensor.deleter_user_data = helper.get();
  // Ownership of `helper` only transfers to the C API on success; release after Check passes.
  Check(detail::item_api()->SetTensor(item.handle_.get_mutable(), &tensor));
  helper.release();
  return item;
}

inline Item Item::ImageFromData(const std::string& format, const void* data, size_t data_size) {
  Item item(FOUNDRY_LOCAL_ITEM_IMAGE);
  flImageData image{};
  image.version = FOUNDRY_LOCAL_API_VERSION;
  image.data = data;
  image.mutable_data = nullptr;
  image.data_size = data_size;
  image.format = format.c_str();
  image.uri = nullptr;
  image.deleter = nullptr;
  image.deleter_user_data = nullptr;
  Check(detail::item_api()->SetImage(item.handle_.get_mutable(), &image));
  return item;
}

inline Item Item::ImageFromData(const std::string& format, void* data, size_t data_size,
                                std::function<void(const flImageData*)> deleter) {
  auto helper = std::make_unique<detail::DataDeleterHelper<flImageData>>(std::move(deleter));
  Item item(FOUNDRY_LOCAL_ITEM_IMAGE);
  flImageData image{};
  image.version = FOUNDRY_LOCAL_API_VERSION;
  image.data = data;
  image.mutable_data = data;
  image.data_size = data_size;
  image.format = format.c_str();
  image.uri = nullptr;
  image.deleter = &detail::DataDeleterHelper<flImageData>::CDeleter;
  image.deleter_user_data = helper.get();
  // Ownership of `helper` only transfers to the C API on success; release after Check passes.
  Check(detail::item_api()->SetImage(item.handle_.get_mutable(), &image));
  helper.release();
  return item;
}

inline Item Item::ImageFromUri(const std::string& uri, const std::optional<std::string>& format) {
  Item item(FOUNDRY_LOCAL_ITEM_IMAGE);
  flImageData image{};
  image.version = FOUNDRY_LOCAL_API_VERSION;
  image.data = nullptr;
  image.mutable_data = nullptr;
  image.data_size = 0;
  image.format = format ? format->c_str() : nullptr;
  image.uri = uri.c_str();
  image.deleter = nullptr;
  image.deleter_user_data = nullptr;
  Check(detail::item_api()->SetImage(item.handle_.get_mutable(), &image));
  return item;
}

inline Item Item::AudioFromData(const std::string& format, const void* data, size_t data_size,
                                int sample_rate, int channels) {
  Item item(FOUNDRY_LOCAL_ITEM_AUDIO);
  flAudioData audio{};
  audio.version = FOUNDRY_LOCAL_API_VERSION;
  audio.data = data;
  audio.mutable_data = nullptr;
  audio.data_size = data_size;
  audio.format = format.c_str();
  audio.uri = nullptr;
  audio.sample_rate = sample_rate;
  audio.channels = channels;
  audio.deleter = nullptr;
  audio.deleter_user_data = nullptr;
  Check(detail::item_api()->SetAudio(item.handle_.get_mutable(), &audio));
  return item;
}

inline Item Item::AudioFromData(const std::string& format, void* data, size_t data_size,
                                std::function<void(const flAudioData*)> deleter,
                                int sample_rate, int channels) {
  auto helper = std::make_unique<detail::DataDeleterHelper<flAudioData>>(std::move(deleter));
  Item item(FOUNDRY_LOCAL_ITEM_AUDIO);
  flAudioData audio{};
  audio.version = FOUNDRY_LOCAL_API_VERSION;
  audio.data = data;
  audio.mutable_data = data;
  audio.data_size = data_size;
  audio.format = format.c_str();
  audio.uri = nullptr;
  audio.sample_rate = sample_rate;
  audio.channels = channels;
  audio.deleter = &detail::DataDeleterHelper<flAudioData>::CDeleter;
  audio.deleter_user_data = helper.get();
  // Ownership of `helper` only transfers to the C API on success; release after Check passes.
  Check(detail::item_api()->SetAudio(item.handle_.get_mutable(), &audio));
  helper.release();
  return item;
}

inline Item Item::AudioFromUri(const std::string& uri, const std::optional<std::string>& format,
                               int sample_rate, int channels) {
  Item item(FOUNDRY_LOCAL_ITEM_AUDIO);
  flAudioData audio{};
  audio.version = FOUNDRY_LOCAL_API_VERSION;
  audio.data = nullptr;
  audio.mutable_data = nullptr;
  audio.data_size = 0;
  audio.format = format ? format->c_str() : nullptr;
  audio.uri = uri.c_str();
  audio.sample_rate = sample_rate;
  audio.channels = channels;
  audio.deleter = nullptr;
  audio.deleter_user_data = nullptr;
  Check(detail::item_api()->SetAudio(item.handle_.get_mutable(), &audio));
  return item;
}

inline Item Item::ToolCall(const std::string& call_id, const std::string& name,
                           const std::string& arguments) {
  Item item(FOUNDRY_LOCAL_ITEM_TOOL_CALL);
  flToolCallData tc{FOUNDRY_LOCAL_API_VERSION, call_id.c_str(), name.c_str(), arguments.c_str()};
  Check(detail::item_api()->SetToolCall(item.handle_.get_mutable(), &tc));
  return item;
}

inline Item Item::ToolResult(const std::string& call_id, const std::string& result) {
  Item item(FOUNDRY_LOCAL_ITEM_TOOL_RESULT);
  flToolResultData tr{FOUNDRY_LOCAL_API_VERSION, call_id.c_str(), result.c_str()};
  Check(detail::item_api()->SetToolResult(item.handle_.get_mutable(), &tr));
  return item;
}

// ===========================================================================
// ItemQueue
// ===========================================================================

inline ItemQueue::ItemQueue() : Item(FOUNDRY_LOCAL_ITEM_QUEUE) {}

inline void ItemQueue::Push(Item&& item) {
  flItemQueue* queue = nullptr;
  Check(detail::item_api()->GetQueue(handle_.get_mutable(), &queue));
  Check(detail::item_api()->ItemQueue_Push(queue, item.native_handle_mutable()));
  item.detach();
}

inline std::optional<Item> ItemQueue::TryPop() {
  flItemQueue* queue = nullptr;
  Check(detail::item_api()->GetQueue(handle_.get_mutable(), &queue));

  flItem* item = nullptr;
  if (!detail::item_api()->ItemQueue_TryPop(queue, &item) || !item) {
    return std::nullopt;
  }
  return Item(*item);
}

inline size_t ItemQueue::Size() {
  flItemQueue* queue = nullptr;
  Check(detail::item_api()->GetQueue(handle_.get_mutable(), &queue));
  return detail::item_api()->ItemQueue_Size(queue);
}

inline void ItemQueue::MarkFinished() {
  flItemQueue* queue = nullptr;
  Check(detail::item_api()->GetQueue(handle_.get_mutable(), &queue));
  detail::item_api()->ItemQueue_MarkFinished(queue);
}

inline bool ItemQueue::IsFinished() {
  flItemQueue* queue = nullptr;
  Check(detail::item_api()->GetQueue(handle_.get_mutable(), &queue));
  return detail::item_api()->ItemQueue_IsFinished(queue);
}

// ===========================================================================
// Request
// ===========================================================================

inline Request::Request()
    : handle_(detail::CreateRequest(), detail::inference_api()->Request_Release) {}

inline Request& Request::AddItem(Item& item, bool take_ownership) {
  Check(detail::inference_api()->Request_AddItem(handle_.get_mutable(), item.native_handle_mutable(),
                                                 take_ownership));
  if (take_ownership) {
    item.detach();
  }
  return *this;
}

inline size_t Request::GetItemCount() const noexcept {
  return detail::inference_api()->Request_GetItemCount(handle_.get());
}

inline Item Request::GetItem(size_t idx) const {
  const flItem* item = nullptr;
  Check(detail::inference_api()->Request_GetItem(handle_.get(), idx, &item));
  return Item(*item);
}

inline Request& Request::SetOptions(const RequestOptions& options) {
  KeyValuePairs kvp = detail::ToKeyValuePairs(options);
  Check(detail::inference_api()->Request_SetOptions(handle_.get_mutable(), kvp.native_handle()));
  return *this;
}

inline void Request::Cancel() {
  Check(detail::inference_api()->Request_Cancel(handle_.get_mutable()));
}

inline flRequest* detail::CreateRequest() {
  flRequest* req = nullptr;
  Check(detail::inference_api()->Request_Create(&req));
  return req;
}

// ===========================================================================
// Response
// ===========================================================================

inline Response::Response(flResponse* response)
    : handle_(response, detail::inference_api()->Response_Release) {
  auto item_count = detail::inference_api()->Response_GetItemCount(handle_.get());
  items_.reserve(item_count);

  for (size_t i = 0; i < item_count; ++i) {
    const flItem* item = nullptr;
    Check(detail::inference_api()->Response_GetItem(handle_.get(), i, &item));
    items_.emplace_back(*item);
  }
}

inline flFinishReason Response::GetFinishReason() const noexcept {
  return detail::inference_api()->Response_GetFinishReason(handle_.get());
}

inline flUsage Response::GetUsage() const {
  flUsage usage{};
  usage.version = FOUNDRY_LOCAL_API_VERSION;
  Check(detail::inference_api()->Response_GetUsage(handle_.get(), &usage));
  return usage;
}

inline flResponse* detail::CreateResponse() {
  flResponse* resp = nullptr;
  Check(detail::inference_api()->Response_Create(&resp));
  return resp;
}

// ===========================================================================
// Session
// ===========================================================================

inline Session::Session(IModel& model)
    : handle_(detail::CreateSession(model), detail::inference_api()->Session_Release) {}

inline Response Session::ProcessRequest(const Request& request) {
  flResponse* response = nullptr;
  Check(detail::inference_api()->Session_ProcessRequest(
      handle_.get_mutable(), request.native_handle(), &response));
  return Response(response);
}

inline Session& Session::SetOptions(const RequestOptions& options) {
  KeyValuePairs kvp = detail::ToKeyValuePairs(options);
  Check(detail::inference_api()->Session_SetOptions(handle_.get_mutable(), kvp.native_handle()));
  return *this;
}

inline Session& Session::SetStreamingCallback(std::function<int(flStreamingCallbackData)> callback) {
  flStreamingCallback callback_fn = nullptr;
  void* user_data = nullptr;

  if (callback) {
    streaming_callback_helper_ = std::make_unique<detail::StreamingCallbackHelper>(std::move(callback));
    callback_fn = &detail::StreamingCallbackHelper::CCallback;
    user_data = streaming_callback_helper_.get();
  } else {
    streaming_callback_helper_.reset();
  }

  Check(detail::inference_api()->Session_SetStreamingCallback(handle_.get_mutable(), callback_fn, user_data));
  return *this;
}

inline flSession* detail::CreateSession(IModel& model) {
  flSession* session = nullptr;
  Check(detail::inference_api()->Session_Create(
      detail::AsModel(model).native_handle(), &session));
  return session;
}

// ===========================================================================
// ChatSession
// ===========================================================================

inline ChatSession::ChatSession(IModel& model) : Session(model) {
  auto task = model.GetInfo().Task();
  if (task != "chat-completion" && task != "vision-language-chat") {
    throw std::invalid_argument("Model is not designed for chat tasks");
  }
}

inline ChatSession& ChatSession::AddToolDefinition(const ToolDefinition& tool_def) {
  flToolDefinition c_def = tool_def.ToC();
  Check(detail::inference_api()->Session_AddToolDefinition(handle_.get_mutable(), &c_def));
  return *this;
}

inline bool ChatSession::RemoveToolDefinition(std::string_view tool_name) {
  // The C ABI requires a NUL-terminated string. string_view is not guaranteed to be NUL-terminated,
  // so materialize a temporary std::string here.
  std::string name(tool_name);
  bool removed = false;
  Check(detail::inference_api()->Session_RemoveToolDefinition(handle_.get_mutable(), name.c_str(), &removed));
  return removed;
}

inline size_t ChatSession::TurnCount() const {
  return detail::inference_api()->Session_GetTurnCount(handle_.get());
}

inline void ChatSession::UndoTurns(size_t count) {
  Check(detail::inference_api()->Session_UndoTurns(handle_.get_mutable(), count));
}

// ===========================================================================
// AudioSession
// ===========================================================================

inline AudioSession::AudioSession(IModel& model) : Session(model) {
  if (model.GetInfo().Task() != "automatic-speech-recognition") {
    throw std::invalid_argument("Model is not designed for audio transcription tasks");
  }
}

// ===========================================================================
// EmbeddingsSession
// ===========================================================================

inline EmbeddingsSession::EmbeddingsSession(IModel& model) : Session(model) {
  if (model.GetInfo().Task() != "embeddings") {
    throw std::invalid_argument("Model is not designed for text embedding tasks");
  }
}

inline std::vector<float> EmbeddingsSession::Embed(const std::string& input) {
  std::vector<std::string> inputs{input};
  auto results = Embed(inputs);
  return std::move(results[0]);
}

inline std::vector<std::vector<float>> EmbeddingsSession::Embed(const std::vector<std::string>& inputs) {
  Request request;
  for (const auto& input : inputs) {
    request.AddItem(Item::Text(input));
  }

  Response response = ProcessRequest(request);
  const auto& items = response.GetItems();
  if (items.size() != inputs.size()) {
    throw std::runtime_error("EmbeddingsSession::Embed: response item count does not match input count");
  }

  std::vector<std::vector<float>> results;
  results.reserve(items.size());
  for (const auto& item : items) {
    if (item.GetType() != FOUNDRY_LOCAL_ITEM_TENSOR) {
      throw std::runtime_error("EmbeddingsSession::Embed: response item is not a tensor");
    }

    TensorContent tensor = item.GetTensor();
    if (tensor.data_type != FOUNDRY_LOCAL_TENSOR_FLOAT) {
      throw std::runtime_error("EmbeddingsSession::Embed: tensor data type is not float");
    }

    if (tensor.shape.empty()) {
      throw std::runtime_error("EmbeddingsSession::Embed: tensor shape is empty");
    }

    size_t count = 1;
    for (auto d : tensor.shape) {
      count *= static_cast<size_t>(d);
    }
    const auto* data = static_cast<const float*>(tensor.data);
    results.emplace_back(data, data + count);
  }

  return results;
}

// ===========================================================================
// RequestOptions — build a KeyValuePairs for the C ABI
// ===========================================================================

namespace detail {

/// Build a KeyValuePairs from a RequestOptions:
///   1. Seed with additional_options so typed fields win on key collision.
///   2. Layer typed SearchOptions fields on top.
///   3. Layer tool_choice on top.
inline KeyValuePairs ToKeyValuePairs(const RequestOptions& opts) {
  KeyValuePairs kvp;

  for (const auto& entry : opts.additional_options.GetAll()) {
    // GetAll returns string_views; KeyValuePairs::Set requires null-terminated C strings.
    std::string key(entry.key);
    std::string value(entry.value);
    kvp.Set(key.c_str(), value.c_str());
  }

  const auto& s = opts.search;

  if (s.temperature.has_value()) {
    kvp.Set(FOUNDRY_LOCAL_PARAM_TEMPERATURE, std::to_string(*s.temperature).c_str());
  }

  if (s.top_p.has_value()) {
    kvp.Set(FOUNDRY_LOCAL_PARAM_TOP_P, std::to_string(*s.top_p).c_str());
  }

  if (s.top_k.has_value()) {
    kvp.Set(FOUNDRY_LOCAL_PARAM_TOP_K, std::to_string(*s.top_k).c_str());
  }

  if (s.max_output_tokens.has_value()) {
    kvp.Set(FOUNDRY_LOCAL_PARAM_MAX_OUTPUT_TOKENS, std::to_string(*s.max_output_tokens).c_str());
  }

  if (s.frequency_penalty.has_value()) {
    kvp.Set(FOUNDRY_LOCAL_PARAM_FREQUENCY_PENALTY, std::to_string(*s.frequency_penalty).c_str());
  }

  if (s.presence_penalty.has_value()) {
    kvp.Set(FOUNDRY_LOCAL_PARAM_PRESENCE_PENALTY, std::to_string(*s.presence_penalty).c_str());
  }

  if (s.seed.has_value()) {
    kvp.Set(FOUNDRY_LOCAL_PARAM_SEED, std::to_string(*s.seed).c_str());
  }

  if (s.early_stopping.has_value()) {
    kvp.Set(FOUNDRY_LOCAL_PARAM_EARLY_STOPPING, *s.early_stopping ? "true" : "false");
  }

  if (s.do_sample.has_value()) {
    kvp.Set(FOUNDRY_LOCAL_PARAM_DO_SAMPLE, *s.do_sample ? "true" : "false");
  }

  if (opts.tool_choice.has_value()) {
    const char* value = nullptr;
    switch (*opts.tool_choice) {
      case FOUNDRY_LOCAL_TOOL_CHOICE_AUTO:
        value = "auto";
        break;
      case FOUNDRY_LOCAL_TOOL_CHOICE_NONE:
        value = "none";
        break;
      case FOUNDRY_LOCAL_TOOL_CHOICE_REQUIRED:
        value = "required";
        break;
    }

    if (value != nullptr) {
      kvp.Set(FOUNDRY_LOCAL_PARAM_TOOL_CHOICE, value);
    }
  }

  return kvp;
}

}  // namespace detail

}  // namespace foundry_local
