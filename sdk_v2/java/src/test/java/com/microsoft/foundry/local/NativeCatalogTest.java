// Copyright (c) Microsoft Corporation. Licensed under the MIT License.
package com.microsoft.foundry.local;

import static org.junit.jupiter.api.Assertions.*;
import static org.junit.jupiter.api.Assumptions.assumeTrue;

import java.nio.file.Path;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;

class NativeCatalogTest {
    @TempDir Path temporary;

    @Test void publicAndLocalCatalogsExposeDetachedGeneralModelMetadata() {
        String runtime = setting("foundry.test.runtime", "FOUNDRY_LOCAL_NATIVE_BIN_DIR");
        String cache = setting("foundry.test.cache", "FOUNDRY_TEST_DATA_DIR");
        String modelId = setting("foundry.test.model", "FOUNDRY_TEST_MODEL");
        if (Boolean.getBoolean("foundry.test.native.required")) {
            assertNotNull(runtime, "Missing native test runtime");
            assertNotNull(cache, "Missing native test cache");
            assertNotNull(modelId, "Missing native test model");
        } else {
            assumeTrue(runtime != null && cache != null && modelId != null,
                    "Configure the native test runtime, cache, and model");
        }

        Configuration config = Configuration.builder("java-catalog-test")
                .runtimeDirectory(Path.of(runtime))
                .modelCacheDirectory(Path.of(cache))
                .appDataDirectory(temporary)
                .build();
        ModelInfo detached;
        try (FoundryLocalManager manager = new FoundryLocalManager(config)) {
            Catalog publicCatalog = manager.catalog();
            assertEquals(CatalogType.PUBLIC, publicCatalog.type());
            assertSame(publicCatalog, manager.catalog(CatalogType.PUBLIC));
            Catalog localCatalog = manager.catalog(CatalogType.LOCAL);
            assertEquals(CatalogType.LOCAL, localCatalog.type());
            assertSame(localCatalog, manager.catalog(CatalogType.LOCAL));
            assertNotNull(localCatalog.models());

            detached = publicCatalog.getModelVariant(modelId).info();
            assertEquals("automatic-speech-recognition", detached.task());
            assertEquals(detached.task(), detached.getStringProperty(ModelProperties.TASK));
            assertNotEquals(DeviceType.INVALID, detached.deviceType());
            assertNotNull(detached.modelSettings());
            assertThrows(UnsupportedOperationException.class, () ->
                    detached.stringProperties().put(ModelProperties.TASK, "changed"));
        }

        assertEquals("automatic-speech-recognition", detached.task());
        assertEquals(detached.task(), detached.stringProperties().get(ModelProperties.TASK));
    }

    private static String setting(String property, String environmentVariable) {
        String value = System.getProperty(property);
        if (value == null) value = System.getenv(environmentVariable);
        return value == null || value.isBlank() ? null : value;
    }
}
