from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from foundry_local_sdk import (
    CatalogType,
    ChatSession,
    Configuration,
    FoundryLocalManager,
    IModel,
    MessageItem,
    Request,
    Response,
    TextItem,
)

from launcher import DEFAULT_MODEL_ID, MODEL_ID_PATTERN, get_or_register_model

DEFAULT_PROMPTS = (
    "Write a Python function that chunks a list into groups of three.",
    "Give two reasons to prefer pathlib over string path manipulation.",
    "Correct this expression: `values.sort().reverse()`.",
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run concurrent typed Foundry Local requests."
    )
    parser.add_argument(
        "-m",
        "--model",
        type=Path,
        required=True,
        help="Model directory containing genai_config.json.",
    )
    parser.add_argument(
        "--model-id",
        default=DEFAULT_MODEL_ID,
        help="Local catalog ID in <name>:<version> form.",
    )
    parser.add_argument(
        "--skip-cuda-ep",
        action="store_true",
        help="Do not register CUDAExecutionProvider.",
    )
    args = parser.parse_args()
    args.model = args.model.expanduser().resolve()
    if not args.model.is_dir() or not (args.model / "genai_config.json").is_file():
        parser.error("--model must be a directory containing genai_config.json")
    if not MODEL_ID_PATTERN.fullmatch(args.model_id):
        parser.error("--model-id must use <name>:<positive-integer-version> form")
    return args


def response_text(response: Response) -> str:
    chunks: list[str] = []
    for item in response:
        if isinstance(item, MessageItem):
            chunks.extend(
                part.text for part in item.parts if isinstance(part, TextItem)
            )
        elif isinstance(item, TextItem):
            chunks.append(item.text)
    return "".join(chunks)


def complete(model: IModel, prompt: str) -> str:
    # Sessions retain conversation state, so concurrent tasks must not share one.
    with ChatSession(model) as session:
        with Request().add_item(MessageItem.user(prompt)) as request:
            with session.process_request(request) as response:
                return response_text(response)


def main() -> None:
    args = parse_args()
    FoundryLocalManager.initialize(Configuration(app_name="LocalCodingTypedClient"))
    manager = FoundryLocalManager.instance
    if manager is None:
        raise RuntimeError("Foundry Local manager initialization returned no instance")

    model: IModel | None = None
    registered_here = False
    loaded_here = False
    try:
        if not args.skip_cuda_ep:
            result = manager.download_and_register_eps(["CUDAExecutionProvider"])
            if not result.success:
                raise RuntimeError(
                    f"CUDA execution-provider registration failed: {result.status}"
                )

        model, registered_here = get_or_register_model(
            manager, args.model, args.model_id
        )
        if not model.is_loaded:
            model.load()
            loaded_here = True

        with ThreadPoolExecutor(max_workers=len(DEFAULT_PROMPTS)) as executor:
            futures = [
                executor.submit(complete, model, prompt) for prompt in DEFAULT_PROMPTS
            ]
            for prompt, future in zip(DEFAULT_PROMPTS, futures, strict=True):
                print(f"\nUSER: {prompt}\nASSISTANT: {future.result()}")
    finally:
        try:
            if model is not None and loaded_here:
                model.unload()
        finally:
            try:
                if registered_here:
                    manager.get_catalog(CatalogType.LOCAL).unregister_model(
                        args.model_id
                    )
            finally:
                manager.close()


if __name__ == "__main__":
    main()
