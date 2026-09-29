// Copyright (c) Microsoft Corporation. Licensed under the MIT License.
package com.microsoft.foundry.local;

import static org.junit.jupiter.api.Assertions.*;
import static org.junit.jupiter.api.Assumptions.assumeTrue;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.TimeUnit;
import org.junit.jupiter.api.Test;

class NativeLegacyRuntimeTest {
    @Test void rejectsReleasedLegacyRuntimeWithoutAccessingItsFunctionTables() throws Exception {
        String directory = System.getProperty("foundry.test.legacy.runtime",
                System.getenv("FOUNDRY_TEST_LEGACY_BIN_DIR"));
        if (Boolean.getBoolean("foundry.test.legacy.required")) {
            assertNotNull(directory, "Missing released legacy runtime for the required ABI test");
            assertFalse(directory.isBlank(), "Missing released legacy runtime for the required ABI test");
        } else {
            assumeTrue(directory != null && !directory.isBlank(), "Configure a released legacy runtime");
        }
        Process process = NativeTestProcess.start(NativeLegacyRuntimeProbe.class, directory);
        try {
            assertTrue(process.waitFor(30, TimeUnit.SECONDS), "Legacy runtime probe did not exit");
            String output = new String(process.getInputStream().readAllBytes(), StandardCharsets.UTF_8);
            assertEquals(0, process.exitValue(), output);
            assertTrue(output.contains("legacy-runtime-rejected"), output);
        } finally {
            if (process.isAlive()) {
                process.destroyForcibly();
                process.waitFor();
            }
        }
    }
}
