from __future__ import annotations

import io
import json
import os
import shutil
import tarfile
import unittest
import zipfile

from orchestrator import pkg_inspect


class PackageInspectTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.fixture_dir = os.path.join(os.path.dirname(__file__), "__pkg_inspect_fixtures__")
        os.makedirs(cls.fixture_dir, exist_ok=True)

    @classmethod
    def tearDownClass(cls) -> None:
        shutil.rmtree(cls.fixture_dir, ignore_errors=True)

    def test_inspect_nupkg_clean_and_dirty(self) -> None:
        clean = self._write_nupkg("clean.nupkg", dirty=False)
        report = pkg_inspect.inspect_nupkg(clean)
        self.assertEqual(report["name"], "Microsoft.AI.Foundry.Local.Runtime")
        self.assertEqual(report["version"], "2.0.0-rc1")
        self.assertIn("win-x64", report["runtime_targets"])
        self.assertTrue(report["native_lib_present"])
        self.assertEqual(report["dependencies"][0]["id"], "Microsoft.ML.OnnxRuntime")
        self.assertEqual(report["license"]["expression"], "MIT")
        self.assertEqual(report["internal_path_leakage"], [])
        self._assert_all_ok(pkg_inspect.to_assertions(report, "Microsoft.AI.Foundry.Local.Runtime", "2.0.0rc1"))

        dirty = self._write_nupkg("dirty.nupkg", dirty=True)
        dirty_report = pkg_inspect.inspect(dirty)
        assertions = pkg_inspect.to_assertions(dirty_report, "Microsoft.AI.Foundry.Local.Runtime", "2.0.0-rc1")
        self.assertFalse(self._assertion(assertions, "no internal-path/debug leakage")["ok"])

    def test_inspect_wheel_clean_and_dirty(self) -> None:
        clean = self._write_wheel("clean.whl", dirty=False)
        report = pkg_inspect.inspect_wheel(clean)
        self.assertEqual(report["name"], "foundry-local-sdk")
        self.assertEqual(report["version"], "2.0.0rc1")
        self.assertEqual(report["requires_dist"], ["requests >=2"])
        self.assertEqual(report["tags"][0]["platform"], "win_amd64")
        self.assertTrue(report["native_lib_present"])
        self.assertIn("foundry_local_sdk/native/foundry_local.pyd", report["native_libs"])
        self.assertTrue(any(row["path"].endswith("METADATA") for row in report["record"]))
        self._assert_all_ok(pkg_inspect.to_assertions(report, "foundry-local-sdk", "2.0.0-rc1"))

        dirty = self._write_wheel("dirty.whl", dirty=True)
        dirty_report = pkg_inspect.inspect(dirty)
        assertions = pkg_inspect.to_assertions(dirty_report, "foundry-local-sdk", "2.0.0rc1")
        self.assertFalse(self._assertion(assertions, "no internal-path/debug leakage")["ok"])

    def test_inspect_npm_clean_and_dirty(self) -> None:
        clean = self._write_npm("clean.tgz", dirty=False)
        report = pkg_inspect.inspect_npm(clean)
        self.assertEqual(report["name"], "foundry-local-sdk")
        self.assertEqual(report["version"], "2.0.0-rc1")
        self.assertEqual(report["dependencies"], {"node-addon-api": "^8.0.0"})
        self.assertEqual(report["os"], ["darwin", "linux", "win32"])
        self.assertEqual(report["cpu"], ["x64", "arm64"])
        self.assertEqual(report["main"], "dist/index.js")
        self.assertTrue(report["native_lib_present"])
        self.assertIn("package/prebuilds/darwin-arm64/foundry_local.node", report["native_libs"])
        self._assert_all_ok(pkg_inspect.to_assertions(report, "foundry-local-sdk", "2.0.0rc1"))

        dirty = self._write_npm("dirty.tgz", dirty=True)
        dirty_report = pkg_inspect.inspect(dirty)
        assertions = pkg_inspect.to_assertions(dirty_report, "foundry-local-sdk", "2.0.0-rc1")
        self.assertFalse(self._assertion(assertions, "no internal-path/debug leakage")["ok"])

    def _write_nupkg(self, filename: str, dirty: bool) -> str:
        path = os.path.join(self.fixture_dir, filename)
        nuspec = """<?xml version="1.0"?>
<package xmlns="http://schemas.microsoft.com/packaging/2013/05/nuspec.xsd">
  <metadata>
    <id>Microsoft.AI.Foundry.Local.Runtime</id>
    <version>2.0.0-rc1</version>
    <license type="expression">MIT</license>
    <dependencies><dependency id="Microsoft.ML.OnnxRuntime" version="1.28.0" /></dependencies>
  </metadata>
</package>
"""
        with zipfile.ZipFile(path, "w") as zf:
            zf.writestr("Microsoft.AI.Foundry.Local.Runtime.nuspec", nuspec)
            zf.writestr("runtimes/win-x64/native/foundry_local.dll", b"native")
            zf.writestr("build/native/Microsoft.AI.Foundry.Local.Runtime.targets", "<Project />")
            if dirty:
                zf.writestr("content/readme.txt", "built from C:\\Users\\someone\\secret")
                zf.writestr("symbols/foundry_local.pdb", b"debug")
        return path

    def _write_wheel(self, filename: str, dirty: bool) -> str:
        path = os.path.join(self.fixture_dir, filename)
        metadata = """Metadata-Version: 2.4
Name: foundry-local-sdk
Version: 2.0.0rc1
Requires-Dist: requests >=2
License-Expression: MIT
License-File: LICENSE
"""
        wheel = """Wheel-Version: 1.0
Generator: test
Root-Is-Purelib: false
Tag: cp312-cp312-win_amd64
"""
        record = "foundry_local_sdk-2.0.0rc1.dist-info/METADATA,,\nfoundry_local_sdk/native/foundry_local.pyd,,\n"
        with zipfile.ZipFile(path, "w") as zf:
            zf.writestr("foundry_local_sdk-2.0.0rc1.dist-info/METADATA", metadata)
            zf.writestr("foundry_local_sdk-2.0.0rc1.dist-info/WHEEL", wheel)
            zf.writestr("foundry_local_sdk-2.0.0rc1.dist-info/RECORD", record)
            zf.writestr("foundry_local_sdk/native/foundry_local.pyd", b"native")
            zf.writestr("foundry_local_sdk-2.0.0rc1.dist-info/LICENSE", "MIT")
            if dirty:
                zf.writestr("foundry_local_sdk/debug.txt", "source: C:\\Users\\someone\\secret")
        return path

    def _write_npm(self, filename: str, dirty: bool) -> str:
        path = os.path.join(self.fixture_dir, filename)
        package_json = {
            "name": "foundry-local-sdk",
            "version": "2.0.0-rc1",
            "dependencies": {"node-addon-api": "^8.0.0"},
            "os": ["darwin", "linux", "win32"],
            "cpu": ["x64", "arm64"],
            "bin": {"foundry-local": "bin/foundry-local.js"},
            "main": "dist/index.js",
            "license": "MIT",
        }
        files = {
            "package/package.json": json.dumps(package_json),
            "package/prebuilds/darwin-arm64/foundry_local.node": b"native",
            "package/dist/index.js": "module.exports = {};",
        }
        if dirty:
            files["package/docs/debug.txt"] = "built at C:\\Users\\someone\\secret"
        with tarfile.open(path, "w:gz") as tf:
            for name, data in files.items():
                raw = data if isinstance(data, bytes) else data.encode("utf-8")
                info = tarfile.TarInfo(name)
                info.size = len(raw)
                tf.addfile(info, io.BytesIO(raw))
        return path

    def _assert_all_ok(self, assertions: list[dict[str, object]]) -> None:
        self.assertTrue(all(a["ok"] for a in assertions), assertions)

    def _assertion(self, assertions: list[dict[str, object]], name: str) -> dict[str, object]:
        return next(a for a in assertions if a["name"] == name)


if __name__ == "__main__":
    unittest.main()
