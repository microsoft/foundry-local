// Copyright (c) Microsoft Corporation. Licensed under the MIT License.
import com.microsoft.foundry.local.CatalogType;
import com.microsoft.foundry.local.Configuration;
import com.microsoft.foundry.local.FoundryLocalManager;
import java.nio.file.Path;
import java.util.Map;

class AzureRegistryCatalog {
    public static void main(String[] args) {
        var configuration = Configuration.builder("RegistryDemo")
                .runtimeDirectory(Path.of(args[0]))
                .modelCacheDirectory(Path.of("model-cache"))
                .appDataDirectory(Path.of("app-data"))
                .build();
        try (var manager = new FoundryLocalManager(configuration)) {
            var catalog = manager.catalog(CatalogType.AZURE_REGISTRY,
                    Map.of("name", "azureml-dev", "credential", "azure_cli"));
            for (var variant : catalog.modelVariants()) {
                System.out.println("- " + variant.alias() + " " + variant.id());
            }
        }
    }
}
