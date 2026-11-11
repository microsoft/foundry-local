#!/usr/bin/env python3
# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.
"""Local Azure Registry model catalog smoke test using the active Azure CLI login.

    az login
    python azure_registry_model_catalog.py --model-id smollm2-135m-instruct-generic-cpu:1

Lists the latest version of each available hardware variant and its alias, then runs the selected model.
Use --model-id to select an older version directly. Downloads use the default cache.
The script never publishes models, changes tags, or removes cached model files.
"""

import argparse
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--registry", default="prathikrao-pmc-test")
    selection = parser.add_mutually_exclusive_group(required=True)
    selection.add_argument("--model-id", help="Exact <variant-name>:<positive-version>")
    selection.add_argument("--alias", help="Resolve the best compatible variant interactively")
    parser.add_argument("--prompt", default="What is the capital of France? Reply briefly.")
    parser.add_argument("--max-output-tokens", type=int, default=128)
    args = parser.parse_args()
    if args.max_output_tokens <= 0:
        parser.error("--max-output-tokens must be positive")

    from foundry_local_sdk import (
        CatalogType, ChatSession, Configuration, FoundryLocalManager, MessageItem,
        Request, RequestOptions, SearchOptions, TextItem, TextItemType,
    )

    manager = FoundryLocalManager(Configuration(
        app_name="AzureRegistryModelCatalogE2E",
        disable_nonessential_telemetry=True,
    ))
    model = None
    try:
        ep_result = manager.download_and_register_eps()
        print(f"Execution providers: {ep_result.status}", flush=True)
        if ep_result.failed_eps:
            print(f"Warning: unavailable execution providers: {', '.join(ep_result.failed_eps)}", flush=True)
        catalog = manager.get_catalog(CatalogType.AZURE_REGISTRY, registry_name=args.registry)
        print(f"Registry: {args.registry}", flush=True)
        rows = sorted(
            (group.alias, variant.id)
            for group in catalog.list_models()
            for variant in group.variants
        )
        alias_width = max([len("Alias")] + [len(alias) for alias, _ in rows])
        model_width = max([len("Model ID")] + [len(model_id) for _, model_id in rows])
        print(f"\nAvailable models ({len(rows)} variants):")
        print(f"{'Alias':<{alias_width}}  Model ID")
        print(f"{'-' * alias_width}  {'-' * model_width}")
        for alias, model_id in rows:
            print(f"{alias:<{alias_width}}  {model_id}")
        print(flush=True)
        
        model = catalog.get_model_variant(args.model_id) if args.model_id else catalog.get_model(args.alias)
        if model is None:
            raise RuntimeError("No compatible model found. Check model ID and tags: "
                               "alias, task, device, execution_provider, os, architecture.")
        print(f"Selected: {model.id}", flush=True)
        model.download()
        if not model.is_cached or not (Path(model.get_path()) / "genai_config.json").is_file():
            raise RuntimeError("Download failed and/or did not produce a completed GenAI package.")

        print("Download/cache checks passed; loading model.", flush=True)
        model.load()
        if not model.is_loaded:
            raise RuntimeError("Model did not enter the loaded state.")
        with ChatSession(model) as session:
            session.set_options(RequestOptions(search=SearchOptions(
                temperature=0, max_output_tokens=args.max_output_tokens,
            )))
            with Request().add_item(MessageItem.user(args.prompt)) as request:
                with session.process_request(request) as response:
                    text = []
                    for item in response:
                        parts = item.parts if isinstance(item, MessageItem) else [item]
                        text.extend(part.text for part in parts
                                    if isinstance(part, TextItem) and part.type == TextItemType.DEFAULT)
                    answer = "".join(text).strip()
                    if not answer:
                        raise RuntimeError("Inference returned no assistant text.")
                    print(f"Assistant: {answer}", flush=True)
        model.unload()
        if model.is_loaded:
            raise RuntimeError("Model remained loaded after unload.")
        print("PASS: resolve -> download/cache -> load -> inference -> unload", flush=True)
    finally:
        try:
            if model is not None and model.is_loaded:
                model.unload()
        finally:
            manager.close()


if __name__ == "__main__":
    main()
