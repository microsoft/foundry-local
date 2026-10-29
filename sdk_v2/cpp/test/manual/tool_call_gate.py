#!/usr/bin/env python3
# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.
"""Real-model protocol checks. This does not replace the actual Copilot coding-agent qualification."""

import argparse
import ast
import copy
import io
import json
from pathlib import Path
import re
import time
import urllib.error
import urllib.request


FAILED_TOOL_RECOVERY = "failed_tool_recovery_auto"
TOOL_FAILURE = "Intentional qualification failure. Do not retry; write the requested Python function and test."


def failed_tool_recovery_request():
    return {
        "temperature": 0,
        "max_tokens": 2048,
        "tools": [{
            "type": "function",
            "function": {
                "name": "multiply",
                "description": "Multiply two integers. Call this tool when the user explicitly requests multiplication.",
                "parameters": {
                    "type": "object",
                    "properties": {"a": {"type": "integer"}, "b": {"type": "integer"}},
                    "required": ["a", "b"],
                    "additionalProperties": False,
                },
            },
        }],
        "messages": [
            {"role": "system", "content": "You are a coding assistant. Follow the user's instructions."},
            {"role": "user", "content": (
                "Call multiply with a=7 and b=9. If that tool fails, do not retry it. "
                "Instead write a Python multiply(a,b) function and one unit test asserting multiply(7,9)==63."
            )},
        ],
    }


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

    yield FAILED_TOOL_RECOVERY, failed_tool_recovery_request(), [], None


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
        if delta.get("reasoning_content") is not None:
            message["reasoning_content"] = message.get("reasoning_content", "") + delta["reasoning_content"]
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


def is_multiply_test_call(node):
    return (
        isinstance(node, ast.Call) and isinstance(node.func, ast.Name) and node.func.id == "multiply"
        and not node.keywords and len(node.args) == 2
        and all(isinstance(arg, ast.Constant) and type(arg.value) is int and arg.value == value
                for arg, value in zip(node.args, (7, 9)))
    )


def is_expected_assertion(node):
    if isinstance(node, ast.Assert):
        test = node.test
        return (
            isinstance(test, ast.Compare) and len(test.ops) == 1 and isinstance(test.ops[0], ast.Eq)
            and is_multiply_test_call(test.left) and len(test.comparators) == 1
            and isinstance(test.comparators[0], ast.Constant)
            and type(test.comparators[0].value) is int and test.comparators[0].value == 63
        )
    return (
        isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute) and node.func.attr == "assertEqual"
        and len(node.args) == 2 and is_multiply_test_call(node.args[0])
        and isinstance(node.args[1], ast.Constant) and type(node.args[1].value) is int and node.args[1].value == 63
    )


def has_python_multiply_and_test(content):
    # Inspect syntax only: never execute model-generated code as part of qualification.
    blocks = re.findall(r"```(?:python|py)?[ \t]*\r?\n(.*?)```", content, re.DOTALL)
    try:
        tree = ast.parse("\n\n".join(blocks) if blocks else content)
    except SyntaxError:
        return False
    functions = [node for node in ast.walk(tree) if isinstance(node, ast.FunctionDef) and node.name == "multiply"]
    valid_function = False
    for function in functions:
        body = function.body
        if body and isinstance(body[0], ast.Expr) and isinstance(body[0].value, ast.Constant):
            body = body[1:]
        if [arg.arg for arg in function.args.args] != ["a", "b"] or len(body) != 1:
            continue
        value = body[0].value if isinstance(body[0], ast.Return) else None
        valid_function |= (
            isinstance(value, ast.BinOp) and isinstance(value.op, ast.Mult)
            and isinstance(value.left, ast.Name) and value.left.id == "a"
            and isinstance(value.right, ast.Name) and value.right.id == "b"
        )
    return valid_function and any(is_expected_assertion(node) for node in ast.walk(tree))


def fail_multiply(a, b):
    if type(a) is not int or type(b) is not int or (a, b) != (7, 9):
        raise ValueError("Expected multiply arguments a=7, b=9")
    raise RuntimeError(TOOL_FAILURE)


def complete_failed_tool_recovery(endpoint, request, timeout, transport=complete):
    first, first_evidence = transport(endpoint, copy.deepcopy(request), timeout)
    calls = first.get("tool_calls") or []
    if len(calls) != 1 or calls[0]["function"]["name"] != "multiply":
        raise RuntimeError("Expected one initial multiply call")
    call = calls[0]
    arguments = json.loads(call["function"]["arguments"])
    if arguments != {"a": 7, "b": 9}:
        raise RuntimeError("Expected initial multiply arguments a=7, b=9")
    try:
        fail_multiply(**arguments)
    except RuntimeError as error:
        tool_result = str(error)
    else:
        raise RuntimeError("Expected the multiply callback to fail")
    recovery = copy.deepcopy(request)
    recovery["messages"] += [
        dict(first, role="assistant"),
        {"role": "tool", "tool_call_id": call["id"], "content": tool_result},
    ]
    final, final_evidence = transport(endpoint, recovery, timeout)
    return {
        "passed": not final.get("tool_calls") and has_python_multiply_and_test(final.get("content") or ""),
        "callback_count": 1,
        "tools_unchanged": recovery["tools"] == request["tools"],
        "response": {"first": first_evidence, "recovery": final_evidence},
    }


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
    require("valid_stream_reasoning_history", lambda: parse_stream(stream(
        choice({"reasoning_content": "Need "}), choice({"reasoning_content": "multiply."}),
        choice(tool_delta), choice({}, "tool_calls")))[0] == dict(expected_tool, reasoning_content="Need multiply."))
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
    python = "def multiply(a, b):\n    return a * b\n\nassert multiply(7, 9) == 63\n"
    require("recovery_accepts_python_and_exact_assertion", lambda: has_python_multiply_and_test(python))
    require("recovery_accepts_fenced_python", lambda: has_python_multiply_and_test("```python\n" + python + "```"))
    require("recovery_accepts_unittest", lambda: has_python_multiply_and_test(
        "def multiply(a, b):\n    return a * b\n\n"
        "import unittest\nclass TestMultiply(unittest.TestCase):\n"
        "    def test_product(self):\n        self.assertEqual(multiply(7, 9), 63)\n"))
    for name, source in {
        "empty_response": "",
        "wrong_product": python.replace("== 63", "== 64"),
        "wrong_function": python.replace("a * b", "a + b"),
        "missing_test": "def multiply(a, b):\n    return a * b\n",
        "text_only": "Define multiply(a,b) and assert that the result is 63.",
        "invalid_python": "def multiply(",
    }.items():
        require("recovery_rejects_" + name, lambda s=source: not has_python_multiply_and_test(s))

    def recovery_round_trip(final):
        request = dict(failed_tool_recovery_request(), model="offline-model", stream=False)
        original = copy.deepcopy(request)
        seen = []
        fresh_call = {"id": "fresh_call", "type": "function",
                      "function": {"name": "multiply", "arguments": '{"a":7,"b":9}'}}
        first = {"role": "assistant", "content": "", "reasoning_content": "Need multiply.", "tool_calls": [fresh_call]}

        def transport(endpoint, body, timeout):
            seen.append(copy.deepcopy(body))
            return (first, {}) if len(seen) == 1 else (final, {})

        result = complete_failed_tool_recovery("offline-endpoint", request, 1, transport)
        require("recovery_does_not_mutate_original_request", lambda: request == original)
        require("recovery_preserves_tools_and_auto_default", lambda: (
            seen[0]["tools"] == seen[1]["tools"] == request["tools"]
            and all("tool_choice" not in body and "reasoning_effort" not in body for body in seen)))
        require("recovery_preserves_real_call_reasoning_and_error", lambda: (
            seen[1]["messages"] == original["messages"] + [
                first, {"role": "tool", "tool_call_id": "fresh_call", "content": TOOL_FAILURE}]))
        require("recovery_executes_exactly_one_callback", lambda: result["callback_count"] == 1 and len(seen) == 2)
        return result

    require("recovery_qualifies_final_python", lambda: recovery_round_trip({"content": python})["passed"])
    require("recovery_rejects_repeated_call", lambda: not recovery_round_trip(tool)["passed"])
    require("recovery_rejects_noninteger_callback_arguments", lambda: fail_multiply(7.0, 9), rejects=True)
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
                if name == FAILED_TOOL_RECOVERY:
                    result.update(complete_failed_tool_recovery(args.endpoint, request, args.timeout_seconds))
                else:
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
