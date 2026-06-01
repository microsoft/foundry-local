// Public response shape for `LiveAudioTranscriptionSession.getStream()`. Kept stable for source-compat with the
// V1 SDK surface. The native audio session currently only produces raw text, so `is_final` is always `true` and
// `content` always has a single part with both `text` and `transcript` set to that token. A future native change
// is expected to populate timing fields and partial/interim results.

/** A single content part within a transcription result. Mirrors the OpenAI Realtime API's `ContentPart` shape. */
export interface TranscriptionContentPart {
  /** The transcribed text. */
  text?: string | null;
  /** Alias of `text`, matching the OpenAI Realtime API's `ContentPart.transcript` field. */
  transcript?: string | null;
}

/**
 * A transcription result from a live-audio session. Shaped like the OpenAI Realtime API's `ConversationItem` so
 * callers access text via `result.content[0].text` or `result.content[0].transcript`.
 */
export interface LiveAudioTranscriptionResponse {
  /** Unique identifier for this result, if available. */
  id?: string | null;
  /** Whether this is a partial (interim) or final result for this segment. */
  is_final: boolean;
  /** The transcription content parts. */
  content: TranscriptionContentPart[];
  /** Start time offset of this segment in the audio stream (seconds), if available. */
  start_time?: number | null;
  /** End time offset of this segment in the audio stream (seconds), if available. */
  end_time?: number | null;
}
