// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local;

using System.Runtime.InteropServices;

using Microsoft.AI.Foundry.Local.Detail.Interop;
using Microsoft.AI.Foundry.Local.Detail.Native;

public sealed class AudioItem : Item
{
    private IntPtr _data;
    private int _dataSize;
    private PinContext? _pinContext;  // held for borrowed path; null for owned/URI/native

    // Static deleter thunk — created once, never collected
    private static readonly FlAudioDataDeleterDelegate s_deleter = AudioDeleter;
    private static readonly IntPtr s_deleterPtr = Marshal.GetFunctionPointerForDelegate(s_deleter);

    /// <summary>Raw audio bytes. Only valid while this item is alive. Empty for URI-based items.</summary>
    public ReadOnlySpan<byte> Data
    {
        get
        {
            unsafe { return new ReadOnlySpan<byte>((void*)_data, _dataSize); }
        }
    }

    public string? Format { get; private set; }
    public string? Uri { get; private set; }
    public int SampleRate { get; private set; }
    public int Channels { get; private set; }

    /// <summary>Create from a URI (file path, URL, etc.).</summary>
    public AudioItem(string uri, string? format = null) : base(ItemType.Audio)
    {
        try
        {
            Uri = uri;
            Format = format;
            SampleRate = 0;
            Channels = 0;
            _data = IntPtr.Zero;
            _dataSize = 0;

            var uriNative = Detail.Utf8.StringToCoTaskMem(uri);
            var formatNative = format != null ? Detail.Utf8.StringToCoTaskMem(format) : IntPtr.Zero;

            try
            {
                var audioData = new FlAudioData
                {
                    Version = NativeMethods.ApiVersion,
                    Data = IntPtr.Zero,
                    MutableData = IntPtr.Zero,
                    DataSize = UIntPtr.Zero,
                    Format = formatNative,
                    Uri = uriNative,
                    SampleRate = 0,
                    Channels = 0,
                    Deleter = IntPtr.Zero,
                    DeleterUserData = IntPtr.Zero,
                };
                Api.CheckStatus(Api.Item.SetAudio(Ptr, ref audioData));
            }
            finally
            {
                Marshal.FreeCoTaskMem(uriNative);

                if (formatNative != IntPtr.Zero)
                {
                    Marshal.FreeCoTaskMem(formatNative);
                }
            }
        }
        catch
        {
            DisposeOnConstructionFailure();
            throw;
        }
    }

    /// <summary>
    /// Create from read-only borrowed data. The item pins the memory until disposed.
    /// </summary>
    public AudioItem(string format, ReadOnlyMemory<byte> data) : base(ItemType.Audio)
    {
        try
        {
            Format = format;
            Uri = null;

            _pinContext = PinContext.Pin(data);
            SetNativeAudio(_pinContext.Pointer, _pinContext.Length, mutableData: IntPtr.Zero, format);

            _data = _pinContext.Pointer;
            _dataSize = _pinContext.Length;
        }
        catch
        {
            _pinContext?.Dispose();
            _pinContext = null;
            DisposeOnConstructionFailure();
            throw;
        }
    }

    /// <summary>
    /// Create from mutable borrowed data. The item pins the memory until disposed.
    /// Native code may write into the buffer.
    /// </summary>
    public AudioItem(string format, Memory<byte> data) : base(ItemType.Audio)
    {
        try
        {
            Format = format;
            Uri = null;

            _pinContext = PinContext.Pin(data);
            SetNativeAudio(_pinContext.Pointer, _pinContext.Length, mutableData: _pinContext.Pointer, format);

            _data = _pinContext.Pointer;
            _dataSize = _pinContext.Length;
        }
        catch
        {
            _pinContext?.Dispose();
            _pinContext = null;
            DisposeOnConstructionFailure();
            throw;
        }
    }

    /// <summary>
    /// Create from read-only owned data. The native item takes ownership via a pin that
    /// survives past Dispose(). Safe for ItemQueue push and ownership transfer.
    /// </summary>
    /// <remarks>
    /// The C ABI requires <c>mutable_data</c> to be non-NULL when a deleter is set, so the
    /// payload is defensively copied into a freshly-allocated <see cref="byte"/>[] that we own.
    /// This avoids handing the native side a writable pointer to memory the caller intended
    /// to be read-only (which could be backed by interned / read-only segments).
    /// </remarks>
    public static AudioItem CreateOwned(string format, ReadOnlyMemory<byte> data)
    {
        var copy = data.ToArray();
        return CreateOwned(format, (Memory<byte>)copy);
    }

    /// <summary>
    /// Create from mutable owned data. The native item takes ownership via a pin that
    /// survives past Dispose(). Safe for ItemQueue push and ownership transfer.
    /// </summary>
    public static AudioItem CreateOwned(string format, Memory<byte> data)
    {
        var item = new AudioItem(ItemType.Audio);
        var userData = PinContext.PinForNativeDeleter(data, out var dataPtr, out var dataSize);

        try
        {
            item.Format = format;
            item.SetNativeAudioOwned(dataPtr, dataSize, dataPtr, format, s_deleterPtr, userData);
            item._data = dataPtr;
            item._dataSize = dataSize;
            return item;
        }
        catch
        {
            // Native never received userData — release the GCHandle / pin ourselves to avoid leaks.
            PinContext.ReleaseFromNative(userData);
            item.DisposeOnConstructionFailure();
            throw;
        }
    }

    /// <summary>
    /// Create from read-only owned data with explicit sample rate and channel count.
    /// </summary>
    /// <remarks>
    /// See <see cref="CreateOwned(string, ReadOnlyMemory{byte})"/> for why a defensive copy is taken.
    /// </remarks>
    public static AudioItem CreateOwned(string format, ReadOnlyMemory<byte> data, int sampleRate, int channels)
    {
        var copy = data.ToArray();
        return CreateOwned(format, (Memory<byte>)copy, sampleRate, channels);
    }

    /// <summary>
    /// Create from mutable owned data with explicit sample rate and channel count.
    /// </summary>
    public static AudioItem CreateOwned(string format, Memory<byte> data, int sampleRate, int channels)
    {
        var item = new AudioItem(ItemType.Audio);
        var userData = PinContext.PinForNativeDeleter(data, out var dataPtr, out var dataSize);

        try
        {
            item.Format = format;
            item.SampleRate = sampleRate;
            item.Channels = channels;
            item.SetNativeAudioOwned(dataPtr, dataSize, dataPtr, format, s_deleterPtr, userData,
                                     sampleRate, channels);
            item._data = dataPtr;
            item._dataSize = dataSize;
            return item;
        }
        catch
        {
            PinContext.ReleaseFromNative(userData);
            item.DisposeOnConstructionFailure();
            throw;
        }
    }

    /// <summary>
    /// Create a format-descriptor AudioItem with no data, useful for specifying audio
    /// parameters (e.g., PCM format with sample rate and channels) for live streaming sessions.
    /// </summary>
    public static AudioItem CreateFormatDescriptor(string format, int sampleRate, int channels)
    {
        var item = new AudioItem(ItemType.Audio);

        try
        {
            item.Format = format;
            item.SampleRate = sampleRate;
            item.Channels = channels;
            item._data = IntPtr.Zero;
            item._dataSize = 0;

            item.SetNativeAudio(IntPtr.Zero, 0, IntPtr.Zero, format, sampleRate, channels);

            return item;
        }
        catch
        {
            item.DisposeOnConstructionFailure();
            throw;
        }
    }

    /// <summary>Wrap an existing native audio item (from Response, Queue, etc.).</summary>
    internal AudioItem(IntPtr ptr, bool ownsHandle) : base(ptr, ownsHandle)
    {
        var status = Api.Item.GetAudio(Ptr, out var audio);
        Api.CheckStatus(status);
        _data = audio.Data;
        _dataSize = (int)(ulong)audio.DataSize;
        Format = Detail.Utf8.PtrToString(audio.Format);
        Uri = Detail.Utf8.PtrToString(audio.Uri);
        SampleRate = audio.SampleRate;
        Channels = audio.Channels;
    }

    // Private constructor for static factories — creates native item but sets no data yet
    private AudioItem(ItemType type) : base(type)
    {
    }

    private void SetNativeAudio(IntPtr dataPtr, int dataSize, IntPtr mutableData, string format,
                                 int sampleRate = 0, int channels = 0)
    {
        var formatNative = Detail.Utf8.StringToCoTaskMem(format);

        try
        {
            var audioData = new FlAudioData
            {
                Version = NativeMethods.ApiVersion,
                Data = dataPtr,
                MutableData = mutableData,
                DataSize = (UIntPtr)dataSize,
                Format = formatNative,
                Uri = IntPtr.Zero,
                SampleRate = sampleRate,
                Channels = channels,
                Deleter = IntPtr.Zero,
                DeleterUserData = IntPtr.Zero,
            };
            Api.CheckStatus(Api.Item.SetAudio(Ptr, ref audioData));
        }
        finally
        {
            Marshal.FreeCoTaskMem(formatNative);
        }
    }

    private void SetNativeAudioOwned(IntPtr dataPtr, int dataSize, IntPtr mutableData,
                                     string format, IntPtr deleter, IntPtr deleterUserData,
                                     int sampleRate = 0, int channels = 0)
    {
        var formatNative = Detail.Utf8.StringToCoTaskMem(format);

        try
        {
            var audioData = new FlAudioData
            {
                Version = NativeMethods.ApiVersion,
                Data = dataPtr,
                MutableData = mutableData,
                DataSize = (UIntPtr)dataSize,
                Format = formatNative,
                Uri = IntPtr.Zero,
                SampleRate = sampleRate,
                Channels = channels,
                Deleter = deleter,
                DeleterUserData = deleterUserData,
            };
            Api.CheckStatus(Api.Item.SetAudio(Ptr, ref audioData));
        }
        finally
        {
            Marshal.FreeCoTaskMem(formatNative);
        }
    }

    private static void AudioDeleter(ref FlAudioData data, IntPtr userData)
    {
        PinContext.ReleaseFromNative(userData);
    }

    protected override void OnDisposing()
    {
        _pinContext?.Dispose();
        _pinContext = null;
    }
}
