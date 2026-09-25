// Copyright (c) Microsoft Corporation. Licensed under the MIT License.
package com.microsoft.foundry.local;

/** Manager-owned inference session, independent of the inference scenario. */
abstract class OwnedSession implements AutoCloseable {
    abstract Model model();

    @Override
    public abstract void close();
}
