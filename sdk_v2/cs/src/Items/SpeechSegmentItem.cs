// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

using System.Collections.Generic;
using System.Runtime.InteropServices;

using Microsoft.AI.Foundry.Local.Detail;
using Microsoft.AI.Foundry.Local.Detail.Interop;
using Microsoft.AI.Foundry.Local.Detail.Native;

namespace Microsoft.AI.Foundry.Local;

/// <summary>One word within a <see cref="SpeechSegmentItem"/>. Output-only.</summary>
public sealed record SpeechWord(
    string Text,
    long? StartTimeMs,
    long? EndTimeMs,
    float? Confidence,
    string? SpeakerId);

/// <summary>
/// Output-only item produced by <see cref="AudioSession"/>. A streaming callback receives
/// zero-or-more PARTIAL segments followed by exactly one FINAL closing the segment.
/// Entries of a <see cref="SpeechResultItem.Segments"/> use NONE or FINAL.
/// </summary>
public sealed class SpeechSegmentItem : Item
{
    public SpeechSegmentKind Kind { get; }

    /// <summary>UTF-8 text. For PARTIAL: the cumulative current hypothesis, not a delta.</summary>
    public string Text { get; }

    public long? StartTimeMs { get; }
    public long? EndTimeMs { get; }

    /// <summary>True on the first segment of a new utterance.</summary>
    public bool UtteranceStart { get; }

    public IReadOnlyList<SpeechWord> Words { get; }

    /// <summary>Per-segment language for code-switching. Null when not reported.</summary>
    public string? Language { get; }

    internal SpeechSegmentItem(IntPtr ptr, bool ownsHandle) : base(ptr, ownsHandle)
    {
        var status = Api.Item.GetSpeechSegment(Ptr, out var data);
        Api.CheckStatus(status);

        Kind = (SpeechSegmentKind)data.Kind;
        Text = Utf8.PtrToString(data.Text) ?? string.Empty;
        StartTimeMs = data.StartTimeMs == FlSpeech.DurationUnset ? null : data.StartTimeMs;
        EndTimeMs = data.EndTimeMs == FlSpeech.DurationUnset ? null : data.EndTimeMs;
        UtteranceStart = data.UtteranceStart;
        Language = Utf8.PtrToString(data.Language);
        Words = ReadWords(data.Words, (int)data.WordsCount.ToUInt32());
    }

    private static SpeechWord[] ReadWords(IntPtr arr, int count)
    {
        if (arr == IntPtr.Zero || count == 0)
        {
            return Array.Empty<SpeechWord>();
        }

        var size = Marshal.SizeOf<FlSpeechWord>();
        var words = new SpeechWord[count];
        for (int i = 0; i < count; i++)
        {
            var w = Marshal.PtrToStructure<FlSpeechWord>(arr + (i * size));
            words[i] = new SpeechWord(
                Text: Utf8.PtrToString(w.Text) ?? string.Empty,
                StartTimeMs: w.StartTimeMs == FlSpeech.DurationUnset ? null : w.StartTimeMs,
                EndTimeMs: w.EndTimeMs == FlSpeech.DurationUnset ? null : w.EndTimeMs,
                Confidence: w.Confidence == FlSpeech.ConfidenceUnset ? null : w.Confidence,
                SpeakerId: Utf8.PtrToString(w.SpeakerId));
        }
        return words;
    }
}
