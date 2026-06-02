# <complete_code>
# <imports>
import struct

from foundry_local_sdk import (
    Configuration,
    EmbeddingsSession,
    FoundryLocalManager,
    Request,
    TensorItem,
    TextItem,
)
# </imports>


def main():
    # <init>
    # Initialize the Foundry Local SDK
    config = Configuration(app_name="foundry_local_samples")
    FoundryLocalManager.initialize(config)
    manager = FoundryLocalManager.instance

    # Select and load an embedding model from the catalog
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

    with EmbeddingsSession(model) as session:
        # <single_embedding>
        print("\n--- Single Embedding ---")
        with Request().add_item(TextItem("The quick brown fox jumps over the lazy dog")) as req:
            resp = session.process_request(req)
            try:
                tensor = next(it for it in resp if isinstance(it, TensorItem))
                print(f"Shape: {tensor.shape}")
                count = 5
                first_values = struct.unpack(f"{count}f", tensor.data[: count * 4])
                print(f"First {count} values: {list(first_values)}")
            finally:
                resp._close()
        # </single_embedding>

        # <batch_embedding>
        print("\n--- Batch Embeddings ---")
        with Request() as req:
            req.add_item(TextItem("Machine learning is a subset of artificial intelligence"))
            req.add_item(TextItem("The capital of France is Paris"))
            req.add_item(TextItem("Rust is a systems programming language"))

            resp = session.process_request(req)
            try:
                tensors = [it for it in resp if isinstance(it, TensorItem)]
                print(f"Number of embeddings: {len(tensors)}")
                for i, tensor in enumerate(tensors):
                    print(f"  [{i}] Shape: {tensor.shape}")
            finally:
                resp._close()
        # </batch_embedding>

    # Clean up
    model.unload()
    print("\nModel unloaded.")


if __name__ == "__main__":
    main()
# </complete_code>
