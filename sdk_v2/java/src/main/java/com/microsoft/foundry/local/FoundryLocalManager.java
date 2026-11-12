// Copyright (c) Microsoft Corporation. Licensed under the MIT License.
package com.microsoft.foundry.local;

import com.sun.jna.Pointer;
import com.sun.jna.ptr.PointerByReference;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Objects;
import java.util.Set;

/**
 * Owns the native singleton and every session. Catalogs/models are borrowed views.
 *
 * <p>Only one manager may be open at a time. After close, another manager can be created in
 * the same JVM only with the same resolved runtime directory used by the first manager.
 */
public final class FoundryLocalManager implements AutoCloseable {
    private static volatile FoundryLocalManager open;
    private static boolean exitHookInstalled;
    final NativeApi api;
    final Set<OwnedSession> sessions = new HashSet<>();
    private record CatalogKey(CatalogType type, Map<String, String> options) {}
    private final Map<CatalogKey, Catalog> catalogs = new HashMap<>();
    private final Map<Pointer, Catalog> catalogsByHandle = new HashMap<>();
    private Pointer handle;

    public FoundryLocalManager(Configuration configuration) {
        this(configuration, null);
    }

    FoundryLocalManager(Configuration configuration, NativeApi nativeApi) {
        NativeApi.outsideCallback();
        synchronized (FoundryLocalManager.class) {
            if (open != null) throw new IllegalStateException("Only one FoundryLocalManager may be open per JVM");
            installExitHook();
            api = nativeApi == null ? NativeApi.load(configuration.runtimeDirectory()) : nativeApi;
            Pointer config = api.create(api.config, NativeApi.ConfigurationApi.CREATE, configuration.appName());
            try {
                api.check(api.config.pointer(
                        NativeApi.ConfigurationApi.SET_DEFAULT_LOG_LEVEL, config,
                        configuration.logLevel().nativeValue()));
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
                            Boolean.toString(configuration.disableNonessentialTelemetry()));
                    api.check(api.config.pointer(
                            NativeApi.ConfigurationApi.SET_ADDITIONAL_OPTIONS,
                            config,
                            pairs.getValue()));
                } finally {
                    api.root.call(NativeApi.Root.KEY_VALUE_PAIRS_RELEASE, pairs.getValue());
                }
                handle = api.create(api.root, NativeApi.Root.MANAGER_CREATE, config);
                open = this;
            } finally {
                api.config.call(NativeApi.ConfigurationApi.RELEASE, config);
            }
        }
    }

    // The native runtime must release its manager before its C++ static destructors run at
    // process exit; otherwise the manager's logger teardown can use already-destroyed globals.
    private static void installExitHook() {
        if (exitHookInstalled) return;
        Runtime.getRuntime().addShutdownHook(new Thread(FoundryLocalManager::closeAtExit, "foundry-local-exit"));
        exitHookInstalled = true;
    }

    private static void closeAtExit() {
        FoundryLocalManager manager = open;
        if (manager == null) return;
        try {
            manager.close();
        } catch (RuntimeException | Error ignored) {
            // Best effort: a close failure must not block process exit.
        }
    }

    public String runtimeVersion() { return api.version; }
    public String nativeTarget() { return NativeApi.target(); }

    public Catalog catalog() {
        return catalog(CatalogType.PUBLIC);
    }

    public Catalog catalog(CatalogType type) {
        return catalog(type, Map.of());
    }

    /**
     * Gets a manager-owned catalog with source-specific settings.
     * Azure Registry requires {@code name} and accepts {@code credential}
     * ({@code azure_cli} or {@code workload_identity}). Options require native API v4.
     * Public/local option validation is performed by the native runtime.
     */
    public Catalog catalog(CatalogType type, Map<String, String> options) {
        NativeApi.outsideCallback();
        Objects.requireNonNull(type, "type");
        Map<String, String> settings = options == null ? Map.of() : Map.copyOf(options);
        settings.forEach((key, value) -> {
            if (key.isEmpty() || key.indexOf('\0') >= 0 || value.indexOf('\0') >= 0) {
                throw new IllegalArgumentException("Catalog options require nonempty keys and strings without NUL");
            }
        });
        synchronized (this) {
            checkOpen();
            CatalogKey key = new CatalogKey(type, settings);
            Catalog cached = catalogs.get(key);
            if (cached != null) return cached;

            Pointer catalogHandle;
            if (!settings.isEmpty() || type == CatalogType.AZURE_REGISTRY) {
                if (api.apiVersion < 4) {
                    throw new IllegalStateException("Catalog options require native API version 4 or newer");
                }
                PointerByReference pairs = new PointerByReference();
                api.root.call(NativeApi.Root.KEY_VALUE_PAIRS_CREATE, pairs);
                if (pairs.getValue() == null) {
                    throw new IllegalStateException("Native API returned null catalog options");
                }
                try {
                    settings.forEach((name, value) -> api.root.call(
                            NativeApi.Root.KEY_VALUE_PAIRS_ADD, pairs.getValue(), name, value));
                    catalogHandle = api.create(
                            api.root, NativeApi.Root.MANAGER_GET_CATALOG_WITH_OPTIONS,
                            handle, type.nativeValue(), pairs.getValue());
                } finally {
                    api.root.call(NativeApi.Root.KEY_VALUE_PAIRS_RELEASE, pairs.getValue());
                }
            } else {
                catalogHandle = api.create(
                            api.root,
                            NativeApi.Root.MANAGER_GET_CATALOG_BY_TYPE,
                            handle,
                            type.nativeValue());
            }
            Catalog catalog = catalogsByHandle.computeIfAbsent(
                    catalogHandle, value -> new Catalog(this, value, type));
            catalogs.put(key, catalog);
            return catalog;
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
            Throwable failure = null;
            try {
                api.check(api.root.pointer(NativeApi.Root.MANAGER_SHUTDOWN, handle));
            } catch (RuntimeException | Error e) {
                failure = NativeApi.preserveFailure(failure, e);
            }
            for (OwnedSession session : new ArrayList<>(sessions)) {
                try {
                    session.close();
                } catch (RuntimeException | Error e) {
                    failure = NativeApi.preserveFailure(failure, e);
                }
            }
            try {
                api.root.call(NativeApi.Root.MANAGER_RELEASE, handle);
            } catch (RuntimeException | Error e) {
                failure = NativeApi.preserveFailure(failure, e);
            } finally {
                catalogs.clear();
                catalogsByHandle.clear();
                handle = null;
                synchronized (FoundryLocalManager.class) {
                    open = null;
                }
            }
            NativeApi.rethrow(failure);
        }
    }
}
