// Discriminated-union Item shape used on both sides of the JS<->native
// boundary. The native layer accepts plain JS objects of this shape on
// `Request.addItem` and returns objects of this shape from `Session.send`
// (in `response.output`). No native handle is kept on the JS side — each
// `Item` is a fully-copied plain object owned by the JS GC.
//
// Mirrors the C++ wrapper's `Item` hierarchy (TextItem, MessageItem,
// BytesItem, TensorItem, ImageItem, AudioItem, ToolCallItem, ToolResultItem)
// from `sdk_v2/cpp/include/foundry_local/foundry_local_cpp.h`.

/** Role for a chat message. */
export type MessageRole = "system" | "user" | "assistant" | "tool" | "developer";

/** Sub-type of a text item. `"default"` is plain text. */
export type TextItemKind = "default" | "reasoning" | "openai-json";

/** Tensor element type. */
export type TensorDataType =
  | "float"
  | "uint8"
  | "int8"
  | "uint16"
  | "int16"
  | "int32"
  | "int64"
  | "string"
  | "bool"
  | "float16"
  | "double"
  | "uint32"
  | "uint64"
  | "unknown";

export interface TextItem {
  readonly type: "text";
  /** UTF-8 text content. */
  readonly text: string;
  /** Item kind. Defaults to `"default"` when constructed via `Item.text`. */
  readonly textType?: TextItemKind;
}

export interface MessageItem {
  readonly type: "message";
  readonly role: MessageRole;
  /**
   * Convenience: when the message contains a single default text part, the
   * native layer also surfaces it as `content`. Inputs may set either
   * `content` (treated as a single text part) or `parts`; not both.
   */
  readonly content?: string;
  readonly parts?: ReadonlyArray<TextItem | ImageItem | AudioItem>;
}

export interface BytesItem {
  readonly type: "bytes";
  readonly data: Uint8Array;
}

export interface TensorItem {
  readonly type: "tensor";
  readonly dataType: TensorDataType;
  readonly shape: ReadonlyArray<number>;
  /** Raw element bytes (size = product(shape) * elemSize(dataType)). */
  readonly data: Uint8Array;
}

export interface ImageItem {
  readonly type: "image";
  /** Source URI for the image (file://, https://, or data:). Mutually exclusive with `data`. */
  readonly uri?: string;
  /** Pre-decoded raw bytes. Mutually exclusive with `uri`. Requires `format` when used. */
  readonly data?: Uint8Array;
  /** Codec / container hint (e.g. "png", "jpeg"). Mirrors the C++ wrapper's `format`. */
  readonly format?: string;
}

export interface AudioItem {
  readonly type: "audio";
  readonly uri?: string;
  readonly data?: Uint8Array;
  /** Codec hint (e.g. "wav", "mp3"). Required when constructing from `data`. */
  readonly format?: string;
  readonly sampleRate?: number;
  readonly channels?: number;
}

export interface ToolCallItem {
  readonly type: "toolCall";
  readonly callId: string;
  readonly name: string;
  /** Arguments JSON string. Mirrors the C++ wrapper's `arguments`. */
  readonly arguments: string;
}

export interface ToolResultItem {
  readonly type: "toolResult";
  readonly callId: string;
  readonly result: string;
}

/**
 * Discriminator for a {@link SpeechSegmentItem}. PARTIAL/FINAL describe the state of the current segment
 * hypothesis in a streaming callback — PARTIAL is the evolving guess for the in-progress segment, FINAL closes
 * it. They do not describe the overall response. NONE is used for segments embedded in the aggregate
 * {@link SpeechResultItem}, where the streaming distinction no longer applies.
 */
export type SpeechSegmentKind = "none" | "partial" | "final";

/** One word inside a {@link SpeechSegmentItem}. Timing, confidence, and speaker fields are omitted when unset. */
export interface SpeechWord {
  readonly text: string;
  readonly startTimeMs?: number;
  readonly endTimeMs?: number;
  readonly confidence?: number;
  readonly speakerId?: string;
}

/**
 * Recognized/translated speech segment from an `AudioSession` (output-only). In a streaming callback, `text`
 * for a PARTIAL segment is the cumulative current hypothesis for the segment, not a delta. As an entry of a
 * {@link SpeechResultItem}, `kind` is `"final"` (or `"none"` for a single non-segmented transcript).
 */
export interface SpeechSegmentItem {
  readonly type: "speechSegment";
  readonly kind: SpeechSegmentKind;
  readonly text: string;
  readonly startTimeMs?: number;
  readonly endTimeMs?: number;
  readonly utteranceStart: boolean;
  readonly words: ReadonlyArray<SpeechWord>;
  readonly language?: string;
}

/**
 * Final aggregate transcription from an `AudioSession` (output-only). `segments` carries the per-segment
 * breakdown owned by the result; each entry's `kind` is `"final"` or `"none"`.
 */
export interface SpeechResultItem {
  readonly type: "speechResult";
  readonly text: string;
  readonly language?: string;
  readonly durationMs?: number;
  readonly segments: ReadonlyArray<SpeechSegmentItem>;
}

/**
 * Tagged union of all item shapes the JS surface understands. Outputs from
 * `Session.send` are always one of these; inputs to `Request.addItem` accept
 * the same shapes. Raw-bytes inputs (bytes/tensor/image-from-data/
 * audio-from-data) are pinned zero-copy — the source `Uint8Array` is read
 * directly by the native layer and must remain unmodified until the owning
 * `Request` is dropped. See the per-factory JSDoc for the full contract.
 */
export type Item =
  | TextItem
  | MessageItem
  | BytesItem
  | TensorItem
  | ImageItem
  | AudioItem
  | ToolCallItem
  | ToolResultItem
  | SpeechSegmentItem
  | SpeechResultItem;

/**
 * Factory helpers for building Item objects with the right shape. Mirrors
 * the `Items::` static helpers in the C++ wrapper. `Item` here is
 * declaration-merged with the `Item` discriminated-union type above — so
 * `Item` is usable as a type (`function f(x: Item)`) and as a value
 * (`Item.text("hi")`).
 */
export const Item = Object.freeze({
  text(text: string, textType: TextItemKind = "default"): TextItem {
    return { type: "text", text, textType };
  },
  message(role: MessageRole, contentOrParts: string | ReadonlyArray<TextItem | ImageItem | AudioItem>): MessageItem {
    if (typeof contentOrParts === "string") {
      return { type: "message", role, content: contentOrParts };
    }
    return { type: "message", role, parts: contentOrParts };
  },
  systemMessage(content: string): MessageItem {
    return { type: "message", role: "system", content };
  },
  userMessage(content: string | ReadonlyArray<TextItem | ImageItem | AudioItem>): MessageItem {
    return typeof content === "string"
      ? { type: "message", role: "user", content }
      : { type: "message", role: "user", parts: content };
  },
  assistantMessage(content: string): MessageItem {
    return { type: "message", role: "assistant", content };
  },
  developerMessage(content: string): MessageItem {
    return { type: "message", role: "developer", content };
  },
  toolCall(callId: string, name: string, argumentsJson: string): ToolCallItem {
    return { type: "toolCall", callId, name, arguments: argumentsJson };
  },
  toolResult(callId: string, result: string): ToolResultItem {
    return { type: "toolResult", callId, result };
  },
  imageFromUri(uri: string, format?: string): ImageItem {
    return format === undefined ? { type: "image", uri } : { type: "image", uri, format };
  },
  audioFromUri(uri: string, format?: string): AudioItem {
    return format === undefined ? { type: "audio", uri } : { type: "audio", uri, format };
  },
  /**
   * Build a raw-bytes item. The native layer reads `data` directly — **no
   * copy is made**. The source `Uint8Array` is pinned via an N-API reference
   * for the owning `Request`'s lifetime and must not be modified until that
   * `Request` is dropped. `SharedArrayBuffer`-backed views are rejected
   * because the bytes are read on a native thread without synchronization.
   */
  bytes(data: Uint8Array): BytesItem {
    return { type: "bytes", data };
  },
  /**
   * Build a tensor item. `data.byteLength` must equal `product(shape) *
   * elementSize(dataType)`. The native layer reads `data` directly — **no
   * copy is made** — and pins it for the owning `Request`'s lifetime; do
   * not modify the source until that `Request` is dropped.
   */
  tensor(dataType: TensorDataType, shape: ReadonlyArray<number>, data: Uint8Array): TensorItem {
    return { type: "tensor", dataType, shape, data };
  },
  /**
   * Build an image item from already-encoded bytes (PNG, JPEG, ...). The
   * native layer reads `data` directly — **no copy is made** — and pins it
   * for the owning `Request`'s lifetime; do not modify the source until
   * that `Request` is dropped.
   */
  imageFromData(format: string, data: Uint8Array): ImageItem {
    return { type: "image", data, format };
  },
  /**
   * Build an audio item from raw or container-encoded bytes. The native
   * layer reads `data` directly — **no copy is made** — and pins it for
   * the owning `Request`'s lifetime; do not modify the source until that
   * `Request` is dropped. Particularly relevant for live-PCM streaming
   * where the source is a large rolling buffer.
   */
  audioFromData(format: string, data: Uint8Array, sampleRate?: number, channels?: number): AudioItem {
    const item: AudioItem = { type: "audio", data, format };
    return {
      ...item,
      ...(sampleRate !== undefined ? { sampleRate } : {}),
      ...(channels !== undefined ? { channels } : {}),
    };
  },
  /**
   * Build an audio item carrying only the format descriptor — no initial
   * bytes and no URI. Use this when the actual audio bytes will be supplied
   * via an accompanying `ItemQueue` (live PCM streaming). Mirrors the C++
   * `Item::AudioFromData(format, nullptr, 0, sampleRate, channels)` shape.
   *
   * For one-shot audio with data already in hand, use {@link audioFromData}.
   * For audio backed by a file or URL, use {@link audioFromUri}.
   */
  audioDescriptor(format: string, sampleRate: number, channels: number): AudioItem {
    return { type: "audio", format, sampleRate, channels };
  },
} as const);
