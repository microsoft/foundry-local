// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local.Detail;

using System.Collections.Concurrent;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Threading;

using Microsoft.Extensions.Logging;

using static Microsoft.AI.Foundry.Local.Detail.ICoreInterop;

internal partial class CoreInterop : ICoreInterop
{
    // TODO: Android and iOS may need special handling. See ORT C# NativeMethods.shared.cs
    internal const string LibraryName = "Microsoft.AI.Foundry.Local.Core";

    private readonly ILogger _logger;

#if NET5_0_OR_GREATER
    private static readonly bool IsWindows = OperatingSystem.IsWindows();
    private static readonly bool IsLinux = OperatingSystem.IsLinux();
    private static readonly bool IsMacOS = OperatingSystem.IsMacOS();
#else
    private static readonly bool IsWindows = RuntimeInformation.IsOSPlatform(OSPlatform.Windows);
    private static readonly bool IsLinux = RuntimeInformation.IsOSPlatform(OSPlatform.Linux);
    private static readonly bool IsMacOS = RuntimeInformation.IsOSPlatform(OSPlatform.OSX);
#endif

    private static IntPtr genaiLibHandle = IntPtr.Zero;
    private static IntPtr ortLibHandle = IntPtr.Zero;
    private static readonly NativeCallbackFn handleCallbackDelegate = HandleCallback;

    // Map of in-flight callback helpers keyed by an opaque integer ID we pass to native as `userData`.
    // Native must never see a raw managed pointer: if it fires a stale callback after ExecuteCommandImpl
    // has returned, the lookup misses and we no-op instead of dereferencing freed memory.
    private static long nextCallbackId;
    private static readonly ConcurrentDictionary<long, CallbackHelper> callbackHelpers = new();

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private unsafe delegate void ExecuteCommandDelegate(RequestBuffer* req, ResponseBuffer* resp);

    internal class CallbackHelper
    {
        public CallbackFn Callback { get; }
        public Exception? Exception { get; set; }
        public CallbackHelper(CallbackFn callback)
        {
            Callback = callback ?? throw new ArgumentNullException(nameof(callback));
        }
    }

    static CoreInterop()
    {
        InitializeNativeLibraryResolver();
    }

    internal CoreInterop(Configuration config, ILogger logger)
    {
        _logger = logger ?? throw new ArgumentNullException(nameof(logger));

        var request = new CoreInteropRequest { Params = config.AsDictionary() };
        var response = ExecuteCommand("initialize", request);

        if (response.Error != null)
        {
            throw new FoundryLocalException($"Error initializing Foundry.Local.Core library: {response.Error}");
        }
        else
        {
            _logger.LogInformation("Foundry.Local.Core initialized successfully: {Response}", response.Data);
        }
    }

    /// <summary>For testing. Skips the 'initialize' command so assumes this has been done previously.</summary>
    internal CoreInterop(ILogger logger)
    {
        _logger = logger ?? throw new ArgumentNullException(nameof(logger));
    }

    // Implemented in CoreInterop.NetStandard.cs and CoreInterop.Modern.cs.
    static partial void InitializeNativeLibraryResolver();

    private static string AddLibraryExtension(string name) =>
        IsWindows ? $"{name}.dll" :
        IsLinux ? $"{name}.so" :
        IsMacOS ? $"{name}.dylib" :
        throw new PlatformNotSupportedException();

    // We need to manually load ORT and ORT GenAI dlls on Windows to ensure
    // a) we're using the libraries we think we are
    // b) that dependencies are resolved correctly as the dlls may not be in the default load path.
    // It's a 'Try' as we can't do anything else if it fails as the dlls may be available somewhere else.
    private static void LoadOrtDllsIfInSameDir(string path)
    {
        var genaiLibName = AddLibraryExtension("onnxruntime-genai");
        var ortLibName = AddLibraryExtension("onnxruntime");
        var genaiPath = Path.Combine(path, genaiLibName);
        var ortPath = Path.Combine(path, ortLibName);

        // Need to load ORT first as the winml GenAI library redirects and tries to load a winml onnxruntime.dll,
        // which will not have the EPs we expect/require. If/when we don't bundle our own onnxruntime.dll we need to
        // revisit this.
        var loadedOrt = TryLoadNativeLibrary(ortPath, out ortLibHandle);
        var loadedGenAI = TryLoadNativeLibrary(genaiPath, out genaiLibHandle);

        Debug.WriteLine($"Loaded ORT:{loadedOrt} handle={ortLibHandle}");
        Debug.WriteLine($"Loaded GenAI: {loadedGenAI} handle={genaiLibHandle}");
    }

    private static int HandleCallback(nint data, int length, nint callbackHelperId)
    {
        // `callbackHelperId` is an opaque integer ID, NOT a GCHandle pointer. If the producing call has
        // already returned and removed its entry, this lookup misses and we cancel cleanly.
        var id = (long)callbackHelperId;
        if (!callbackHelpers.TryGetValue(id, out var helper))
        {
            return 1; // late callback after ExecuteCommandImpl returned; cancel
        }

        var callbackData = string.Empty;
        try
        {
            if (data != IntPtr.Zero && length > 0)
            {
                var managedData = new byte[length];
                Marshal.Copy(data, managedData, 0, length);
                callbackData = System.Text.Encoding.UTF8.GetString(managedData);
            }

            helper.Callback.Invoke(callbackData);
            return 0; // continue
        }
        catch (OperationCanceledException ex)
        {
            helper.Exception ??= ex;
            return 1; // cancel
        }
        catch (Exception ex)
        {
            FoundryLocalManager.Instance.Logger.LogError(ex, $"Error in callback. Callback data: {callbackData}");
            helper.Exception ??= ex;
            return 1; // cancel on error
        }
    }

    public Response ExecuteCommandImpl(string commandName, string? commandInput,
                                       CallbackFn? callback = null)
    {
        try
        {
            byte[] commandBytes = System.Text.Encoding.UTF8.GetBytes(commandName);
            IntPtr commandPtr = Marshal.AllocHGlobal(commandBytes.Length);
            Marshal.Copy(commandBytes, 0, commandPtr, commandBytes.Length);

            byte[]? inputBytes = null;
            IntPtr? inputPtr = null;

            if (commandInput != null)
            {
                inputBytes = System.Text.Encoding.UTF8.GetBytes(commandInput);
                inputPtr = Marshal.AllocHGlobal(inputBytes.Length);
                Marshal.Copy(inputBytes, 0, inputPtr.Value, inputBytes.Length);
            }

            var request = new RequestBuffer
            {
                Command = commandPtr,
                CommandLength = commandBytes.Length,
                Data = inputPtr ?? IntPtr.Zero,
                DataLength = inputBytes?.Length ?? 0
            };

            ResponseBuffer response = default;
            Exception? callbackException = null;

            if (callback != null)
            {
                // We pass an opaque integer ID (not a GCHandle pointer) as `userData` so a late callback
                // from native after this call returns becomes a harmless lookup miss instead of a
                // use-after-free of a freed GCHandle.

                var helper = new CallbackHelper(callback);
                var funcPtr = Marshal.GetFunctionPointerForDelegate(handleCallbackDelegate);
                var helperId = Interlocked.Increment(ref nextCallbackId);
                callbackHelpers[helperId] = helper;

                try
                {
                    unsafe
                    {
                        CoreExecuteCommandWithCallback(&request, &response, funcPtr, (nint)helperId);
                    }
                }
                finally
                {
                    callbackHelpers.TryRemove(helperId, out _);
                }

                callbackException = helper.Exception;
            }
            else
            {
                unsafe
                {
                    CoreExecuteCommand(&request, &response);
                }
            }

            Response result = new();

            // Marshal response. Will have either Data or Error populated. Not both.
            if (response.Data != IntPtr.Zero && response.DataLength > 0)
            {
                byte[] managedResponse = new byte[response.DataLength];
                Marshal.Copy(response.Data, managedResponse, 0, response.DataLength);
                result.Data = System.Text.Encoding.UTF8.GetString(managedResponse);
                _logger.LogDebug($"Command: {commandName} succeeded.");
            }

            if (response.Error != IntPtr.Zero && response.ErrorLength > 0)
            {
                result.Error = PtrToStringUtf8(response.Error, response.ErrorLength);
                _logger.LogDebug($"Input:{commandInput ?? "null"}");
                _logger.LogDebug($"Command: {commandName} Error: {result.Error}");
            }

            // TODO: Validate this works. C# specific. Attempting to avoid calling free_response to do this
            Marshal.FreeHGlobal(response.Data);
            Marshal.FreeHGlobal(response.Error);

            Marshal.FreeHGlobal(commandPtr);
            if (commandInput != null)
            {
                Marshal.FreeHGlobal(inputPtr!.Value);
            }

            if (callbackException != null)
            {
                if (callbackException is OperationCanceledException canceledException)
                {
                    throw canceledException;
                }

                throw new FoundryLocalException("Exception in callback handler. See InnerException for details",
                                                callbackException);
            }

            return result;
        }
        catch (Exception ex) when (ex is not OperationCanceledException)
        {
            var msg = $"Error executing command '{commandName}' with input {commandInput ?? "null"}";
            throw new FoundryLocalException(msg, ex, _logger);
        }
    }

    public Response ExecuteCommand(string commandName, CoreInteropRequest? commandInput = null)
    {
        var commandInputJson = commandInput?.ToJson();
        return ExecuteCommandImpl(commandName, commandInputJson);
    }

    public Response ExecuteCommandWithCallback(string commandName, CoreInteropRequest? commandInput,
                                               CallbackFn callback)
    {
        var commandInputJson = commandInput?.ToJson();
        return ExecuteCommandImpl(commandName, commandInputJson, callback);
    }

    public Task<Response> ExecuteCommandAsync(string commandName, CoreInteropRequest? commandInput = null,
                                              CancellationToken? cancellationToken = null)
    {
        var ct = cancellationToken ?? CancellationToken.None;
        return Task.Run(() => ExecuteCommand(commandName, commandInput), ct);
    }

    public Task<Response> ExecuteCommandWithCallbackAsync(string commandName, CoreInteropRequest? commandInput,
                                                          CallbackFn callback,
                                                          CancellationToken? cancellationToken = null)
    {
        var ct = cancellationToken ?? CancellationToken.None;
        return Task.Run(() => ExecuteCommandWithCallback(commandName, commandInput, callback), ct);
    }

    /// <summary>
    /// Marshal a ResponseBuffer from unmanaged memory into a managed Response and free the unmanaged memory.
    /// </summary>
    private Response MarshalResponse(ResponseBuffer response)
    {
        Response result = new();

        if (response.Data != IntPtr.Zero && response.DataLength > 0)
        {
            byte[] managedResponse = new byte[response.DataLength];
            Marshal.Copy(response.Data, managedResponse, 0, response.DataLength);
            result.Data = System.Text.Encoding.UTF8.GetString(managedResponse);
        }

        if (response.Error != IntPtr.Zero && response.ErrorLength > 0)
        {
            result.Error = PtrToStringUtf8(response.Error, response.ErrorLength);
        }

        Marshal.FreeHGlobal(response.Data);
        Marshal.FreeHGlobal(response.Error);

        return result;
    }

    public Response StartAudioStream(CoreInteropRequest request)
    {
        return ExecuteCommand("audio_stream_start", request);
    }

    public Response PushAudioData(CoreInteropRequest request, ReadOnlyMemory<byte> audioData)
    {
        try
        {
            var commandInputJson = request.ToJson();
            byte[] commandBytes = System.Text.Encoding.UTF8.GetBytes("audio_stream_push");
            byte[] inputBytes = System.Text.Encoding.UTF8.GetBytes(commandInputJson);

            IntPtr commandPtr = Marshal.AllocHGlobal(commandBytes.Length);
            Marshal.Copy(commandBytes, 0, commandPtr, commandBytes.Length);

            IntPtr inputPtr = Marshal.AllocHGlobal(inputBytes.Length);
            Marshal.Copy(inputBytes, 0, inputPtr, inputBytes.Length);

            // Pin the managed audio data so GC won't move it during the native call
            using var audioHandle = audioData.Pin();

            unsafe
            {
                var reqBuf = new StreamingRequestBuffer
                {
                    Command = commandPtr,
                    CommandLength = commandBytes.Length,
                    Data = inputPtr,
                    DataLength = inputBytes.Length,
                    BinaryData = (nint)audioHandle.Pointer,
                    BinaryDataLength = audioData.Length
                };

                ResponseBuffer response = default;

                try
                {
                    CoreExecuteCommandWithBinary(&reqBuf, &response);
                }
                finally
                {
                    Marshal.FreeHGlobal(commandPtr);
                    Marshal.FreeHGlobal(inputPtr);
                }

                return MarshalResponse(response);
            }
        }
        catch (Exception ex) when (ex is not OperationCanceledException)
        {
            throw new FoundryLocalException("Error executing audio_stream_push", ex, _logger);
        }
    }

    public Response StopAudioStream(CoreInteropRequest request)
    {
        return ExecuteCommand("audio_stream_stop", request);
    }

    private static string PtrToStringUtf8(IntPtr ptr, int length)
    {
#if NETSTANDARD2_0
        byte[] buffer = new byte[length];
        Marshal.Copy(ptr, buffer, 0, length);
        return System.Text.Encoding.UTF8.GetString(buffer);
#else
        return Marshal.PtrToStringUTF8(ptr, length)!;
#endif
    }
}
