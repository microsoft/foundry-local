// Copyright (c) Microsoft Corporation. Licensed under the MIT License.
package smoke;

import com.microsoft.foundry.local.Catalog;
import com.microsoft.foundry.local.Configuration;
import com.microsoft.foundry.local.FoundryLocalManager;
import com.microsoft.foundry.local.LogLevel;
import com.microsoft.foundry.local.TranscriptionResult;
import com.sun.jna.Pointer;
import java.nio.file.Path;

public final class PackagedConsumer {
    private PackagedConsumer() {}

    public static Configuration configuration(Path path) {
        return Configuration.builder("smoke")
                .runtimeDirectory(path)
                .modelCacheDirectory(path)
                .appDataDirectory(path)
                .logLevel(LogLevel.DEBUG)
                .disableNonessentialTelemetry(true)
                .build();
    }

    public static String modelId(FoundryLocalManager manager, String alias, String exactId) {
        Catalog catalog = manager.catalog();
        return catalog.getModel(alias).info().id() + catalog.getModelVariant(exactId).info().id();
    }

    public static Class<TranscriptionResult> resultType() { return TranscriptionResult.class; }

    public static Pointer transitiveJnaType() { return Pointer.NULL; }
}
