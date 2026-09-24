// Copyright (c) Microsoft Corporation. Licensed under the MIT License.
package com.microsoft.foundry.local;

import java.nio.file.Path;

/** Checks runtime-version negotiation in a fresh JVM without a resident native library. */
public final class NativeLegacyRuntimeProbe {
    private NativeLegacyRuntimeProbe() {}

    public static void main(String[] args) {
        Path directory = Path.of(args[0]);
        try {
            new FoundryLocalManager(new Configuration("legacy-check", directory, directory, directory));
            throw new AssertionError("An older runtime was accepted");
        } catch (IllegalStateException e) {
            if (!e.getMessage().contains("C API 2")) throw e;
            System.out.println("legacy-runtime-rejected");
        }
    }
}
