// Copyright (c) Microsoft Corporation. Licensed under the MIT License.
package com.microsoft.foundry.local;

/** Native Foundry Local log levels. */
public enum LogLevel {
    VERBOSE(0), DEBUG(1), INFO(2), WARNING(3), ERROR(4), FATAL(5);

    private final int nativeValue;

    LogLevel(int nativeValue) { this.nativeValue = nativeValue; }

    int nativeValue() { return nativeValue; }
}
