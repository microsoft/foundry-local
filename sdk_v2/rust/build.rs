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

use std::env;
use std::fs;
use std::io::{self, Read};
use std::path::{Path, PathBuf};

const FEEDS: &[&str] = &[
    "https://api.nuget.org/v3/index.json",
    "https://pkgs.dev.azure.com/aiinfra/PublicPackages/_packaging/ORT-Nightly/nuget/v3/index.json",
];

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

fn resolve_base_address(feed_url: &str) -> Result<String, String> {
    let body: String = ureq::get(feed_url)
        .call()
        .map_err(|e| format!("fetch feed index {feed_url}: {e}"))?
        .body_mut()
        .read_to_string()
        .map_err(|e| format!("read feed index: {e}"))?;
    let index: serde_json::Value =
        serde_json::from_str(&body).map_err(|e| format!("parse feed index: {e}"))?;
    for resource in index["resources"].as_array().ok_or("missing resources")? {
        if resource["@type"].as_str() == Some("PackageBaseAddress/3.0.0") {
            if let Some(id) = resource["@id"].as_str() {
                return Ok(if id.ends_with('/') {
                    id.to_string()
                } else {
                    format!("{id}/")
                });
            }
        }
    }
    Err(format!("no PackageBaseAddress in {feed_url}"))
}

fn try_download(
    pkg: &NuGetPackage,
    rid: &str,
    out_dir: &Path,
    feed_url: &str,
) -> Result<usize, String> {
    let base = resolve_base_address(feed_url)?;
    let name = pkg.name.to_lowercase();
    let version = pkg.version.to_lowercase();
    let url = format!("{base}{name}/{version}/{name}.{version}.nupkg");
    println!("cargo:warning=Downloading {} {}", pkg.name, pkg.version);

    let mut response = ureq::get(&url)
        .call()
        .map_err(|e| format!("download {}: {e}", pkg.name))?;
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

    let ext = native_lib_extension();
    let native_prefix = format!("runtimes/{rid}/native/");
    let runtime_prefix = format!("runtimes/{rid}/");
    let mut archive = zip::ZipArchive::new(io::Cursor::new(&bytes))
        .map_err(|e| format!("open nupkg {}: {e}", pkg.name))?;

    let mut extracted = 0usize;
    for i in 0..archive.len() {
        let mut file = archive.by_index(i).map_err(|e| format!("zip entry: {e}"))?;
        let entry = file.name().to_string();
        if !entry.ends_with(&format!(".{ext}")) {
            continue;
        }
        let direct = entry
            .strip_prefix(&runtime_prefix)
            .map(|r| !r.is_empty() && !r.contains('/'))
            .unwrap_or(false);
        if !entry.starts_with(&native_prefix) && !direct {
            continue;
        }
        let file_name = match Path::new(&entry).file_name() {
            Some(n) => n.to_string_lossy().to_string(),
            None => continue,
        };
        let dest = out_dir.join(&file_name);
        let mut out =
            fs::File::create(&dest).map_err(|e| format!("create {}: {e}", dest.display()))?;
        io::copy(&mut file, &mut out).map_err(|e| format!("write {}: {e}", dest.display()))?;
        println!("cargo:warning=  Extracted {file_name}");
        extracted += 1;
    }
    Ok(extracted)
}

fn download_and_extract(pkg: &NuGetPackage, rid: &str, out_dir: &Path) -> Result<(), String> {
    if out_dir.join(&pkg.expected_file).exists() {
        return Ok(());
    }
    if pkg.version.trim().is_empty() {
        return Err(format!("no version configured for {}", pkg.name));
    }
    let mut last = String::new();
    for feed in FEEDS {
        match try_download(pkg, rid, out_dir, feed) {
            // Integrity guard: a "successful" download must actually yield the
            // expected native binary (the archive opened as a valid zip and the
            // expected file landed on disk). Cryptographic SHA-512 verification
            // against the feed's published packageHash is feed-specific — the
            // registration layout differs between nuget.org and the Azure DevOps
            // feed — and is left as future hardening.
            Ok(_) if out_dir.join(&pkg.expected_file).exists() => return Ok(()),
            Ok(_) => {
                last = format!(
                    "{} {} downloaded but did not contain expected file {}",
                    pkg.name, pkg.version, pkg.expected_file
                )
            }
            Err(e) => last = e,
        }
    }
    Err(format!(
        "download {} {} failed: {last}",
        pkg.name, pkg.version
    ))
}

fn copy_from_local_dir(src: &Path, out_dir: &Path) -> bool {
    let ext = native_lib_extension();
    let Ok(entries) = fs::read_dir(src) else {
        return false;
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
    copied
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

    let manifest_dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap_or_default());
    let out_dir = PathBuf::from(env::var("OUT_DIR").expect("OUT_DIR not set"));

    // Guard: the crate-local deps_versions.json (what ships in the published
    // crate) must not drift from the canonical sdk_v2/deps_versions.json.
    assert_deps_versions_in_sync(&manifest_dir);

    // 1. Local C++ build output (dev path).
    if let Ok(local) = env::var("FOUNDRY_LOCAL_NATIVE_BIN_DIR") {
        let src = Path::new(&local);
        if src.is_dir() && copy_from_local_dir(src, &out_dir) {
            emit_native_dir(&out_dir);
            return;
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
        let mut failed = false;
        for pkg in &packages {
            if let Err(e) = download_and_extract(pkg, rid, &out_dir) {
                println!("cargo:warning={e}");
                failed = true;
            }
        }
        if !failed {
            emit_native_dir(&out_dir);
        }
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
