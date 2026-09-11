use std::fs;
use std::io::{self, Read, Seek};
use std::path::{Path, PathBuf};

pub fn extract_package<R: Read + Seek>(
    reader: R,
    rid: &str,
    extension: &str,
    expected_file: &str,
    staging_dir: &Path,
) -> Result<Vec<PathBuf>, String> {
    reset_dir(staging_dir)?;
    let result = extract_package_inner(reader, rid, extension, expected_file, staging_dir);
    if result.is_err() {
        cleanup_dir(staging_dir);
    }
    result
}

fn extract_package_inner<R: Read + Seek>(
    reader: R,
    rid: &str,
    extension: &str,
    expected_file: &str,
    staging_dir: &Path,
) -> Result<Vec<PathBuf>, String> {
    let native_prefix = format!("runtimes/{rid}/native/");
    let runtime_prefix = format!("runtimes/{rid}/");
    let mut archive =
        zip::ZipArchive::new(reader).map_err(|error| format!("open nupkg: {error}"))?;
    let mut extracted = Vec::new();

    for index in 0..archive.len() {
        let mut file = archive
            .by_index(index)
            .map_err(|error| format!("read zip entry: {error}"))?;
        let entry = file.name().to_string();
        if !entry.ends_with(&format!(".{extension}")) {
            continue;
        }
        let direct = entry
            .strip_prefix(&runtime_prefix)
            .is_some_and(|relative| !relative.is_empty() && !relative.contains('/'));
        if !entry.starts_with(&native_prefix) && !direct {
            continue;
        }
        let Some(file_name) = Path::new(&entry).file_name() else {
            continue;
        };
        let destination = staging_dir.join(file_name);
        let mut output = fs::File::create(&destination)
            .map_err(|error| format!("create {}: {error}", destination.display()))?;
        io::copy(&mut file, &mut output)
            .map_err(|error| format!("write {}: {error}", destination.display()))?;
        extracted.push(destination);
    }

    if extracted
        .iter()
        .any(|path| path.file_name().is_some_and(|name| name == expected_file))
    {
        Ok(extracted)
    } else {
        Err(format!(
            "archive did not contain expected file {expected_file}"
        ))
    }
}

pub fn stage_files(files: &[PathBuf], out_dir: &Path) -> Result<(), String> {
    for file in files {
        let file_name = file
            .file_name()
            .ok_or_else(|| format!("native file has no file name: {}", file.display()))?;
        fs::copy(file, out_dir.join(file_name))
            .map_err(|error| format!("stage {}: {error}", file.display()))?;
    }
    Ok(())
}

pub fn cleanup_dir(path: &Path) {
    if let Err(error) = fs::remove_dir_all(path) {
        if error.kind() != io::ErrorKind::NotFound {
            println!(
                "cargo:warning=Could not remove temporary directory {}: {error}",
                path.display()
            );
        }
    }
}

fn reset_dir(path: &Path) -> Result<(), String> {
    match fs::remove_dir_all(path) {
        Ok(()) => {}
        Err(error) if error.kind() == io::ErrorKind::NotFound => {}
        Err(error) => {
            return Err(format!(
                "remove temporary directory {}: {error}",
                path.display()
            ));
        }
    }
    fs::create_dir_all(path)
        .map_err(|error| format!("create temporary directory {}: {error}", path.display()))
}
