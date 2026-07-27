// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local;

/// <summary>
/// Convenience wrapper for an OpenAI REST JSON payload (chat completions or audio transcription
/// request/response). Internally a <see cref="TextItem"/> with <see cref="TextItemType.OpenAIJson"/>.
///
/// <para><b>Outbound only.</b> The native runtime returns OpenAI JSON payloads as plain
/// <see cref="ItemType.Text"/> items with <see cref="TextItemType.OpenAIJson"/>; there is no
/// distinct native item type for JSON, so <see cref="Item.FromNative"/> will never produce a
/// <see cref="JsonItem"/>. Inspect <see cref="TextItem.Type"/> to detect a returned OpenAI JSON
/// payload from a <see cref="TextItem"/> view.</para>
/// </summary>
public sealed class JsonItem : TextItem
{
    public string Json => Text;

    public JsonItem(string json) : base(json, TextItemType.OpenAIJson)
    {
    }

    internal JsonItem(IntPtr ptr, bool ownsHandle) : base(ptr, ownsHandle)
    {
    }
}
