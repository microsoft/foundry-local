// Copyright (c) Microsoft Corporation. Licensed under the MIT License.
package com.microsoft.foundry.local;

import com.sun.jna.Pointer;
import java.util.ArrayList;
import java.util.List;
import java.util.regex.Pattern;

/** Borrowed from a manager. Queries catalog metadata but never download model weights. */
public final class Catalog {
    private static final Pattern EXACT_MODEL_ID =
            Pattern.compile("[A-Za-z0-9][A-Za-z0-9._-]*:(0|[1-9][0-9]*)");
    private final FoundryLocalManager owner;
    private final Pointer handle;
    private final CatalogType type;

    Catalog(FoundryLocalManager owner, Pointer handle, CatalogType type) {
        this.owner = owner;
        this.handle = handle;
        this.type = type;
    }

    public CatalogType type() { return type; }

    /** Looks up a model by its catalog alias. */
    public Model getModel(String alias) {
        NativeApi.outsideCallback();
        if (alias == null || alias.isBlank() || alias.indexOf('\0') >= 0) {
            throw new IllegalArgumentException("A nonempty model alias without NUL is required");
        }
        synchronized (owner) {
            owner.checkOpen();
            Pointer modelHandle = owner.api.output(
                    owner.api.catalog, NativeApi.CatalogApi.GET_MODEL, handle, alias);
            return new Model(owner, requireModelHandle(alias, modelHandle));
        }
    }

    /**
     * Looks up an exact name:version without alias fallback.
     *
     * @throws ModelNotFoundException if the exact ID is valid but unavailable
     */
    public Model getModelVariant(String exactId) {
        NativeApi.outsideCallback();
        validateExactId(exactId);
        synchronized (owner) {
            owner.checkOpen();
            Pointer modelHandle = owner.api.output(
                    owner.api.catalog, NativeApi.CatalogApi.GET_MODEL_VARIANT, handle, exactId);
            Model model = new Model(owner, requireModelHandle(exactId, modelHandle));
            if (!model.info().id().equals(exactId)) {
                throw new IllegalStateException("Catalog returned a different model ID");
            }
            return model;
        }
    }

    private static void validateExactId(String exactId) {
        if (exactId == null || !EXACT_MODEL_ID.matcher(exactId).matches()) {
            throw new IllegalArgumentException("A canonical model ID in name:version form is required");
        }
        try {
            Integer.parseInt(exactId.substring(exactId.indexOf(':') + 1));
        } catch (NumberFormatException e) {
            throw new IllegalArgumentException("The model ID version is out of range", e);
        }
    }

    static Pointer requireModelHandle(String exactId, Pointer handle) {
        if (handle == null) throw new ModelNotFoundException(exactId);
        return handle;
    }

    /** One entry per catalog alias. */
    public List<ModelInfo> models() {
        return listModels(false);
    }

    /** All variants of all catalog aliases. */
    public List<ModelInfo> modelVariants() {
        return listModels(true);
    }

    private List<ModelInfo> listModels(boolean variants) {
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
                    if (!variants) {
                        models.add(alias.info());
                    } else {
                        Pointer listOfVariants = api.create(api.model, NativeApi.ModelApi.GET_VARIANTS, alias.handle);
                        try {
                            for (long j = 0; j < api.root.size(NativeApi.Root.MODEL_LIST_SIZE, listOfVariants); j++) {
                                models.add(new Model(
                                        owner,
                                        api.root.pointer(NativeApi.Root.MODEL_LIST_GET_AT, listOfVariants, j)).info());
                            }
                        } finally { api.root.call(NativeApi.Root.MODEL_LIST_RELEASE, listOfVariants); }
                    }
                }
                return List.copyOf(models);
            } finally { api.root.call(NativeApi.Root.MODEL_LIST_RELEASE, list); }
        }
    }
}
