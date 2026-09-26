// Copyright (c) Microsoft Corporation. Licensed under the MIT License.
package com.microsoft.foundry.local;

import java.nio.file.Path;
import java.util.Objects;

/**
 * Immutable native runtime configuration. Building a configuration never loads native code or downloads assets.
 *
 * <p>The first runtime directory loaded is retained for the JVM lifetime. Recreated managers must use the same
 * resolved runtime directory.
 */
public final class Configuration {
    private final String appName;
    private final Path runtimeDirectory;
    private final Path modelCacheDirectory;
    private final Path appDataDirectory;
    private final LogLevel logLevel;
    private final boolean disableNonessentialTelemetry;

    private Configuration(Builder builder) {
        appName = builder.appName;
        runtimeDirectory = builder.runtimeDirectory;
        modelCacheDirectory = builder.modelCacheDirectory;
        appDataDirectory = builder.appDataDirectory;
        logLevel = builder.logLevel;
        disableNonessentialTelemetry = builder.disableNonessentialTelemetry;
    }

    public static Builder builder(String appName) {
        return new Builder(appName);
    }

    public String appName() { return appName; }
    public Path runtimeDirectory() { return runtimeDirectory; }
    public Path modelCacheDirectory() { return modelCacheDirectory; }
    public Path appDataDirectory() { return appDataDirectory; }
    public LogLevel logLevel() { return logLevel; }
    public boolean disableNonessentialTelemetry() { return disableNonessentialTelemetry; }

    public static final class Builder {
        private final String appName;
        private Path runtimeDirectory;
        private Path modelCacheDirectory;
        private Path appDataDirectory;
        private LogLevel logLevel = LogLevel.FATAL;
        private boolean disableNonessentialTelemetry = true;

        private Builder(String appName) {
            this.appName = validateAppName(appName);
        }

        public Builder runtimeDirectory(Path value) {
            runtimeDirectory = normalize(value, "runtimeDirectory");
            return this;
        }

        public Builder modelCacheDirectory(Path value) {
            modelCacheDirectory = normalize(value, "modelCacheDirectory");
            return this;
        }

        public Builder appDataDirectory(Path value) {
            appDataDirectory = normalize(value, "appDataDirectory");
            return this;
        }

        public Builder logLevel(LogLevel value) {
            logLevel = Objects.requireNonNull(value, "logLevel");
            return this;
        }

        public Builder disableNonessentialTelemetry(boolean value) {
            disableNonessentialTelemetry = value;
            return this;
        }

        public Configuration build() {
            Objects.requireNonNull(runtimeDirectory, "runtimeDirectory");
            Objects.requireNonNull(modelCacheDirectory, "modelCacheDirectory");
            Objects.requireNonNull(appDataDirectory, "appDataDirectory");
            return new Configuration(this);
        }
    }

    private static String validateAppName(String value) {
        Objects.requireNonNull(value, "appName");
        if (value.isBlank() || value.indexOf('\0') >= 0) {
            throw new IllegalArgumentException("appName must be nonempty and contain no NUL");
        }
        return value;
    }

    private static Path normalize(Path value, String name) {
        return Objects.requireNonNull(value, name).toAbsolutePath().normalize();
    }
}
