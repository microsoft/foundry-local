# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.

from __future__ import annotations

import argparse
import logging
import os
import platform
import shutil
import subprocess
import sys
import textwrap

from pathlib import Path

SCRIPT_DIR = Path(__file__).parent


# ---------------------------------------------------------------------------
# Logging
# ---------------------------------------------------------------------------

def _get_logger(name: str, level: int = logging.INFO) -> logging.Logger:
    logging.basicConfig(format="%(asctime)s %(name)s [%(levelname)s] - %(message)s", level=level)
    logger = logging.getLogger(name)
    logger.setLevel(level)
    return logger


log = _get_logger("build.py")


# ---------------------------------------------------------------------------
# Platform helpers
# ---------------------------------------------------------------------------

def is_windows() -> bool:
    return sys.platform.startswith("win")


def is_linux() -> bool:
    return sys.platform.startswith("linux")


def is_mac() -> bool:
    return sys.platform.startswith("darwin")


def _path_from_env_var(var: str) -> Path | None:
    """Return a Path from an environment variable, or None if unset/empty."""
    val = os.environ.get(var)
    return Path(val) if val else None


# ---------------------------------------------------------------------------
# Subprocess runner
# ---------------------------------------------------------------------------

def run(cmd: list[str], cwd: Path | str | None = None, env: dict[str, str] | None = None) -> None:
    """Run a command, logging and streaming output. Raises on non-zero exit."""
    log.info("Running: %s", " ".join(str(c) for c in cmd))
    subprocess.run(cmd, cwd=cwd, env=env, check=True)


# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------

def _parse_args() -> argparse.Namespace:
    class HelpFormatter(argparse.ArgumentDefaultsHelpFormatter, argparse.RawDescriptionHelpFormatter):
        pass

    parser = argparse.ArgumentParser(
        description="Foundry Local C++ SDK Build Driver.",
        epilog=textwrap.dedent("""\
            There are 3 phases which can be individually selected.

            The configure (--configure) phase will run CMake to generate makefiles.
            The build (--build) phase will build all projects.
            The test (--test) phase will run all unit tests.

            Default behavior is --configure --build --test.

            If phases are explicitly specified only those phases will be run.
            E.g., run with --build to rebuild without running the configure or test phases.
        """),
        fromfile_prefix_chars="@",
        formatter_class=HelpFormatter,
    )

    parser.add_argument(
        "--build_dir",
        type=Path,
        help="Path to the build directory. Defaults to 'build/<platform>'.",
    )

    parser.add_argument(
        "--config",
        default="RelWithDebInfo",
        type=str,
        choices=["Debug", "MinSizeRel", "Release", "RelWithDebInfo"],
        help="Configuration to build.",
    )

    # Build phases
    parser.add_argument("--configure", action="store_true", help="Run CMake configure to generate makefiles.")
    parser.add_argument("--build", action="store_true", help="Build.")
    parser.add_argument("--test", action="store_true", help="Run tests.")
    parser.add_argument(
        "--clean", action="store_true", help="Run 'cmake --build --target clean' for the selected config."
    )

    parser.add_argument("--skip_tests", action="store_true",
                        help="Skip running tests. Overrides --test. Tests are still built.")

    parser.add_argument(
        "--parallel",
        nargs="?",
        const=0,
        default=0,
        type=int,
        help="Use parallel build jobs. Optional value specifies max jobs (0 = number of CPUs, default).",
    )

    # CMake tool paths
    parser.add_argument("--cmake_path", default="cmake", type=Path, help="Path to the CMake program.")
    parser.add_argument("--ctest_path", default="ctest", type=Path, help="Path to the CTest program.")

    parser.add_argument(
        "--cmake_generator",
        choices=[
            "MinGW Makefiles",
            "Ninja",
            "NMake Makefiles",
            "Unix Makefiles",
            "Visual Studio 17 2022",
            "Visual Studio 18 2026",
            "Xcode",
        ],
        default=None,
        help="Specify the generator that CMake invokes. Auto-detected if not set.",
    )

    parser.add_argument(
        "--cmake_extra_defines",
        nargs="+",
        action="append",
        help="Extra CMake -D definitions (without the leading -D).",
    )

    # Project-specific options
    parser.add_argument("--vcpkg_root", default=None, type=Path,
                        help="Root directory of vcpkg. Auto-detected if not set.")
    parser.add_argument("--ort_home", default=None, type=Path, help="Root directory of ONNX Runtime.")
    parser.add_argument("--ort_genai_home", default=None, type=Path, help="Root directory of ONNX Runtime GenAI.")
    parser.add_argument(
        "--skip_examples", action="store_true", help="Skip building the example programs."
    )
    parser.add_argument(
        "--skip_service", action="store_true", help="Skip building web service support (oat++)."
    )
    parser.add_argument(
        "--winml_sdk_version", default=None, type=str,
        help="Override the Microsoft.Windows.AI.MachineLearning NuGet version for the WinML EP "
             "catalog (Windows only). Defaults to the version pinned in deps_versions.json.",
    )

    # Cross-compilation (mutually exclusive targets)
    cross_group = parser.add_mutually_exclusive_group()
    cross_group.add_argument(
        "--arm64",
        action="store_true",
        help="[cross-compiling] Generate ARM64 build files.",
    )
    cross_group.add_argument(
        "--android",
        action="store_true",
        help="[cross-compiling] Build for Android.",
    )

    # Android options
    parser.add_argument(
        "--android_abi",
        default="arm64-v8a",
        choices=["arm64-v8a", "x86_64"],
        help="Android target ABI. Use arm64-v8a for devices, x86_64 for emulator.",
    )
    parser.add_argument(
        "--android_api",
        type=int,
        default=28,
        help="Android minimum API level. Default 28 (Android 9.0 Pie).",
    )
    parser.add_argument(
        "--android_home",
        type=Path,
        default=_path_from_env_var("ANDROID_HOME"),
        help="Path to the Android SDK. Defaults to ANDROID_HOME env var, "
        "or inferred from --android_ndk_path (<ndk/version>/../..).",
    )
    parser.add_argument(
        "--android_ndk_path",
        type=Path,
        default=_path_from_env_var("ANDROID_NDK_HOME"),
        help="Path to the Android NDK. Typically <Android SDK>/ndk/<version>.",
    )
    parser.add_argument(
        "--android_run_emulator",
        action="store_true",
        help="Create/start an Android emulator to run tests. "
        "Requires --android and --android_abi x86_64.",
    )
    parser.add_argument(
        "--android_emulator_api",
        type=int,
        default=None,
        help="API level for the emulator system image. "
        "Defaults to --android_api. Use when the installed system image "
        "differs from the build API level (e.g. build at 28, test on 29).",
    )
    parser.add_argument(
        "--java_home",
        type=Path,
        default=_path_from_env_var("JAVA_HOME"),
        help="Path to a Java 17+ JDK. Required for Android SDK cmdline-tools "
        "(sdkmanager, avdmanager). Defaults to JAVA_HOME env var.",
    )
    return parser.parse_args()


# ---------------------------------------------------------------------------
# Validation
# ---------------------------------------------------------------------------

def _resolve_executable_path(command_or_path: Path) -> Path:
    executable_path = shutil.which(str(command_or_path))
    if executable_path is None:
        raise ValueError(f"Failed to resolve executable path for '{command_or_path}'.")
    return Path(executable_path)


def _default_cmake_generator() -> str:
    if is_windows():
        return "Visual Studio 18 2026"
    if is_mac():
        return "Unix Makefiles"
    return "Unix Makefiles"


def _validate_args(args: argparse.Namespace) -> None:
    # Default to all 3 stages
    if not any((args.configure, args.clean, args.build, args.test)):
        args.configure = True
        args.build = True
        args.test = True

    args.cmake_path = _resolve_executable_path(args.cmake_path)
    args.ctest_path = _resolve_executable_path(args.ctest_path)

    if args.cmake_generator is None:
        args.cmake_generator = _default_cmake_generator()

    # Build directory — anchored to this script's directory (sdk_v2/cpp/) so output is
    # cwd-independent. The C# and Python SDKs both load the native binary via paths
    # rooted at sdk_v2/cpp/build/<platform>/<config>/...; running build.py from another
    # cwd must not relocate the output. See .github/instructions/cpp-build.instructions.md.
    if not args.build_dir:
        target_sys = platform.system()
        if args.android:
            target_sys = f"Android-{args.android_abi}"
        elif is_mac():
            target_sys = "macOS"
        args.build_dir = SCRIPT_DIR / "build" / target_sys

    args.build_dir = args.build_dir / args.config

    if args.configure:
        # Create the build directory if it doesn't exist during the configure phase
        args.build_dir.mkdir(parents=True, exist_ok=True)
    elif not args.build_dir.exists():
        raise FileNotFoundError(
            f"Build directory '{args.build_dir}' does not exist. Run with --configure first."
        )

    args.build_dir = args.build_dir.resolve(strict=True)

    # vcpkg root
    if not args.vcpkg_root:
        # Auto-detect: VCPKG_ROOT env, then VS-bundled location
        env_root = os.environ.get("VCPKG_ROOT")
        if env_root and Path(env_root).is_dir():
            args.vcpkg_root = Path(env_root)
        else:
            vs_vcpkg = Path(r"C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\vcpkg")
            if vs_vcpkg.is_dir():
                args.vcpkg_root = vs_vcpkg
    if args.vcpkg_root:
        if not args.vcpkg_root.is_dir():
            raise ValueError(f"--vcpkg_root '{args.vcpkg_root}' does not exist or is not a directory.")
        args.vcpkg_root = args.vcpkg_root.resolve(strict=True)
        toolchain = args.vcpkg_root / "scripts" / "buildsystems" / "vcpkg.cmake"
        if not toolchain.is_file():
            raise FileNotFoundError(f"vcpkg toolchain not found at '{toolchain}'.")
        args.vcpkg_toolchain = toolchain
    else:
        raise ValueError(
            "vcpkg root not found. Set VCPKG_ROOT environment variable or pass --vcpkg_root."
        )

    # ORT home
    if args.ort_home:
        if not args.ort_home.exists() or not args.ort_home.is_dir():
            raise ValueError(f"--ort_home '{args.ort_home}' does not exist or is not a directory.")
        args.ort_home = args.ort_home.resolve(strict=True)

    # ORT GenAI home
    if args.ort_genai_home:
        if not args.ort_genai_home.exists() or not args.ort_genai_home.is_dir():
            raise ValueError(f"--ort_genai_home '{args.ort_genai_home}' does not exist or is not a directory.")
        args.ort_genai_home = args.ort_genai_home.resolve(strict=True)

    if args.parallel < 0:
        raise ValueError("--parallel must be >= 0")

    if args.parallel == 0:
        args.parallel = os.cpu_count() or 1

    # cmake_extra_defines
    args.cmake_extra_defines = (
        [f"-D{d}" for j in args.cmake_extra_defines for d in j] if args.cmake_extra_defines else []
    )

    # Cross-compilation
    if args.arm64:
        if args.test:
            log.warning(
                "Cannot test on host for cross-compiled ARM64 builds. Disabling --test."
            )
            args.test = False

    if args.android:
        # Ninja is the only generator that works well with the NDK toolchain
        args.cmake_generator = "Ninja"

        # Resolve NDK: explicit arg > ANDROID_NDK_HOME env > let vcpkg discover
        ndk_home = None
        if args.android_ndk_path:
            ndk_home = args.android_ndk_path.resolve(strict=True)
        elif os.environ.get("ANDROID_NDK_HOME"):
            ndk_home = Path(os.environ["ANDROID_NDK_HOME"]).resolve(strict=True)

        if ndk_home:
            ndk_toolchain = ndk_home / "build" / "cmake" / "android.toolchain.cmake"
            if not ndk_toolchain.is_file():
                raise FileNotFoundError(
                    f"NDK toolchain not found at '{ndk_toolchain}'. "
                    f"Check that the NDK path points to the NDK root."
                )
            log.info("Using Android NDK at: %s", ndk_home)
        else:
            log.info("No NDK path specified; relying on vcpkg/CMake NDK discovery.")

        args.android_ndk_home = ndk_home

        # Infer android_home from the NDK path if not explicitly provided.
        # NDK lives at <sdk>/ndk/<version>, so grandparent is the SDK root.
        if not args.android_home and ndk_home:
            args.android_home = ndk_home.parent.parent

        # Map ABI to vcpkg triplet
        abi_to_triplet = {
            "arm64-v8a": "arm64-android",
            "x86_64": "x64-android",
        }
        args.android_vcpkg_triplet = abi_to_triplet[args.android_abi]

        # Resolve ninja — CMake needs an explicit path when it's not in a standard location
        ninja_path = shutil.which("ninja")
        if not ninja_path:
            raise ValueError(
                "ninja not found on PATH. Install via 'pip install ninja' "
                "or ensure ninja is available."
            )
        args.ninja_path = Path(ninja_path)

        # Testing on an emulator requires x86_64 ABI and --android_run_emulator
        if args.test and not args.android_run_emulator:
            log.warning(
                "Cannot test on host for Android builds. Disabling --test. "
                "Use --android_run_emulator --android_abi x86_64 to run tests on an emulator."
            )
            args.test = False

        if args.android_run_emulator:
            if args.android_abi != "x86_64":
                raise ValueError(
                    "--android_run_emulator requires --android_abi x86_64 (emulator only supports x86_64)."
                )
            if not args.android_home:
                raise ValueError(
                    "--android_home is required for --android_run_emulator "
                    "(or set ANDROID_HOME environment variable)."
                )
            args.android_home = args.android_home.resolve(strict=True)
            if not args.android_emulator_api:
                args.android_emulator_api = args.android_api

            # Validate Java 17+ for SDK cmdline-tools
            if args.java_home:
                args.java_home = args.java_home.resolve(strict=True)
                sys.path.insert(0, str(SCRIPT_DIR / "tools"))
                import android as android_tools
                android_tools.validate_java_home(args.java_home)
            else:
                log.warning(
                    "No --java_home or JAVA_HOME set. "
                    "sdkmanager/avdmanager may fail if system Java is < 17."
                )


# ---------------------------------------------------------------------------
# Build phases
# ---------------------------------------------------------------------------

def configure(args: argparse.Namespace) -> None:
    """Run CMake configure."""
    command = [
        str(args.cmake_path),
        "-G", args.cmake_generator,
        f"-DCMAKE_BUILD_TYPE={args.config}",
        f"-DCMAKE_TOOLCHAIN_FILE={args.vcpkg_toolchain}",
        "-S", str(SCRIPT_DIR),
        "-B", str(args.build_dir),
    ]

    # Use custom triplet overlay for Linux (adds -fPIC for shared library linking)
    triplets_dir = SCRIPT_DIR / "triplets"
    if triplets_dir.is_dir():
        command += [f"-DVCPKG_OVERLAY_TRIPLETS={triplets_dir}"]

    # Project options
    build_tests = "ON"

    build_examples = "OFF" if args.skip_examples else "ON"
    build_service = "OFF" if args.skip_service else "ON"

    command += [
        f"-DFOUNDRY_LOCAL_BUILD_TESTS={build_tests}",
        f"-DFOUNDRY_LOCAL_BUILD_EXAMPLES={build_examples}",
        f"-DFOUNDRY_LOCAL_BUILD_SERVICE={build_service}",
    ]

    # Enable vcpkg manifest features for tests
    if build_tests == "ON":
        command += ["-DVCPKG_MANIFEST_FEATURES=tests"]

    # WinML EP catalog is enabled automatically on Windows by CMake. Allow an
    # optional version override for the Microsoft.Windows.AI.MachineLearning NuGet.
    if args.winml_sdk_version:
        command += [f"-DWINML_EP_CATALOG_VERSION={args.winml_sdk_version}"]

    if args.ort_home:
        command += [f"-DORT_HOME={args.ort_home}"]

    if args.ort_genai_home:
        command += [f"-DORT_GENAI_HOME={args.ort_genai_home}"]

    # Cross-compilation
    if args.cmake_generator.startswith("Visual Studio"):
        if args.arm64:
            command += ["-A", "ARM64"]

    if args.android:
        # VCPKG_CHAINLOAD_TOOLCHAIN_FILE tells vcpkg.cmake to chain-load the NDK
        # toolchain for the top-level project. The triplet sets this for port builds;
        # the command line is needed for the project itself.
        ndk_toolchain_file = str(args.android_ndk_home / "build" / "cmake" /
                                 "android.toolchain.cmake") if args.android_ndk_home else ""
        command += [
            f"-DVCPKG_TARGET_TRIPLET={args.android_vcpkg_triplet}",
            f"-DANDROID_PLATFORM=android-{args.android_api}",
            f"-DANDROID_ABI={args.android_abi}",
            f"-DCMAKE_MAKE_PROGRAM={args.ninja_path}",
        ]
        if ndk_toolchain_file:
            command += [f"-DVCPKG_CHAINLOAD_TOOLCHAIN_FILE={ndk_toolchain_file}"]

    command += args.cmake_extra_defines

    # For Android, set ANDROID_NDK_HOME so the vcpkg triplet can chain-load
    # the NDK toolchain. This ensures both vcpkg ports and the top-level
    # project use the same NDK.
    env = None
    if args.android and args.android_ndk_home:
        env = os.environ.copy()
        env["ANDROID_NDK_HOME"] = str(args.android_ndk_home)

    # Anchor cwd to SCRIPT_DIR (sdk_v2/cpp/) so vcpkg's cwd-relative scratch
    # directories (downloads, buildtrees) stay inside sdk_v2/cpp/build, not at
    # the invoker's cwd. The -B arg controls cmake; vcpkg ignores it for caches.
    run(command, cwd=str(SCRIPT_DIR), env=env)


def build(args: argparse.Namespace) -> None:
    """Build all targets."""
    command = [str(args.cmake_path), "--build", str(args.build_dir), "--config", args.config]

    if args.parallel > 1:
        # CMake maps this to the native parallelism flag for the active generator:
        # MSBuild (/m) on Windows, make/ninja (-j) on Linux/macOS.
        command += ["--parallel", str(args.parallel)]

    run(command, cwd=str(SCRIPT_DIR))


def clean(args: argparse.Namespace) -> None:
    """Clean the build output."""
    log.info("Cleaning targets.")
    command = [
        str(args.cmake_path), "--build", str(args.build_dir),
        "--config", args.config, "--target", "clean",
    ]
    run(command, cwd=str(SCRIPT_DIR))


def test(args: argparse.Namespace) -> None:
    """Run CTest."""
    command = [
        str(args.ctest_path),
        "--build-config", args.config,
        "--output-on-failure",
        "--timeout", "600",
    ]
    run(command, cwd=str(args.build_dir))


def android_test(args: argparse.Namespace) -> None:
    """Run tests on an Android emulator."""
    import contextlib
    sys.path.insert(0, str(SCRIPT_DIR / "tools"))
    import android as android_tools

    sdk_tool_paths = android_tools.get_sdk_tool_paths(args.android_home)

    with contextlib.ExitStack() as stack:
        if args.android_run_emulator:
            avd_name = "foundry_local_android"
            system_image = f"system-images;android-{args.android_emulator_api};default;{args.android_abi}"

            android_tools.create_virtual_device(sdk_tool_paths, system_image, avd_name,
                                                sdk_root=args.android_home,
                                                java_home=args.java_home)
            emulator_proc = android_tools.start_emulator(
                sdk_tool_paths,
                avd_name,
                extra_args=["-partition-size", "2047", "-wipe-data"],
                sdk_root=args.android_home,
            )
            stack.callback(android_tools.stop_emulator, emulator_proc)

        # Collect shared libraries that need to be pushed alongside the test binary
        build_dir = args.build_dir
        shared_libs = list(build_dir.glob("*.so"))

        # Also include ORT/GenAI .so files that were copied to the build dir
        for pattern in ["libonnxruntime.so", "libonnxruntime-genai.so"]:
            shared_libs.extend(build_dir.glob(pattern))

        # Deduplicate
        shared_libs = list({lib.resolve(): lib for lib in shared_libs}.values())

        test_binary = build_dir / "bin" / "foundry_local_tests"
        test_data = build_dir / "bin" / "testdata"

        # Resolve test model cache the same way the C++ tests do (FOUNDRY_TEST_DATA_DIR env var)
        model_cache_env = os.environ.get("FOUNDRY_TEST_DATA_DIR")
        model_cache = Path(model_cache_env) if model_cache_env else None

        exit_code = android_tools.run_tests_on_device(
            sdk_tool_paths,
            test_binary,
            shared_libs,
            test_data_dir=test_data if test_data.exists() else None,
            model_cache_dir=model_cache if model_cache and model_cache.is_dir() else None,
        )

        if exit_code != 0:
            raise RuntimeError(f"Android tests failed with exit code {exit_code}")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    if not (is_windows() or is_linux() or is_mac()):
        raise OSError(f"Unsupported platform: {sys.platform}")

    arguments = _parse_args()
    _validate_args(arguments)

    if arguments.configure:
        configure(arguments)

    if arguments.clean:
        clean(arguments)

    if arguments.build:
        build(arguments)

    if arguments.test and not arguments.skip_tests:
        if arguments.android:
            android_test(arguments)
        else:
            test(arguments)
