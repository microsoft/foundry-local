// Copyright (c) Microsoft Corporation. Licensed under the MIT License.
package com.microsoft.foundry.local;

import com.sun.jna.Pointer;
import java.io.IOException;
import java.nio.file.Path;
import java.util.Objects;
import java.util.function.Consumer;

/** One request at a time; close each Transcription before starting the next. */
public final class AudioSession implements AutoCloseable {
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

    public Transcription transcribeWav(Path wav, Consumer<SpeechEvent> listener) throws IOException {
        NativeApi.outsideCallback();
        WavAudio audio = WavAudio.read(wav);
        return start(audio.pcm(), audio.format(), listener);
    }

    public Transcription streamPcm(PcmFormat format, Consumer<SpeechEvent> listener) {
        return start(null, Objects.requireNonNull(format), listener);
    }

    private Transcription start(byte[] wav, PcmFormat format, Consumer<SpeechEvent> listener) {
        NativeApi.outsideCallback();
        synchronized (model.owner) {
            model.owner.checkOpen();
            if (handle == null) throw new IllegalStateException("Session is closed");
            if (active != null && !active.isClosed()) throw new IllegalStateException("Close the previous transcription");
            active = new Transcription(this, wav, format, Objects.requireNonNull(listener));
            return active;
        }
    }

    @Override public void close() {
        NativeApi.outsideCallback();
        synchronized (model.owner) {
            if (handle == null) return;
            if (active != null) active.close();
            api.inference.call(NativeApi.InferenceApi.SESSION_RELEASE, handle);
            handle = null;
            model.owner.sessions.remove(this);
        }
    }
}
