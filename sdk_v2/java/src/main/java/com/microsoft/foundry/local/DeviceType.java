// Copyright (c) Microsoft Corporation. Licensed under the MIT License.
package com.microsoft.foundry.local;

/** Hardware device type used by a model variant. */
public enum DeviceType {
    INVALID,
    CPU,
    GPU,
    NPU;

    static DeviceType fromNative(int value) {
        return switch (value) {
            case 1 -> CPU;
            case 2 -> GPU;
            case 3 -> NPU;
            default -> INVALID;
        };
    }
}
