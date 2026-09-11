// Copyright (c) Microsoft Corporation. Licensed under the MIT License.
package com.microsoft.foundry.local;

import com.sun.jna.Pointer;
import java.util.ArrayList;
import java.util.List;

/** Borrowed from a manager. Queries may fetch public catalog metadata, never model weights. */
public final class Catalog {
    private final FoundryLocalManager owner;
    private final Pointer handle;

    Catalog(FoundryLocalManager owner, Pointer handle) { this.owner = owner; this.handle = handle; }

    /** No alias fallback: the returned native identity must equal the requested name:version. */
    public Model getModel(String exactId) {
        NativeApi.outsideCallback();
        if (exactId == null || !exactId.matches("[A-Za-z0-9._-]+:[0-9]+")) {
            throw new IllegalArgumentException("An exact model ID in name:version form is required");
        }
        synchronized (owner) {
            owner.checkOpen();
            Model model = new Model(owner, owner.api.create(
                    owner.api.catalog, NativeApi.CatalogApi.GET_MODEL_VARIANT, handle, exactId));
            if (!model.info().id().equals(exactId)) {
                throw new IllegalStateException("Catalog returned a different model ID");
            }
            return model;
        }
    }

    public List<ModelInfo> models() {
        NativeApi.outsideCallback();
        synchronized (owner) {
            owner.checkOpen();
            NativeApi api = owner.api;
            Pointer list = api.create(api.catalog, NativeApi.CatalogApi.GET_MODELS, handle);
            try {
                List<ModelInfo> models = new ArrayList<>();
                long size = api.root.size(NativeApi.Root.MODEL_LIST_SIZE, list);
                for (long i = 0; i < size; i++) {
                    Model alias = new Model(owner, api.root.pointer(NativeApi.Root.MODEL_LIST_GET_AT, list, i));
                    Pointer variants = api.create(api.model, NativeApi.ModelApi.GET_VARIANTS, alias.handle);
                    try {
                        for (long j = 0; j < api.root.size(NativeApi.Root.MODEL_LIST_SIZE, variants); j++) {
                            models.add(new Model(
                                    owner,
                                    api.root.pointer(NativeApi.Root.MODEL_LIST_GET_AT, variants, j)).info());
                        }
                    } finally { api.root.call(NativeApi.Root.MODEL_LIST_RELEASE, variants); }
                }
                return List.copyOf(models);
            } finally { api.root.call(NativeApi.Root.MODEL_LIST_RELEASE, list); }
        }
    }
}
