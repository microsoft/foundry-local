"""
Foundry Local SDK - BYOM local registration E2E Demo

Demonstrates the supported v2 BYOM flow:
  1. Resolve/download a small catalog model.
  2. Clone its model directory under a new location.
  3. Explicitly register that directory in the LOCAL catalog.
  4. Load it, run a typed ChatSession request, unload it.
  5. Unregister the local model and delete the clone.
"""

from __future__ import annotations

import datetime
import errno
import json
import os
import shutil
import sys
from pathlib import Path

from foundry_local_sdk import (
    CatalogType,
    ChatSession,
    Configuration,
    FoundryLocalManager,
    MessageItem,
    ModelInfoBuilder,
    Request,
)

PASS = "\033[92m[PASS]\033[0m"
INFO = "\033[94m[INFO]\033[0m"

SOURCE_ALIAS = "qwen2.5-0.5b"
BYOM_ALIAS = f"fl-local-byom-{datetime.datetime.now().strftime('%Y%m%d-%H%M%S')}"
BYOM_MODEL_ID = f"{BYOM_ALIAS}:1"
BYOM_SUBDIR = "byom_local_registration"
PROMPT = "What is the golden ratio? Answer in one short sentence."


def section(title: str) -> None:
    print(f"\n{'=' * 72}\n  {title}\n{'=' * 72}")


def clone_model_dir(src: Path, dst: Path, new_name: str) -> None:
    dst.mkdir(parents=True, exist_ok=True)
    for entry in src.rglob("*"):
        rel = entry.relative_to(src)
        target = dst / rel
        if entry.is_dir():
            target.mkdir(parents=True, exist_ok=True)
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        try:
            os.link(entry, target)
        except OSError:
            shutil.copy2(entry, target)

    src_info = json.loads((src / "inference_model.json").read_text(encoding="utf-8"))
    (dst / "inference_model.json").unlink()
    src_info["Name"] = new_name
    (dst / "inference_model.json").write_text(json.dumps(src_info), encoding="utf-8")


def extract_text(response) -> str:
    parts: list[str] = []
    for item in response:
        if hasattr(item, "text") and isinstance(getattr(item, "text", None), str):
            parts.append(item.text)
        elif hasattr(item, "parts"):
            for part in item.parts:
                if hasattr(part, "text") and isinstance(getattr(part, "text", None), str):
                    parts.append(part.text)
    return "".join(parts)


def main() -> int:
    print(f"{INFO} BYOM alias for this run: {BYOM_ALIAS}")

    config = Configuration(app_name="foundry_local_byom_local_registration")
    FoundryLocalManager.initialize(config)
    manager = FoundryLocalManager.instance

    section("Step 1: Register execution providers")
    manager.download_and_register_eps()

    section(f"Step 2: Resolve source catalog model {SOURCE_ALIAS!r}")
    source = manager.catalog.get_model(SOURCE_ALIAS)
    if source is None:
        raise RuntimeError(f"Model {SOURCE_ALIAS!r} not found in the catalog")
    cpu_variant = next(
        (variant for variant in source.variants
         if variant.info.runtime and variant.info.runtime.execution_provider == "CPUExecutionProvider"),
        None,
    )
    if cpu_variant is None:
        raise RuntimeError("The BYOM demo requires a CPU execution-provider variant.")
    source.select_variant(cpu_variant)
    print(f"  resolved to variant id: {source.id}")

    if not source.is_cached:
        print(f"  {INFO} source not cached; downloading...")
        source.download(lambda progress: print(f"\r  download: {progress:6.2f}%", end="", flush=True))
        print()

    src_path = Path(source.get_path())
    publisher_dir = src_path.parent.parent
    group_dir = publisher_dir / BYOM_SUBDIR
    clone_dir = group_dir / BYOM_ALIAS

    section("Step 3: Clone source model directory")
    print(f"  cloning -> {clone_dir}")
    clone_model_dir(src_path, clone_dir, BYOM_ALIAS)

    local_catalog = manager.get_catalog(CatalogType.LOCAL)
    registered = None
    try:
        section("Step 4: Register BYOM in local catalog")
        with ModelInfoBuilder() as metadata:
            metadata.set_string_property("task", "chat-completion")
            metadata.set_string_property("display_name", f"{BYOM_ALIAS} local registration demo")
            registered = local_catalog.register_model(str(clone_dir), BYOM_MODEL_ID, metadata)
        print(f"  {PASS} registered {registered.id}")

        section("Step 5: Load and run typed ChatSession")
        registered.load()
        with ChatSession(registered) as session:
            with Request().add_item(MessageItem.user(PROMPT)) as request:
                with session.process_request(request) as response:
                    print(f"  prompt: {PROMPT}")
                    print(f"  assistant: {extract_text(response)}")

        section("Step 6: Unload and unregister")
        registered.unload()
        local_catalog.unregister_model(BYOM_MODEL_ID)
        registered = None
        print(f"  {PASS} unregistered {BYOM_MODEL_ID}")
        return 0
    finally:
        if registered is not None:
            try:
                registered.unload()
            except Exception as exc:
                print(f"Could not unload local model during cleanup: {exc}", file=sys.stderr)
            local_catalog.unregister_model(BYOM_MODEL_ID)
        shutil.rmtree(clone_dir)
        try:
            group_dir.rmdir()
        except OSError as exc:
            if exc.errno != errno.ENOTEMPTY:
                raise


if __name__ == "__main__":
    sys.exit(main())
