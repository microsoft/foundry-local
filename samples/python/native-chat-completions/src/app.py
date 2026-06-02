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

    # <chat_completion>
    with ChatSession(model) as session:
        # Turn 1 — non-streaming. System message steers the assistant.
        with Request() as req1:
            req1.add_item(MessageItem.system("You are a concise science tutor. Answer in 1-2 sentences."))
            req1.add_item(MessageItem.user("Why is the sky blue?"))

            print("Turn 1 (non-streaming):")
            resp1 = session.process_request(req1)
            # Chat responses contain a single MessageItem with a single TextItem part.
            print(resp1.get_item(0).get_simple_text())

        # Turn 2 — streaming.
        session.set_streaming(True)
        with Request() as req2:
            # Session retains history — only the new turn is sent.
            req2.add_item(MessageItem.user("And why are sunsets red?"))

            print("\nTurn 2 (streaming):")
            for item in session.process_streaming_request(req2):
                if isinstance(item, TextItem):
                    print(item.text, end="", flush=True)
            print()

        print(f"\nCompleted turns in session: {session.turn_count}")
    # </chat_completion>

    # Clean up
    model.unload()
    print("Model unloaded.")


if __name__ == "__main__":
    main()
# </complete_code>
