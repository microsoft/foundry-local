# Live Audio Transcription — Foundry Local SDK Example (Python)
#
# Tries PyAudio mic capture first; falls back to synthetic PCM if unavailable.
#
# Usage:
#   pip install -r requirements.txt
#   python src/app.py              # Live microphone
#   python src/app.py --synth      # Synthetic 440Hz sine wave

import math
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
    TextItem,
)

use_synth = "--synth" in sys.argv

print("===========================================================")
print("   Foundry Local -- Live Audio Transcription Demo (Python)")
print("===========================================================")
print()

config = Configuration(app_name="foundry_local_samples")
FoundryLocalManager.initialize(config)
manager = FoundryLocalManager.instance

manager.download_and_register_eps()

# English-only:
model_alias = "nemotron-speech-streaming-en-0.6b"
# Multi-lingual (supports 30+ languages including auto-detect):
# model_alias = "nvidia-nemotron-3.5-asr-streaming-multilingual-0.6b"
model = manager.catalog.get_model(model_alias)
if model is None:
    raise RuntimeError(f'Model "{model_alias}" not found in catalog')

model.download(
    lambda progress: print(f"\rDownloading model: {progress:.2f}%", end="", flush=True)
)
print()
print(f"Loading model {model.id}...", end="")
model.load()
print("done.")

session = AudioSession(model)
session.set_options(RequestOptions(additional_options={"language": "en"}))
# Multi-lingual examples:
#   additional_options={"language": "de"}      # German
#   additional_options={"language": "zh-CN"}   # Chinese (Simplified)
#   additional_options={"language": "auto"}    # Auto-detect language
session.set_streaming(True)

# ItemQueue stays caller-owned (transfer_ownership=False) so we can push chunks
# while the streaming request runs. Request is built once and reused.
audio_queue = ItemQueue()
request = Request()
request.add_item(AudioItem.create_format_descriptor("pcm", 16000, 1))
request.add_item(audio_queue, transfer_ownership=False)

result_stream = session.process_streaming_request(request)
print("✓ Session started")

# --- Background thread reads transcription results (mirrors JS readPromise) ---

def read_results():
    for item in result_stream:
        if isinstance(item, TextItem) and item.text:
            print(item.text, end="", flush=True)


read_thread = threading.Thread(target=read_results, daemon=True)
read_thread.start()

# --- Microphone capture (mirrors JS naudiodon2 / C++ PortAudio) ---
# Try PyAudio for mic input; fall back to synthetic PCM on failure.

RATE = 16000
CHANNELS = 1
CHUNK = RATE // 10  # 100ms of audio = 1600 frames

stop_event = threading.Event()
mic_active = False
pa = None
stream = None

if not use_synth:
    try:
        import pyaudio

        pa = pyaudio.PyAudio()
        stream = pa.open(
            format=pyaudio.paInt16,
            channels=CHANNELS,
            rate=RATE,
            input=True,
            frames_per_buffer=CHUNK,
        )
        mic_active = True

        print()
        print("===========================================================")
        print("  LIVE TRANSCRIPTION ACTIVE")
        print("  Speak into your microphone.")
        print("  Press ENTER to stop.")
        print("===========================================================")
        print()

        def capture_mic():
            while not stop_event.is_set():
                try:
                    pcm_data = stream.read(CHUNK, exception_on_overflow=False)
                    if pcm_data:
                        audio_queue.push(BytesItem(pcm_data))
                except Exception as e:
                    print(f"\n[ERROR] Microphone capture failed: {e}")
                    stop_event.set()
                    break

        capture_thread = threading.Thread(target=capture_mic, daemon=True)
        capture_thread.start()

    except Exception as e:
        print(f"Could not initialize microphone: {e}")
        print("Falling back to synthetic audio test...")
        print()
        mic_active = False
        if stream:
            stream.close()
        if pa:
            pa.terminate()
        pa = None
        stream = None

# Fallback: push synthetic PCM (440Hz sine wave) — mirrors JS catch block
if not mic_active:
    print("Pushing synthetic audio (440Hz sine, 2s)...")
    duration = 2
    total_samples = RATE * duration
    pcm_bytes = bytearray(total_samples * 2)
    for i in range(total_samples):
        t = i / RATE
        sample = int(32767 * 0.5 * math.sin(2 * math.pi * 440 * t))
        struct.pack_into("<h", pcm_bytes, i * 2, sample)

    chunk_size = (RATE // 10) * 2  # 100ms
    for offset in range(0, len(pcm_bytes), chunk_size):
        end = min(offset + chunk_size, len(pcm_bytes))
        audio_queue.push(BytesItem(bytes(pcm_bytes[offset:end])))
        time.sleep(0.1)

    print("✓ Synthetic audio pushed")
    time.sleep(3)  # Wait for remaining transcription results


# --- Graceful shutdown — ENTER (rather than Ctrl+C)  ---

def shutdown():
    print("\nStopping...")
    stop_event.set()

    if stream:
        stream.stop_stream()
        stream.close()
    if pa:
        pa.terminate()

    # Signal end-of-input; the streaming session drains remaining items and completes.
    audio_queue.mark_finished()
    read_thread.join(timeout=5)

    # Drop native refs so model.unload() can succeed (session refcount must hit zero).
    global result_stream, request, audio_queue, session
    result_stream = request = audio_queue = session = None

    model.unload()
    print("✓ Done")
    sys.exit(0)


if mic_active:
    # Block until ENTER pressed
    try:
        input()
    except EOFError:
        pass

shutdown()
