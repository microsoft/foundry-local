# <complete_code>
# <imports>
import sys

from foundry_local_sdk import (
    AudioItem,
    AudioSession,
    Configuration,
    FoundryLocalManager,
    Request,
    RequestOptions,
    SpeechResultItem,
)
# </imports>


def main():
    # <init>
    config = Configuration(app_name="foundry_local_samples")
    FoundryLocalManager.initialize(config)
    manager = FoundryLocalManager.instance

    current_ep = ""

    def _ep_progress(ep_name: str, percent: float):
        nonlocal current_ep
        if ep_name != current_ep:
            if current_ep:
                print()
            current_ep = ep_name
        print(f"\r  {ep_name:<30}  {percent:5.1f}%", end="", flush=True)

    manager.download_and_register_eps(progress_callback=_ep_progress)
    if current_ep:
        print()

    model = manager.catalog.get_model("whisper-tiny")
    model.download(
        lambda progress: print(
            f"\rDownloading model: {progress:.2f}%",
            end="",
            flush=True,
        )
    )
    print()
    model.load()
    print("Model loaded.")
    # </init>

    audio_file = sys.argv[1] if len(sys.argv) > 1 else "Recording.mp3"

    try:
        # <transcription>
        with AudioSession(model) as session:
            with Request().add_item(AudioItem.from_uri(audio_file)) as request:
                request.set_options(RequestOptions(additional_options={"language": "en"}))
                with session.process_request(request) as response:
                    result = next((item for item in response if isinstance(item, SpeechResultItem)), None)
                    if result is None:
                        raise RuntimeError("Expected a SpeechResultItem in the audio response")
                    print("Transcription:")
                    print(result.text)
        # </transcription>
    finally:
        model.unload()

if __name__ == "__main__":
    main()
# </complete_code>
