// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

using Microsoft.AI.Foundry.Local.Detail.Native;

namespace Microsoft.AI.Foundry.Local;

public sealed class Request : IDisposable
{
    internal IntPtr Ptr { get; private set; }
    private bool _disposed;

    public Request()
    {
        Api.EnsureInitialized();
        var status = Api.Inference.RequestCreate(out var ptr);
        Api.CheckStatus(status);
        Ptr = ptr;
    }

    /// <summary>
    /// Add an item to the request.
    /// </summary>
    /// <param name="item">The item to add. Must not be null.</param>
    /// <param name="takeOwnership">
    /// When <c>true</c> (default) the request takes ownership of <paramref name="item"/> and the caller must not
    /// use it afterwards. When <c>false</c> the caller retains ownership — required for an <see cref="ItemQueue"/>
    /// the caller continues to push into while the request is being processed, and useful for sharing a single
    /// input <see cref="Item"/> (e.g. a tensor) across multiple requests.
    /// </param>
    public Request AddItem(Item item, bool takeOwnership = true)
    {
        Detail.Throw.IfNull(item);
        Api.CheckStatus(Api.Inference.RequestAddItem(Ptr, item.Ptr, takeOwnership));

        if (takeOwnership)
        {
            item.ReleaseOwnership();
        }

        return this;
    }

    public int ItemCount => (int)(ulong)Api.Inference.RequestGetItemCount(Ptr);

    /// <summary>
    /// Returns a non-owning view of the item at <paramref name="index"/>. The returned
    /// <see cref="Item"/> wraps a native handle owned by this <see cref="Request"/>; it
    /// is valid only while this <see cref="Request"/> is alive. Do not call
    /// <see cref="Item.Dispose"/> on it, and do not retain or use it after the parent
    /// <see cref="Request"/> has been disposed.
    /// </summary>
    public Item GetItem(int index)
    {
        var status = Api.Inference.RequestGetItem(Ptr, (UIntPtr)index, out var itemPtr);
        Api.CheckStatus(status);
        return Item.FromNative(itemPtr, ownsHandle: false);
    }

    /// <summary>
    /// Set per-request inference options. Per-request options override session-level
    /// options for this request only.
    /// </summary>
    public Request SetOptions(RequestOptions options)
    {
        Detail.Throw.IfNull(options);

        Api.Root.CreateKeyValuePairs(out var kvpPtr);

        try
        {
            foreach (var kvp in options.ToDictionary())
            {
                Api.Root.AddKeyValuePair(kvpPtr, kvp.Key, kvp.Value);
            }

            Api.CheckStatus(Api.Inference.RequestSetOptions(Ptr, kvpPtr));
        }
        finally
        {
            Api.Root.KeyValuePairsRelease(kvpPtr);
        }

        return this;
    }

    internal Request SetOptions(IntPtr options)
    {
        Api.CheckStatus(Api.Inference.RequestSetOptions(Ptr, options));
        return this;
    }

    public void Cancel()
    {
        Api.CheckStatus(Api.Inference.RequestCancel(Ptr));
    }

    public void Dispose()
    {
        if (!_disposed && Ptr != IntPtr.Zero)
        {
            Api.Inference.RequestRelease(Ptr);
            Ptr = IntPtr.Zero;
            _disposed = true;
        }
    }
}
