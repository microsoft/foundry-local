# <complete_code>
# <imports>
from foundry_local_sdk import Configuration, FoundryLocalManager
# </imports>


def main():
    # <init>
    # Initialize the Foundry Local SDK
    config = Configuration(app_name="foundry_local_samples")
    FoundryLocalManager.initialize(config)
    manager = FoundryLocalManager.instance

    # Download and register all execution providers.
    current_ep = ""
    def ep_progress(ep_name: str, percent: float):
        nonlocal current_ep
        if ep_name != current_ep:
            if current_ep:
                print()
            current_ep = ep_name
        print(f"\r  {ep_name:<30}  {percent:5.1f}%", end="", flush=True)

    manager.download_and_register_eps(progress_callback=ep_progress)
    if current_ep:
        print()

    # Select and load a model from the catalog
    model = manager.catalog.get_model("qwen2.5-0.5b")
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

    # Get a chat client
    client = model.get_chat_client()
    # </init>

    # <streaming>
    # Create the conversation messages
    messages = [
        {"role": "user", "content": "What is the golden ratio?"}
    ]

    # Stream the response token by token
    print("Assistant: ", end="", flush=True)
    for chunk in client.complete_streaming_chat(messages):
        if not chunk.choices:  # the final chunk can carry no choices
            continue
        content = chunk.choices[0].delta.content
        if content:
            print(content, end="", flush=True)
    print()
    # </streaming>

    # Clean up
    model.unload()
    print("Model unloaded.")


if __name__ == "__main__":
    main()
# </complete_code>
