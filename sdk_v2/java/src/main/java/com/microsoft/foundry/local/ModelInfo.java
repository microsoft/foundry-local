// Copyright (c) Microsoft Corporation. Licensed under the MIT License.
package com.microsoft.foundry.local;

/** Detached immutable metadata, valid even after the manager closes. */
public record ModelInfo(String id, String alias, String name, int version, String uri,
                        String executionProvider, String task, String license, String licenseDescription) {}
