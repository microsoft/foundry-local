from foundry_local_sdk import CatalogType, Configuration, FoundryLocalManager

manager = FoundryLocalManager(Configuration(app_name="RegistryDemo"))
try:
    result = manager.download_and_register_eps()
    if not result.success:
        raise RuntimeError(result.status)
    catalog = manager.get_catalog(CatalogType.AZURE_REGISTRY, {"name": "azureml-dev", "credential": "azure_cli"})
    for model in catalog.list_models():
        for variant in model.variants:
            print("-", model.alias, variant.id)
finally:
    manager.close()