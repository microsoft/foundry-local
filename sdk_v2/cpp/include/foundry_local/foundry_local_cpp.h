// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

/// @file foundry_local_cpp.h
/// Header-only C++ wrapper over the Foundry Local C API.
/// External consumers include this + link against the shared library.
///
/// This file contains class declarations only. All method implementations
/// are in foundry_local_cpp.inline.h, included at the end.
#pragma once

// This header requires C++17 or later.
// The sdk_integration_test target builds as C++17 to enforce this.
#if defined(__cplusplus) && __cplusplus < 201703L && !defined(_MSVC_LANG)
#error "foundry_local_cpp.h requires C++17 or later."
#elif defined(_MSVC_LANG) && _MSVC_LANG < 201703L
#error "foundry_local_cpp.h requires C++17 or later."
#endif

#include "foundry_local/foundry_local_c.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <gsl/span>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace foundry_local {

// ===========================================================================
// Error handling
// ===========================================================================

/// Exception thrown when a C API call fails.
class Error : public std::runtime_error {
 public:
  Error(flStatus& status);
  Error(std::string_view message, flErrorCode code);
  flErrorCode Code() const noexcept { return code_; }

 private:
  static const char* GetErrorMessage(flStatus& status);
  flErrorCode code_;
};

/// Check a C API status; throw Error if non-null (non-null = error).
inline void Check(flStatus* status);

/// Get the library version string.
inline const char* Version() noexcept;

namespace detail {

/// Get the API function table (cached on first call).
/// Returns nullptr if the library does not support the requested API version.
inline const flApi* api() {
  static const flApi* p = FoundryLocalGetApi(FOUNDRY_LOCAL_API_VERSION);
  return p;
}

/// Get the Catalog sub-API (cached on first call).
inline const flCatalogApi* catalog_api() {
  static const flCatalogApi* p = api()->GetCatalogApi();
  return p;
}

/// Get the Configuration sub-API (cached on first call).
inline const flConfigurationApi* config_api() {
  static const flConfigurationApi* p = api()->GetConfigurationApi();
  return p;
}

/// Get the Inference sub-API (cached on first call).
inline const flInferenceApi* inference_api() {
  static const flInferenceApi* p = api()->GetInferenceApi();
  return p;
}

/// Get the Item sub-API (cached on first call).
inline const flItemApi* item_api() {
  static const flItemApi* p = api()->GetItemApi();
  return p;
}

/// Get the Model sub-API (cached on first call).
inline const flModelApi* model_api() {
  static const flModelApi* p = api()->GetModelApi();
  return p;
}

/// RAII handle for opaque C types. Supports dual mutable/const pointer design.
/// Mutable construction stores both pointers and optionally owns the resource.
/// Const construction stores only the read-only pointer (non-owning view).
template <typename T>
class Base {
 public:
  // Mutable construction — both pointers set. Optionally owning (release_fn).
  explicit Base(T* p, std::function<void(T*)> release_fn = nullptr) noexcept
      : const_p_(p), mutable_p_(p), release_fn_(std::move(release_fn)) {}

  // Const construction — read-only view, never owns.
  explicit Base(const T* p) noexcept : const_p_(p) {}

  ~Base() {
    if (mutable_p_ && release_fn_) {
      release_fn_(mutable_p_);
    }
  }

  Base(const Base&) = delete;
  Base& operator=(const Base&) = delete;

  Base(Base&& v) noexcept
      : const_p_(v.const_p_), mutable_p_(v.mutable_p_), release_fn_(std::move(v.release_fn_)) {
    v.const_p_ = nullptr;
    v.mutable_p_ = nullptr;
  }

  Base& operator=(Base&& v) noexcept {
    if (this != &v) {
      if (mutable_p_ && release_fn_) {
        release_fn_(mutable_p_);
      }
      const_p_ = v.const_p_;
      mutable_p_ = v.mutable_p_;
      release_fn_ = std::move(v.release_fn_);
      v.const_p_ = nullptr;
      v.mutable_p_ = nullptr;
    }
    return *this;
  }

  const T* get() const noexcept { return const_p_; }

  T* get_mutable() {
    if (!mutable_p_) {
      throw Error("Mutable access on a read-only handle", FOUNDRY_LOCAL_ERROR_INVALID_USAGE);
    }
    return mutable_p_;
  }

  bool is_mutable() const noexcept { return mutable_p_ != nullptr; }

  void detach() noexcept {
    const_p_ = nullptr;
    mutable_p_ = nullptr;
    release_fn_ = nullptr;
  }

 private:
  const T* const_p_{};
  T* mutable_p_{};
  std::function<void(T*)> release_fn_{};
};

}  // namespace detail

// ===========================================================================
// Forward declarations
// ===========================================================================

class IModel;
class ICatalog;
class Catalog;
class Model;
class ModelList;
class Item;
struct InputOutputInfo;

// ===========================================================================
// KeyValuePairs
// ===========================================================================

/// A key/value entry.
struct KeyValueEntry {
  std::string_view key;
  std::string_view value;
};

/// Unified key/value pair wrapper. Supports both owning (mutable) and non-owning (read-only) modes.
class KeyValuePairs {
 public:
  /// Create new empty (mutable, owning).
  KeyValuePairs();

  /// Create from initializer list (mutable, owning).
  KeyValuePairs(std::initializer_list<std::pair<const char*, const char*>> pairs);

  /// Read-only view (non-owning).
  explicit KeyValuePairs(const flKeyValuePairs& p) : handle_(&p) {}

  /// Mutable non-owning (e.g. from GetMutableMetadata).
  explicit KeyValuePairs(flKeyValuePairs& p) : handle_(&p) {}

  KeyValuePairs(KeyValuePairs&&) noexcept = default;
  KeyValuePairs& operator=(KeyValuePairs&&) noexcept = default;

  /// Get a value by key. Returns nullopt if not found.
  std::optional<std::string_view> Get(const char* key) const noexcept;

  /// Get all key/value pairs.
  std::vector<KeyValueEntry> GetAll() const;

  /// Native handle accessor (const).
  const flKeyValuePairs* native_handle() const noexcept { return handle_.get(); }

  /// Add or replace a key/value pair. Throws if read-only.
  KeyValuePairs& Set(const char* key, const char* value);

  /// Remove a key. Throws if read-only.
  KeyValuePairs& Remove(const char* key);

 private:
  detail::Base<flKeyValuePairs> handle_;
};

// ===========================================================================
// Configuration
// ===========================================================================

class Configuration {
 public:
  explicit Configuration(const std::string& app_name);

  Configuration(Configuration&&) noexcept = default;
  Configuration& operator=(Configuration&&) noexcept = default;

  /// Optional. Directory for Foundry Local SDK app data. Defaults to ~/.<app_name>.
  /// Use `{home}` as a placeholder for the user's home directory.
  Configuration& SetAppDataDir(const std::string& value);

  /// Optional. Directory for log files. Defaults to <app_data_dir>/logs.
  Configuration& SetLogsDir(const std::string& value);

  /// Optional. Directory for cached models. Defaults to <app_data_dir>/cache/models.
  Configuration& SetModelCacheDir(const std::string& value);

  /// Optional. Default log level. Defaults to Warning.
  Configuration& SetDefaultLogLevel(flLogLevel level);

  /// Optional. Add a catalog URL to connect to.
  /// Defaults to the Azure Foundry Local Catalog if none are added.
  Configuration& AddCatalogUrl(const std::string& url,
                               const std::optional<std::string>& filter_override = std::nullopt);

  /// Optional. Add an endpoint for the web service to bind to.
  /// Defaults to "http://127.0.0.1:0" (ephemeral port) if none are added.
  Configuration& AddWebServiceEndpoint(const std::string& url);

  /// Optional. Set additional/undocumented options as key/value pairs.
  /// These are passed through to the core implementation.
  Configuration& SetAdditionalOptions(const KeyValuePairs& options);

  /// Optional. URL of an external Foundry Local service for client-only mode.
  /// When set, the catalog reads only from the local disk cache and local-only
  /// operations (StartWebService, session creation) return errors.
  Configuration& SetExternalServiceUrl(const std::string& url);

  /// Optional. Azure region for the model registry download endpoint.
  /// Defaults to "centralus" when not set.
  Configuration& SetCatalogRegion(const std::string& region);

  const flConfiguration* native_handle() const noexcept { return handle_.get(); }

 private:
  detail::Base<flConfiguration> handle_;
};

// ===========================================================================
// EpInfo — execution provider discovery result
// ===========================================================================

/// Information about a discoverable execution provider.
struct EpInfo {
  std::string name;    ///< EP name (e.g. "CUDAExecutionProvider")
  bool is_registered;  ///< Whether this EP has been successfully registered
};

// ===========================================================================
// Runtime — device + execution provider pair
// ===========================================================================

/// Runtime info for a model variant: where it runs (device + execution provider).
struct Runtime {
  flDeviceType device_type;
  std::optional<std::string_view> execution_provider;
};

// ===========================================================================
// ModelInfo — non-owning read-only view
// ===========================================================================

/// Non-owning view over an opaque flModelInfo. Lifetime is tied to the owning Model/Catalog. Immutable.
class ModelInfo {
 public:
  explicit ModelInfo(const flModelInfo& info) noexcept : info_(&info) {}

  // Core identity.
  std::string_view Id() const noexcept;
  std::string_view Name() const noexcept;
  int Version() const noexcept;
  std::string_view Alias() const noexcept;
  std::string_view Uri() const noexcept;
  flDeviceType DeviceType() const noexcept;
  std::optional<std::string_view> ExecutionProvider() const noexcept;
  /// Returns the device + execution provider pair, or nullopt if device_type is unknown/invalid.
  std::optional<Runtime> GetRuntime() const noexcept;

  // Key-value lookups
  std::optional<std::string_view> GetPromptTemplate(const char* key) const noexcept;
  std::optional<std::string_view> GetModelSetting(const char* key) const noexcept;

  /// Default/recommended inference settings declared by the model author.
  /// Returns a non-owning read-only view; nullopt if no settings are declared.
  /// Useful for discovering which parameters a model supports overriding.
  std::optional<KeyValuePairs> GetModelSettings() const noexcept;

  /// Get a string property by key. Known keys are defined by the FOUNDRY_LOCAL_MODEL_PROP_*_STR constants.
  std::optional<std::string_view> GetStringProperty(const char* key) const noexcept;

  /// Get an int property by key. Known keys are defined by the FOUNDRY_LOCAL_MODEL_PROP_*_INT constants.
  /// Returns default_value if key is not set.
  int64_t GetIntProperty(const char* key, int64_t default_value = -1) const noexcept;

  // --- Typed property accessors (convenience wrappers over Get{String,Int}Property) ---

  /// Display name shown in UIs. May differ from name().
  std::optional<std::string_view> DisplayName() const noexcept;
  /// Model format, e.g. "onnx".
  std::optional<std::string_view> ModelType() const noexcept;
  /// Publisher / organization.
  std::optional<std::string_view> Publisher() const noexcept;
  /// SPDX license identifier.
  std::optional<std::string_view> License() const noexcept;
  /// Human-readable license description.
  std::optional<std::string_view> LicenseDescription() const noexcept;
  /// Task the model is designed for, e.g. "chat", "text-generation".
  std::optional<std::string_view> Task() const noexcept;
  /// Source of the model, e.g. "AzureCatalog".
  std::optional<std::string_view> ModelProvider() const noexcept;
  /// Minimum Foundry Local version required to run this model.
  std::optional<std::string_view> MinFlVersion() const noexcept;
  /// URI of the parent model (for derived/quantized variants).
  std::optional<std::string_view> ParentUri() const noexcept;

  /// Whether the model supports tool/function calling. nullopt if unspecified.
  std::optional<bool> SupportsToolCalling() const noexcept;
  /// Download size in megabytes. nullopt if unspecified.
  std::optional<int64_t> FilesizeMb() const noexcept;
  /// Maximum output tokens the model can generate. nullopt if unspecified.
  std::optional<int64_t> MaxOutputTokens() const noexcept;
  /// Unix timestamp when the model entry was created. 0 if unset.
  int64_t CreatedAtUnix() const noexcept;
  /// Whether this is a test/synthetic model (not for production).
  bool IsTestModel() const noexcept;
  /// Maximum context length in tokens. nullopt if unspecified.
  std::optional<int64_t> ContextLength() const noexcept;
  /// Comma-separated list of supported input modalities (e.g. "text,image"). nullopt if unspecified.
  std::optional<std::string_view> InputModalities() const noexcept;
  /// Comma-separated list of supported output modalities. nullopt if unspecified.
  std::optional<std::string_view> OutputModalities() const noexcept;
  /// Comma-separated list of model capabilities. nullopt if unspecified.
  std::optional<std::string_view> Capabilities() const noexcept;

 private:
  static std::string_view safe(const char* s) noexcept { return s ? s : ""; }
  const flModelInfo* info_;
};

// ===========================================================================
// Content structs (pure data — no methods)
//
// All `*Content` structs returned by `Item::Get*()` are non-owning views over
// storage owned by the underlying `Item`. Pointers, `std::string_view`s, and
// nested `Item` parts remain valid only for the lifetime of the source `Item`
// (and until that Item is mutated). Copy fields into owning containers
// (`std::string`, `std::vector`, etc.) if you need them to outlive the Item.
// ===========================================================================

/// Content returned from a TEXT item. Carries the text plus a `flTextItemType` subtype tag so callers
/// can distinguish ordinary assistant text from reasoning / chain-of-thought content without losing data.
struct TextContent {
  std::string_view text;
  flTextItemType type = FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT;
};

/// Content returned from a BYTES item.
struct BytesContent {
  flItemType item_type;  ///< The type of item these bytes represent.
  const void* data;      ///< Raw byte buffer (item-internal, do not free).
  size_t data_size;      ///< Byte count.
};

/// Content returned from a TENSOR item. Data points into the item's internal buffer.
struct TensorContent {
  flTensorDataType data_type;
  const void* data;
  std::vector<int64_t> shape;
};

/// Content returned from an IMAGE item. Either bytes (data + data_size + format) or URI.
struct ImageContent {
  const void* data;                        ///< Raw bytes (nullptr when URI-based).
  size_t data_size;                        ///< Byte count (0 when URI-based).
  std::optional<std::string_view> format;  ///< Image format.
  std::optional<std::string_view> uri;     ///< Set when image is provided as a URI.
};

/// Content returned from an AUDIO item. Either bytes (data + data_size + format) or URI.
struct AudioContent {
  const void* data;                        ///< Raw bytes (nullptr when URI-based).
  size_t data_size;                        ///< Byte count (0 when URI-based).
  std::optional<std::string_view> format;  ///< Audio format.
  std::optional<std::string_view> uri;     ///< Set when audio is provided as a URI.
  int sample_rate;                         ///< Sample rate in Hz (0 = unspecified).
  int channels;                            ///< Number of channels (0 = unspecified).
};

/// Content returned from a MESSAGE item.
///
/// `parts` exposes the typed content parts (TEXT / IMAGE / AUDIO `Item`s).
/// Each entry is a non-owning view over a part owned by the underlying message.
/// Use `Item::GetText()` / `GetImage()` / `GetAudio()` to read part data.
///
/// Most chat messages have a single TEXT part. Use `IsSimpleText()` to check
/// for that shape and `GetSimpleText()` to read its text without iterating.
struct MessageContent {
  flMessageRole role;
  std::vector<Item> parts;
  std::optional<std::string_view> name;

  /// True when the message has exactly one part and it is a TEXT item.
  bool IsSimpleText() const;

  /// Text of the single TEXT part. Throws if !IsSimpleText().
  std::string GetSimpleText() const;
};

/// Content returned from a TOOL_CALL item.
struct ToolCallContent {
  std::string_view call_id;
  std::string_view name;
  std::string_view arguments;
};

/// Content returned from a TOOL_RESULT item.
struct ToolResultContent {
  std::string_view call_id;
  std::string_view result;
};

/// One word in a SPEECH_SEGMENT. Optional fields use std::optional / empty string_view.
struct SpeechWord {
  std::string_view text;
  std::optional<int64_t> start_time_ms;
  std::optional<int64_t> end_time_ms;
  std::optional<float> confidence;
  std::optional<std::string_view> speaker_id;
};

/// Content returned from a SPEECH_SEGMENT item (output-only).
///
/// See SpeechOutputTypes.md for the streaming model. PARTIAL `text` is the
/// cumulative current hypothesis for the segment, not a delta. As an entry of
/// a SpeechResultContent, `kind` is FINAL (or NONE for a single non-segmented
/// transcript).
struct SpeechSegmentContent {
  flSpeechSegmentKind kind;
  std::string_view text;
  std::optional<int64_t> start_time_ms;
  std::optional<int64_t> end_time_ms;
  bool utterance_start;
  std::vector<SpeechWord> words;
  std::optional<std::string_view> language;
};

/// Content returned from a SPEECH_RESULT item (output-only).
/// `segments` exposes non-owning views over segment items owned by the result.
struct SpeechResultContent {
  std::string_view text;
  std::optional<std::string_view> language;
  std::optional<int64_t> duration_ms;
  std::vector<Item> segments;
};

// ===========================================================================
// Item
// ===========================================================================

/// Unified item wrapper. Supports both owning (mutable) and non-owning (read-only) modes.
class Item {
 public:
  /// Adopt an already-created item (owning, mutable).
  explicit Item(flItem& raw);

  /// Read-only view (non-owning).
  explicit Item(const flItem& raw) : handle_(&raw) {}

  Item(Item&&) noexcept = default;
  Item& operator=(Item&&) noexcept = default;

  // --- Read accessors (always work) ---

  flItemType GetType() const noexcept;
  BytesContent GetBytes() const;
  TextContent GetText() const;
  TensorContent GetTensor() const;
  ImageContent GetImage() const;
  AudioContent GetAudio() const;
  MessageContent GetMessage() const;
  ToolCallContent GetToolCall() const;
  ToolResultContent GetToolResult() const;
  SpeechSegmentContent GetSpeechSegment() const;
  SpeechResultContent GetSpeechResult() const;

  const flItem* native_handle() const noexcept { return handle_.get(); }
  flItem* native_handle_mutable() { return handle_.get_mutable(); }
  void detach() noexcept { handle_.detach(); }

  // --- Static factory methods ---

  /// Create a text item with an optional subtype tag (default = ordinary text).
  static Item Text(const std::string& text,
                   flTextItemType type = FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT);

  /// Create a non-owning bytes item. Caller must keep `data` alive for the item's lifetime.
  static Item Bytes(flItemType item_type, const void* data, size_t data_size);

  /// Create an owning bytes item. Ownership of `data` is transferred to the item.
  static Item Bytes(flItemType item_type, void* data, size_t data_size,
                    std::function<void(const flBytesData*)> deleter);

  /// Create a non-owning tensor item. Caller must keep `data` alive for the item's lifetime.
  static Item Tensor(flTensorDataType data_type, const void* data, const int64_t* shape, size_t rank);

  /// Create an owning tensor item. Ownership of `data` is transferred to the item.
  static Item Tensor(flTensorDataType data_type, void* data, const int64_t* shape, size_t rank,
                     std::function<void(const flTensorData*)> deleter);

  /// Create a non-owning image from raw bytes. format is e.g. "png". Caller must keep `data` alive.
  static Item ImageFromData(const std::string& format, const void* data, size_t data_size);

  /// Create an owning image from raw bytes. Ownership of `data` is transferred to the item.
  static Item ImageFromData(const std::string& format, void* data, size_t data_size,
                            std::function<void(const flImageData*)> deleter);

  /// Create an image from a URI (file path, URL, etc.).
  static Item ImageFromUri(const std::string& uri, const std::optional<std::string>& format = std::nullopt);

  /// Create a non-owning audio from raw bytes. format is e.g. "mp3". Caller must keep `data` alive.
  static Item AudioFromData(const std::string& format, const void* data, size_t data_size,
                            int sample_rate = 0, int channels = 0);

  /// Create an owning audio from raw bytes. Ownership of `data` is transferred to the item.
  static Item AudioFromData(const std::string& format, void* data, size_t data_size,
                            std::function<void(const flAudioData*)> deleter,
                            int sample_rate = 0, int channels = 0);

  /// Create an audio item from a URI (file path, URL, etc.).
  static Item AudioFromUri(const std::string& uri, const std::optional<std::string>& format = std::nullopt,
                           int sample_rate = 0, int channels = 0);

  /// Create a tool call item.
  static Item ToolCall(const std::string& call_id, const std::string& name, const std::string& arguments);

  /// Create a tool result item.
  static Item ToolResult(const std::string& call_id, const std::string& result);

 protected:
  // For ItemQueue to construct via item type
  explicit Item(flItemType type);
  detail::Base<flItem> handle_;
};

/// Describes a model's expected inputs and outputs.
struct InputOutputInfo {
  std::vector<Item> inputs;
  std::vector<Item> outputs;
};

/// A queue item containing an flItemQueue of sub-items.
/// Provides push/pop/iteration over contained items.
/// Used for streaming input data (eg. realtime audio) and output data (e.g. streaming tokens).
class ItemQueue : public Item {
 public:
  /// Construct with a new empty queue.
  ItemQueue();

  /// Push an item into the queue. Transfers ownership — do not use the Item after this call.
  void Push(Item&& item);

  /// Try to pop the front item. Returns nullopt if the queue is empty. Caller takes ownership.
  std::optional<Item> TryPop();

  /// Get the number of items currently in the queue.
  size_t Size();

  /// Mark the queue as finished (no more items will be pushed).
  void MarkFinished();

  /// Check whether the queue has been marked finished.
  bool IsFinished();
};

/// A typed message item.
///
/// Convenience constructor over the C ABI's SetMessage call. Parts are passed
/// to the native layer, which deep-clones them; once the constructor returns
/// the caller's part vector can be safely dropped.
class MessageItem : public Item {
 public:
  /// Build a message with role, plain-text content, and optional participant name.
  /// Wraps `content` in a single TEXT part.
  MessageItem(flMessageRole role, const std::string& content,
              const std::optional<std::string>& name = std::nullopt);

  /// Build a message with role, typed content parts (TEXT / IMAGE / AUDIO),
  /// and optional participant name. The native layer copies each part.
  MessageItem(flMessageRole role, std::vector<Item> parts,
              const std::optional<std::string>& name = std::nullopt);
};

// ===========================================================================
// Convenience message constructors — free functions returning Item
// ===========================================================================

/// Create a system message item.
inline MessageItem SystemMessage(const std::string& content,
                                 const std::optional<std::string>& name = std::nullopt) {
  return MessageItem(FOUNDRY_LOCAL_ROLE_SYSTEM, content, name);
}

/// Create a user message item.
inline MessageItem UserMessage(const std::string& content,
                               const std::optional<std::string>& name = std::nullopt) {
  return MessageItem(FOUNDRY_LOCAL_ROLE_USER, content, name);
}

/// Create an assistant message item.
inline MessageItem AssistantMessage(const std::string& content,
                                    const std::optional<std::string>& name = std::nullopt) {
  return MessageItem(FOUNDRY_LOCAL_ROLE_ASSISTANT, content, name);
}

/// Create a developer message item.
inline MessageItem DeveloperMessage(const std::string& content,
                                    const std::optional<std::string>& name = std::nullopt) {
  return MessageItem(FOUNDRY_LOCAL_ROLE_DEVELOPER, content, name);
}

// ===========================================================================
// IModel interface
// ===========================================================================

/// IModel is an internal abstraction used by the SDK and by tests for dependency injection. It must NOT be
/// implemented by user code: every IModel reference accepted by the SDK is required at runtime to be a concrete
/// `Model` instance, and is downcast accordingly via `detail::AsModel`. Custom subclasses will trigger an
/// exception at the downcast site (FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT).
class IModel {
 public:
  virtual ~IModel() = default;
  virtual ModelInfo GetInfo() const = 0;
  virtual bool IsCached() const = 0;
  virtual bool IsLoaded() const = 0;
  virtual std::string_view GetPath() const = 0;
  virtual InputOutputInfo GetInputOutputInfo() const = 0;
  virtual ModelList GetVariants() const = 0;
  virtual void SelectVariant(const IModel& variant) = 0;
  virtual void Download(std::function<int(float)> progress = nullptr) = 0;
  virtual void Load() = 0;
  virtual void Unload() = 0;
  virtual void RemoveFromCache() = 0;

  /// Identity hook used by `detail::AsModel` to recover the concrete `Model` from an `IModel&` without
  /// requiring RTTI / `dynamic_cast`. The default implementations return nullptr; the concrete `Model`
  /// overrides to return `this`. Custom `IModel` subclasses must not override these — doing so would
  /// defeat the safety check at the downcast site.
  virtual Model* AsConcreteModelHook() noexcept { return nullptr; }
  virtual const Model* AsConcreteModelHook() const noexcept { return nullptr; }
};

// ===========================================================================
// Model — concrete IModel implementation using composition
// ===========================================================================

class Model final : public IModel {
 public:
  /// Mutable construction (from catalog lookups that return flModel*).
  explicit Model(flModel& m) : handle_(&m) {}

  /// Const construction (for non-owning read-only views within ModelList).
  explicit Model(const flModel& m) : handle_(&m) {}

  Model(Model&&) noexcept = default;
  Model& operator=(Model&&) noexcept = default;

  // IModel overrides
  ModelInfo GetInfo() const override;
  bool IsCached() const override;
  bool IsLoaded() const override;
  std::string_view GetPath() const override;
  InputOutputInfo GetInputOutputInfo() const override;
  ModelList GetVariants() const override;
  void SelectVariant(const IModel& variant) override;
  void Download(std::function<int(float)> progress = nullptr) override;
  void Load() override;
  void Unload() override;
  void RemoveFromCache() override;

  // Identity hooks — see IModel::AsConcreteModelHook.
  Model* AsConcreteModelHook() noexcept override { return this; }
  const Model* AsConcreteModelHook() const noexcept override { return this; }

  /// Native handle accessors (not on IModel — keeps interface clean for mocks).
  const flModel* native_handle() const noexcept { return handle_.get(); }
  flModel* native_handle_mutable() { return handle_.get_mutable(); }

 private:
  detail::Base<flModel> handle_;
};

// ===========================================================================
// ModelList
// ===========================================================================

class ModelList {
 public:
  ModelList(flModelList& model_list);

  ModelList(ModelList&&) noexcept = default;
  ModelList& operator=(ModelList&&) noexcept = default;
  ModelList(const ModelList&) = delete;
  ModelList& operator=(const ModelList&) = delete;

  gsl::span<const std::unique_ptr<IModel>> Models() const noexcept;
  size_t size() const noexcept;
  auto begin() const noexcept { return models_.begin(); }
  auto end() const noexcept { return models_.end(); }

 private:
  detail::Base<flModelList> handle_;
  std::vector<std::unique_ptr<IModel>> models_;
};

// ===========================================================================
// ICatalog interface
// ===========================================================================

class ICatalog {
 public:
  virtual ~ICatalog() = default;
  virtual std::string_view GetName() const = 0;
  virtual ModelList GetModels() const = 0;
  virtual ModelList GetCachedModels() const = 0;
  virtual ModelList GetLoadedModels() const = 0;
  virtual std::unique_ptr<IModel> GetModel(const std::string& alias) const = 0;
  virtual std::unique_ptr<IModel> GetModelVariant(const std::string& model_id) const = 0;
  virtual std::unique_ptr<IModel> GetLatestVersion(const IModel& model) const = 0;

  /// Get all versions of a model alias. `model_alias` must be non-empty.
  /// `variant_name` optionally narrows the result to a single variant; empty
  /// returns every variant. `max_versions` selects the latest X versions per
  /// variant name (defaults to 50, matching the web service contract); pass 0
  /// or a negative value for no per-variant cap. Each call performs a fresh
  /// query and the returned model handles remain valid until the next
  /// GetModelVersions call for the same alias or until the catalog is destroyed.
  /// Queries for different aliases do not invalidate each other's results.
  virtual ModelList GetModelVersions(const std::string& model_alias,
                                     const std::string& variant_name = {},
                                     int max_versions = 50) = 0;
};

// ===========================================================================
// Catalog — concrete ICatalog implementation using composition
// ===========================================================================

class Catalog final : public ICatalog {
 public:
  /// Adopt an already-created catalog handle (owning).
  /// Most users should obtain a catalog via Manager::GetCatalog() rather than constructing one directly.
  explicit Catalog(flCatalog& catalog) : handle_(&catalog) {}

  Catalog(Catalog&&) noexcept = default;
  Catalog& operator=(Catalog&&) noexcept = default;

  std::string_view GetName() const override;
  ModelList GetModels() const override;
  ModelList GetCachedModels() const override;
  ModelList GetLoadedModels() const override;
  std::unique_ptr<IModel> GetModel(const std::string& alias) const override;
  std::unique_ptr<IModel> GetModelVariant(const std::string& model_id) const override;
  std::unique_ptr<IModel> GetLatestVersion(const IModel& model) const override;
  ModelList GetModelVersions(const std::string& model_alias,
                             const std::string& variant_name = {},
                             int max_versions = 50) override;

 private:
  detail::Base<flCatalog> handle_;
};

// ===========================================================================
// Manager
// ===========================================================================

/// @brief Main entry point for the Foundry Local SDK. Manages configuration, catalogs, and the embedded web service.
///
/// Construct a single Manager at application startup, then use it to access catalogs and manage the web service.
/// Manager is thread-safe for concurrent use from multiple threads.
/// Shutdown() can be called from any thread.
/// Manager is designed to be long-lived and only one instance is allowed at any point in time.
/// The Manager can be destroyed and recreated if you need to change configuration.
class Manager {
 public:
  explicit Manager(Configuration&& config);
  ~Manager() = default;
  Manager(Manager&&) noexcept = default;
  Manager& operator=(Manager&&) noexcept = default;
  Manager(const Manager&) = delete;
  Manager& operator=(const Manager&) = delete;

  const Configuration& GetConfiguration() const { return config_; }

  /// Get the catalog for querying models. Creates on first call, caches internally.
  ICatalog& GetCatalog() const;

  /// Start the embedded web service.
  void StartWebService();

  /// Stop the embedded web service.
  void StopWebService();

  /// Get the URLs the web service is bound to. Returns an empty vector when the web service is not
  /// running (i.e. before StartWebService() or after StopWebService()); callers can use that as an
  /// "is running" check without a separate API.
  std::vector<std::string> GetWebServiceEndpoints() const;

  /// Get discoverable execution providers and their registration status.
  std::vector<EpInfo> GetDiscoverableEps() const;

  /// Download and register execution providers. Blocking.
  /// @param ep_names  EP names to download. Empty = all discoverable EPs.
  /// @param progress  Optional callback: (ep_name, percent) -> return true to continue, false to cancel.
  void DownloadAndRegisterEps(
      const std::vector<std::string>& ep_names = {},
      std::function<bool(std::string_view ep_name, float percent)> progress = nullptr);

  /// Whether an EP download/registration operation is currently in progress.
  bool IsEpDownloadInProgress() const;

  /// Begin graceful shutdown. Safe to call from any thread. Idempotent.
  void Shutdown();

  /// Check if Shutdown() has been called.
  bool IsShutdownRequested() const;

 private:
  detail::Base<flManager> handle_;
  Configuration config_;
  mutable std::unique_ptr<Catalog> catalog_;
  mutable std::unique_ptr<std::once_flag> catalog_once_{std::make_unique<std::once_flag>()};
};

// ===========================================================================
// ToolDefinition
// ===========================================================================

/// C++ wrapper for flToolDefinition. Sets the version field automatically.
struct ToolDefinition {
  std::string name;         ///< Tool name.
  std::string description;  ///< Tool description for model context.
  std::string json_schema;  ///< JSON schema defining the tool's arguments.

  ToolDefinition(std::string name, std::string description, std::string json_schema)
      : name(std::move(name)), description(std::move(description)), json_schema(std::move(json_schema)) {}

  /// Convert to the C struct for passing across the ABI boundary.
  flToolDefinition ToC() const noexcept {
    return {FOUNDRY_LOCAL_API_VERSION, name.c_str(), description.c_str(), json_schema.c_str()};
  }
};

// ===========================================================================
// RequestOptions — typed options for inference requests
// ===========================================================================

/// Pure sampling / decoder knobs. Each field is std::optional — only set values are forwarded to the C ABI.
struct SearchOptions {
  std::optional<float> temperature;        ///< Sampling temperature [0.0, 2.0].
  std::optional<float> top_p;              ///< Nucleus sampling [0.0, 1.0].
  std::optional<int> top_k;                ///< Top-k sampling.
  std::optional<int> max_output_tokens;    ///< Maximum tokens to generate.
  std::optional<float> frequency_penalty;  ///< Frequency penalty [-2.0, 2.0].
  std::optional<float> presence_penalty;   ///< Presence penalty [-2.0, 2.0].
  std::optional<int> seed;                 ///< Random seed for reproducibility.
  std::optional<bool> early_stopping;      ///< Stop on stop-sequence match.
  std::optional<bool> do_sample;           ///< Whether to sample (false = greedy).
};

/// Options to apply to all requests (when passed to Session::SetOptions) or to
/// override session options for a single request (when passed to Request::SetOptions).
struct RequestOptions {
  SearchOptions search;                     ///< Sampling/decoder parameters.
  std::optional<flToolChoice> tool_choice;  ///< Tool-choice mode for tool-enabled requests.
  KeyValuePairs additional_options;         ///< Passthrough for params not yet typed.
                                            ///< Typed fields take precedence on key collision.
};

// ===========================================================================
// Inference — Request, Response, Session
// ===========================================================================

class Response;  // forward declaration for Session

/// Wrapper for an opaque flRequest.
class Request {
 public:
  Request();

  /// Construct a request from a list of items.
  /// Enables: Request request{SystemMessage("..."), UserMessage("...")};
  template <typename... Items,
            std::enable_if_t<(sizeof...(Items) > 0) &&
                                 (std::is_base_of_v<Item, std::remove_reference_t<Items>> && ...),
                             int> = 0>
  Request(Items&&... items) : Request() {
    (AddItem(std::forward<Items>(items)), ...);
  }

  /// Add an item to the request.
  /// If take_ownership is true (default), the request takes ownership of the item.
  /// If false, the caller must ensure the item and its data remains valid for the request's lifetime.
  Request& AddItem(Item& item, bool take_ownership = true);

  Request& AddItem(Item&& item) { return AddItem(item, true); }

  size_t GetItemCount() const noexcept;
  Item GetItem(size_t idx) const;

  /// Options for this request. Overrides session options for the duration of this request.
  Request& SetOptions(const RequestOptions& options);

  /// Cancel the current request. Inferencing will stop as soon as possible.
  void Cancel();

  const flRequest* native_handle() const noexcept { return handle_.get(); }

 private:
  detail::Base<flRequest> handle_;
};

/// Wrapper for an opaque flResponse.
class Response {
 public:
  /// Get the response items as non-owning read-only views.
  /// Lifetime is tied to this Response object.
  const std::vector<Item>& GetItems() const noexcept { return items_; }

  flFinishReason GetFinishReason() const noexcept;
  flUsage GetUsage() const;

 private:
  friend class Session;
  explicit Response(flResponse* response);
  detail::Base<flResponse> handle_;
  std::vector<Item> items_;
};

namespace detail {
struct StreamingCallbackHelper;
}  // namespace detail

/// Wrapper for an opaque flSession. Created from a loaded IModel.
class Session {
 public:
  explicit Session(IModel& model);

  Session(Session&&) noexcept = default;
  Session& operator=(Session&&) noexcept = default;

  /// Options to apply to all requests on this session.
  Session& SetOptions(const RequestOptions& options);

  /// Set the streaming callback using a std::function.
  /// Return 0 to continue, non-zero to cancel.
  Session& SetStreamingCallback(std::function<int(flStreamingCallbackData)> callback);

  /// Process the request.
  /// Populates the response with output items, finish reason, and usage.
  Response ProcessRequest(const Request& request);

 protected:
  detail::Base<flSession> handle_;

 private:
  std::unique_ptr<detail::StreamingCallbackHelper> streaming_callback_helper_;
};

class ChatSession : public Session {
 public:
  explicit ChatSession(IModel& model);

  /// Add a tool definition that is available for the entire session.
  ChatSession& AddToolDefinition(const ToolDefinition& tool_definition);

  /// Remove a previously-added tool definition by name.
  /// Returns true if a matching tool was found and removed, false if no tool with that name was registered.
  /// Useful when the available tool set changes mid-conversation.
  bool RemoveToolDefinition(std::string_view tool_name);

  /// Get the number of completed turns.
  size_t TurnCount() const;

  /// Undo the last `count` turns: rewinds the generator and removes the turns'
  /// input messages and assistant replies from history.
  void UndoTurns(size_t count);
};

/// Session for automatic-speech-recognition (transcription) models.
///
/// Each request produces a SpeechResultItem with the full transcript and per-segment detail;
/// the streaming callback (when registered) receives a SpeechSegmentItem per decoded token.
class AudioSession : public Session {
 public:
  explicit AudioSession(IModel& model);
};

class EmbeddingsSession : public Session {
 public:
  explicit EmbeddingsSession(IModel& model);

  /// Convenience helper: generate an L2-normalized embedding vector for a single input.
  std::vector<float> Embed(const std::string& input);

  /// Convenience helper: generate L2-normalized embedding vectors for a batch of inputs.
  std::vector<std::vector<float>> Embed(const std::vector<std::string>& inputs);
};

// ===========================================================================
// Callback helpers
// ===========================================================================
namespace detail {

/// Adapts a std::function streaming callback for use with the C API's function-pointer + void* interface.
struct StreamingCallbackHelper {
  using CallbackFn = std::function<int(flStreamingCallbackData)>;

  explicit StreamingCallbackHelper(CallbackFn callback) : callback_(std::move(callback)) {}

  static int CCallback(flStreamingCallbackData data, void* user_data) {
    auto* helper = static_cast<StreamingCallbackHelper*>(user_data);
    return helper->callback_(data);
  }

 private:
  CallbackFn callback_;
};

/// Adapts a std::function deleter for use with C API data struct deleters.
/// Templatized on the data struct type (flBytesData, flTensorData, etc.).
/// Self-deleting: the C deleter calls the user function, then frees the helper.
template <typename TData>
struct DataDeleterHelper {
  using DeleterFn = std::function<void(const TData*)>;

  explicit DataDeleterHelper(DeleterFn fn) : fn_(std::move(fn)) {}

  static void CDeleter(const TData* data, void* user_data) {
    auto* helper = static_cast<DataDeleterHelper*>(user_data);
    if (helper->fn_) {
      helper->fn_(data);
    }

    // The deleter is provided at the same time as the data to the C API and ownership transfers at that point.
    // FL implementation guaranteed to always call the deleter exactly once to release the memory.
    delete helper;
  }

 private:
  DeleterFn fn_;
};

}  // namespace detail

}  // namespace foundry_local

// All method implementations live here to keep this header clean for API review.
#include "foundry_local/foundry_local_cpp.inline.h"
