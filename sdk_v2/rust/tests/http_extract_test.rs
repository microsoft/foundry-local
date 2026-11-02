#[allow(dead_code)]
#[path = "../build_support.rs"]
mod build_support;

#[path = "../http_extract.rs"]
mod http_extract;

use std::fs;
use std::io::{Cursor, Write};
use std::path::PathBuf;
use std::sync::atomic::{AtomicUsize, Ordering};

fn test_dir(name: &str) -> PathBuf {
    static NEXT_ID: AtomicUsize = AtomicUsize::new(0);
    let id = NEXT_ID.fetch_add(1, Ordering::Relaxed);
    std::env::temp_dir().join(format!(
        "foundry-rust-http-extract-{name}-{}-{id}",
        std::process::id()
    ))
}

fn archive(entries: &[(&str, &[u8])]) -> Cursor<Vec<u8>> {
    let mut bytes = Cursor::new(Vec::new());
    {
        let mut writer = zip::ZipWriter::new(&mut bytes);
        for (name, contents) in entries {
            writer
                .start_file(*name, zip::write::SimpleFileOptions::default())
                .unwrap();
            writer.write_all(contents).unwrap();
        }
        writer.finish().unwrap();
    }
    bytes.set_position(0);
    bytes
}

#[test]
fn rejected_archive_does_not_contaminate_shared_output() {
    let root = test_dir("rejected");
    let staging = root.join("staging");
    let output = root.join("output");
    fs::create_dir_all(&output).unwrap();

    let result = http_extract::extract_package(
        archive(&[("runtimes/win-x64/native/companion.dll", b"stale")]),
        "win-x64",
        "dll",
        "expected.dll",
        &staging,
    );

    assert!(result.is_err());
    assert!(!output.join("companion.dll").exists());
    assert!(!staging.exists());
    fs::remove_dir_all(root).unwrap();
}

#[test]
fn failed_attempt_cannot_contaminate_later_success() {
    let root = test_dir("retry");
    let staging = root.join("staging");
    let output = root.join("output");
    fs::create_dir_all(&output).unwrap();

    assert!(http_extract::extract_package(
        archive(&[("runtimes/win-x64/native/failed-only.dll", b"failed")]),
        "win-x64",
        "dll",
        "expected.dll",
        &staging,
    )
    .is_err());
    let files = http_extract::extract_package(
        archive(&[
            ("runtimes/win-x64/native/expected.dll", b"expected"),
            ("runtimes/win-x64/native/companion.dll", b"companion"),
        ]),
        "win-x64",
        "dll",
        "expected.dll",
        &staging,
    )
    .unwrap();
    http_extract::stage_files(&files, &output).unwrap();

    assert_eq!(fs::read(output.join("expected.dll")).unwrap(), b"expected");
    assert_eq!(
        fs::read(output.join("companion.dll")).unwrap(),
        b"companion"
    );
    assert!(!output.join("failed-only.dll").exists());
    fs::remove_dir_all(root).unwrap();
}

#[test]
fn shared_host_and_manifest_survive_extraction_and_staging() {
    for (rid, library, extension, host) in [
        (
            "win-x64",
            "foundry_local.dll",
            "dll",
            "foundry-local-host.exe",
        ),
        (
            "linux-x64",
            "libfoundry_local.so",
            "so",
            "foundry-local-host",
        ),
        (
            "osx-arm64",
            "libfoundry_local.dylib",
            "dylib",
            "foundry-local-host",
        ),
    ] {
        let root = test_dir(rid);
        let staging = root.join("staging");
        let output = root.join("output");
        fs::create_dir_all(&output).unwrap();
        let prefix = format!("runtimes/{rid}/native/");
        let files = http_extract::extract_package(
            archive(&[
                (&format!("{prefix}{library}"), b"native"),
                (&format!("{prefix}{host}"), b"host"),
                (&format!("{prefix}foundry-local-runtime.json"), b"manifest"),
                (&format!("{prefix}unrelated.exe"), b"excluded"),
            ]),
            rid,
            extension,
            library,
            &staging,
        )
        .unwrap();
        http_extract::stage_files(&files, &output).unwrap();
        assert_eq!(fs::read(output.join(host)).unwrap(), b"host");
        assert_eq!(
            fs::read(output.join("foundry-local-runtime.json")).unwrap(),
            b"manifest"
        );
        assert!(!output.join("unrelated.exe").exists());
        #[cfg(unix)]
        if host == "foundry-local-host" {
            use std::os::unix::fs::PermissionsExt;
            assert_ne!(
                fs::metadata(output.join(host))
                    .unwrap()
                    .permissions()
                    .mode()
                    & 0o100,
                0
            );
        }
        fs::remove_dir_all(root).unwrap();
    }
}
