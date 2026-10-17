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


def print_response_text(response) -> None:
    for item in response:
        if isinstance(item, TextItem) and item.type == TextItemType.DEFAULT:
            print(item.text, end="", flush=True)
        elif hasattr(item, "parts"):
            for part in item.parts:
                if isinstance(part, TextItem) and part.type == TextItemType.DEFAULT:
                    print(part.text, end="", flush=True)
    print()


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
        with ChatSession(model) as session:
            session.set_options(
                RequestOptions(search=SearchOptions(temperature=0.0, max_output_tokens=256))
            )

            # <streaming>
            with Request().add_item(MessageItem.user("What is the golden ratio?")) as request:
                print("Assistant: ", end="", flush=True)
                session.set_streaming(True)
                with session.process_streaming_request(request) as stream:
                    for item in stream:
                        if isinstance(item, TextItem) and item.type == TextItemType.DEFAULT:
                            print(item.text, end="", flush=True)
                print()
            # </streaming>
    finally:
        model.unload()
        print("Model unloaded.")


if __name__ == "__main__":
    main()
# </complete_code>
