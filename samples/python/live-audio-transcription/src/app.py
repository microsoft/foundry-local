# Live PCM transcription using a format descriptor and a borrowed BytesItem queue.
import math
import signal
import struct
import sys
import threading
import time

from foundry_local_sdk import (
    AudioItem,
    AudioSession,
    BytesItem,
    Configuration,
    FoundryLocalManager,
    ItemQueue,
    Request,
    RequestOptions,
    SpeechResultItem,
    SpeechSegmentItem,
    SpeechSegmentKind,
)

RATE = 16000
CHANNELS = 1
CHUNK = RATE // 10


def main():
    FoundryLocalManager.initialize(Configuration(app_name="foundry_local_samples"))
    manager = FoundryLocalManager.instance
    manager.download_and_register_eps()
    model = manager.catalog.get_model("nemotron-speech-streaming-en-0.6b")
    model.download(lambda progress: print(f"\rDownloading: {progress:.2f}%", end="", flush=True))
    print()
    model.load()

    stop_event = threading.Event()
    previous_handler = signal.signal(signal.SIGINT, lambda *_: stop_event.set())
    pa = None
    microphone = None
    errors = []

    try:
        if "--synth" not in sys.argv:
            try:
                import pyaudio

                pa = pyaudio.PyAudio()
                microphone = pa.open(
                    format=pyaudio.paInt16,
                    channels=CHANNELS,
                    rate=RATE,
                    input=True,
                    frames_per_buffer=CHUNK,
                )
            except (ImportError, OSError) as exc:
                print(f"Microphone unavailable: {exc}\nUsing synthetic audio.")
                if pa is not None:
                    pa.terminate()
                    pa = None

        with AudioSession(model) as session, ItemQueue() as queue, Request() as request:
            session.set_streaming(True)
            request.add_item(AudioItem.create_format_descriptor("pcm", sample_rate=RATE, channels=CHANNELS))
            request.add_item(queue, transfer_ownership=False)
            request.set_options(RequestOptions(additional_options={"language": "en"}))

            def read_results():
                try:
                    with session.process_streaming_request(request) as stream:
                        for item in stream:
                            with item:
                                if not isinstance(item, SpeechSegmentItem) or not item.text:
                                    continue
                                if item.kind == SpeechSegmentKind.PARTIAL:
                                    print(f"\r{item.text}", end="", flush=True)
                                elif item.kind == SpeechSegmentKind.FINAL:
                                    print(f"\n[FINAL] {item.text}")
                                else:
                                    print(item.text, end="", flush=True)

                        with stream.final_response as response:
                            result = next((item for item in response if isinstance(item, SpeechResultItem)), None)
                            if result is None:
                                raise RuntimeError("Expected a SpeechResultItem in the audio response")
                            print(f"\n[aggregate] {result.text}")
                except Exception as exc:
                    errors.append(exc)
                finally:
                    stop_event.set()

            reader = threading.Thread(target=read_results)
            reader.start()
            try:
                if microphone is not None:
                    print("Speak into your microphone. Press Ctrl+C to stop.")
                    while not stop_event.is_set():
                        pcm_data = microphone.read(CHUNK, exception_on_overflow=False)
                        if pcm_data:
                            queue.push(BytesItem(pcm_data))
                else:
                    print("Pushing synthetic audio (440Hz sine, 2s)...")
                    for offset in range(0, RATE * 2, CHUNK):
                        if stop_event.is_set():
                            break
                        pcm = b"".join(
                            struct.pack("<h", int(16383 * math.sin(2 * math.pi * 440 * i / RATE)))
                            for i in range(offset, offset + CHUNK)
                        )
                        queue.push(BytesItem(pcm))
                        time.sleep(0.1)
            finally:
                queue.mark_finished()
                reader.join()

            if errors:
                raise errors[0]
    finally:
        if microphone is not None:
            microphone.stop_stream()
            microphone.close()
        if pa is not None:
            pa.terminate()
        signal.signal(signal.SIGINT, previous_handler)
        model.unload()
    print("Done")


if __name__ == "__main__":
    main()
