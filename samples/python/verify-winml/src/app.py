"""
Foundry Local SDK - WinML 2.0 EP Verification Script

Verifies:
  1. Execution providers are discovered and registered
  2. Accelerated models appear in catalog after EP registration
  3. Streaming chat completions work on an accelerated model
"""

import sys
import time

from foundry_local_sdk import (
    ChatSession,
    Configuration,
    FoundryLocalManager,
    MessageItem,
    Request,
    RequestOptions,
    SearchOptions,
    TextItem,
    TextItemType,
)

PASS = "\033[92m[PASS]\033[0m"
FAIL = "\033[91m[FAIL]\033[0m"
INFO = "\033[94m[INFO]\033[0m"
WARN = "\033[93m[WARN]\033[0m"

results: list[tuple[str, bool]] = []


def log_result(test_name: str, passed: bool, detail: str = ""):
    status = PASS if passed else FAIL
    msg = f"{status} {test_name}"
    if detail:
        msg += f" - {detail}"
    print(msg)
    results.append((test_name, passed))


def print_separator(title: str):
    print(f"\n{'=' * 60}")
    print(f"  {title}")
    print(f"{'=' * 60}\n")


def is_accelerated_variant(variant) -> bool:
    runtime = variant.info.runtime
    return runtime is not None and runtime.device_type in ("GPU", "NPU")


def main():
    print_separator("Initialization")
    config = Configuration(app_name="verify_winml")
    FoundryLocalManager.initialize(config)
    manager = FoundryLocalManager.instance
    print(f"{INFO} FoundryLocalManager initialized.")

    print_separator("Step 1: Discover & Register Execution Providers")
    eps = []
    try:
        eps = manager.discover_eps()
        print(f"{INFO} Discovered {len(eps)} execution providers:")
        for ep in eps:
            print(f"  - {ep.name:40s}  Registered: {ep.is_registered}")
        log_result("EP Discovery", True, f"{len(eps)} EP(s) found")
    except Exception as e:
        log_result("EP Discovery", False, str(e))

    if not eps:
        detail = "No execution providers discovered on this machine"
        log_result("EP Download & Registration", False, detail)
        print(f"\n{FAIL} {detail}.")
        print_summary_and_exit()

    try:
        progress_state = {"ep": None, "percent": -1.0}

        def ep_progress(ep_name: str, percent: float):
            if progress_state["ep"] is not None and (
                progress_state["ep"] != ep_name or percent < progress_state["percent"]
            ):
                print()
            progress_state["ep"] = ep_name
            progress_state["percent"] = percent
            print(f"\r  Downloading {ep_name}: {percent:.1f}%", end="", flush=True)

        result = manager.download_and_register_eps(progress_callback=ep_progress)
        if progress_state["ep"] is not None:
            print()

        print(f"{INFO} EP registration result: success={result.success}, status={result.status}")
        if result.registered_eps:
            print(f"  Registered: {', '.join(result.registered_eps)}")
        if result.failed_eps:
            print(f"  Failed:     {', '.join(result.failed_eps)}")

        download_ok = result.success
        detail = (
            f"{len(result.registered_eps)} EP(s) registered"
            if download_ok and result.registered_eps
            else result.status
        )
        log_result("EP Download & Registration", download_ok, detail)
        if not download_ok:
            print_summary_and_exit()
    except Exception as e:
        print()
        log_result("EP Download & Registration", False, str(e))
        print_summary_and_exit()

    print_separator("Step 2: Model Catalog - Accelerated Models")
    models = manager.catalog.list_models()
    print(f"{INFO} Total models in catalog: {len(models)}")

    accelerated_variants = []
    for model in models:
        for variant in model.variants:
            if is_accelerated_variant(variant):
                accelerated_variants.append(variant)

    print(f"{INFO} Accelerated model variants: {len(accelerated_variants)}")
    for variant in accelerated_variants:
        runtime = variant.info.runtime
        ep = runtime.execution_provider if runtime else "?"
        device = runtime.device_type if runtime else "?"
        print(f"  - {variant.id:50s}  Device: {device:3s}  EP: {ep}")

    log_result(
        "Catalog - Accelerated models found",
        len(accelerated_variants) > 0,
        f"{len(accelerated_variants)} accelerated variant(s)",
    )

    if not accelerated_variants:
        print(f"\n{FAIL} No accelerated model variants are available.")
        print(f"{WARN} Ensure the system has a compatible accelerator and matching model variants installed.")
        print_summary_and_exit()

    print_separator("Step 3: Download & Load Model")
    chosen = None
    downloaded_any = False
    last_load_error = None

    for candidate in accelerated_variants:
        chosen_ep = candidate.info.runtime.execution_provider if candidate.info.runtime else "unknown"
        print(f"\n{INFO} Trying model: {candidate.id} (EP: {chosen_ep})")

        try:
            if not candidate.is_cached:
                candidate.download(
                    progress_callback=lambda percent: print(
                        f"\r  Downloading model: {percent:.1f}%",
                        end="",
                        flush=True,
                    )
                )
                print()
            else:
                print(f"{INFO} Model already cached")
            downloaded_any = True
        except Exception as e:
            print()
            print(f"{WARN} Skipping {candidate.id}: download failed: {e}")
            last_load_error = e
            continue

        try:
            candidate.load()
            chosen = candidate
            break
        except Exception as e:
            print(f"{WARN} Skipping {candidate.id}: load failed: {e}")
            last_load_error = e

    log_result(
        "Model Download",
        downloaded_any,
        "At least one accelerated variant downloaded"
        if downloaded_any
        else str(last_load_error) if last_load_error else "No accelerated variant could be downloaded",
    )

    if chosen is None:
        log_result(
            "Model Load",
            False,
            str(last_load_error) if last_load_error else "No accelerated variant could be loaded on this machine",
        )
        print_summary_and_exit()

    log_result("Model Load", True, f"Loaded {chosen.id}")

    print_separator("Step 4: Streaming Chat Completions (Native)")
    try:
        with ChatSession(chosen) as session:
            session.set_options(
                RequestOptions(search=SearchOptions(temperature=0, max_output_tokens=16))
            )
            session.set_streaming(True)

            response_text = ""
            start = time.time()
            with Request() as request:
                request.add_item(MessageItem.system("You are a helpful assistant."))
                request.add_item(MessageItem.user("What is 2 + 2? Reply with just the number."))
                with session.process_streaming_request(request) as stream:
                    for item in stream:
                        if isinstance(item, TextItem) and item.type == TextItemType.DEFAULT:
                            response_text += item.text
                            print(item.text, end="", flush=True)

            elapsed = time.time() - start
            print()
            log_result(
                "Streaming Chat (Native)",
                len(response_text) > 0,
                f"{len(response_text)} chars in {elapsed:.2f}s",
            )
    except Exception as e:
        log_result("Streaming Chat (Native)", False, str(e))

    try:
        chosen.unload()
        print(f"{INFO} Model unloaded.")
    except Exception as e:
        print(f"{WARN} Failed to unload model: {e}")

    print_summary_and_exit()


def print_summary_and_exit():
    print_separator("Summary")
    passed = sum(1 for _, p in results if p)
    total = len(results)
    for name, passed_result in results:
        print(f"  {'PASS' if passed_result else 'FAIL'} {name}")
    print(f"\n  {passed}/{total} tests passed")
    if passed < total:
        sys.exit(1)


if __name__ == "__main__":
    main()
