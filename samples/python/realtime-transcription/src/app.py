#!/usr/bin/env python3
"""Stream live microphone audio through the native AudioSession API."""

from __future__ import annotations

import argparse
import threading

import sounddevice  # type: ignore[import-not-found]

from foundry_local_sdk import (
    AudioItem,
    AudioSession,
    BytesItem,
    Configuration,
    FoundryLocalManager,
    ItemQueue,
    Request,
    SpeechResultItem,
    SpeechSegmentItem,
    SpeechSegmentKind,
)

APP_NAME = "foundry_local_samples"
MODEL_ALIAS = "nemotron-speech-streaming-en-0.6b"
SAMPLE_RATE = 16_000
CHANNELS = 1
BLOCK_DURATION_MS = 100


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--seconds",
        type=float,
        default=15.0,
        help="number of seconds to capture (default: 15)",
    )
    args = parser.parse_args()
    if args.seconds <= 0:
        parser.error("--seconds must be greater than zero")
    return args


def main() -> None:
    args = parse_args()
    manager = FoundryLocalManager(Configuration(app_name=APP_NAME))
    ep_result = manager.download_and_register_eps()
    if not ep_result.success:
        manager.close()
        raise RuntimeError(
            f"Execution provider registration failed: {ep_result.failed_eps}"
        )

    model = manager.catalog.get_model(MODEL_ALIAS)
    if model is None:
        manager.close()
        raise RuntimeError(f"Model {MODEL_ALIAS!r} was not found in the catalog.")

    try:
        if not model.is_cached:
            model.download(
                progress_callback=lambda percent: print(
                    f"\rDownloading model: {percent:5.1f}%",
                    end="",
                    flush=True,
                )
            )
            print()
        model.load()

        audio = AudioItem.create_format_descriptor(
            "pcm", sample_rate=SAMPLE_RATE, channels=CHANNELS
        )
        audio_queue = ItemQueue()
        capture_finished = threading.Event()
        queue_lock = threading.Lock()
        callback_errors: list[str] = []

        def on_audio(
            input_data: memoryview,
            _frames: int,
            _time_info: object,
            status: object,
        ) -> None:
            if status:
                callback_errors.append(str(status))
            try:
                with queue_lock:
                    if not capture_finished.is_set():
                        audio_queue.push(BytesItem(bytes(input_data)))
            except Exception as error:
                # Exceptions must not escape across PortAudio's callback boundary.
                callback_errors.append(str(error))

        def finish_capture() -> None:
            with queue_lock:
                capture_finished.set()
                audio_queue.mark_finished()

        with Request() as request:
            request.add_item(audio)
            # The request borrows this queue because the microphone callback keeps using it.
            request.add_item(audio_queue, transfer_ownership=False)

            with AudioSession(model) as session:
                session.set_streaming(True)
                timer = threading.Timer(args.seconds, finish_capture)
                timer.start()
                print(
                    f"Listening for {args.seconds:g} seconds. Speak into your microphone..."
                )
                try:
                    with sounddevice.RawInputStream(
                        samplerate=SAMPLE_RATE,
                        channels=CHANNELS,
                        dtype="int16",
                        blocksize=SAMPLE_RATE * BLOCK_DURATION_MS // 1000,
                        callback=on_audio,
                    ):
                        with session.process_streaming_request(request) as stream:
                            for item in stream:
                                if not isinstance(item, SpeechSegmentItem):
                                    continue
                                if item.kind is SpeechSegmentKind.PARTIAL:
                                    print(f"\r{item.text}", end="", flush=True)
                                elif item.kind is SpeechSegmentKind.FINAL:
                                    print(f"\n[final] {item.text}")
                                else:
                                    print(item.text, end="", flush=True)

                            with stream.final_response as response:
                                result = next(
                                    (
                                        item
                                        for item in response
                                        if isinstance(item, SpeechResultItem)
                                    ),
                                    None,
                                )
                                if result is not None:
                                    print(f"\nTranscript: {result.text}")
                finally:
                    timer.cancel()
                    if not capture_finished.is_set():
                        finish_capture()

        if callback_errors:
            print(f"Audio status: {', '.join(callback_errors)}")
    finally:
        if model.is_loaded:
            model.unload()
        manager.close()


if __name__ == "__main__":
    main()
