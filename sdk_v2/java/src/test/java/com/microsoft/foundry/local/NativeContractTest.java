// Copyright (c) Microsoft Corporation. Licensed under the MIT License.
package com.microsoft.foundry.local;

import static org.junit.jupiter.api.Assertions.*;
import java.nio.file.Files;
import java.nio.file.Path;
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

}
