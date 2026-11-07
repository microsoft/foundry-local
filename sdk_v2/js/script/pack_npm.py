"""CI-only: normalize host modes and validate the finished multi-platform npm tarball."""

import os
from pathlib import Path
import sys
import tarfile
import tempfile


NATIVE_LIBRARIES = {
    "win32-x64": "foundry_local.dll",
    "win32-arm64": "foundry_local.dll",
    "linux-x64": "libfoundry_local.so",
    "linux-arm64": "libfoundry_local.so",
    "darwin-arm64": "libfoundry_local.dylib",
}
REQUIRED_PAYLOADS = {
    f"package/prebuilds/{platform}/{filename}"
    for platform, library in NATIVE_LIBRARIES.items()
    for filename in (
        library,
        "foundry_local_node.node",
        "foundry-local-host.exe" if platform.startswith("win32-") else "foundry-local-host",
        "foundry-local-runtime.json",
    )
}
POSIX_HOSTS = {name for name in REQUIRED_PAYLOADS if name.endswith("/foundry-local-host")}


def validate_archive(path):
    with tarfile.open(path, "r:gz") as archive:
        for name in sorted(REQUIRED_PAYLOADS):
            try:
                entry = archive.getmember(name)
            except KeyError:
                raise ValueError(f"Required npm payload missing: {name}") from None
            if not entry.isfile():
                raise ValueError(f"Required npm payload is not a regular file: {name}")
            if name in POSIX_HOSTS and entry.mode & 0o111 != 0o111:
                raise ValueError(f"Required npm host is not executable: {name}")


def normalize_archive(path):
    path = Path(path)
    # Windows npm pack cannot preserve the POSIX modes from per-RID build artifacts.
    # Rewrite after npm's lifecycle hooks, changing only the three POSIX host modes.
    with tempfile.NamedTemporaryFile(dir=path.parent, suffix=".tgz", delete=False) as temporary:
        normalized = Path(temporary.name)
    try:
        with tarfile.open(path, "r:gz") as source, tarfile.open(normalized, "w:gz") as destination:
            for entry in source:
                if entry.name in POSIX_HOSTS:
                    entry.mode |= 0o111
                if entry.isfile():
                    with source.extractfile(entry) as payload:
                        destination.addfile(entry, payload)
                else:
                    destination.addfile(entry)
        validate_archive(normalized)
        os.replace(normalized, path)
    finally:
        normalized.unlink(missing_ok=True)


if __name__ == "__main__":
    normalize_archive(sys.argv[1])
    print(f"Validated npm payloads for all five prebuilds and three executable POSIX hosts: {sys.argv[1]}")
