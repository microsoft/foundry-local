// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local;

using System.Buffers;
using System.Runtime.InteropServices;

/// <summary>
/// Manages pinning of Memory/ReadOnlyMemory buffers for native interop.
/// Borrowed items hold a PinContext and dispose it. Owned items transfer
/// the PinContext to a GCHandle that the native deleter releases.
/// </summary>
internal sealed class PinContext : IDisposable
{
    private MemoryHandle _handle;

    /// <summary>Stable pointer to the pinned data.</summary>
    public IntPtr Pointer { get; }

    /// <summary>Byte count.</summary>
    public int Length { get; }

    private PinContext(MemoryHandle handle, IntPtr pointer, int length)
    {
        _handle = handle;
        Pointer = pointer;
        Length = length;
    }

    public static PinContext Pin(ReadOnlyMemory<byte> data)
    {
        var handle = data.Pin();
        IntPtr ptr;

        unsafe
        {
            ptr = (IntPtr)handle.Pointer;
        }

        return new PinContext(handle, ptr, data.Length);
    }

    public static PinContext Pin(Memory<byte> data)
        => Pin((ReadOnlyMemory<byte>)data);

    /// <summary>
    /// Pin <paramref name="data"/> and immediately transfer ownership of the pin to a GCHandle
    /// that the native deleter releases via <see cref="ReleaseFromNative"/>. Returns the IntPtr
    /// to pass as <c>deleter_user_data</c>, along with the stable pointer/length of the pinned data.
    /// </summary>
    /// <remarks>
    /// No <see cref="PinContext"/> escapes to the caller: it is created and rooted in the GCHandle
    /// entirely within this call, so the pin lives until the native item is destroyed. If rooting
    /// fails, the pin is released here so the buffer is never left pinned.
    /// </remarks>
    public static IntPtr PinForNativeDeleter(Memory<byte> data, out IntPtr pointer, out int length)
    {
        // IDisposableAnalyzers cannot model ownership that is handed to unmanaged code: the pin is
        // rooted in a GCHandle and released by a native deleter callback, neither of which the
        // analyzer can see. This is the single ownership-transfer boundary for all owned items, so
        // the suppression lives here once rather than being scattered across the item factories.
#pragma warning disable IDISP001 // Dispose created — ownership is transferred to the native deleter.
        var ctx = Pin(data);
#pragma warning restore IDISP001
        pointer = ctx.Pointer;
        length = ctx.Length;

        try
        {
            return AllocForNativeDeleter(ctx);
        }
        catch
        {
            ctx.Dispose();
            throw;
        }
    }

    /// <summary>
    /// Allocate a GCHandle so the native deleter can find this PinContext.
    /// Returns the IntPtr to pass as deleter_user_data.
    /// After this call, the PinContext is kept alive by the GCHandle.
    /// </summary>
    private static IntPtr AllocForNativeDeleter(PinContext ctx)
    {
        var gcHandle = GCHandle.Alloc(ctx, GCHandleType.Normal);
        return GCHandle.ToIntPtr(gcHandle);
    }

    /// <summary>
    /// Called by native deleter thunks. Unpins the memory and frees the GCHandle.
    /// </summary>
    public static void ReleaseFromNative(IntPtr userData)
    {
        var gcHandle = GCHandle.FromIntPtr(userData);
        var ctx = (PinContext)gcHandle.Target!;
        ctx._handle.Dispose();
        gcHandle.Free();
    }

    /// <summary>For borrowed path: unpin in Item.Dispose().</summary>
    public void Dispose()
    {
        _handle.Dispose();
    }
}
