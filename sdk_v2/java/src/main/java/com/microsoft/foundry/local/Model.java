// Copyright (c) Microsoft Corporation. Licensed under the MIT License.
package com.microsoft.foundry.local;

import com.sun.jna.Pointer;
import com.sun.jna.ptr.IntByReference;
import java.lang.ref.Reference;
import java.nio.file.Path;
import java.util.Objects;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.DoubleConsumer;

/** Borrowed exact native model. No method implicitly downloads model weights or EPs. */
public final class Model {
    final FoundryLocalManager owner;
    final Pointer handle;
    Model(FoundryLocalManager owner, Pointer handle) { this.owner = owner; this.handle = handle; }

    public ModelInfo info() {
        NativeApi.outsideCallback();
        synchronized (owner) {
            owner.checkOpen();
            NativeApi api = owner.api;
            Pointer info = api.create(api.model, NativeApi.ModelApi.GET_INFO, handle);
            return new ModelInfo(
                    NativeApi.text(api.model.pointer(NativeApi.ModelApi.INFO_GET_ID, info)),
                    NativeApi.text(api.model.pointer(NativeApi.ModelApi.INFO_GET_ALIAS, info)),
                    NativeApi.text(api.model.pointer(NativeApi.ModelApi.INFO_GET_NAME, info)),
                    api.model.integer(NativeApi.ModelApi.INFO_GET_VERSION, info),
                    NativeApi.text(api.model.pointer(NativeApi.ModelApi.INFO_GET_URI, info)),
                    NativeApi.text(api.model.pointer(NativeApi.ModelApi.INFO_GET_EXECUTION_PROVIDER, info)),
                    NativeApi.text(api.model.pointer(NativeApi.ModelApi.INFO_GET_TASK, info)),
                    NativeApi.text(api.model.pointer(NativeApi.ModelApi.INFO_GET_STRING_PROPERTY, info, "license")),
                    NativeApi.text(api.model.pointer(
                            NativeApi.ModelApi.INFO_GET_STRING_PROPERTY,
                            info,
                            "license_description")));
        }
    }

    public boolean isCached() { return flag(NativeApi.ModelApi.IS_CACHED); }
    public boolean isLoaded() { return flag(NativeApi.ModelApi.IS_LOADED); }
    private boolean flag(int slot) {
        NativeApi.outsideCallback();
        synchronized (owner) {
            owner.checkOpen();
            IntByReference value = new IntByReference();
            owner.api.check(owner.api.model.pointer(slot, handle, value));
            return value.getValue() != 0;
        }
    }

    public Path path() {
        NativeApi.outsideCallback();
        synchronized (owner) {
            owner.checkOpen();
            if (!isCached()) throw new IllegalStateException("Model is not cached");
            return Path.of(NativeApi.text(owner.api.create(owner.api.model, NativeApi.ModelApi.GET_PATH, handle)));
        }
    }

    /**
     * Blocking explicit download. Call only after reviewing the model license.
     * Progress is 0..100, on native threads; callbacks must not call SDK methods.
     * Cancellation is observed at native progress checkpoints (not a deadline guarantee).
     */
    public void download(CancellationToken cancellation, DoubleConsumer progress) {
        NativeApi.outsideCallback();
        Objects.requireNonNull(cancellation);
        Objects.requireNonNull(progress);
        synchronized (owner) {
            owner.checkOpen();
            if (cancellation.isCancelled()) throw new FoundryLocalException(5, "Download cancelled before start");
            AtomicReference<Throwable> failure = new AtomicReference<>();
            NativeApi.ProgressCallback callback = (value, userData) -> {
                NativeApi.IN_CALLBACK.set(true);
                try {
                    if (cancellation.isCancelled()) return 1;
                    progress.accept(value);
                    return cancellation.isCancelled() ? 1 : 0;
                } catch (Throwable e) {
                    // Java exceptions must never escape through a native callback trampoline.
                    failure.compareAndSet(null, e);
                    return 1;
                } finally { NativeApi.IN_CALLBACK.remove(); }
            };
            Pointer status;
            try { status = owner.api.model.pointer(NativeApi.ModelApi.DOWNLOAD, handle, callback, null); }
            finally { Reference.reachabilityFence(callback); }
            if (failure.get() != null) {
                if (status != null) owner.api.root.call(NativeApi.Root.STATUS_RELEASE, status);
                throw new IllegalStateException("Download progress callback failed", failure.get());
            }
            owner.api.check(status);
            if (cancellation.isCancelled()) throw new FoundryLocalException(5, "Download cancelled");
        }
    }

    public void load() {
        NativeApi.outsideCallback();
        synchronized (owner) {
            owner.checkOpen();
            if (!isCached()) throw new IllegalStateException("Model is not cached; explicitly download it first");
            owner.api.check(owner.api.model.pointer(NativeApi.ModelApi.LOAD, handle));
        }
    }

    public void unload() {
        NativeApi.outsideCallback();
        synchronized (owner) {
            owner.checkOpen();
            if (owner.sessions.stream().anyMatch(s -> s.model.handle.equals(handle))) {
                throw new IllegalStateException("Close all sessions for this model before unloading it");
            }
            owner.api.check(owner.api.model.pointer(NativeApi.ModelApi.UNLOAD, handle));
        }
    }

    public AudioSession createAudioSession() {
        NativeApi.outsideCallback();
        synchronized (owner) {
            owner.checkOpen();
            if (!info().task().equals("automatic-speech-recognition")) {
                throw new IllegalStateException("The selected model is not an ASR model");
            }
            if (!isLoaded()) throw new IllegalStateException("Explicitly load the model before creating a session");
            return new AudioSession(this);
        }
    }
}
