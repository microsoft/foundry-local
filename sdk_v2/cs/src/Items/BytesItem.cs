// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local;

using System.Runtime.InteropServices;

using Microsoft.AI.Foundry.Local.Detail.Interop;
using Microsoft.AI.Foundry.Local.Detail.Native;

public sealed class BytesItem : Item
{
    private IntPtr _data;
    private int _dataSize;
    private PinContext? _pinContext;

    // Static deleter thunk — created once, never collected
    private static readonly FlBytesDataDeleterDelegate s_deleter = BytesDeleter;
    private static readonly IntPtr s_deleterPtr = Marshal.GetFunctionPointerForDelegate(s_deleter);

    /// <summary>Raw bytes. Only valid while this item is alive.</summary>
    public ReadOnlySpan<byte> Data
    {
        get
        {
            unsafe { return new ReadOnlySpan<byte>((void*)_data, _dataSize); }
        }
    }

    /// <summary>Create from read-only borrowed data. The item pins the memory until disposed.</summary>
    public BytesItem(ReadOnlyMemory<byte> data) : base(ItemType.Bytes)
    {
        try
        {
            _pinContext = PinContext.Pin(data);
            SetNativeBytes(_pinContext.Pointer, _pinContext.Length, mutableData: IntPtr.Zero);

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

    /// <summary>Create from mutable borrowed data. The item pins the memory until disposed.</summary>
    public BytesItem(Memory<byte> data) : base(ItemType.Bytes)
    {
        try
        {
            _pinContext = PinContext.Pin(data);
            SetNativeBytes(_pinContext.Pointer, _pinContext.Length, mutableData: _pinContext.Pointer);

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

    /// <summary>Create from read-only owned data. Safe for ItemQueue push.</summary>
    /// <remarks>
    /// The C ABI requires <c>mutable_data</c> to be non-NULL when a deleter is set, so the
    /// payload is defensively copied into a freshly-allocated <see cref="byte"/>[] that we own.
    /// </remarks>
    public static BytesItem CreateOwned(ReadOnlyMemory<byte> data)
    {
        var copy = data.ToArray();
        return CreateOwned((Memory<byte>)copy);
    }

    /// <summary>Create from mutable owned data. Safe for ItemQueue push.</summary>
    public static BytesItem CreateOwned(Memory<byte> data)
    {
        var item = new BytesItem(ItemType.Bytes);
        var userData = PinContext.PinForNativeDeleter(data, out var dataPtr, out var dataSize);

        try
        {
            item.SetNativeBytesOwned(dataPtr, dataSize, dataPtr, s_deleterPtr, userData);
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

    /// <summary>Wrap an existing native bytes item (from Response, Queue, etc.).</summary>
    internal BytesItem(IntPtr ptr, bool ownsHandle) : base(ptr, ownsHandle)
    {
        var status = Api.Item.GetBytes(Ptr, out var bytes);
        Api.CheckStatus(status);
        _data = bytes.Data;
        _dataSize = (int)(ulong)bytes.DataSize;
    }

    // Private constructor for static factories — creates native item but sets no data yet
    private BytesItem(ItemType type) : base(type)
    {
    }

    private void SetNativeBytes(IntPtr dataPtr, int dataSize, IntPtr mutableData)
    {
        var bytesData = new FlBytesData
        {
            Version = NativeMethods.ApiVersion,
            ItemType = FlItemType.Bytes,
            Data = dataPtr,
            MutableData = mutableData,
            DataSize = (UIntPtr)dataSize,
            Deleter = IntPtr.Zero,
            DeleterUserData = IntPtr.Zero,
        };
        Api.CheckStatus(Api.Item.SetBytes(Ptr, ref bytesData));
    }

    private void SetNativeBytesOwned(IntPtr dataPtr, int dataSize, IntPtr mutableData,
                                     IntPtr deleter, IntPtr deleterUserData)
    {
        var bytesData = new FlBytesData
        {
            Version = NativeMethods.ApiVersion,
            ItemType = FlItemType.Bytes,
            Data = dataPtr,
            MutableData = mutableData,
            DataSize = (UIntPtr)dataSize,
            Deleter = deleter,
            DeleterUserData = deleterUserData,
        };
        Api.CheckStatus(Api.Item.SetBytes(Ptr, ref bytesData));
    }

    private static void BytesDeleter(ref FlBytesData data, IntPtr userData)
    {
        PinContext.ReleaseFromNative(userData);
    }

    protected override void OnDisposing()
    {
        _pinContext?.Dispose();
        _pinContext = null;
    }
}
