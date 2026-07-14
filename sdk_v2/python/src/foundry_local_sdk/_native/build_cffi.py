"""cffi out-of-line API mode extension builder.

Run this script directly to (re)compile the cffi extension during development::

    cd sdk_v2/python
    python src/foundry_local_sdk/_native/build_cffi.py

The generated ``_cffi_bindings.abi3.pyd`` (Windows) or
``_cffi_bindings.abi3.so`` (Linux/macOS) will be placed alongside this
file in the ``_native/`` package directory.

The extension is compiled against the real ``foundry_local_c.h`` header so
struct layouts and enum values are verified at build time.  The actual
``foundry_local.dll/.so/.dylib`` is *not* linked at compile time — it is
discovered and loaded at runtime by ``lib_loader.py`` via ``ffi.dlopen()``.
"""

from __future__ import annotations
import os as _os
import sys as _sys

import pathlib

import cffi

# Resolve the include directory: build_cffi.py lives at
#   sdk_v2/python/src/foundry_local_sdk/_native/build_cffi.py
# Five parent steps reach sdk_v2/, then we descend into cpp/include/.
_HERE = pathlib.Path(__file__).resolve()
_SDK_V2_DIR = _HERE.parent.parent.parent.parent.parent  # sdk_v2/
_INCLUDE_DIR = _SDK_V2_DIR / "cpp" / "include"

if not (_INCLUDE_DIR / "foundry_local" / "foundry_local_c.h").exists():
    raise FileNotFoundError(
        f"Could not locate foundry_local_c.h under {_INCLUDE_DIR}. "
        "Run this script from the sdk_v2/python/ directory inside the repo."
    )

ffi = cffi.FFI()

# ---------------------------------------------------------------------------
# cdef — pre-processed for cffi's parser:
#   - SAL annotations stripped (_In_, _Out_, _Outptr_, etc.)
#   - C++ keywords stripped (noexcept, extern "C", __declspec, FL_EXPORT, FL_API_CALL, etc.)
#   - #define macros and #pragma directives omitted
#   - bool → _Bool  (cffi's cdef parser does not support the C99 bool keyword)
#   - Field order MUST match foundry_local_c.h exactly — any mismatch silently
#     corrupts all subsequent function-pointer offsets.
# ---------------------------------------------------------------------------
ffi.cdef(
    """
/* -----------------------------------------------------------------------
 * Enumerations
 * ----------------------------------------------------------------------- */

typedef enum flErrorCode {
    FOUNDRY_LOCAL_OK = 0,
    FOUNDRY_LOCAL_ERROR_NOT_IMPLEMENTED = 1,
    FOUNDRY_LOCAL_ERROR_INTERNAL = 2,
    FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT = 3,
    FOUNDRY_LOCAL_ERROR_INVALID_USAGE = 4,
    FOUNDRY_LOCAL_ERROR_OPERATION_CANCELLED = 5,
    FOUNDRY_LOCAL_ERROR_NETWORK = 6,
} flErrorCode;

typedef enum flLogLevel {
    FOUNDRY_LOCAL_LOG_VERBOSE = 0,
    FOUNDRY_LOCAL_LOG_DEBUG = 1,
    FOUNDRY_LOCAL_LOG_INFO = 2,
    FOUNDRY_LOCAL_LOG_WARNING = 3,
    FOUNDRY_LOCAL_LOG_ERROR = 4,
    FOUNDRY_LOCAL_LOG_FATAL = 5,
} flLogLevel;

typedef enum flDeviceType {
    FOUNDRY_LOCAL_DEVICE_NOTSET = 0,
    FOUNDRY_LOCAL_DEVICE_CPU = 1,
    FOUNDRY_LOCAL_DEVICE_GPU = 2,
    FOUNDRY_LOCAL_DEVICE_NPU = 3,
} flDeviceType;

typedef enum flTensorDataType {
    FOUNDRY_LOCAL_TENSOR_UNDEFINED = 0,
    FOUNDRY_LOCAL_TENSOR_FLOAT = 1,
    FOUNDRY_LOCAL_TENSOR_UINT8 = 2,
    FOUNDRY_LOCAL_TENSOR_INT8 = 3,
    FOUNDRY_LOCAL_TENSOR_UINT16 = 4,
    FOUNDRY_LOCAL_TENSOR_INT16 = 5,
    FOUNDRY_LOCAL_TENSOR_INT32 = 6,
    FOUNDRY_LOCAL_TENSOR_INT64 = 7,
    FOUNDRY_LOCAL_TENSOR_STRING = 8,
    FOUNDRY_LOCAL_TENSOR_BOOL = 9,
    FOUNDRY_LOCAL_TENSOR_FLOAT16 = 10,
    FOUNDRY_LOCAL_TENSOR_DOUBLE = 11,
    FOUNDRY_LOCAL_TENSOR_UINT32 = 12,
    FOUNDRY_LOCAL_TENSOR_UINT64 = 13,
    FOUNDRY_LOCAL_TENSOR_COMPLEX64 = 14,
    FOUNDRY_LOCAL_TENSOR_COMPLEX128 = 15,
    FOUNDRY_LOCAL_TENSOR_BFLOAT16 = 16,
    FOUNDRY_LOCAL_TENSOR_FLOAT8E4M3FN = 17,
    FOUNDRY_LOCAL_TENSOR_FLOAT8E4M3FNUZ = 18,
    FOUNDRY_LOCAL_TENSOR_FLOAT8E5M2 = 19,
    FOUNDRY_LOCAL_TENSOR_FLOAT8E5M2FNUZ = 20,
    FOUNDRY_LOCAL_TENSOR_UINT4 = 21,
    FOUNDRY_LOCAL_TENSOR_INT4 = 22,
    FOUNDRY_LOCAL_TENSOR_FLOAT4E2M1 = 23,
    FOUNDRY_LOCAL_TENSOR_FLOAT8E8M0 = 24,
} flTensorDataType;

typedef enum flItemType {
    FOUNDRY_LOCAL_ITEM_UNKNOWN = 0,
    FOUNDRY_LOCAL_ITEM_BYTES = 1,
    FOUNDRY_LOCAL_ITEM_TENSOR = 10,
    FOUNDRY_LOCAL_ITEM_TEXT = 20,
    FOUNDRY_LOCAL_ITEM_MESSAGE = 21,
    FOUNDRY_LOCAL_ITEM_IMAGE = 25,
    FOUNDRY_LOCAL_ITEM_AUDIO = 30,
    FOUNDRY_LOCAL_ITEM_SPEECH_SEGMENT = 31,
    FOUNDRY_LOCAL_ITEM_SPEECH_RESULT = 32,
    FOUNDRY_LOCAL_ITEM_TOOL_CALL = 100,
    FOUNDRY_LOCAL_ITEM_TOOL_RESULT = 101,
    FOUNDRY_LOCAL_ITEM_QUEUE = 200,
} flItemType;

typedef enum flSpeechSegmentKind {
    FOUNDRY_LOCAL_SPEECH_SEGMENT_NONE = 0,
    FOUNDRY_LOCAL_SPEECH_SEGMENT_PARTIAL = 1,
    FOUNDRY_LOCAL_SPEECH_SEGMENT_FINAL = 2,
} flSpeechSegmentKind;

typedef enum flTextItemType {
    FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT = 0,
    FOUNDRY_LOCAL_TEXT_ITEM_TYPE_REASONING = 1,
    FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON = 2,
} flTextItemType;

typedef enum flMessageRole {
    FOUNDRY_LOCAL_ROLE_NONE = 0,
    FOUNDRY_LOCAL_ROLE_SYSTEM = 1,
    FOUNDRY_LOCAL_ROLE_USER = 2,
    FOUNDRY_LOCAL_ROLE_ASSISTANT = 3,
    FOUNDRY_LOCAL_ROLE_TOOL = 4,
    FOUNDRY_LOCAL_ROLE_DEVELOPER = 5,
} flMessageRole;

typedef enum flFinishReason {
    FOUNDRY_LOCAL_FINISH_NONE = 0,
    FOUNDRY_LOCAL_FINISH_ERROR = 1,
    FOUNDRY_LOCAL_FINISH_STOP = 2,
    FOUNDRY_LOCAL_FINISH_LENGTH = 3,
    FOUNDRY_LOCAL_FINISH_TOOL_CALLS = 4,
} flFinishReason;

/* -----------------------------------------------------------------------
 * Opaque handle forward declarations.
 * These are the "object" types managed by the library.
 * ----------------------------------------------------------------------- */

typedef struct flApi flApi;
typedef struct flCatalog flCatalog;
typedef struct flConfiguration flConfiguration;
typedef struct flItem flItem;
typedef struct flItemQueue flItemQueue;
typedef struct flKeyValuePairs flKeyValuePairs;
typedef struct flManager flManager;
typedef struct flModel flModel;
typedef struct flModelInfo flModelInfo;
typedef struct flModelList flModelList;
typedef struct flRequest flRequest;
typedef struct flResponse flResponse;
typedef struct flSession flSession;
typedef struct flStatus flStatus;

/* Forward declarations for the sub-API vtable structs. */
typedef struct flCatalogApi flCatalogApi;
typedef struct flConfigurationApi flConfigurationApi;
typedef struct flItemApi flItemApi;
typedef struct flInferenceApi flInferenceApi;
typedef struct flModelApi flModelApi;

/* -----------------------------------------------------------------------
 * Token usage
 * ----------------------------------------------------------------------- */

typedef struct flUsage {
    uint32_t version;
    int64_t prompt_tokens;
    int64_t completion_tokens;
    int64_t total_tokens;
} flUsage;

typedef struct flEpInfo {
    uint32_t version;
    const char* name;
    _Bool is_registered;
} flEpInfo;

/* -----------------------------------------------------------------------
 * Versioned data structs
 * ----------------------------------------------------------------------- */

/* Forward declarations needed for the deleter typedefs below. */
typedef struct flBytesData flBytesData;
typedef struct flTensorData flTensorData;
typedef struct flImageData flImageData;
typedef struct flAudioData flAudioData;

/* Per-type deleter callbacks — called when the item is destroyed. */
typedef void (*flBytesDataDeleter)(const flBytesData* data, void* user_data);
typedef void (*flTensorDataDeleter)(const flTensorData* data, void* user_data);
typedef void (*flImageDataDeleter)(const flImageData* data, void* user_data);
typedef void (*flAudioDataDeleter)(const flAudioData* data, void* user_data);

typedef struct flTextData {
    uint32_t version;
    const char* text;
    flTextItemType type;
} flTextData;

struct flBytesData {
    uint32_t version;
    flItemType item_type;
    const void* data;
    void* mutable_data;
    size_t data_size;
    flBytesDataDeleter deleter;
    void* deleter_user_data;
};

struct flTensorData {
    uint32_t version;
    flTensorDataType data_type;
    const void* data;
    void* mutable_data;
    const int64_t* shape;
    size_t rank;
    flTensorDataDeleter deleter;
    void* deleter_user_data;
};

typedef struct flMessageData {
    uint32_t version;
    flMessageRole role;
    const flItem* const* content_items;
    size_t content_items_count;
    const char* name;
} flMessageData;

struct flImageData {
    uint32_t version;
    const void* data;
    void* mutable_data;
    size_t data_size;
    const char* format;
    const char* uri;
    flImageDataDeleter deleter;
    void* deleter_user_data;
};

struct flAudioData {
    uint32_t version;
    const void* data;
    void* mutable_data;
    size_t data_size;
    const char* format;
    const char* uri;
    int sample_rate;
    int channels;
    flAudioDataDeleter deleter;
    void* deleter_user_data;
};

typedef struct flToolCallData {
    uint32_t version;
    const char* call_id;
    const char* name;
    const char* arguments;
} flToolCallData;

typedef struct flToolResultData {
    uint32_t version;
    const char* call_id;
    const char* result;
} flToolResultData;

/* Speech recognition output types. SPEECH_SEGMENT / SPEECH_RESULT are output-only;
   the ABI exposes only Get accessors. Time fields use FOUNDRY_LOCAL_DURATION_UNSET
   (INT64_MIN, defined Python-side as _DURATION_UNSET) when absent. */

typedef struct flSpeechWord {
    uint32_t version;
    const char* text;
    int64_t start_time_ms;
    int64_t end_time_ms;
    float confidence;
    const char* speaker_id;
} flSpeechWord;

typedef struct flSpeechSegmentData {
    uint32_t version;
    flSpeechSegmentKind kind;
    const char* text;
    int64_t start_time_ms;
    int64_t end_time_ms;
    _Bool utterance_start;
    const flSpeechWord* words;
    size_t words_count;
    const char* language;
} flSpeechSegmentData;

typedef struct flSpeechResultData {
    uint32_t version;
    const char* text;
    const char* language;
    int64_t duration_ms;
    const flItem* const* segments;
    size_t segments_count;
} flSpeechResultData;

typedef struct flStreamingCallbackData {
    uint32_t version;
    flItemQueue* item_queue;
} flStreamingCallbackData;

typedef struct flToolDefinition {
    uint32_t version;
    const char* name;
    const char* description;
    const char* json_schema;
} flToolDefinition;

/* -----------------------------------------------------------------------
 * Callback types
 * ----------------------------------------------------------------------- */

typedef int (*flProgressCallback)(float value, void* user_data);
typedef int (*flStreamingCallback)(flStreamingCallbackData event, void* user_data);
typedef int (*flEpProgressCallback)(const char* ep_name, float value, void* user_data);

/* -----------------------------------------------------------------------
 * Status pointer typedef
 * ----------------------------------------------------------------------- */

typedef flStatus* flStatusPtr;

/* -----------------------------------------------------------------------
 * Root API vtable
 * ----------------------------------------------------------------------- */

typedef struct flApi {
    /* Status */
    flStatusPtr (*Status_Create)(flErrorCode error_code, const char* error_msg);
    void (*Status_Release)(flStatus* instance);
    flErrorCode (*Status_GetErrorCode)(const flStatus* status);
    const char* (*Status_GetErrorMessage)(const flStatus* status);

    /* Manager lifecycle */
    flStatusPtr (*Manager_Create)(const flConfiguration* config, flManager** out_manager);
    void (*Manager_Release)(flManager* instance);

    flStatusPtr (*Manager_GetCatalog)(const flManager* manager, flCatalog** out_catalog);
    flStatusPtr (*Manager_WebServiceStart)(flManager* manager);
    flStatusPtr (*Manager_WebServiceUrls)(const flManager* manager, const char* const** out_urls, size_t* out_num_urls);
    flStatusPtr (*Manager_WebServiceStop)(flManager* manager);

    /* Sub-API accessors — returned pointers are valid for the lifetime of the library. */
    const flCatalogApi* (*GetCatalogApi)(void);
    const flConfigurationApi* (*GetConfigurationApi)(void);
    const flItemApi* (*GetItemApi)(void);
    const flInferenceApi* (*GetInferenceApi)(void);
    const flModelApi* (*GetModelApi)(void);

    /* KeyValuePairs */
    void (*CreateKeyValuePairs)(flKeyValuePairs** out);
    void (*AddKeyValuePair)(flKeyValuePairs* kvps, const char* key, const char* value);
    const char* (*GetKeyValue)(const flKeyValuePairs* kvps, const char* key);
    void (*GetKeyValuePairs)(const flKeyValuePairs* kvps, const char* const** keys, const char* const** values, size_t* num_entries);
    void (*RemoveKeyValuePair)(flKeyValuePairs* kvps, const char* key);
    void (*KeyValuePairs_Release)(flKeyValuePairs* instance);

    /* ModelList */
    void (*ModelList_Release)(flModelList* instance);
    size_t (*ModelList_Size)(const flModelList* models);
    flModel* (*ModelList_GetAt)(const flModelList* models, size_t idx);

    /* EP detection */
    flStatusPtr (*Manager_GetDiscoverableEps)(const flManager* manager, const flEpInfo** out_eps, size_t* out_count);
    flStatusPtr (*Manager_DownloadAndRegisterEps)(flManager* manager, const char* const* ep_names, size_t num_ep_names, flEpProgressCallback callback, void* user_data);
    _Bool (*Manager_IsEpDownloadInProgress)(const flManager* manager);
    flStatusPtr (*Manager_Shutdown)(flManager* manager);
    _Bool (*Manager_IsShutdownRequested)(const flManager* manager);
} flApi;

/* -----------------------------------------------------------------------
 * Item API vtable
 * ----------------------------------------------------------------------- */

typedef struct flItemApi {
    flStatusPtr (*Create)(flItemType type, flItem** out_item);
    void (*Item_Release)(flItem* instance);
    flItemType (*GetType)(const flItem* item);
    flStatusPtr (*SetBytes)(flItem* item, const flBytesData* bytes);
    flStatusPtr (*SetTensor)(flItem* item, const flTensorData* tensor);
    flStatusPtr (*SetText)(flItem* item, const flTextData* text_data);
    flStatusPtr (*SetMessage)(flItem* item, const flMessageData* message);
    flStatusPtr (*SetImage)(flItem* item, const flImageData* image);
    flStatusPtr (*SetAudio)(flItem* item, const flAudioData* audio);
    flStatusPtr (*SetToolCall)(flItem* item, const flToolCallData* tool_call);
    flStatusPtr (*SetToolResult)(flItem* item, const flToolResultData* tool_result);
    flStatusPtr (*GetBytes)(const flItem* item, flBytesData* out_bytes);
    flStatusPtr (*GetText)(const flItem* item, flTextData* out_text_data);
    flStatusPtr (*GetMessage)(const flItem* item, flMessageData* out_message);
    flStatusPtr (*GetTensor)(const flItem* item, flTensorData* out_tensor);
    flStatusPtr (*GetImage)(const flItem* item, flImageData* out_image);
    flStatusPtr (*GetAudio)(const flItem* item, flAudioData* out_audio);
    flStatusPtr (*GetToolCall)(const flItem* item, flToolCallData* out_tool_call);
    flStatusPtr (*GetToolResult)(const flItem* item, flToolResultData* out_tool_result);
    flStatusPtr (*GetSpeechSegment)(const flItem* item, flSpeechSegmentData* out_segment);
    flStatusPtr (*GetSpeechResult)(const flItem* item, flSpeechResultData* out_result);
    flStatusPtr (*GetMetadata)(const flItem* item, const flKeyValuePairs** out_metadata);
    flStatusPtr (*GetMutableMetadata)(flItem* item, flKeyValuePairs** out_metadata);
    flStatusPtr (*GetQueue)(flItem* item, flItemQueue** out_queue);
    flStatusPtr (*ItemQueue_Create)(flItemQueue** out_queue);
    void (*ItemQueue_Release)(flItemQueue* instance);
    flStatusPtr (*ItemQueue_Push)(flItemQueue* queue, flItem* item);
    _Bool (*ItemQueue_TryPop)(flItemQueue* queue, flItem** out_item);
    size_t (*ItemQueue_Size)(const flItemQueue* queue);
    void (*ItemQueue_MarkFinished)(flItemQueue* queue);
    _Bool (*ItemQueue_IsFinished)(const flItemQueue* queue);
} flItemApi;

/* -----------------------------------------------------------------------
 * Inference API vtable
 * ----------------------------------------------------------------------- */

typedef struct flInferenceApi {
    flStatusPtr (*Request_Create)(flRequest** out_request);
    void (*Request_Release)(flRequest* instance);
    flStatusPtr (*Request_AddItem)(flRequest* request, flItem* item, _Bool take_ownership);
    size_t (*Request_GetItemCount)(const flRequest* request);
    flStatusPtr (*Request_GetItem)(const flRequest* request, size_t idx, const flItem** out_item);
    flStatusPtr (*Request_SetOptions)(flRequest* request, const flKeyValuePairs* options);
    flStatusPtr (*Request_Cancel)(flRequest* request);
    flStatusPtr (*Response_Create)(flResponse** out_response);
    void (*Response_Release)(flResponse* instance);
    size_t (*Response_GetItemCount)(const flResponse* response);
    flStatusPtr (*Response_GetItem)(const flResponse* response, size_t idx, const flItem** out_item);
    flFinishReason (*Response_GetFinishReason)(const flResponse* response);
    flStatusPtr (*Response_GetUsage)(const flResponse* response, flUsage* out_usage);
    flStatusPtr (*Session_Create)(const flModel* model, flSession** out_session);
    void (*Session_Release)(flSession* instance);
    flStatusPtr (*Session_SetStreamingCallback)(flSession* session, flStreamingCallback callback, void* user_data);
    flStatusPtr (*Session_SetOptions)(flSession* session, const flKeyValuePairs* options);
    flStatusPtr (*Session_ProcessRequest)(flSession* session, const flRequest* request, flResponse** response);
    flStatusPtr (*Session_AddToolDefinition)(flSession* session, const flToolDefinition* tool_def);
    flStatusPtr (*Session_RemoveToolDefinition)(flSession* session, const char* tool_name, bool* out_removed);
    size_t (*Session_GetTurnCount)(const flSession* session);
    flStatusPtr (*Session_UndoTurns)(flSession* session, size_t count);
} flInferenceApi;

/* -----------------------------------------------------------------------
 * Configuration API vtable
 * ----------------------------------------------------------------------- */

typedef struct flConfigurationApi {
    flStatusPtr (*Create)(const char* app_name, flConfiguration** out_config);
    void (*Configuration_Release)(flConfiguration* instance);
    flStatusPtr (*SetDefaultLogLevel)(flConfiguration* config, flLogLevel level);
    flStatusPtr (*SetAppDataDir)(flConfiguration* config, const char* dir);
    flStatusPtr (*SetLogsDir)(flConfiguration* config, const char* dir);
    flStatusPtr (*SetModelCacheDir)(flConfiguration* config, const char* dir);
    flStatusPtr (*AddCatalogUrl)(flConfiguration* config, const char* url, const char* filter_override);
    flStatusPtr (*SetCatalogRegion)(flConfiguration* config, const char* region);
    flStatusPtr (*AddWebServiceEndpoint)(flConfiguration* config, const char* url);
    flStatusPtr (*SetExternalServiceUrl)(flConfiguration* config, const char* url);
    flStatusPtr (*SetAdditionalOptions)(flConfiguration* config, const flKeyValuePairs* options);
} flConfigurationApi;

/* -----------------------------------------------------------------------
 * Catalog API vtable
 * ----------------------------------------------------------------------- */

typedef struct flCatalogApi {
    flStatusPtr (*GetName)(const flCatalog* catalog, const char** out_name);
    flStatusPtr (*GetModels)(const flCatalog* catalog, flModelList** out_models);
    flStatusPtr (*GetModel)(const flCatalog* catalog, const char* alias, flModel** out_model);
    flStatusPtr (*GetModelVariant)(const flCatalog* catalog, const char* model_id, flModel** out_model);
    flStatusPtr (*GetLatestVersion)(const flCatalog* catalog, const flModel* model, flModel** out_model);
    flStatusPtr (*GetCachedModels)(const flCatalog* catalog, flModelList** out_models);
    flStatusPtr (*GetLoadedModels)(const flCatalog* catalog, flModelList** out_models);
    flStatusPtr (*GetModelVersions)(const flCatalog* catalog, const char* model_alias, const char* model_name, int32_t max_versions, flModelList** out_models);
} flCatalogApi;

/* -----------------------------------------------------------------------
 * Model API vtable
 * ----------------------------------------------------------------------- */

typedef struct flModelApi {
    flStatusPtr (*GetInfo)(const flModel* model, const flModelInfo** out_info);
    flStatusPtr (*GetInputOutputInfo)(const flModel* model, const flItem* const** out_inputs, size_t* out_num_inputs, const flItem* const** out_outputs, size_t* out_num_outputs);
    flStatusPtr (*IsCached)(const flModel* model, int* out_cached);
    flStatusPtr (*GetPath)(const flModel* model, const char** out_path);
    flStatusPtr (*Download)(flModel* model, flProgressCallback callback, void* user_data);
    flStatusPtr (*IsLoaded)(const flModel* model, int* out_loaded);
    flStatusPtr (*Load)(flModel* model);
    flStatusPtr (*Unload)(flModel* model);
    flStatusPtr (*RemoveFromCache)(flModel* model);
    flStatusPtr (*GetVariants)(const flModel* model, flModelList** out_variants);
    flStatusPtr (*SelectVariant)(flModel* model, const flModel* variant);
    const char* (*Info_GetId)(const flModelInfo* info);
    const char* (*Info_GetName)(const flModelInfo* info);
    int (*Info_GetVersion)(const flModelInfo* info);
    const char* (*Info_GetAlias)(const flModelInfo* info);
    const char* (*Info_GetUri)(const flModelInfo* info);
    flDeviceType (*Info_GetDeviceType)(const flModelInfo* info);
    const char* (*Info_GetExecutionProvider)(const flModelInfo* info);
    const char* (*Info_GetTask)(const flModelInfo* info);
    const flKeyValuePairs* (*Info_GetPromptTemplates)(const flModelInfo* info);
    const flKeyValuePairs* (*Info_GetModelSettings)(const flModelInfo* info);
    const char* (*Info_GetStringProperty)(const flModelInfo* info, const char* key);
    int64_t (*Info_GetIntProperty)(const flModelInfo* info, const char* key, int64_t default_value);
} flModelApi;

/* -----------------------------------------------------------------------
 * Exported entry points
 * ----------------------------------------------------------------------- */

const flApi* FoundryLocalGetApi(uint32_t version);
const char* FoundryLocalGetVersionString(void);
"""
)


# Locate the import library (.lib on Windows, .so on Linux/macOS) for the
# build-time linker step.  On Windows the import library lives in a sibling
# directory to the DLL:
#   DLL:  sdk_v2/cpp/build/Windows/RelWithDebInfo/bin/RelWithDebInfo/
#   .lib: sdk_v2/cpp/build/Windows/RelWithDebInfo/RelWithDebInfo/
_dev_library_dirs: list[str] = []
_dev_libraries: list[str] = []

if _sys.platform == "win32":
    # Search order:
    #   1. The wheel-build staging dir alongside this file
    #      (sdk_v2/python/src/foundry_local_sdk/_native/<rid>/foundry_local.lib).
    #      The CI pipeline stages foundry_local.lib here; this is the path that
    #      matters for shipped wheels.
    #   2. The local C++ dev build output
    #      (sdk_v2/cpp/build/Windows/<Config>/<Config>/foundry_local.lib).
    #      Used for inner-loop dev when invoking build_cffi.py directly.
    _lib_candidates = [
        _HERE.parent / "win-x64" / "foundry_local.lib",
        _HERE.parent / "win-arm64" / "foundry_local.lib",
        _SDK_V2_DIR / "cpp" / "build" / "Windows" / "RelWithDebInfo" / "RelWithDebInfo" / "foundry_local.lib",
        _SDK_V2_DIR / "cpp" / "build" / "Windows" / "Debug" / "Debug" / "foundry_local.lib",
    ]
    for _lib_candidate in _lib_candidates:
        if _lib_candidate.exists():
            _dev_library_dirs = [str(_lib_candidate.parent)]
            _dev_libraries = ["foundry_local"]
            break
elif _sys.platform == "darwin":
    # macOS: link against libfoundry_local.dylib. Same staging convention as
    # Windows — CI stages the dylib into _native/<rid>/, dev builds land under
    # sdk_v2/cpp/build/macOS/<Config>/.
    _lib_candidates = [
        _HERE.parent / "osx-arm64" / "libfoundry_local.dylib",
        _HERE.parent / "osx-x64" / "libfoundry_local.dylib",
        _SDK_V2_DIR / "cpp" / "build" / "macOS" / "RelWithDebInfo" / "libfoundry_local.dylib",
        _SDK_V2_DIR / "cpp" / "build" / "macOS" / "Debug" / "libfoundry_local.dylib",
    ]
    for _lib_candidate in _lib_candidates:
        if _lib_candidate.exists():
            _dev_library_dirs = [str(_lib_candidate.parent)]
            _dev_libraries = ["foundry_local"]
            break
else:
    # Linux (and any other ELF platform): link against libfoundry_local.so.
    _lib_candidates = [
        _HERE.parent / "linux-x64" / "libfoundry_local.so",
        _HERE.parent / "linux-arm64" / "libfoundry_local.so",
        _SDK_V2_DIR / "cpp" / "build" / "Linux" / "RelWithDebInfo" / "libfoundry_local.so",
        _SDK_V2_DIR / "cpp" / "build" / "Linux" / "Debug" / "libfoundry_local.so",
    ]
    for _lib_candidate in _lib_candidates:
        if _lib_candidate.exists():
            _dev_library_dirs = [str(_lib_candidate.parent)]
            _dev_libraries = ["foundry_local"]
            break

# Cross-compile knobs (Windows ARM64 from x64 host).
#
# When cross-compiling, setuptools' MSVCCompiler auto-injects the *running*
# (host-arch) interpreter's lib/include dirs AHEAD of anything we pass via the
# LIB / INCLUDE env vars. That means a target-arch python import lib placed on
# LIB still loses to the host-arch python312.lib and the linker emits
# "unresolved external __imp_PyImport_ImportModule" etc.
#
# Entries passed via set_source's library_dirs= and include_dirs= land at the
# FRONT of /LIBPATH and /I respectively, so we use these env-var hooks to get
# target-arch python's headers and import lib first in the search order.
#
# Set in CI by .pipelines/sdk_v2/templates/steps-build-python.yml when
# targetArch=arm64; harmless when unset.


def _split_paths(env_value: str | None) -> list[str]:
    if not env_value:
        return []
    return [p for p in env_value.split(_os.pathsep) if p]


_extra_include_dirs = _split_paths(_os.environ.get("FL_PYTHON_EXTRA_INCLUDE_DIRS"))
_extra_library_dirs = _split_paths(_os.environ.get("FL_PYTHON_EXTRA_LIBRARY_DIRS"))

# Force-include <string.h> via a compiler flag rather than the source preamble:
# cffi emits its own scaffolding (including a memset() call for zero-init)
# AHEAD of the user-supplied #include block, so adding `#include <string.h>`
# to set_source's preamble is too late.  macOS clang treats implicit function
# declarations as a hard error, so we inject the header at the compiler level.
#   - clang/gcc: -include <header>
#   - MSVC:      /FI <header>
if _sys.platform == "win32":
    _force_include_args = ["/FIstring.h"]
else:
    _force_include_args = ["-include", "string.h"]

ffi.set_source(
    "foundry_local_sdk._native._cffi_bindings",
    # <stdbool.h> must come first: foundry_local_c.h uses `bool` but only
    # includes <stddef.h> and <stdint.h>.  In C++ `bool` is a built-in type,
    # but the cffi-generated wrapper is compiled as C, so we must define it.
    # Include the real header so the compiler verifies struct layouts and enum
    # values against the actual declarations at build time.
    "#include <stdbool.h>\n"
    r'#include "foundry_local/foundry_local_c.h"',
    include_dirs=_extra_include_dirs + [str(_INCLUDE_DIR)],
    libraries=_dev_libraries,
    library_dirs=_extra_library_dirs + _dev_library_dirs,
    extra_compile_args=_force_include_args,
    # Build against Python's stable ABI (PEP 384) targeting 3.11 so a single
    # compiled extension works on every CPython >= 3.11. Combined with the
    # bdist_wheel option in setup.py this produces a `cp311-abi3-<plat>` wheel
    # instead of one wheel per (Python minor x platform).
    py_limited_api=True,
    define_macros=[("Py_LIMITED_API", "0x030B0000")],
)

if __name__ == "__main__":
    # cffi derives the output subdirectory from the dotted module name
    # ("foundry_local_sdk/_native/_cffi_bindings"), so tmpdir must point to the
    # package source root (src/) for the .pyd to land at:
    #   src/foundry_local_sdk/_native/_cffi_bindings.abi3.pyd  (Windows)
    #   src/foundry_local_sdk/_native/_cffi_bindings.abi3.so   (Linux/macOS)
    _src_dir = str(_HERE.parent.parent.parent)  # sdk_v2/python/src/
    ffi.compile(tmpdir=_src_dir, verbose=True)
