#!/usr/bin/env python3
# -------------------------------------------------------------------------
# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.
# --------------------------------------------------------------------------

"""Exercise concurrent multi-turn ChatSessions with coding-riddle variations.

Each session receives a different coding riddle in the first parallel round.
The second parallel round sends only a variation that depends on the first
turn's code and answer; prior messages are retained by ChatSession and are not
resent by this script.
"""

from __future__ import annotations

import argparse
import json
import os
import tempfile
import threading
from concurrent.futures import ThreadPoolExecutor
from contextlib import ExitStack
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from foundry_local_sdk import (
    Catalog,
    ChatSession,
    Configuration,
    FoundryLocalManager,
    IModel,
    MessageItem,
    Request,
    RequestOptions,
    SearchOptions,
    TextItem,
)


DEFAULT_MODEL_ID = "parallel-chat-continuation-generic-gpu:1"
LOCAL_CATALOG_TYPE = 1


@dataclass(frozen=True)
class Scenario:
    name: str
    initial_prompt: str
    initial_expected_suffix: str
    follow_up_prompt: str
    follow_up_expected_suffix: str


BUILT_IN_SCENARIOS = (
    Scenario(
        name="python-aliased-rows",
        initial_prompt="""Analyze this Python code without running it:

grid = [[0] * 3] * 3
grid[1][2] = 7
print(grid)

What is printed, and what language behavior causes it?
Use ALIASED_ROWS or INDEPENDENT_ROWS for the cause label.
End with exactly: OUTPUT=<compact-list> CAUSE=<UPPER_SNAKE_CASE>""",
        initial_expected_suffix="OUTPUT=[[0,0,7],[0,0,7],[0,0,7]] CAUSE=ALIASED_ROWS",
        follow_up_prompt=(
            "Using the code and diagnosis from the previous turn, replace only the grid initialization with "
            "[[0] * 3 for _ in range(3)]. Keep the dimensions and mutation unchanged. What is printed now, and "
            "does the original cause remain? Use ALIASED_ROWS or INDEPENDENT_ROWS for the cause label. "
            "End with exactly: OUTPUT=<compact-list> CAUSE=<UPPER_SNAKE_CASE>"
        ),
        follow_up_expected_suffix="OUTPUT=[[0,0,0],[0,0,7],[0,0,0]] CAUSE=INDEPENDENT_ROWS",
    ),
    Scenario(
        name="javascript-loop-closures",
        initial_prompt="""Analyze this JavaScript without running it:

const callbacks = [];
for (var i = 0; i < 3; i++) {
  callbacks.push(() => i);
}
console.log(callbacks.map(fn => fn()).join(","));

What is printed, and why?
Use FUNCTION_SCOPED_VAR or BLOCK_SCOPED_BINDINGS for the cause label.
End with exactly: OUTPUT=<comma-separated-values> CAUSE=<UPPER_SNAKE_CASE>""",
        initial_expected_suffix="OUTPUT=3,3,3 CAUSE=FUNCTION_SCOPED_VAR",
        follow_up_prompt=(
            "Using the same program from the previous turn, replace only var with let and leave everything else "
            "unchanged. What is printed now, and what changed semantically? "
            "Use FUNCTION_SCOPED_VAR or BLOCK_SCOPED_BINDINGS for the cause label. "
            "End with exactly: OUTPUT=<comma-separated-values> CAUSE=<UPPER_SNAKE_CASE>"
        ),
        follow_up_expected_suffix="OUTPUT=0,1,2 CAUSE=BLOCK_SCOPED_BINDINGS",
    ),
    Scenario(
        name="sql-not-in-null",
        initial_prompt="""Analyze this SQL query:

items(id) contains: 1, 2, 3
excluded(id) contains: 2, NULL

SELECT id FROM items
WHERE id NOT IN (SELECT id FROM excluded)
ORDER BY id;

Which rows are returned, and what SQL rule determines the result?
Use NOT_IN_NULL or NULL_SAFE_ANTIJOIN for the cause label.
End with exactly: ROWS=<comma-separated-values-or-NONE> CAUSE=<UPPER_SNAKE_CASE>""",
        initial_expected_suffix="ROWS=NONE CAUSE=NOT_IN_NULL",
        follow_up_prompt=(
            "Using the same tables and intent from the previous turn, replace the predicate with a correlated "
            "NOT EXISTS equality check. Which ordered rows are returned, and why is the NULL no longer fatal? "
            "Use NOT_IN_NULL or NULL_SAFE_ANTIJOIN for the cause label. "
            "End with exactly: ROWS=<comma-separated-values-or-NONE> CAUSE=<UPPER_SNAKE_CASE>"
        ),
        follow_up_expected_suffix="ROWS=1,3 CAUSE=NULL_SAFE_ANTIJOIN",
    ),
    Scenario(
        name="cpp-reverse-loop",
        initial_prompt="""Analyze this C++ code for an empty vector:

std::vector<int> values;
for (std::size_t i = values.size() - 1; i >= 0; --i) {
  consume(values[i]);
}

Identify the first correctness bug before consume can safely execute.
Use UNSIGNED_UNDERFLOW for the bug label.
End with exactly: BUG=<UPPER_SNAKE_CASE>""",
        initial_expected_suffix="BUG=UNSIGNED_UNDERFLOW",
        follow_up_prompt=(
            "Using the loop and diagnosis from the previous turn, change only the counter to "
            "auto i = std::ssize(values) - 1 while keeping i >= 0 and --i. For the same empty vector, how many "
            "iterations execute and is the original bug fixed? Use FIXED for the bug label if the underflow is "
            "eliminated. End with exactly: ITERATIONS=<number> BUG=<word>"
        ),
        follow_up_expected_suffix="ITERATIONS=0 BUG=FIXED",
    ),
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run parallel coding-riddle turns and history-dependent variations through Foundry Local.",
    )
    parser.add_argument(
        "--model-path",
        type=Path,
        required=True,
        help="Local model directory containing genai_config.json.",
    )
    parser.add_argument("--model-id", default=DEFAULT_MODEL_ID, help="Temporary local-catalog model ID.")
    parser.add_argument(
        "--scenario-file",
        type=Path,
        help="Optional JSON array of scenarios; replaces built-in scenarios.",
    )
    parser.add_argument(
        "--app-data-dir",
        type=Path,
        help="Persistent SDK app-data directory. Defaults to a temporary directory.",
    )
    parser.add_argument(
        "--model-cache-dir",
        type=Path,
        help="Persistent model-cache directory. Defaults to a temporary directory.",
    )
    parser.add_argument("--max-output-tokens", type=int, default=1536)
    parser.add_argument("--workers", type=int, default=4, help="Maximum parallel ChatSession calls.")
    parser.add_argument("--skip-ep-registration", action="store_true", help="Skip CUDA EP registration.")
    parser.add_argument("--show-responses", action=argparse.BooleanOptionalAction, default=True)
    args = parser.parse_args()

    if args.max_output_tokens <= 0:
        parser.error("--max-output-tokens must be positive")
    if args.workers <= 0:
        parser.error("--workers must be positive")
    if ":" not in args.model_id:
        parser.error("--model-id must use <name>:<version>")
    return args


def load_scenarios(path: Path | None) -> tuple[Scenario, ...]:
    if path is None:
        return BUILT_IN_SCENARIOS

    try:
        values: Any = json.loads(path.read_text(encoding="utf-8"))
        scenarios = tuple(Scenario(**value) for value in values)
    except (OSError, json.JSONDecodeError, TypeError) as error:
        raise ValueError(f"Could not load scenarios from {path}: {error}") from error
    if not scenarios:
        raise ValueError("Scenario file must contain at least one scenario.")
    return scenarios


def register_local_gpu_model(manager: FoundryLocalManager, model_path: Path, model_id: str) -> tuple[Catalog, IModel]:
    # The native local-catalog surface is newer than the public Python Catalog wrapper, so this validation example
    # uses the already-declared CFFI vtable until Catalog exposes equivalent methods.
    from foundry_local_sdk._native.api import api, ffi
    from foundry_local_sdk.imodel import _ModelImpl

    catalog_out = ffi.new("flCatalog**")
    api.check_status(
        api.root.Manager_GetCatalogByType(
            manager._native_manager,
            LOCAL_CATALOG_TYPE,
            catalog_out,
        )
    )
    catalog = Catalog(catalog_out[0], parent=manager)

    metadata_out = ffi.new("flModelInfo**")
    api.check_status(api.model.CreateModelInfo(metadata_out))
    metadata = metadata_out[0]
    try:
        for key, value in (
            ("task", "chat-completion"),
            ("device_type", "GPU"),
            ("execution_provider", "CUDAExecutionProvider"),
        ):
            api.check_status(api.model.Info_SetStringProperty(metadata, key.encode(), value.encode()))

        model_out = ffi.new("flModel**")
        api.check_status(
            api.catalog.RegisterModel(
                catalog._ptr,
                os.fsencode(model_path),
                model_id.encode(),
                metadata,
                model_out,
            )
        )
    finally:
        api.model.ReleaseModelInfo(metadata)

    return catalog, _ModelImpl(model_out[0], parent=catalog)


def unregister_local_model(catalog: Catalog, model_id: str) -> None:
    from foundry_local_sdk._native.api import api

    api.check_status(api.catalog.UnregisterModel(catalog._ptr, model_id.encode()))


def collect_response_text(response: Any) -> str:
    text = ""
    for item in response:
        if isinstance(item, TextItem):
            text += item.text
        elif isinstance(item, MessageItem):
            text += "".join(part.text for part in item.parts if isinstance(part, TextItem))
    return text


def run_turn(session: ChatSession, prompt: str, start: threading.Event) -> str:
    start.wait()
    with Request().add_item(MessageItem.user(prompt)) as request:
        with session.process_request(request) as response:
            return collect_response_text(response)


def run_parallel_turn(
    sessions: list[ChatSession],
    prompts: list[str],
    workers: int,
) -> list[str]:
    start = threading.Event()
    with ThreadPoolExecutor(max_workers=min(workers, len(sessions))) as executor:
        futures = [
            executor.submit(run_turn, session, prompt, start)
            for session, prompt in zip(sessions, prompts, strict=True)
        ]
        start.set()
        return [future.result() for future in futures]


def require_suffix(scenario: Scenario, turn: int, output: str, expected: str) -> None:
    if not output.rstrip().endswith(expected):
        raise RuntimeError(
            f"{scenario.name} turn {turn} did not end with {expected!r}.\n"
            f"Full response:\n{output}"
        )


def main() -> None:
    args = parse_args()
    scenarios = load_scenarios(args.scenario_file)
    model_path = args.model_path.expanduser().resolve(strict=True)
    if not (model_path / "genai_config.json").is_file():
        raise FileNotFoundError(f"genai_config.json was not found under {model_path}")

    with ExitStack() as stack:
        temporary_root = Path(stack.enter_context(tempfile.TemporaryDirectory(prefix="foundry-parallel-qa-")))
        app_data_dir = (args.app_data_dir or temporary_root / "appdata").expanduser().resolve()
        model_cache_dir = (args.model_cache_dir or temporary_root / "cache" / "models").expanduser().resolve()
        app_data_dir.mkdir(parents=True, exist_ok=True)
        model_cache_dir.mkdir(parents=True, exist_ok=True)

        FoundryLocalManager.initialize(
            Configuration(
                app_name="ParallelChatContinuation",
                app_data_dir=str(app_data_dir),
                model_cache_dir=str(model_cache_dir),
            )
        )
        manager = FoundryLocalManager.instance
        if manager is None:
            raise RuntimeError("Foundry Local manager initialization failed.")
        stack.callback(manager.close)

        if not args.skip_ep_registration:
            result = manager.download_and_register_eps(["CUDAExecutionProvider"])
            if not result.success:
                raise RuntimeError(f"CUDA EP registration failed: {result.status}")

        catalog, model = register_local_gpu_model(manager, model_path, args.model_id)
        stack.callback(unregister_local_model, catalog, args.model_id)
        model.load()
        stack.callback(model.unload)

        sessions = [stack.enter_context(ChatSession(model)) for _ in scenarios]
        options = RequestOptions(
            search=SearchOptions(
                temperature=0,
                max_output_tokens=args.max_output_tokens,
            )
        )
        for session in sessions:
            session.set_options(options)

        first_outputs = run_parallel_turn(
            sessions,
            [scenario.initial_prompt for scenario in scenarios],
            args.workers,
        )
        for scenario, output in zip(scenarios, first_outputs, strict=True):
            require_suffix(scenario, 1, output, scenario.initial_expected_suffix)
            if args.show_responses:
                print(f"\n[{scenario.name}] turn 1\n{output}")

        second_outputs = run_parallel_turn(
            sessions,
            [scenario.follow_up_prompt for scenario in scenarios],
            args.workers,
        )
        for scenario, output in zip(scenarios, second_outputs, strict=True):
            require_suffix(scenario, 2, output, scenario.follow_up_expected_suffix)
            if args.show_responses:
                print(f"\n[{scenario.name}] turn 2\n{output}")

        print(f"\nPASS: {len(scenarios)} parallel sessions retained isolated history across both coding-riddle turns.")


if __name__ == "__main__":
    main()
