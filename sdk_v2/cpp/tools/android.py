# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.

"""Android emulator management and test execution utilities.

Used by build.py to run native GTest binaries on an Android emulator.
"""

from __future__ import annotations

import collections
import contextlib
import datetime
import logging
import os
import signal
import subprocess
import sys
import time
from pathlib import Path

log = logging.getLogger("tools.android")

SdkToolPaths = collections.namedtuple("SdkToolPaths", ["emulator", "adb", "sdkmanager", "avdmanager"])


def _is_windows() -> bool:
    return sys.platform.startswith("win")


def _ext(name: str, windows_ext: str) -> str:
    """Append a platform-specific extension to a tool name."""
    return f"{name}.{windows_ext}" if _is_windows() else name


def validate_java_home(java_home: Path) -> None:
    """Validate that java_home points to a Java 17+ installation.

    Raises ValueError if the path is invalid or the version is too old.
    """
    java_bin = java_home / "bin" / (_ext("java", "exe"))
    if not java_bin.is_file():
        raise ValueError(f"JAVA_HOME does not contain a java binary: {java_bin}")

    try:
        output = subprocess.check_output(
            [str(java_bin), "-version"], stderr=subprocess.STDOUT, timeout=10
        ).decode()
    except (subprocess.CalledProcessError, OSError, subprocess.TimeoutExpired) as e:
        raise ValueError(f"Failed to run java -version at {java_bin}: {e}") from e

    # Parse version from output like: openjdk version "17.0.10" or java version "1.8.0_431"
    import re
    match = re.search(r'version "(\d+)(?:\.(\d+))?', output)
    if not match:
        raise ValueError(f"Could not parse Java version from: {output.strip()}")

    major = int(match.group(1))
    # Java 8 and earlier report as 1.x (e.g. "1.8.0_431" means Java 8)
    if major == 1 and match.group(2):
        major = int(match.group(2))
    if major < 17:
        raise ValueError(
            f"Java 17+ is required for Android SDK tools, found Java {major} at {java_home}. "
            f"Set --java_home or JAVA_HOME to a Java 17+ installation."
        )

    log.info("Using Java %d at: %s", major, java_home)


def _sdk_tool_env(java_home: Path | None) -> dict[str, str] | None:
    """Return an environment dict with JAVA_HOME set for cmdline-tools, or None."""
    if java_home:
        env = os.environ.copy()
        env["JAVA_HOME"] = str(java_home)
        return env
    return None


def get_sdk_tool_paths(sdk_root: Path) -> SdkToolPaths:
    """Resolve paths to Android SDK tools.

    emulator and adb are required; sdkmanager and avdmanager may be None
    if cmdline-tools are not installed (AVD creation falls back to manual
    config files in that case).
    """
    def _optional(path: Path) -> str | None:
        try:
            return str(path.resolve(strict=True))
        except (FileNotFoundError, OSError):
            return None

    return SdkToolPaths(
        emulator=str((sdk_root / "emulator" / _ext("emulator", "exe")).resolve(strict=True)),
        adb=str((sdk_root / "platform-tools" / _ext("adb", "exe")).resolve(strict=True)),
        sdkmanager=_optional(
            sdk_root / "cmdline-tools" / "latest" / "bin" / _ext("sdkmanager", "bat")
        ),
        avdmanager=_optional(
            sdk_root / "cmdline-tools" / "latest" / "bin" / _ext("avdmanager", "bat")
        ),
    )


def _create_avd_manually(
    sdk_root: Path,
    system_image: str,
    avd_name: str,
) -> None:
    """Create an AVD by writing config files directly.

    This bypasses avdmanager, which may fail due to Java version mismatches
    or SDK XML format incompatibilities.
    """
    # Parse system image package name: "system-images;android-XX;variant;abi"
    parts = system_image.split(";")
    if len(parts) != 4:
        raise ValueError(f"Unexpected system image format: {system_image}")

    api_name = parts[1]  # e.g. "android-29"
    abi = parts[3]       # e.g. "x86_64"

    # Verify the system image exists on disk
    image_dir = sdk_root / "system-images" / api_name / parts[2] / abi
    if not image_dir.is_dir():
        raise FileNotFoundError(
            f"System image not found at {image_dir}. "
            f"Install it with: sdkmanager --install \"{system_image}\""
        )

    avd_home = Path.home() / ".android" / "avd"
    avd_dir = avd_home / f"{avd_name}.avd"
    avd_ini = avd_home / f"{avd_name}.ini"

    avd_dir.mkdir(parents=True, exist_ok=True)

    # The .ini file that the emulator uses to locate the AVD
    avd_ini.write_text(
        f"avd.ini.encoding=UTF-8\n"
        f"path={avd_dir}\n"
        f"path.rel=avd\\{avd_name}.avd\n"
        f"target={api_name}\n",
        encoding="utf-8",
    )

    # Relative path from SDK root that the emulator expects
    image_rel = f"system-images/{api_name}/{parts[2]}/{abi}/"

    avd_config = avd_dir / "config.ini"
    avd_config.write_text(
        f"AvdId={avd_name}\n"
        f"PlayStore.enabled=false\n"
        f"abi.type={abi}\n"
        f"avd.ini.displayname={avd_name}\n"
        f"avd.ini.encoding=UTF-8\n"
        f"disk.dataPartition.size=2G\n"
        f"hw.cpu.arch={abi}\n"
        f"hw.cpu.ncore=4\n"
        f"hw.lcd.density=480\n"
        f"hw.lcd.height=1920\n"
        f"hw.lcd.width=1080\n"
        f"hw.ramSize=4096\n"
        f"hw.sdCard.present=no\n"
        f"image.sysdir.1={image_rel}\n"
        f"tag.display=Default\n"
        f"tag.id={parts[2]}\n",
        encoding="utf-8",
    )

    log.info("AVD '%s' created manually at %s", avd_name, avd_dir)


def create_virtual_device(
    sdk_tool_paths: SdkToolPaths,
    system_image: str,
    avd_name: str,
    sdk_root: Path | None = None,
    java_home: Path | None = None,
) -> None:
    """Install a system image and create an AVD.

    Tries avdmanager first. If it fails (Java version, SDK XML issues),
    falls back to manual config file creation.
    """
    # Try installing the system image via sdkmanager (best-effort)
    sdk_env = _sdk_tool_env(java_home)

    if sdk_tool_paths.sdkmanager:
        try:
            log.info("Installing system image: %s", system_image)
            subprocess.run(
                [sdk_tool_paths.sdkmanager, "--install", system_image],
                input=b"y",
                check=True,
                env=sdk_env,
            )
        except (subprocess.CalledProcessError, OSError) as e:
            log.warning("sdkmanager install failed (%s); assuming image is already installed.", e)

    # Try avdmanager first
    if sdk_tool_paths.avdmanager:
        try:
            log.info("Creating AVD via avdmanager: %s", avd_name)
            subprocess.run(
                [
                    sdk_tool_paths.avdmanager, "create", "avd",
                    "--name", avd_name,
                    "--package", system_image,
                    "--force",
                ],
                input=b"no",
                check=True,
                env=sdk_env,
            )
            return
        except (subprocess.CalledProcessError, OSError) as e:
            log.warning("avdmanager failed (%s); falling back to manual AVD creation.", e)

    # Fallback: create config files directly
    if not sdk_root:
        raise RuntimeError(
            "avdmanager failed and sdk_root was not provided for manual AVD creation."
        )

    _create_avd_manually(sdk_root, system_image, avd_name)


# Process creation flags for clean subprocess management.
_CREATION_FLAGS = subprocess.CREATE_NEW_PROCESS_GROUP if _is_windows() else 0
_STOP_SIGNAL = signal.CTRL_BREAK_EVENT if _is_windows() else signal.SIGTERM


def _stop_process(proc: subprocess.Popen) -> None:
    """Send a graceful stop signal, then kill if the process doesn't exit."""
    if proc.returncode is not None:
        return

    log.debug("Stopping process: %s", proc.args)
    proc.send_signal(_STOP_SIGNAL)
    try:
        proc.wait(30)
    except subprocess.TimeoutExpired:
        log.warning("Timeout waiting for process to exit, killing.")
        proc.kill()


def start_emulator(
    sdk_tool_paths: SdkToolPaths,
    avd_name: str,
    extra_args: list[str] | None = None,
    sdk_root: Path | None = None,
) -> subprocess.Popen:
    """Start an Android emulator and wait for it to boot.

    Returns the emulator subprocess. The caller is responsible for stopping it
    via stop_emulator().
    """
    emulator_args = [
        sdk_tool_paths.emulator,
        "-avd", avd_name,
        "-memory", "4096",
        "-no-snapstorage",
        "-no-audio",
        "-no-boot-anim",
        "-gpu", "swiftshader_indirect",
        "-delay-adb",
    ]

    # Headless on Linux (no display in CI). Windows/macOS can show a window.
    if sys.platform.startswith("linux"):
        emulator_args.append("-no-window")

    if extra_args:
        emulator_args += extra_args

    log.info("Starting emulator: %s", " ".join(emulator_args))

    # Set ANDROID_SDK_ROOT so the emulator can find system images
    emu_env = None
    if sdk_root:
        emu_env = os.environ.copy()
        emu_env["ANDROID_SDK_ROOT"] = str(sdk_root)

    emulator_proc = subprocess.Popen(emulator_args, creationflags=_CREATION_FLAGS, env=emu_env)

    try:
        _wait_for_boot(sdk_tool_paths)
    except Exception:
        _stop_process(emulator_proc)
        raise

    return emulator_proc


def _wait_for_boot(sdk_tool_paths: SdkToolPaths, timeout_minutes: int = 20) -> None:
    """Block until the emulator is fully booted."""
    sleep_seconds = 10
    deadline = datetime.datetime.now() + datetime.timedelta(minutes=timeout_minutes)

    # Phase 1: wait for adb connectivity
    log.info("Waiting for adb device...")
    waiter = subprocess.Popen(
        [sdk_tool_paths.adb, "wait-for-device", "shell", "ls", "/data/local/tmp"],
        creationflags=_CREATION_FLAGS,
    )
    try:
        while True:
            ret = waiter.poll()
            if ret is not None:
                if ret == 0:
                    break
                raise RuntimeError(f"adb wait-for-device exited with code {ret}")
            if datetime.datetime.now() > deadline:
                raise RuntimeError("Timeout waiting for adb device")
            time.sleep(sleep_seconds)
    finally:
        _stop_process(waiter)

    log.info("adb device connected. Waiting for boot_completed...")

    # Phase 2: wait for sys.boot_completed
    while True:
        try:
            output = subprocess.check_output(
                [sdk_tool_paths.adb, "shell", "getprop", "sys.boot_completed"],
                timeout=10,
            )
            if output.decode().strip() == "1":
                log.info("Emulator boot complete.")
                return
        except (subprocess.TimeoutExpired, subprocess.CalledProcessError):
            pass

        if datetime.datetime.now() > deadline:
            raise RuntimeError("Timeout waiting for sys.boot_completed")

        log.debug("sys.boot_completed not set yet, retrying in %ds...", sleep_seconds)
        time.sleep(sleep_seconds)


def stop_emulator(emulator_proc: subprocess.Popen) -> None:
    """Stop a running emulator."""
    log.info("Stopping emulator...")
    _stop_process(emulator_proc)


def run_tests_on_device(
    sdk_tool_paths: SdkToolPaths,
    test_binaries: list[Path],
    shared_libs: list[Path],
    device_dir: str = "/data/local/tmp/foundry_tests",
    test_data_dir: Path | None = None,
    model_cache_dir: Path | None = None,
) -> int:
    """Push native test binaries and their shared library dependencies to the device and run them.

    Args:
        sdk_tool_paths: Resolved SDK tool paths.
        test_binaries: Host test executables to push and run (e.g. foundry_local_tests,
            sdk_integration_tests).
        shared_libs: Shared libraries (.so) to push alongside the binaries.
        device_dir: Target directory on the device.
        test_data_dir: Optional host directory with test data to push.
        model_cache_dir: Optional host directory with test models (e.g. test-data-shared).
            Only subdirectories with "cpu" in their name are pushed (the emulator has no GPU).

    Returns:
        The first non-zero test exit code, or 0 if every binary passed.
    """
    adb = sdk_tool_paths.adb

    # Create target directory on device
    subprocess.run([adb, "shell", "mkdir", "-p", device_dir], check=True)

    # Push the test binaries
    for test_binary in test_binaries:
        log.info("Pushing test binary: %s", test_binary.name)
        subprocess.run([adb, "push", str(test_binary), f"{device_dir}/"], check=True)
        subprocess.run(
            [adb, "shell", "chmod", "755", f"{device_dir}/{test_binary.name}"],
            check=True,
        )

    # Push shared libraries
    for lib in shared_libs:
        if lib.exists():
            log.info("Pushing shared library: %s", lib.name)
            subprocess.run([adb, "push", str(lib), f"{device_dir}/"], check=True)

    # Push test data if provided
    if test_data_dir and test_data_dir.is_dir():
        log.info("Pushing test data: %s", test_data_dir)
        subprocess.run([adb, "push", str(test_data_dir), f"{device_dir}/testdata"], check=True)

    # Push test model cache if provided (large models for inference tests).
    # Only push model directories whose name contains "cpu" — the emulator has no GPU,
    # so gpu/cuda models are dead weight. Also skip "embedding" models (not yet supported
    # on Android). This naturally skips non-model dirs like .config too.
    device_model_cache = f"{device_dir}/test-model-cache"
    pushed_models = False
    if model_cache_dir and model_cache_dir.is_dir():
        for model_dir in sorted(model_cache_dir.iterdir()):
            if not model_dir.is_dir() or "cpu" not in model_dir.name or "embedding" in model_dir.name:
                continue

            if not pushed_models:
                subprocess.run([adb, "shell", "mkdir", "-p", device_model_cache], check=True)
                pushed_models = True

            log.info("Pushing test model '%s' (this may take a while)", model_dir.name)
            subprocess.run(
                [adb, "push", str(model_dir), f"{device_model_cache}/"],
                check=True,
            )

    # Build environment variables for the test process. HOME must point at a writable location
    env_vars = f"HOME={device_dir} LD_LIBRARY_PATH={device_dir}"
    if pushed_models:
        env_vars += f" FOUNDRY_TEST_DATA_DIR={device_model_cache}"

    # Build a CA certificate bundle from Android system certs for OpenSSL/libcurl.
    # Statically-linked OpenSSL ignores SSL_CERT_DIR at runtime (the --openssldir
    # baked at build time wins), but SSL_CERT_FILE with a concatenated PEM bundle works.
    # API 34+ moved certs to the Conscrypt APEX module; older versions use /system.
    ssl_cert_file = f"{device_dir}/cacert.pem"
    cert_source = None
    for cert_path in ["/apex/com.android.conscrypt/cacerts", "/system/etc/security/cacerts"]:
        result = subprocess.run(
            [adb, "shell", f"test -d {cert_path} && echo exists"],
            capture_output=True, text=True,
        )
        if "exists" in result.stdout:
            cert_source = cert_path
            break

    if cert_source:
        log.info("Building CA bundle from %s", cert_source)
        subprocess.run(
            [adb, "shell", f"cat {cert_source}/*.0 > {ssl_cert_file}"],
            check=True,
        )
        env_vars += f" SSL_CERT_FILE={ssl_cert_file}"
    else:
        log.warning("No system CA certificate directory found; TLS connections may fail.")

    # Clear logcat before running tests so we only capture relevant output.
    subprocess.run([adb, "logcat", "-c"], check=False)

    # Run each test binary in turn, run them all and fail the run if any binary fails.
    returncode = 0
    for test_binary in test_binaries:
        cmd = f"cd {device_dir} && {env_vars} ./{test_binary.name} --gtest_color=yes"
        log.info("Running tests on device: %s", cmd)
        result = subprocess.run([adb, "shell", cmd])
        returncode = returncode or result.returncode

    # Capture logcat output from the SDK (tag: foundry_local) and any crash/abort diagnostics.
    log.info("Capturing logcat output...")
    logcat_result = subprocess.run(
        [adb, "logcat", "-d",
         "-s", "foundry_local:*", "DEBUG:*", "libc:*"],
        capture_output=True, text=True,
    )
    if logcat_result.stdout.strip():
        log.info("=== logcat (foundry_local / crash) ===\n%s", logcat_result.stdout)
    else:
        log.info("No logcat output from foundry_local tag.")

    return returncode
