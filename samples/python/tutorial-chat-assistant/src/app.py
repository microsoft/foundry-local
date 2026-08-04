# <complete_code>
# <imports>
from foundry_local_sdk import (
    ChatSession,
    Configuration,
    FoundryLocalManager,
    MessageItem,
    Request,
    TextItem,
)
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
    model.download(lambda progress: print(f"\rDownloading model: {progress:.2f}%", end="", flush=True))
    print()
    model.load()
    print("Model loaded and ready.")
    # </init>

    # <conversation_loop>
    # ChatSession keeps conversation history across turns; we send only new items each turn.
    with ChatSession(model) as session:
        session.set_streaming(True)

        system_message = MessageItem.system(
            "You are a helpful, friendly assistant. Keep your responses "
            "concise and conversational. If you don't know something, say so."
        )

        print("\nChat assistant ready! Type 'quit' to exit.\n")

        first_turn = True
        while True:
            # Print prompt explicitly: input()'s prompt goes through readline to stderr
            # when stdout isn't a TTY, which breaks captured-output scenarios.
            print("You: ", end="", flush=True)
            user_input = input()
            if user_input.strip().lower() in ("quit", "exit"):
                break

            with Request() as req:
                if first_turn:
                    req.add_item(system_message)
                    first_turn = False
                req.add_item(MessageItem.user(user_input))

                # <streaming>
                print("Assistant: ", end="", flush=True)
                for item in session.process_streaming_request(req):
                    if isinstance(item, TextItem):
                        print(item.text, end="", flush=True)
                print("\n")
                # </streaming>
    # </conversation_loop>

    # Clean up - unload the model
    model.unload()
    print("Model unloaded. Goodbye!")


if __name__ == "__main__":
    main()
# </complete_code>
