from foundry_local_sdk import (
    CatalogType, ChatSession, Configuration, FoundryLocalManager,
    MessageItem, Request, TextItem, TextItemType,
)

manager = FoundryLocalManager(Configuration(app_name="RegistryDemo"))
try:
    result = manager.download_and_register_eps()
    if not result.success:
        raise RuntimeError(result.status)
    catalog = manager.get_catalog(CatalogType.AZURE_REGISTRY, {"name": "azureml-staging", "credential": "azure_cli"})
    model = catalog.get_model("qwen2.5-0.5b")
    if model is None:
        raise RuntimeError("qwen2.5-0.5b is not available in this registry")
    print("Using", model.id)
    model.download()
    model.load()
    try:
        with ChatSession(model) as session:
            with Request().add_item(MessageItem.user("What is 2 + 2? Answer briefly.")) as request:
                with session.process_request(request) as response:
                    for item in response:
                        if isinstance(item, MessageItem):
                            for part in item.parts:
                                if isinstance(part, TextItem) and part.type == TextItemType.DEFAULT:
                                    print(part.text)
    finally:
        model.unload()
finally:
    manager.close()