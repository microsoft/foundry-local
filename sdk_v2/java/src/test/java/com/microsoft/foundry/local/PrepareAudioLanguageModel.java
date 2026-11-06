// Copyright (c) Microsoft Corporation. Licensed under the MIT License.
package com.microsoft.foundry.local;

import java.nio.file.Path;

/** Explicit CI provisioning step; recognition tests never download their own model. */
public final class PrepareAudioLanguageModel {
    private PrepareAudioLanguageModel() {}

    public static void main(String[] args) {
        Configuration config = Configuration.builder("java-language-model-preparation")
                .runtimeDirectory(Path.of(required("FOUNDRY_LOCAL_NATIVE_BIN_DIR")))
                .modelCacheDirectory(Path.of(required("FOUNDRY_TEST_DATA_DIR")))
                .appDataDirectory(Path.of(required("FOUNDRY_TEST_APP_DATA_DIR")))
                .build();
        try (FoundryLocalManager manager = new FoundryLocalManager(config)) {
            Model model = manager.catalog().getModelVariant(required("FOUNDRY_TEST_MODEL"));
            System.out.println("Preparing " + model.info().id() + " with runtime " + manager.runtimeVersion());
            if (!model.isCached()) {
                model.download(new CancellationToken(), progress -> {});
            }
            if (!model.isCached()) throw new IllegalStateException("Model download did not populate the cache");
            System.out.println("Language test model is cached");
        }
    }

    private static String required(String name) {
        String value = System.getenv(name);
        if (value == null || value.isBlank()) throw new IllegalArgumentException("Missing " + name);
        return value;
    }
}
