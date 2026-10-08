# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.

"""Opt-in real-model gate for automatic Qwen XML tool-region guidance."""

import argparse
import ctypes
import json
import os
from pathlib import Path
import sys
import tempfile
import time


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--model", required=True, type=Path, help="Directory containing Qwen genai_config.json")
parser.add_argument("--binding", type=Path, help="Directory containing a built foundry_local_sdk Python package")
args = parser.parse_args()
if sys.platform != "win32":
    parser.error("This CUDA ARM64 smoke test requires Windows")
model_path = args.model.resolve(strict=True)
if not (model_path / "genai_config.json").is_file():
    parser.error(f"Not a GenAI model directory: {model_path}")

cpp_dir = Path(__file__).resolve().parents[2]
native = cpp_dir / "build" / "Windows" / "RelWithDebInfo" / "bin" / "RelWithDebInfo"
os.environ["FOUNDRY_LOCAL_LIB_DIR"] = str(native)
os.environ["FOUNDRY_LOCAL_CUDA_EP_LIBRARY"] = str(native / "onnxruntime_providers_cuda.dll")
os.environ["PATH"] = str(native) + os.pathsep + os.environ["PATH"]
dll_directory = os.add_dll_directory(str(native))
libraries = [ctypes.WinDLL(str(native / name))
             for name in ("onnxruntime.dll", "onnxruntime-genai.dll", "foundry_local.dll")]
if args.binding:
    sys.path.insert(0, str(args.binding.resolve(strict=True)))

from foundry_local_sdk import CatalogType, Configuration, FoundryLocalManager, ModelInfoBuilder
from foundry_local_sdk.items import MessageItem, TextItem, ToolCallItem, ToolResultItem
from foundry_local_sdk.logging_helper import LogLevel
from foundry_local_sdk.request import Request
from foundry_local_sdk.session import ChatSession
from foundry_local_sdk.session_types import RequestOptions, SearchOptions


temporary_logs = tempfile.TemporaryDirectory(prefix="qwen-region-guidance-",
                                             dir=cpp_dir / "build" / "Windows" / "RelWithDebInfo")
log_dir = Path(temporary_logs.name)
admission_marker = "Qwen XML region guidance installed for Engine turn"


def admission_count():
    return sum(log.read_text(encoding="utf-8", errors="replace").count(admission_marker)
               for log in log_dir.glob("foundry_local*.log"))


def require_admission(before, tool):
    deadline = time.monotonic() + 5
    while admission_count() < before + 1 and time.monotonic() < deadline:
        time.sleep(0.05)
    assert admission_count() == before + 1, f"{tool}: delimited guidance was not installed exactly once"


def run_case(model, name, tool, schema, prompt, expected):
    with ChatSession(model) as session:
        session.add_tool_definition(name, f"Call {name} to complete the request.", json.dumps(schema))
        session.set_options(RequestOptions(search=SearchOptions(temperature=0, max_output_tokens=550)))
        before = admission_count()
        with Request().add_item(MessageItem.user(prompt)) as request:
            with session.process_request(request) as response:
                items = list(response)

        require_admission(before, tool)
        calls = [item for item in items if isinstance(item, ToolCallItem)]
        visible = "".join(part.text for item in items if isinstance(item, MessageItem)
                          for part in item.parts if isinstance(part, TextItem))
        assert "<tool_call>" not in visible, (name, visible)
        assert len(calls) == 1 and calls[0].name == name, (name, calls, visible)
        fields = json.loads(calls[0].arguments)
        if callable(expected):
            expected(fields)
        else:
            assert fields == expected, fields
        print(f"PASS {tool}: {fields}", flush=True)

        session.set_options(RequestOptions(search=SearchOptions(temperature=0, max_output_tokens=100)))
        with Request().add_item(ToolResultItem(calls[0].call_id, "Operation completed.")) as followup:
            with session.process_request(followup) as response:
                items = list(response)
        require_admission(before + 1, f"{tool} continuation")
        assert any(isinstance(item, MessageItem) for item in items), (name, items)
        print(f"PASS {tool}: tool-result continuation", flush=True)


def require_edit(fields):
    assert set(fields) == {"path", "code"} and fields["path"] == "src/example.py", fields
    assert "\n" in fields["code"] and "return x * 2" in fields["code"], fields


model_id = "qwen-region-guidance-smoke:1"
manager = FoundryLocalManager(Configuration(
    app_name="QwenRegionGuidanceSmoke", logs_dir=str(log_dir), log_level=LogLevel.DEBUG,
    disable_nonessential_telemetry=True))
try:
    catalog = manager.get_catalog(CatalogType.LOCAL)
    model = catalog.get_model_variant(model_id)
    if model is not None and (
        Path(model.get_path()).resolve() != model_path
        or model.info.get_int_property("supports_tool_calling") != 1
        or model.info.get_int_property("supports_reasoning") != 1
        or model.info.get_string_property("tool_call_start") != "<tool_call>"
        or model.info.get_string_property("tool_call_end") != "</tool_call>"
    ):
        catalog.unregister_model(model_id)
        model = None
    if model is None:
        with ModelInfoBuilder() as info:
            info.set_string_property("task", "chat-completion")
            info.set_string_property("display_name", "Qwen region guidance smoke")
            info.set_int_property("context_length", 262144)
            info.set_int_property("supports_tool_calling", 1)
            info.set_int_property("supports_reasoning", 1)
            info.set_string_property("tool_call_start", "<tool_call>")
            info.set_string_property("tool_call_end", "</tool_call>")
            info.set_string_property("reasoning_start", "<think>")
            info.set_string_property("reasoning_end", "</think>")
            model = catalog.register_model(model_path, model_id, info)

    eps = manager.download_and_register_eps(["CUDAExecutionProvider"])
    if not eps.success or "CUDAExecutionProvider" not in eps.registered_eps:
        raise RuntimeError(f"CUDA EP unavailable: {eps}")
    model.load()
    try:
        run_case(model, "grep", "required paths",
                 {"type": "object", "properties": {
                     "pattern": {"type": "string"}, "paths": {"type": "array", "items": {"type": "string"}}},
                  "required": ["pattern", "paths"], "additionalProperties": False},
                 "Call grep to search for TODO in src. Set paths to an array containing src.",
                 {"paths": ["src"], "pattern": "TODO"})
        run_case(model, "get_status", "parameterless", {"type": "object", "properties": {},
                                                        "additionalProperties": False},
                 "Call get_status now to read the current server status. Then report its result.",
                 {})
        run_case(model, "grep", "optional paths",
                 {"type": "object", "properties": {
                     "pattern": {"type": "string"}, "paths": {"type": "array", "items": {"type": "string"}}},
                  "required": ["pattern"], "additionalProperties": False},
                 "Call grep to find TODO. Set pattern to TODO; omit the optional paths argument.",
                 {"pattern": "TODO"})
        run_case(model, "edit", "multiline code",
                 {"type": "object", "properties": {"path": {"type": "string"}, "code": {"type": "string"}},
                  "required": ["path", "code"], "additionalProperties": False},
                 "Call edit to write a two-line Python function in src/example.py: "
                 "'def double(x):' followed by '    return x * 2'.", require_edit)
    finally:
        model.unload()
finally:
    try:
        manager.close()
    finally:
        temporary_logs.cleanup()
