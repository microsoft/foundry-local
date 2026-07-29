# Speech Output Types — Design

Native SDK types returned by `AudioSession` for speech-to-text (file and live),
translation, and ASR scenarios. References: OpenAI `verbose_json` / Realtime
transcription events, Azure Speech SDK recognition results.

## Design rules

- **Output-only types.** These types are produced by `AudioSession` and flow out
  through the streaming callback and the final `Response`. Callers never
  construct them as inputs. The C ABI therefore exposes only Get accessors —
  no Set functions, no `Item_Create` for these types.
- **One set of types covers transcription, translation, and ASR.** Task
  selection (transcribe vs translate, target language) is a Request parameter,
  not a type variant. `text` is the recognized-or-translated string either way.
- **One shared segment type** for both streaming events and final-result entries,
  discriminated by `kind`.
- **No event wrapper, no `event_id`, no segment `id`.** Ordering is a property of
  the callback channel; segment identity is implicit in stream order (zero-or-more
  `kPartial` for the current segment, then one `kFinal` closes it). A web service
  above the SDK can add envelope/sequence metadata.
- **`text` on `kPartial` is the cumulative current hypothesis for the segment**,
  not a delta-since-last-event (Azure-style). A delta is recoverable by diffing
  against the previous hypothesis.
- **`utterance_start` is a boolean on the segment.** Knowable at emission time
  (VAD says "speech started" → producer tags the first `kPartial` of the new
  segment). There is no `utterance_end` field: end-of-utterance can't be known
  when the `kFinal` is emitted without delaying it by the silence threshold.
  Instead, end is implicit — the next `utterance_start` marks it (consumer
  infers end at the previous `kFinal.end_time`), a future `kSilence` event
  marks it explicitly, or the final `SpeechResult` marks it for file
  transcription.
- **Time as `int64_t` milliseconds.** Must survive the C ABI, so time fields are
  plain `int64_t` carrying an explicit `_ms` suffix. The ABI cannot carry
  `std::optional`, so absent values use the `FOUNDRY_LOCAL_DURATION_UNSET`
  (`INT64_MIN`) sentinel; the C++ wrapper converts these back to
  `std::optional<int64_t>`.
- **Two C ABI item types** — one for streaming segments, one for the final
  aggregate. Both additive to existing items.

## Types

The types exist in three layers: SDK-internal item classes, the C ABI contract
(`foundry_local_c.h`), and the C++ wrapper "content" views
(`foundry_local_cpp.h`). The C ABI is the canonical contract.

### C ABI (canonical contract)

```c
#define FOUNDRY_LOCAL_DURATION_UNSET INT64_MIN  // sentinel for absent time fields

// Sentinel for absent confidence. -FLT_MAX (not an in-range value like -1.0f) so it never
// collides with a legitimate score if "confidence" is ever a non-probability metric.
#define FOUNDRY_LOCAL_CONFIDENCE_UNSET (-FLT_MAX)

typedef enum flSpeechSegmentKind {
  FOUNDRY_LOCAL_SPEECH_SEGMENT_NONE    = 0,  // entry in a final aggregate result
  FOUNDRY_LOCAL_SPEECH_SEGMENT_PARTIAL = 1,  // streaming: hypothesis; may change
  FOUNDRY_LOCAL_SPEECH_SEGMENT_FINAL   = 2,  // streaming: stable, or final-result entry
} flSpeechSegmentKind;

typedef struct flSpeechWord {
  uint32_t version;
  const char* text;        // always populated
  int64_t start_time_ms;   // FOUNDRY_LOCAL_DURATION_UNSET if absent
  int64_t end_time_ms;     // FOUNDRY_LOCAL_DURATION_UNSET if absent
  float confidence;        // 0..1, FOUNDRY_LOCAL_CONFIDENCE_UNSET (-FLT_MAX) if absent
  const char* speaker_id;  // NULL if absent
} flSpeechWord;

typedef struct flSpeechSegmentData {
  uint32_t version;
  flSpeechSegmentKind kind;
  const char* text;           // PARTIAL: cumulative current hypothesis; may be NULL/""
  int64_t start_time_ms;      // UNSET if absent
  int64_t end_time_ms;        // UNSET if absent
  bool utterance_start;       // true on first PARTIAL of a new utterance
  const flSpeechWord* words;  // borrowed; length = words_count
  size_t words_count;
  const char* language;       // per-segment, for code-switching; NULL if absent
} flSpeechSegmentData;

typedef struct flSpeechResultData {
  uint32_t version;
  const char* text;               // concatenated final transcript; may be NULL/""
  const char* language;           // detected source language; NULL if absent
  int64_t duration_ms;            // total audio duration; UNSET if absent
  const flItem* const* segments;  // borrowed SPEECH_SEGMENT items; length = segments_count
  size_t segments_count;
} flSpeechResultData;
```

### C++ wrapper (`foundry_local_cpp.h`)

Optionals replace sentinels; borrowed C strings become `std::string_view`.

```cpp
namespace fl {

struct SpeechWord {
  std::string_view text;
  std::optional<int64_t> start_time_ms;
  std::optional<int64_t> end_time_ms;
  std::optional<float> confidence;            // 0..1
  std::optional<std::string_view> speaker_id;
};

struct SpeechSegmentContent {
  flSpeechSegmentKind kind;
  std::string_view text;                      // PARTIAL: cumulative current hypothesis
  std::optional<int64_t> start_time_ms;
  std::optional<int64_t> end_time_ms;
  bool utterance_start;
  std::vector<SpeechWord> words;
  std::optional<std::string_view> language;   // per-segment, for code-switching
};

struct SpeechResultContent {
  std::string_view text;                      // concatenated final transcript
  std::optional<std::string_view> language;   // detected source language
  std::optional<int64_t> duration_ms;         // total audio duration
  std::vector<Item> segments;                 // SPEECH_SEGMENT items, kind FINAL or NONE
};

}  // namespace fl
```

Internally these are backed by `SpeechSegmentItem` and `SpeechResultItem` (both
derive from `Item`); a `SpeechResultItem` owns its child `SpeechSegmentItem`s.
Accessors snapshot field values into the cached C ABI struct via `Finalize()`
before exposure.

## C ABI item types

```c
FOUNDRY_LOCAL_ITEM_SPEECH_SEGMENT = 31,  // pushed via streaming callback
FOUNDRY_LOCAL_ITEM_SPEECH_RESULT  = 32,  // final aggregate in response.items
```

These types are output-only — the ABI exposes `GetSpeechSegment` /
`GetSpeechResult` accessors, but no setters and no `Item_Create` support.
Attempting to create one returns `FOUNDRY_LOCAL_ERROR_INVALID_USAGE`.

`TextItem` remains the trivial fallback for `response_format: "text"`.

## V1 scope

The current producer (`AudioSession` over ORT GenAI — Whisper, Nemotron
streaming) emits one segment **per decoded token, all with kind `NONE`**.
Today's models surface only decoded tokens with no hypothesis-revision signal,
so `PARTIAL` / `FINAL` and `utterance_start` are part of the contract but not
yet produced. `NONE` is the honest label until a segmenting ASR exists.

Populated today:

- `SpeechSegmentItem`: `kind` (always `NONE`), `text` (one token)
- `SpeechResultItem`: `text` (concatenated transcript), `segments`

Defined in the contract but intentionally unset by the current producer:

- segment: `start_time_ms`, `end_time_ms`, `utterance_start`, `words`,
  `language`
- result: `language`, `duration_ms` — GenAI reports neither a detected source
  language nor audio duration; the request-side language is only a hint
- `PARTIAL` / `FINAL` segment kinds
- `SpeechWord` fields (`text` timings, `confidence`, `speaker_id`)

## Growth headroom (not built)

- **Diarization**: `speaker_id` already present on `flSpeechWord`.
- **N-best alternatives**: future `alternatives` array on the segment struct,
  appended as a V2 field after the existing trailer.
- **Per-segment diagnostics** (Whisper `avg_logprob`, `no_speech_prob`,
  `compression_ratio`; multi-channel `channel`; etc.): pushed as a separate
  diagnostic item type rather than overloading the segment struct.
- **OpenAI `verbose_json` compatibility**: handled by a
  `ToOpenAIVerboseJson(const SpeechResult&)` adapter in
  `contracts/audio_transcriptions.*`, not by changing native types.

Multi-target translation in a single pass is intentionally out of scope —
that's a server-side concern, not a local-inferencing one.
