// Copyright (c) Microsoft Corporation. Licensed under the MIT License.
package com.microsoft.foundry.local;

import com.sun.jna.Pointer;
import com.sun.jna.ptr.PointerByReference;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.Set;

/** Owns the native singleton and every session. Catalogs/models are borrowed views. */
public final class FoundryLocalManager implements AutoCloseable {
    private static boolean active;
    final NativeApi api;
    final Set<AudioSession> sessions = new HashSet<>();
    private Pointer handle;

    public FoundryLocalManager(Configuration configuration) {
        NativeApi.outsideCallback();
        synchronized (FoundryLocalManager.class) {
            if (active) throw new IllegalStateException("Only one FoundryLocalManager may be open per JVM");
            api = NativeApi.load(configuration.runtimeDirectory());
            Pointer config = api.create(api.config, NativeApi.ConfigurationApi.CREATE, configuration.appName());
            try {
                // Native status errors still propagate; keep the SDK quiet unless callers opt into logging.
                api.check(api.config.pointer(NativeApi.ConfigurationApi.SET_DEFAULT_LOG_LEVEL, config, 5));
                api.check(api.config.pointer(
                        NativeApi.ConfigurationApi.SET_APP_DATA_DIRECTORY,
                        config,
                        configuration.appDataDirectory().toString()));
                api.check(api.config.pointer(
                        NativeApi.ConfigurationApi.SET_MODEL_CACHE_DIRECTORY,
                        config,
                        configuration.modelCacheDirectory().toString()));
                PointerByReference pairs = new PointerByReference();
                api.root.call(NativeApi.Root.KEY_VALUE_PAIRS_CREATE, pairs);
                try {
                    api.root.call(
                            NativeApi.Root.KEY_VALUE_PAIRS_ADD,
                            pairs.getValue(),
                            "DisableNonessentialTelemetry",
                            "true");
                    api.check(api.config.pointer(
                            NativeApi.ConfigurationApi.SET_ADDITIONAL_OPTIONS,
                            config,
                            pairs.getValue()));
                } finally {
                    api.root.call(NativeApi.Root.KEY_VALUE_PAIRS_RELEASE, pairs.getValue());
                }
                handle = api.create(api.root, NativeApi.Root.MANAGER_CREATE, config);
                active = true;
            } finally {
                api.config.call(NativeApi.ConfigurationApi.RELEASE, config);
            }
        }
    }

    public String runtimeVersion() { return api.version; }
    public String nativeTarget() { return NativeApi.target(); }

    public Catalog catalog() {
        NativeApi.outsideCallback();
        synchronized (this) {
            checkOpen();
            return new Catalog(this, api.create(api.root, NativeApi.Root.MANAGER_GET_CATALOG, handle));
        }
    }

    void checkOpen() {
        NativeApi.outsideCallback();
        if (handle == null) throw new IllegalStateException("Manager is closed");
    }

    @Override public void close() {
        NativeApi.outsideCallback();
        synchronized (this) {
            if (handle == null) return;
            RuntimeException failure = null;
            for (AudioSession session : new ArrayList<>(sessions)) {
                try {
                    session.close();
                } catch (RuntimeException e) {
                    if (failure == null) failure = e;
                    else failure.addSuppressed(e);
                }
            }
            try {
                api.check(api.root.pointer(NativeApi.Root.MANAGER_SHUTDOWN, handle));
            } catch (RuntimeException e) {
                if (failure == null) failure = e;
                else failure.addSuppressed(e);
            } finally {
                try {
                    api.root.call(NativeApi.Root.MANAGER_RELEASE, handle);
                } finally {
                    handle = null;
                    synchronized (FoundryLocalManager.class) {
                        active = false;
                    }
                }
            }
            if (failure != null) throw failure;
        }
    }
}
