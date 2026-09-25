// Copyright (c) Microsoft Corporation. Licensed under the MIT License.
package com.microsoft.foundry.local;

import static org.junit.jupiter.api.Assertions.*;
import com.sun.jna.Pointer;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;

class NativeContractTest {
    @TempDir Path temporary;

    @Test void missingModelReportsTheRequestedId() {
        ModelNotFoundException error = assertThrows(
                ModelNotFoundException.class,
                () -> Catalog.requireModelHandle("missing-model:9", null));
        assertEquals("missing-model:9", error.modelId());
        assertTrue(error.getMessage().contains("missing-model:9"));
    }

    @Test void modelLookupRejectsNonCanonicalIdsBeforeQueryingNativeCatalog() {
        Catalog catalog = new Catalog(null, null, CatalogType.PUBLIC);
        for (String invalidId : new String[] {
            "", "missing-version", ":1", ".name:1", "model:", "model:1:2", "model:-1",
            "model:+1", "model:01", "model:2147483648", "model/path:1", "model name:1"
        }) {
            assertThrows(IllegalArgumentException.class, () -> catalog.getModelVariant(invalidId), invalidId);
        }
    }

    @Test void aliasLookupRejectsInvalidAliasesBeforeQueryingNativeCatalog() {
        Catalog catalog = new Catalog(null, null, CatalogType.PUBLIC);
        for (String invalidAlias : new String[] {"", " ", "bad\0alias"}) {
            assertThrows(IllegalArgumentException.class, () -> catalog.getModel(invalidAlias));
        }
        assertThrows(IllegalArgumentException.class, () -> catalog.getModel(null));
    }

    @Test void refusesAV1OnlyRuntimeBeforeReadingFunctionTables() {
        AtomicInteger requestedVersion = new AtomicInteger();
        IllegalStateException error = assertThrows(IllegalStateException.class, () ->
                NativeApi.requireApiVersion(version -> {
                    requestedVersion.set(version);
                    return version == 1 ? new Pointer(1) : null;
                }));
        assertEquals(2, requestedVersion.get());
        assertTrue(error.getMessage().contains("C API 2"));
    }

    @Test void matchesPackaged64BitStructSizes() {
        assertEquals(16, new NativeApi.CallbackData().size());
        assertEquals(72, new NativeApi.AudioData().size());
        assertEquals(48, new NativeApi.BytesData().size());
        assertEquals(64, new NativeApi.SegmentData().size());
        assertEquals(48, new NativeApi.ResultData().size());
    }

    @Test void runtimeDirectoryMustContainTheCurrentPlatformLibrary() {
        IllegalArgumentException error =
                assertThrows(IllegalArgumentException.class, () -> NativeApi.findFoundryLibrary(temporary));
        assertTrue(error.getMessage().contains("Foundry Local native library"));
    }

    @Test void resolvesTheCurrentPlatformLibraryWithoutAReleaseSpecificHashLock() throws Exception {
        Path library = temporary.resolve(NativeApi.foundryLibraryName());
        Files.createFile(library);
        assertEquals(library, NativeApi.findFoundryLibrary(temporary));
    }

    @Test void callbackLifecycleReentrancyIsRejected() {
        NativeApi.IN_CALLBACK.set(true);
        try { assertThrows(IllegalStateException.class, NativeApi::outsideCallback); }
        finally { NativeApi.IN_CALLBACK.remove(); }
    }

    @Test void cleanupPreservesTheFirstFailureAndSuppressesLaterFailures() {
        IllegalStateException first = new IllegalStateException("transcription");
        IllegalArgumentException second = new IllegalArgumentException("session");
        assertSame(first, NativeApi.preserveFailure(first, second));
        assertArrayEquals(new Throwable[] {second}, first.getSuppressed());
        assertSame(second, NativeApi.preserveFailure(null, second));
        assertSame(first, assertThrows(IllegalStateException.class, () -> NativeApi.rethrow(first)));
    }

    @Test void modelInfoIsAnImmutableDetachedValue() {
        Map<String, String> settings = new LinkedHashMap<>();
        settings.put("temperature", null);
        ModelInfo info = new ModelInfo(
                "model:1", "model", "model", 1, "uri", DeviceType.CPU, "CPUExecutionProvider",
                "chat-completion", true, Map.of("task", "chat-completion"), Map.of("context_length", 4096L),
                settings);

        assertEquals("chat-completion", info.getStringProperty("task"));
        assertEquals(4096L, info.getIntProperty("context_length", -1));
        assertEquals(4096L, info.contextLength());
        assertNull(info.supportsToolCalling());
        assertEquals(-1L, info.getIntProperty("missing", -1));
        assertTrue(info.modelSettings().containsKey("temperature"));
        assertNull(info.modelSettings().get("temperature"));
        assertThrows(UnsupportedOperationException.class, () -> info.intProperties().put("new", 1L));
        assertThrows(IllegalArgumentException.class, () -> info.getStringProperty(""));
        assertThrows(IllegalArgumentException.class, () -> info.getIntProperty("bad\0key", 0));
    }

}
