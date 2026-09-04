//! Build script for the Foundry Local v2 Rust SDK.
//!
//! Obtains the native `foundry_local` library (plus ONNX Runtime + GenAI) and
//! makes it discoverable at runtime via the `FOUNDRY_NATIVE_DIR` compile-time
//! env that `detail::api` consults.
//!
//! Native acquisition order:
//!   1. `FOUNDRY_LOCAL_NATIVE_BIN_DIR` — copy native files from a local C++ build
//!      (the dev path, mirroring the C# `FoundryLocalNativeBinDir`).
//!   2. `FOUNDRY_LOCAL_RUNTIME_VERSION` — download the Runtime NuGet package
//!      (`Microsoft.AI.Foundry.Local.Runtime`) plus ORT/GenAI for the RID.
//!   3. Otherwise no-op: the library is resolved at runtime from
//!      `FOUNDRY_LOCAL_LIB_DIR`, next to the executable, or the system path.

use std::collections::HashMap;
use std::env;
use std::fs;
use std::io::{self, Read};
use std::path::{Path, PathBuf};
use std::process::Command;

mod build_support;
mod http_extract;

use build_support::{NuGetConfig, NuGetMode};

struct DepsVersions {
    ort: String,
    genai: String,
}

fn load_deps_versions(manifest_dir: &Path) -> DepsVersions {
    let candidates = [
        manifest_dir.join("deps_versions.json"),
        manifest_dir.join("..").join("deps_versions.json"),
    ];
    let json_path = candidates
        .iter()
        .find(|p| p.exists())
        .cloned()
        .unwrap_or_else(|| candidates[0].clone());
    println!("cargo:rerun-if-changed={}", json_path.display());

    let content = fs::read_to_string(&json_path).unwrap_or_default();
    let stripped = content.strip_prefix('\u{FEFF}').unwrap_or(&content);
    let val: serde_json::Value = serde_json::from_str(stripped).unwrap_or(serde_json::Value::Null);
    let s = |key: &str| -> String {
        val.get(key)
            .and_then(|o| o.get("version"))
            .and_then(|v| v.as_str())
            .unwrap_or("")
            .to_string()
    };
    DepsVersions {
        ort: s("onnxruntime"),
        genai: s("onnxruntime-genai"),
    }
}

/// Fail the build if the crate-local `deps_versions.json` has drifted from the
/// canonical `sdk_v2/deps_versions.json`.
///
/// The crate copy is what ships in the published crate (Cargo cannot `include`
/// files outside the package root), while the canonical file is the single
/// source of truth in the source tree. When both are present — i.e. an in-repo
/// build — their version pins must match. When the canonical parent is absent
/// (a build from the published crate), there is nothing to check.
fn assert_deps_versions_in_sync(manifest_dir: &Path) {
    let crate_copy = manifest_dir.join("deps_versions.json");
    let canonical = manifest_dir.join("..").join("deps_versions.json");
    println!("cargo:rerun-if-changed={}", canonical.display());

    let (Ok(local), Ok(parent)) = (
        fs::read_to_string(&crate_copy),
        fs::read_to_string(&canonical),
    ) else {
        return; // published crate: canonical parent absent — nothing to check.
    };

    // Compare version pins only, ignoring the free-form "_comment" field so the
    // two files may carry their own descriptive comments.
    let pins = |s: &str| -> serde_json::Value {
        let stripped = s.strip_prefix('\u{FEFF}').unwrap_or(s);
        let mut v: serde_json::Value =
            serde_json::from_str(stripped).unwrap_or(serde_json::Value::Null);
        if let Some(obj) = v.as_object_mut() {
            obj.remove("_comment");
        }
        v
    };

    assert!(
        pins(&local) == pins(&parent),
        "deps_versions.json drift: {} differs from canonical {}. \
         Re-copy the canonical file into the crate before publishing.",
        crate_copy.display(),
        canonical.display()
    );
}

/// Cargo's *target* OS (e.g. "windows", "linux", "macos"), not the build host.
/// Set by Cargo for build scripts, so it is correct under cross-compilation.
fn target_os() -> String {
    env::var("CARGO_CFG_TARGET_OS").unwrap_or_default()
}

/// Cargo's *target* architecture (e.g. "x86_64", "aarch64"), not the build host.
fn target_arch() -> String {
    env::var("CARGO_CFG_TARGET_ARCH").unwrap_or_default()
}

fn get_rid() -> Option<&'static str> {
    match (target_os().as_str(), target_arch().as_str()) {
        ("windows", "x86_64") => Some("win-x64"),
        ("windows", "aarch64") => Some("win-arm64"),
        ("linux", "x86_64") => Some("linux-x64"),
        ("linux", "aarch64") => Some("linux-arm64"),
        ("macos", "aarch64") => Some("osx-arm64"),
        ("macos", "x86_64") => Some("osx-x64"),
        _ => None,
    }
}

fn native_lib_extension() -> &'static str {
    match target_os().as_str() {
        "windows" => "dll",
        "macos" => "dylib",
        _ => "so",
    }
}

struct NuGetPackage {
    name: String,
    version: String,
    expected_file: String,
}

fn get_packages(deps: &DepsVersions, runtime_version: &str) -> Vec<NuGetPackage> {
    let ext = native_lib_extension();
    let prefix = if target_os() == "windows" { "" } else { "lib" };

    // Single unified runtime package. On Windows it bundles the reg-free WinML 2.x
    // runtime (Microsoft.Windows.AI.MachineLearning.dll) next to foundry_local; there
    // is no separate .Runtime.WinML flavor. Kept consistent with the C#/JS/Python
    // bindings, which were unified onto this one package.
    vec![
        NuGetPackage {
            name: "Microsoft.AI.Foundry.Local.Runtime".to_string(),
            version: runtime_version.to_string(),
            expected_file: format!("{prefix}foundry_local.{ext}"),
        },
        NuGetPackage {
            name: "Microsoft.ML.OnnxRuntime".to_string(),
            version: deps.ort.clone(),
            expected_file: format!("{prefix}onnxruntime.{ext}"),
        },
        NuGetPackage {
            name: "Microsoft.ML.OnnxRuntimeGenAI.Foundry".to_string(),
            version: deps.genai.clone(),
            expected_file: format!("{prefix}onnxruntime-genai.{ext}"),
        },
    ]
}

fn resolve_base_address(
    agent: &ureq::Agent,
    feed_url: &str,
    cache: &mut HashMap<String, String>,
) -> Result<String, String> {
    if let Some(base) = cache.get(feed_url) {
        return Ok(base.clone());
    }
    let safe_feed_url = build_support::redact_url(feed_url);
    let body: String = agent
        .get(feed_url)
        .call()
        .map_err(|error| {
            build_support::redact_urls_in_text(&format!(
                "fetch feed index {safe_feed_url}: {error}"
            ))
        })?
        .body_mut()
        .read_to_string()
        .map_err(|e| format!("read feed index: {e}"))?;
    let index: serde_json::Value =
        serde_json::from_str(&body).map_err(|e| format!("parse feed index: {e}"))?;
    for resource in index["resources"].as_array().ok_or("missing resources")? {
        if resource["@type"].as_str() == Some("PackageBaseAddress/3.0.0") {
            if let Some(id) = resource["@id"].as_str() {
                build_support::validate_http_package_base_address(id)?;
                let base = if id.ends_with('/') {
                    id.to_string()
                } else {
                    format!("{id}/")
                };
                cache.insert(feed_url.to_string(), base.clone());
                return Ok(base);
            }
        }
    }
    Err(format!("no PackageBaseAddress in {safe_feed_url}"))
}

fn try_download(
    agent: &ureq::Agent,
    pkg: &NuGetPackage,
    rid: &str,
    staging_dir: &Path,
    feed_url: &str,
    service_index_cache: &mut HashMap<String, String>,
) -> Result<Vec<PathBuf>, String> {
    let base = resolve_base_address(agent, feed_url, service_index_cache)?;
    let name = pkg.name.to_lowercase();
    let version = pkg.version.to_lowercase();
    let url = format!("{base}{name}/{version}/{name}.{version}.nupkg");
    println!("cargo:warning=Downloading {} {}", pkg.name, pkg.version);

    let mut response = agent.get(&url).call().map_err(|error| {
        build_support::redact_urls_in_text(&format!("download {}: {error}", pkg.name))
    })?;
    let mut bytes = Vec::new();
    response
        .body_mut()
        .as_reader()
        .read_to_end(&mut bytes)
        .map_err(|e| format!("read body {}: {e}", pkg.name))?;

    // Integrity guard: reject an empty/truncated payload before we try to treat
    // it as a nupkg archive.
    if bytes.is_empty() {
        return Err(format!("downloaded {} {} is empty", pkg.name, pkg.version));
    }

    http_extract::extract_package(
        io::Cursor::new(bytes),
        rid,
        native_lib_extension(),
        &pkg.expected_file,
        staging_dir,
    )
    .map_err(|error| format!("extract nupkg {}: {error}", pkg.name))
}

fn download_and_extract(
    agent: &ureq::Agent,
    pkg: &NuGetPackage,
    rid: &str,
    out_dir: &Path,
    feeds: &[String],
    service_index_cache: &mut HashMap<String, String>,
) -> Result<(), String> {
    if build_support::package_is_current(out_dir, &pkg.expected_file, &pkg.version) {
        return Ok(());
    }
    if pkg.version.trim().is_empty() {
        return Err(format!("no version configured for {}", pkg.name));
    }
    let staging_dir = out_dir.join(format!(".nuget-http-{}", pkg.expected_file));
    let mut last = String::new();
    for feed in feeds {
        match try_download(agent, pkg, rid, &staging_dir, feed, service_index_cache) {
            // Integrity guard: a "successful" download must actually yield the
            // expected native binary (the archive opened as a valid zip and the
            // expected file landed on disk). Cryptographic SHA-512 verification
            // against the feed's published packageHash is feed-specific — the
            // registration layout differs between nuget.org and the Azure DevOps
            // feed — and is left as future hardening.
            Ok(files) => {
                let stage_result = http_extract::stage_files(&files, out_dir);
                http_extract::cleanup_dir(&staging_dir);
                stage_result?;
                for file in files {
                    if let Some(file_name) = file.file_name() {
                        println!("cargo:warning=  Extracted {}", file_name.to_string_lossy());
                    }
                }
                build_support::record_package_version(out_dir, &pkg.expected_file, &pkg.version)?;
                return Ok(());
            }
            Err(error) => {
                http_extract::cleanup_dir(&staging_dir);
                last = error;
            }
        }
    }
    Err(format!(
        "download {} {} failed: {last}",
        pkg.name, pkg.version
    ))
}

fn missing_packages<'a>(packages: &'a [NuGetPackage], out_dir: &Path) -> Vec<&'a NuGetPackage> {
    packages
        .iter()
        .filter(|pkg| !build_support::package_is_current(out_dir, &pkg.expected_file, &pkg.version))
        .collect()
}

fn reset_temp_dir(path: &Path) -> Result<(), String> {
    match fs::remove_dir_all(path) {
        Ok(()) => {}
        Err(error) if error.kind() == io::ErrorKind::NotFound => {}
        Err(error) => {
            return Err(format!(
                "remove temporary directory {}: {error}",
                path.display()
            ))
        }
    }
    fs::create_dir_all(path)
        .map_err(|error| format!("create temporary directory {}: {error}", path.display()))
}

fn cleanup_temp_dir(path: &Path) {
    if let Err(error) = fs::remove_dir_all(path) {
        println!(
            "cargo:warning=Could not remove temporary directory {}: {error}",
            path.display()
        );
    }
}

fn stage_restored_package(
    pkg: &NuGetPackage,
    package_dir: &Path,
    rid: &str,
    out_dir: &Path,
) -> Result<(), String> {
    let files = build_support::collect_native_files(package_dir, rid, native_lib_extension())?;
    if files.is_empty() {
        return Err(format!(
            "No native files found for RID '{rid}' in {} {}.",
            pkg.name, pkg.version
        ));
    }
    if !build_support::contains_expected_file(&files, &pkg.expected_file) {
        return Err(format!(
            "{} {} did not contain expected file {} for RID '{rid}'.",
            pkg.name, pkg.version, pkg.expected_file
        ));
    }
    build_support::invalidate_package(out_dir, &pkg.expected_file)?;
    for file in files {
        let file_name = file
            .file_name()
            .ok_or_else(|| format!("native file has no file name: {}", file.display()))?;
        fs::copy(&file, out_dir.join(file_name))
            .map_err(|error| format!("stage {}: {error}", file.display()))?;
        println!("cargo:warning=  Staged {}", file_name.to_string_lossy());
    }
    if out_dir.join(&pkg.expected_file).exists() {
        build_support::record_package_version(out_dir, &pkg.expected_file, &pkg.version)
    } else {
        Err(format!(
            "{} {} restored but did not contain expected file {}",
            pkg.name, pkg.version, pkg.expected_file
        ))
    }
}

fn run_command(command: &str, args: &[std::ffi::OsString], operation: &str) -> Result<(), String> {
    let output = Command::new(command).args(args).output().map_err(|error| {
        if error.kind() == io::ErrorKind::NotFound {
            format!(
                "{operation} command not found: '{command}'. Install it or configure its \
                     FOUNDRY_LOCAL_*_COMMAND override."
            )
        } else {
            format!("start {operation}: {error}")
        }
    })?;
    if output.status.success() {
        return Ok(());
    }
    let stdout = String::from_utf8_lossy(&output.stdout);
    let stderr = String::from_utf8_lossy(&output.stderr);
    let detail = build_support::redact_urls_in_text(&format!("{stdout}\n{stderr}"));
    Err(format!(
        "{operation} failed (exit {}).\n{}",
        output
            .status
            .code()
            .map_or_else(|| "unknown".to_string(), |code| code.to_string()),
        detail.trim()
    ))
}

fn restore_with_dotnet(
    config: &NuGetConfig,
    packages: &[NuGetPackage],
    rid: &str,
    out_dir: &Path,
) -> Result<(), String> {
    let missing = missing_packages(packages, out_dir);
    if missing.is_empty() {
        return Ok(());
    }
    let temp_dir = out_dir.join("nuget-dotnet-restore");
    reset_temp_dir(&temp_dir)?;
    let result = (|| {
        let project_path = temp_dir.join("restore.csproj");
        let packages_dir = temp_dir.join("packages");
        fs::create_dir_all(&packages_dir)
            .map_err(|error| format!("create package directory: {error}"))?;
        let package_refs: Vec<_> = missing
            .iter()
            .map(|pkg| (pkg.name.as_str(), pkg.version.as_str()))
            .collect();
        fs::write(
            &project_path,
            build_support::generate_restore_project(&package_refs),
        )
        .map_err(|error| format!("write restore project: {error}"))?;
        let args = build_support::build_dotnet_restore_args(config, &project_path, &packages_dir);
        println!("cargo:warning=Running dotnet restore for native runtime packages");
        run_command(&config.dotnet_command, &args, "dotnet restore")?;
        for pkg in missing {
            let package_dir =
                build_support::find_dotnet_package_dir(&packages_dir, &pkg.name, &pkg.version)?;
            stage_restored_package(pkg, &package_dir, rid, out_dir)?;
        }
        Ok(())
    })();
    cleanup_temp_dir(&temp_dir);
    result
}

fn restore_with_nuget(
    config: &NuGetConfig,
    packages: &[NuGetPackage],
    rid: &str,
    out_dir: &Path,
) -> Result<(), String> {
    let missing = missing_packages(packages, out_dir);
    if missing.is_empty() {
        return Ok(());
    }
    let temp_dir = out_dir.join("nuget-cli-restore");
    reset_temp_dir(&temp_dir)?;
    let result = (|| {
        let packages_dir = temp_dir.join("packages");
        fs::create_dir_all(&packages_dir)
            .map_err(|error| format!("create package directory: {error}"))?;
        for pkg in missing {
            let args = build_support::build_nuget_install_args(
                config,
                &pkg.name,
                &pkg.version,
                &packages_dir,
            );
            println!(
                "cargo:warning=Running nuget install for {} {}",
                pkg.name, pkg.version
            );
            run_command(&config.nuget_command, &args, "nuget install")?;
            let package_dir =
                build_support::find_nuget_package_dir(&packages_dir, &pkg.name, &pkg.version)?;
            stage_restored_package(pkg, &package_dir, rid, out_dir)?;
        }
        Ok(())
    })();
    cleanup_temp_dir(&temp_dir);
    result
}

fn copy_from_local_dir(src: &Path, out_dir: &Path) -> Result<bool, String> {
    let ext = native_lib_extension();
    let Ok(entries) = fs::read_dir(src) else {
        return Ok(false);
    };
    let mut copied = false;
    for entry in entries.flatten() {
        let path = entry.path();
        if path.extension().and_then(|e| e.to_str()) == Some(ext) {
            if let Some(name) = path.file_name() {
                if fs::copy(&path, out_dir.join(name)).is_ok() {
                    println!(
                        "cargo:warning=Copied {} from FOUNDRY_LOCAL_NATIVE_BIN_DIR",
                        name.to_string_lossy()
                    );
                    copied = true;
                }
            }
        }
    }
    if copied {
        build_support::invalidate_package_version_markers(out_dir)?;
    }
    Ok(copied)
}

fn emit_native_dir(out_dir: &Path) {
    println!("cargo:rustc-link-search=native={}", out_dir.display());
    println!("cargo:rustc-env=FOUNDRY_NATIVE_DIR={}", out_dir.display());
    // Link kernel32 when *targeting* Windows (not merely when built on Windows),
    // so cross-compiled builds get the right platform libraries.
    if target_os() == "windows" {
        println!("cargo:rustc-link-lib=kernel32");
    }
}

fn main() {
    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-env-changed=FOUNDRY_LOCAL_NATIVE_BIN_DIR");
    println!("cargo:rerun-if-env-changed=FOUNDRY_LOCAL_RUNTIME_VERSION");
    println!("cargo:rerun-if-env-changed=FOUNDRY_LOCAL_NUGET_MODE");
    println!("cargo:rerun-if-env-changed=FOUNDRY_LOCAL_NUGET_FEEDS");
    println!("cargo:rerun-if-env-changed=FOUNDRY_LOCAL_NUGET_CONFIG");
    println!("cargo:rerun-if-env-changed=FOUNDRY_LOCAL_DOTNET_COMMAND");
    println!("cargo:rerun-if-env-changed=FOUNDRY_LOCAL_NUGET_COMMAND");

    // docs.rs builds run in an offline sandbox and only need the crate to
    // type-check — `cargo doc` never links or runs the native library. Skip all
    // native acquisition there so documentation always builds regardless of the
    // native-dependency env vars. (This crate's default path is already a no-op,
    // but the guard makes docs builds robust and self-documenting.)
    if env::var_os("DOCS_RS").is_some() {
        return;
    }

    let manifest_dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap_or_default());
    let out_dir = PathBuf::from(env::var("OUT_DIR").expect("OUT_DIR not set"));

    // Guard: the crate-local deps_versions.json (what ships in the published
    // crate) must not drift from the canonical sdk_v2/deps_versions.json.
    assert_deps_versions_in_sync(&manifest_dir);

    // 1. Local C++ build output (dev path).
    if let Ok(local) = env::var("FOUNDRY_LOCAL_NATIVE_BIN_DIR") {
        let src = Path::new(&local);
        if src.is_dir() {
            match copy_from_local_dir(src, &out_dir) {
                Ok(true) => {
                    emit_native_dir(&out_dir);
                    return;
                }
                Ok(false) => {}
                Err(error) => {
                    println!("cargo:warning=local native staging failed: {error}");
                    return;
                }
            }
        }
    }

    // 2. Runtime NuGet download (release path), only when a version is pinned.
    let runtime_version = env::var("FOUNDRY_LOCAL_RUNTIME_VERSION").unwrap_or_default();
    if !runtime_version.trim().is_empty() {
        let rid = match get_rid() {
            Some(r) => r,
            None => {
                println!(
                    "cargo:warning=Unsupported platform {} {}; skipping native download.",
                    target_os(),
                    target_arch()
                );
                return;
            }
        };
        let deps = load_deps_versions(&manifest_dir);
        let packages = get_packages(&deps, &runtime_version);
        let config = build_support::read_config()
            .unwrap_or_else(|error| panic!("invalid NuGet configuration: {error}"));
        if let Some(config_file) = &config.config_file {
            println!("cargo:rerun-if-changed={config_file}");
        }
        let package_versions: Vec<_> = packages
            .iter()
            .map(|pkg| (pkg.expected_file.as_str(), pkg.version.as_str()))
            .collect();
        let result = build_support::reset_native_cache_if_stale(
            &out_dir,
            &package_versions,
            native_lib_extension(),
        )
        .and_then(|_| match config.mode {
            NuGetMode::Http => {
                let mut failed = false;
                let mut service_index_cache = HashMap::new();
                let http_config = ureq::Agent::config_builder().https_only(true).build();
                let http_agent = ureq::Agent::new_with_config(http_config);
                for pkg in &packages {
                    if let Err(error) = download_and_extract(
                        &http_agent,
                        pkg,
                        rid,
                        &out_dir,
                        &config.feeds,
                        &mut service_index_cache,
                    ) {
                        println!("cargo:warning={error}");
                        failed = true;
                    }
                }
                if failed {
                    Err("one or more native runtime packages failed to download".to_string())
                } else {
                    Ok(())
                }
            }
            NuGetMode::Dotnet => restore_with_dotnet(&config, &packages, rid, &out_dir),
            NuGetMode::Nuget => restore_with_nuget(&config, &packages, rid, &out_dir),
        });
        if let Err(error) = result {
            println!("cargo:warning=native package acquisition failed: {error}");
            return;
        }
        emit_native_dir(&out_dir);
        return;
    }

    // 3. No build-time native configured. Runtime discovery handles loading:
    //    FOUNDRY_LOCAL_LIB_DIR, the executable's directory, or the system loader
    //    path (see detail::api::resolve_library_path). Stay quiet when the runtime
    //    override is set — the `FOUNDRY_LOCAL_LIB_DIR=... cargo run` workflow is a
    //    fully supported path and shouldn't trigger a build warning. Only hint when
    //    nothing is configured at build *or* run time.
    println!("cargo:rerun-if-env-changed=FOUNDRY_LOCAL_LIB_DIR");
    if env::var_os("FOUNDRY_LOCAL_LIB_DIR").is_none() {
        println!(
            "cargo:warning=foundry-local-sdk: no native library configured. Provide it at build time \
             via FOUNDRY_LOCAL_NATIVE_BIN_DIR (local C++ build) or FOUNDRY_LOCAL_RUNTIME_VERSION (NuGet), \
             or at runtime via FOUNDRY_LOCAL_LIB_DIR / by placing foundry_local on the loader search path."
        );
    }
}
