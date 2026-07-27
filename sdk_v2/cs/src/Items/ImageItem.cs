// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local;

using System.Runtime.InteropServices;

using Microsoft.AI.Foundry.Local.Detail.Interop;
using Microsoft.AI.Foundry.Local.Detail.Native;

public sealed class ImageItem : Item
{
    private IntPtr _data;
    private int _dataSize;
    private PinContext? _pinContext;  // held for borrowed path; null for owned/URI/native

    // Static deleter thunk — created once, never collected
    private static readonly FlImageDataDeleterDelegate s_deleter = ImageDeleter;
    private static readonly IntPtr s_deleterPtr = Marshal.GetFunctionPointerForDelegate(s_deleter);

    /// <summary>Raw image bytes. Only valid while this item is alive. Empty for URI-based items.</summary>
    public ReadOnlySpan<byte> Data
    {
        get
        {
            unsafe { return new ReadOnlySpan<byte>((void*)_data, _dataSize); }
        }
    }

    public string? Format { get; private set; }
    public string? Uri { get; private set; }

    /// <summary>Create from raw bytes. format is e.g. "png".</summary>
    public ImageItem(string format, ReadOnlyMemory<byte> data) : base(ItemType.Image)
    {
        try
        {
            Format = format;
            Uri = null;

            _pinContext = PinContext.Pin(data);
            SetNativeImage(_pinContext.Pointer, _pinContext.Length, mutableData: IntPtr.Zero, format);

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
    public ImageItem(string format, Memory<byte> data) : base(ItemType.Image)
    {
        try
        {
            Format = format;
            Uri = null;

            _pinContext = PinContext.Pin(data);
            SetNativeImage(_pinContext.Pointer, _pinContext.Length, mutableData: _pinContext.Pointer, format);

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

    /// <summary>Create from a URI (file path, URL, etc.).</summary>
    public ImageItem(string uri, string? format = null) : base(ItemType.Image)
    {
        try
        {
            Uri = uri;
            Format = format;
            _data = IntPtr.Zero;
            _dataSize = 0;

            var uriNative = Detail.Utf8.StringToCoTaskMem(uri);
            var formatNative = format != null ? Detail.Utf8.StringToCoTaskMem(format) : IntPtr.Zero;

            try
            {
                var imageData = new FlImageData
                {
                    Version = NativeMethods.ApiVersion,
                    Data = IntPtr.Zero,
                    MutableData = IntPtr.Zero,
                    DataSize = UIntPtr.Zero,
                    Format = formatNative,
                    Uri = uriNative,
                    Deleter = IntPtr.Zero,
                    DeleterUserData = IntPtr.Zero,
                };
                Api.CheckStatus(Api.Item.SetImage(Ptr, ref imageData));
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
    /// Create from read-only owned data. The native item takes ownership via a pin that
    /// survives past Dispose(). Safe for ItemQueue push and ownership transfer.
    /// </summary>
    /// <remarks>
    /// The C ABI requires <c>mutable_data</c> to be non-NULL when a deleter is set, so the
    /// payload is defensively copied into a freshly-allocated <see cref="byte"/>[] that we own.
    /// </remarks>
    public static ImageItem CreateOwned(string format, ReadOnlyMemory<byte> data)
    {
        var copy = data.ToArray();
        return CreateOwned(format, (Memory<byte>)copy);
    }

    /// <summary>
    /// Create from mutable owned data. The native item takes ownership via a pin that
    /// survives past Dispose(). Safe for ItemQueue push and ownership transfer.
    /// </summary>
    public static ImageItem CreateOwned(string format, Memory<byte> data)
    {
        var item = new ImageItem(ItemType.Image);
        var userData = PinContext.PinForNativeDeleter(data, out var dataPtr, out var dataSize);

        try
        {
            item.Format = format;
            item.SetNativeImageOwned(dataPtr, dataSize, dataPtr, format, s_deleterPtr, userData);
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

    /// <summary>Wrap an existing native image item (from Response, Queue, etc.).</summary>
    internal ImageItem(IntPtr ptr, bool ownsHandle) : base(ptr, ownsHandle)
    {
        var status = Api.Item.GetImage(Ptr, out var image);
        Api.CheckStatus(status);
        _data = image.Data;
        _dataSize = (int)(ulong)image.DataSize;
        Format = Detail.Utf8.PtrToString(image.Format);
        Uri = Detail.Utf8.PtrToString(image.Uri);
    }

    // Private constructor for static factories — creates native item but sets no data yet
    private ImageItem(ItemType type) : base(type)
    {
    }

    private void SetNativeImage(IntPtr dataPtr, int dataSize, IntPtr mutableData, string format)
    {
        var formatNative = Detail.Utf8.StringToCoTaskMem(format);

        try
        {
            var imageData = new FlImageData
            {
                Version = NativeMethods.ApiVersion,
                Data = dataPtr,
                MutableData = mutableData,
                DataSize = (UIntPtr)dataSize,
                Format = formatNative,
                Uri = IntPtr.Zero,
                Deleter = IntPtr.Zero,
                DeleterUserData = IntPtr.Zero,
            };
            Api.CheckStatus(Api.Item.SetImage(Ptr, ref imageData));
        }
        finally
        {
            Marshal.FreeCoTaskMem(formatNative);
        }
    }

    private void SetNativeImageOwned(IntPtr dataPtr, int dataSize, IntPtr mutableData,
                                     string format, IntPtr deleter, IntPtr deleterUserData)
    {
        var formatNative = Detail.Utf8.StringToCoTaskMem(format);

        try
        {
            var imageData = new FlImageData
            {
                Version = NativeMethods.ApiVersion,
                Data = dataPtr,
                MutableData = mutableData,
                DataSize = (UIntPtr)dataSize,
                Format = formatNative,
                Uri = IntPtr.Zero,
                Deleter = deleter,
                DeleterUserData = deleterUserData,
            };
            Api.CheckStatus(Api.Item.SetImage(Ptr, ref imageData));
        }
        finally
        {
            Marshal.FreeCoTaskMem(formatNative);
        }
    }

    private static void ImageDeleter(ref FlImageData data, IntPtr userData)
    {
        PinContext.ReleaseFromNative(userData);
    }

    protected override void OnDisposing()
    {
        _pinContext?.Dispose();
        _pinContext = null;
    }
}
