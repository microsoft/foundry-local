#[allow(dead_code)]
#[path = "../build_support.rs"]
mod build_support;

use std::collections::HashMap;
use std::ffi::OsString;
use std::fs;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicUsize, Ordering};

use build_support::{NuGetConfig, NuGetMode, DEFAULT_FEEDS};

fn config_from(values: &[(&str, &str)], is_windows: bool) -> Result<NuGetConfig, String> {
    let values: HashMap<_, _> = values
        .iter()
        .map(|(key, value)| ((*key).to_string(), (*value).to_string()))
        .collect();
    build_support::read_config_from(|name| values.get(name).cloned(), is_windows)
}

fn args_as_strings(args: Vec<OsString>) -> Vec<String> {
    args.into_iter()
        .map(|value| value.to_string_lossy().into_owned())
        .collect()
}

fn test_dir(name: &str) -> PathBuf {
    static NEXT_ID: AtomicUsize = AtomicUsize::new(0);
    let id = NEXT_ID.fetch_add(1, Ordering::Relaxed);
    std::env::temp_dir().join(format!(
        "foundry-rust-build-support-{name}-{}-{id}",
        std::process::id()
    ))
}

fn create_file(path: &Path) {
    fs::create_dir_all(path.parent().expect("test file must have a parent")).unwrap();
    fs::write(path, []).unwrap();
}

#[test]
fn config_defaults_to_http_and_public_feeds() {
    let config = config_from(&[], false).unwrap();
    assert_eq!(config.mode, NuGetMode::Http);
    assert_eq!(
        config.feeds,
        DEFAULT_FEEDS
            .iter()
            .map(|feed| (*feed).to_string())
            .collect::<Vec<_>>()
    );
    assert_eq!(config.dotnet_command, "dotnet");
    assert_eq!(config.nuget_command, "nuget");
}

#[test]
fn config_accepts_each_mode_and_windows_nuget_default() {
    assert_eq!(
        config_from(&[("FOUNDRY_LOCAL_NUGET_MODE", "http")], false)
            .unwrap()
            .mode,
        NuGetMode::Http
    );
    assert_eq!(
        config_from(&[("FOUNDRY_LOCAL_NUGET_MODE", "dotnet")], false)
            .unwrap()
            .mode,
        NuGetMode::Dotnet
    );
    let config = config_from(&[("FOUNDRY_LOCAL_NUGET_MODE", "nuget")], true).unwrap();
    assert_eq!(config.mode, NuGetMode::Nuget);
    assert_eq!(config.nuget_command, "nuget.exe");
}

#[test]
fn config_rejects_invalid_mode() {
    let error = config_from(&[("FOUNDRY_LOCAL_NUGET_MODE", "ftp")], false).unwrap_err();
    assert!(error.contains("Invalid FOUNDRY_LOCAL_NUGET_MODE"));
}

#[test]
fn custom_feeds_replace_defaults_and_ignore_empty_entries() {
    let config = config_from(
        &[(
            "FOUNDRY_LOCAL_NUGET_FEEDS",
            " https://one.example/v3/index.json ; ; https://two.example/v3/index.json ",
        )],
        false,
    )
    .unwrap();
    assert_eq!(
        config.feeds,
        [
            "https://one.example/v3/index.json",
            "https://two.example/v3/index.json"
        ]
    );
    assert!(!config
        .feeds
        .iter()
        .any(|feed| DEFAULT_FEEDS.contains(&feed.as_str())));
}

#[test]
fn config_rejects_empty_invalid_insecure_and_credentialed_http_feeds() {
    for feed in [
        "",
        " ; ",
        "not-a-url",
        "http://feed.example/v3/index.json",
        "https://user:secret@feed.example/v3/index.json",
    ] {
        assert!(
            config_from(&[("FOUNDRY_LOCAL_NUGET_FEEDS", feed)], false).is_err(),
            "feed should have been rejected: {feed}"
        );
    }
}

#[test]
fn tool_modes_allow_non_https_feeds() {
    for mode in ["dotnet", "nuget"] {
        let config = config_from(
            &[
                ("FOUNDRY_LOCAL_NUGET_MODE", mode),
                (
                    "FOUNDRY_LOCAL_NUGET_FEEDS",
                    "http://trusted-intranet.example/v3/index.json",
                ),
            ],
            false,
        )
        .unwrap();
        assert_eq!(
            config.feeds,
            ["http://trusted-intranet.example/v3/index.json"]
        );
    }
}

#[test]
fn http_package_base_address_must_be_https() {
    assert!(
        build_support::validate_http_package_base_address("https://packages.example/v3/flat/")
            .is_ok()
    );
    assert!(
        build_support::validate_http_package_base_address("http://packages.example/v3/flat/")
            .is_err()
    );
}

#[test]
fn config_rejects_mode_incompatible_variables() {
    let cases = [
        vec![("FOUNDRY_LOCAL_NUGET_CONFIG", "NuGet.Config")],
        vec![("FOUNDRY_LOCAL_DOTNET_COMMAND", "dotnet-custom")],
        vec![("FOUNDRY_LOCAL_NUGET_COMMAND", "nuget-custom")],
        vec![
            ("FOUNDRY_LOCAL_NUGET_MODE", "dotnet"),
            ("FOUNDRY_LOCAL_NUGET_COMMAND", "nuget-custom"),
        ],
        vec![
            ("FOUNDRY_LOCAL_NUGET_MODE", "nuget"),
            ("FOUNDRY_LOCAL_DOTNET_COMMAND", "dotnet-custom"),
        ],
    ];
    for values in cases {
        assert!(config_from(&values, false).is_err());
    }
}

#[test]
fn config_accepts_config_file_and_mode_specific_commands() {
    let dotnet = config_from(
        &[
            ("FOUNDRY_LOCAL_NUGET_MODE", "dotnet"),
            ("FOUNDRY_LOCAL_NUGET_CONFIG", "/tmp/NuGet.Config"),
            ("FOUNDRY_LOCAL_DOTNET_COMMAND", "dotnet-custom"),
        ],
        false,
    )
    .unwrap();
    assert_eq!(dotnet.config_file.as_deref(), Some("/tmp/NuGet.Config"));
    assert_eq!(dotnet.dotnet_command, "dotnet-custom");

    let nuget = config_from(
        &[
            ("FOUNDRY_LOCAL_NUGET_MODE", "nuget"),
            ("FOUNDRY_LOCAL_NUGET_CONFIG", "/tmp/NuGet.Config"),
            ("FOUNDRY_LOCAL_NUGET_COMMAND", "nuget-custom"),
        ],
        false,
    )
    .unwrap();
    assert_eq!(nuget.config_file.as_deref(), Some("/tmp/NuGet.Config"));
    assert_eq!(nuget.nuget_command, "nuget-custom");
}

#[test]
fn dotnet_args_use_config_without_sources() {
    let config = config_from(
        &[
            ("FOUNDRY_LOCAL_NUGET_MODE", "dotnet"),
            ("FOUNDRY_LOCAL_NUGET_CONFIG", "/tmp/NuGet.Config"),
        ],
        false,
    )
    .unwrap();
    let args = args_as_strings(build_support::build_dotnet_restore_args(
        &config,
        Path::new("restore.csproj"),
        Path::new("packages"),
    ));
    assert_eq!(
        args,
        [
            "restore",
            "restore.csproj",
            "--packages",
            "packages",
            "--no-cache",
            "--configfile",
            "/tmp/NuGet.Config"
        ]
    );
    assert!(!args.iter().any(|arg| arg == "--source"));
    assert!(!args.iter().any(|arg| arg == "--runtime"));
}

#[test]
fn dotnet_args_use_each_feed_without_config() {
    let config = config_from(
        &[
            ("FOUNDRY_LOCAL_NUGET_MODE", "dotnet"),
            (
                "FOUNDRY_LOCAL_NUGET_FEEDS",
                "https://one.example/v3/index.json;https://two.example/v3/index.json",
            ),
        ],
        false,
    )
    .unwrap();
    let args = args_as_strings(build_support::build_dotnet_restore_args(
        &config,
        Path::new("restore.csproj"),
        Path::new("packages"),
    ));
    assert_eq!(args.iter().filter(|arg| *arg == "--source").count(), 2);
    assert!(args.contains(&"https://one.example/v3/index.json".to_string()));
    assert!(args.contains(&"https://two.example/v3/index.json".to_string()));
}

#[test]
fn nuget_args_use_exact_install_flags_and_config_without_sources() {
    let config = config_from(
        &[
            ("FOUNDRY_LOCAL_NUGET_MODE", "nuget"),
            ("FOUNDRY_LOCAL_NUGET_CONFIG", "/tmp/NuGet.Config"),
        ],
        false,
    )
    .unwrap();
    let args = args_as_strings(build_support::build_nuget_install_args(
        &config,
        "Microsoft.ML.OnnxRuntime",
        "1.2.3",
        Path::new("packages"),
    ));
    assert_eq!(
        args,
        [
            "install",
            "Microsoft.ML.OnnxRuntime",
            "-Version",
            "1.2.3",
            "-OutputDirectory",
            "packages",
            "-NonInteractive",
            "-DirectDownload",
            "-DependencyVersion",
            "Ignore",
            "-ConfigFile",
            "/tmp/NuGet.Config"
        ]
    );
    assert!(!args.iter().any(|arg| arg == "-Source"));
}

#[test]
fn generated_restore_project_pins_exact_versions_and_escapes_xml() {
    let project = build_support::generate_restore_project(&[
        ("Package.One", "1.2.3"),
        ("Package<&Two", "4.5.6"),
    ]);
    assert!(project.contains(r#"Include="Package.One" Version="[1.2.3]""#));
    assert!(project.contains(r#"Include="Package&lt;&amp;Two" Version="[4.5.6]""#));
    assert!(project.contains("<TargetFramework>net8.0</TargetFramework>"));
}

#[test]
fn package_discovery_handles_dotnet_and_nuget_layouts() {
    let root = test_dir("package-layouts");
    let dotnet_dir = root
        .join("dotnet")
        .join("microsoft.ml.onnxruntime")
        .join("1.2.3");
    fs::create_dir_all(&dotnet_dir).unwrap();
    assert_eq!(
        build_support::find_dotnet_package_dir(
            &root.join("dotnet"),
            "Microsoft.ML.OnnxRuntime",
            "1.2.3"
        )
        .unwrap(),
        dotnet_dir
    );

    let nuget_dir = root.join("nuget").join("microsoft.ml.onnxruntime.1.2.3");
    fs::create_dir_all(&nuget_dir).unwrap();
    assert_eq!(
        build_support::find_nuget_package_dir(
            &root.join("nuget"),
            "Microsoft.ML.OnnxRuntime",
            "1.2.3"
        )
        .unwrap(),
        nuget_dir
    );
    fs::remove_dir_all(root).unwrap();
}

#[test]
fn native_collection_matches_http_mode_layout_and_extension_filter() {
    let root = test_dir("native-files");
    let native_dir = root.join("runtimes").join("linux-x64").join("native");
    let runtime_dir = root.join("runtimes").join("linux-x64");
    create_file(&native_dir.join("libfoundry_local.so"));
    create_file(&native_dir.join("readme.txt"));
    create_file(&runtime_dir.join("libonnxruntime-genai.so"));
    create_file(&runtime_dir.join("libonnxruntime.so.1"));
    create_file(&runtime_dir.join("nested").join("ignored.so"));

    let files = build_support::collect_native_files(&root, "linux-x64", "so").unwrap();
    assert_eq!(
        files,
        [
            runtime_dir.join("libonnxruntime-genai.so"),
            native_dir.join("libfoundry_local.so")
        ]
    );
    fs::remove_dir_all(root).unwrap();
}

#[test]
fn url_redaction_removes_credentials_queries_and_fragments() {
    let text = "failed https://user:secret@example.test/package?sig=secret#fragment then retry";
    let redacted = build_support::redact_urls_in_text(text);
    assert_eq!(redacted, "failed https://example.test/package then retry");
    assert!(!redacted.contains("secret"));
}

#[test]
fn url_redaction_recognizes_mixed_case_schemes() {
    let text =
        "failed HTTPS://user:secret@example.test/package?sig=secret then Http://other.test/#token";
    let redacted = build_support::redact_urls_in_text(text);
    assert_eq!(
        redacted,
        "failed HTTPS://example.test/package then Http://other.test/"
    );
    assert!(!redacted.contains("secret"));
    assert!(!redacted.contains("token"));
}

#[test]
fn package_cache_requires_a_matching_version_marker() {
    let root = test_dir("package-version");
    fs::create_dir_all(&root).unwrap();
    create_file(&root.join("native.dll"));
    assert!(!build_support::package_is_current(
        &root,
        "native.dll",
        "1.0.0"
    ));

    build_support::record_package_version(&root, "native.dll", "1.0.0").unwrap();
    assert!(build_support::package_is_current(
        &root,
        "native.dll",
        "1.0.0"
    ));
    assert!(!build_support::package_is_current(
        &root,
        "native.dll",
        "2.0.0"
    ));
    fs::remove_dir_all(root).unwrap();
}

#[test]
fn package_invalidation_removes_binary_and_version_marker() {
    let root = test_dir("package-invalidation");
    fs::create_dir_all(&root).unwrap();
    create_file(&root.join("native.dll"));
    build_support::record_package_version(&root, "native.dll", "1.0.0").unwrap();

    build_support::invalidate_package(&root, "native.dll").unwrap();

    assert!(!root.join("native.dll").exists());
    assert!(!build_support::package_is_current(
        &root,
        "native.dll",
        "1.0.0"
    ));
    fs::remove_dir_all(root).unwrap();
}

#[test]
fn expected_file_check_uses_the_current_package_file_list() {
    let files = vec![
        PathBuf::from("runtimes/win-x64/native/helper.dll"),
        PathBuf::from("runtimes/win-x64/native/native.dll"),
    ];
    assert!(build_support::contains_expected_file(&files, "native.dll"));
    assert!(!build_support::contains_expected_file(
        &files,
        "missing.dll"
    ));
}

#[test]
fn stale_package_resets_all_managed_native_files() {
    let root = test_dir("reset-native-cache");
    fs::create_dir_all(&root).unwrap();
    create_file(&root.join("runtime.dll"));
    create_file(&root.join("stale-companion.dll"));
    create_file(&root.join("ort.dll"));
    create_file(&root.join("keep.txt"));
    build_support::record_package_version(&root, "runtime.dll", "1.0.0").unwrap();
    build_support::record_package_version(&root, "ort.dll", "2.0.0").unwrap();

    let reset = build_support::reset_native_cache_if_stale(
        &root,
        &[("runtime.dll", "1.1.0"), ("ort.dll", "2.0.0")],
        "dll",
    )
    .unwrap();

    assert!(reset);
    assert!(!root.join("runtime.dll").exists());
    assert!(!root.join("stale-companion.dll").exists());
    assert!(!root.join("ort.dll").exists());
    assert!(root.join("keep.txt").exists());
    assert!(!build_support::package_is_current(
        &root, "ort.dll", "2.0.0"
    ));
    fs::remove_dir_all(root).unwrap();
}

#[test]
fn current_native_cache_is_left_untouched() {
    let root = test_dir("keep-native-cache");
    fs::create_dir_all(&root).unwrap();
    create_file(&root.join("runtime.dll"));
    create_file(&root.join("companion.dll"));
    build_support::record_package_version(&root, "runtime.dll", "1.0.0").unwrap();

    let reset =
        build_support::reset_native_cache_if_stale(&root, &[("runtime.dll", "1.0.0")], "dll")
            .unwrap();

    assert!(!reset);
    assert!(root.join("runtime.dll").exists());
    assert!(root.join("companion.dll").exists());
    fs::remove_dir_all(root).unwrap();
}

#[test]
fn local_staging_invalidates_nuget_cache_identity() {
    let root = test_dir("local-source-transition");
    fs::create_dir_all(&root).unwrap();
    fs::write(root.join("runtime.dll"), b"nuget").unwrap();
    fs::write(root.join("ort.dll"), b"nuget").unwrap();
    build_support::record_package_version(&root, "runtime.dll", "1.0.0").unwrap();
    build_support::record_package_version(&root, "ort.dll", "2.0.0").unwrap();

    fs::write(root.join("runtime.dll"), b"local").unwrap();
    build_support::invalidate_package_version_markers(&root).unwrap();

    assert_eq!(fs::read(root.join("runtime.dll")).unwrap(), b"local");
    assert!(!build_support::package_is_current(
        &root,
        "runtime.dll",
        "1.0.0"
    ));
    assert!(!build_support::package_is_current(
        &root, "ort.dll", "2.0.0"
    ));
    assert!(build_support::reset_native_cache_if_stale(
        &root,
        &[("runtime.dll", "1.0.0"), ("ort.dll", "2.0.0")],
        "dll",
    )
    .unwrap());
    assert!(!root.join("runtime.dll").exists());
    assert!(!root.join("ort.dll").exists());
    fs::remove_dir_all(root).unwrap();
}

#[test]
fn marker_invalidation_is_a_noop_without_package_markers() {
    let root = test_dir("no-package-markers");
    fs::create_dir_all(&root).unwrap();
    create_file(&root.join("runtime.dll"));
    create_file(&root.join(".unrelated"));

    build_support::invalidate_package_version_markers(&root).unwrap();

    assert!(root.join("runtime.dll").exists());
    assert!(root.join(".unrelated").exists());
    fs::remove_dir_all(root).unwrap();
}
