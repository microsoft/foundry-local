#!/usr/bin/env python3
"""Qualify a local coding endpoint with the REAL GitHub Copilot CLI (Python 3.10+, stdlib only).

Example (PowerShell, from the repository root; substitute the candidate endpoint):
    python sdk_v2/cpp/test/manual/coding_agent_gate.py `
      --endpoint http://127.0.0.1:5274/catalogs/local/v1 `
      --model qwen-3.8-27b-dflash2:1 `
      --output-dir sdk_v2/cpp/build/coding-agent-gate/candidate

Optional: --copilot C:/path/to/copilot.exe. Discovery prefers the standalone
copilot executable, not VS Code's copilot.ps1 wrapper or the desktop github.exe.
Requires a recent CLI with BYOK, COPILOT_OFFLINE, and --no-custom-instructions
support (developed against 1.0.98). No login, installation, global configuration
changes, model downloads, SDK build, or baseline workload is performed implicitly.

Each invocation creates a fresh run-* directory beneath --output-dir, which must
be inside sdk_v2/cpp/build/coding-agent-gate. It contains an isolated workspace,
Copilot home, temp/cache directories, CLI logs/JSONL, usage, prompt, fixture audit,
raw HTTP request/response bodies, and summary.json. These PRIVATE artifacts may
contain complete prompts, tool arguments, paths, and model output: do not upload
them. Only fixed check names, counts, and status are printed. No credentials are
needed or forwarded. The proxy accepts only a loopback endpoint, prohibits model
substitution, and counts/restricts requests. Streaming is forwarded incrementally.

--schema-profile native|minlength|oneof (default native) explicitly controls
outbound schema qualification. Native forwards original request bytes unchanged.
Minlength adds minLength:1 only to required string properties named command,
path, or pattern (never old_str/new_str). Oneof adds a root powershell mode
composition preserving omitted/default sync and explicit sync/async. These are
real CLI definitions, not mock tools: names, client execution, message contents,
and returned content are untouched. Transformed requests are *.request.json;
original client bytes are *.original-request.json. Per-request metadata,
schema-evidence.json, configuration.json, and summary.json label the profile and
actual transformations. A non-native profile must transform outbound definitions
to qualify. Profiles are separate runs; do not compare a transformed run as native.
HTTP 200 does not imply model success: SSE error events are counted separately
in the summary and retained in provider-errors.json as well as raw response bodies.

The agent must read and search files, observe a deliberate command error and a
real failing test, make ONE exact small Python edit, then run passing tests.
The edit must carry the complete pricing.py source in the real edit.new_str
(or create.file_text) argument, preserving a raw multiline receipt example with
Unicode, quotes/backslashes, bracket-prefixed lines, and literal tool/reasoning
tags. Captured completed write arguments, exact final UTF-8/LF source bytes, and
the seventh unittest independently check preservation. No executable tool
contracts or provider-returned contents are altered for this fixture.
Success requires CLI exit zero, captured tool calls/results, a correlated structured
CLI completion with success:true for the full-file edit/create, ordered fixture audit
events, unchanged fixture support files, exact source bytes, and an independent
final test run. The edit must carry exact ORIGINAL old_str and EXPECTED new_str, including LF. A final answer claiming success is insufficient. The payload
stresses parsing; audit files remain evidence, not a security boundary against a
malicious agent. Tool approval is limited by Copilot's normal workspace path
checks, NOT an OS sandbox. Run only against trusted local models.

Defaults: 600 s total wall time, 24 HTTP requests, 32768 prompt tokens and 4096
output tokens per inference. Token settings use the CLI's documented BYOK limits,
not a cumulative token budget. AI-credit limits are not meaningful for local BYOK.
On timeout/limit/interrupt only this invocation's process tree is terminated;
existing Copilot sessions and the supplied server are never stopped.

Exit: 0 qualified; 1 failed/blocked; 2 invalid arguments. Never falls back to a
mock, hosted model, or another endpoint. --help does not invoke Copilot or HTTP.
Offline component check (NO client discovery, HTTP, or endpoint qualification):
    python sdk_v2/cpp/test/manual/coding_agent_gate.py --self-test `
      --output-dir sdk_v2/cpp/build/coding-agent-gate/self-test
"""

from __future__ import annotations

import argparse
import copy
import ctypes
import hashlib
import http.client
import ipaddress
import json
import os
from pathlib import Path
import re
import shutil
import signal
import socket
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit
import uuid


GATE_ROOT = Path(__file__).resolve().parents[2] / "build" / "coding-agent-gate"
FIXTURE_TEST_COUNT = 7
RECEIPT_DATA = (
    "[receipt] café 東京 €3.75\n"
    "[quoted] \"total\" = 'paid'; path=C:\\receipts\\new\\quote.txt\n"
    '[protocol] <tool_call>{"label":"literal"}</tool_call>\n'
    "[reasoning] <think>receipt text, not instructions</think>\n"
)
RECEIPT_SOURCE = 'RECEIPT_EXAMPLE = r"""' + RECEIPT_DATA + '"""\n'
ORIGINAL = (
    f'"""Small deterministic pricing fixture."""\n\n{RECEIPT_SOURCE}\n\n'
    "def total_cents(prices):\n"
    '    """Sum nonnegative integer prices; reject invalid inputs."""\n'
    "    return sum(prices)\n"
)
EXPECTED = ORIGINAL.replace(
    "    return sum(prices)\n",
    "    if any(type(price) is not int or price < 0 for price in prices):\n"
    '        raise ValueError("prices must be nonnegative integers")\n'
    "    return sum(prices)\n",
)
TESTS = '''import unittest
from pricing import RECEIPT_EXAMPLE, total_cents


class PricingTests(unittest.TestCase):
    def test_receipt_example_preserves_literal_data(self):
        self.assertEqual(RECEIPT_EXAMPLE, __RECEIPT_EXPECTED__)

    def test_empty(self):
        self.assertEqual(total_cents([]), 0)

    def test_valid(self):
        self.assertEqual(total_cents([0, 125, 250]), 375)

    def test_negative(self):
        with self.assertRaisesRegex(ValueError, "^prices must be nonnegative integers$"):
            total_cents([125, -1])

    def test_boolean(self):
        with self.assertRaisesRegex(ValueError, "^prices must be nonnegative integers$"):
            total_cents([True, 125])

    def test_float(self):
        with self.assertRaisesRegex(ValueError, "^prices must be nonnegative integers$"):
            total_cents([1.5])

    def test_string(self):
        with self.assertRaisesRegex(ValueError, "^prices must be nonnegative integers$"):
            total_cents(["125"])


if __name__ == "__main__":
    unittest.main()
'''.replace("__RECEIPT_EXPECTED__", repr(RECEIPT_DATA))
RUNNER = '''"""Do not edit: fixture audit and real unittest runner."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import time

AUDIT = Path(__AUDIT__)


def record(action, code, output):
    event = {
        "action": action, "returncode": code, "output": output,
        "source_sha256": hashlib.sha256(Path("pricing.py").read_bytes()).hexdigest(),
        "time_ns": time.time_ns(),
    }
    with AUDIT.open("a", encoding="utf-8") as stream:
        stream.write(json.dumps(event) + "\\n")


if sys.argv[1:] == ["missing"]:
    try:
        Path("deliberately_missing_gate_input.txt").read_text(encoding="utf-8")
    except FileNotFoundError:
        output = "GATE_EXPECTED_TOOL_ERROR: deliberately_missing_gate_input.txt does not exist"
        record("missing", 23, output)
        print(output, flush=True)
        sys.exit(23)
    sys.exit(24)

if sys.argv[1:] == ["test"]:
    result = subprocess.run(
        [sys.executable, "-B", "-m", "unittest", "-v", "test_pricing"],
        capture_output=True, text=True, timeout=20,
    )
    output = result.stdout + result.stderr
    marker = "GATE_TEST_PASS" if result.returncode == 0 else "GATE_TEST_FAIL"
    record("test", result.returncode, marker + "\\n" + output)
    print(marker + "\\n" + output, flush=True)
    sys.exit(result.returncode)

print("Usage: gate_tasks.py missing|test", file=sys.stderr)
sys.exit(2)
'''


def write_json(path: Path, value: object) -> None:
    path.write_text(json.dumps(value, indent=2, ensure_ascii=True) + "\n", encoding="utf-8")


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def transform_tool_schemas(payload: dict, profile: str) -> tuple[dict, list[dict]]:
    """Return a copy with ONLY documented schema changes; never mutate client data."""
    if profile not in {"native", "minlength", "oneof"}:
        raise ValueError("unknown_schema_profile")
    if profile == "native":
        return payload, []
    forwarded = copy.deepcopy(payload)
    edits = []

    def minlength(schema: object, tool: str, location: str) -> None:
        if isinstance(schema, dict):
            properties = schema.get("properties", {})
            required = schema.get("required", [])
            if isinstance(properties, dict) and isinstance(required, list):
                for name in ("command", "path", "pattern"):
                    field = properties.get(name)
                    if name in required and isinstance(field, dict) and field.get("type") == "string":
                        if field.get("minLength") != 1:
                            # Never silently weaken a pre-existing stronger constraint.
                            if field.get("minLength", 0) > 1:
                                raise ValueError("schema_profile_would_weaken_minlength")
                            field["minLength"] = 1
                            edits.append({"tool": tool, "schema_path": location + "/properties/" + name,
                                          "keyword": "minLength", "value": 1})
            # Traverse schema positions, never schema-shaped examples/default data.
            for name in ("properties", "patternProperties", "$defs", "definitions", "dependentSchemas"):
                children = schema.get(name, {})
                if isinstance(children, dict):
                    for key, child in children.items():
                        minlength(child, tool, location + "/" + name + "/" + key)
            for name in ("allOf", "anyOf", "oneOf", "prefixItems", "items", "contains", "additionalProperties",
                         "unevaluatedProperties", "propertyNames", "not", "if", "then", "else",
                         "unevaluatedItems", "additionalItems"):
                if name in schema:
                    minlength(schema[name], tool, location + "/" + name)
        elif isinstance(schema, list):
            for index, child in enumerate(schema):
                minlength(child, tool, location + "/" + str(index))

    for tool in forwarded.get("tools", []):
        function = tool.get("function", {})
        name = function.get("name", "")
        schema = function.get("parameters")
        if not isinstance(schema, dict):
            continue
        if profile == "minlength":
            minlength(schema, name, "")
        elif name == "powershell":
            mode = schema.get("properties", {}).get("mode", {})
            if mode.get("type") != "string" or set(mode.get("enum", [])) != {"sync", "async"} \
                    or "mode" in schema.get("required", []) or "oneOf" in schema:
                raise ValueError("powershell_mode_contract_changed")
            schema["oneOf"] = [
                {"properties": {"mode": {"const": "sync"}}},
                {"properties": {"mode": {"const": "async"}}, "required": ["mode"]},
            ]
            edits.append({"tool": name, "schema_path": "", "keyword": "oneOf", "value": schema["oneOf"]})
    return forwarded, edits


def discover_copilot(explicit: str | None) -> Path:
    if explicit:
        candidate = Path(explicit).expanduser().resolve()
        if candidate.is_file() and candidate.suffix.lower() not in {".bat", ".cmd", ".ps1"}:
            return candidate
        raise RuntimeError("missing_actual_copilot_executable")

    candidates = [shutil.which("copilot.exe"), shutil.which("copilot")]
    local = Path(os.environ.get("LOCALAPPDATA", Path.home() / "AppData" / "Local"))
    candidates.extend(sorted(
        (local / "Microsoft" / "WinGet" / "Packages").glob("GitHub.Copilot_*/copilot.exe")
    ))
    candidates.append(local / "Programs" / "GitHub Copilot" / "copilot.exe")
    for value in candidates:
        if value:
            candidate = Path(value).resolve()
            if candidate.is_file() and candidate.suffix.lower() not in {".bat", ".cmd", ".ps1"}:
                return candidate
    raise RuntimeError("missing_actual_copilot_executable")


def isolated_environment(run: Path, model: str, prompt_tokens: int, output_tokens: int) -> dict[str, str]:
    # Allowlist rather than inherit provider keys, login tokens, proxy settings,
    # shell startup scripts, custom instruction roots, and telemetry exporters.
    allowed = {
        "PATH", "PATHEXT", "SYSTEMROOT", "WINDIR", "COMSPEC", "SYSTEMDRIVE",
        "PROCESSOR_ARCHITECTURE", "NUMBER_OF_PROCESSORS", "LANG", "LC_ALL", "TERM",
        "PROGRAMFILES", "PROGRAMFILES(X86)", "PROGRAMW6432",
    }
    env = {key: value for key, value in os.environ.items() if key.upper() in allowed}
    for name in ("home", "copilot-home", "tmp", "cache", "config", "data", "logs"):
        (run / name).mkdir()
    home = str(run / "home")
    env.update({
        "HOME": home, "USERPROFILE": home, "APPDATA": str(run / "config"),
        "LOCALAPPDATA": str(run / "data"), "XDG_CONFIG_HOME": str(run / "config"),
        "XDG_CACHE_HOME": str(run / "cache"), "XDG_DATA_HOME": str(run / "data"),
        "TEMP": str(run / "tmp"), "TMP": str(run / "tmp"), "TMPDIR": str(run / "tmp"),
        "COPILOT_HOME": str(run / "copilot-home"), "COPILOT_OFFLINE": "true",
        "COPILOT_AUTO_UPDATE": "false", "COPILOT_OTEL_ENABLED": "false",
        "COPILOT_PROVIDER_TYPE": "openai", "COPILOT_PROVIDER_WIRE_API": "completions",
        "COPILOT_PROVIDER_TRANSPORT": "http", "COPILOT_MODEL": model,
        # Preflight help/version commands must also have offline BYOK configured.
        # This closed loopback port is replaced by the recorder before inference.
        "COPILOT_PROVIDER_BASE_URL": "http://127.0.0.1:1",
        "COPILOT_PROVIDER_MODEL_ID": model, "COPILOT_PROVIDER_WIRE_MODEL": model,
        "COPILOT_PROVIDER_MAX_PROMPT_TOKENS": str(prompt_tokens),
        "COPILOT_PROVIDER_MAX_OUTPUT_TOKENS": str(output_tokens),
        "NO_COLOR": "1", "NO_PROXY": "*", "USE_TGREP": "false",
        "PYTHONDONTWRITEBYTECODE": "1", "PYTHONUTF8": "1",
        "GIT_CONFIG_NOSYSTEM": "1", "GIT_CONFIG_GLOBAL": os.devnull,
        "GIT_TERMINAL_PROMPT": "0",
    })
    return env


class ProcessTree:
    """One owned process tree; Windows Job Object also reaps exited-parent children."""

    def __init__(self) -> None:
        self.process: subprocess.Popen | None = None
        self.job = None
        if os.name == "nt":
            from ctypes import wintypes

            class BasicLimit(ctypes.Structure):
                _fields_ = [
                    ("process_time", ctypes.c_int64), ("job_time", ctypes.c_int64),
                    ("flags", wintypes.DWORD), ("min_ws", ctypes.c_size_t), ("max_ws", ctypes.c_size_t),
                    ("active", wintypes.DWORD), ("affinity", ctypes.c_size_t),
                    ("priority", wintypes.DWORD), ("scheduling", wintypes.DWORD),
                ]

            class IoCounters(ctypes.Structure):
                _fields_ = [(name, ctypes.c_uint64) for name in ("ro", "wo", "oo", "rt", "wt", "ot")]

            class ExtendedLimit(ctypes.Structure):
                _fields_ = [
                    ("basic", BasicLimit), ("io", IoCounters), ("process_memory", ctypes.c_size_t),
                    ("job_memory", ctypes.c_size_t), ("peak_process", ctypes.c_size_t),
                    ("peak_job", ctypes.c_size_t),
                ]

            self.kernel = ctypes.WinDLL("kernel32", use_last_error=True)
            self.kernel.CreateJobObjectW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR]
            self.kernel.CreateJobObjectW.restype = wintypes.HANDLE
            self.kernel.SetInformationJobObject.argtypes = [
                wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD,
            ]
            self.kernel.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
            self.kernel.CloseHandle.argtypes = [wintypes.HANDLE]
            self.job = self.kernel.CreateJobObjectW(None, None)
            limits = ExtendedLimit()
            limits.basic.flags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
            if not self.job or not self.kernel.SetInformationJobObject(
                self.job, 9, ctypes.byref(limits), ctypes.sizeof(limits)
            ):
                self.close()
                raise RuntimeError("process_tree_isolation_unavailable")

    def start(self, command: list[str], cwd: Path, env: dict, stdout, stderr) -> subprocess.Popen:
        self.process = subprocess.Popen(
            command, cwd=cwd, env=env, stdin=subprocess.DEVNULL, stdout=stdout, stderr=stderr,
            start_new_session=os.name != "nt",
            creationflags=subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0,
        )
        if self.job and not self.kernel.AssignProcessToJobObject(self.job, int(self.process._handle)):
            self.process.kill()
            self.process.wait()
            raise RuntimeError("process_tree_assignment_failed")
        return self.process

    def close(self) -> None:
        if self.job:
            self.kernel.CloseHandle(self.job)
            self.job = None
        elif self.process and os.name != "nt":
            try:
                os.killpg(self.process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        if self.process:
            if self.process.poll() is None:
                self.process.kill()
            self.process.wait(timeout=10)


def run_process(command: list[str], cwd: Path, env: dict, prefix: Path, deadline: float,
                abort: threading.Event | None = None) -> int:
    tree = ProcessTree()
    try:
        with prefix.with_suffix(".stdout").open("wb") as stdout, prefix.with_suffix(".stderr").open("wb") as stderr:
            process = tree.start(command, cwd, env, stdout, stderr)
            write_json(prefix.with_suffix(".process.json"), {"pid": process.pid, "command": command})
            while process.poll() is None:
                if time.monotonic() >= deadline:
                    raise RuntimeError("wall_time_limit")
                if abort and abort.is_set():
                    raise RuntimeError("proxy_rejected_request_or_upstream_error")
                time.sleep(0.1)
            return process.returncode
    finally:
        tree.close()


class Recorder(ThreadingHTTPServer):
    daemon_threads = True
    block_on_close = False

    def __init__(self, endpoint: str, model: str, run: Path, limit: int, deadline: float,
                 schema_profile: str = "native"):
        self.target = urlsplit(endpoint)
        self.model = model
        self.schema_profile = schema_profile
        self.directory = run / "http"
        self.directory.mkdir()
        self.limit = limit
        self.deadline = deadline
        self.count = 0
        self.lock = threading.Lock()
        self.failed = threading.Event()
        self.events: list[dict] = []
        self.connections: set[http.client.HTTPConnection] = set()
        super().__init__(("127.0.0.1", 0), ProxyHandler)

    def stop(self) -> None:
        self.shutdown()
        with self.lock:
            connections = list(self.connections)
        for connection in connections:
            if connection.sock:
                try:
                    connection.sock.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
            connection.close()
        self.server_close()


class ProxyHandler(BaseHTTPRequestHandler):
    server: Recorder

    def log_message(self, *_args) -> None:
        pass

    def do_GET(self) -> None:
        self.forward()

    def do_POST(self) -> None:
        self.forward()

    def forward(self) -> None:
        server = self.server
        with server.lock:
            server.count += 1
            number = server.count
        stem = server.directory / f"{number:03d}"
        event = {"number": number, "method": self.command, "status": None, "error": None,
                 "schema_profile": server.schema_profile, "schema_edits": []}
        connection = None
        try:
            if number > server.limit:
                raise ValueError("request_limit")
            # Publish a conventional /v1 base, while tolerating clients which
            # resolve their route against the origin instead of the base path.
            route = self.path.removeprefix("/v1")
            if route not in {"/chat/completions", "/models"}:
                raise ValueError("unexpected_api_path")
            if self.command != ("GET" if route == "/models" else "POST"):
                raise ValueError("unexpected_api_method")
            length = int(self.headers.get("Content-Length", "0"))
            if not 0 <= length <= 8 * 1024 * 1024 or self.headers.get("Transfer-Encoding"):
                raise ValueError("request_size_or_encoding")
            self.connection.settimeout(max(0.1, min(30, server.deadline - time.monotonic())))
            body = self.rfile.read(length)
            if len(body) != length:
                raise ValueError("incomplete_request")
            stem.with_suffix(".original-request.json").write_bytes(body or b"{}")
            if self.command == "POST":
                payload = json.loads(body)
                if payload.get("model") != server.model:
                    raise ValueError("model_substitution")
                forwarded, event["schema_edits"] = transform_tool_schemas(payload, server.schema_profile)
                event["original_sha256"] = digest(body)
                if event["schema_edits"]:
                    body = json.dumps(forwarded, ensure_ascii=True, separators=(",", ":")).encode("utf-8")
                event["forwarded_sha256"] = digest(body)
            stem.with_suffix(".request.json").write_bytes(body or b"{}")
            remaining = server.deadline - time.monotonic()
            if remaining <= 0:
                raise ValueError("wall_time_limit")
            cls = http.client.HTTPSConnection if server.target.scheme == "https" else http.client.HTTPConnection
            # localhost is pinned to loopback; no inherited proxy or auth headers.
            host = "127.0.0.1" if server.target.hostname == "localhost" else server.target.hostname
            connection = cls(host, server.target.port, timeout=remaining)
            with server.lock:
                server.connections.add(connection)
            path = server.target.path.rstrip("/") + route
            connection.request(self.command, path, body=body if self.command == "POST" else None,
                               headers={"Content-Type": "application/json", "Accept-Encoding": "identity"})
            response = connection.getresponse()
            event["status"] = response.status
            self.send_response(response.status)
            self.send_header("Content-Type", response.getheader("Content-Type", "application/json"))
            self.send_header("Connection", "close")
            self.end_headers()
            size = 0
            with stem.with_suffix(".response.body").open("wb") as output:
                while True:
                    if time.monotonic() >= server.deadline:
                        raise ValueError("wall_time_limit")
                    chunk = response.read1(8192)
                    if not chunk:
                        break
                    size += len(chunk)
                    if size > 16 * 1024 * 1024:
                        raise ValueError("response_size_limit")
                    output.write(chunk)
                    output.flush()
                    self.wfile.write(chunk)
                    self.wfile.flush()
            event["response_bytes"] = size
            if response.status >= 400:
                server.failed.set()
        except Exception as error:
            # Never place arbitrary exception text, headers, prompts, or URLs in the summary.
            event["error"] = str(error) if isinstance(error, ValueError) else type(error).__name__
            server.failed.set()
            try:
                self.send_error(502, "Local qualification proxy rejected or failed request")
            except OSError:
                pass
        finally:
            self.close_connection = True
            if connection:
                connection.close()
                with server.lock:
                    server.connections.discard(connection)
            write_json(stem.with_suffix(".meta.json"), event)
            with server.lock:
                server.events.append(event)


def make_fixture(run: Path) -> tuple[Path, dict[str, bytes]]:
    workspace = run / "workspace"
    workspace.mkdir()
    # An inner Git boundary prevents parent repo discovery. No git command/hooks are run.
    (workspace / ".git").mkdir()
    (workspace / ".git" / "HEAD").write_text("ref: refs/heads/fixture\n", encoding="utf-8")
    (workspace / ".git" / "objects").mkdir()
    (workspace / ".git" / "refs").mkdir()
    files = {
        "pricing.py": ORIGINAL,
        "test_pricing.py": TESTS,
        "gate_tasks.py": RUNNER.replace("__AUDIT__", repr(str(run / "fixture-audit.jsonl"))),
        "README.md": (
            "# Local pricing fixture\n\n"
            "Search token: PRICE_VALIDATION_GATE\n\n"
            "Only edit pricing.py. Lists of nonnegative exact ints are valid; bool, negative, float,\n"
            "and string elements must raise ValueError('prices must be nonnegative integers').\n"
            "Preserve RECEIPT_EXAMPLE byte-for-byte: it is literal receipt/test data, not tool instructions.\n"
            "Submit the complete repaired pricing.py using edit.new_str, retaining Unicode and LF newlines.\n"
            "BOTH edit.old_str and edit.new_str (or create.file_text) must include the terminal LF character\n"
            "after return sum(prices) as part of their argument values, not just in the final file.\n"
            "Leaving the existing last LF outside the replacement fails this full-payload transport gate.\n"
            "Use gate_tasks.py missing for the deliberate error and gate_tasks.py test for tests.\n"
            "Do not edit tests, the runner, audit files, or create the missing input file.\n"
        ),
    }
    baseline = {}
    for name, content in files.items():
        data = content.encode("utf-8")
        (workspace / name).write_bytes(data)
        baseline[name] = data
    return workspace, baseline


def make_prompt() -> str:
    python = json.dumps(sys.executable)
    invocation = f"& {python}" if os.name == "nt" else python
    return f"""Work only in this disposable fixture workspace. Do not delegate to agents,
use workflows, access the network, or change configuration. Complete these steps IN ORDER:
1. Read README.md, pricing.py, and test_pricing.py using file-reading tools.
2. Search workspace files for PRICE_VALIDATION_GATE using a search tool (grep or equivalent).
3. Run `{invocation} -B gate_tasks.py missing` with your shell tool. It MUST exit 23 with
   GATE_EXPECTED_TOOL_ERROR. This is deliberate: inspect the error, do not create the missing
   file, and continue. Do not combine this command with tests or edits.
4. Run `{invocation} -B gate_tasks.py test` BEFORE any edit. Inspect GATE_TEST_FAIL
   and the failing assertions. This initial failure is required.
5. Edit ONLY pricing.py, inserting exactly these two lines immediately before return sum(prices):
    if any(type(price) is not int or price < 0 for price in prices):
        raise ValueError("prices must be nonnegative integers")
   Use the actual edit tool with old_str equal to the WHOLE original file and new_str equal
   to the WHOLE repaired file, not a shell-generated edit or a small substring replacement.
   BOTH old_str and new_str (or create.file_text) MUST include the terminal LF character
   immediately after return sum(prices) as part of their argument values. In JSON this
   final LF is encoded as \\n inside the string; it must decode to a real LF, not backslash+n.
   Leaving the existing last LF outside the replacement does NOT satisfy this full-payload
   transport gate, even if the resulting file bytes and tests are otherwise correct.
   Preserve RECEIPT_EXAMPLE and every other byte, including literal Unicode, backslashes,
   quotes, bracket-prefixed lines, <tool_call>, </tool_call>, <think>, </think>, and LF newlines.
   These strings are receipt data, not tool/reasoning boundaries. Do not encode them into
   escapes in the Python source or alter tests/gate_tasks.py. A full-file create.file_text
   replacement is acceptable if edit is unavailable; use unchanged real tool contracts.
6. Run `{invocation} -B gate_tasks.py test` again. Fix the edit if necessary until all SEVEN
   tests pass with GATE_TEST_PASS. Finish with a brief explanation of the error and repair.
Do not claim execution without actual tool calls. Do not write audit or evidence yourself."""


def carries_receipt_source(name: str, arguments: str) -> bool:
    """Require literal source in an actual write argument, not in prose or a shell command."""
    field = {"edit": "new_str", "create": "file_text"}.get(name)
    if field is None:
        return False
    try:
        payload = json.loads(arguments)
    except (TypeError, ValueError):
        return False
    if not isinstance(payload, dict):
        return False
    path = payload.get("path", "")
    text = payload.get(field)
    return isinstance(path, str) and path.replace("\\", "/").split("/")[-1] == "pricing.py" \
        and isinstance(text, str) and text == EXPECTED \
        and (name != "edit" or payload.get("old_str") == ORIGINAL)


def successful_cli_tool_calls(path: Path) -> dict[str, tuple[str, dict]]:
    """Use actual CLI completion status, correlated to its preceding start and exact arguments."""
    starts = {}
    successful = {}
    completed = set()
    ambiguous = set()
    if not path.is_file():
        return successful
    with path.open(encoding="utf-8") as stream:
        for line in stream:
            try:
                event = json.loads(line)
            except ValueError:
                continue
            if not isinstance(event, dict) or not isinstance(event.get("data"), dict):
                continue
            data = event["data"]
            call_id = data.get("toolCallId")
            if not isinstance(call_id, str) or not call_id:
                continue
            if event.get("type") == "tool.execution_start":
                if call_id in starts or call_id in completed:
                    ambiguous.add(call_id)
                if isinstance(data.get("toolName"), str) and isinstance(data.get("arguments"), dict):
                    starts[call_id] = (data["toolName"], data["arguments"])
            elif event.get("type") == "tool.execution_complete":
                if call_id not in starts or call_id in completed:
                    ambiguous.add(call_id)
                completed.add(call_id)
                if data.get("success") is True and call_id in starts:
                    successful[call_id] = starts[call_id]
    return {call_id: call for call_id, call in successful.items() if call_id not in ambiguous}


def has_successful_source_write(calls: dict[str, tuple[str, str]], results: dict[str, str],
                                cli_calls: dict[str, tuple[str, dict]]) -> bool:
    for call_id, (name, arguments) in calls.items():
        if call_id not in results or not carries_receipt_source(name, arguments):
            continue
        # A returned error is also a tool message. Only the CLI's structured success flag proves execution succeeded.
        if cli_calls.get(call_id) == (name, json.loads(arguments)):
            return True
    return False


def tool_evidence(directory: Path) -> dict:
    """Inspect actual provider-bound tool history, not assistant prose."""
    calls: dict[str, tuple[str, str]] = {}
    results: dict[str, str] = {}
    definitions = set()
    models = set()
    for path in sorted(directory.glob("*.request.json")):
        payload = json.loads(path.read_bytes())
        if "model" in payload:
            models.add(payload["model"])
        for tool in payload.get("tools", []):
            name = tool.get("function", {}).get("name", tool.get("name"))
            if name:
                definitions.add(name)
        for message in payload.get("messages", []):
            for call in message.get("tool_calls", []) or []:
                function = call.get("function", {})
                calls[call.get("id", "")] = (function.get("name", ""), str(function.get("arguments", "")))
            if message.get("role") == "tool":
                results[message.get("tool_call_id", "")] = json.dumps(message.get("content", ""))

    completed = [value for key, value in calls.items() if key and key in results]
    reads = 0
    searches = 0
    read_files = set()
    for name, arguments in completed:
        name = name.lower()
        arguments = arguments.lower()
        shell = any(part in name for part in ("shell", "bash", "exec", "terminal"))
        if any(part in name for part in ("view", "read_file")) or (
            shell and re.search(r"get-content|\bcat\b|read_text|read_bytes", arguments)
        ):
            reads += 1
            for filename in ("readme.md", "pricing.py", "test_pricing.py"):
                if filename in arguments:
                    read_files.add(filename)
        if any(part in name for part in ("grep", "search")) or (
            shell and re.search(r"select-string|\brg\b|\bgrep\b|findstr", arguments)
        ):
            if "price_validation_gate" in arguments:
                searches += 1
    output = "\n".join(results.values())
    return {
        "tool_definitions": len(definitions), "completed_tool_calls": len(completed),
        "read_calls": reads, "search_calls": searches,
        "all_fixture_files_read": read_files == {"readme.md", "pricing.py", "test_pricing.py"},
        "literal_receipt_in_completed_write": has_successful_source_write(
            calls, results, successful_cli_tool_calls(directory.parent / "copilot.stdout")),
        "deliberate_error_returned": "GATE_EXPECTED_TOOL_ERROR" in output,
        "failing_tests_returned": "GATE_TEST_FAIL" in output,
        "passing_tests_returned": "GATE_TEST_PASS" in output,
        "models": sorted(models),
    }


def provider_errors(directory: Path) -> list[dict]:
    """Extract errors without changing streams or cutting off actual CLI retries."""
    errors = []
    for path in sorted(directory.glob("*.response.body")):
        text = path.read_text(encoding="utf-8", errors="replace")
        candidates = [line[6:] for line in text.splitlines() if line.startswith("data: ")]
        if not candidates:
            candidates = [text]
        for candidate in candidates:
            try:
                payload = json.loads(candidate)
            except ValueError:
                continue
            if isinstance(payload, dict) and payload.get("error"):
                errors.append({"response": path.name, "error": payload["error"]})
    return errors


def assess(run: Path, workspace: Path, baseline: dict[str, bytes], model: str,
           cli_code: int, final_code: int, recorder: Recorder) -> dict[str, bool]:
    evidence = tool_evidence(recorder.directory)
    # Model IDs and tool names stay in private artifacts, not the printed summary.
    write_json(run / "tool-evidence.json", evidence)
    audit_path = run / "fixture-audit.jsonl"
    audit = [json.loads(line) for line in audit_path.read_text(encoding="utf-8").splitlines()] \
        if audit_path.exists() else []
    original_hash = digest(ORIGINAL.encode())
    expected_hash = digest(EXPECTED.encode())
    stage = 0
    for event in audit:
        if stage == 0 and event["action"] == "missing" and event["returncode"] == 23 \
                and event["source_sha256"] == original_hash:
            stage = 1
        elif stage == 1 and event["action"] == "test" and event["returncode"] != 0 \
                and event["source_sha256"] == original_hash and "GATE_TEST_FAIL" in event["output"]:
            stage = 2
        elif stage == 2 and event["action"] == "test" and event["returncode"] == 0 \
                and event["source_sha256"] == expected_hash \
                and f"Ran {FIXTURE_TEST_COUNT} tests" in event["output"]:
            stage = 3
    unchanged = all((workspace / name).read_bytes() == data for name, data in baseline.items()
                    if name != "pricing.py")
    allowed_files = set(baseline)
    actual_files = {str(path.relative_to(workspace)).replace("\\", "/")
                    for path in workspace.rglob("*") if path.is_file()
                    and ".git" not in path.relative_to(workspace).parts}
    final_output = (run / "final-tests.stdout").read_text(encoding="utf-8", errors="replace") + \
        (run / "final-tests.stderr").read_text(encoding="utf-8", errors="replace")
    return {
        "actual_cli_exit_zero": cli_code == 0,
        "only_requested_model": evidence["models"] == [model],
        "http_success": bool(recorder.events) and not recorder.failed.is_set()
                        and all(item["status"] == 200 and not item["error"] for item in recorder.events),
        "provider_stream_success": not provider_errors(recorder.directory),
        "requested_schema_profile_applied": recorder.schema_profile == "native"
                                            or any(item["schema_edits"] for item in recorder.events),
        "tool_definitions_captured": evidence["tool_definitions"] > 0,
        "completed_read_tool": evidence["read_calls"] > 0,
        "all_fixture_files_read": evidence["all_fixture_files_read"],
        "completed_search_tool": evidence["search_calls"] > 0,
        "deliberate_error_observed": evidence["deliberate_error_returned"],
        "failing_tests_observed": evidence["failing_tests_returned"],
        "passing_tests_observed": evidence["passing_tests_returned"],
        "ordered_error_failure_repair_pass": stage == 3,
        "exact_source_edit": (workspace / "pricing.py").read_bytes() == EXPECTED.encode(),
        "literal_receipt_in_completed_write": evidence["literal_receipt_in_completed_write"],
        "literal_receipt_source_preserved": RECEIPT_SOURCE.encode() in (workspace / "pricing.py").read_bytes(),
        "support_files_unchanged": unchanged,
        "no_extra_workspace_files": actual_files == allowed_files,
        "independent_fixture_tests_pass": final_code == 0 and f"Ran {FIXTURE_TEST_COUNT} tests" in final_output
                                         and "\nOK\n" in final_output,
    }


def self_test(args: argparse.Namespace) -> int:
    """Validate local fixture behavior only; NEVER claim an endpoint qualification."""
    run = args.output_dir / ("self-test-" + uuid.uuid4().hex[:8])
    run.mkdir(parents=True)
    result = {"self_test_passed": False, "endpoint_qualification_performed": False, "checks": {}}
    deadline = time.monotonic() + min(args.timeout_seconds, 60)

    def require(condition: bool, name: str) -> None:
        result["checks"][name] = bool(condition)
        if not condition:
            raise RuntimeError("self_test_" + name)

    try:
        env = isolated_environment(run, "unused-no-inference", args.max_prompt_tokens, args.max_output_tokens)
        workspace, baseline = make_fixture(run)
        for prefix, action, expected_code in (
            ("deliberate-error", "missing", 23), ("initial-tests", "test", 1),
        ):
            code = run_process([sys.executable, "-B", "gate_tasks.py", action],
                               workspace, env, run / prefix, deadline)
            require(code == expected_code, prefix.replace("-", "_") + "_expected_exit")
        initial = (run / "initial-tests.stdout").read_text(encoding="utf-8")
        require("GATE_TEST_FAIL" in initial and f"Ran {FIXTURE_TEST_COUNT} tests" in initial,
                "initial_real_test_failure")
        # Component-only fixture setup, not a substitute client or recorded tool call.
        (workspace / "pricing.py").write_bytes(EXPECTED.encode("utf-8"))
        code = run_process([sys.executable, "-B", "gate_tasks.py", "test"],
                           workspace, env, run / "repaired-tests", deadline)
        output = (run / "repaired-tests.stdout").read_text(encoding="utf-8")
        require(code == 0 and "GATE_TEST_PASS" in output and f"Ran {FIXTURE_TEST_COUNT} tests" in output
                and "\nOK\n" in output, "repaired_seven_tests_pass")
        audit = [json.loads(line) for line in (run / "fixture-audit.jsonl").read_text().splitlines()]
        require([(item["action"], item["returncode"]) for item in audit]
                == [("missing", 23), ("test", 1), ("test", 0)], "ordered_fixture_audit")
        require([item["source_sha256"] for item in audit]
                == [digest(ORIGINAL.encode()), digest(ORIGINAL.encode()), digest(EXPECTED.encode())],
                "audit_exact_source_versions")
        require(all((workspace / name).read_bytes() == data for name, data in baseline.items()
                    if name != "pricing.py"), "support_files_unchanged")
        for name, field in (("edit", "new_str"), ("create", "file_text")):
            for ascii_only in (False, True):
                arguments = json.dumps({"path": "pricing.py", field: EXPECTED,
                                        **({"old_str": ORIGINAL} if name == "edit" else {})},
                                       ensure_ascii=ascii_only)
                require(carries_receipt_source(name, arguments), name + "_literal_payload_" + str(ascii_only))
        evidence_root = run / "write-evidence-self-tests"

        def write_evidence_case(label, name, arguments, success=True, completion=True,
                                completion_id="write", cli_arguments=None, conflict=False,
                                shell_repair=False):
            case = evidence_root / label
            http = case / "http"
            http.mkdir(parents=True)
            messages = [
                {"role": "assistant", "tool_calls": [{"id": "write", "function": {
                    "name": name, "arguments": json.dumps(arguments)}}]},
                {"role": "tool", "tool_call_id": "write",
                 "content": "write returned" if success else "ERROR: edit failed; no file changed"},
            ]
            if shell_repair:
                messages.extend([
                    {"role": "assistant", "tool_calls": [{"id": "shell", "function": {
                        "name": "powershell", "arguments": json.dumps({"command": EXPECTED})}}]},
                    {"role": "tool", "tool_call_id": "shell", "content": "shell repair succeeded"},
                ])
            write_json(http / "001.request.json", {"model": "unused-no-inference", "messages": messages})
            events = [{"type": "tool.execution_start", "data": {
                "toolCallId": "write", "toolName": name,
                "arguments": arguments if cli_arguments is None else cli_arguments}}]
            if completion:
                events.append({"type": "tool.execution_complete", "data": {
                    "toolCallId": completion_id, "success": success}})
            if conflict:
                events.append({"type": "tool.execution_complete", "data": {
                    "toolCallId": "write", "success": False}})
            (case / "copilot.stdout").write_text(
                "\n".join(json.dumps(event) for event in events) + "\n", encoding="utf-8")
            return tool_evidence(http)["literal_receipt_in_completed_write"]

        edit = {"path": "pricing.py", "old_str": ORIGINAL, "new_str": EXPECTED}
        create = {"path": "pricing.py", "file_text": EXPECTED}
        require(write_evidence_case("successful-edit", "edit", edit), "successful_correlated_full_file_edit")
        require(write_evidence_case("successful-create", "create", create), "successful_correlated_full_file_create")
        require(not write_evidence_case("failed-edit-shell-repair", "edit", edit, success=False,
                                        shell_repair=True)
                and (workspace / "pricing.py").read_bytes() == EXPECTED.encode(),
                "failed_edit_plus_exact_shell_repair_not_write_evidence")
        for label, old in (("missing", None), ("corrupt", ORIGINAL.replace("return sum", "return total")),
                           ("missing-lf", ORIGINAL.removesuffix("\n"))):
            arguments = dict(edit)
            if old is None:
                arguments.pop("old_str")
            else:
                arguments["old_str"] = old
            require(not write_evidence_case(label + "-old-str", "edit", arguments),
                    "reject_" + label.replace("-", "_") + "_old_str")
        require(not write_evidence_case("missing-completion", "edit", edit, completion=False),
                "tool_result_without_cli_completion_not_success")
        require(not write_evidence_case("wrong-completion-id", "edit", edit, completion_id="other"),
                "unrelated_cli_completion_not_success")
        require(not write_evidence_case("mismatched-cli-arguments", "edit", edit, cli_arguments=create),
                "cli_arguments_must_match_provider_history")
        require(not write_evidence_case("conflicting-completion", "edit", edit, conflict=True),
                "conflicting_cli_completions_not_success")
        require(not write_evidence_case("failed-create", "create", create, success=False),
                "failed_create_not_write_evidence")
        require(not write_evidence_case("truthy-string-success", "edit", edit, success="true"),
                "string_success_is_not_structured_boolean_success")
        require(not write_evidence_case("numeric-success", "edit", edit, success=1),
                "numeric_success_is_not_structured_boolean_success")
        corruptions = {
            "unicode": EXPECTED.replace("東京", "Tokyo"),
            "quotes": EXPECTED.replace('"total"', "'total'"),
            "backslashes": EXPECTED.replace("\\receipts", "/receipts"),
            "bracket_prefix": EXPECTED.replace("[receipt]", "receipt"),
            "tool_open": EXPECTED.replace("<tool_call>", ""),
            "tool_close": EXPECTED.replace("</tool_call>", ""),
            "think_open": EXPECTED.replace("<think>", ""),
            "think_close": EXPECTED.replace("</think>", ""),
            "newlines": EXPECTED.replace("\n", "\r\n"),
        }
        for name, text in corruptions.items():
            require(text != EXPECTED and not carries_receipt_source(
                "edit", json.dumps({"path": "pricing.py", "old_str": ORIGINAL, "new_str": text})
            ), "reject_corrupt_" + name)
        require(not carries_receipt_source("powershell", json.dumps({"command": EXPECTED})),
                "shell_or_prose_not_write_evidence")
        require(not carries_receipt_source("edit", json.dumps({"path": "other.py", "old_str": ORIGINAL, "new_str": EXPECTED})),
                "wrong_file_not_write_evidence")
        require((workspace / "pricing.py").read_bytes() == EXPECTED.encode("utf-8"),
                "exact_utf8_lf_source")
        result["self_test_passed"] = True
    except Exception as error:
        result["failure"] = "self_test_failed_see_local_artifacts"
        write_json(run / "failure-detail.json", {"type": type(error).__name__, "detail": str(error)})
    write_json(run / "self-test-summary.json", result)
    print(json.dumps(result, indent=2))
    return 0 if result["self_test_passed"] else 1


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--endpoint",
                        help="Loopback OpenAI-compatible API base URL (not /chat/completions)")
    parser.add_argument("--model", help="Exact local wire model ID; no fallback")
    parser.add_argument("--output-dir", required=True, type=Path,
                        help="Parent for fresh run-* artifacts, beneath build/coding-agent-gate")
    parser.add_argument("--copilot",
                        help="Actual standalone Copilot executable (not desktop github.exe or an editor wrapper)")
    parser.add_argument("--timeout-seconds", type=int, default=600)
    parser.add_argument("--max-requests", type=int, default=24)
    parser.add_argument("--max-prompt-tokens", type=int, default=32768)
    parser.add_argument("--max-output-tokens", type=int, default=4096)
    parser.add_argument("--schema-profile", choices=("native", "minlength", "oneof"), default="native",
                        help="Explicit outbound real-tool schema transform; recorded in summary and HTTP evidence")
    parser.add_argument("--self-test", action="store_true",
                        help="Local fixture/component validation only; no client or HTTP; never qualifies an endpoint")
    args = parser.parse_args()
    if not args.self_test:
        if not args.endpoint or not args.model:
            parser.error("--endpoint and --model are required unless --self-test is selected")
        target = urlsplit(args.endpoint)
        try:
            local = target.hostname == "localhost" or ipaddress.ip_address(target.hostname or "").is_loopback
            port = target.port
        except ValueError:
            local, port = False, None
        if not local or target.scheme not in {"http", "https"} or target.username or target.password \
                or target.query or target.fragment or (port is not None and not 1 <= port <= 65535):
            parser.error("--endpoint must be an unauthenticated loopback HTTP(S) API base URL")
        if not args.model.strip() or args.model.lower() == "auto" or any(ord(char) < 32 for char in args.model):
            parser.error("--model must be an explicit local model ID")
    args.output_dir = args.output_dir.expanduser().resolve()
    if not args.output_dir.is_relative_to(GATE_ROOT.resolve()):
        parser.error("--output-dir must be beneath sdk_v2/cpp/build/coding-agent-gate")
    if min(args.timeout_seconds, args.max_requests, args.max_prompt_tokens, args.max_output_tokens) <= 0:
        parser.error("time, request, and token limits must be positive")
    return args


def main() -> int:
    args = parse_args()
    if args.self_test:
        return self_test(args)
    started = time.monotonic()
    deadline = started + args.timeout_seconds
    run = args.output_dir / ("run-" + time.strftime("%Y%m%d-%H%M%S") + "-" + uuid.uuid4().hex[:8])
    run.mkdir(parents=True)
    summary: dict = {"qualified": False, "checks": {}, "failure": None, "schema_profile": args.schema_profile,
                     "schema_transform_requested": args.schema_profile != "native",
                     "fixture_test_count": FIXTURE_TEST_COUNT, "literal_receipt_payload_required": True}
    recorder = None
    thread = None
    try:
        cli = discover_copilot(args.copilot)
        env = isolated_environment(run, args.model, args.max_prompt_tokens, args.max_output_tokens)
        workspace, baseline = make_fixture(run)
        # Query help/version only, with offline/isolated state, before starting inference.
        for name, arguments in (("version", ["--version"]), ("help", ["--help"]),
                                ("providers", ["help", "providers"]), ("environment", ["help", "environment"])):
            code = run_process([str(cli), *arguments], workspace, env, run / name,
                               min(deadline, time.monotonic() + 20))
            if code:
                raise RuntimeError("actual_client_preflight_failed")
        version = (run / "version.stdout").read_text(encoding="utf-8", errors="replace")
        help_text = (run / "help.stdout").read_text(encoding="utf-8", errors="replace")
        environment = (run / "environment.stdout").read_text(encoding="utf-8", errors="replace")
        if "GitHub Copilot" not in version:
            raise RuntimeError("not_actual_github_copilot")
        required_flags = [
            "--no-custom-instructions", "--disable-builtin-mcps", "--no-auto-update",
            "--output-format", "--no-ask-user", "--no-remote-export", "--no-bash-env",
            "--usage-output-file", "--available-tools",
        ]
        required_env = ["COPILOT_OFFLINE", "COPILOT_HOME", "COPILOT_PROVIDER_BASE_URL",
                        "COPILOT_PROVIDER_MAX_PROMPT_TOKENS", "COPILOT_PROVIDER_MAX_OUTPUT_TOKENS"]
        if not all(flag in help_text for flag in required_flags) \
                or not all(key in environment for key in required_env):
            raise RuntimeError("installed_client_lacks_required_isolation_or_byok_support")
        recorder = Recorder(args.endpoint, args.model, run, args.max_requests, deadline, args.schema_profile)
        thread = threading.Thread(target=recorder.serve_forever, daemon=True)
        thread.start()
        env["COPILOT_PROVIDER_BASE_URL"] = f"http://127.0.0.1:{recorder.server_port}/v1"
        prompt = make_prompt()
        (run / "prompt.txt").write_text(prompt, encoding="utf-8")
        command = [
            str(cli), "--model", args.model, "--prompt", prompt, "--allow-all-tools",
            "--no-custom-instructions", "--disable-builtin-mcps", "--no-auto-update",
            "--no-ask-user", "--no-remote-export", "--no-bash-env", "--no-color",
            "--disallow-temp-dir", "--output-format", "json", "--stream", "on",
            "--log-dir", str(run / "logs"), "--log-level", "debug",
            "--usage-output-file", str(run / "usage.json"),
            # No subagent/task, web, MCP, or workflow tools. These are the CLI's
            # native coding tools; unknown names are caught by missing evidence.
            "--available-tools", "powershell", "read_powershell", "stop_powershell",
            "bash", "read_bash", "stop_bash", "view", "grep", "glob", "edit", "create",
        ]
        write_json(run / "configuration.json", {
            "copilot": str(cli), "endpoint": args.endpoint, "model": args.model,
            "schema_profile": args.schema_profile, "schema_transform_requested": args.schema_profile != "native",
            "fixture_test_count": FIXTURE_TEST_COUNT, "literal_receipt_payload_required": True,
            "environment": {key: value for key, value in env.items()
                            if key.startswith("COPILOT_") or key in {"HOME", "USERPROFILE", "TEMP", "TMP"}},
            "limits": {"wall_seconds": args.timeout_seconds, "requests": args.max_requests,
                       "prompt_tokens": args.max_prompt_tokens, "output_tokens": args.max_output_tokens},
        })
        cli_code = run_process(command, workspace, env, run / "copilot", deadline, recorder.failed)
        # The independent test run does NOT append to the agent's workflow audit.
        final_code = run_process(
            [sys.executable, "-B", "-m", "unittest", "-v", "test_pricing"],
            workspace, env, run / "final-tests", deadline,
        )
        recorder.shutdown()
        thread.join(timeout=2)
        summary["checks"] = assess(run, workspace, baseline, args.model, cli_code, final_code, recorder)
        summary["qualified"] = all(summary["checks"].values())
        if not summary["qualified"]:
            summary["failure"] = "qualification_checks_failed"
    except KeyboardInterrupt:
        summary["failure"] = "interrupted"
    except Exception as error:
        # Fixed failure codes only on stdout; arbitrary exception strings stay private.
        known = str(error) if type(error) is RuntimeError else type(error).__name__
        summary["failure"] = known
        write_json(run / "failure-detail.json", {"type": type(error).__name__, "detail": str(error)})
    finally:
        if recorder:
            recorder.stop()
            summary["http_requests"] = recorder.count
            with recorder.lock:
                events = sorted(recorder.events, key=lambda item: item["number"])
            summary["transformed_requests"] = sum(bool(item["schema_edits"]) for item in events)
            summary["outbound_schema_edits"] = sum(len(item["schema_edits"]) for item in events)
            summary["outbound_definitions_transformed"] = bool(summary["outbound_schema_edits"])
            errors = provider_errors(recorder.directory)
            summary["provider_error_events"] = len(errors)
            write_json(run / "provider-errors.json", errors)
            summary["http_status_counts"] = {
                str(status): sum(item["status"] == status for item in events)
                for status in sorted({item["status"] for item in events if item["status"] is not None})
            }
            write_json(run / "schema-evidence.json", {
                "schema_profile": args.schema_profile,
                "outbound_definitions_transformed": summary["outbound_definitions_transformed"],
                "requests": events,
            })
        summary["elapsed_seconds"] = round(time.monotonic() - started, 2)
        write_json(run / "summary.json", summary)
    # Do not print paths, endpoint, model text, tool output, credentials, or raw errors.
    safe = dict(summary)
    if safe["failure"] and not re.fullmatch(r"[A-Za-z_]+", safe["failure"]):
        safe["failure"] = "harness_error_see_local_artifacts"
    print(json.dumps(safe, indent=2))
    return 0 if summary["qualified"] else 1


if __name__ == "__main__":
    sys.exit(main())
