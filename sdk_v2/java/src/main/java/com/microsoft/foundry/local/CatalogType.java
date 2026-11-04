// Copyright (c) Microsoft Corporation. Licensed under the MIT License.
package com.microsoft.foundry.local;

/** Selects the manager-owned model catalog. */
public enum CatalogType {
    PUBLIC(0),
    LOCAL(1);

    private final int nativeValue;

    CatalogType(int nativeValue) {
        this.nativeValue = nativeValue;
    }

    int nativeValue() {
        return nativeValue;
    }
}
