# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.
"""Model-free ELF artifact and dynamic-loader regression tests."""

import ctypes.util
import os
from pathlib import Path
import socketserver
import subprocess
import sys
import threading
from types import SimpleNamespace
import unittest
from unittest.mock import Mock


def check_artifact(library, nm, readelf):
    symbols = subprocess.check_output(
        [nm, "--dynamic", "--defined-only", "--format=posix", library], text=True
    )
    exported = {line.split()[0] for line in symbols.splitlines() if line.strip()}
    expected = {"FoundryLocalGetApi", "FoundryLocalGetVersionString"}
    if exported != expected:
        raise AssertionError(
            f"Unexpected dynamic exports: {exported}; expected {expected}"
        )
    dynamic = subprocess.check_output([readelf, "--dynamic", library], text=True)
    if not any(
        "(FLAGS_1)" in line and "NODELETE" in line for line in dynamic.splitlines()
    ):
        raise AssertionError("Built library is missing DF_1_NODELETE")


class TlsProbe(socketserver.BaseRequestHandler):
    def handle(self):
        self.request.settimeout(10)
        data = bytearray()
        while len(data) < 2:
            chunk = self.request.recv(2 - len(data))
            if not chunk:
                break
            data.extend(chunk)
        if data.startswith(b"\x16\x03"):
            self.server.saw_client_hello.set()
        # Deliberately fail the TLS handshake after OpenSSL has initialized on
        # the native worker. No certificate, external service or model is needed.
        self.request.sendall(b"HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n")


class TlsProbeTest(unittest.TestCase):
    def check_prefix(self, chunks, expected):
        request = Mock()
        request.recv.side_effect = chunks
        server = SimpleNamespace(saw_client_hello=threading.Event())
        TlsProbe(request, ("127.0.0.1", 1), server)
        self.assertEqual(server.saw_client_hello.is_set(), expected)
        request.sendall.assert_called_once()
        return request

    def test_fragmented_header(self):
        request = self.check_prefix([b"\x16", b"\x03"], True)
        self.assertEqual([call.args[0] for call in request.recv.call_args_list], [2, 1])

    def test_early_eof(self):
        self.check_prefix([b"\x16", b""], False)

    def test_non_tls_header(self):
        self.check_prefix([b"HT"], False)


def check_runtime(library, probe, output_dir, order):
    host_curl = ctypes.util.find_library("curl")
    if not host_curl:
        raise RuntimeError(
            "System libcurl is required for the ELF loader regression test"
        )
    output = Path(output_dir).resolve()
    output.mkdir(parents=True, exist_ok=True)
    payload = output / "curl-payload.txt"
    payload.write_text("host curl remains independent\n", encoding="utf-8")
    with socketserver.ThreadingTCPServer(("127.0.0.1", 0), TlsProbe) as server:
        server.daemon_threads = True
        server.saw_client_hello = threading.Event()
        thread = threading.Thread(target=server.serve_forever)
        thread.start()
        try:
            env = os.environ.copy()
            # Never send telemetry or use a proxy for the loopback-only request.
            env.update(TF_BUILD="true", NO_PROXY="*", no_proxy="*")
            subprocess.run(
                [
                    probe,
                    library,
                    host_curl,
                    payload.as_uri(),
                    f"https://127.0.0.1:{server.server_address[1]}/models",
                    str(output),
                    order,
                ],
                env=env,
                check=True,
                timeout=100,
            )
            if not server.saw_client_hello.is_set():
                raise AssertionError(
                    "Native worker did not exercise bundled OpenSSL TLS"
                )
        finally:
            server.shutdown()
            thread.join()


if __name__ == "__main__":
    if sys.argv[1] == "artifact":
        check_artifact(*sys.argv[2:])
    elif sys.argv[1] == "runtime":
        check_runtime(*sys.argv[2:])
    elif sys.argv[1] == "probe":
        unittest.main(argv=[sys.argv[0]])
    else:
        raise ValueError(f"Unknown test mode: {sys.argv[1]}")
