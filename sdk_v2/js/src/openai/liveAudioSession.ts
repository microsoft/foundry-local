// `LiveAudioTranscriptionSession`. Push PCM audio chunks via `append()` and consume transcription results via
// `getStream()`. The native audio session streams raw text tokens through `Session.processStreamingRequest`; this
// class wraps each token in a `LiveAudioTranscriptionResponse` for V1 source-compat and drains them through a
// small Promise-based async buffer that `getStream()` yields from.

import { ItemQueue } from "../item-queue.js";
import { Item, type SpeechResultItem } from "../items.js";
import type { Model } from "../model.js";
import { Request } from "../request.js";
import { AudioSession } from "../session.js";
import type { LiveAudioTranscriptionResponse } from "./liveAudioTypes.js";

type State = "Created" | "Started" | "Stopped" | "Disposed";

export class LiveAudioTranscriptionOptions {
  sampleRate = 16000;
  channels = 1;
  bitsPerSample = 16;
  language?: string;
  pushQueueCapacity = 100;
}

export class LiveAudioTranscriptionSession implements AsyncDisposable, Disposable {
  readonly #model: Model;
  public settings = new LiveAudioTranscriptionOptions();

  #state: State = "Created";
  #queue: ItemQueue | undefined;
  #session: AudioSession | undefined;
  #request: Request | undefined;
  #processingPromise: Promise<void> | undefined;

  // Async buffer driving `getStream()`. A single pending resolver is woken whenever a response is enqueued or
  // the stream completes.
  #pending: LiveAudioTranscriptionResponse[] = [];
  #waiter: (() => void) | null = null;
  #streamDone = false;
  #streamError: unknown = null;

  /** @internal — construct via `audioClient.createLiveTranscriptionSession()`. */
  constructor(model: Model) {
    this.#model = model;
  }

  /** @internal Identifies the model the session is bound to. */
  get modelId(): string {
    return this.#model.id;
  }

  async start(): Promise<void> {
    this.#throwIfDisposed();
    if (this.#state === "Started") {
      throw new Error(`Session can only be started when not running (was ${this.#state}).`);
    }

    if (this.#state === "Stopped") {
      // Prior run finished. Release old native handles before creating a new
      // streaming pipeline for the next run.
      this.#queue?.dispose();
      this.#session?.dispose();
      this.#queue = undefined;
      this.#session = undefined;
      this.#request = undefined;
      this.#processingPromise = undefined;
    }

    // Per-run stream state. Reset before wiring the new processing task.
    this.#pending = [];
    this.#waiter = null;
    this.#streamDone = false;
    this.#streamError = null;

    const descriptor = Item.audioDescriptor("pcm", this.settings.sampleRate, this.settings.channels);

    this.#queue = new ItemQueue();
    this.#session = new AudioSession(this.#model);
    this.#request = new Request();
    this.#request.addItem(descriptor);
    this.#request.addItem(this.#queue);

    const session = this.#session;
    const request = this.#request;

    // Kick off streaming without awaiting. The native ProcessRequest runs on
    // a libuv worker thread, so the JS thread is free to push chunks into
    // the queue concurrently.
    this.#processingPromise = (async () => {
      try {
        const stream = session.processStreamingRequest(request);
        for await (const item of stream) {
          if (item.type !== "speechSegment") continue;
          if (item.text === "") continue;
          // Per-token partial result from AudioSession. `is_final` here marks the last message in the stream
          // (set by the final-Response drain below), not per-segment finality, so segment-level
          // `kind === "final"` on intermediate items is intentionally not propagated.
          this.#enqueueResponse({
            is_final: false,
            content: [{ text: item.text, transcript: item.text }],
          });
        }
        // Final aggregate transcript — sourced from the terminal Response's speechResult item rather than
        // locally concatenated tokens so we get the canonical model output (with whatever post-processing the
        // native side applied) and stay consistent with non-streaming Response semantics.
        const response = await stream.response;
        const finalText =
          response.output.find((it): it is SpeechResultItem => it.type === "speechResult")?.text ?? "";
        if (finalText !== "") {
          this.#enqueueResponse({
            is_final: true,
            content: [{ text: finalText, transcript: finalText }],
          });
        }
        this.#completeStream();
      } catch (err) {
        this.#failStream(err);
      }
    })();

    this.#state = "Started";
  }

  async append(pcmData: Uint8Array): Promise<void> {
    this.#throwIfDisposed();
    if (this.#state !== "Started") {
      throw new Error(`Session must be Started to append audio (was ${this.#state}).`);
    }
    if (!(pcmData instanceof Uint8Array)) {
      throw new TypeError("append(pcmData): expected a Uint8Array");
    }
    // Copy so the caller's buffer is not pinned for the lifetime of the queue.
    const copy = new Uint8Array(pcmData.length);
    copy.set(pcmData);
    // queue exists because state === Started.
    (this.#queue as ItemQueue).push(Item.bytes(copy));
  }

  async *getStream(): AsyncGenerator<LiveAudioTranscriptionResponse> {
    this.#throwIfDisposed();
    if (this.#state !== "Started") {
      throw new Error(`Session must be Started to read stream (was ${this.#state}).`);
    }
    while (true) {
      const next = this.#pending.shift();
      if (next !== undefined) {
        yield next;
        continue;
      }
      if (this.#streamDone) {
        if (this.#streamError !== null) {
          throw this.#streamError;
        }
        return;
      }
      await new Promise<void>((resolve) => {
        this.#waiter = resolve;
      });
    }
  }

  async stop(): Promise<void> {
    this.#throwIfDisposed();
    if (this.#state !== "Started") {
      // Created (never started) or already Stopped — no-op.
      return;
    }
    // Signal end-of-input to the consumer side of the queue. The native
    // ProcessRequest will drain remaining items and return.
    this.#queue?.markFinished();
    if (this.#processingPromise !== undefined) {
      try {
        await this.#processingPromise;
      } catch {
        // Errors surface to consumers via getStream(); swallow here.
      }
    }
    this.#state = "Stopped";
  }

  async dispose(): Promise<void> {
    if (this.#state === "Disposed") return;

    if (this.#state === "Started") {
      try {
        await this.stop();
      } catch {
        // Keep dispose() best-effort and non-throwing. Stream errors already
        // surface through getStream().
      }
    }

    this.#queue?.dispose();
    this.#session?.dispose();
    // Request has no dispose() — it's released by GC once the native
    // streaming worker drops its pin.
    this.#queue = undefined;
    this.#session = undefined;
    this.#request = undefined;
    this.#state = "Disposed";
  }

  [Symbol.dispose](): void {
    void this.dispose();
  }

  [Symbol.asyncDispose](): Promise<void> {
    return this.dispose();
  }

  #throwIfDisposed(): void {
    if (this.#state === "Disposed") {
      throw new Error("LiveAudioTranscriptionSession: already disposed");
    }
  }

  #enqueueResponse(response: LiveAudioTranscriptionResponse): void {
    this.#pending.push(response);
    this.#wake();
  }

  #completeStream(): void {
    if (this.#streamDone) return;
    this.#streamDone = true;
    this.#wake();
  }

  #failStream(err: unknown): void {
    if (this.#streamDone) return;
    this.#streamError = err;
    this.#streamDone = true;
    this.#wake();
  }

  #wake(): void {
    const w = this.#waiter;
    if (w !== null) {
      this.#waiter = null;
      w();
    }
  }
}
