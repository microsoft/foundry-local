// Copyright (c) Microsoft Corporation. Licensed under the MIT License.
package com.microsoft.foundry.local;

import com.sun.jna.Memory;
import com.sun.jna.Pointer;
import com.sun.jna.ptr.PointerByReference;
import java.lang.ref.Reference;
import java.time.Duration;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.Consumer;
import java.util.function.Function;

/** Owns a request, input buffers and callback lifetimes until the native worker has joined. */
public final class Transcription implements AutoCloseable {
    private final AudioSession session;
    private final NativeApi api;
    private final PcmFormat format;
    private final Consumer<SpeechEvent> listener;
    private final Completion completion = new Completion();
    private final AtomicReference<Throwable> callbackFailure = new AtomicReference<>();
    private final Map<Long, Memory> buffers = new ConcurrentHashMap<>();
    private final NativeApi.StreamCallback callback = this::onEvent;
    private final NativeApi.BytesDeleter deleter = this::releaseBuffer;
    private final Thread worker;
    private final Thread feeder;
    private Pointer request, queue;
    private boolean finished, closing, closed;
    private long bufferedBytes;
    private final long originNanos = System.nanoTime();
    private long firstInputNanos, firstNonemptyNanos, inputClosedNanos, submittedBytes;

    Transcription(AudioSession session, byte[] wav, PcmFormat format, Consumer<SpeechEvent> listener) {
        this.session = session;
        this.api = session.api;
        this.format = format;
        this.listener = listener;
        request = api.create(api.inference, NativeApi.InferenceApi.REQUEST_CREATE);
        try {
            Pointer audio = api.create(api.item, NativeApi.ItemApi.CREATE, 30);
            boolean transferred = false;
            try (Memory text = NativeApi.utf8("pcm")) {
                NativeApi.AudioData data = new NativeApi.AudioData();
                data.format = text;
                data.sampleRate = format.sampleRate();
                data.channels = format.channels();
                data.write();
                api.check(api.item.pointer(NativeApi.ItemApi.SET_AUDIO, audio, data));
                transferred = true;
                api.check(api.inference.pointer(NativeApi.InferenceApi.REQUEST_ADD_ITEM, request, audio, (byte) 1));
            } finally {
                if (!transferred) api.item.call(NativeApi.ItemApi.RELEASE, audio);
            }
            {
                Pointer queueItem = api.create(api.item, NativeApi.ItemApi.CREATE, 200);
                boolean queueTransferred = false;
                try {
                    queue = api.create(api.item, NativeApi.ItemApi.GET_QUEUE, queueItem);
                    queueTransferred = true;
                    api.check(api.inference.pointer(
                            NativeApi.InferenceApi.REQUEST_ADD_ITEM, request, queueItem, (byte) 1));
                } finally {
                    if (!queueTransferred) api.item.call(NativeApi.ItemApi.RELEASE, queueItem);
                }
            }
            api.check(api.inference.pointer(
                    NativeApi.InferenceApi.SESSION_SET_STREAMING_CALLBACK,
                    session.handle,
                    callback,
                    null));
        } catch (RuntimeException | Error e) {
            api.inference.call(NativeApi.InferenceApi.REQUEST_RELEASE, request);
            request = null;
            throw e;
        }
        worker = new Thread(this::run, "foundry-java-asr");
        feeder = wav == null ? null : new Thread(() -> feedWav(wav), "foundry-java-asr-input");
        worker.start();
        if (feeder != null) feeder.start();
    }

    private void feedWav(byte[] pcm) {
        try {
            for (int offset = 0; offset < pcm.length; offset += 3200) {
                writePcm(java.util.Arrays.copyOfRange(pcm, offset, Math.min(offset + 3200, pcm.length)));
            }
            finishInput();
        } catch (InterruptedException | RuntimeException e) {
            synchronized (this) {
                if (!completion.isCancelled() && !closing && !isDone()) {
                    callbackFailure.compareAndSet(null, e);
                    cancel();
                }
            }
            if (e instanceof InterruptedException) Thread.currentThread().interrupt();
        }
    }

    /** Copies a complete PCM chunk. Applies bounded backpressure (at most 2 seconds queued). */
    public synchronized void writePcm(byte[] bytes) throws InterruptedException {
        NativeApi.outsideCallback();
        if (feeder != null && Thread.currentThread() != feeder) {
            throw new IllegalStateException("WAV input is managed automatically");
        }
        format.validateChunk(bytes);
        ensureWritable();
        while (bufferedBytes + bytes.length > 64000) {
            wait(100);
            ensureWritable();
        }
        Memory memory = new Memory(bytes.length);
        memory.write(0, bytes, 0, bytes.length);
        long address = Pointer.nativeValue(memory);
        buffers.put(address, memory);
        bufferedBytes += bytes.length;
        Pointer item = null;
        boolean ownsBuffer = false;
        try {
            item = api.create(api.item, NativeApi.ItemApi.CREATE, 1);
            NativeApi.BytesData data = new NativeApi.BytesData();
            data.data = memory;
            data.mutableData = memory;
            data.dataSize = bytes.length;
            data.deleter = deleter;
            data.write();
            api.check(api.item.pointer(NativeApi.ItemApi.SET_BYTES, item, data));
            ownsBuffer = true;
            Pointer transferredItem = item;
            item = null;
            api.check(api.item.pointer(NativeApi.ItemApi.QUEUE_PUSH, queue, transferredItem));
            if (firstInputNanos == 0) firstInputNanos = System.nanoTime();
            submittedBytes += bytes.length;
        } finally {
            if (item != null) api.item.call(NativeApi.ItemApi.RELEASE, item);
            if (!ownsBuffer) freeBuffer(address);
        }
    }

    private void ensureWritable() {
        if (closed || closing || finished || completion.isCancelled() || isDone()) {
            throw new IllegalStateException("Transcription no longer accepts PCM");
        }
    }

    /** Signals natural end of PCM input. Unlike cancel(), this flushes final recognition. */
    public synchronized void finishInput() {
        NativeApi.outsideCallback();
        if (feeder != null && Thread.currentThread() != feeder) {
            throw new IllegalStateException("WAV input is managed automatically");
        }
        if (closed || closing) throw new IllegalStateException("Transcription is closed");
        if (queue == null) throw new IllegalStateException("Only PCM requests have an input queue");
        if (!finished) {
            api.item.call(NativeApi.ItemApi.QUEUE_MARK_FINISHED, queue);
            inputClosedNanos = System.nanoTime();
            finished = true;
        }
    }

    public synchronized void cancel() {
        NativeApi.outsideCallback();
        if (closed || !completion.cancel()) return;
        api.check(api.inference.pointer(NativeApi.InferenceApi.REQUEST_CANCEL, request));
        if (queue != null && !finished) {
            api.item.call(NativeApi.ItemApi.QUEUE_MARK_FINISHED, queue);
            finished = true;
        }
        notifyAll();
    }

    public boolean isDone() { return completion.result.isDone(); }
    public synchronized boolean isClosed() { return closed; }
    public synchronized boolean isCancelled() { return completion.isCancelled(); }

    /** Milliseconds share one request-local monotonic origin; absent observations remain null. */
    public synchronized TranscriptionTiming timing() {
        synchronized (completion) {
            return new TranscriptionTiming(relative(firstInputNanos), relative(firstNonemptyNanos),
                    relative(inputClosedNanos), relative(completion.finalizedNanos),
                    relative(completion.cancellationNanos), submittedBytes);
        }
    }

    private Double relative(long nanos) { return nanos == 0 ? null : (nanos - originNanos) / 1_000_000.0; }

    public TranscriptionResult await() throws InterruptedException {
        NativeApi.outsideCallback();
        try { return completion.result.get(); }
        catch (ExecutionException e) { throw propagate(e.getCause()); }
    }

    public TranscriptionResult await(Duration timeout) throws InterruptedException, TimeoutException {
        NativeApi.outsideCallback();
        try { return completion.result.get(timeout.toMillis(), TimeUnit.MILLISECONDS); }
        catch (ExecutionException e) { throw propagate(e.getCause()); }
    }

    private static RuntimeException propagate(Throwable e) {
        if (e instanceof RuntimeException runtime) return runtime;
        if (e instanceof Error error) throw error;
        return new IllegalStateException("Native transcription failed", e);
    }

    private int onEvent(NativeApi.CallbackData event, Pointer userData) {
        NativeApi.IN_CALLBACK.set(true);
        try {
            PointerByReference next = new PointerByReference();
            while (api.item.bool(NativeApi.ItemApi.QUEUE_TRY_POP, event.queue, next)) {
                Pointer item = next.getValue();
                try {
                    if (api.item.integer(NativeApi.ItemApi.GET_TYPE, item) != 31) {
                        throw new IllegalStateException("Unexpected ASR stream item");
                    }
                    NativeApi.SegmentData data = new NativeApi.SegmentData();
                    data.write();
                    api.check(api.item.pointer(NativeApi.ItemApi.GET_SPEECH_SEGMENT, item, data));
                    data.read();
                    SpeechEvent.Kind kind = switch (data.kind) {
                        case 0 -> SpeechEvent.Kind.TOKEN;
                        case 1 -> SpeechEvent.Kind.PARTIAL;
                        case 2 -> SpeechEvent.Kind.FINAL;
                        default -> throw new IllegalStateException("Unknown speech segment kind: " + data.kind);
                    };
                    String text = NativeApi.text(data.text);
                    synchronized (this) {
                        if (!text.isBlank() && firstNonemptyNanos == 0) firstNonemptyNanos = System.nanoTime();
                    }
                    listener.accept(new SpeechEvent(kind, text,
                            optionalTime(data.start), optionalTime(data.end), data.utteranceStart != 0));
                } finally { api.item.call(NativeApi.ItemApi.RELEASE, item); }
            }
            return completion.isCancelled() ? 1 : 0;
        } catch (Throwable e) {
            callbackFailure.compareAndSet(null, e);
            return 1;
        } finally { NativeApi.IN_CALLBACK.remove(); }
    }

    private static Long optionalTime(long value) { return value == Long.MIN_VALUE ? null : value; }

    private void run() {
        long start = System.nanoTime();
        PointerByReference response = new PointerByReference();
        try {
            Pointer status = api.inference.pointer(
                    NativeApi.InferenceApi.SESSION_PROCESS_REQUEST, session.handle, request, response);
            if (callbackFailure.get() != null) {
                if (status != null) api.root.call(NativeApi.Root.STATUS_RELEASE, status);
                throw new IllegalStateException("ASR callback or input feeder failed", callbackFailure.get());
            }
            try { api.check(status); }
            catch (FoundryLocalException e) {
                if (e.code() != 5) throw e;
                synchronized (completion) { completion.cancelled = true; }
            }
            completion.complete(wasCancelled -> {
                int reason = response.getValue() == null
                        ? 0
                        : api.inference.integer(
                                NativeApi.InferenceApi.RESPONSE_GET_FINISH_REASON, response.getValue());
                String text = "", language = "";
                Long duration = null;
                wasCancelled = cancelledResult(wasCancelled, response.getValue() != null, reason);
                if (!wasCancelled) {
                    boolean found = false;
                    for (long i = 0;
                            i < api.inference.size(
                                    NativeApi.InferenceApi.RESPONSE_GET_ITEM_COUNT, response.getValue());
                            i++) {
                        Pointer item = api.create(
                                api.inference,
                                NativeApi.InferenceApi.RESPONSE_GET_ITEM,
                                response.getValue(),
                                i);
                        if (api.item.integer(NativeApi.ItemApi.GET_TYPE, item) == 32) {
                            NativeApi.ResultData data = new NativeApi.ResultData();
                            data.write();
                            api.check(api.item.pointer(NativeApi.ItemApi.GET_SPEECH_RESULT, item, data));
                            data.read();
                            text = NativeApi.text(data.text);
                            language = NativeApi.text(data.language);
                            duration = optionalTime(data.duration);
                            found = true;
                        }
                    }
                    if (!found) throw new IllegalStateException("Native ASR response has no speech result");
                }
                return new TranscriptionResult(text, language, duration, wasCancelled, reason,
                        TimeUnit.NANOSECONDS.toMillis(System.nanoTime() - start));
            });
        } catch (Throwable e) {
            completion.fail(e);
        } finally {
            if (response.getValue() != null) {
                api.inference.call(NativeApi.InferenceApi.RESPONSE_RELEASE, response.getValue());
            }
            synchronized (this) { notifyAll(); }
            Reference.reachabilityFence(callback);
            Reference.reachabilityFence(deleter);
        }
    }

    static final class Completion {
        final CompletableFuture<TranscriptionResult> result = new CompletableFuture<>();
        private boolean cancelled;
        private long cancellationNanos, finalizedNanos;

        synchronized boolean cancel() {
            if (result.isDone()) return false;
            if (cancellationNanos == 0) cancellationNanos = System.nanoTime();
            cancelled = true;
            return true;
        }

        synchronized boolean isCancelled() { return cancelled; }

        void complete(Function<Boolean, TranscriptionResult> decode) {
            boolean wasCancelled = isCancelled();
            TranscriptionResult value = decode.apply(wasCancelled);
            // Native decoding is outside the lock; only cancellation and terminal publication compete here.
            synchronized (this) {
                if (cancelled) {
                    value = new TranscriptionResult("", "", null, true,
                            value.nativeFinishReason(), value.elapsedMillis());
                }
                finalizedNanos = System.nanoTime();
                result.complete(value);
            }
        }

        synchronized void fail(Throwable error) { result.completeExceptionally(error); }
    }

    static boolean cancelledResult(boolean cancellationRequested, boolean hasResponse, int finishReason) {
        if (cancellationRequested) return true;
        if (!hasResponse) throw new IllegalStateException("Native ASR returned no response");
        if (finishReason != 2) throw new IllegalStateException("Unexpected native finish reason: " + finishReason);
        return false;
    }

    private void releaseBuffer(Pointer data, Pointer userData) {
        // flBytesData.mutable_data is at offset 16 on every supported 64-bit target.
        freeBuffer(Pointer.nativeValue(data.getPointer(16)));
    }

    private synchronized void freeBuffer(long address) {
        Memory memory = buffers.remove(address);
        if (memory != null) {
            bufferedBytes -= memory.size();
            memory.close();
            notifyAll();
        }
    }

    /** Cancels unfinished work and waits for native callbacks before releasing handles. */
    @Override public void close() {
        NativeApi.outsideCallback();
        boolean interrupted = false;
        synchronized (this) {
            while (closing && !closed) {
                try { wait(); }
                catch (InterruptedException e) { interrupted = true; }
            }
            if (closed) {
                if (interrupted) Thread.currentThread().interrupt();
                return;
            }
            cancel();
            closing = true;
        }
        while (worker.isAlive()) {
            try { worker.join(); }
            catch (InterruptedException e) { interrupted = true; }
        }
        while (feeder != null && feeder.isAlive()) {
            try { feeder.join(); }
            catch (InterruptedException e) { interrupted = true; }
        }
        try {
            try {
                api.check(api.inference.pointer(
                        NativeApi.InferenceApi.SESSION_SET_STREAMING_CALLBACK,
                        session.handle,
                        null,
                        null));
            } finally {
                api.inference.call(NativeApi.InferenceApi.REQUEST_RELEASE, request);
                synchronized (this) {
                    request = null;
                    queue = null;
                    closed = true;
                    closing = false;
                    notifyAll();
                }
            }
            if (!buffers.isEmpty()) throw new IllegalStateException("Native request did not release all PCM buffers");
        } finally {
            if (interrupted) Thread.currentThread().interrupt();
            Reference.reachabilityFence(callback);
            Reference.reachabilityFence(deleter);
        }
    }
}
