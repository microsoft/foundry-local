# <complete_code>
# <imports>
import sys
from pathlib import Path

from foundry_local_sdk import (
    AudioItem,
    AudioSession,
    ChatSession,
    Configuration,
    FoundryLocalManager,
    MessageItem,
    Request,
    RequestOptions,
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
    # </init>

    # <transcription>
    # Load the speech-to-text model
    speech_model = manager.catalog.get_model("whisper-tiny")
    speech_model.download(
        lambda progress: print(f"\rDownloading speech model: {progress:.2f}%", end="", flush=True)
    )
    print()
    speech_model.load()
    print("Speech model loaded.")

    # Default to the shared samples/testdata/meeting-notes.wav.
    default_audio = Path(__file__).resolve().parents[3] / "testdata" / "meeting-notes.wav"
    audio_path = sys.argv[1] if len(sys.argv) > 1 else str(default_audio)

    with AudioSession(speech_model) as audio_session:
        audio_session.set_options(RequestOptions(additional_options={"language": "en"}))
        with Request() as req:
            req.add_item(AudioItem.from_uri(audio_path))
            response = audio_session.process_request(req)
            # Audio transcription responses contain a single TextItem.
            transcript = response.get_item(0).text.strip()

    print(f"\nTranscription:\n{transcript}")

    # Free the speech model before loading the chat model
    speech_model.unload()
    # </transcription>

    # <summarization>
    # Load the chat model for summarization
    chat_model = manager.catalog.get_model("qwen2.5-0.5b")
    chat_model.download(
        lambda progress: print(f"\rDownloading chat model: {progress:.2f}%", end="", flush=True)
    )
    print()
    chat_model.load()
    print("Chat model loaded.")

    with ChatSession(chat_model) as session, Request() as req:
        req.add_item(MessageItem.system(
            "You are a note-taking assistant. Summarize the following transcription "
            "into organized, concise notes with bullet points."
        ))
        req.add_item(MessageItem.user(transcript))

        print("\nSummary:")
        response = session.process_request(req)
        # Chat responses contain a single MessageItem with a single TextItem part.
        print(response.get_item(0).get_simple_text())

    chat_model.unload()
    print("\nDone. Models unloaded.")
    # </summarization>


if __name__ == "__main__":
    main()
# </complete_code>
