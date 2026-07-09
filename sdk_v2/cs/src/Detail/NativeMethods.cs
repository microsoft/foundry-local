// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

// Low-level P/Invoke bindings for the Foundry Local C API.
// Generated from foundry_local_c.h — update when the C header changes.
//
// Architecture:
//   The native library exports only two symbols:
//     - FoundryLocalGetApi(uint version) → flApi*
//     - FoundryLocalGetVersionString() → const char*
//   All other functionality is accessed through versioned vtable structs
//   of function pointers returned by FoundryLocalGetApi.
//
// Usage:
//   var apiPtr = NativeMethods.FoundryLocalGetApi(NativeMethods.ApiVersion);
//   var api = Marshal.PtrToStructure<NativeMethods.FlApi>(apiPtr);
//   // api.ManagerCreate(...), api.GetCatalogApi(), etc.

// Suppress analyzer warnings for generated interop bindings that must match the C API header exactly.
#pragma warning disable CA1720  // Identifier contains type name (enum values must match C header)
#pragma warning disable CA1401  // P/Invokes should not be visible (internal use only, via Detail.Native.Api)
#pragma warning disable SYSLIB1054 // Use LibraryImportAttribute (delegates use DllImport-only marshalling features)

namespace Microsoft.AI.Foundry.Local.Detail.Interop;

using System.Runtime.InteropServices;

// -----------------------------------------------------------------------
// Constants
// -----------------------------------------------------------------------
public static partial class NativeMethods
{
    public const uint ApiVersion = 1;
    public const string LibraryName = "foundry_local";

    // The first P/Invoke through this class can come from any of several entry
    // points (FoundryLocalManager, but also direct uses of Request/Item/ItemQueue
    // by tests and consumers). Run the native-library bootstrap here so it always
    // happens before the JIT resolves the first DllImport regardless of caller.
    // DllLoader.Initialize is idempotent and cheap.
    static NativeMethods()
    {
        DllLoader.Initialize();
    }

    // -----------------------------------------------------------------------
    // Exported functions — the ONLY two DLL exports
    // -----------------------------------------------------------------------

    [DllImport(LibraryName, CallingConvention = CallingConvention.Winapi, EntryPoint = "FoundryLocalGetApi")]
    public static extern IntPtr FoundryLocalGetApi(uint version);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Winapi, EntryPoint = "FoundryLocalGetVersionString")]
    public static extern IntPtr FoundryLocalGetVersionString();
}

// -----------------------------------------------------------------------
// Enums  (values must match foundry_local_c.h exactly)
// -----------------------------------------------------------------------

public enum FlErrorCode
{
    Ok = 0,
    NotImplemented = 1,
    Internal = 2,
    InvalidArgument = 3,
    InvalidUsage = 4,
    OperationCancelled = 5,
    Network = 6,
}

public enum FlLogLevel
{
    Verbose = 0,
    Debug = 1,
    Info = 2,
    Warning = 3,
    Error = 4,
    Fatal = 5,
}

public enum FlDeviceType
{
    NotSet = 0,
    CPU = 1,
    GPU = 2,
    NPU = 3,
}

public enum FlTensorDataType
{
    Undefined = 0,
    Float = 1,
    UInt8 = 2,
    Int8 = 3,
    UInt16 = 4,
    Int16 = 5,
    Int32 = 6,
    Int64 = 7,
    String = 8,
    Bool = 9,
    Float16 = 10,
    Double = 11,
    UInt32 = 12,
    UInt64 = 13,
    Complex64 = 14,
    Complex128 = 15,
    BFloat16 = 16,
    Float8E4M3FN = 17,
    Float8E4M3FNUZ = 18,
    Float8E5M2 = 19,
    Float8E5M2FNUZ = 20,
    UInt4 = 21,
    Int4 = 22,
    Float4E2M1 = 23,
    Float8E8M0 = 24,
}

public enum FlItemType
{
    Unknown = 0,
    Bytes = 1,
    Tensor = 10,
    Text = 20,
    Message = 21,
    Image = 25,
    Audio = 30,
    SpeechSegment = 31,
    SpeechResult = 32,
    ToolCall = 100,
    ToolResult = 101,
    Queue = 200,
}

/// <summary>Discriminator for a SPEECH_SEGMENT item; matches flSpeechSegmentKind.</summary>
public enum FlSpeechSegmentKind
{
    None = 0,
    Partial = 1,
    Final = 2,
}

/// <summary>
/// Subtype tag for TEXT items. Distinguishes ordinary assistant text from reasoning content
/// and from opaque OpenAI REST JSON payloads carried as text.
/// </summary>
public enum FlTextItemType
{
    Default = 0,
    Reasoning = 1,
    OpenAIJson = 2,
}

public enum FlMessageRole
{
    None = 0,
    System = 1,
    User = 2,
    Assistant = 3,
    Tool = 4,
    Developer = 5,
}

public enum FlFinishReason
{
    None = 0,
    Error = 1,
    Stop = 2,
    Length = 3,
    ToolCalls = 4,
}

// -----------------------------------------------------------------------
// Versioned data structs  (layout must match C header on x64)
//
// Default StructLayout Sequential packing matches C natural alignment:
//   uint32 before a pointer gets 4 bytes of implicit padding.
// -----------------------------------------------------------------------

[StructLayout(LayoutKind.Sequential)]
public struct FlUsage
{
    public uint Version;
    // 4 bytes implicit padding
    public long PromptTokens;
    public long CompletionTokens;
    public long TotalTokens;
}

[StructLayout(LayoutKind.Sequential)]
public struct FlBytesData
{
    public uint Version;
    public FlItemType ItemType;
    public IntPtr Data;           // const void*
    public IntPtr MutableData;    // void* (NULL for read-only)
    public UIntPtr DataSize;
    public IntPtr Deleter;        // flBytesDataDeleter (NULL if not owned)
    public IntPtr DeleterUserData;
}

[StructLayout(LayoutKind.Sequential)]
public struct FlTensorData
{
    public uint Version;
    public FlTensorDataType DataType;
    public IntPtr Data;           // const void*
    public IntPtr MutableData;    // void* (NULL for read-only)
    public IntPtr Shape;          // const int64_t*
    public UIntPtr Rank;
    public IntPtr Deleter;        // flTensorDataDeleter (NULL if not owned)
    public IntPtr DeleterUserData;
}

[StructLayout(LayoutKind.Sequential)]
public struct FlMessageData
{
    public uint Version;
    public FlMessageRole Role;
    public IntPtr ContentItems;       // const flItem* const* (pointer to array of flItem*)
    public UIntPtr ContentItemsCount; // size_t
    public IntPtr Name;               // const char* (nullable)
}

[StructLayout(LayoutKind.Sequential)]
public struct FlTextData
{
    public uint Version;
    public IntPtr Text;          // const char* (UTF-8)
    public FlTextItemType Type;
    // 4 bytes implicit trailing padding to 8-byte alignment
}

[StructLayout(LayoutKind.Sequential)]
public struct FlImageData
{
    public uint Version;
    // 4 bytes implicit padding
    public IntPtr Data;           // const void*
    public IntPtr MutableData;    // void* (NULL for read-only)
    public UIntPtr DataSize;
    public IntPtr Format;         // const char*
    public IntPtr Uri;            // const char*
    public IntPtr Deleter;        // flImageDataDeleter (NULL if not owned)
    public IntPtr DeleterUserData;
}

[StructLayout(LayoutKind.Sequential)]
public struct FlAudioData
{
    public uint Version;
    // 4 bytes implicit padding
    public IntPtr Data;           // const void*
    public IntPtr MutableData;    // void* (NULL for read-only)
    public UIntPtr DataSize;
    public IntPtr Format;         // const char*
    public IntPtr Uri;            // const char*
    public int SampleRate;        // int (4 bytes)
    public int Channels;          // int (4 bytes)
    public IntPtr Deleter;        // flAudioDataDeleter (NULL if not owned)
    public IntPtr DeleterUserData;
}

[StructLayout(LayoutKind.Sequential)]
public struct FlToolCallData
{
    public uint Version;
    // 4 bytes implicit padding
    public IntPtr CallId;      // const char*
    public IntPtr Name;        // const char*
    public IntPtr Arguments;   // const char*
}

[StructLayout(LayoutKind.Sequential)]
public struct FlToolResultData
{
    public uint Version;
    // 4 bytes implicit padding
    public IntPtr CallId;   // const char*
    public IntPtr Result;   // const char*
}

/// <summary>Sentinel for absent time fields in flSpeechWord / flSpeechSegmentData / flSpeechResultData.</summary>
public static class FlSpeech
{
    public const long DurationUnset = long.MinValue;  // INT64_MIN
    public const float ConfidenceUnset = float.MinValue;  // -FLT_MAX (FOUNDRY_LOCAL_CONFIDENCE_UNSET)
}

[StructLayout(LayoutKind.Sequential)]
public struct FlSpeechWord
{
    public uint Version;
    // 4 bytes implicit padding
    public IntPtr Text;             // const char* (UTF-8)
    public long StartTimeMs;        // FlSpeech.DurationUnset if absent
    public long EndTimeMs;          // FlSpeech.DurationUnset if absent
    public float Confidence;        // FlSpeech.ConfidenceUnset if absent
    // 4 bytes implicit padding before IntPtr
    public IntPtr SpeakerId;        // const char* (NULL if absent)
}

[StructLayout(LayoutKind.Sequential)]
public struct FlSpeechSegmentData
{
    public uint Version;
    public FlSpeechSegmentKind Kind;
    public IntPtr Text;             // const char* (UTF-8, may be NULL/"")
    public long StartTimeMs;        // FlSpeech.DurationUnset if absent
    public long EndTimeMs;          // FlSpeech.DurationUnset if absent
    [MarshalAs(UnmanagedType.U1)]
    public bool UtteranceStart;
    // 7 bytes implicit padding before pointer
    public IntPtr Words;            // const flSpeechWord* (borrowed array)
    public UIntPtr WordsCount;
    public IntPtr Language;         // const char* (NULL if absent)
}

[StructLayout(LayoutKind.Sequential)]
public struct FlSpeechResultData
{
    public uint Version;
    // 4 bytes implicit padding
    public IntPtr Text;             // const char* (UTF-8, may be NULL/"")
    public IntPtr Language;         // const char* (NULL if absent)
    public long DurationMs;         // FlSpeech.DurationUnset if absent
    public IntPtr Segments;         // const flItem* const* (borrowed array)
    public UIntPtr SegmentsCount;
}

[StructLayout(LayoutKind.Sequential)]
public struct FlStreamingCallbackData
{
    public uint Version;
    // 4 bytes implicit padding
    public IntPtr ItemQueue;
}

[StructLayout(LayoutKind.Sequential)]
public struct FlToolDefinition
{
    public uint Version;
    // 4 bytes implicit padding
    public IntPtr Name;        // const char*
    public IntPtr Description; // const char*
    public IntPtr JsonSchema;  // const char*
}

[StructLayout(LayoutKind.Sequential)]
public struct FlEpInfo
{
    public uint Version;
    // 4 bytes implicit padding
    public IntPtr Name;        // const char* (UTF-8, owned by Manager)
    [MarshalAs(UnmanagedType.U1)]
    public bool IsRegistered;
    // 7 bytes implicit trailing padding to 8-byte alignment
}

// -----------------------------------------------------------------------
// Callback delegates  (Cdecl — plain C function pointers)
// -----------------------------------------------------------------------

/// <summary>Progress callback for model downloads. Return 0 to continue, non-zero to cancel.</summary>
[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
public delegate int FlProgressCallback(float value, IntPtr userData);

/// <summary>Streaming response callback. Return 0 to continue, non-zero to cancel.</summary>
[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
public delegate int FlStreamingCallback(FlStreamingCallbackData data, IntPtr userData);

/// <summary>Item destructor callback.</summary>
[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
public delegate void FlItemDeleter(IntPtr item, IntPtr userData);

/// <summary>Deleter for bytes data. Called when native item is destroyed.</summary>
[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
public delegate void FlBytesDataDeleterDelegate(ref FlBytesData data, IntPtr userData);

/// <summary>Deleter for image data. Called when native item is destroyed.</summary>
[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
public delegate void FlImageDataDeleterDelegate(ref FlImageData data, IntPtr userData);

/// <summary>Deleter for audio data. Called when native item is destroyed.</summary>
[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
public delegate void FlAudioDataDeleterDelegate(ref FlAudioData data, IntPtr userData);

// EP detection

/// <summary>EP download progress callback. Return 0 to continue, non-zero to cancel.</summary>
[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
public delegate int FlEpProgressCallback(
    [MarshalAs((UnmanagedType)48 /* LPUTF8Str */)] string epName,
    float value,
    IntPtr userData);

// -----------------------------------------------------------------------
// Delegate types for vtable function pointers
//
// Naming: Fl{SubApi}_{FunctionName}Delegate
// All delegates use StdCall to match FL_API_CALL (__stdcall).
// Functions returning flStatus* return IntPtr (non-null = error).
// Opaque handle types are IntPtr.
// -----------------------------------------------------------------------

// --- Root API (flApi) delegates ---

// Status
[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlApi_StatusCreateDelegate(FlErrorCode errorCode, [MarshalAs((UnmanagedType)48 /* LPUTF8Str */)] string errorMsg);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate void FlApi_StatusReleaseDelegate(IntPtr status);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate FlErrorCode FlApi_StatusGetErrorCodeDelegate(IntPtr status);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlApi_StatusGetErrorMessageDelegate(IntPtr status);

// Manager lifecycle
[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlApi_ManagerCreateDelegate(IntPtr config, out IntPtr outManager);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate void FlApi_ManagerReleaseDelegate(IntPtr manager);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlApi_ManagerGetCatalogDelegate(IntPtr manager, out IntPtr outCatalog);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlApi_ManagerWebServiceStartDelegate(IntPtr manager);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlApi_ManagerWebServiceUrlsDelegate(IntPtr manager, out IntPtr outUrls, out UIntPtr outNumUrls);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlApi_ManagerWebServiceStopDelegate(IntPtr manager);

// Sub-API accessors
[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlApi_GetSubApiDelegate();

// KeyValuePairs
[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate void FlApi_CreateKeyValuePairsDelegate(out IntPtr outKvps);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate void FlApi_AddKeyValuePairDelegate(IntPtr kvps, [MarshalAs((UnmanagedType)48 /* LPUTF8Str */)] string key, [MarshalAs((UnmanagedType)48 /* LPUTF8Str */)] string value);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlApi_GetKeyValueDelegate(IntPtr kvps, [MarshalAs((UnmanagedType)48 /* LPUTF8Str */)] string key);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate void FlApi_GetKeyValuePairsDelegate(IntPtr kvps, out IntPtr keys, out IntPtr values, out UIntPtr numEntries);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate void FlApi_RemoveKeyValuePairDelegate(IntPtr kvps, [MarshalAs((UnmanagedType)48 /* LPUTF8Str */)] string key);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate void FlApi_KeyValuePairsReleaseDelegate(IntPtr kvps);

// ModelList
[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate void FlApi_ModelListReleaseDelegate(IntPtr models);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate UIntPtr FlApi_ModelListSizeDelegate(IntPtr models);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlApi_ModelListGetAtDelegate(IntPtr models, UIntPtr idx);

// EP detection
[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlApi_ManagerGetDiscoverableEpsDelegate(
    IntPtr manager,
    out IntPtr outEps,
    out UIntPtr outCount);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlApi_ManagerDownloadAndRegisterEpsDelegate(
    IntPtr manager,
    IntPtr epNames,
    UIntPtr numEpNames,
    FlEpProgressCallback? callback,
    IntPtr userData);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
[return: MarshalAs(UnmanagedType.U1)]
public delegate bool FlApi_ManagerIsEpDownloadInProgressDelegate(IntPtr manager);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlApi_ManagerShutdownDelegate(IntPtr manager);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
[return: MarshalAs(UnmanagedType.U1)]
public delegate bool FlApi_ManagerIsShutdownRequestedDelegate(IntPtr manager);

// --- Item API (flItemApi) delegates ---

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlItem_CreateDelegate(FlItemType type, out IntPtr outItem);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate void FlItem_ReleaseDelegate(IntPtr item);

// ItemQueue
[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlItem_QueueCreateDelegate(out IntPtr outQueue);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate void FlItem_QueueReleaseDelegate(IntPtr queue);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlItem_QueuePushDelegate(IntPtr queue, IntPtr item);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
[return: MarshalAs(UnmanagedType.U1)]
public delegate bool FlItem_QueueTryPopDelegate(IntPtr queue, out IntPtr outItem);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate UIntPtr FlItem_QueueSizeDelegate(IntPtr queue);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate void FlItem_QueueMarkFinishedDelegate(IntPtr queue);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
[return: MarshalAs(UnmanagedType.U1)]
public delegate bool FlItem_QueueGetFinishedDelegate(IntPtr queue);

// Item type
[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate FlItemType FlItem_GetTypeDelegate(IntPtr item);

// Setters — take versioned struct by pointer (ref in C#)
[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlItem_SetBytesDelegate(IntPtr item, ref FlBytesData bytes);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlItem_SetTensorDelegate(IntPtr item, ref FlTensorData tensor);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlItem_SetTextDelegate(IntPtr item, ref FlTextData text);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlItem_SetMessageDelegate(IntPtr item, ref FlMessageData message);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlItem_SetImageDelegate(IntPtr item, ref FlImageData image);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlItem_SetAudioDelegate(IntPtr item, ref FlAudioData audio);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlItem_SetToolCallDelegate(IntPtr item, ref FlToolCallData toolCall);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlItem_SetToolResultDelegate(IntPtr item, ref FlToolResultData toolResult);

// Getters — fill versioned struct by pointer (out in C#)
[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlItem_GetBytesDelegate(IntPtr item, out FlBytesData outBytes);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlItem_GetTextDelegate(IntPtr item, out FlTextData outText);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlItem_GetMessageDelegate(IntPtr item, out FlMessageData outMessage);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlItem_GetTensorDelegate(IntPtr item, out FlTensorData outTensor);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlItem_GetImageDelegate(IntPtr item, out FlImageData outImage);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlItem_GetAudioDelegate(IntPtr item, out FlAudioData outAudio);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlItem_GetToolCallDelegate(IntPtr item, out FlToolCallData outToolCall);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlItem_GetToolResultDelegate(IntPtr item, out FlToolResultData outToolResult);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlItem_GetSpeechSegmentDelegate(IntPtr item, out FlSpeechSegmentData outSegment);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlItem_GetSpeechResultDelegate(IntPtr item, out FlSpeechResultData outResult);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlItem_GetMetadataDelegate(IntPtr item, out IntPtr outMetadata);

// Queue item get
[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlItem_GetQueueDelegate(IntPtr item, out IntPtr outQueue);

// --- Inference API (flInferenceApi) delegates ---

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlInference_RequestCreateDelegate(out IntPtr outRequest);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate void FlInference_RequestReleaseDelegate(IntPtr request);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlInference_RequestAddItemDelegate(IntPtr request, IntPtr item, [MarshalAs(UnmanagedType.U1)] bool takeOwnership);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate UIntPtr FlInference_RequestGetItemCountDelegate(IntPtr request);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlInference_RequestGetItemDelegate(IntPtr request, UIntPtr idx, out IntPtr outItem);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlInference_RequestSetOptionsDelegate(IntPtr request, IntPtr options);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlInference_RequestCancelDelegate(IntPtr request);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlInference_ResponseCreateDelegate(out IntPtr outResponse);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate void FlInference_ResponseReleaseDelegate(IntPtr response);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate UIntPtr FlInference_ResponseGetItemCountDelegate(IntPtr response);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlInference_ResponseGetItemDelegate(IntPtr response, UIntPtr idx, out IntPtr outItem);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate FlFinishReason FlInference_ResponseGetFinishReasonDelegate(IntPtr response);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlInference_ResponseGetUsageDelegate(IntPtr response, out FlUsage outUsage);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlInference_SessionCreateDelegate(IntPtr model, out IntPtr outSession);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate void FlInference_SessionReleaseDelegate(IntPtr session);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlInference_SessionAddToolDefinitionDelegate(IntPtr session, ref FlToolDefinition toolDef);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlInference_SessionRemoveToolDefinitionDelegate(IntPtr session,
    [MarshalAs((UnmanagedType)48 /* LPUTF8Str */)] string toolName,
    [MarshalAs(UnmanagedType.U1)] out bool outRemoved);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlInference_SessionSetStreamingCallbackDelegate(IntPtr session,
    FlStreamingCallback? callback, IntPtr userData);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlInference_SessionSetOptionsDelegate(IntPtr session, IntPtr options);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlInference_SessionProcessRequestDelegate(IntPtr session, IntPtr request, ref IntPtr response);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate UIntPtr FlInference_SessionGetTurnCountDelegate(IntPtr session);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlInference_SessionUndoTurnsDelegate(IntPtr session, UIntPtr count);

// --- Configuration API (flConfigurationApi) delegates ---

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlConfig_CreateDelegate([MarshalAs((UnmanagedType)48 /* LPUTF8Str */)] string appName, out IntPtr outConfig);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate void FlConfig_ReleaseDelegate(IntPtr config);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlConfig_SetStringDelegate(IntPtr config, [MarshalAs((UnmanagedType)48 /* LPUTF8Str */)] string value);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlConfig_SetDefaultLogLevelDelegate(IntPtr config, FlLogLevel level);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlConfig_AddCatalogUrlDelegate(IntPtr config,
    [MarshalAs((UnmanagedType)48 /* LPUTF8Str */)] string url,
    [MarshalAs((UnmanagedType)48 /* LPUTF8Str */)] string? filterOverride);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlConfig_AddWebServiceEndpointDelegate(IntPtr config,
    [MarshalAs((UnmanagedType)48 /* LPUTF8Str */)] string url);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlConfig_SetAdditionalOptionsDelegate(IntPtr config, IntPtr options);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlConfig_SetExternalServiceUrlDelegate(IntPtr config,
    [MarshalAs((UnmanagedType)48 /* LPUTF8Str */)] string url);

// --- Catalog API (flCatalogApi) delegates ---

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlCatalog_GetModelsDelegate(IntPtr catalog, out IntPtr outModels);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlCatalog_GetModelDelegate(IntPtr catalog,
    [MarshalAs((UnmanagedType)48 /* LPUTF8Str */)] string alias, out IntPtr outModel);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlCatalog_GetModelVariantDelegate(IntPtr catalog,
    [MarshalAs((UnmanagedType)48 /* LPUTF8Str */)] string modelId, out IntPtr outModel);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlCatalog_GetLatestVersionDelegate(IntPtr catalog,
    IntPtr model, out IntPtr outModel);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlCatalog_GetCachedModelsDelegate(IntPtr catalog, out IntPtr outModels);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlCatalog_GetLoadedModelsDelegate(IntPtr catalog, out IntPtr outModels);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlCatalog_GetModelVersionsDelegate(IntPtr catalog, IntPtr modelAlias, IntPtr modelName, int maxVersions, out IntPtr outModels);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlCatalog_GetNameDelegate(IntPtr catalog, out IntPtr outName);

// --- Model API (flModelApi) delegates ---

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlModel_GetInputOutputInfoDelegate(IntPtr model,
    out IntPtr outInputs, out UIntPtr outNumInputs, out IntPtr outOutputs, out UIntPtr outNumOutputs);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlModel_IsCachedDelegate(IntPtr model, out int outCached);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlModel_IsLoadedDelegate(IntPtr model, out int outLoaded);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlModel_GetPathDelegate(IntPtr model, out IntPtr outPath);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlModel_DownloadDelegate(IntPtr model, FlProgressCallback? callback, IntPtr userData);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlModel_LoadDelegate(IntPtr model);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlModel_UnloadDelegate(IntPtr model);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlModel_RemoveFromCacheDelegate(IntPtr model);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlModel_GetInfoDelegate(IntPtr model, out IntPtr outInfo);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlModel_GetVariantsDelegate(IntPtr model, out IntPtr outVariants);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlModel_SelectVariantDelegate(IntPtr model, IntPtr variant);

// ModelInfo accessors
[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlModel_InfoGetStringDelegate(IntPtr info);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate int FlModel_InfoGetVersionDelegate(IntPtr info);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate FlDeviceType FlModel_InfoGetDeviceTypeDelegate(IntPtr info);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlModel_InfoGetKvpDelegate(IntPtr info);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate IntPtr FlModel_InfoGetStringPropertyDelegate(IntPtr info,
    [MarshalAs((UnmanagedType)48 /* LPUTF8Str */)] string key);

[UnmanagedFunctionPointer(CallingConvention.Winapi)]
public delegate long FlModel_InfoGetIntPropertyDelegate(IntPtr info,
    [MarshalAs((UnmanagedType)48 /* LPUTF8Str */)] string key, long defaultValue);

// -----------------------------------------------------------------------
// Vtable structs — marshalled from the native function pointer tables.
// Field order MUST match the C header exactly.
// -----------------------------------------------------------------------

/// <summary>Root API table returned by FoundryLocalGetApi().</summary>
[StructLayout(LayoutKind.Sequential)]
public struct FlApi
{
    // Status
    public FlApi_StatusCreateDelegate StatusCreate;
    public FlApi_StatusReleaseDelegate StatusRelease;
    public FlApi_StatusGetErrorCodeDelegate StatusGetErrorCode;
    public FlApi_StatusGetErrorMessageDelegate StatusGetErrorMessage;

    // Manager lifecycle
    public FlApi_ManagerCreateDelegate ManagerCreate;
    public FlApi_ManagerReleaseDelegate ManagerRelease;
    public FlApi_ManagerGetCatalogDelegate ManagerGetCatalog;
    public FlApi_ManagerWebServiceStartDelegate ManagerWebServiceStart;
    public FlApi_ManagerWebServiceUrlsDelegate ManagerWebServiceUrls;
    public FlApi_ManagerWebServiceStopDelegate ManagerWebServiceStop;

    // Sub-API accessors
    public FlApi_GetSubApiDelegate GetCatalogApi;
    public FlApi_GetSubApiDelegate GetConfigurationApi;
    public FlApi_GetSubApiDelegate GetItemApi;
    public FlApi_GetSubApiDelegate GetInferenceApi;
    public FlApi_GetSubApiDelegate GetModelApi;

    // KeyValuePairs
    public FlApi_CreateKeyValuePairsDelegate CreateKeyValuePairs;
    public FlApi_AddKeyValuePairDelegate AddKeyValuePair;
    public FlApi_GetKeyValueDelegate GetKeyValue;
    public FlApi_GetKeyValuePairsDelegate GetKeyValuePairs;
    public FlApi_RemoveKeyValuePairDelegate RemoveKeyValuePair;
    public FlApi_KeyValuePairsReleaseDelegate KeyValuePairsRelease;

    // ModelList
    public FlApi_ModelListReleaseDelegate ModelListRelease;
    public FlApi_ModelListSizeDelegate ModelListSize;
    public FlApi_ModelListGetAtDelegate ModelListGetAt;

    // EP detection
    public FlApi_ManagerGetDiscoverableEpsDelegate ManagerGetDiscoverableEps;
    public FlApi_ManagerDownloadAndRegisterEpsDelegate ManagerDownloadAndRegisterEps;
    public FlApi_ManagerIsEpDownloadInProgressDelegate ManagerIsEpDownloadInProgress;

    // Manager shutdown
    public FlApi_ManagerShutdownDelegate ManagerShutdown;
    public FlApi_ManagerIsShutdownRequestedDelegate ManagerIsShutdownRequested;
}

/// <summary>Item API table — field order MUST match flItemApi in foundry_local_c.h.</summary>
[StructLayout(LayoutKind.Sequential)]
public struct FlItemApi
{
    // Lifecycle
    public FlItem_CreateDelegate Create;
    public FlItem_ReleaseDelegate Release;

    // Type — 'new' hides inherited object.GetType(). Name must match C vtable layout.
    public new FlItem_GetTypeDelegate GetType;

    // Setters
    public FlItem_SetBytesDelegate SetBytes;
    public FlItem_SetTensorDelegate SetTensor;
    public FlItem_SetTextDelegate SetText;
    public FlItem_SetMessageDelegate SetMessage;
    public FlItem_SetImageDelegate SetImage;
    public FlItem_SetAudioDelegate SetAudio;
    public FlItem_SetToolCallDelegate SetToolCall;
    public FlItem_SetToolResultDelegate SetToolResult;

    // Getters
    public FlItem_GetBytesDelegate GetBytes;
    public FlItem_GetTextDelegate GetText;
    public FlItem_GetMessageDelegate GetMessage;
    public FlItem_GetTensorDelegate GetTensor;
    public FlItem_GetImageDelegate GetImage;
    public FlItem_GetAudioDelegate GetAudio;
    public FlItem_GetToolCallDelegate GetToolCall;
    public FlItem_GetToolResultDelegate GetToolResult;
    public FlItem_GetSpeechSegmentDelegate GetSpeechSegment;
    public FlItem_GetSpeechResultDelegate GetSpeechResult;

    // Metadata
    public FlItem_GetMetadataDelegate GetMetadata;
    public FlItem_GetMetadataDelegate GetMutableMetadata;

    // Queue item
    public FlItem_GetQueueDelegate GetQueue;

    // ItemQueue
    public FlItem_QueueCreateDelegate QueueCreate;
    public FlItem_QueueReleaseDelegate QueueRelease;
    public FlItem_QueuePushDelegate QueuePush;
    public FlItem_QueueTryPopDelegate QueueTryPop;
    public FlItem_QueueSizeDelegate QueueSize;
    public FlItem_QueueMarkFinishedDelegate QueueMarkFinished;
    public FlItem_QueueGetFinishedDelegate QueueGetFinished;
}

/// <summary>Inference API table — request, response, session.</summary>
[StructLayout(LayoutKind.Sequential)]
public struct FlInferenceApi
{
    // Request
    public FlInference_RequestCreateDelegate RequestCreate;
    public FlInference_RequestReleaseDelegate RequestRelease;
    public FlInference_RequestAddItemDelegate RequestAddItem;
    public FlInference_RequestGetItemCountDelegate RequestGetItemCount;
    public FlInference_RequestGetItemDelegate RequestGetItem;
    public FlInference_RequestSetOptionsDelegate RequestSetOptions;
    public FlInference_RequestCancelDelegate RequestCancel;

    // Response
    public FlInference_ResponseCreateDelegate ResponseCreate;
    public FlInference_ResponseReleaseDelegate ResponseRelease;
    public FlInference_ResponseGetItemCountDelegate ResponseGetItemCount;
    public FlInference_ResponseGetItemDelegate ResponseGetItem;
    public FlInference_ResponseGetFinishReasonDelegate ResponseGetFinishReason;
    public FlInference_ResponseGetUsageDelegate ResponseGetUsage;

    // Session
    public FlInference_SessionCreateDelegate SessionCreate;
    public FlInference_SessionReleaseDelegate SessionRelease;
    public FlInference_SessionSetStreamingCallbackDelegate SessionSetStreamingCallback;
    public FlInference_SessionSetOptionsDelegate SessionSetOptions;
    public FlInference_SessionProcessRequestDelegate SessionProcessRequest;

    // Chat session features
    public FlInference_SessionAddToolDefinitionDelegate SessionAddToolDefinition;
    public FlInference_SessionRemoveToolDefinitionDelegate SessionRemoveToolDefinition;
    public FlInference_SessionGetTurnCountDelegate SessionGetTurnCount;
    public FlInference_SessionUndoTurnsDelegate SessionUndoTurns;
}

/// <summary>Configuration API table.</summary>
[StructLayout(LayoutKind.Sequential)]
public struct FlConfigurationApi
{
    public FlConfig_CreateDelegate Create;
    public FlConfig_ReleaseDelegate Release;
    public FlConfig_SetDefaultLogLevelDelegate SetDefaultLogLevel;
    public FlConfig_SetStringDelegate SetAppDataDir;
    public FlConfig_SetStringDelegate SetLogsDir;
    public FlConfig_SetStringDelegate SetModelCacheDir;
    public FlConfig_AddCatalogUrlDelegate AddCatalogUrl;
    public FlConfig_SetStringDelegate SetCatalogRegion;
    public FlConfig_AddWebServiceEndpointDelegate AddWebServiceEndpoint;
    public FlConfig_SetExternalServiceUrlDelegate SetExternalServiceUrl;
    public FlConfig_SetAdditionalOptionsDelegate SetAdditionalOptions;
}

/// <summary>Catalog API table.</summary>
[StructLayout(LayoutKind.Sequential)]
public struct FlCatalogApi
{
    public FlCatalog_GetNameDelegate GetName;
    public FlCatalog_GetModelsDelegate GetModels;
    public FlCatalog_GetModelDelegate GetModel;
    public FlCatalog_GetModelVariantDelegate GetModelVariant;
    public FlCatalog_GetLatestVersionDelegate GetLatestVersion;
    public FlCatalog_GetCachedModelsDelegate GetCachedModels;
    public FlCatalog_GetLoadedModelsDelegate GetLoadedModels;
    public FlCatalog_GetModelVersionsDelegate GetModelVersions;
}

/// <summary>Model API table — model operations and ModelInfo accessors.</summary>
[StructLayout(LayoutKind.Sequential)]
public struct FlModelApi
{
    // ModelInfo (GetInfo first — matches C header order)
    public FlModel_GetInfoDelegate GetInfo;

    // Model operations
    public FlModel_GetInputOutputInfoDelegate GetInputOutputInfo;
    public FlModel_IsCachedDelegate IsCached;
    public FlModel_GetPathDelegate GetPath;
    public FlModel_DownloadDelegate Download;
    public FlModel_IsLoadedDelegate IsLoaded;
    public FlModel_LoadDelegate Load;
    public FlModel_UnloadDelegate Unload;
    public FlModel_RemoveFromCacheDelegate RemoveFromCache;

    // Variant support
    public FlModel_GetVariantsDelegate GetVariants;
    public FlModel_SelectVariantDelegate SelectVariant;

    // ModelInfo accessors
    public FlModel_InfoGetStringDelegate InfoGetId;
    public FlModel_InfoGetStringDelegate InfoGetName;
    public FlModel_InfoGetVersionDelegate InfoGetVersion;
    public FlModel_InfoGetStringDelegate InfoGetAlias;
    public FlModel_InfoGetStringDelegate InfoGetUri;
    public FlModel_InfoGetDeviceTypeDelegate InfoGetDeviceType;
    public FlModel_InfoGetStringDelegate InfoGetExecutionProvider;
    public FlModel_InfoGetStringDelegate InfoGetTask;
    public FlModel_InfoGetKvpDelegate InfoGetPromptTemplates;
    public FlModel_InfoGetKvpDelegate InfoGetModelSettings;
    public FlModel_InfoGetStringPropertyDelegate InfoGetStringProperty;
    public FlModel_InfoGetIntPropertyDelegate InfoGetIntProperty;
}

// -----------------------------------------------------------------------
// Well-known property key constants
// -----------------------------------------------------------------------

public static class ModelProperties
{
    // String properties
    public const string DisplayName = "display_name";
    public const string ModelType = "type";
    public const string Publisher = "publisher";
    public const string License = "license";
    public const string LicenseDescription = "license_description";
    public const string Task = "task";
    public const string ModelProvider = "model_provider";
    public const string MinFlVersion = "min_fl_version";
    public const string ParentUri = "parent_uri";
    public const string ToolCallStart = "tool_call_start";
    public const string ToolCallEnd = "tool_call_end";
    public const string ReasoningStart = "reasoning_start";
    public const string ReasoningEnd = "reasoning_end";

    // Int properties
    public const string SupportsToolCalling = "supports_tool_calling";
    public const string SupportsReasoning = "supports_reasoning";
    public const string FileSizeMb = "filesize_mb";
    public const string MaxOutputTokens = "max_output_tokens";
    public const string CreatedAtUnix = "created_at_unix";
    public const string IsTestModel = "is_test_model";
}

public static class Parameters
{
    // Sampling / generation
    public const string Temperature = "temperature";
    public const string TopP = "top_p";
    public const string TopK = "top_k";
    public const string MaxOutputTokens = "max_output_tokens";
    public const string Stop = "stop";
    public const string FrequencyPenalty = "frequency_penalty";
    public const string PresencePenalty = "presence_penalty";
    public const string Seed = "seed";
    public const string EarlyStopping = "early_stopping";

    // Tool calling
    public const string ToolChoice = "tool_choice";

    // Output format
    public const string ResponseFormat = "response_format";

    // Conversation state
    public const string PreviousResponseId = "previous_response_id";
    public const string Instructions = "instructions";
    public const string Store = "store";

    // Reasoning
    public const string ReasoningEffort = "reasoning_effort";
}
