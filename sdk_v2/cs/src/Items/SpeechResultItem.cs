// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

using System.Collections.Generic;
using System.Runtime.InteropServices;

using Microsoft.AI.Foundry.Local.Detail;
using Microsoft.AI.Foundry.Local.Detail.Interop;
using Microsoft.AI.Foundry.Local.Detail.Native;

namespace Microsoft.AI.Foundry.Local;

/// <summary>
/// Output-only item produced by <see cref="AudioSession"/> as the final aggregate of a
/// transcription request. Holds the full transcript and per-segment detail.
/// </summary>
public sealed class SpeechResultItem : Item
{
    /// <summary>Concatenated final transcript (UTF-8). Empty when nothing was recognized.</summary>
    public string Text { get; }

    /// <summary>Detected source language. Null when not reported by the model.</summary>
    public string? Language { get; }

    /// <summary>Total audio duration. Null when not reported by the model.</summary>
    public long? DurationMs { get; }

    /// <summary>
    /// Per-segment detail. Entries are <see cref="SpeechSegmentItem"/>s with
    /// <see cref="SpeechSegmentKind.Final"/> or <see cref="SpeechSegmentKind.None"/>.
    /// The segments are owned by this result item — do not dispose them individually.
    /// </summary>
    public IReadOnlyList<SpeechSegmentItem> Segments { get; }

    internal SpeechResultItem(IntPtr ptr, bool ownsHandle) : base(ptr, ownsHandle)
    {
        var status = Api.Item.GetSpeechResult(Ptr, out var data);
        Api.CheckStatus(status);

        Text = Utf8.PtrToString(data.Text) ?? string.Empty;
        Language = Utf8.PtrToString(data.Language);
        DurationMs = data.DurationMs == FlSpeech.DurationUnset ? null : data.DurationMs;
        Segments = ReadSegments(data.Segments, (int)data.SegmentsCount.ToUInt32());
    }

    private static SpeechSegmentItem[] ReadSegments(IntPtr arr, int count)
    {
        if (arr == IntPtr.Zero || count == 0)
        {
            return Array.Empty<SpeechSegmentItem>();
        }

        var segs = new SpeechSegmentItem[count];
        for (int i = 0; i < count; i++)
        {
            var segPtr = Marshal.ReadIntPtr(arr, i * IntPtr.Size);
            // Borrowed handle — owned by the parent SpeechResultItem.
            segs[i] = new SpeechSegmentItem(segPtr, ownsHandle: false);
        }
        return segs;
    }
}
