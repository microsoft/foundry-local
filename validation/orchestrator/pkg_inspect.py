"""Package artifact inspection helpers for release validation.

The inspectors are intentionally dependency-free and only read local archive files. They do not contact package feeds.
"""
from __future__ import annotations

import csv
import email.parser
import hashlib
import io
import json
import os
import posixpath
import re
import tarfile
import zipfile
from typing import Any, Dict, Iterable, List
from xml.etree import ElementTree

_NATIVE_LIB_RE = re.compile(r"(^|/|\\)foundry_local\.(dll|so|dylib)$", re.IGNORECASE)
_WHEEL_NATIVE_RE = re.compile(r"\.(so|pyd|dylib)$", re.IGNORECASE)
_NPM_NATIVE_RE = re.compile(r"\.node$", re.IGNORECASE)
_INTERNAL_PATH_RE = re.compile(
    r"([A-Za-z]:\\\\Users\\\\|[A-Za-z]:\\Users\\|/Users/|/home/|/var/folders/|/tmp/|/var/tmp/)",
    re.IGNORECASE,
)
_DEBUG_FILE_RE = re.compile(r"(\.pdb$|\.snupkg(/|$)|\.dSYM(/|$)|(^|/)\.git(/|$)|(^|/)\.npmrc$)", re.IGNORECASE)
_TEXT_EXTENSIONS = {
    "", ".json", ".nuspec", ".xml", ".txt", ".md", ".py", ".js", ".ts", ".targets", ".props",
    ".config", ".cfg", ".toml", ".yaml", ".yml", ".metadata", ".record", ".wheel",
}


def sha256_file(path: str) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def inspect(path: str) -> Dict[str, Any]:
    lower = path.lower()
    if lower.endswith(".nupkg"):
        return inspect_nupkg(path)
    if lower.endswith(".whl"):
        return inspect_wheel(path)
    if lower.endswith(".tgz") or lower.endswith(".tar.gz"):
        return inspect_npm(path)
    raise ValueError(f"unsupported package extension: {path}")


def inspect_nupkg(path: str) -> Dict[str, Any]:
    with zipfile.ZipFile(path) as zf:
        names = zf.namelist()
        nuspec_name = next((n for n in names if n.lower().endswith(".nuspec")), None)
        if not nuspec_name:
            raise ValueError(f"nupkg missing .nuspec: {path}")
        root = ElementTree.fromstring(zf.read(nuspec_name))
        metadata = _first_child(root, "metadata")
        if metadata is None:
            metadata = root
        package_id = _child_text(metadata, "id")
        version = _child_text(metadata, "version")
        license_info = _nuget_license(metadata, names)
        dependencies = _nuget_dependencies(metadata)
        runtime_targets = sorted(_nuget_runtime_targets(names))
        native_libs = sorted(n for n in names if _NATIVE_LIB_RE.search(_norm(n)))
        leakage = _scan_zip_leakage(zf, names)

    return {
        "type": "nupkg",
        "path": path,
        "sha256": sha256_file(path),
        "name": package_id,
        "id": package_id,
        "version": version,
        "runtime_targets": runtime_targets,
        "native_lib_present": bool(native_libs),
        "native_libs": native_libs,
        "dependencies": dependencies,
        "license": license_info,
        "internal_path_leakage": leakage,
    }


def inspect_wheel(path: str) -> Dict[str, Any]:
    with zipfile.ZipFile(path) as zf:
        names = zf.namelist()
        metadata_name = next((n for n in names if n.endswith(".dist-info/METADATA")), None)
        wheel_name = next((n for n in names if n.endswith(".dist-info/WHEEL")), None)
        record_name = next((n for n in names if n.endswith(".dist-info/RECORD")), None)
        if not metadata_name or not wheel_name:
            raise ValueError(f"wheel missing METADATA or WHEEL: {path}")
        metadata = email.parser.Parser().parsestr(zf.read(metadata_name).decode("utf-8", "replace"))
        wheel = email.parser.Parser().parsestr(zf.read(wheel_name).decode("utf-8", "replace"))
        tags = [_parse_wheel_tag(tag) for tag in wheel.get_all("Tag", [])]
        record_entries = _read_record(zf.read(record_name).decode("utf-8", "replace")) if record_name else []
        native_libs = sorted(n for n in names if _WHEEL_NATIVE_RE.search(n))
        license_info = _wheel_license(metadata, names)
        leakage = _scan_zip_leakage(zf, names)

    return {
        "type": "wheel",
        "path": path,
        "sha256": sha256_file(path),
        "name": metadata.get("Name"),
        "version": metadata.get("Version"),
        "requires_dist": metadata.get_all("Requires-Dist", []),
        "tags": tags,
        "record": record_entries,
        "native_lib_present": bool(native_libs),
        "native_libs": native_libs,
        "license": license_info,
        "internal_path_leakage": leakage,
    }


def inspect_npm(path: str) -> Dict[str, Any]:
    with tarfile.open(path, "r:gz") as tf:
        members = [m for m in tf.getmembers() if m.isfile()]
        names = [m.name for m in members]
        package_member = next((m for m in members if m.name == "package/package.json"), None)
        if not package_member:
            raise ValueError(f"npm package missing package/package.json: {path}")
        package_file = tf.extractfile(package_member)
        package_json = json.loads(package_file.read().decode("utf-8")) if package_file else {}
        native_bins = sorted(n for n in names if _NPM_NATIVE_RE.search(n))
        license_info = _npm_license(package_json, names)
        leakage = _scan_tar_leakage(tf, members)

    return {
        "type": "npm",
        "path": path,
        "sha256": sha256_file(path),
        "name": package_json.get("name"),
        "version": package_json.get("version"),
        "dependencies": package_json.get("dependencies", {}),
        "os": package_json.get("os"),
        "cpu": package_json.get("cpu"),
        "bin": package_json.get("bin"),
        "main": package_json.get("main"),
        "native_lib_present": bool(native_bins),
        "native_libs": native_bins,
        "package_json": package_json,
        "license": license_info,
        "internal_path_leakage": leakage,
    }


def to_assertions(report: Dict[str, Any], expected_name: str, expected_version: str,
                  expect_native: bool = True) -> List[Dict[str, Any]]:
    actual_name = report.get("name") or report.get("id")
    actual_version = report.get("version")
    leakage = report.get("internal_path_leakage") or []
    license_info = report.get("license")
    asserts = [
        _assertion(
            "package name matches",
            actual_name == expected_name,
            f"actual={actual_name!r}, expected={expected_name!r}",
        ),
        _assertion(
            "package version matches",
            _normalize_version(actual_version) == _normalize_version(expected_version),
            f"actual={actual_version!r}, expected={expected_version!r}",
        ),
    ]
    if expect_native:
        # Some packages are pure-managed and ship native code in a separate runtime package
        # (e.g. the C# Microsoft.AI.Foundry.Local depends on ...Runtime for the native libs);
        # for those, callers pass expect_native=False so this check is not applied.
        asserts.append(_assertion(
            "native lib present",
            bool(report.get("native_lib_present")),
            f"native_libs={report.get('native_libs', [])!r}",
        ))
    asserts.append(_assertion("no internal-path/debug leakage", not leakage,
                              "; ".join(leakage) if leakage else None))
    asserts.append(_assertion("license present", _license_present(license_info), f"license={license_info!r}"))
    return asserts


def _assertion(name: str, ok: bool, detail: str | None = None) -> Dict[str, Any]:
    return {"name": name, "ok": ok, "detail": None if ok else detail}


def _normalize_version(version: Any) -> str:
    value = str(version or "").strip().lower().replace("_", "-")
    value = re.sub(r"(?<=\d)(a|b|rc)(\d+)$", r"-\1\2", value)
    value = re.sub(r"(?<=\d)(alpha|beta|preview)(\d+)$", r"-\1\2", value)
    return value


def _license_present(license_info: Any) -> bool:
    if isinstance(license_info, dict):
        return any(bool(license_info.get(k)) for k in ("expression", "file", "text", "metadata"))
    return bool(license_info)


def _norm(path: str) -> str:
    return path.replace("\\", "/")


def _local_name(path: str) -> str:
    return posixpath.basename(_norm(path))


def _first_child(element: ElementTree.Element, local_name: str) -> ElementTree.Element | None:
    for child in element:
        if _strip_ns(child.tag) == local_name:
            return child
    return None


def _children(element: ElementTree.Element, local_name: str) -> Iterable[ElementTree.Element]:
    for child in element:
        if _strip_ns(child.tag) == local_name:
            yield child


def _child_text(element: ElementTree.Element, local_name: str) -> str | None:
    child = _first_child(element, local_name)
    return child.text.strip() if child is not None and child.text else None


def _strip_ns(tag: str) -> str:
    return tag.rsplit("}", 1)[-1]


def _nuget_license(metadata: ElementTree.Element, names: List[str]) -> Dict[str, str | None]:
    license_element = _first_child(metadata, "license")
    license_file = None
    license_expression = None
    if license_element is not None and license_element.text:
        license_type = license_element.attrib.get("type", "expression")
        if license_type == "file":
            license_file = license_element.text.strip()
        else:
            license_expression = license_element.text.strip()
    if not license_file:
        license_file = next((_local_name(n) for n in names if _local_name(n).lower().startswith("license")), None)
    return {"expression": license_expression, "file": license_file, "url": _child_text(metadata, "licenseUrl")}


def _nuget_dependencies(metadata: ElementTree.Element) -> List[Dict[str, str | None]]:
    dependencies: List[Dict[str, str | None]] = []
    deps_element = _first_child(metadata, "dependencies")
    if deps_element is None:
        return dependencies
    for dependency in deps_element.iter():
        if _strip_ns(dependency.tag) != "dependency":
            continue
        dependencies.append({
            "id": dependency.attrib.get("id"),
            "version": dependency.attrib.get("version"),
            "include": dependency.attrib.get("include"),
            "exclude": dependency.attrib.get("exclude"),
        })
    return dependencies


def _nuget_runtime_targets(names: List[str]) -> set[str]:
    targets: set[str] = set()
    for name in names:
        norm = _norm(name)
        parts = norm.split("/")
        if len(parts) >= 3 and parts[0] == "runtimes" and parts[2] == "native":
            targets.add(parts[1])
        if parts and parts[0] in {"build", "buildTransitive", "buildMultiTargeting"}:
            targets.add("/".join(parts[:2]) if len(parts) > 1 else parts[0])
    return targets


def _parse_wheel_tag(tag: str) -> Dict[str, str]:
    parts = tag.split("-", 2)
    return {
        "tag": tag,
        "python": parts[0] if len(parts) > 0 else "",
        "abi": parts[1] if len(parts) > 1 else "",
        "platform": parts[2] if len(parts) > 2 else "",
    }


def _read_record(text: str) -> List[Dict[str, str]]:
    rows: List[Dict[str, str]] = []
    for row in csv.reader(io.StringIO(text)):
        if not row:
            continue
        padded = row + [""] * (3 - len(row))
        rows.append({"path": padded[0], "hash": padded[1], "size": padded[2]})
    return rows


def _wheel_license(metadata: Any, names: List[str]) -> Dict[str, Any]:
    files = metadata.get_all("License-File", [])
    archive_license = next((n for n in names if _local_name(n).lower().startswith("license")), None)
    return {
        "metadata": metadata.get("License") or metadata.get("License-Expression"),
        "file": files[0] if files else archive_license,
        "classifiers": [c for c in metadata.get_all("Classifier", []) if "License" in c],
    }


def _npm_license(package_json: Dict[str, Any], names: List[str]) -> Dict[str, Any]:
    archive_license = next((n for n in names if _local_name(n).lower().startswith("license")), None)
    return {"expression": package_json.get("license"), "file": archive_license}


def _scan_zip_leakage(zf: zipfile.ZipFile, names: List[str]) -> List[str]:
    findings: List[str] = []
    for name in names:
        findings.extend(_path_findings(name))
        if _should_scan_text(name):
            try:
                data = zf.read(name, 1024 * 1024 + 1)
            except TypeError:
                data = zf.read(name)
            findings.extend(_content_findings(name, data))
    return _dedupe(findings)


def _scan_tar_leakage(tf: tarfile.TarFile, members: List[tarfile.TarInfo]) -> List[str]:
    findings: List[str] = []
    for member in members:
        findings.extend(_path_findings(member.name))
        if _should_scan_text(member.name):
            extracted = tf.extractfile(member)
            if extracted:
                findings.extend(_content_findings(member.name, extracted.read(1024 * 1024 + 1)))
    return _dedupe(findings)


def _path_findings(path: str) -> List[str]:
    findings = []
    norm = _norm(path)
    if norm.startswith("/") or re.match(r"^[A-Za-z]:/", norm):
        findings.append(f"absolute path entry: {path}")
    if ".." in norm.split("/"):
        findings.append(f"parent-directory path entry: {path}")
    if _INTERNAL_PATH_RE.search(path) or _INTERNAL_PATH_RE.search(norm):
        findings.append(f"internal path in entry name: {path}")
    if _DEBUG_FILE_RE.search(norm):
        findings.append(f"debug/internal file entry: {path}")
    return findings


def _should_scan_text(path: str) -> bool:
    norm = _norm(path)
    if _DEBUG_FILE_RE.search(norm):
        return False
    ext = posixpath.splitext(norm)[1].lower()
    return ext in _TEXT_EXTENSIONS or _local_name(norm).lower() in {"metadata", "wheel", "record", "package.json"}


def _content_findings(path: str, data: bytes) -> List[str]:
    if len(data) > 1024 * 1024:
        return [f"text file too large to scan fully: {path}"]
    text = data.decode("utf-8", "ignore")
    return [f"internal path in file content: {path}"] if _INTERNAL_PATH_RE.search(text) else []


def _dedupe(values: Iterable[str]) -> List[str]:
    seen = set()
    result = []
    for value in values:
        if value not in seen:
            seen.add(value)
            result.append(value)
    return result
