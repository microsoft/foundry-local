// Copyright (c) Microsoft Corporation. Licensed under the MIT License.
package com.microsoft.foundry.local;

import java.util.NoSuchElementException;

/** An exact model ID was valid but is not available from the catalog. */
public final class ModelNotFoundException extends NoSuchElementException {
    private final String modelId;

    public ModelNotFoundException(String modelId) {
        super("Model not found: " + modelId);
        this.modelId = modelId;
    }

    public String modelId() { return modelId; }
}
