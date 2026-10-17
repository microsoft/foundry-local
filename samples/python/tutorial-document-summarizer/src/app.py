# <complete_code>
# <imports>
import sys
from pathlib import Path

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


def extract_text(response) -> str:
    parts: list[str] = []
    for item in response:
        if isinstance(item, TextItem) and item.type == TextItemType.DEFAULT:
            parts.append(item.text)
        elif hasattr(item, "parts"):
            for part in item.parts:
                if isinstance(part, TextItem) and part.type == TextItemType.DEFAULT:
                    parts.append(part.text)
    return "".join(parts)


def summarize_file(model, file_path, system_prompt):
    content = Path(file_path).read_text(encoding="utf-8")
    with ChatSession(model) as session:
        session.set_options(
            RequestOptions(search=SearchOptions(temperature=0.7, max_output_tokens=512))
        )
        with Request() as request:
            request.add_item(MessageItem.system(system_prompt))
            request.add_item(MessageItem.user(content))
            with session.process_request(request) as response:
                print(extract_text(response))


def summarize_directory(model, directory, system_prompt):
    txt_files = sorted(Path(directory).glob("*.txt"))
    if not txt_files:
        print(f"No .txt files found in {directory}")
        return
    for txt_file in txt_files:
        print(f"--- {txt_file.name} ---")
        summarize_file(model, txt_file, system_prompt)
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
    model.download(lambda p: print(f"\rDownloading model: {p:.2f}%", end="", flush=True))
    print()
    model.load()
    print("Model loaded and ready.\n")
    # </init>

    try:
        # <summarization>
        system_prompt = (
            "Summarize the following document into concise bullet points. "
            "Focus on the key points and main ideas."
        )

        # <file_reading>
        target = sys.argv[1] if len(sys.argv) > 1 else "document.txt"
        target_path = Path(target)
        # </file_reading>

        if target_path.is_dir():
            summarize_directory(model, target_path, system_prompt)
        else:
            print(f"--- {target_path.name} ---")
            summarize_file(model, target_path, system_prompt)
        # </summarization>
    finally:
        model.unload()
        print("\nModel unloaded. Done!")


if __name__ == "__main__":
    main()
# </complete_code>
