// Copyright (c) Microsoft Corporation. Licensed under the MIT License.
package com.microsoft.foundry.local;

import java.nio.file.Path;
import java.util.Objects;

/**
 * Explicit locations only. Construction never loads native code or downloads assets.
 *
 * <p>The first runtime directory loaded is retained for the JVM lifetime. Recreated managers
 * must use the same resolved runtime directory.
 */
public record Configuration(String appName, Path runtimeDirectory, Path modelCacheDirectory,
                            Path appDataDirectory, LogLevel logLevel, boolean disableNonessentialTelemetry) {
    public Configuration(String appName, Path runtimeDirectory, Path modelCacheDirectory, Path appDataDirectory) {
        this(appName, runtimeDirectory, modelCacheDirectory, appDataDirectory, LogLevel.FATAL, true);
    }

    public Configuration {
        Objects.requireNonNull(appName, "appName");
        Objects.requireNonNull(logLevel, "logLevel");
        if (appName.isBlank() || appName.indexOf('\0') >= 0) {
            throw new IllegalArgumentException("appName must be nonempty and contain no NUL");
        }
        runtimeDirectory = Objects.requireNonNull(runtimeDirectory).toAbsolutePath().normalize();
        modelCacheDirectory = Objects.requireNonNull(modelCacheDirectory).toAbsolutePath().normalize();
        appDataDirectory = Objects.requireNonNull(appDataDirectory).toAbsolutePath().normalize();
    }
}
