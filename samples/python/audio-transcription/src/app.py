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
    SpeechSegmentItem,
)
# </imports>


# <init>
# Initialize the Foundry Local SDK
config = Configuration(app_name="foundry_local_samples")
FoundryLocalManager.initialize(config)
manager = FoundryLocalManager.instance

# Download and register all execution providers.
current_ep = ""
def _ep_progress(ep_name: str, percent: float):
    global current_ep
    if ep_name != current_ep:
        if current_ep:
            print()
        current_ep = ep_name
    print(f"\r  {ep_name:<30}  {percent:5.1f}%", end="", flush=True)

manager.download_and_register_eps(progress_callback=_ep_progress)
if current_ep:
    print()

# Load the whisper model for speech-to-text
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

# <transcription>
audio_file = sys.argv[1] if len(sys.argv) > 1 else "Recording.mp3"
with AudioSession(model) as session:
    session.set_options(RequestOptions(additional_options={"language": "en"}))
    session.set_streaming(True)
    print(f"Transcribing audio with streaming output: {audio_file}")
    with Request() as req:
        req.add_item(AudioItem.from_uri(audio_file))
        for item in session.process_streaming_request(req):
            if isinstance(item, SpeechSegmentItem):
                print(item.text, end="", flush=True)
    print()
# </transcription>

# Clean up
model.unload()
# </complete_code>
