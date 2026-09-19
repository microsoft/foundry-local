// Copyright (c) Microsoft Corporation. Licensed under the MIT License.
package com.microsoft.foundry.local;

/** Native errors retain the C ABI error code; cancellation is code 5. */
public final class FoundryLocalException extends RuntimeException {
    private final int code;

    public FoundryLocalException(int code, String message) {
        super(message);
        this.code = code;
    }

    public int code() { return code; }
}
