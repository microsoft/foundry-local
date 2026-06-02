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
)
# </imports>


SYSTEM_PROMPT = (
    "Summarize the following document into concise bullet points. "
    "Focus on the key points and main ideas."
)


def summarize_file(model, file_path: Path) -> None:
    """Summarize a single file in its own ChatSession."""
    content = file_path.read_text(encoding="utf-8")
    # Fresh session per file — no cross-document conversation history.
    with ChatSession(model) as session, Request() as req:
        req.add_item(MessageItem.system(SYSTEM_PROMPT))
        req.add_item(MessageItem.user(content))

        response = session.process_request(req)
        # Chat responses contain a single MessageItem with a single TextItem part.
        print(response.get_item(0).get_simple_text())


def summarize_directory(model, directory: Path) -> None:
    """Summarize all .txt files in a directory."""
    txt_files = sorted(directory.glob("*.txt"))

    if not txt_files:
        print(f"No .txt files found in {directory}")
        return

    for txt_file in txt_files:
        print(f"--- {txt_file.name} ---")
        summarize_file(model, txt_file)
        print()


def main():
    # <init>
    # Initialize the Foundry Local SDK
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

    # <summarization>
    # Default to the shared samples/testdata/document.txt.
    default_document = Path(__file__).resolve().parents[3] / "testdata" / "document.txt"
    target = sys.argv[1] if len(sys.argv) > 1 else default_document
    target_path = Path(target)

    if target_path.is_dir():
        summarize_directory(model, target_path)
    else:
        print(f"--- {target_path.name} ---")
        summarize_file(model, target_path)
    # </summarization>

    # Clean up
    model.unload()
    print("\nModel unloaded. Done!")


if __name__ == "__main__":
    main()
# </complete_code>
