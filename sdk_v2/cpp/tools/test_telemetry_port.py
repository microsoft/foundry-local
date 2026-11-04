"""Check the telemetry overlay's platform and feature policy without downloads."""

import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


CPP_ROOT = Path(__file__).resolve().parents[1]
PORT = CPP_ROOT / "ports" / "cpp-client-telemetry"


class TelemetryPortTests(unittest.TestCase):
    def run_port(self, platform, features):
        version = json.loads((PORT / "vcpkg.json").read_text())["version"]
        features_string = ";".join(features)
        portfile = (PORT / "portfile.cmake").as_posix()
        scratch = CPP_ROOT / "build" / "telemetry-port-tests"
        scratch.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=scratch) as temporary:
            root = Path(temporary)
            script = root / "check.cmake"
            options = root / "options.txt"
            script.write_text(
                f"""
cmake_minimum_required(VERSION 3.18)
function(vcpkg_check_linkage)
endfunction()
function(vcpkg_from_github)
    cmake_parse_arguments(PARSE_ARGV 0 arg "" "OUT_SOURCE_PATH;REPO;REF;SHA512;HEAD_REF" "PATCHES")
    if(NOT arg_REF STREQUAL "v{version}")
        message(FATAL_ERROR "Unexpected release: ${{arg_REF}}")
    endif()
    if(NOT arg_PATCHES STREQUAL "apple-system-libraries.patch;sdk-version-lifetime.patch")
        message(FATAL_ERROR "Missing telemetry compatibility patches")
    endif()
    set(${{arg_OUT_SOURCE_PATH}} "{root.as_posix()}" PARENT_SCOPE)
endfunction()
function(vcpkg_check_features)
    cmake_parse_arguments(PARSE_ARGV 0 arg "" "OUT_FEATURE_OPTIONS" "FEATURES")
    set(result)
    while(arg_FEATURES)
        list(POP_FRONT arg_FEATURES feature option)
        set(value OFF)
        if(feature IN_LIST FEATURES)
            set(value ON)
        endif()
        list(APPEND result "-D${{option}}=${{value}}")
    endwhile()
    set(${{arg_OUT_FEATURE_OPTIONS}} "${{result}}" PARENT_SCOPE)
endfunction()
function(vcpkg_cmake_configure)
    cmake_parse_arguments(PARSE_ARGV 0 arg "" "SOURCE_PATH" "OPTIONS")
    string(REPLACE ";" "\\n" result "${{arg_OPTIONS}}")
    file(WRITE "{options.as_posix()}" "${{result}}")
endfunction()
function(vcpkg_cmake_install)
endfunction()
function(vcpkg_cmake_config_fixup)
endfunction()
function(vcpkg_install_copyright)
endfunction()
set(VERSION {version})
set(FEATURES "{features_string}")
set(VCPKG_TARGET_IS_{platform} ON)
set(CURRENT_PACKAGES_DIR "{root.as_posix()}/package")
include("{portfile}")
""",
                encoding="utf-8",
            )
            result = subprocess.run(
                [shutil.which("cmake") or "cmake", "-P", str(script)],
                capture_output=True,
                text=True,
                check=False,
            )
            return result, options.read_text().splitlines() if options.exists() else []

    def test_foundry_platform_policy(self):
        for platform in ("WINDOWS", "LINUX", "OSX", "IOS", "ANDROID"):
            with self.subTest(platform=platform):
                result, options = self.run_port(
                    platform, ["minimal-sqlite", "curl-openssl", "no-logging"]
                )
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                for option in (
                    "-DMATSDK_DISABLE_LOGGING=ON",
                    "-DMATSDK_ENABLE_DEVICE_ID=OFF",
                    "-DMATSDK_DISABLE_EXCEPTIONS=OFF",
                    "-DMATSDK_USE_WININET=OFF",
                    "-DMATSDK_ANDROID_HTTP_CLIENT=CURL",
                    "-DMATSDK_ANDROID_USE_ROOM=OFF",
                    "-DMATSDK_ENABLE_CAPI_HTTP_CLIENT=OFF",
                    "-DMATSDK_CURL_PROVIDER=SYSTEM",
                    "-DMATSDK_ZLIB_PROVIDER=SYSTEM",
                    "-DBUILD_SHARED_LIBS=OFF",
                ):
                    self.assertIn(option, options)
                apple = platform in ("OSX", "IOS")
                self.assertIn(
                    f"-DMATSDK_SQLITE_PROVIDER={'SYSTEM' if apple else 'MINIMAL'}",
                    options,
                )
                self.assertIn(
                    f"-DMATSDK_BUILD_APPLE_HTTP={'ON' if apple else 'OFF'}", options
                )
                self.assertEqual(
                    "-DCMAKE_OSX_DEPLOYMENT_TARGET=13.0" in options, platform == "IOS"
                )

    def test_opt_in_features_and_external_sqlite(self):
        result, options = self.run_port(
            "LINUX", ["device-id", "system-sqlite", "curl-openssl"]
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("-DMATSDK_ENABLE_DEVICE_ID=ON", options)
        self.assertIn("-DMATSDK_DISABLE_LOGGING=OFF", options)
        self.assertIn("-DMATSDK_SQLITE_PROVIDER=SYSTEM", options)

    def test_rejects_conflicting_sqlite_features(self):
        result, _ = self.run_port("LINUX", ["minimal-sqlite", "system-sqlite"])
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("mutually exclusive", " ".join(result.stderr.split()))

    def test_manifest_policy(self):
        manifest = json.loads((CPP_ROOT / "vcpkg.json").read_text())
        overlay = json.loads((PORT / "vcpkg.json").read_text())
        telemetry = manifest["features"]["telemetry"]["dependencies"][0]
        self.assertRegex(manifest["builtin-baseline"], r"^[0-9a-f]{40}$")
        self.assertIn(
            {"name": "cpp-client-telemetry", "version": overlay["version"]},
            manifest["overrides"],
        )
        self.assertFalse(telemetry["default-features"])
        self.assertEqual(
            set(telemetry["features"]), {"minimal-sqlite", "curl-openssl", "no-logging"}
        )
        self.assertNotIn("device-id", overlay["default-features"])
        curl = overlay["features"]["curl-openssl"]["dependencies"][0]
        self.assertEqual(curl["features"], ["openssl"])
        self.assertEqual(curl["platform"], "linux | android")
        self.assertFalse(curl["default-features"])
        zlib = next(
            item
            for item in overlay["dependencies"]
            if isinstance(item, dict) and item["name"] == "zlib"
        )
        self.assertEqual(zlib["platform"], "!osx & !ios")
        sqlite = overlay["features"]["system-sqlite"]["dependencies"][0]
        self.assertEqual(sqlite["platform"], "!osx & !ios")


if __name__ == "__main__":
    unittest.main()
