use std::env;
use std::ffi::OsString;
use std::fs;
use std::path::{Path, PathBuf};

pub const DEFAULT_FEEDS: &[&str] = &[
    "https://api.nuget.org/v3/index.json",
    "https://pkgs.dev.azure.com/aiinfra/PublicPackages/_packaging/ORT-Nightly/nuget/v3/index.json",
];

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum NuGetMode {
    Http,
    Dotnet,
    Nuget,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct NuGetConfig {
    pub mode: NuGetMode,
    pub feeds: Vec<String>,
    pub config_file: Option<String>,
    pub dotnet_command: String,
    pub nuget_command: String,
}

pub fn read_config() -> Result<NuGetConfig, String> {
    read_config_from(|name| env::var(name).ok(), cfg!(windows))
}

pub fn read_config_from<F>(get_env: F, is_windows: bool) -> Result<NuGetConfig, String>
where
    F: Fn(&str) -> Option<String>,
{
    let mode_raw = get_env("FOUNDRY_LOCAL_NUGET_MODE");
    let mode = match mode_raw.as_deref().filter(|value| !value.is_empty()) {
        None | Some("http") => NuGetMode::Http,
        Some("dotnet") => NuGetMode::Dotnet,
        Some("nuget") => NuGetMode::Nuget,
        Some(value) => {
            return Err(format!(
                "Invalid FOUNDRY_LOCAL_NUGET_MODE '{value}'. Expected 'http', 'dotnet', or 'nuget'."
            ))
        }
    };

    let feeds_raw = get_env("FOUNDRY_LOCAL_NUGET_FEEDS");
    let feeds = match feeds_raw {
        Some(raw) => {
            let values: Vec<_> = raw
                .split(';')
                .map(str::trim)
                .filter(|value| !value.is_empty())
                .map(str::to_string)
                .collect();
            if values.is_empty() {
                return Err(
                    "FOUNDRY_LOCAL_NUGET_FEEDS is set but contains no feed URLs.".to_string(),
                );
            }
            values
        }
        None => DEFAULT_FEEDS
            .iter()
            .map(|feed| (*feed).to_string())
            .collect(),
    };

    for feed in &feeds {
        validate_feed_url(feed, mode == NuGetMode::Http)?;
    }

    let config_file = nonempty(get_env("FOUNDRY_LOCAL_NUGET_CONFIG"));
    let dotnet_command = nonempty(get_env("FOUNDRY_LOCAL_DOTNET_COMMAND"));
    let nuget_command = nonempty(get_env("FOUNDRY_LOCAL_NUGET_COMMAND"));

    match mode {
        NuGetMode::Http => {
            if config_file.is_some() {
                return Err(
                    "FOUNDRY_LOCAL_NUGET_CONFIG is only valid when FOUNDRY_LOCAL_NUGET_MODE is \
                     'dotnet' or 'nuget'."
                        .to_string(),
                );
            }
            if dotnet_command.is_some() {
                return Err("FOUNDRY_LOCAL_DOTNET_COMMAND is only valid when \
                     FOUNDRY_LOCAL_NUGET_MODE=dotnet."
                    .to_string());
            }
            if nuget_command.is_some() {
                return Err("FOUNDRY_LOCAL_NUGET_COMMAND is only valid when \
                     FOUNDRY_LOCAL_NUGET_MODE=nuget."
                    .to_string());
            }
        }
        NuGetMode::Dotnet if nuget_command.is_some() => {
            return Err("FOUNDRY_LOCAL_NUGET_COMMAND is only valid when \
                 FOUNDRY_LOCAL_NUGET_MODE=nuget."
                .to_string())
        }
        NuGetMode::Nuget if dotnet_command.is_some() => {
            return Err("FOUNDRY_LOCAL_DOTNET_COMMAND is only valid when \
                 FOUNDRY_LOCAL_NUGET_MODE=dotnet."
                .to_string())
        }
        NuGetMode::Dotnet | NuGetMode::Nuget => {}
    }

    Ok(NuGetConfig {
        mode,
        feeds,
        config_file,
        dotnet_command: dotnet_command.unwrap_or_else(|| "dotnet".to_string()),
        nuget_command: nuget_command
            .unwrap_or_else(|| if is_windows { "nuget.exe" } else { "nuget" }.to_string()),
    })
}

fn nonempty(value: Option<String>) -> Option<String> {
    value.filter(|value| !value.is_empty())
}

pub fn validate_feed_url(url: &str, require_https: bool) -> Result<(), String> {
    let (scheme, remainder) = url.split_once("://").ok_or_else(|| {
        format!(
            "FOUNDRY_LOCAL_NUGET_FEEDS contains an invalid URL: {}",
            redact_url(url)
        )
    })?;
    let authority = remainder
        .split(['/', '?', '#'])
        .next()
        .filter(|value| !value.is_empty());
    let valid_scheme = !scheme.is_empty()
        && scheme
            .chars()
            .all(|ch| ch.is_ascii_alphanumeric() || matches!(ch, '+' | '-' | '.'));
    if !valid_scheme || authority.is_none() || url.chars().any(char::is_whitespace) {
        return Err(format!(
            "FOUNDRY_LOCAL_NUGET_FEEDS contains an invalid URL: {}",
            redact_url(url)
        ));
    }
    if authority.is_some_and(|value| value.contains('@')) {
        return Err("FOUNDRY_LOCAL_NUGET_FEEDS must not contain embedded credentials.".to_string());
    }
    if require_https && !scheme.eq_ignore_ascii_case("https") {
        return Err(format!(
            "FOUNDRY_LOCAL_NUGET_FEEDS must use HTTPS in http mode: {}",
            redact_url(url)
        ));
    }
    Ok(())
}

pub fn validate_http_package_base_address(url: &str) -> Result<(), String> {
    validate_feed_url(url, true).map_err(|_| {
        format!(
            "NuGet PackageBaseAddress must use a valid HTTPS URL in http mode: {}",
            redact_url(url)
        )
    })
}

pub fn redact_url(url: &str) -> String {
    let Some((scheme, remainder)) = url.split_once("://") else {
        return url.to_string();
    };
    let end = remainder.find(['/', '?', '#']).unwrap_or(remainder.len());
    let authority = &remainder[..end];
    let authority = authority
        .rsplit_once('@')
        .map_or(authority, |(_, host)| host);
    let path = &remainder[end..];
    let sensitive = path.find(['?', '#']).unwrap_or(path.len());
    format!("{scheme}://{authority}{}", &path[..sensitive])
}

pub fn redact_urls_in_text(text: &str) -> String {
    let mut result = String::with_capacity(text.len());
    let mut remaining = text;
    while let Some(start) = find_url_start(remaining) {
        result.push_str(&remaining[..start]);
        let url_and_rest = &remaining[start..];
        let end = url_and_rest
            .find(|ch: char| ch.is_whitespace() || matches!(ch, '"' | '\'' | '<' | '>'))
            .unwrap_or(url_and_rest.len());
        result.push_str(&redact_url(&url_and_rest[..end]));
        remaining = &url_and_rest[end..];
    }
    result.push_str(remaining);
    result
}

fn find_url_start(value: &str) -> Option<usize> {
    let bytes = value.as_bytes();
    [
        bytes
            .windows(b"https://".len())
            .position(|window| window.eq_ignore_ascii_case(b"https://")),
        bytes
            .windows(b"http://".len())
            .position(|window| window.eq_ignore_ascii_case(b"http://")),
    ]
    .into_iter()
    .flatten()
    .min()
}

pub fn invalidate_package(out_dir: &Path, expected_file: &str) -> Result<(), String> {
    remove_file_if_present(&out_dir.join(expected_file))?;
    remove_file_if_present(&package_version_path(out_dir, expected_file))
}

pub fn invalidate_package_version_markers(out_dir: &Path) -> Result<(), String> {
    for entry in fs::read_dir(out_dir)
        .map_err(|error| format!("read native cache {}: {error}", out_dir.display()))?
    {
        let entry = entry.map_err(|error| format!("read native cache entry: {error}"))?;
        let file_name = entry.file_name();
        let file_name = file_name.to_string_lossy();
        if entry
            .file_type()
            .map_err(|error| format!("read native cache entry type: {error}"))?
            .is_file()
            && file_name.starts_with('.')
            && file_name.ends_with(".version")
        {
            remove_file_if_present(&entry.path())?;
        }
    }
    Ok(())
}

fn remove_file_if_present(path: &Path) -> Result<(), String> {
    match fs::remove_file(path) {
        Ok(()) => Ok(()),
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(()),
        Err(error) => Err(format!(
            "remove stale package file {}: {error}",
            path.display()
        )),
    }
}

pub fn contains_expected_file(files: &[PathBuf], expected_file: &str) -> bool {
    files.iter().any(|file| {
        file.file_name()
            .is_some_and(|file_name| file_name == expected_file)
    })
}

pub fn package_is_current(out_dir: &Path, expected_file: &str, version: &str) -> bool {
    out_dir.join(expected_file).is_file()
        && fs::read_to_string(package_version_path(out_dir, expected_file))
            .is_ok_and(|recorded| recorded.trim() == version)
}

pub fn record_package_version(
    out_dir: &Path,
    expected_file: &str,
    version: &str,
) -> Result<(), String> {
    let marker = package_version_path(out_dir, expected_file);
    fs::write(&marker, version)
        .map_err(|error| format!("write package version marker {}: {error}", marker.display()))
}

fn package_version_path(out_dir: &Path, expected_file: &str) -> PathBuf {
    out_dir.join(format!(".{expected_file}.version"))
}

pub fn reset_native_cache_if_stale(
    out_dir: &Path,
    packages: &[(&str, &str)],
    extension: &str,
) -> Result<bool, String> {
    if packages
        .iter()
        .all(|(expected_file, version)| package_is_current(out_dir, expected_file, version))
    {
        return Ok(false);
    }

    for entry in fs::read_dir(out_dir)
        .map_err(|error| format!("read native cache {}: {error}", out_dir.display()))?
    {
        let entry = entry.map_err(|error| format!("read native cache entry: {error}"))?;
        if entry
            .file_type()
            .map_err(|error| format!("read native cache entry type: {error}"))?
            .is_file()
            && entry.path().extension().and_then(|value| value.to_str()) == Some(extension)
        {
            remove_file_if_present(&entry.path())?;
        }
    }
    for (expected_file, _) in packages {
        remove_file_if_present(&package_version_path(out_dir, expected_file))?;
    }
    Ok(true)
}

pub fn generate_restore_project(packages: &[(&str, &str)]) -> String {
    let references = packages
        .iter()
        .map(|(name, version)| {
            format!(
                "    <PackageReference Include=\"{}\" Version=\"[{}]\" />",
                escape_xml(name),
                escape_xml(version)
            )
        })
        .collect::<Vec<_>>()
        .join("\n");
    format!(
        "<Project Sdk=\"Microsoft.NET.Sdk\">\n\
         \x20 <PropertyGroup>\n\
         \x20   <TargetFramework>net8.0</TargetFramework>\n\
         \x20   <EnableDefaultCompileItems>false</EnableDefaultCompileItems>\n\
         \x20   <IsPackable>false</IsPackable>\n\
         \x20 </PropertyGroup>\n\
         \x20 <ItemGroup>\n\
         {references}\n\
         \x20 </ItemGroup>\n\
         </Project>\n"
    )
}

fn escape_xml(value: &str) -> String {
    value
        .replace('&', "&amp;")
        .replace('"', "&quot;")
        .replace('<', "&lt;")
        .replace('>', "&gt;")
}

pub fn build_dotnet_restore_args(
    config: &NuGetConfig,
    project_path: &Path,
    packages_dir: &Path,
) -> Vec<OsString> {
    let mut args = vec![
        "restore".into(),
        project_path.as_os_str().to_owned(),
        "--packages".into(),
        packages_dir.as_os_str().to_owned(),
        "--no-cache".into(),
    ];
    if let Some(config_file) = &config.config_file {
        args.push("--configfile".into());
        args.push(config_file.into());
    } else {
        for feed in &config.feeds {
            args.push("--source".into());
            args.push(feed.into());
        }
    }
    args
}

pub fn build_nuget_install_args(
    config: &NuGetConfig,
    id: &str,
    version: &str,
    output_dir: &Path,
) -> Vec<OsString> {
    let mut args = vec![
        "install".into(),
        id.into(),
        "-Version".into(),
        version.into(),
        "-OutputDirectory".into(),
        output_dir.as_os_str().to_owned(),
        "-NonInteractive".into(),
        "-DirectDownload".into(),
        "-DependencyVersion".into(),
        "Ignore".into(),
    ];
    if let Some(config_file) = &config.config_file {
        args.push("-ConfigFile".into());
        args.push(config_file.into());
    } else {
        for feed in &config.feeds {
            args.push("-Source".into());
            args.push(feed.into());
        }
    }
    args
}

pub fn find_dotnet_package_dir(
    packages_dir: &Path,
    id: &str,
    version: &str,
) -> Result<PathBuf, String> {
    let path = packages_dir
        .join(id.to_lowercase())
        .join(version.to_lowercase());
    if path.is_dir() {
        Ok(path)
    } else {
        Err(format!(
            "Restored package not found at expected path: {}",
            path.display()
        ))
    }
}

pub fn find_nuget_package_dir(
    output_dir: &Path,
    id: &str,
    version: &str,
) -> Result<PathBuf, String> {
    let expected = format!("{id}.{version}").to_lowercase();
    let entries = fs::read_dir(output_dir)
        .map_err(|error| format!("read package directory {}: {error}", output_dir.display()))?;
    for entry in entries {
        let entry = entry.map_err(|error| format!("read package directory entry: {error}"))?;
        if entry
            .file_type()
            .map_err(|error| format!("read package entry type: {error}"))?
            .is_dir()
            && entry.file_name().to_string_lossy().to_lowercase() == expected
        {
            return Ok(entry.path());
        }
    }
    Err(format!(
        "Restored package not found under {} (expected {id}.{version}).",
        output_dir.display()
    ))
}

pub fn collect_native_files(
    package_dir: &Path,
    rid: &str,
    extension: &str,
) -> Result<Vec<PathBuf>, String> {
    let runtime_dir = package_dir.join("runtimes").join(rid);
    let native_dir = runtime_dir.join("native");
    let mut files = collect_matching_files(&native_dir, extension)?;
    files.extend(collect_matching_files(&runtime_dir, extension)?);
    files.sort();
    files.dedup();
    Ok(files)
}

fn collect_matching_files(directory: &Path, extension: &str) -> Result<Vec<PathBuf>, String> {
    if !directory.is_dir() {
        return Ok(Vec::new());
    }
    let mut files = Vec::new();
    for entry in fs::read_dir(directory)
        .map_err(|error| format!("read native directory {}: {error}", directory.display()))?
    {
        let entry = entry.map_err(|error| format!("read native directory entry: {error}"))?;
        if entry
            .file_type()
            .map_err(|error| format!("read native entry type: {error}"))?
            .is_file()
            && entry.path().extension().and_then(|value| value.to_str()) == Some(extension)
        {
            files.push(entry.path());
        }
    }
    Ok(files)
}
