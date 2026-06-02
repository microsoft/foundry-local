// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

using Microsoft.AI.Foundry.Local.Detail.Native;

namespace Microsoft.AI.Foundry.Local;

/// <summary>
/// A queue of items used for streaming. Items can be pushed and popped.
/// </summary>
/// <remarks>
/// Mirrors the C++ <c>class ItemQueue : Item</c> — a queue <i>is</i> an item (carries item type tag
/// <see cref="ItemType.Queue"/>). The base <see cref="Item.Ptr"/> stores the owning <c>flItem*</c>; the
/// inner <c>flItemQueue*</c> needed by the queue-specific ABI calls is fetched on demand via
/// <c>Api.Item.GetQueue</c>. The inherited dispose path (<c>Item_Release</c>) is polymorphic on the
/// type tag and correctly destroys queues, so no override is needed.
/// </remarks>
public sealed class ItemQueue : Item
{
    // The inner flItemQueue* is stable for the item's lifetime, so resolve it once at construction
    // rather than calling Item_GetQueue on every Push/TryPop/Count/etc. (each call is a P/Invoke).
    private readonly IntPtr _queuePtr;

    public ItemQueue() : base(ItemType.Queue)
    {
        Api.CheckStatus(Api.Item.GetQueue(Ptr, out _queuePtr));
    }

    internal ItemQueue(IntPtr ptr, bool ownsHandle) : base(ptr, ownsHandle)
    {
        Api.CheckStatus(Api.Item.GetQueue(Ptr, out _queuePtr));
    }

    /// <summary>Push an item into the queue. Transfers ownership of the item.</summary>
    public void Push(Item item)
    {
        Detail.Throw.IfNull(item);
        var nativePtr = item.ReleaseOwnership();
        Api.CheckStatus(Api.Item.QueuePush(_queuePtr, nativePtr));
    }

    /// <summary>Try to pop an item from the queue. Returns null if queue is empty.</summary>
    public Item? TryPop()
    {
        if (Api.Item.QueueTryPop(_queuePtr, out var itemPtr))
        {
            return Item.FromNative(itemPtr, ownsHandle: true);
        }

        return null;
    }

    public int Count => checked((int)(ulong)Api.Item.QueueSize(_queuePtr));

    public void MarkFinished() => Api.Item.QueueMarkFinished(_queuePtr);

    public bool IsFinished => Api.Item.QueueGetFinished(_queuePtr);
}
