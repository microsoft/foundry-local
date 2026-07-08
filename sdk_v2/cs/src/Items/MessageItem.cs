// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local;
using System.Runtime.InteropServices;

using Microsoft.AI.Foundry.Local.Detail.Interop;
using Microsoft.AI.Foundry.Local.Detail.Native;

/// <summary>
/// Chat message with role, typed content parts, and optional participant name.
///
/// <para>Most messages are plain text and use the single-string convenience constructor.
/// Multi-modal callers (e.g. vision input) construct with an explicit list of parts
/// (<see cref="TextItem"/> / <see cref="ImageItem"/> / <see cref="AudioItem"/>).
/// Other item types are rejected at the C ABI boundary.</para>
///
/// <para>Lifetime model: <see cref="MessageItem"/> takes ownership of its parts and
/// disposes them when itself is disposed. The native runtime borrows the part handles
/// during request processing and deep-copies any retained messages (e.g. into chat
/// history), so the borrowed views become independent of this MessageItem's lifetime
/// once handed to the runtime.</para>
/// </summary>
public sealed class MessageItem : Item
{
    public MessageRole Role { get; }
    public string? Name { get; }
    public IReadOnlyList<Item> Parts => _parts;

    private readonly List<Item> _parts;
    private readonly bool _ownsParts;

    /// <summary>Create a system message.</summary>
    public static MessageItem System(string content, string? name = null)
        => new(MessageRole.System, content, name);

    /// <summary>Create a user message.</summary>
    public static MessageItem User(string content, string? name = null)
        => new(MessageRole.User, content, name);

    /// <summary>Create an assistant message.</summary>
    public static MessageItem Assistant(string content, string? name = null)
        => new(MessageRole.Assistant, content, name);

    /// <summary>Create a developer message.</summary>
    public static MessageItem Developer(string content, string? name = null)
        => new(MessageRole.Developer, content, name);

    /// <summary>
    /// Single-text convenience constructor. Wraps <paramref name="content"/> in a
    /// <see cref="TextItem"/> part. Throws if <paramref name="content"/> is empty.
    /// </summary>
    public MessageItem(MessageRole role, string content, string? name = null)
        : base(ItemType.Message)
    {
        try
        {
            if (string.IsNullOrEmpty(content))
            {
                throw new ArgumentException("MessageItem requires non-empty text", nameof(content));
            }

            Role = role;
            Name = name;

            var textPart = new TextItem(content);
            _parts = new List<Item> { textPart };
            _ownsParts = true;

            SetNativeMessage(role, _parts, name);
        }
        catch
        {
            if (_parts is not null && _ownsParts)
            {
                foreach (var part in _parts)
                {
                    part.Dispose();
                }
            }

            DisposeOnConstructionFailure();
            throw;
        }
    }

    /// <summary>
    /// Multi-part constructor. Takes ownership of the supplied parts; they are disposed
    /// when this MessageItem is disposed. Throws if <paramref name="parts"/> is empty
    /// or contains a null entry.
    /// </summary>
    public MessageItem(MessageRole role, IEnumerable<Item> parts, string? name = null)
        : base(ItemType.Message)
    {
        try
        {
            Detail.Throw.IfNull(parts);

            Role = role;
            Name = name;

            _parts = new List<Item>();
            foreach (var part in parts)
            {
                if (part is null)
                {
                    throw new ArgumentException("MessageItem content part must not be null", nameof(parts));
                }

                _parts.Add(part);
            }

            if (_parts.Count == 0)
            {
                throw new ArgumentException("MessageItem requires at least one content part", nameof(parts));
            }

            _ownsParts = true;
            SetNativeMessage(role, _parts, name);
        }
        catch
        {
            if (_parts is not null && _ownsParts)
            {
                foreach (var part in _parts)
                {
                    part.Dispose();
                }
            }

            DisposeOnConstructionFailure();
            throw;
        }
    }

    /// <summary>
    /// Wraps a native message handle. Parts read from the native side are non-owning views
    /// whose lifetime is tied to the underlying native message item, not to the wrappers
    /// returned here.
    /// </summary>
    internal MessageItem(IntPtr ptr, bool ownsHandle) : base(ptr, ownsHandle)
    {
        var status = Api.Item.GetMessage(Ptr, out var message);
        Api.CheckStatus(status);

        Role = (MessageRole)message.Role;
        Name = Detail.Utf8.PtrToString(message.Name);

        var count = (int)(ulong)message.ContentItemsCount;
        _parts = new List<Item>(count);

        if (count > 0 && message.ContentItems != IntPtr.Zero)
        {
            var partHandles = new IntPtr[count];
            Marshal.Copy(message.ContentItems, partHandles, 0, count);
            foreach (var partPtr in partHandles)
            {
                if (partPtr == IntPtr.Zero)
                {
                    continue;
                }

                _parts.Add(Item.FromNative(partPtr, ownsHandle: false));
            }
        }

        _ownsParts = false;
    }

    /// <summary>
    /// True when the message has exactly one part and it is a <see cref="TextItem"/>.
    /// </summary>
    public bool IsSimpleText() => _parts.Count == 1 && _parts[0] is TextItem;

    /// <summary>
    /// Text of the single <see cref="TextItem"/> part. Throws if <see cref="IsSimpleText"/>
    /// is false.
    /// </summary>
    public string GetSimpleText()
    {
        if (!IsSimpleText())
        {
            throw new InvalidOperationException("MessageItem is not a single TextItem");
        }

        return ((TextItem)_parts[0]).Text;
    }

    protected override void OnDisposing()
    {
        if (_ownsParts)
        {
            foreach (var part in _parts)
            {
                part.Dispose();
            }
        }
    }

    private void SetNativeMessage(MessageRole role, List<Item> parts, string? name)
    {
        var partHandles = new IntPtr[parts.Count];
        for (var i = 0; i < parts.Count; i++)
        {
            partHandles[i] = parts[i].Ptr;
        }

        var partsHandle = GCHandle.Alloc(partHandles, GCHandleType.Pinned);
        var nameNative = name != null ? Detail.Utf8.StringToCoTaskMem(name) : IntPtr.Zero;

        try
        {
            var data = new FlMessageData
            {
                Version = NativeMethods.ApiVersion,
                Role = (FlMessageRole)role,
                ContentItems = partsHandle.AddrOfPinnedObject(),
                ContentItemsCount = checked((UIntPtr)(ulong)partHandles.Length),
                Name = nameNative,
            };
            Api.CheckStatus(Api.Item.SetMessage(Ptr, ref data));
        }
        finally
        {
            partsHandle.Free();

            if (nameNative != IntPtr.Zero)
            {
                Marshal.FreeCoTaskMem(nameNative);
            }
        }
    }
}
