# <complete_code>
# <imports>
import struct

from foundry_local_sdk import (
    Configuration,
    EmbeddingsSession,
    FoundryLocalManager,
    Request,
    TensorDataType,
    TensorItem,
    TextItem,
)
# </imports>


def tensor_to_floats(item: TensorItem) -> list[float]:
    if item.data_type != TensorDataType.FLOAT:
        raise TypeError(f"Expected FLOAT tensor output, got {item.data_type}")
    return list(struct.unpack(f"<{len(item.data) // 4}f", item.data))


def main():
    # <init>
    config = Configuration(app_name="foundry_local_samples")
    FoundryLocalManager.initialize(config)
    manager = FoundryLocalManager.instance

    model = manager.catalog.get_model("qwen3-embedding-0.6b")
    model.download(
        lambda progress: print(
            f"\rDownloading model: {progress:.2f}%",
            end="",
            flush=True,
        )
    )
    print()
    model.load()
    print("Model loaded and ready.")
    # </init>

    try:
        with EmbeddingsSession(model) as session:
            # <single_embedding>
            print("\n--- Single Embedding ---")
            with Request().add_item(TextItem("The quick brown fox jumps over the lazy dog")) as request:
                with session.process_request(request) as response:
                    embedding = tensor_to_floats(response.get_item(0))
                    print(f"Dimensions: {len(embedding)}")
                    print(f"First 5 values: {embedding[:5]}")
            # </single_embedding>

            # <batch_embedding>
            print("\n--- Batch Embeddings ---")
            with Request() as request:
                request.add_item(TextItem("Machine learning is a subset of artificial intelligence"))
                request.add_item(TextItem("The capital of France is Paris"))
                request.add_item(TextItem("Rust is a systems programming language"))
                with session.process_request(request) as response:
                    print(f"Number of embeddings: {response.item_count}")
                    for i in range(response.item_count):
                        dims = len(tensor_to_floats(response.get_item(i)))
                        print(f"  [{i}] Dimensions: {dims}")
            # </batch_embedding>
    finally:
        model.unload()
        print("\nModel unloaded.")


if __name__ == "__main__":
    main()
# </complete_code>
