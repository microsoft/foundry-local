// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local;
using System.Runtime.InteropServices;

using Microsoft.AI.Foundry.Local.Detail.Interop;
using Microsoft.AI.Foundry.Local.Detail.Native;

public class TextItem : Item
{
    public string Text { get; }

    public TextItemType Type { get; }

    public TextItem(string text) : this(text, TextItemType.Default)
    {
    }

    public TextItem(string text, TextItemType type) : base(ItemType.Text)
    {
        Text = text;
        Type = type;
        SetNative(text, type);
    }

    /// <summary>
    /// Convenience factory for an OpenAI REST JSON payload (chat completions, audio
    /// transcription, embeddings request/response). Equivalent to
    /// <c>new TextItem(json, TextItemType.OpenAIJson)</c>.
    /// </summary>
    public static TextItem OpenAIJson(string json) => new(json, TextItemType.OpenAIJson);

    internal TextItem(IntPtr ptr, bool ownsHandle) : base(ptr, ownsHandle)
    {
        var data = new FlTextData { Version = NativeMethods.ApiVersion };
        Api.CheckStatus(Api.Item.GetText(Ptr, out data));
        Text = Detail.Utf8.PtrToString(data.Text) ?? string.Empty;
        Type = (TextItemType)data.Type;
    }

    private void SetNative(string text, TextItemType type)
    {
        var textNative = Detail.Utf8.StringToCoTaskMem(text);

        try
        {
            var data = new FlTextData
            {
                Version = NativeMethods.ApiVersion,
                Text = textNative,
                Type = (FlTextItemType)type,
            };
            Api.CheckStatus(Api.Item.SetText(Ptr, ref data));
        }
        finally
        {
            Marshal.FreeCoTaskMem(textNative);
        }
    }
}
