"""Correctness assertions for Foundry Local release-validation feature runners.

Every assertion is pure, dependency-free, and returns the result-record schema shape used by
``validation.orchestrator.results`` and ``runners._assert``::

    {"name": str, "ok": bool, "detail": str | None}

The functions are intentionally small and conservative: they validate observable behavior
from SDK samples and OpenAI-compatible server responses without relying on any SDK package.
"""
from __future__ import annotations

import json
import math
import re
from typing import Any, Iterable, Sequence

Assertion = dict[str, Any]


def _result(name: str, ok: bool, detail: str | None = None) -> Assertion:
    """Return a schema-compatible assertion dictionary."""
    return {"name": name, "ok": bool(ok), "detail": None if ok else detail}


def assert_chat_nonempty(response_text: Any) -> Assertion:
    """Assert that a chat response is a non-empty string containing non-whitespace text."""
    ok = isinstance(response_text, str) and bool(response_text.strip())
    return _result("chat response is non-empty", ok, "response text is empty or not a string")


def assert_chat_completion_shape(obj: Any) -> Assertion:
    """Assert that an OpenAI-style chat completion has assistant content and a finish reason."""
    errors: list[str] = []
    choice = _first_choice(obj, errors)
    message = choice.get("message") if isinstance(choice, dict) else None
    if not isinstance(message, dict):
        errors.append("choices[0].message is missing or not an object")
    else:
        if message.get("role") != "assistant":
            errors.append("choices[0].message.role is not 'assistant'")
        if "content" not in message:
            errors.append("choices[0].message.content is missing")
        elif not isinstance(message.get("content"), str):
            errors.append("choices[0].message.content is not a string")
    if not isinstance(choice, dict) or not choice.get("finish_reason"):
        errors.append("choices[0].finish_reason is missing")
    return _result("chat completion has expected shape", not errors, "; ".join(errors))


def assert_stream_reconstruction(chunks: Any, full_text: Any) -> Assertion:
    """Assert streamed text deltas concatenate to ``full_text`` and a final stop was observed."""
    if not isinstance(full_text, str):
        return _result("stream reconstructs final text", False, "full_text is not a string")
    if not isinstance(chunks, Iterable) or isinstance(chunks, (str, bytes, dict)):
        return _result("stream reconstructs final text", False, "chunks is not an iterable of chunk objects")

    pieces: list[str] = []
    saw_stop = False
    for index, chunk in enumerate(chunks):
        if not isinstance(chunk, dict):
            return _result("stream reconstructs final text", False, f"chunk {index} is not an object")
        choices = chunk.get("choices")
        if not isinstance(choices, list) or not choices:
            return _result("stream reconstructs final text", False, f"chunk {index} has no choices")
        choice = choices[0]
        if not isinstance(choice, dict):
            return _result("stream reconstructs final text", False, f"chunk {index} choices[0] is not an object")
        delta = choice.get("delta") or {}
        if delta is not None and not isinstance(delta, dict):
            return _result("stream reconstructs final text", False, f"chunk {index} delta is not an object")
        content = delta.get("content") if isinstance(delta, dict) else None
        if content is not None:
            if not isinstance(content, str):
                return _result("stream reconstructs final text", False, f"chunk {index} delta.content is not a string")
            pieces.append(content)
        saw_stop = saw_stop or choice.get("finish_reason") == "stop"

    reconstructed = "".join(pieces)
    if reconstructed != full_text:
        return _result("stream reconstructs final text", False, "streamed deltas do not match final text")
    if not saw_stop:
        return _result("stream reconstructs final text", False, "stream did not terminate with finish_reason='stop'")
    return _result("stream reconstructs final text", True)


def assert_tool_call(obj: Any, expected_name: str, required_arg_keys: Sequence[str]) -> Assertion:
    """Assert a tool/function call with JSON arguments contains all required argument keys."""
    calls = _extract_tool_calls(obj)
    if not calls:
        return _result("tool call has expected name and arguments", False, "no tool/function call found")

    seen_names: list[str] = []
    for call in calls:
        name, args_raw = _tool_call_name_and_args(call)
        if name:
            seen_names.append(name)
        if name != expected_name:
            continue
        try:
            args = _parse_json_args(args_raw)
        except ValueError as exc:
            return _result("tool call has expected name and arguments", False, str(exc))
        missing = [key for key in required_arg_keys if key not in args]
        if missing:
            return _result("tool call has expected name and arguments", False, f"missing argument keys: {missing}")
        return _result("tool call has expected name and arguments", True)

    return _result("tool call has expected name and arguments", False, f"expected {expected_name!r}; saw {seen_names}")


def assert_embedding(vectors: Any, expected_count: int, expected_dim: int | None = None) -> Assertion:
    """Assert embedding vectors have expected count, finite numeric values, dimensions, and non-zero norm."""
    if not isinstance(expected_count, int) or expected_count < 0:
        return _result("embedding vectors are valid", False, "expected_count must be a non-negative integer")
    if expected_dim is not None and (not isinstance(expected_dim, int) or expected_dim <= 0):
        return _result("embedding vectors are valid", False, "expected_dim must be a positive integer")
    if not isinstance(vectors, list):
        return _result("embedding vectors are valid", False, "vectors is not a list")
    if len(vectors) != expected_count:
        return _result("embedding vectors are valid", False, f"expected {expected_count} vectors, got {len(vectors)}")

    dimension: int | None = expected_dim
    for index, vector in enumerate(vectors):
        if not isinstance(vector, list):
            return _result("embedding vectors are valid", False, f"vector {index} is not a list")
        if dimension is None:
            dimension = len(vector)
        if len(vector) != dimension:
            detail = f"vector {index} has dimension {len(vector)}, expected {dimension}"
            return _result("embedding vectors are valid", False, detail)
        if not vector:
            return _result("embedding vectors are valid", False, f"vector {index} is empty")
        norm_sq = 0.0
        for value_index, value in enumerate(vector):
            if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
                return _result("embedding vectors are valid", False, f"vector {index}[{value_index}] is not finite")
            norm_sq += float(value) * float(value)
        if norm_sq == 0.0:
            return _result("embedding vectors are valid", False, f"vector {index} has zero norm")
    return _result("embedding vectors are valid", True)


def assert_transcription(hypothesis: Any, reference: Any, max_wer: float = 0.4) -> Assertion:
    """Assert word error rate between hypothesis and reference is at or below ``max_wer``."""
    if not isinstance(hypothesis, str) or not isinstance(reference, str):
        return _result("transcription word error rate is acceptable", False, "hypothesis/reference must be strings")
    if not isinstance(max_wer, (int, float)) or isinstance(max_wer, bool) or max_wer < 0:
        return _result("transcription word error rate is acceptable", False, "max_wer must be a non-negative number")
    hyp_words = _words(hypothesis)
    ref_words = _words(reference)
    wer = _word_error_rate(hyp_words, ref_words)
    ok = wer <= float(max_wer)
    detail = None if ok else f"WER {wer:.3f} exceeds threshold {float(max_wer):.3f}"
    return _result("transcription word error rate is acceptable", ok, detail)


def assert_http_ok(status_code: Any) -> Assertion:
    """Assert an HTTP status code is in the 2xx success range."""
    ok = isinstance(status_code, int) and not isinstance(status_code, bool) and 200 <= status_code <= 299
    return _result("http status is 2xx", ok, f"status_code={status_code!r}")


def assert_openai_error_shape(obj: Any) -> Assertion:
    """Assert an OpenAI-style error payload has ``error.message`` and ``error.type`` strings."""
    error = obj.get("error") if isinstance(obj, dict) else None
    ok = isinstance(error, dict) and isinstance(error.get("message"), str) and isinstance(error.get("type"), str)
    ok = ok and bool(error.get("message")) and bool(error.get("type"))
    return _result("openai error has expected shape", ok, "missing non-empty error.message or error.type")


def assert_json_schema_subset(obj: Any, required_paths: Sequence[str]) -> Assertion:
    """Assert every dotted key path in ``required_paths`` exists in ``obj``.

    Path components address dictionary keys; integer components also address list indexes, so paths such as
    ``choices.0.message.content`` are supported for OpenAI-style arrays.
    """
    missing = [path for path in required_paths if not _path_exists(obj, path)]
    ok = not missing
    return _result("json schema subset paths exist", ok, f"missing paths: {missing}")


def assert_version_equals(actual: Any, expected: Any) -> Assertion:
    """Assert two version strings are equal after normalizing common rc/pre-release spellings."""
    if not isinstance(actual, str) or not isinstance(expected, str):
        return _result("version matches expected", False, "actual/expected versions must be strings")
    actual_norm = _normalize_version(actual)
    expected_norm = _normalize_version(expected)
    ok = actual_norm == expected_norm
    return _result("version matches expected", ok, f"actual={actual_norm!r}, expected={expected_norm!r}")


def all_ok(assertions: Iterable[Assertion]) -> bool:
    """Return True when every assertion dictionary has ``ok is True``."""
    return all(assertion.get("ok") is True for assertion in assertions)


def summarize(assertions: Sequence[Assertion]) -> str:
    """Return a compact human-readable assertion summary."""
    total = len(assertions)
    passed = sum(1 for assertion in assertions if assertion.get("ok") is True)
    failed = [str(assertion.get("name", "<unnamed>")) for assertion in assertions if assertion.get("ok") is not True]
    if not failed:
        return f"{passed}/{total} assertions passed"
    return f"{passed}/{total} assertions passed; failed: {', '.join(failed)}"


def _first_choice(obj: Any, errors: list[str]) -> dict[str, Any] | None:
    if not isinstance(obj, dict):
        errors.append("completion is not an object")
        return None
    choices = obj.get("choices")
    if not isinstance(choices, list) or not choices:
        errors.append("choices is missing or empty")
        return None
    if not isinstance(choices[0], dict):
        errors.append("choices[0] is not an object")
        return None
    return choices[0]


def _extract_tool_calls(obj: Any) -> list[Any]:
    if not isinstance(obj, dict):
        return []
    direct = obj.get("tool_calls")
    if isinstance(direct, list):
        return direct
    if isinstance(obj.get("function_call"), dict):
        return [obj["function_call"]]

    choices = obj.get("choices")
    if not isinstance(choices, list) or not choices or not isinstance(choices[0], dict):
        return []
    message = choices[0].get("message")
    if not isinstance(message, dict):
        return []
    if isinstance(message.get("tool_calls"), list):
        return message["tool_calls"]
    if isinstance(message.get("function_call"), dict):
        return [message["function_call"]]
    return []


def _tool_call_name_and_args(call: Any) -> tuple[str | None, Any]:
    if not isinstance(call, dict):
        return None, None
    function = call.get("function")
    if isinstance(function, dict):
        return function.get("name"), function.get("arguments")
    return call.get("name"), call.get("arguments")


def _parse_json_args(args_raw: Any) -> dict[str, Any]:
    if isinstance(args_raw, dict):
        return args_raw
    if not isinstance(args_raw, str):
        raise ValueError("tool arguments are not a JSON object string")
    try:
        args = json.loads(args_raw)
    except json.JSONDecodeError as exc:
        raise ValueError(f"tool arguments are not valid JSON: {exc.msg}") from exc
    if not isinstance(args, dict):
        raise ValueError("tool arguments JSON is not an object")
    return args


def _words(text: str) -> list[str]:
    return re.findall(r"[\w']+", text.lower())


def _word_error_rate(hyp_words: Sequence[str], ref_words: Sequence[str]) -> float:
    if not ref_words:
        return 0.0 if not hyp_words else math.inf
    previous = list(range(len(hyp_words) + 1))
    for row, ref_word in enumerate(ref_words, start=1):
        current = [row]
        for column, hyp_word in enumerate(hyp_words, start=1):
            substitution_cost = 0 if ref_word == hyp_word else 1
            current.append(min(previous[column] + 1, current[column - 1] + 1, previous[column - 1] + substitution_cost))
        previous = current
    return previous[-1] / len(ref_words)


def _path_exists(obj: Any, path: str) -> bool:
    if not isinstance(path, str) or not path:
        return False
    current = obj
    for part in path.split("."):
        if isinstance(current, dict):
            if part not in current:
                return False
            current = current[part]
        elif isinstance(current, list) and part.isdigit():
            index = int(part)
            if index >= len(current):
                return False
            current = current[index]
        else:
            return False
    return True


def _normalize_version(version: str) -> tuple[int, int, int, tuple[str, int] | None] | str:
    text = version.strip().lower().replace("_", "-")
    if text.startswith("v"):
        text = text[1:]
    text = text.split("+", 1)[0]
    match = re.fullmatch(r"(\d+)\.(\d+)\.(\d+)(?:[-.]?(rc|pre|preview|a|alpha|b|beta)[-.]?(\d+)?)?", text)
    if not match:
        return text
    major, minor, patch, label, number = match.groups()
    label_map = {"alpha": "a", "beta": "b", "preview": "pre"}
    prerelease = None if label is None else (label_map.get(label, label), int(number or 0))
    return int(major), int(minor), int(patch), prerelease
