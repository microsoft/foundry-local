// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

// Suppress IDisposableAnalyzers warnings for the Item base:
// - IDISP023: Dispose accesses static Api which is always available
// - IDISP025: Subclasses are sealed with no extra native resources — virtual dispose not needed
#pragma warning disable IDISP023
#pragma warning disable IDISP025

namespace Microsoft.AI.Foundry.Local;
using Microsoft.AI.Foundry.Local.Detail.Interop;
using Microsoft.AI.Foundry.Local.Detail.Native;

public class Item : IDisposable
{
    internal IntPtr Ptr { get; private set; }
    private bool _ownsHandle;
    private bool _disposed;

    internal Item(IntPtr ptr, bool ownsHandle)
    {
        Ptr = ptr;
        _ownsHandle = ownsHandle;
    }

    protected Item(ItemType type)
    {
        Api.EnsureInitialized();
        var status = Api.Item.Create((FlItemType)type, out var ptr);
        Api.CheckStatus(status);
        Ptr = ptr;
        _ownsHandle = true;
    }

    public ItemType ItemType => (ItemType)Api.Item.GetType(Ptr);

    internal static Item FromNative(IntPtr ptr, bool ownsHandle)
    {
        var type = (ItemType)Api.Item.GetType(ptr);

        return type switch
        {
            ItemType.Bytes => new BytesItem(ptr, ownsHandle),
            ItemType.Text => new TextItem(ptr, ownsHandle),
            ItemType.Message => new MessageItem(ptr, ownsHandle),
            ItemType.Image => new ImageItem(ptr, ownsHandle),
            ItemType.Audio => new AudioItem(ptr, ownsHandle),
            ItemType.SpeechSegment => new SpeechSegmentItem(ptr, ownsHandle),
            ItemType.SpeechResult => new SpeechResultItem(ptr, ownsHandle),
            ItemType.ToolCall => new ToolCallItem(ptr, ownsHandle),
            ItemType.ToolResult => new ToolResultItem(ptr, ownsHandle),
            ItemType.Tensor => new TensorItem(ptr, ownsHandle),
            ItemType.Queue => new ItemQueue(ptr, ownsHandle),
            _ => new Item(ptr, ownsHandle),
        };
    }

    /// <summary>
    /// Transfer ownership to the caller (e.g., before adding to a Request).
    /// After this call the item will no longer release the native handle.
    /// </summary>
    internal IntPtr ReleaseOwnership()
    {
        _ownsHandle = false;
        return Ptr;
    }

    public void Dispose()
    {
        if (!_disposed)
        {
            // Invoke the OnDisposing hook BEFORE releasing the native handle so subclasses
            // can still touch the native side (e.g. release child handles, copy data out)
            // while their native pointer is still valid.
            try
            {
                OnDisposing();
            }
            finally
            {
                if (_ownsHandle && Ptr != IntPtr.Zero)
                {
                    Api.Item.Release(Ptr);
                    Ptr = IntPtr.Zero;
                }

                _disposed = true;
            }
        }

        GC.SuppressFinalize(this);
    }

    // Finalizer: last-resort native handle release if Dispose() was never called.
    // Intentionally does NOT invoke OnDisposing (which is allowed to touch managed
    // state). Skip on runtime shutdown, where the native DLL may already be gone.
    ~Item()
    {
        if (!_disposed && _ownsHandle)
        {
            Api.FinalizeRelease(Ptr, Api.Item.Release.Invoke);
        }
    }

    /// <summary>
    /// Release the native handle without invoking <see cref="OnDisposing"/>. Intended for
    /// derived constructors to call from a catch block when initialization throws after
    /// <c>base(ItemType)</c> has allocated the handle but subclass state has not yet been
    /// fully initialized (so <see cref="OnDisposing"/> would be unsafe to run).
    /// </summary>
    private protected void DisposeOnConstructionFailure()
    {
        if (!_disposed)
        {
            if (_ownsHandle && Ptr != IntPtr.Zero)
            {
                Api.Item.Release(Ptr);
                Ptr = IntPtr.Zero;
            }

            _disposed = true;
        }
    }

    /// <summary>
    /// Called during Dispose <b>before</b> the native handle is released. Override in
    /// subclasses to release additional resources (e.g., unpin borrowed memory). The
    /// native <see cref="Ptr"/> is still valid for the duration of this call.
    /// </summary>
    protected virtual void OnDisposing()
    {
    }
}
