# <complete_code>
# <imports>
from foundry_local_sdk import (
    ChatSession,
    Configuration,
    FoundryLocalManager,
    MessageItem,
    Request,
    RequestOptions,
    SearchOptions,
    TextItem,
    TextItemType,
)
# </imports>


def main():
    # <init>
    config = Configuration(app_name="foundry_local_samples")
    FoundryLocalManager.initialize(config)
    manager = FoundryLocalManager.instance

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

    model = manager.catalog.get_model("qwen2.5-0.5b")
    model.download(lambda progress: print(f"\rDownloading model: {progress:.2f}%", end="", flush=True))
    print()
    model.load()
    print("Model loaded and ready.")
    # </init>

    print("\nChat assistant ready! Type 'quit' to exit.\n")

    try:
        with ChatSession(model) as session:
            session.set_options(
                RequestOptions(search=SearchOptions(temperature=0.7, max_output_tokens=512))
            )

            # <system_prompt>
            system_prompt = (
                "You are a helpful, friendly assistant. Keep your responses "
                "concise and conversational. If you don't know something, say so."
            )
            # </system_prompt>

            first_turn = True

            # <conversation_loop>
            while True:
                user_input = input("You: ")
                if user_input.strip().lower() in ("quit", "exit"):
                    break

                with Request() as request:
                    if first_turn:
                        request.add_item(MessageItem.system(system_prompt))
                        first_turn = False
                    request.add_item(MessageItem.user(user_input))

                    # <streaming>
                    print("Assistant: ", end="", flush=True)
                    session.set_streaming(True)
                    with session.process_streaming_request(request) as stream:
                        for item in stream:
                            if isinstance(item, TextItem) and item.type == TextItemType.DEFAULT:
                                print(item.text, end="", flush=True)
                    print("\n")
                    # </streaming>
            # </conversation_loop>
    finally:
        model.unload()
        print("Model unloaded. Goodbye!")


if __name__ == "__main__":
    main()
# </complete_code>
