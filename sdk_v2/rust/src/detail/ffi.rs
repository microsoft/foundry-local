//! Raw FFI declarations mirroring `sdk_v2/cpp/include/foundry_local/foundry_local_c.h`.
//!
//! This module is a faithful `repr(C)` transcription of the Foundry Local C ABI.
//! It exposes the opaque handle types, versioned data structs, enums, callback
//! signatures, and the six function-pointer tables that the library returns via
//! [`FoundryLocalGetApi`].
//!
//! Everything here is `unsafe` to use; safe wrappers live in [`super::api`] and the
//! higher-level detail modules.
#![allow(non_snake_case)]
#![allow(non_camel_case_types)]
#![allow(dead_code)]

use core::ffi::c_void;
use std::os::raw::{c_char, c_int};

/// The library is built against this API version (`FOUNDRY_LOCAL_API_VERSION`).
pub const FOUNDRY_LOCAL_API_VERSION: u32 = 1;

// ── Opaque handle types ──────────────────────────────────────────────────────

macro_rules! opaque_type {
    ($name:ident) => {
        #[repr(C)]
        pub struct $name {
            _data: [u8; 0],
            _marker: core::marker::PhantomData<(*mut u8, core::marker::PhantomPinned)>,
        }
    };
}

opaque_type!(flCatalog);
opaque_type!(flConfiguration);
opaque_type!(flItem);
opaque_type!(flItemQueue);
opaque_type!(flKeyValuePairs);
opaque_type!(flManager);
opaque_type!(flModel);
opaque_type!(flModelInfo);
opaque_type!(flModelList);
opaque_type!(flRequest);
opaque_type!(flResponse);
opaque_type!(flSession);
opaque_type!(flStatus);

/// A non-null `flStatus*` indicates an error; `null` indicates success.
pub type flStatusPtr = *mut flStatus;

// ── Enums (C enums are `int`-sized) ──────────────────────────────────────────

pub type flErrorCode = c_int;
pub const FOUNDRY_LOCAL_OK: flErrorCode = 0;
pub const FOUNDRY_LOCAL_ERROR_NOT_IMPLEMENTED: flErrorCode = 1;
pub const FOUNDRY_LOCAL_ERROR_INTERNAL: flErrorCode = 2;
pub const FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT: flErrorCode = 3;
pub const FOUNDRY_LOCAL_ERROR_INVALID_USAGE: flErrorCode = 4;
pub const FOUNDRY_LOCAL_ERROR_OPERATION_CANCELLED: flErrorCode = 5;
pub const FOUNDRY_LOCAL_ERROR_NETWORK: flErrorCode = 6;

pub type flLogLevel = c_int;
pub const FOUNDRY_LOCAL_LOG_VERBOSE: flLogLevel = 0;
pub const FOUNDRY_LOCAL_LOG_DEBUG: flLogLevel = 1;
pub const FOUNDRY_LOCAL_LOG_INFO: flLogLevel = 2;
pub const FOUNDRY_LOCAL_LOG_WARNING: flLogLevel = 3;
pub const FOUNDRY_LOCAL_LOG_ERROR: flLogLevel = 4;
pub const FOUNDRY_LOCAL_LOG_FATAL: flLogLevel = 5;

pub type flDeviceType = c_int;
pub const FOUNDRY_LOCAL_DEVICE_NOTSET: flDeviceType = 0;
pub const FOUNDRY_LOCAL_DEVICE_CPU: flDeviceType = 1;
pub const FOUNDRY_LOCAL_DEVICE_GPU: flDeviceType = 2;
pub const FOUNDRY_LOCAL_DEVICE_NPU: flDeviceType = 3;

pub type flTensorDataType = c_int;
pub const FOUNDRY_LOCAL_TENSOR_UNDEFINED: flTensorDataType = 0;
pub const FOUNDRY_LOCAL_TENSOR_FLOAT: flTensorDataType = 1;
pub const FOUNDRY_LOCAL_TENSOR_UINT8: flTensorDataType = 2;
pub const FOUNDRY_LOCAL_TENSOR_INT8: flTensorDataType = 3;
pub const FOUNDRY_LOCAL_TENSOR_UINT16: flTensorDataType = 4;
pub const FOUNDRY_LOCAL_TENSOR_INT16: flTensorDataType = 5;
pub const FOUNDRY_LOCAL_TENSOR_INT32: flTensorDataType = 6;
pub const FOUNDRY_LOCAL_TENSOR_INT64: flTensorDataType = 7;
pub const FOUNDRY_LOCAL_TENSOR_STRING: flTensorDataType = 8;
pub const FOUNDRY_LOCAL_TENSOR_BOOL: flTensorDataType = 9;
pub const FOUNDRY_LOCAL_TENSOR_FLOAT16: flTensorDataType = 10;
pub const FOUNDRY_LOCAL_TENSOR_DOUBLE: flTensorDataType = 11;
pub const FOUNDRY_LOCAL_TENSOR_UINT32: flTensorDataType = 12;
pub const FOUNDRY_LOCAL_TENSOR_UINT64: flTensorDataType = 13;
pub const FOUNDRY_LOCAL_TENSOR_COMPLEX64: flTensorDataType = 14;
pub const FOUNDRY_LOCAL_TENSOR_COMPLEX128: flTensorDataType = 15;
pub const FOUNDRY_LOCAL_TENSOR_BFLOAT16: flTensorDataType = 16;
pub const FOUNDRY_LOCAL_TENSOR_FLOAT8E4M3FN: flTensorDataType = 17;
pub const FOUNDRY_LOCAL_TENSOR_FLOAT8E4M3FNUZ: flTensorDataType = 18;
pub const FOUNDRY_LOCAL_TENSOR_FLOAT8E5M2: flTensorDataType = 19;
pub const FOUNDRY_LOCAL_TENSOR_FLOAT8E5M2FNUZ: flTensorDataType = 20;
pub const FOUNDRY_LOCAL_TENSOR_UINT4: flTensorDataType = 21;
pub const FOUNDRY_LOCAL_TENSOR_INT4: flTensorDataType = 22;
pub const FOUNDRY_LOCAL_TENSOR_FLOAT4E2M1: flTensorDataType = 23;
pub const FOUNDRY_LOCAL_TENSOR_FLOAT8E8M0: flTensorDataType = 24;

pub type flItemType = c_int;
pub const FOUNDRY_LOCAL_ITEM_UNKNOWN: flItemType = 0;
pub const FOUNDRY_LOCAL_ITEM_BYTES: flItemType = 1;
pub const FOUNDRY_LOCAL_ITEM_TENSOR: flItemType = 10;
pub const FOUNDRY_LOCAL_ITEM_TEXT: flItemType = 20;
pub const FOUNDRY_LOCAL_ITEM_MESSAGE: flItemType = 21;
pub const FOUNDRY_LOCAL_ITEM_IMAGE: flItemType = 25;
pub const FOUNDRY_LOCAL_ITEM_AUDIO: flItemType = 30;
pub const FOUNDRY_LOCAL_ITEM_SPEECH_SEGMENT: flItemType = 31;
pub const FOUNDRY_LOCAL_ITEM_SPEECH_RESULT: flItemType = 32;
pub const FOUNDRY_LOCAL_ITEM_TOOL_CALL: flItemType = 100;
pub const FOUNDRY_LOCAL_ITEM_TOOL_RESULT: flItemType = 101;
pub const FOUNDRY_LOCAL_ITEM_QUEUE: flItemType = 200;

pub type flTextItemType = c_int;
pub const FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT: flTextItemType = 0;
pub const FOUNDRY_LOCAL_TEXT_ITEM_TYPE_REASONING: flTextItemType = 1;
pub const FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON: flTextItemType = 2;

pub type flMessageRole = c_int;
pub const FOUNDRY_LOCAL_ROLE_NONE: flMessageRole = 0;
pub const FOUNDRY_LOCAL_ROLE_SYSTEM: flMessageRole = 1;
pub const FOUNDRY_LOCAL_ROLE_USER: flMessageRole = 2;
pub const FOUNDRY_LOCAL_ROLE_ASSISTANT: flMessageRole = 3;
pub const FOUNDRY_LOCAL_ROLE_TOOL: flMessageRole = 4;
pub const FOUNDRY_LOCAL_ROLE_DEVELOPER: flMessageRole = 5;

pub type flFinishReason = c_int;
pub const FOUNDRY_LOCAL_FINISH_NONE: flFinishReason = 0;
pub const FOUNDRY_LOCAL_FINISH_ERROR: flFinishReason = 1;
pub const FOUNDRY_LOCAL_FINISH_STOP: flFinishReason = 2;
pub const FOUNDRY_LOCAL_FINISH_LENGTH: flFinishReason = 3;
pub const FOUNDRY_LOCAL_FINISH_TOOL_CALLS: flFinishReason = 4;

pub type flToolChoice = c_int;
pub const FOUNDRY_LOCAL_TOOL_CHOICE_AUTO: flToolChoice = 0;
pub const FOUNDRY_LOCAL_TOOL_CHOICE_NONE: flToolChoice = 1;
pub const FOUNDRY_LOCAL_TOOL_CHOICE_REQUIRED: flToolChoice = 2;

// ── Well-known property / parameter keys ─────────────────────────────────────

pub const FOUNDRY_LOCAL_MODEL_PROP_DISPLAY_NAME_STR: &str = "display_name";
pub const FOUNDRY_LOCAL_MODEL_PROP_MODEL_TYPE_STR: &str = "type";
pub const FOUNDRY_LOCAL_MODEL_PROP_PUBLISHER_STR: &str = "publisher";
pub const FOUNDRY_LOCAL_MODEL_PROP_LICENSE_STR: &str = "license";
pub const FOUNDRY_LOCAL_MODEL_PROP_LICENSE_DESCRIPTION_STR: &str = "license_description";
pub const FOUNDRY_LOCAL_MODEL_PROP_TASK_STR: &str = "task";
pub const FOUNDRY_LOCAL_MODEL_PROP_MODEL_PROVIDER_STR: &str = "model_provider";
pub const FOUNDRY_LOCAL_MODEL_PROP_MIN_FL_VERSION_STR: &str = "min_fl_version";
pub const FOUNDRY_LOCAL_MODEL_PROP_INPUT_MODALITIES_STR: &str = "input_modalities";
pub const FOUNDRY_LOCAL_MODEL_PROP_OUTPUT_MODALITIES_STR: &str = "output_modalities";
pub const FOUNDRY_LOCAL_MODEL_PROP_CAPABILITIES_STR: &str = "capabilities";

pub const FOUNDRY_LOCAL_MODEL_PROP_SUPPORTS_TOOL_CALLING_INT: &str = "supports_tool_calling";
pub const FOUNDRY_LOCAL_MODEL_PROP_SUPPORTS_REASONING_INT: &str = "supports_reasoning";
pub const FOUNDRY_LOCAL_MODEL_PROP_FILESIZE_MB_INT: &str = "filesize_mb";
pub const FOUNDRY_LOCAL_MODEL_PROP_MAX_OUTPUT_TOKENS_INT: &str = "max_output_tokens";
pub const FOUNDRY_LOCAL_MODEL_PROP_CREATED_AT_UNIX_INT: &str = "created_at_unix";
pub const FOUNDRY_LOCAL_MODEL_PROP_CONTEXT_LENGTH_INT: &str = "context_length";

pub const FOUNDRY_LOCAL_PARAM_TEMPERATURE: &str = "temperature";
pub const FOUNDRY_LOCAL_PARAM_TOP_P: &str = "top_p";
pub const FOUNDRY_LOCAL_PARAM_TOP_K: &str = "top_k";
pub const FOUNDRY_LOCAL_PARAM_MAX_OUTPUT_TOKENS: &str = "max_output_tokens";
pub const FOUNDRY_LOCAL_PARAM_FREQUENCY_PENALTY: &str = "frequency_penalty";
pub const FOUNDRY_LOCAL_PARAM_PRESENCE_PENALTY: &str = "presence_penalty";
pub const FOUNDRY_LOCAL_PARAM_SEED: &str = "seed";
pub const FOUNDRY_LOCAL_PARAM_EARLY_STOPPING: &str = "early_stopping";
pub const FOUNDRY_LOCAL_PARAM_DO_SAMPLE: &str = "do_sample";
pub const FOUNDRY_LOCAL_PARAM_TOOL_CHOICE: &str = "tool_choice";

// ── Versioned data structs ───────────────────────────────────────────────────

#[repr(C)]
pub struct flUsage {
    pub version: u32,
    pub prompt_tokens: i64,
    pub completion_tokens: i64,
    pub total_tokens: i64,
}

#[repr(C)]
pub struct flEpInfo {
    pub version: u32,
    pub name: *const c_char,
    pub is_registered: bool,
}

pub type flBytesDataDeleter =
    Option<unsafe extern "C" fn(data: *const flBytesData, user_data: *mut c_void)>;
pub type flTensorDataDeleter =
    Option<unsafe extern "C" fn(data: *const flTensorData, user_data: *mut c_void)>;
pub type flImageDataDeleter =
    Option<unsafe extern "C" fn(data: *const flImageData, user_data: *mut c_void)>;
pub type flAudioDataDeleter =
    Option<unsafe extern "C" fn(data: *const flAudioData, user_data: *mut c_void)>;

#[repr(C)]
pub struct flTextData {
    pub version: u32,
    pub text: *const c_char,
    pub r#type: flTextItemType,
}

#[repr(C)]
pub struct flBytesData {
    pub version: u32,
    pub item_type: flItemType,
    pub data: *const c_void,
    pub mutable_data: *mut c_void,
    pub data_size: usize,
    pub deleter: flBytesDataDeleter,
    pub deleter_user_data: *mut c_void,
}

#[repr(C)]
pub struct flTensorData {
    pub version: u32,
    pub data_type: flTensorDataType,
    pub data: *const c_void,
    pub mutable_data: *mut c_void,
    pub shape: *const i64,
    pub rank: usize,
    pub deleter: flTensorDataDeleter,
    pub deleter_user_data: *mut c_void,
}

#[repr(C)]
pub struct flMessageData {
    pub version: u32,
    pub role: flMessageRole,
    pub content_items: *const *const flItem,
    pub content_items_count: usize,
    pub name: *const c_char,
}

#[repr(C)]
pub struct flImageData {
    pub version: u32,
    pub data: *const c_void,
    pub mutable_data: *mut c_void,
    pub data_size: usize,
    pub format: *const c_char,
    pub uri: *const c_char,
    pub deleter: flImageDataDeleter,
    pub deleter_user_data: *mut c_void,
}

#[repr(C)]
pub struct flAudioData {
    pub version: u32,
    pub data: *const c_void,
    pub mutable_data: *mut c_void,
    pub data_size: usize,
    pub format: *const c_char,
    pub uri: *const c_char,
    pub sample_rate: c_int,
    pub channels: c_int,
    pub deleter: flAudioDataDeleter,
    pub deleter_user_data: *mut c_void,
}

#[repr(C)]
pub struct flToolCallData {
    pub version: u32,
    pub call_id: *const c_char,
    pub name: *const c_char,
    pub arguments: *const c_char,
}

#[repr(C)]
pub struct flToolResultData {
    pub version: u32,
    pub call_id: *const c_char,
    pub result: *const c_char,
}

// ── Speech recognition output types (output-only; see foundry_local_c.h) ──────

/// Sentinel for absent time fields in the speech structs (`INT64_MIN`).
pub const FOUNDRY_LOCAL_DURATION_UNSET: i64 = i64::MIN;
/// Sentinel for absent confidence in `flSpeechWord` (`-FLT_MAX`).
pub const FOUNDRY_LOCAL_CONFIDENCE_UNSET: f32 = f32::MIN;

pub type flSpeechSegmentKind = c_int;
pub const FOUNDRY_LOCAL_SPEECH_SEGMENT_NONE: flSpeechSegmentKind = 0;
pub const FOUNDRY_LOCAL_SPEECH_SEGMENT_PARTIAL: flSpeechSegmentKind = 1;
pub const FOUNDRY_LOCAL_SPEECH_SEGMENT_FINAL: flSpeechSegmentKind = 2;

#[repr(C)]
pub struct flSpeechWord {
    pub version: u32,
    pub text: *const c_char,
    pub start_time_ms: i64,
    pub end_time_ms: i64,
    pub confidence: f32,
    pub speaker_id: *const c_char,
}

#[repr(C)]
pub struct flSpeechSegmentData {
    pub version: u32,
    pub kind: flSpeechSegmentKind,
    pub text: *const c_char,
    pub start_time_ms: i64,
    pub end_time_ms: i64,
    pub utterance_start: bool,
    pub words: *const flSpeechWord,
    pub words_count: usize,
    pub language: *const c_char,
}

#[repr(C)]
pub struct flSpeechResultData {
    pub version: u32,
    pub text: *const c_char,
    pub language: *const c_char,
    pub duration_ms: i64,
    pub segments: *const *const flItem,
    pub segments_count: usize,
}

#[repr(C)]
pub struct flStreamingCallbackData {
    pub version: u32,
    pub item_queue: *mut flItemQueue,
}

#[repr(C)]
pub struct flToolDefinition {
    pub version: u32,
    pub name: *const c_char,
    pub description: *const c_char,
    pub json_schema: *const c_char,
}

// ── Callback types (plain C calling convention) ──────────────────────────────

pub type flProgressCallback =
    Option<unsafe extern "C" fn(value: f32, user_data: *mut c_void) -> c_int>;
pub type flStreamingCallback =
    Option<unsafe extern "C" fn(event: flStreamingCallbackData, user_data: *mut c_void) -> c_int>;
pub type flEpProgressCallback = Option<
    unsafe extern "C" fn(ep_name: *const c_char, value: f32, user_data: *mut c_void) -> c_int,
>;

// ── Exported entry points (FL_API_CALL == __stdcall on Win32) ────────────────
//
// The library is loaded at runtime via `libloading`; these are the signatures of
// the two exported symbols resolved from it. `flApi` is the root vtable type.

pub type flApi = flApiVtable;

pub type FoundryLocalGetApiFn = unsafe extern "system" fn(version: u32) -> *const flApiVtable;
pub type FoundryLocalGetVersionStringFn = unsafe extern "system" fn() -> *const c_char;

pub const FOUNDRY_LOCAL_GET_API_SYMBOL: &[u8] = b"FoundryLocalGetApi\0";
pub const FOUNDRY_LOCAL_GET_VERSION_STRING_SYMBOL: &[u8] = b"FoundryLocalGetVersionString\0";

// ── Function tables ──────────────────────────────────────────────────────────
//
// Field order and signatures MUST match foundry_local_c.h exactly — the tables
// are consumed positionally, so a new entry inserted mid-table upstream must be
// mirrored at the same position here (e.g. GetSpeechSegment/GetSpeechResult sit
// between GetToolResult and GetMetadata in flItemApi).

/// Root API table (`flApi`).
#[repr(C)]
pub struct flApiVtable {
    // Status
    pub Status_Create:
        unsafe extern "system" fn(error_code: flErrorCode, error_msg: *const c_char) -> flStatusPtr,
    pub Status_Release: unsafe extern "system" fn(instance: *mut flStatus),
    pub Status_GetErrorCode: unsafe extern "system" fn(status: *const flStatus) -> flErrorCode,
    pub Status_GetErrorMessage: unsafe extern "system" fn(status: *const flStatus) -> *const c_char,

    // Manager lifecycle
    pub Manager_Create: unsafe extern "system" fn(
        config: *const flConfiguration,
        out_manager: *mut *mut flManager,
    ) -> flStatusPtr,
    pub Manager_Release: unsafe extern "system" fn(instance: *mut flManager),

    pub Manager_GetCatalog: unsafe extern "system" fn(
        manager: *const flManager,
        out_catalog: *mut *mut flCatalog,
    ) -> flStatusPtr,
    pub Manager_WebServiceStart: unsafe extern "system" fn(manager: *mut flManager) -> flStatusPtr,
    pub Manager_WebServiceUrls: unsafe extern "system" fn(
        manager: *const flManager,
        out_urls: *mut *const *const c_char,
        out_num_urls: *mut usize,
    ) -> flStatusPtr,
    pub Manager_WebServiceStop: unsafe extern "system" fn(manager: *mut flManager) -> flStatusPtr,

    // Sub-API accessors
    pub GetCatalogApi: unsafe extern "system" fn() -> *const flCatalogApiVtable,
    pub GetConfigurationApi: unsafe extern "system" fn() -> *const flConfigurationApiVtable,
    pub GetItemApi: unsafe extern "system" fn() -> *const flItemApiVtable,
    pub GetInferenceApi: unsafe extern "system" fn() -> *const flInferenceApiVtable,
    pub GetModelApi: unsafe extern "system" fn() -> *const flModelApiVtable,

    // KeyValuePairs
    pub CreateKeyValuePairs: unsafe extern "system" fn(out: *mut *mut flKeyValuePairs),
    pub AddKeyValuePair: unsafe extern "system" fn(
        kvps: *mut flKeyValuePairs,
        key: *const c_char,
        value: *const c_char,
    ),
    pub GetKeyValue: unsafe extern "system" fn(
        kvps: *const flKeyValuePairs,
        key: *const c_char,
    ) -> *const c_char,
    pub GetKeyValuePairs: unsafe extern "system" fn(
        kvps: *const flKeyValuePairs,
        keys: *mut *const *const c_char,
        values: *mut *const *const c_char,
        num_entries: *mut usize,
    ),
    pub RemoveKeyValuePair:
        unsafe extern "system" fn(kvps: *mut flKeyValuePairs, key: *const c_char),
    pub KeyValuePairs_Release: unsafe extern "system" fn(instance: *mut flKeyValuePairs),

    // ModelList
    pub ModelList_Release: unsafe extern "system" fn(instance: *mut flModelList),
    pub ModelList_Size: unsafe extern "system" fn(models: *const flModelList) -> usize,
    pub ModelList_GetAt:
        unsafe extern "system" fn(models: *const flModelList, idx: usize) -> *mut flModel,

    // EP detection
    pub Manager_GetDiscoverableEps: unsafe extern "system" fn(
        manager: *const flManager,
        out_eps: *mut *const flEpInfo,
        out_count: *mut usize,
    ) -> flStatusPtr,
    pub Manager_DownloadAndRegisterEps: unsafe extern "system" fn(
        manager: *mut flManager,
        ep_names: *const *const c_char,
        num_ep_names: usize,
        callback: flEpProgressCallback,
        user_data: *mut c_void,
    ) -> flStatusPtr,
    pub Manager_IsEpDownloadInProgress:
        unsafe extern "system" fn(manager: *const flManager) -> bool,

    pub Manager_Shutdown: unsafe extern "system" fn(manager: *mut flManager) -> flStatusPtr,
    pub Manager_IsShutdownRequested: unsafe extern "system" fn(manager: *const flManager) -> bool,
}

/// Item API table (`flItemApi`).
#[repr(C)]
pub struct flItemApiVtable {
    pub Create:
        unsafe extern "system" fn(item_type: flItemType, out_item: *mut *mut flItem) -> flStatusPtr,
    pub Item_Release: unsafe extern "system" fn(instance: *mut flItem),
    pub GetType: unsafe extern "system" fn(item: *const flItem) -> flItemType,

    pub SetBytes:
        unsafe extern "system" fn(item: *mut flItem, bytes: *const flBytesData) -> flStatusPtr,
    pub SetTensor:
        unsafe extern "system" fn(item: *mut flItem, tensor: *const flTensorData) -> flStatusPtr,
    pub SetText:
        unsafe extern "system" fn(item: *mut flItem, text_data: *const flTextData) -> flStatusPtr,
    pub SetMessage:
        unsafe extern "system" fn(item: *mut flItem, message: *const flMessageData) -> flStatusPtr,
    pub SetImage:
        unsafe extern "system" fn(item: *mut flItem, image: *const flImageData) -> flStatusPtr,
    pub SetAudio:
        unsafe extern "system" fn(item: *mut flItem, audio: *const flAudioData) -> flStatusPtr,
    pub SetToolCall: unsafe extern "system" fn(
        item: *mut flItem,
        tool_call: *const flToolCallData,
    ) -> flStatusPtr,
    pub SetToolResult: unsafe extern "system" fn(
        item: *mut flItem,
        tool_result: *const flToolResultData,
    ) -> flStatusPtr,

    pub GetBytes:
        unsafe extern "system" fn(item: *const flItem, out_bytes: *mut flBytesData) -> flStatusPtr,
    pub GetText: unsafe extern "system" fn(
        item: *const flItem,
        out_text_data: *mut flTextData,
    ) -> flStatusPtr,
    pub GetMessage: unsafe extern "system" fn(
        item: *const flItem,
        out_message: *mut flMessageData,
    ) -> flStatusPtr,
    pub GetTensor: unsafe extern "system" fn(
        item: *const flItem,
        out_tensor: *mut flTensorData,
    ) -> flStatusPtr,
    pub GetImage:
        unsafe extern "system" fn(item: *const flItem, out_image: *mut flImageData) -> flStatusPtr,
    pub GetAudio:
        unsafe extern "system" fn(item: *const flItem, out_audio: *mut flAudioData) -> flStatusPtr,
    pub GetToolCall: unsafe extern "system" fn(
        item: *const flItem,
        out_tool_call: *mut flToolCallData,
    ) -> flStatusPtr,
    pub GetToolResult: unsafe extern "system" fn(
        item: *const flItem,
        out_tool_result: *mut flToolResultData,
    ) -> flStatusPtr,

    pub GetSpeechSegment: unsafe extern "system" fn(
        item: *const flItem,
        out_segment: *mut flSpeechSegmentData,
    ) -> flStatusPtr,
    pub GetSpeechResult: unsafe extern "system" fn(
        item: *const flItem,
        out_result: *mut flSpeechResultData,
    ) -> flStatusPtr,

    pub GetMetadata: unsafe extern "system" fn(
        item: *const flItem,
        out_metadata: *mut *const flKeyValuePairs,
    ) -> flStatusPtr,
    pub GetMutableMetadata: unsafe extern "system" fn(
        item: *mut flItem,
        out_metadata: *mut *mut flKeyValuePairs,
    ) -> flStatusPtr,
    pub GetQueue: unsafe extern "system" fn(
        item: *mut flItem,
        out_queue: *mut *mut flItemQueue,
    ) -> flStatusPtr,

    // ItemQueue
    pub ItemQueue_Create:
        unsafe extern "system" fn(out_queue: *mut *mut flItemQueue) -> flStatusPtr,
    pub ItemQueue_Release: unsafe extern "system" fn(instance: *mut flItemQueue),
    pub ItemQueue_Push:
        unsafe extern "system" fn(queue: *mut flItemQueue, item: *mut flItem) -> flStatusPtr,
    pub ItemQueue_TryPop:
        unsafe extern "system" fn(queue: *mut flItemQueue, out_item: *mut *mut flItem) -> bool,
    pub ItemQueue_Size: unsafe extern "system" fn(queue: *const flItemQueue) -> usize,
    pub ItemQueue_MarkFinished: unsafe extern "system" fn(queue: *mut flItemQueue),
    pub ItemQueue_IsFinished: unsafe extern "system" fn(queue: *const flItemQueue) -> bool,
}

/// Inference API table (`flInferenceApi`).
#[repr(C)]
pub struct flInferenceApiVtable {
    pub Request_Create: unsafe extern "system" fn(out_request: *mut *mut flRequest) -> flStatusPtr,
    pub Request_Release: unsafe extern "system" fn(instance: *mut flRequest),
    pub Request_AddItem: unsafe extern "system" fn(
        request: *mut flRequest,
        item: *mut flItem,
        take_ownership: bool,
    ) -> flStatusPtr,
    pub Request_GetItemCount: unsafe extern "system" fn(request: *const flRequest) -> usize,
    pub Request_GetItem: unsafe extern "system" fn(
        request: *const flRequest,
        idx: usize,
        out_item: *mut *const flItem,
    ) -> flStatusPtr,
    pub Request_SetOptions: unsafe extern "system" fn(
        request: *mut flRequest,
        options: *const flKeyValuePairs,
    ) -> flStatusPtr,
    pub Request_Cancel: unsafe extern "system" fn(request: *mut flRequest) -> flStatusPtr,

    pub Response_Create:
        unsafe extern "system" fn(out_response: *mut *mut flResponse) -> flStatusPtr,
    pub Response_Release: unsafe extern "system" fn(instance: *mut flResponse),
    pub Response_GetItemCount: unsafe extern "system" fn(response: *const flResponse) -> usize,
    pub Response_GetItem: unsafe extern "system" fn(
        response: *const flResponse,
        idx: usize,
        out_item: *mut *const flItem,
    ) -> flStatusPtr,
    pub Response_GetFinishReason:
        unsafe extern "system" fn(response: *const flResponse) -> flFinishReason,
    pub Response_GetUsage: unsafe extern "system" fn(
        response: *const flResponse,
        out_usage: *mut flUsage,
    ) -> flStatusPtr,

    pub Session_Create: unsafe extern "system" fn(
        model: *const flModel,
        out_session: *mut *mut flSession,
    ) -> flStatusPtr,
    pub Session_Release: unsafe extern "system" fn(instance: *mut flSession),
    pub Session_SetStreamingCallback: unsafe extern "system" fn(
        session: *mut flSession,
        callback: flStreamingCallback,
        user_data: *mut c_void,
    ) -> flStatusPtr,
    pub Session_SetOptions: unsafe extern "system" fn(
        session: *mut flSession,
        options: *const flKeyValuePairs,
    ) -> flStatusPtr,
    pub Session_ProcessRequest: unsafe extern "system" fn(
        session: *mut flSession,
        request: *const flRequest,
        response: *mut *mut flResponse,
    ) -> flStatusPtr,
    pub Session_AddToolDefinition: unsafe extern "system" fn(
        session: *mut flSession,
        tool_def: *const flToolDefinition,
    ) -> flStatusPtr,
    pub Session_RemoveToolDefinition: unsafe extern "system" fn(
        session: *mut flSession,
        tool_name: *const c_char,
        out_removed: *mut bool,
    ) -> flStatusPtr,
    pub Session_GetTurnCount: unsafe extern "system" fn(session: *const flSession) -> usize,
    pub Session_UndoTurns:
        unsafe extern "system" fn(session: *mut flSession, count: usize) -> flStatusPtr,
}

/// Configuration API table (`flConfigurationApi`).
#[repr(C)]
pub struct flConfigurationApiVtable {
    pub Create: unsafe extern "system" fn(
        app_name: *const c_char,
        out_config: *mut *mut flConfiguration,
    ) -> flStatusPtr,
    pub Configuration_Release: unsafe extern "system" fn(instance: *mut flConfiguration),
    pub SetDefaultLogLevel:
        unsafe extern "system" fn(config: *mut flConfiguration, level: flLogLevel) -> flStatusPtr,
    pub SetAppDataDir:
        unsafe extern "system" fn(config: *mut flConfiguration, dir: *const c_char) -> flStatusPtr,
    pub SetLogsDir:
        unsafe extern "system" fn(config: *mut flConfiguration, dir: *const c_char) -> flStatusPtr,
    pub SetModelCacheDir:
        unsafe extern "system" fn(config: *mut flConfiguration, dir: *const c_char) -> flStatusPtr,
    pub AddCatalogUrl: unsafe extern "system" fn(
        config: *mut flConfiguration,
        url: *const c_char,
        filter_override: *const c_char,
    ) -> flStatusPtr,
    pub SetCatalogRegion: unsafe extern "system" fn(
        config: *mut flConfiguration,
        region: *const c_char,
    ) -> flStatusPtr,
    pub AddWebServiceEndpoint:
        unsafe extern "system" fn(config: *mut flConfiguration, url: *const c_char) -> flStatusPtr,
    pub SetExternalServiceUrl:
        unsafe extern "system" fn(config: *mut flConfiguration, url: *const c_char) -> flStatusPtr,
    pub SetAdditionalOptions: unsafe extern "system" fn(
        config: *mut flConfiguration,
        options: *const flKeyValuePairs,
    ) -> flStatusPtr,
}

/// Catalog API table (`flCatalogApi`).
#[repr(C)]
pub struct flCatalogApiVtable {
    pub GetName: unsafe extern "system" fn(
        catalog: *const flCatalog,
        out_name: *mut *const c_char,
    ) -> flStatusPtr,
    pub GetModels: unsafe extern "system" fn(
        catalog: *const flCatalog,
        out_models: *mut *mut flModelList,
    ) -> flStatusPtr,
    pub GetModel: unsafe extern "system" fn(
        catalog: *const flCatalog,
        alias: *const c_char,
        out_model: *mut *mut flModel,
    ) -> flStatusPtr,
    pub GetModelVariant: unsafe extern "system" fn(
        catalog: *const flCatalog,
        model_id: *const c_char,
        out_model: *mut *mut flModel,
    ) -> flStatusPtr,
    pub GetLatestVersion: unsafe extern "system" fn(
        catalog: *const flCatalog,
        model: *const flModel,
        out_model: *mut *mut flModel,
    ) -> flStatusPtr,
    pub GetCachedModels: unsafe extern "system" fn(
        catalog: *const flCatalog,
        out_models: *mut *mut flModelList,
    ) -> flStatusPtr,
    pub GetLoadedModels: unsafe extern "system" fn(
        catalog: *const flCatalog,
        out_models: *mut *mut flModelList,
    ) -> flStatusPtr,
    pub GetModelVersions: unsafe extern "system" fn(
        catalog: *const flCatalog,
        model_alias: *const c_char,
        model_name: *const c_char,
        max_versions: i32,
        out_models: *mut *mut flModelList,
    ) -> flStatusPtr,
}

/// Model API table (`flModelApi`).
#[repr(C)]
pub struct flModelApiVtable {
    pub GetInfo: unsafe extern "system" fn(
        model: *const flModel,
        out_info: *mut *const flModelInfo,
    ) -> flStatusPtr,
    pub GetInputOutputInfo: unsafe extern "system" fn(
        model: *const flModel,
        out_inputs: *mut *const *const flItem,
        out_num_inputs: *mut usize,
        out_outputs: *mut *const *const flItem,
        out_num_outputs: *mut usize,
    ) -> flStatusPtr,

    pub IsCached:
        unsafe extern "system" fn(model: *const flModel, out_cached: *mut c_int) -> flStatusPtr,
    pub GetPath: unsafe extern "system" fn(
        model: *const flModel,
        out_path: *mut *const c_char,
    ) -> flStatusPtr,
    pub Download: unsafe extern "system" fn(
        model: *mut flModel,
        callback: flProgressCallback,
        user_data: *mut c_void,
    ) -> flStatusPtr,

    pub IsLoaded:
        unsafe extern "system" fn(model: *const flModel, out_loaded: *mut c_int) -> flStatusPtr,
    pub Load: unsafe extern "system" fn(model: *mut flModel) -> flStatusPtr,
    pub Unload: unsafe extern "system" fn(model: *mut flModel) -> flStatusPtr,
    pub RemoveFromCache: unsafe extern "system" fn(model: *mut flModel) -> flStatusPtr,

    pub GetVariants: unsafe extern "system" fn(
        model: *const flModel,
        out_variants: *mut *mut flModelList,
    ) -> flStatusPtr,
    pub SelectVariant:
        unsafe extern "system" fn(model: *mut flModel, variant: *const flModel) -> flStatusPtr,

    pub Info_GetId: unsafe extern "system" fn(info: *const flModelInfo) -> *const c_char,
    pub Info_GetName: unsafe extern "system" fn(info: *const flModelInfo) -> *const c_char,
    pub Info_GetVersion: unsafe extern "system" fn(info: *const flModelInfo) -> c_int,
    pub Info_GetAlias: unsafe extern "system" fn(info: *const flModelInfo) -> *const c_char,
    pub Info_GetUri: unsafe extern "system" fn(info: *const flModelInfo) -> *const c_char,
    pub Info_GetDeviceType: unsafe extern "system" fn(info: *const flModelInfo) -> flDeviceType,
    pub Info_GetExecutionProvider:
        unsafe extern "system" fn(info: *const flModelInfo) -> *const c_char,
    pub Info_GetTask: unsafe extern "system" fn(info: *const flModelInfo) -> *const c_char,
    pub Info_GetPromptTemplates:
        unsafe extern "system" fn(info: *const flModelInfo) -> *const flKeyValuePairs,
    pub Info_GetModelSettings:
        unsafe extern "system" fn(info: *const flModelInfo) -> *const flKeyValuePairs,
    pub Info_GetStringProperty:
        unsafe extern "system" fn(info: *const flModelInfo, key: *const c_char) -> *const c_char,
    pub Info_GetIntProperty: unsafe extern "system" fn(
        info: *const flModelInfo,
        key: *const c_char,
        default_value: i64,
    ) -> i64,
}
