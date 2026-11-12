// Copyright (c) Microsoft Corporation. Licensed under the MIT License.
package com.microsoft.foundry.local;

import static org.junit.jupiter.api.Assertions.*;

import com.sun.jna.Callback;
import com.sun.jna.CallbackReference;
import com.sun.jna.Memory;
import com.sun.jna.Pointer;
import com.sun.jna.ptr.PointerByReference;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import org.junit.jupiter.api.Test;

/** Mocks C function tables, not HTTP: networking and credential handling belong to native code. */
class CatalogOptionsTest {
    @Test void negotiatesTheNewestSupportedApiWithoutRequiringV4() {
        for (int supported : new int[] {4, 3, 2}) {
            List<Integer> requests = new ArrayList<>();
            Pointer table = new Pointer(42);
            NativeApi.NegotiatedApi api = NativeApi.requireApiVersion(version -> {
                requests.add(version);
                return version == supported ? table : null;
            });
            assertEquals(supported, api.version());
            assertSame(table, api.pointer());
            assertEquals(switch (supported) {
                case 4 -> List.of(4);
                case 3 -> List.of(4, 3);
                default -> List.of(4, 3, 2);
            }, requests);
        }
    }

    @Test void registryOptionsCrossTheV4BoundaryAsUtf8AndAreReleased() {
        NativeMock mock = new NativeMock(4);
        try (FoundryLocalManager manager = mock.manager()) {
            Map<String, String> options = Map.of("name", "registry-世界", "credential", "azure_cli");
            Catalog catalog = manager.catalog(CatalogType.AZURE_REGISTRY, options);
            assertEquals(CatalogType.AZURE_REGISTRY, catalog.type());
            assertEquals(List.of(new Request(2, options)), mock.optionRequests);
            assertEquals(1, mock.pairCreates);
            assertEquals(1, mock.pairReleases);
            assertTrue(mock.pairs.isEmpty());
            assertTrue(mock.typedRequests.isEmpty());
        }
    }

    @Test void cachesImmutableOptionsAndReusesWrappersForEqualNativePointers() {
        NativeMock mock = new NativeMock(4);
        try (FoundryLocalManager manager = mock.manager()) {
            Map<String, String> options = new LinkedHashMap<>();
            options.put("name", "registry");
            options.put("credential", "azure_cli");
            Catalog first = manager.catalog(CatalogType.AZURE_REGISTRY, options);
            Map<String, String> reordered = new LinkedHashMap<>();
            reordered.put("credential", "azure_cli");
            reordered.put("name", "registry");
            assertSame(first, manager.catalog(CatalogType.AZURE_REGISTRY, reordered));
            assertEquals(1, mock.optionRequests.size());

            options.put("credential", "workload_identity");
            assertSame(first, manager.catalog(CatalogType.AZURE_REGISTRY, options));
            assertEquals(2, mock.optionRequests.size());
            assertEquals("azure_cli", mock.optionRequests.get(0).options().get("credential"));
            assertEquals("workload_identity", mock.optionRequests.get(1).options().get("credential"));
            assertSame(first, manager.catalog(CatalogType.AZURE_REGISTRY, reordered));
            assertEquals(2, mock.optionRequests.size());

            mock.catalogHandle = new Pointer(203);
            assertNotSame(first, manager.catalog(CatalogType.AZURE_REGISTRY, Map.of("name", "other")));
        }
    }

    @Test void publicAndLocalKeepUsingTheExistingTypedRouteOnAllSupportedApis() {
        for (int version : new int[] {2, 3, 4}) {
            NativeMock mock = new NativeMock(version);
            try (FoundryLocalManager manager = mock.manager()) {
                Catalog publicCatalog = manager.catalog();
                assertSame(publicCatalog, manager.catalog(CatalogType.PUBLIC));
                assertSame(publicCatalog, manager.catalog(CatalogType.PUBLIC, null));
                assertSame(publicCatalog, manager.catalog(CatalogType.PUBLIC, Map.of()));
                mock.catalogHandle = new Pointer(204);
                Catalog local = manager.catalog(CatalogType.LOCAL);
                assertEquals(CatalogType.LOCAL, local.type());
                assertSame(local, manager.catalog(CatalogType.LOCAL, Map.of()));
                assertEquals(List.of(0, 1), mock.typedRequests);
                assertTrue(mock.optionRequests.isEmpty());
                assertEquals(0, mock.pairCreates);
            }
        }
    }

    @Test void v2AndV3RejectOptionsBeforeReadingTheV4Slot() {
        for (int version : new int[] {2, 3}) {
            NativeMock mock = new NativeMock(version);
            try (FoundryLocalManager manager = mock.manager()) {
                assertThrows(IllegalStateException.class, () -> manager.catalog(CatalogType.AZURE_REGISTRY));
                assertThrows(IllegalStateException.class, () ->
                        manager.catalog(CatalogType.AZURE_REGISTRY, Map.of("name", "registry")));
                IllegalStateException error = assertThrows(IllegalStateException.class, () ->
                        manager.catalog(CatalogType.PUBLIC, Map.of("unsupported", "value")));
                assertTrue(error.getMessage().contains("version 4"));
                assertEquals(0, mock.pairCreates);
                assertTrue(mock.optionRequests.isEmpty());
                assertNotNull(manager.catalog());
            }
        }
    }

    @Test void missingRegistrySettingsStillUseV4AndNativeValidationErrorsAreNotCached() {
        NativeMock mock = new NativeMock(4);
        try (FoundryLocalManager manager = mock.manager()) {
            mock.status = new Pointer(300);
            FoundryLocalException error = assertThrows(FoundryLocalException.class, () ->
                    manager.catalog(CatalogType.AZURE_REGISTRY));
            assertEquals(7, error.code());
            assertEquals("mock catalog failure", error.getMessage());
            assertEquals(List.of(new Request(2, Map.of())), mock.optionRequests);
            assertEquals(1, mock.statusReleases);
            assertEquals(1, mock.pairReleases);
            mock.status = null;
            assertNotNull(manager.catalog(CatalogType.AZURE_REGISTRY));
            assertEquals(2, mock.optionRequests.size());
        }
    }

    @Test void publicOptionsAreForwardedForNativeValidationAndReleasedOnError() {
        NativeMock mock = new NativeMock(4);
        try (FoundryLocalManager manager = mock.manager()) {
            mock.status = new Pointer(300);
            Map<String, String> options = Map.of("unknown", "value");
            assertThrows(FoundryLocalException.class, () -> manager.catalog(CatalogType.PUBLIC, options));
            assertEquals(List.of(new Request(0, options)), mock.optionRequests);
            assertEquals(1, mock.pairReleases);
            assertTrue(mock.pairs.isEmpty());
        }
    }

    @Test void rejectsNullNativeCatalogsAndReleasesOptionsBeforeRetrying() {
        NativeMock mock = new NativeMock(4);
        try (FoundryLocalManager manager = mock.manager()) {
            mock.catalogHandle = null;
            assertThrows(IllegalStateException.class, () ->
                    manager.catalog(CatalogType.AZURE_REGISTRY, Map.of("name", "registry")));
            assertEquals(1, mock.pairReleases);
            mock.catalogHandle = new Pointer(200);
            assertNotNull(manager.catalog(CatalogType.AZURE_REGISTRY, Map.of("name", "registry")));
            assertEquals(2, mock.optionRequests.size());
        }
    }

    @Test void allocationFailureDoesNotCallTheCatalogFunction() {
        NativeMock mock = new NativeMock(4);
        try (FoundryLocalManager manager = mock.manager()) {
            mock.failAllocation = true;
            assertThrows(IllegalStateException.class, () ->
                    manager.catalog(CatalogType.AZURE_REGISTRY, Map.of("name", "registry")));
            assertTrue(mock.optionRequests.isEmpty());
            assertEquals(0, mock.pairReleases);
        }
    }

    @Test void invalidStringsAreRejectedBeforeAllocatingNativeOptions() {
        NativeMock mock = new NativeMock(4);
        try (FoundryLocalManager manager = mock.manager()) {
            for (Map<String, String> invalid : List.of(
                    Map.of("", "value"), Map.of("bad\0key", "value"), Map.of("name", "bad\0value"))) {
                assertThrows(IllegalArgumentException.class, () ->
                        manager.catalog(CatalogType.AZURE_REGISTRY, invalid));
            }
            Map<String, String> nullValue = new HashMap<>();
            nullValue.put("name", null);
            assertThrows(NullPointerException.class, () -> manager.catalog(CatalogType.AZURE_REGISTRY, nullValue));
            Map<String, String> nullKey = new HashMap<>();
            nullKey.put(null, "value");
            assertThrows(NullPointerException.class, () -> manager.catalog(CatalogType.AZURE_REGISTRY, nullKey));
            assertThrows(NullPointerException.class, () -> manager.catalog(null, Map.of()));
            assertEquals(0, mock.pairCreates);
        }
    }

    @Test void cachedCatalogsCannotEscapeManagerCloseOrCallbackGuards() {
        NativeMock mock = new NativeMock(4);
        FoundryLocalManager manager = mock.manager();
        try {
            manager.catalog(CatalogType.AZURE_REGISTRY, Map.of("name", "registry"));
            NativeApi.IN_CALLBACK.set(true);
            try {
                assertThrows(IllegalStateException.class, () ->
                        manager.catalog(CatalogType.AZURE_REGISTRY, Map.of("name", "registry")));
            } finally {
                NativeApi.IN_CALLBACK.remove();
            }
        } finally {
            manager.close();
        }
        assertThrows(IllegalStateException.class, () ->
                manager.catalog(CatalogType.AZURE_REGISTRY, Map.of("name", "registry")));
        assertEquals(1, mock.optionRequests.size());
        assertEquals(1, mock.managerReleases);
        manager.close();
        assertEquals(1, mock.managerReleases);
    }

    private record Request(int type, Map<String, String> options) {}

    public interface GetTable extends Callback { Pointer invoke(); }
    public interface Create extends Callback { Pointer invoke(Pointer value, PointerByReference output); }
    public interface Status extends Callback { Pointer invoke(Pointer value); }
    public interface SetLevel extends Callback { Pointer invoke(Pointer value, int level); }
    public interface SetOption extends Callback { Pointer invoke(Pointer value, Pointer option); }
    public interface Release extends Callback { void invoke(Pointer value); }
    public interface CreatePairs extends Callback { void invoke(PointerByReference output); }
    public interface AddPair extends Callback { void invoke(Pointer pairs, Pointer key, Pointer value); }
    public interface GetCatalog extends Callback { Pointer invoke(Pointer manager, int type, PointerByReference output); }
    public interface GetCatalogWithOptions extends Callback {
        Pointer invoke(Pointer manager, int type, Pointer pairs, PointerByReference output);
    }
    public interface GetCode extends Callback { int invoke(Pointer status); }
    public interface GetMessage extends Callback { Pointer invoke(Pointer status); }

    private static final class NativeMock {
        final List<Callback> callbacks = new ArrayList<>();
        final Memory root;
        final Memory config = new Memory(11 * 8L);
        final Memory unusedTable = new Memory(8);
        final Memory errorMessage = NativeApi.utf8("mock catalog failure");
        final List<Request> optionRequests = new ArrayList<>();
        final List<Integer> typedRequests = new ArrayList<>();
        final Map<Pointer, Map<String, String>> pairs = new HashMap<>();
        final int version;
        Pointer catalogHandle = new Pointer(200);
        Pointer status;
        boolean failAllocation;
        int pairCreates, pairReleases, statusReleases, managerReleases;

        NativeMock(int version) {
            this.version = version;
            // v2/v3 tables intentionally end before the appended v4 slot.
            root = new Memory((version >= 4 ? 31 : 30) * 8L);
            root.clear();
            config.clear();
            unusedTable.clear();
            for (int slot : new int[] {10, 12, 13, 14}) {
                set(root, slot, (GetTable) () -> unusedTable);
            }
            set(root, 11, (GetTable) () -> config);
            set(config, 0, (Create) (name, output) -> { output.setValue(new Pointer(100)); return null; });
            set(config, 1, (Release) ignored -> {});
            set(config, 2, (SetLevel) (handle, level) -> null);
            for (int slot : new int[] {3, 5, 10}) {
                set(config, slot, (SetOption) (handle, option) -> null);
            }
            set(root, 4, (Create) (configuration, output) -> { output.setValue(new Pointer(101)); return null; });
            set(root, 5, (Release) ignored -> managerReleases++);
            set(root, 27, (Status) ignored -> null);
            set(root, 15, (CreatePairs) output -> {
                pairCreates++;
                if (!failAllocation) {
                    Pointer handle = new Pointer(1000 + pairCreates);
                    pairs.put(handle, new HashMap<>());
                    output.setValue(handle);
                }
            });
            set(root, 16, (AddPair) (handle, key, value) ->
                    pairs.get(handle).put(NativeApi.text(key), NativeApi.text(value)));
            set(root, 20, (Release) handle -> { pairReleases++; pairs.remove(handle); });
            set(root, 29, (GetCatalog) (manager, type, output) -> {
                typedRequests.add(type);
                output.setValue(catalogHandle);
                return status;
            });
            if (version >= 4) {
                set(root, 30, (GetCatalogWithOptions) (manager, type, options, output) -> {
                    optionRequests.add(new Request(type, Map.copyOf(pairs.get(options))));
                    output.setValue(catalogHandle);
                    return status;
                });
            }
            set(root, 1, (Release) ignored -> statusReleases++);
            set(root, 2, (GetCode) ignored -> 7);
            set(root, 3, (GetMessage) ignored -> errorMessage);
        }

        private void set(Memory table, int slot, Callback callback) {
            callbacks.add(callback);
            table.setPointer(slot * 8L, CallbackReference.getFunctionPointer(callback));
        }

        FoundryLocalManager manager() {
            Path directory = Path.of(".");
            NativeApi api = new NativeApi(directory, "mock", requested -> requested == version ? root : null);
            FoundryLocalManager manager = new FoundryLocalManager(Configuration.builder("catalog-test")
                    .runtimeDirectory(directory).appDataDirectory(directory).modelCacheDirectory(directory).build(), api);
            pairCreates = pairReleases = 0;
            return manager;
        }
    }
}
