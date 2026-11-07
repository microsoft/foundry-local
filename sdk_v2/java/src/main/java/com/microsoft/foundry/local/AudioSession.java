// Copyright (c) Microsoft Corporation. Licensed under the MIT License.
package com.microsoft.foundry.local;

import com.sun.jna.Pointer;
import java.io.IOException;
import java.nio.file.Path;
import java.util.Objects;
import java.util.function.Consumer;

/** One request at a time; close each Transcription before starting the next. */
public final class AudioSession extends OwnedSession {
    final Model model;
    final NativeApi api;
    Pointer handle;
    private Transcription active;

    AudioSession(Model model) {
        this.model = model;
        api = model.owner.api;
        handle = api.create(api.inference, NativeApi.InferenceApi.SESSION_CREATE, model.handle);
        model.owner.sessions.add(this);
    }

    @Override Model model() { return model; }

    /**
     * Decodes a PCM WAV file and submits its samples through the native streaming-audio path.
     * This method does not use the native file/URI transcription path.
     */
    public Transcription transcribeWav(Path wav, Consumer<SpeechEvent> listener) throws IOException {
        return transcribeWav(wav, null, listener);
    }

    /**
     * Decodes a PCM WAV file through the streaming-audio path with a request-scoped language hint.
     * See {@link #streamPcm(PcmFormat, String, Consumer)} for language semantics.
     *
     * @param language native model language code, explicit {@code "auto"}, or {@code null} to omit
     * @throws IllegalArgumentException if language is blank or contains NUL
     */
    public Transcription transcribeWav(Path wav, String language, Consumer<SpeechEvent> listener) throws IOException {
        NativeApi.outsideCallback();
        validateLanguage(language);
        WavAudio audio = WavAudio.read(wav);
        return start(audio.pcm(), audio.format(), language, listener);
    }

    /** Starts a native streaming-audio transcription for PCM chunks supplied by the caller. */
    public Transcription streamPcm(PcmFormat format, Consumer<SpeechEvent> listener) {
        return streamPcm(format, null, listener);
    }

    /**
     * Starts PCM transcription with a language hint applied only to this request.
     * {@code null} preserves the native default; it does not request automatic detection.
     * Use {@code "auto"} to explicitly request detection on models that support it, or a
     * model-supported code such as {@code "en-US"} or {@code "zh-CN"}.
     * Non-null hints are forwarded unchanged. Support depends on the native runtime and model;
     * unrecognized codes may be ignored by native. This does not filter transcript text.
     *
     * @param language native model language code, explicit {@code "auto"}, or {@code null} to omit
     * @throws IllegalArgumentException if language is blank or contains NUL
     */
    public Transcription streamPcm(PcmFormat format, String language, Consumer<SpeechEvent> listener) {
        NativeApi.outsideCallback();
        validateLanguage(language);
        return start(null, Objects.requireNonNull(format), language, listener);
    }

    static void validateLanguage(String language) {
        if (language != null && (language.isBlank() || language.indexOf('\0') >= 0)) {
            throw new IllegalArgumentException("Language must be nonblank and must not contain NUL");
        }
    }

    private Transcription start(byte[] wav, PcmFormat format, String language, Consumer<SpeechEvent> listener) {
        NativeApi.outsideCallback();
        synchronized (model.owner) {
            model.owner.checkOpen();
            if (handle == null) throw new IllegalStateException("Session is closed");
            if (active != null && !active.isClosed()) {
                throw new IllegalStateException("Close the previous transcription");
            }
            active = new Transcription(this, wav, format, language, Objects.requireNonNull(listener));
            return active;
        }
    }

    @Override public void close() {
        NativeApi.outsideCallback();
        synchronized (model.owner) {
            if (handle == null) return;
            Throwable failure = null;
            try {
                if (active != null) active.close();
            } catch (RuntimeException | Error e) {
                failure = e;
            }
            try {
                api.inference.call(NativeApi.InferenceApi.SESSION_RELEASE, handle);
            } catch (RuntimeException | Error e) {
                failure = NativeApi.preserveFailure(failure, e);
            } finally {
                active = null;
                handle = null;
                model.owner.sessions.remove(this);
            }
            NativeApi.rethrow(failure);
        }
    }
}
