use std::collections::HashMap;
use std::env;
use std::fs;
use std::io::{self, Read};
use std::path::{Path, PathBuf};
use std::sync::Mutex;

/// Feeds tried in order. Primary: nuget.org (stable releases). Fallback:
/// the public ORT-Nightly Azure DevOps NuGet feed (where dev / pre-release
/// builds of Foundry Local Core, ONNX Runtime and ONNX Runtime GenAI live
/// before they reach nuget.org). If a download from a feed fails for any
/// reason, the next feed is tried.
const FEEDS: &[&str] = &[
    "https://api.nuget.org/v3/index.json",
    "https://pkgs.dev.azure.com/aiinfra/PublicPackages/_packaging/ORT-Nightly/nuget/v3/index.json",
];

/// Versions loaded from deps_versions.json (or deps_versions_winml.json).
/// Both files share common keys — the build script picks the
/// right file based on the winml cargo feature.
struct DepsVersions {
    core: String,
    winml_runtime: Option<String>,
    ort: String,
    genai: String,
}

fn load_deps_versions() -> DepsVersions {
    let winml = env::var("CARGO_FEATURE_WINML").is_ok();
    let manifest_dir = env::var("CARGO_MANIFEST_DIR").unwrap_or_default();
    let manifest_path = Path::new(&manifest_dir);

    // Standard and WinML each have their own versions file.
    let filename = if winml {
        "deps_versions_winml.json"
    } else {
        "deps_versions.json"
    };

    // Check manifest dir first (packaged crate), then parent (repo layout)
    let json_path = if manifest_path.join(filename).exists() {
        manifest_path.join(filename)
    } else {
        manifest_path.join("..").join(filename)
    };

    // Tell Cargo to rebuild if the versions file changes
    println!(
        "cargo:rerun-if-changed={}",
        json_path
            .canonicalize()
            .unwrap_or(json_path.clone())
            .display()
    );

    let content = fs::read_to_string(&json_path).expect("Failed to read deps_versions.json");
    // Strip UTF-8 BOM if present (PowerShell may write files with BOM)
    let stripped_content = content.strip_prefix('\u{FEFF}').unwrap_or(&content);
    let val: serde_json::Value =
        serde_json::from_str(stripped_content).expect("Failed to parse deps_versions.json");

    let s = |obj: &serde_json::Value, key: &str| -> String {
        obj.get(key)
            .and_then(|v| v.as_str())
            .unwrap_or("")
            .to_string()
    };
    let flc = &val["foundry-local-core"];
    let winml_runtime = &val["windows-ai-machinelearning"];
    let ort = &val["onnxruntime"];
    let genai = &val["onnxruntime-genai"];
    DepsVersions {
        core: s(flc, "nuget"),
        winml_runtime: env::var("FOUNDRY_LOCAL_WINDOWS_AI_MACHINELEARNING_VERSION")
            .ok()
            .filter(|v| !v.trim().is_empty())
            .or_else(|| {
                winml_runtime
                    .get("version")
                    .and_then(|v| v.as_str())
                    .map(|v| v.to_string())
            }),
        ort: s(ort, "version"),
        genai: s(genai, "version"),
    }
}

struct NuGetPackage {
    name: &'static str,
    version: String,
    expected_file: String,
    include_files: &'static [&'static str],
    always_extract: bool,
}

const ALL_NATIVE_FILES: &[&str] = &[];
const WINML_RUNTIME_FILES: &[&str] = &["Microsoft.Windows.AI.MachineLearning.dll"];

fn get_rid() -> Option<&'static str> {
    let os = env::consts::OS;
    let arch = env::consts::ARCH;
    match (os, arch) {
        ("windows", "x86_64") => Some("win-x64"),
        ("windows", "aarch64") => Some("win-arm64"),
        ("linux", "x86_64") => Some("linux-x64"),
        ("linux", "aarch64") => Some("linux-arm64"),
        ("macos", "aarch64") => Some("osx-arm64"),
        _ => None,
    }
}

fn native_lib_extension() -> &'static str {
    match env::consts::OS {
        "windows" => "dll",
        "linux" => "so",
        "macos" => "dylib",
        _ => "so",
    }
}

fn native_lib_prefix() -> &'static str {
    if env::consts::OS == "windows" {
        ""
    } else {
        "lib"
    }
}

fn get_packages(rid: &str) -> Vec<NuGetPackage> {
    let winml = env::var("CARGO_FEATURE_WINML").is_ok();
    // Microsoft.ML.OnnxRuntime.Gpu.Linux only ships x86_64 native binaries, so use it
    // only for linux-x64. For linux-arm64 fall through to the Foundry package which
    // provides the arm64 ORT runtime.
    let is_linux_x64 = rid == "linux-x64";
    let deps = load_deps_versions();
    let ext = native_lib_extension();
    let prefix = native_lib_prefix();

    let core_file = format!("Microsoft.AI.Foundry.Local.Core.{ext}");
    let ort_file = format!("{prefix}onnxruntime.{ext}");
    let genai_file = format!("{prefix}onnxruntime-genai.{ext}");
    let winml_runtime_file = "Microsoft.Windows.AI.MachineLearning.dll".to_string();

    // Use pinned versions directly — dynamic resolution via resolve_latest_version
    // is unreliable (feed returns versions in unexpected order, and some old versions
    // require authentication).

    let mut packages = Vec::new();

    if winml {
        packages.push(NuGetPackage {
            name: "Microsoft.AI.Foundry.Local.Core.WinML",
            version: deps.core.clone(),
            expected_file: core_file.clone(),
            include_files: ALL_NATIVE_FILES,
            always_extract: false,
        });
        packages.push(NuGetPackage {
            name: "Microsoft.ML.OnnxRuntime.Foundry",
            version: deps.ort.clone(),
            expected_file: ort_file.clone(),
            include_files: ALL_NATIVE_FILES,
            always_extract: false,
        });
        packages.push(NuGetPackage {
            name: "Microsoft.ML.OnnxRuntimeGenAI.Foundry",
            version: deps.genai.clone(),
            expected_file: genai_file.clone(),
            include_files: ALL_NATIVE_FILES,
            always_extract: false,
        });
        if rid.starts_with("win-") {
            let winml_runtime = deps
                .winml_runtime
                .clone()
                .expect("deps_versions_winml.json is missing windows-ai-machinelearning.version");
            packages.push(NuGetPackage {
                name: "Microsoft.Windows.AI.MachineLearning",
                version: winml_runtime,
                expected_file: winml_runtime_file,
                include_files: WINML_RUNTIME_FILES,
                always_extract: true,
            });
        }
    } else {
        packages.push(NuGetPackage {
            name: "Microsoft.AI.Foundry.Local.Core",
            version: deps.core.clone(),
            expected_file: core_file,
            include_files: ALL_NATIVE_FILES,
            always_extract: false,
        });

        if is_linux_x64 {
            packages.push(NuGetPackage {
                name: "Microsoft.ML.OnnxRuntime.Gpu.Linux",
                version: deps.ort.clone(),
                expected_file: ort_file.clone(),
                include_files: ALL_NATIVE_FILES,
                always_extract: false,
            });
        } else {
            packages.push(NuGetPackage {
                name: "Microsoft.ML.OnnxRuntime.Foundry",
                version: deps.ort.clone(),
                expected_file: ort_file.clone(),
                include_files: ALL_NATIVE_FILES,
                always_extract: false,
            });
        }

        packages.push(NuGetPackage {
            name: "Microsoft.ML.OnnxRuntimeGenAI.Foundry",
            version: deps.genai.clone(),
            expected_file: genai_file,
            include_files: ALL_NATIVE_FILES,
            always_extract: false,
        });
    }

    packages
}

/// Resolve the PackageBaseAddress from a NuGet v3 service index. The result
/// is cached per feed URL so repeated calls within a single build (e.g. one
/// per package, plus retries on fallback feeds) only hit the network once.
fn resolve_base_address(feed_url: &str) -> Result<String, String> {
    static BASE_ADDRESS_CACHE: Mutex<Option<HashMap<String, String>>> = Mutex::new(None);
    {
        let guard = BASE_ADDRESS_CACHE.lock().unwrap();
        if let Some(map) = guard.as_ref() {
            if let Some(cached) = map.get(feed_url) {
                return Ok(cached.clone());
            }
        }
    }

    let body: String = ureq::get(feed_url)
        .call()
        .map_err(|e| format!("Failed to fetch NuGet feed index at {feed_url}: {e}"))?
        .body_mut()
        .read_to_string()
        .map_err(|e| format!("Failed to read feed index response: {e}"))?;

    let index: serde_json::Value =
        serde_json::from_str(&body).map_err(|e| format!("Failed to parse feed index JSON: {e}"))?;

    let resources = index["resources"]
        .as_array()
        .ok_or("Feed index missing 'resources' array")?;

    for resource in resources {
        let rtype = resource["@type"].as_str().unwrap_or("");
        if rtype == "PackageBaseAddress/3.0.0" {
            if let Some(id) = resource["@id"].as_str() {
                let base = if id.ends_with('/') {
                    id.to_string()
                } else {
                    format!("{id}/")
                };
                let mut guard = BASE_ADDRESS_CACHE.lock().unwrap();
                guard
                    .get_or_insert_with(HashMap::new)
                    .insert(feed_url.to_string(), base.clone());
                return Ok(base);
            }
        }
    }

    Err(format!(
        "Could not find PackageBaseAddress/3.0.0 in feed {feed_url}"
    ))
}

/// Try to download and extract a single package from a specific feed. Returns
/// `Ok(())` on success, `Err(reason)` on any failure (network, HTTP error,
/// zip parse error, etc.).
fn try_download_from_feed(
    pkg: &NuGetPackage,
    rid: &str,
    out_dir: &Path,
    feed_url: &str,
) -> Result<(), String> {
    let base_address = resolve_base_address(feed_url)?;
    let lower_name = pkg.name.to_lowercase();
    let lower_version = pkg.version.to_lowercase();
    let url =
        format!("{base_address}{lower_name}/{lower_version}/{lower_name}.{lower_version}.nupkg");

    let feed_host = feed_url
        .split("://")
        .nth(1)
        .and_then(|s| s.split('/').next())
        .unwrap_or(feed_url);

    println!(
        "cargo:warning=Downloading {name} {ver} from {host}",
        name = pkg.name,
        ver = pkg.version,
        host = feed_host,
    );

    let mut response = ureq::get(&url)
        .call()
        .map_err(|e| format!("Failed to download {} from {feed_host}: {e}", pkg.name))?;

    let mut bytes = Vec::new();
    response
        .body_mut()
        .as_reader()
        .read_to_end(&mut bytes)
        .map_err(|e| format!("Failed to read response body for {}: {e}", pkg.name))?;

    let ext = native_lib_extension();
    let native_prefix = format!("runtimes/{rid}/native/");
    let runtime_prefix = format!("runtimes/{rid}/");

    let cursor = io::Cursor::new(&bytes);
    let mut archive = zip::ZipArchive::new(cursor)
        .map_err(|e| format!("Failed to open nupkg as zip for {}: {e}", pkg.name))?;

    let mut extracted = 0usize;
    for i in 0..archive.len() {
        let mut file = archive
            .by_index(i)
            .map_err(|e| format!("Failed to read zip entry: {e}"))?;

        let name = file.name().to_string();
        if !name.ends_with(&format!(".{ext}")) {
            continue;
        }

        let direct_runtime_file = name
            .strip_prefix(&runtime_prefix)
            .map(|relative| !relative.is_empty() && !relative.contains('/'))
            .unwrap_or(false);
        if !name.starts_with(&native_prefix) && !direct_runtime_file {
            continue;
        }

        let file_name = Path::new(&name)
            .file_name()
            .map(|n| n.to_string_lossy().to_string())
            .unwrap_or_default();

        if file_name.is_empty() {
            continue;
        }

        if !pkg.include_files.is_empty()
            && !pkg
                .include_files
                .iter()
                .any(|included| file_name.eq_ignore_ascii_case(included))
        {
            continue;
        }

        let dest = out_dir.join(&file_name);
        let mut out_file = fs::File::create(&dest)
            .map_err(|e| format!("Failed to create {}: {e}", dest.display()))?;
        io::copy(&mut file, &mut out_file)
            .map_err(|e| format!("Failed to write {}: {e}", dest.display()))?;

        println!("cargo:warning=  Extracted {file_name}");
        extracted += 1;
    }

    if extracted == 0 {
        println!(
            "cargo:warning=  No native libraries found for RID '{rid}' in {} {}",
            pkg.name, pkg.version
        );
    }

    Ok(())
}

/// Download a .nupkg and extract native libraries for the given RID into `out_dir`.
/// Skips download if native files from this package are already present.
/// Tries each configured feed in order; on failure falls back to the next.
fn download_and_extract(pkg: &NuGetPackage, rid: &str, out_dir: &Path) -> Result<(), String> {
    // Skip if this package's main native library is already in out_dir
    // (e.g. pre-populated from FOUNDRY_NATIVE_OVERRIDE_DIR).
    if !pkg.always_extract && out_dir.join(&pkg.expected_file).exists() {
        println!(
            "cargo:warning={} already present, skipping download.",
            pkg.name
        );
        return Ok(());
    }

    let mut last_error = String::new();
    for (i, feed_url) in FEEDS.iter().enumerate() {
        match try_download_from_feed(pkg, rid, out_dir, feed_url) {
            Ok(()) => return Ok(()),
            Err(e) => {
                let is_last = i == FEEDS.len() - 1;
                if !is_last {
                    println!(
                        "cargo:warning={} {}: {e}; trying next feed...",
                        pkg.name, pkg.version
                    );
                }
                last_error = e;
            }
        }
    }
    Err(format!(
        "Failed to download {} {} from any configured feed: {last_error}",
        pkg.name, pkg.version
    ))
}

/// Check whether all required native libraries are already present in `out_dir`.
fn libs_already_present(packages: &[NuGetPackage], out_dir: &Path) -> bool {
    packages
        .iter()
        .all(|pkg| out_dir.join(&pkg.expected_file).exists())
}

fn remove_unneeded_winml_runtime_files(out_dir: &Path) {
    if env::var("CARGO_FEATURE_WINML").is_err() || env::consts::OS != "windows" {
        return;
    }

    let directml = out_dir.join("DirectML.dll");
    if directml.exists() {
        match fs::remove_file(&directml) {
            Ok(()) => println!("cargo:warning=Removed unneeded DirectML.dll from OUT_DIR"),
            // Best-effort cleanup: a locked DirectML.dll (common on Windows when a
            // previous build's process still holds a handle) shouldn't fail the
            // entire build script, since the DLL is unused under WinML 2.0.
            Err(e) => {
                println!("cargo:warning=Could not remove unneeded DirectML.dll from OUT_DIR: {e}")
            }
        }
    }
}

fn main() {
    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-env-changed=FOUNDRY_NATIVE_OVERRIDE_DIR");
    println!("cargo:rerun-if-env-changed=FOUNDRY_LOCAL_WINDOWS_AI_MACHINELEARNING_VERSION");
    println!("cargo:rerun-if-env-changed=CARGO_FEATURE_WINML");

    let out_dir = PathBuf::from(env::var("OUT_DIR").expect("OUT_DIR not set"));

    let rid = match get_rid() {
        Some(r) => r,
        None => {
            println!(
                "cargo:warning=Unsupported platform: {} {}. Native libraries will not be downloaded.",
                env::consts::OS,
                env::consts::ARCH,
            );
            return;
        }
    };

    // If FOUNDRY_NATIVE_OVERRIDE_DIR is set (e.g. by CI), copy native
    // libraries from that directory into OUT_DIR. This pre-populates FLC Core
    // binaries that aren't published to a feed yet. The download loop below
    // will then only fetch packages whose files are still missing (ORT, GenAI).
    if let Ok(override_dir) = env::var("FOUNDRY_NATIVE_OVERRIDE_DIR") {
        let src = Path::new(&override_dir);
        if src.is_dir() {
            let ext = native_lib_extension();
            for entry in fs::read_dir(src).expect("Failed to read FOUNDRY_NATIVE_OVERRIDE_DIR") {
                let path = entry.expect("Failed to read dir entry").path();
                if path.extension().and_then(|e| e.to_str()) == Some(ext) {
                    let dest = out_dir.join(path.file_name().unwrap());
                    if env::var("CARGO_FEATURE_WINML").is_ok()
                        && env::consts::OS == "windows"
                        && path
                            .file_name()
                            .and_then(|n| n.to_str())
                            .map(|n| n.eq_ignore_ascii_case("DirectML.dll"))
                            .unwrap_or(false)
                    {
                        println!("cargo:warning=Skipped unneeded DirectML.dll from override dir");
                        continue;
                    }

                    fs::copy(&path, &dest).expect("Failed to copy native lib from override dir");
                    println!(
                        "cargo:warning=Copied {} from override dir",
                        path.file_name().unwrap().to_string_lossy()
                    );
                }
            }
        }
    }

    let packages = get_packages(rid);
    let packages_require_extraction = packages.iter().any(|pkg| pkg.always_extract);

    // Skip all downloads if every required library is already present.
    // WinML packages that overwrite stale runtime files still need to run.
    if !packages_require_extraction && libs_already_present(&packages, &out_dir) {
        println!("cargo:warning=Native libraries already present in OUT_DIR, skipping download.");
        println!("cargo:rustc-link-search=native={}", out_dir.display());
        println!("cargo:rustc-env=FOUNDRY_NATIVE_DIR={}", out_dir.display());
        #[cfg(windows)]
        println!("cargo:rustc-link-lib=kernel32");
        return;
    }

    let mut download_failed = false;
    for pkg in &packages {
        if let Err(e) = download_and_extract(pkg, rid, &out_dir) {
            println!("cargo:warning=Error downloading {}: {e}", pkg.name);
            download_failed = true;
        }
    }

    remove_unneeded_winml_runtime_files(&out_dir);

    if download_failed && !libs_already_present(&packages, &out_dir) {
        panic!(
            "One or more native library downloads failed and required libraries are missing. \
             You can manually place native libraries in the output directory: {}",
            out_dir.display()
        );
    }

    println!("cargo:rustc-link-search=native={}", out_dir.display());
    println!("cargo:rustc-env=FOUNDRY_NATIVE_DIR={}", out_dir.display());

    // LocalFree (used to free native-allocated buffers) lives in kernel32.lib on Windows.
    #[cfg(windows)]
    println!("cargo:rustc-link-lib=kernel32");
}
