# <complete_code>
# <imports>
from foundry_local_sdk import (
    AudioItem,
    AudioSession,
    ChatSession,
    Configuration,
    FoundryLocalManager,
    MessageItem,
    Request,
    RequestOptions,
    SearchOptions,
    SpeechResultItem,
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


def main():
    # <init>
    config = Configuration(app_name="foundry_local_samples")
    FoundryLocalManager.initialize(config)
    manager = FoundryLocalManager.instance
    # </init>

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

    # <transcription>
    speech_model = manager.catalog.get_model("whisper-tiny")
    speech_model.download(
        lambda progress: print(
            f"\rDownloading speech model: {progress:.2f}%",
            end="",
            flush=True,
        )
    )
    print()
    speech_model.load()
    print("Speech model loaded.")

    with AudioSession(speech_model) as audio_session:
        with Request().add_item(AudioItem.from_uri("meeting-notes.wav")) as request:
            request.set_options(RequestOptions(additional_options={"language": "en"}))
            with audio_session.process_request(request) as response:
                result = next((item for item in response if isinstance(item, SpeechResultItem)), None)
                if result is None:
                    raise RuntimeError("Expected a SpeechResultItem in the audio response")
                transcription = result.text
    print(f"\nTranscription:\n{transcription}")

    speech_model.unload()
    # </transcription>

    # <summarization>
    chat_model = manager.catalog.get_model("qwen2.5-0.5b")
    chat_model.download(
        lambda progress: print(
            f"\rDownloading chat model: {progress:.2f}%",
            end="",
            flush=True,
        )
    )
    print()
    chat_model.load()
    print("Chat model loaded.")

    with ChatSession(chat_model) as session:
        session.set_options(
            RequestOptions(search=SearchOptions(temperature=0.7, max_output_tokens=512))
        )
        with Request() as request:
            request.add_item(
                MessageItem.system(
                    "You are a note-taking assistant. Summarize the following "
                    "transcription into organized, concise notes with bullet points."
                )
            )
            request.add_item(MessageItem.user(transcription))
            with session.process_request(request) as response:
                summary = extract_text(response)

    print(f"\nSummary:\n{summary}")
    chat_model.unload()
    print("\nDone. Models unloaded.")
    # </summarization>


if __name__ == "__main__":
    main()
# </complete_code>
