#!/usr/bin/env python3
"""Generate embeddings through the native in-process API."""

from __future__ import annotations

from array import array
from collections.abc import Sequence

from foundry_local_sdk import (
    Configuration,
    EmbeddingsSession,
    FoundryLocalManager,
    Request,
    TensorDataType,
    TensorItem,
    TextItem,
)

APP_NAME = "foundry_local_samples"
MODEL_ALIAS = "qwen3-embedding-0.6b"
INPUTS = (
    "A kitten is sleeping on a rug.",
    "A cat rests on a mat.",
    "Saturn has a prominent ring system.",
)


def cosine_similarity(left: Sequence[float], right: Sequence[float]) -> float:
    # EmbeddingsSession returns L2-normalized vectors, so their dot product is cosine similarity.
    return sum(a * b for a, b in zip(left, right, strict=True))


def main() -> None:
    manager = FoundryLocalManager(Configuration(app_name=APP_NAME))
    ep_result = manager.download_and_register_eps()
    if not ep_result.success:
        manager.close()
        raise RuntimeError(
            f"Execution provider registration failed: {ep_result.failed_eps}"
        )

    model = manager.catalog.get_model(MODEL_ALIAS)
    if model is None:
        manager.close()
        raise RuntimeError(f"Model {MODEL_ALIAS!r} was not found in the catalog.")

    try:
        if not model.is_cached:
            model.download(
                progress_callback=lambda percent: print(
                    f"\rDownloading model: {percent:5.1f}%",
                    end="",
                    flush=True,
                )
            )
            print()

        model.load()
        with EmbeddingsSession(model) as session, Request() as request:
            for text in INPUTS:
                request.add_item(TextItem(text))

            with session.process_request(request) as response:
                tensors = [item for item in response if isinstance(item, TensorItem)]
                if len(tensors) != len(INPUTS):
                    raise RuntimeError(
                        f"Expected {len(INPUTS)} tensors, received {len(tensors)}."
                    )

                embeddings = []
                for tensor in tensors:
                    if tensor.data_type is not TensorDataType.FLOAT:
                        raise RuntimeError(
                            f"Expected FLOAT tensor, received {tensor.data_type.name}."
                        )
                    values = array("f")
                    values.frombytes(tensor.data)
                    embeddings.append(values)

                print(f"Model: {model.id}")
                print(f"Dimensions: {len(embeddings[0])}")
                print(
                    f"Similarity (cat, cat): {cosine_similarity(embeddings[0], embeddings[1])}"
                )
                print(
                    f"Similarity (cat, Saturn): {cosine_similarity(embeddings[0], embeddings[2])}"
                )
    finally:
        if model.is_loaded:
            model.unload()
        manager.close()


if __name__ == "__main__":
    main()
