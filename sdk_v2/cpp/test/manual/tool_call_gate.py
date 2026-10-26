#!/usr/bin/env python3
# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.
"""Real-model protocol checks. This does not replace the actual Copilot coding-agent qualification."""

import argparse
import copy
import io
import json
from pathlib import Path
import time
import urllib.error
import urllib.request


def cases():
    parameters = {
        "type": "object",
        "properties": {"city": {"type": "string", "minLength": 1}},
        "required": ["city"],
        "additionalProperties": False,
    }
    base = {
        "temperature": 0,
        "max_tokens": 2048,
        "tools": [{
            "type": "function",
            "function": {"name": "lookup", "description": "Return the supplied city.", "parameters": parameters},
        }],
        "messages": [{"role": "user", "content": "Call lookup with city Paris."}],
    }
    for choice in ("auto", "required"):
        request = copy.deepcopy(base)
        request["tool_choice"] = choice
        yield "minlength_" + choice, request, [{"city": "Paris"}], None

    request = copy.deepcopy(base)
    request["tool_choice"] = "auto"
    request["tools"][0]["function"]["parameters"]["properties"]["city"].update({
        "format": "email", "x-ui-widget": "textarea",
    })
    yield "annotation_metadata", request, [{"city": "Paris"}], None

    request = copy.deepcopy(base)
    request["tool_choice"] = "auto"
    request["tools"].append({
        "type": "custom",
        "custom": {"name": "apply_patch", "description": "Apply a patch.", "format": {"type": "text"}},
    })
    request["metadata"] = {"tool_output_encoding": json.dumps({
        "type": "raw_envelope", "tool_name": "apply_patch",
        "start_marker": "*** Begin Patch", "end_marker": "*** End Patch",
    })}
    yield "function_with_raw_patch_descriptor", request, [{"city": "Paris"}], None

    request = copy.deepcopy(request)
    request["messages"] += [
        {"role": "assistant", "tool_calls": [{
            "id": "call_previous", "type": "function",
            "function": {"name": "lookup", "arguments": '{"city":"Paris"}'},
        }]},
        {"role": "tool", "tool_call_id": "call_previous", "content": "Lookup failed."},
        {"role": "user", "content": "Call lookup again, this time with city Rome."},
    ]
    yield "function_with_raw_patch_continuation", request, [{"city": "Rome"}], None

    request = copy.deepcopy(base)
    request["tools"][0]["function"]["parameters"]["oneOf"] = [
        {"properties": {"city": {"const": "Paris"}}},
        {"properties": {"city": {"const": "Rome"}}},
    ]
    yield "root_oneof", request, [{"city": "Paris"}], None

    request = copy.deepcopy(base)
    schema = request["tools"][0]["function"]["parameters"]
    schema["$defs"] = {"city": {"type": "string", "minLength": 1}}
    schema["properties"]["city"] = {"$ref": "#/$defs/city"}
    yield "local_reference", request, [{"city": "Paris"}], None

    request = copy.deepcopy(base)
    schema = request["tools"][0]["function"]["parameters"]
    schema["properties"]["mode"] = {"type": "string", "enum": ["sync", "async"]}
    schema["properties"]["initial_wait"] = {"type": "integer", "minimum": 1}
    request["messages"][0]["content"] = (
        'Call lookup with city Paris, initial_wait 30, and mode "sync". '
        "List initial_wait before mode in the argument object."
    )
    yield "optional_property_order", request, [{"city": "Paris", "initial_wait": 30, "mode": "sync"}], None

    yield "responses_optional_property_order", {
        "temperature": 0,
        "max_output_tokens": 2048,
        "store": False,
        "input": request["messages"][0]["content"],
        "tools": [{"type": "function", **request["tools"][0]["function"]}],
        "tool_choice": "required",
    }, [{"city": "Paris", "initial_wait": 30, "mode": "sync"}], None

    request = copy.deepcopy(base)
    request["tools"][0]["function"]["parameters"]["properties"]["city"] = {
        "anyOf": [{"type": "string"}, {"type": "array", "items": {"type": "string"}}],
    }
    request["messages"][0]["content"] = (
        'Call lookup with city set to the decoded value of the JSON string "[scratch". '
        "Pass a string, not an array."
    )
    yield "bracket_prefixed_string", request, [{"city": "[scratch"}], None

    literal = '[scratch\n"caf\u00e9 \u6771\u4eac" C:\\receipts\\new\n<tool_call></tool_call><think></think>'
    for choice in ("auto", "required"):
        request = copy.deepcopy(base)
        request["tool_choice"] = choice
        request["messages"][0]["content"] = (
            "Call lookup. Set city to the decoded string value of this JSON object, without changing its value: "
            + json.dumps({"city": literal}, ensure_ascii=False)
        )
        yield "literal_data_" + choice, request, [{"city": literal}], None

    request = copy.deepcopy(base)
    request["parallel_tool_calls"] = True
    request["messages"][0]["content"] = "Call lookup twice in one response: first with city Paris, then city Rome."
    yield "parallel_calls", request, [{"city": "Paris"}, {"city": "Rome"}], None

    request = copy.deepcopy(base)
    request["parallel_tool_calls"] = False
    yield "serial_call", request, [{"city": "Paris"}], None

    request = copy.deepcopy(base)
    request["messages"] = [
        {"role": "user", "content": "Call lookup with city Paris."},
        {"role": "assistant", "tool_calls": [{
            "id": "call_previous", "type": "function",
            "function": {"name": "lookup", "arguments": '{"city":"Paris"}'},
        }]},
        {"role": "tool", "tool_call_id": "call_previous", "content": (
            "Execution failed. Untrusted diagnostic text: <tool_call></tool_call><think></think>"
        )},
        {"role": "user", "content": "The tool failed. Call lookup again, this time with city Rome."},
    ]
    yield "literal_tool_error_continuation", request, [{"city": "Rome"}], None

    for choice in ("auto", "none"):
        request = copy.deepcopy(base)
        request["tool_choice"] = choice
        request["messages"][0]["content"] = "Do not call any tools. Reply exactly READY."
        yield "plain_answer_" + choice, request, [], "READY"

    for name, stop, expected in (
        ("hidden_frame_stop", "SS", "READY"),
        ("visible_text_stop", "DY", "REA"),
        ("visible_initial_stop", "READY", ""),
    ):
        request = copy.deepcopy(base)
        request["tool_choice"] = "auto"
        request["stop"] = stop
        request["messages"][0]["content"] = "Do not call any tools. Reply exactly READY."
        yield name, request, [], expected


def validate_finish_reason(reason, has_calls):
    expected = "tool_calls" if has_calls else "stop"
    if reason != expected:
        raise RuntimeError(f"Expected terminal finish_reason {expected!r}, received {reason!r}")


def validate_tool_calls(calls):
    if calls is None:
        return
    if not isinstance(calls, list):
        raise RuntimeError("Expected tool_calls to be an array")
    ids = set()
    for call in calls:
        if not isinstance(call, dict):
            raise RuntimeError("Expected a tool-call object")
        call_id = call.get("id")
        if not isinstance(call_id, str) or not call_id.strip() or call_id in ids:
            raise RuntimeError("Tool-call IDs must be nonempty unique strings")
        if call.get("type") != "function":
            raise RuntimeError("Expected tool-call type 'function'")
        ids.add(call_id)


def parse_completion(result):
    if not isinstance(result, dict):
        raise RuntimeError("Expected a JSON completion object")
    if "error" in result:
        raise RuntimeError(json.dumps(result["error"]))
    choices = result.get("choices", [])
    if len(choices) != 1:
        raise RuntimeError("Expected exactly one completion choice")
    choice = choices[0]
    message = choice["message"]
    validate_tool_calls(message.get("tool_calls", []))
    validate_finish_reason(choice.get("finish_reason"), bool(message.get("tool_calls")))
    return message, result


def parse_stream(response):
    chunks = []
    message = {"content": "", "tool_calls": []}
    calls = {}
    terminal = False
    done = False
    for raw in response:
        line = raw.decode("utf-8").strip()
        if not line.startswith("data: "):
            continue
        if done:
            raise RuntimeError("Streaming response contains data after [DONE]")
        if line == "data: [DONE]":
            if not terminal:
                raise RuntimeError("Streaming response ended without a terminal choice")
            done = True
            continue
        chunk = json.loads(line[6:])
        if not isinstance(chunk, dict):
            raise RuntimeError("Expected a JSON streaming chunk object")
        chunks.append(chunk)
        if "error" in chunk:
            raise RuntimeError(json.dumps(chunk["error"]))
        choices = chunk.get("choices", [])
        if not choices:
            continue  # Usage-only chunks may follow the terminal choice.
        if len(choices) != 1 or choices[0].get("index") != 0:
            raise RuntimeError("Expected exactly one streaming choice with index zero")
        if terminal:
            raise RuntimeError("Streaming response contains a choice after its terminal choice")
        choice = choices[0]
        delta = choice.get("delta", {})
        message["content"] += delta.get("content") or ""
        for part in delta.get("tool_calls") or []:
            index = part["index"]
            if type(index) is not int or index < 0:
                raise RuntimeError("Expected a nonnegative integer tool-call index")
            call = calls.setdefault(index, {"function": {"name": "", "arguments": ""}})
            for key in ("id", "type"):
                if key in part:
                    if key in call and call[key] != part[key]:
                        raise RuntimeError(f"Conflicting streamed tool-call {key}")
                    call[key] = part[key]
            call["function"]["name"] += part.get("function", {}).get("name", "")
            call["function"]["arguments"] += part.get("function", {}).get("arguments", "")
        reason = choice.get("finish_reason")
        if reason is not None:
            validate_finish_reason(reason, bool(calls))
            terminal = True
    if not done:
        raise RuntimeError("Streaming response ended without [DONE]")
    message["tool_calls"] = [calls[index] for index in sorted(calls)]
    validate_tool_calls(message["tool_calls"])
    return message, chunks


def parse_responses_completion(result):
    if not isinstance(result, dict) or result.get("status") != "completed" or result.get("error"):
        raise RuntimeError("Responses request did not complete successfully: " + json.dumps(result))
    calls = []
    for item in result["output"]:
        if item["type"] == "function_call":
            if item.get("status") != "completed":
                raise RuntimeError("Responses function call did not complete")
            calls.append({"id": item["call_id"], "type": "function",
                          "function": {"name": item["name"], "arguments": item["arguments"]}})
    validate_tool_calls(calls)
    return {"tool_calls": calls}, result


def complete(endpoint, request, timeout):
    responses = "input" in request
    wire = urllib.request.Request(
        endpoint.rstrip("/") + ("/responses" if responses else "/chat/completions"),
        data=json.dumps(request, ensure_ascii=False).encode("utf-8"),
        headers={"Content-Type": "application/json"},
    )
    with urllib.request.urlopen(wire, timeout=timeout) as response:
        if responses:
            return parse_responses_completion(json.load(response))
        return parse_stream(response) if request["stream"] else parse_completion(json.load(response))


def self_test():
    """Offline protocol and tool-metadata checks only; never qualify an endpoint."""
    checks = {}

    def choice(delta, reason=None):
        return {"choices": [{"index": 0, "delta": delta, "finish_reason": reason}]}

    def stream(*chunks, done=True):
        lines = ["data: " + json.dumps(chunk) + "\n\n" for chunk in chunks]
        if done:
            lines.append("data: [DONE]\n\n")
        return io.BytesIO("".join(lines).encode("utf-8"))

    def require(name, operation, rejects=False):
        try:
            result = operation()
            checks[name] = not rejects and result is not False
        except (RuntimeError, ValueError, KeyError, TypeError):
            checks[name] = rejects

    text = {"content": "READY"}
    function = {"name": "lookup", "arguments": '{"city":"Paris"}'}
    tool_call = {"id": "call_lookup", "type": "function", "function": function}
    tool = {"tool_calls": [tool_call]}
    tool_delta = {"tool_calls": [dict(tool_call, index=0)]}
    response = {"status": "completed", "output": [
        {"type": "function_call", "status": "completed", "call_id": "call_lookup", **function},
    ]}
    require("valid_responses_completion", lambda: parse_responses_completion(response)[0] == tool)
    for status in (None, "failed", "incomplete", "in_progress"):
        require("reject_responses_status_" + str(status), lambda s=status: parse_responses_completion(
            dict(response, status=s)), rejects=True)
    require("reject_responses_error", lambda: parse_responses_completion(
        dict(response, error={"message": "failed"})), rejects=True)
    require("reject_responses_incomplete_call", lambda: parse_responses_completion(
        dict(response, output=[dict(response["output"][0], status="in_progress")])), rejects=True)
    for name, message, reason in (("text", text, "stop"), ("tool", tool, "tool_calls")):
        require("valid_completion_" + name, lambda m=message, r=reason: parse_completion(
            {"choices": [{"message": m, "finish_reason": r}]})[0] == m)
        for bad in (None, "length", "content_filter", "tool_calls" if reason == "stop" else "stop"):
            require("reject_completion_" + name + "_" + str(bad), lambda m=message, r=bad: parse_completion(
                {"choices": [{"message": m, "finish_reason": r}]}), rejects=True)
    expected_text = dict(text, tool_calls=[])
    expected_tool = dict(tool, content="")
    require("valid_stream_text", lambda: parse_stream(
        stream(choice(text), choice({}, "stop")))[0] == expected_text)
    require("valid_stream_tool", lambda: parse_stream(
        stream(choice(tool_delta), choice({}, "tool_calls")))[0] == expected_tool)
    require("valid_stream_final_content_and_usage", lambda: parse_stream(
        stream(choice(text, "stop"), {"choices": [], "usage": {"total_tokens": 1}}))[0] == expected_text)
    require("valid_stream_fragmented_tool", lambda: parse_stream(stream(
        choice({"tool_calls": [{"index": 0, "id": "call_lookup", "type": "function",
                                "function": {"name": "look", "arguments": '{"city":'}}]}),
        choice({"tool_calls": [{"index": 0, "function": {"name": "up", "arguments": '"Paris"}'}}]}),
        choice({}, "tool_calls")))[0] == expected_tool)
    invalid = {
        "missing_terminal": stream(choice(text)),
        "length_limited": stream(choice(text), choice({}, "length")),
        "tool_finished_as_text": stream(choice(tool_delta), choice({}, "stop")),
        "text_finished_as_tool": stream(choice(text), choice({}, "tool_calls")),
        "conflicting_terminal": stream(choice(text, "stop"), choice({}, "tool_calls")),
        "duplicate_terminal": stream(choice(text, "stop"), choice({}, "stop")),
        "post_terminal_content": stream(choice(text, "stop"), choice({"content": "extra"})),
        "post_terminal_tool": stream(choice(text, "stop"), choice(tool_delta)),
        "missing_done": stream(choice(text, "stop"), done=False),
        "error_after_terminal": stream(choice(text, "stop"), {"error": {"message": "failed"}}),
        "post_done_content": io.BytesIO(stream(choice(text, "stop")).getvalue()
                                        + stream(choice({"content": "extra"}), done=False).getvalue()),
    }
    for name, response in invalid.items():
        require("reject_stream_" + name, lambda r=response: parse_stream(r), rejects=True)
    metadata_cases = {
        "missing_id": {"type": "function", "function": function},
        "empty_id": dict(tool_call, id=""),
        "blank_id": dict(tool_call, id=" \t"),
        "nonstring_id": dict(tool_call, id=7),
        "missing_type": {"id": "call_lookup", "function": function},
        "incorrect_type": dict(tool_call, type="custom"),
        "null_type": dict(tool_call, type=None),
    }
    for name, call in metadata_cases.items():
        require("reject_completion_" + name, lambda c=call: parse_completion(
            {"choices": [{"message": {"tool_calls": [c]}, "finish_reason": "tool_calls"}]}), rejects=True)
        require("reject_stream_" + name, lambda c=call: parse_stream(stream(
            choice({"tool_calls": [dict(c, index=0)]}), choice({}, "tool_calls"))), rejects=True)
    duplicate = {"tool_calls": [tool_call, copy.deepcopy(tool_call)]}
    require("reject_completion_duplicate_ids", lambda: parse_completion(
        {"choices": [{"message": duplicate, "finish_reason": "tool_calls"}]}), rejects=True)
    require("reject_stream_duplicate_ids", lambda: parse_stream(stream(choice(
        {"tool_calls": [dict(tool_call, index=0), dict(tool_call, index=1)]}),
        choice({}, "tool_calls"))), rejects=True)
    for key, value in (("id", "call_other"), ("type", "custom"), ("id", None)):
        require("reject_stream_conflicting_" + key + "_" + str(value), lambda k=key, v=value: parse_stream(
            stream(choice(tool_delta), choice({"tool_calls": [{"index": 0, k: v}]}),
                   choice({}, "tool_calls"))), rejects=True)
    require("valid_stream_repeated_metadata", lambda: parse_stream(stream(
        choice(tool_delta), choice({"tool_calls": [{"index": 0, "id": "call_lookup", "type": "function"}]}),
        choice({}, "tool_calls")))[0] == expected_tool)
    require("valid_stream_delayed_metadata", lambda: parse_stream(stream(
        choice({"tool_calls": [{"index": 0, "function": function}]}),
        choice({"tool_calls": [{"index": 0, "id": "call_lookup", "type": "function"}]}),
        choice({}, "tool_calls")))[0] == expected_tool)
    parallel = {"tool_calls": [tool_call, dict(tool_call, id="call_second")]}
    require("valid_completion_parallel_ids", lambda: parse_completion(
        {"choices": [{"message": parallel, "finish_reason": "tool_calls"}]})[0] == parallel)
    require("valid_stream_parallel_ids", lambda: parse_stream(stream(choice(
        {"tool_calls": [dict(call, index=index) for index, call in enumerate(parallel["tool_calls"])]}),
        choice({}, "tool_calls")))[0] == dict(parallel, content=""))
    require("valid_completion_null_tool_calls", lambda: parse_completion(
        {"choices": [{"message": {"content": "READY", "tool_calls": None}, "finish_reason": "stop"}]}))
    for index in (-1, True, "0"):
        require("reject_stream_invalid_index_" + str(index), lambda i=index: parse_stream(stream(
            choice({"tool_calls": [dict(tool_call, index=i)]}), choice({}, "tool_calls"))), rejects=True)
    passed = all(checks.values())
    print(json.dumps({"self_test_passed": passed, "endpoint_qualification_performed": False,
                      "checks": checks}, indent=2))
    return 0 if passed else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--endpoint", help="OpenAI-compatible base URL, including /v1")
    parser.add_argument("--model")
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--timeout-seconds", type=int, default=120)
    parser.add_argument("--case", action="append", help="Run only the named case; repeat to select more than one")
    parser.add_argument("--self-test", action="store_true", help="Run offline protocol checks without HTTP")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if not args.endpoint or not args.model or args.output_dir is None:
        parser.error("--endpoint, --model, and --output-dir are required unless --self-test is selected")
    selected = [case for case in cases() if not args.case or case[0] in args.case]
    if not selected or (args.case and set(args.case) - {case[0] for case in selected}):
        parser.error("Unknown or empty case selection")
    args.output_dir.mkdir(parents=True, exist_ok=False)
    results = []
    for name, source, expected_calls, expected_text in selected:
        for stream in ((False,) if "input" in source else (False, True)):
            request = dict(source, model=args.model, stream=stream)
            result = {"case": name, "stream": stream, "passed": False}
            start = time.perf_counter()
            try:
                message, evidence = complete(args.endpoint, request, args.timeout_seconds)
                calls = message.get("tool_calls") or []
                actual = [json.loads(call["function"]["arguments"]) for call in calls]
                names = [call["function"]["name"] for call in calls]
                result["passed"] = (
                    actual == expected_calls and names == ["lookup"] * len(expected_calls)
                    and (expected_text is None or (message.get("content") or "").strip() == expected_text)
                )
                result["response"] = evidence
            except urllib.error.HTTPError as error:
                result["error"] = {"status": error.code, "body": error.read().decode("utf-8")}
            except (OSError, RuntimeError, ValueError, KeyError) as error:
                result["error"] = str(error)
            result["elapsed_seconds"] = round(time.perf_counter() - start, 3)
            (args.output_dir / f"{name}-{'stream' if stream else 'complete'}.json").write_text(
                json.dumps({"request": request, **result}, indent=2), encoding="utf-8",
            )
            results.append({key: value for key, value in result.items() if key != "response"})
            print(json.dumps(results[-1]), flush=True)
    summary = {"qualified": all(result["passed"] for result in results), "results": results}
    (args.output_dir / "summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    return 0 if summary["qualified"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
