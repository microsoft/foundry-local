# <complete_code>
# <imports>
import base64
import io
import sys
from PIL import Image

from openai import OpenAI

from foundry_local_sdk import Configuration, FoundryLocalManager
# </imports>
import os

if len(sys.argv) < 2:
    print("Usage: python src/app.py <model_alias_or_id> [image_path]")
    print("         python src/app.py --list-models")
    print("  Example: python src/app.py qwen3.5-0.8b")
    print("  Example: python src/app.py qwen3-vl-2b-instruct-generic-cpu")
    sys.exit(1)

def encode_image(path):
    """Read a local image and return (base64_str, media_type)."""
    media_types = {
        "JPEG": "image/jpeg",
        "PNG": "image/png",
        "GIF": "image/gif",
        "BMP": "image/bmp",
        "WEBP": "image/webp",
    }
    with open(path, "rb") as f:
        image_bytes = f.read()

    try:
        image = Image.open(io.BytesIO(image_bytes))
        image_format = image.format
    except Exception as exc:
        raise ValueError(f"Unable to determine image type for '{path}'.") from exc

    media_type = media_types.get(image_format)
    if media_type is None:
        raise ValueError(
            f"Unsupported image format '{image_format}' for '{path}'. "
            f"Supported formats: {', '.join(sorted(media_types))}."
        )

    return base64.b64encode(image_bytes).decode(), media_type

# <init>
config = Configuration(app_name="foundry_local_samples")
FoundryLocalManager.initialize(config)
manager = FoundryLocalManager.instance

current_ep = ""

def _ep_progress(ep_name: str, percent: float):
    global current_ep
    if ep_name != current_ep:
        if current_ep:
            print()
        current_ep = ep_name
    print(f"\r  {ep_name:<30}  {percent:5.1f}%", end="", flush=True)

print("\nInitializing execution providers:")
manager.download_and_register_eps(progress_callback=_ep_progress)
if current_ep:
    print()
# </init>

list_models = sys.argv[1] in ("--list-models", "-l")

if not list_models:
    model_identifier = sys.argv[1]
    default_image = os.path.join(os.path.dirname(__file__), "test_image.jpg")
    image_path = sys.argv[2] if len(sys.argv) > 2 else default_image
else:
    vision_models = [
        m for m in manager.catalog.list_models()
        if getattr(m, "info", None)
        and m.info.task
        and "vision" in m.info.task.lower()
    ]
    if not vision_models:
        print("\nNo vision models found in catalog.")
        sys.exit(0)

    total_variants = sum(len(m.variants) for m in vision_models)
    print(
        f"\nVision models in catalog ({len(vision_models)} aliases, "
        f"{total_variants} variants):"
    )
    print(
        f"  {'ALIAS':<32}  {'INPUT MODALITIES':<20}  {'OUTPUT MODALITIES':<20}  "
        f"{'TASK':<24}  {'CAPABILITIES'}"
    )
    for m in sorted(vision_models, key=lambda x: x.alias):
        task = (m.info.task or "") if getattr(m, "info", None) else ""
        capabilities = m.capabilities or ""
        print(
            f"  {m.alias:<32}  {(m.input_modalities or ''):<20}  "
            f"{(m.output_modalities or ''):<20}  {task:<24}  {capabilities}"
        )

        variants = list(m.variants)
        if not variants:
            continue

        print(
            f"      {'VARIANT ID':<54}  {'DEVICE':<6}  {'EXECUTION PROVIDER':<32}  "
            f"{'SIZE (MB)':>10}  CACHED"
        )
        for v in sorted(
            variants,
            key=lambda x: (
                (x.info.runtime.device_type if x.info.runtime else ""),
                (x.info.runtime.execution_provider if x.info.runtime else ""),
                x.id,
            ),
        ):
            runtime = v.info.runtime
            device = runtime.device_type if runtime else ""
            ep = runtime.execution_provider if runtime else ""
            size = (
                f"{v.info.file_size_mb:>10}"
                if v.info.file_size_mb is not None else f"{'':>10}"
            )
            cached = "yes" if v.is_cached else "no"
            print(
                f"      {v.id:<54}  {device:<6}  {ep:<32}  {size}  {cached}"
            )
    sys.exit(0)


# <model_setup>
model = manager.catalog.get_model(model_identifier)
if model is None:
    model = manager.catalog.get_model_variant(model_identifier)
if model is None:
    available_aliases = [m.alias for m in manager.catalog.list_models()]
    print(f"\nModel '{model_identifier}' not found in catalog (tried alias and variant id).")
    print(f"Available aliases: {available_aliases}")
    print("Run with --list-models to see variant ids.")
    sys.exit(1)

if not model.is_cached:
    print(f"\nDownloading model {model_identifier}...")
    model.download(
        lambda progress: print(f"\rDownloading model: {progress:.2f}%", end="", flush=True)
    )
    print("\nModel downloaded")

print("\nLoading model...")
model.load()
print("Model loaded")
# </model_setup>

# <server_setup>
print("\nStarting web service...")
manager.start_web_service()
base_url = manager.urls[0].rstrip("/") + "/v1"
print("Web service started")

# <<<<<< OPENAI SDK USAGE >>>>>>
# Use the OpenAI SDK to call the local Foundry web service Responses API
openai = OpenAI(base_url=base_url, api_key="notneeded")
# </server_setup>

# <inference>
print(f"\nPreparing image: {image_path}")
image_b64, media_type = encode_image(image_path)

vision_input = [
    {
        "type": "message",
        "role": "user",
        "content": [
            {"type": "input_text", "text": "Describe this image."},
            {
                "type": "input_image",
                "image_data": image_b64,
                "media_type": media_type,
            },
        ],
    }
]

print("\nStreaming vision response...")
stream = openai.responses.create(
    model=model.id,
    input="placeholder",
    extra_body={"input": vision_input, "max_output_tokens": 8192},
    stream=True,
)

print("[ASSISTANT]: ", end="", flush=True)
for event in stream:
    if getattr(event, "type", None) == "response.output_text.delta":
        print(getattr(event, "delta", ""), end="", flush=True)
print()
# </inference>

openai.close()
manager.stop_web_service()
model.unload()
