//! SDK-controlled sharing of a Foundry serving process across independent clients.
//!
//! Catalog and acquisition remain ordinary SDK operations. Only serving runs in
//! `foundry-local-host`; dropping a client never unloads models or stops that host.
//! The state directory must be private to the current user. Inference retains the
//! existing loopback HTTP server's access policy; loopback is not authentication.

use std::collections::BTreeMap;
use std::fs::{self, File, OpenOptions};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::time::{Duration, Instant};

use fs2::FileExt;
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};

use crate::error::{FoundryLocalError, Result};
use crate::CatalogType;

const PROTOCOL_VERSION: u32 = 2;
const START_TIMEOUT: Duration = Duration::from_secs(120);

/// A single shared serving configuration. Different configurations must use
/// different state directories; an incompatible live host is never replaced.
#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct SharedServiceConfig {
    /// User-private directory containing coordination files, not model weights.
    pub directory: PathBuf,
    /// Packaged `foundry-local-host` executable matching this SDK.
    pub executable: PathBuf,
    /// Native Foundry library file used by both catalog client and host.
    pub library: PathBuf,
    /// Shared model cache; downloads use the existing SDK.
    pub model_cache_dir: PathBuf,
    /// Shared SDK application data, including local model registrations.
    pub app_data_dir: PathBuf,
    /// Execution providers to prepare/register in the serving process through
    /// existing SDK acquisition. Empty means no additional provider preparation.
    #[serde(default)]
    pub execution_providers: Vec<String>,
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
struct HostIdentity {
    protocol: u32,
    sdk_version: String,
    executable_hash: String,
    library_hash: String,
    dependency_hashes: BTreeMap<String, String>,
    config: SharedServiceConfig,
}

#[derive(Serialize, Deserialize)]
struct HostRecord {
    identity: HostIdentity,
    pid: u32,
    endpoint: String,
}

/// Lightweight SDK control client. Construction does not start a process or load
/// a native library. Call catalog APIs separately for discovery and preparation.
pub struct SharedService {
    config: SharedServiceConfig,
}

/// Observation of a compatible live host, not a model residency reservation.
#[derive(Clone)]
pub struct SharedServiceConnection {
    endpoint: String,
    client: reqwest::Client,
}

/// Authoritative serving observation. Context length comes from the loaded native
/// configuration, not the catalog; absence means unknown.
#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct SharedModelState {
    /// Resolved model variant ID in the selected catalog.
    pub model_id: String,
    /// Authoritative local artifact path in the serving process.
    pub model_path: PathBuf,
    /// Loaded native context window, when reported by the serving runtime.
    pub context_length: Option<u64>,
}

impl SharedService {
    /// Configure shared serving without starting it.
    pub fn new(config: SharedServiceConfig) -> Result<Self> {
        if !config.directory.is_absolute()
            || !config.executable.is_absolute()
            || !config.library.is_absolute()
            || !config.model_cache_dir.is_absolute()
            || !config.app_data_dir.is_absolute()
        {
            return Err(invalid("shared service paths must be absolute"));
        }
        if config
            .execution_providers
            .iter()
            .any(|name| name.trim().is_empty())
        {
            return Err(invalid("execution-provider names must not be empty"));
        }
        Ok(Self { config })
    }

    /// Observe a running compatible host, without starting one. Errors such as
    /// incompatible identity or unreachable HTTP are not reported as absence.
    pub async fn connect(&self) -> Result<Option<SharedServiceConnection>> {
        let config = self.config.clone();
        let record = crate::detail::task::spawn_blocking(move || {
            if !config.directory.exists() {
                return Ok(None);
            }
            let identity = identity(config)?;
            read_live_record(&identity)
        })
        .await?;
        match record {
            Some(record) => Ok(Some(connect_record(&record).await?)),
            None => Ok(None),
        }
    }

    /// Start or reuse the compatible host. Concurrent clients serialize startup,
    /// while the host owns a separate lifetime lock. Cancelling this waiter does
    /// not kill a process or cancel another client's work.
    pub async fn ensure_started(&self) -> Result<SharedServiceConnection> {
        let config = self.config.clone();
        let identity = crate::detail::task::spawn_blocking(move || {
            create_private_directory(&config.directory)?;
            fs::create_dir_all(&config.model_cache_dir)?;
            fs::create_dir_all(&config.app_data_dir)?;
            identity(config)
        })
        .await?;
        let start = Instant::now();
        let startup_lock = open_lock(&identity.config.directory.join("startup.lock"))?;
        loop {
            match startup_lock.try_lock_exclusive() {
                Ok(()) => break,
                Err(error) if lock_contended(&error) => {
                    check_deadline(start)?;
                    tokio::time::sleep(Duration::from_millis(50)).await;
                }
                Err(error) => return Err(error.into()),
            }
        }

        if let Some(record) = read_live_record(&identity)? {
            return connect_record(&record).await;
        }

        // The start lock protects publication of the host configuration. A host
        // still starting after its initiating client died owns host.lock; don't
        // launch or overwrite its configuration.
        let host_lock = open_lock(&identity.config.directory.join("host.lock"))?;
        let already_starting = match host_lock.try_lock_exclusive() {
            Ok(()) => false,
            Err(error) if lock_contended(&error) => true,
            Err(error) => return Err(error.into()),
        };
        let mut process_exit = None;
        if !already_starting {
            let config_path = identity.config.directory.join("config.json");
            fs::write(&config_path, serde_json::to_vec(&identity)?)?;
            let _ = fs::remove_file(identity.config.directory.join("ready.json"));
            FileExt::unlock(&host_lock)?;
            let mut command = Command::new(&identity.config.executable);
            command
                .arg("--config")
                .arg(config_path)
                .stdin(Stdio::null())
                .stdout(Stdio::null())
                .stderr(Stdio::from(
                    OpenOptions::new()
                        .create(true)
                        .append(true)
                        .open(identity.config.directory.join("host.log"))?,
                ));
            #[cfg(windows)]
            {
                use std::os::windows::process::CommandExt;
                // The serving process must survive the initiating terminal.
                command.creation_flags(0x00000008 | 0x08000000);
            }
            #[cfg(unix)]
            {
                use std::os::unix::process::CommandExt;
                command.process_group(0);
            }
            let mut child = command.spawn()?;
            let (sender, receiver) = tokio::sync::oneshot::channel();
            std::thread::spawn(move || {
                let _ = sender.send(child.wait());
            });
            process_exit = Some(receiver);
        }
        drop(host_lock);

        loop {
            if let Some(record) = read_live_record(&identity)? {
                return connect_record(&record).await;
            }
            if let Some(receiver) = &mut process_exit {
                match receiver.try_recv() {
                    Ok(status) => {
                        return Err(FoundryLocalError::ModelOperation {
                            reason: format!("shared host exited ({:?}); inspect host.log", status?),
                        });
                    }
                    Err(tokio::sync::oneshot::error::TryRecvError::Empty) => {}
                    Err(error) => {
                        return Err(invalid(format!("shared host monitor failed: {error}")))
                    }
                }
            }
            check_deadline(start)?;
            tokio::time::sleep(Duration::from_millis(50)).await;
        }
    }
}

impl SharedServiceConnection {
    /// Base OpenAI-compatible inference endpoint for the selected catalog.
    pub fn inference_endpoint(&self, catalog: CatalogType) -> String {
        format!("{}{}/v1", self.endpoint, catalog_prefix(catalog))
    }

    /// Load an exact catalog variant and artifact in the serving process.
    /// Obtain `model_path` from [`crate::Model::path`] after acquisition. A stale
    /// server registration is rejected before loading or reusing its artifact.
    /// This does not pin residency or reserve resources for a later HTTP request.
    pub async fn load(
        &self,
        catalog: CatalogType,
        model_id: &str,
        model_path: &Path,
    ) -> Result<SharedModelState> {
        if !model_path.is_absolute() {
            return Err(invalid("expected model path must be absolute"));
        }
        let expected = model_path.to_path_buf();
        let expected =
            crate::detail::task::spawn_blocking(move || Ok(fs::canonicalize(expected)?)).await?;
        let state: SharedModelState = serde_json::from_value(
            self.operation(catalog, "load", model_id, Some(&expected))
                .await?,
        )?;
        if state.model_id != model_id || state.context_length == Some(0) {
            return Err(invalid(
                "shared host returned a different model ID or invalid loaded context",
            ));
        }
        if !state.model_path.is_absolute() {
            return Err(invalid("shared host returned a relative model path"));
        }
        let actual = state.model_path.clone();
        let actual =
            crate::detail::task::spawn_blocking(move || Ok(fs::canonicalize(actual)?)).await?;
        if actual != expected {
            return Err(invalid("shared host returned a different model artifact"));
        }
        Ok(state)
    }

    /// Explicit shared unload of the selected artifact. A different artifact or
    /// in-use model is rejected; failure is never converted to a deferred unload.
    /// The absolute path may identify weights that have since been removed.
    pub async fn unload(
        &self,
        catalog: CatalogType,
        model_id: &str,
        model_path: &Path,
    ) -> Result<()> {
        if !model_path.is_absolute() {
            return Err(invalid("expected model path must be absolute"));
        }
        self.operation(catalog, "unload", model_id, Some(model_path))
            .await
            .map(|_| ())
    }

    /// Query this server's loaded IDs, not the calling process's native manager.
    pub async fn loaded_models(&self, catalog: CatalogType) -> Result<Vec<String>> {
        Ok(checked(
            self.client
                .get(format!(
                    "{}{}/models/loaded",
                    self.endpoint,
                    catalog_prefix(catalog)
                ))
                .send()
                .await?,
        )
        .await?
        .json()
        .await?)
    }

    /// Observe loaded artifacts and effective contexts without loading weights.
    /// Match artifact paths, not IDs alone, against the selected client model.
    pub async fn loaded_model_states(&self, catalog: CatalogType) -> Result<Vec<SharedModelState>> {
        let states: Vec<SharedModelState> = checked(
            self.client
                .get(format!(
                    "{}{}/models/loaded",
                    self.endpoint,
                    catalog_prefix(catalog)
                ))
                .query(&[("details", "true")])
                .send()
                .await?,
        )
        .await?
        .json()
        .await?;
        if states.iter().any(|state| {
            state.model_id.is_empty()
                || !state.model_path.is_absolute()
                || state.context_length == Some(0)
        }) {
            return Err(invalid(
                "shared host returned an invalid loaded model state",
            ));
        }
        Ok(states)
    }

    async fn operation(
        &self,
        catalog: CatalogType,
        operation: &str,
        model_id: &str,
        expected_model_path: Option<&Path>,
    ) -> Result<serde_json::Value> {
        if model_id.is_empty() {
            return Err(invalid("model ID must not be empty"));
        }
        let mut request = self.client.get(format!(
            "{}{}/models/{operation}/{}",
            self.endpoint,
            catalog_prefix(catalog),
            urlencoding::encode(model_id)
        ));
        if let Some(path) = expected_model_path {
            request = request.query(&[("expected_model_path", native_path(path)?)]);
        }
        Ok(checked(request.send().await?).await?.json().await?)
    }
}

fn catalog_prefix(catalog: CatalogType) -> &'static str {
    match catalog {
        CatalogType::Public => "",
        CatalogType::Local => "/catalogs/local",
    }
}

fn invalid(message: impl Into<String>) -> FoundryLocalError {
    FoundryLocalError::InvalidConfiguration {
        reason: message.into(),
    }
}

fn identity(mut config: SharedServiceConfig) -> Result<HostIdentity> {
    config.execution_providers.sort();
    config.execution_providers.dedup();
    config.executable = fs::canonicalize(&config.executable)?;
    config.library = fs::canonicalize(&config.library)?;
    config.directory = fs::canonicalize(&config.directory)?;
    config.model_cache_dir = fs::canonicalize(&config.model_cache_dir)?;
    config.app_data_dir = fs::canonicalize(&config.app_data_dir)?;
    let mut dependency_hashes = BTreeMap::new();
    for entry in fs::read_dir(
        config
            .library
            .parent()
            .ok_or_else(|| invalid("native library has no directory"))?,
    )? {
        let path = entry?.path();
        let name = path
            .file_name()
            .and_then(|name| name.to_str())
            .unwrap_or_default();
        let is_library = if cfg!(windows) {
            name.to_ascii_lowercase().ends_with(".dll")
        } else if cfg!(target_os = "macos") {
            name.ends_with(".dylib")
        } else {
            name.ends_with(".so") || name.contains(".so.")
        };
        if path != config.library && is_library && path.metadata()?.is_file() {
            dependency_hashes.insert(name.to_owned(), file_hash(&path)?);
        }
    }
    Ok(HostIdentity {
        protocol: PROTOCOL_VERSION,
        sdk_version: env!("CARGO_PKG_VERSION").into(),
        executable_hash: file_hash(&config.executable)?,
        library_hash: file_hash(&config.library)?,
        dependency_hashes,
        config,
    })
}

fn file_hash(path: &Path) -> Result<String> {
    use std::io::Read;
    let mut file = File::open(path)?;
    let mut hash = Sha256::new();
    let mut buffer = [0; 65536];
    loop {
        let count = file.read(&mut buffer)?;
        if count == 0 {
            break;
        }
        hash.update(&buffer[..count]);
    }
    Ok(format!("{:x}", hash.finalize()))
}

fn open_lock(path: &Path) -> Result<File> {
    Ok(OpenOptions::new()
        .read(true)
        .write(true)
        .create(true)
        .truncate(false)
        .open(path)?)
}

fn read_live_record(expected: &HostIdentity) -> Result<Option<HostRecord>> {
    let lock = open_lock(&expected.config.directory.join("host.lock"))?;
    match lock.try_lock_exclusive() {
        Ok(()) => return Ok(None),
        Err(error) if lock_contended(&error) => {}
        Err(error) => return Err(error.into()),
    }
    let data = match fs::read(expected.config.directory.join("ready.json")) {
        Ok(data) => data,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => return Ok(None),
        Err(error) => return Err(error.into()),
    };
    let record: HostRecord = serde_json::from_slice(&data)?;
    if &record.identity != expected {
        return Err(invalid(
            "a different shared host configuration is running; it was not replaced",
        ));
    }
    let url = reqwest::Url::parse(&record.endpoint).map_err(|error| invalid(error.to_string()))?;
    if url.scheme() != "http" || url.host_str() != Some("127.0.0.1") || url.port().is_none() {
        return Err(invalid(
            "shared host must advertise an explicit IPv4 loopback HTTP endpoint",
        ));
    }
    Ok(Some(record))
}

async fn connect_record(record: &HostRecord) -> Result<SharedServiceConnection> {
    let client = reqwest::Client::builder()
        .no_proxy()
        .redirect(reqwest::redirect::Policy::none())
        .connect_timeout(Duration::from_secs(5))
        .timeout(Duration::from_secs(300))
        .build()?;
    let status: serde_json::Value = checked(
        client
            .get(format!("{}/status", record.endpoint))
            .timeout(Duration::from_secs(5))
            .send()
            .await?,
    )
    .await?
    .json()
    .await?;
    if status.get("pid").and_then(serde_json::Value::as_u64) != Some(u64::from(record.pid)) {
        return Err(invalid(
            "shared serving endpoint no longer belongs to the recorded host",
        ));
    }
    if status
        .get("artifact_qualified_model_management")
        .and_then(serde_json::Value::as_bool)
        != Some(true)
    {
        return Err(invalid(
            "native runtime does not support artifact-qualified shared model management",
        ));
    }
    Ok(SharedServiceConnection {
        endpoint: record.endpoint.clone(),
        client,
    })
}

async fn checked(response: reqwest::Response) -> Result<reqwest::Response> {
    if !response.status().is_success() {
        return Err(FoundryLocalError::ModelOperation {
            reason: format!(
                "shared host returned {}: {}",
                response.status(),
                response.text().await?
            ),
        });
    }
    Ok(response)
}

fn check_deadline(start: Instant) -> Result<()> {
    if start.elapsed() >= START_TIMEOUT {
        return Err(FoundryLocalError::ModelOperation {
            reason: "shared host startup did not complete; inspect host.log and reconcile state before retrying".into(),
        });
    }
    Ok(())
}

fn lock_contended(error: &std::io::Error) -> bool {
    error.raw_os_error() == fs2::lock_contended_error().raw_os_error()
}

fn create_private_directory(path: &Path) -> Result<()> {
    #[cfg(unix)]
    {
        use std::os::unix::fs::DirBuilderExt;
        fs::DirBuilder::new()
            .recursive(true)
            .mode(0o700)
            .create(path)?;
        use std::os::unix::fs::PermissionsExt;
        if fs::metadata(path)?.permissions().mode() & 0o077 != 0 {
            return Err(invalid(
                "shared service directory must be private to the current user",
            ));
        }
    }
    #[cfg(not(unix))]
    fs::create_dir_all(path)?;
    Ok(())
}

/// Entry point for the packaged host executable, not an application lifecycle
/// API. Clients should use [`SharedService`] rather than invoking this directly.
#[doc(hidden)]
pub async fn run_host(config_path: &Path) -> Result<()> {
    let requested: HostIdentity = serde_json::from_slice(&fs::read(config_path)?)?;
    let actual = identity(requested.config.clone())?;
    if requested != actual
        || fs::canonicalize(std::env::current_exe()?)? != actual.config.executable
    {
        return Err(invalid("shared host artifacts changed before startup"));
    }
    let lock = open_lock(&actual.config.directory.join("host.lock"))?;
    lock.try_lock_exclusive()?;
    let config = crate::FoundryLocalConfig::new("foundry-shared-service")
        .library_path(native_path(&actual.config.library)?)
        .model_cache_dir(native_path(&actual.config.model_cache_dir)?)
        .app_data_dir(native_path(&actual.config.app_data_dir)?)
        .logs_dir(native_path(&actual.config.app_data_dir.join("logs"))?)
        .web_service_urls("http://127.0.0.1:0");
    let manager = crate::FoundryLocalManager::create(config)?;
    if !actual.config.execution_providers.is_empty() {
        let names: Vec<&str> = actual
            .config
            .execution_providers
            .iter()
            .map(String::as_str)
            .collect();
        let result = manager.download_and_register_eps(Some(&names)).await?;
        if !result.success {
            return Err(FoundryLocalError::ModelOperation {
                reason: format!(
                    "shared host execution-provider preparation failed: {}",
                    result.status
                ),
            });
        }
    }
    manager.start_web_service().await?;
    let endpoint = manager
        .urls()?
        .into_iter()
        .next()
        .ok_or_else(|| invalid("host has no endpoint"))?;
    let record = HostRecord {
        identity: actual,
        pid: std::process::id(),
        endpoint,
    };
    connect_record(&record).await?;
    let ready_path = record.identity.config.directory.join("ready.json");
    let pending_path = ready_path.with_extension("pending");
    fs::write(&pending_path, serde_json::to_vec(&record)?)?;
    fs::rename(pending_path, &ready_path)?;
    // A normal client release does not stop the host. The process owns all native
    // objects and remains alive until explicit host shutdown or process exit.
    while !manager.is_shutdown_requested() {
        tokio::time::sleep(Duration::from_secs(1)).await;
    }
    manager.shutdown()?;
    fs::remove_file(ready_path)?;
    Ok(())
}

fn native_path(path: &Path) -> Result<String> {
    let value = path
        .to_str()
        .ok_or_else(|| invalid("native paths must be valid UTF-8"))?;
    if value.contains('\0') {
        return Err(invalid("native paths must not contain NUL"));
    }
    // Native configuration still joins some paths with '/', which Windows
    // extended-length paths prohibit. Keep canonical identity, but pass ordinary
    // drive/UNC syntax across this existing UTF-8 SDK boundary.
    #[cfg(windows)]
    {
        if let Some(unc) = value.strip_prefix(r"\\?\UNC\") {
            return Ok(format!(r"\\{unc}"));
        }
        if let Some(drive) = value.strip_prefix(r"\\?\") {
            return Ok(drive.to_owned());
        }
    }
    Ok(value.to_owned())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::{Read, Write};

    fn fixture() -> (tempfile::TempDir, HostIdentity) {
        let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../cpp/build/shared-serving/tests");
        fs::create_dir_all(&root).unwrap();
        let dir = tempfile::tempdir_in(root).unwrap();
        let config = SharedServiceConfig {
            directory: dir.path().join("state"),
            executable: dir.path().join("host"),
            library: dir.path().join("native"),
            model_cache_dir: dir.path().join("models"),
            app_data_dir: dir.path().join("data"),
            execution_providers: Vec::new(),
        };
        create_private_directory(&config.directory).unwrap();
        fs::create_dir_all(&config.model_cache_dir).unwrap();
        fs::create_dir_all(&config.app_data_dir).unwrap();
        fs::write(&config.executable, "test host").unwrap();
        fs::write(&config.library, "test native").unwrap();
        let identity = identity(config).unwrap();
        (dir, identity)
    }

    #[test]
    fn stale_record_is_absent_and_incompatible_live_host_is_not_adopted() {
        let (_dir, identity) = fixture();
        let ready = identity.config.directory.join("ready.json");
        let record = HostRecord {
            identity: identity.clone(),
            pid: 123,
            endpoint: "http://127.0.0.1:12345".into(),
        };
        fs::write(&ready, serde_json::to_vec(&record).unwrap()).unwrap();
        assert!(read_live_record(&identity).unwrap().is_none());
        let lock = open_lock(&identity.config.directory.join("host.lock")).unwrap();
        lock.lock_exclusive().unwrap();
        assert!(read_live_record(&identity).unwrap().is_some());
        let mut changed = identity.clone();
        changed.sdk_version = "different".into();
        assert!(read_live_record(&changed).is_err());
        assert!(ready.exists(), "incompatible host must not be removed");
    }

    #[test]
    fn native_replacement_changes_compatibility() {
        let (_dir, old) = fixture();
        fs::write(&old.config.library, "replaced native").unwrap();
        let updated = identity(old.config.clone()).unwrap();
        assert_ne!(old, updated);
    }

    #[test]
    fn adjacent_runtime_replacement_changes_compatibility() {
        let (_dir, identity_before) = fixture();
        let name = if cfg!(windows) {
            "onnxruntime.dll"
        } else if cfg!(target_os = "macos") {
            "libonnxruntime.dylib"
        } else {
            "libonnxruntime.so"
        };
        let dependency = identity_before.config.library.with_file_name(name);
        fs::write(&dependency, "runtime one").unwrap();
        let before = identity(identity_before.config.clone()).unwrap();
        fs::write(&dependency, "runtime two").unwrap();
        let after = identity(identity_before.config).unwrap();
        assert_ne!(before, after);
    }

    fn http_reply(status: &str, body: &str) -> (String, std::thread::JoinHandle<String>) {
        let listener = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
        let endpoint = format!("http://{}", listener.local_addr().unwrap());
        let response = format!(
            "HTTP/1.1 {status}\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{body}",
            body.len()
        );
        let thread = std::thread::spawn(move || {
            let (mut socket, _) = listener.accept().unwrap();
            socket
                .set_read_timeout(Some(Duration::from_secs(5)))
                .unwrap();
            let mut request = Vec::new();
            let mut byte = [0];
            while !request.ends_with(b"\r\n\r\n") {
                assert_eq!(socket.read(&mut byte).unwrap(), 1);
                request.push(byte[0]);
            }
            socket.write_all(response.as_bytes()).unwrap();
            String::from_utf8(request).unwrap()
        });
        (endpoint, thread)
    }

    #[tokio::test]
    async fn load_routes_exact_local_id_and_busy_unload_is_an_error() {
        let (_dir, identity) = fixture();
        let model_path = identity.config.model_cache_dir.join("weights + v1");
        fs::create_dir(&model_path).unwrap();
        let body = serde_json::json!({
            "status": "loaded",
            "model_id": "coding:1",
            "model_path": model_path,
            "context_length": 4096
        })
        .to_string();
        let (endpoint, server) = http_reply("200 OK", &body);
        let connection = SharedServiceConnection {
            endpoint,
            client: reqwest::Client::builder().no_proxy().build().unwrap(),
        };
        let state = connection
            .load(CatalogType::Local, "coding:1", &model_path.join("."))
            .await
            .unwrap();
        assert_eq!(state.context_length, Some(4096));
        let request = server.join().unwrap();
        assert!(request.starts_with("GET /catalogs/local/models/load/coding%3A1?"));
        let url = reqwest::Url::parse(&format!(
            "http://localhost{}",
            request.split_whitespace().nth(1).unwrap()
        ))
        .unwrap();
        assert_eq!(
            url.query_pairs().collect::<Vec<_>>(),
            vec![(
                "expected_model_path".into(),
                native_path(&fs::canonicalize(&model_path).unwrap())
                    .unwrap()
                    .into()
            )]
        );
        let (endpoint, server) = http_reply("409 Conflict", r#"{"error":"model in use"}"#);
        let connection = SharedServiceConnection {
            endpoint,
            ..connection
        };
        let error = connection
            .unload(CatalogType::Public, "coding:1", &model_path)
            .await
            .unwrap_err();
        assert!(error.to_string().contains("409"), "{error}");
        assert!(server
            .join()
            .unwrap()
            .starts_with("GET /models/unload/coding%3A1?expected_model_path="));
    }

    #[tokio::test]
    async fn malformed_loaded_state_is_not_an_empty_success() {
        let (endpoint, server) = http_reply("200 OK", "{}");
        let connection = SharedServiceConnection {
            endpoint,
            client: reqwest::Client::builder().no_proxy().build().unwrap(),
        };
        assert!(connection.loaded_models(CatalogType::Public).await.is_err());
        server.join().unwrap();
    }

    #[tokio::test]
    async fn unload_validates_path_without_requiring_cached_weights() {
        let (_dir, identity) = fixture();
        let path = identity.config.model_cache_dir.join("removed-weights");
        let (endpoint, server) = http_reply("200 OK", "{}");
        let connection = SharedServiceConnection {
            endpoint,
            client: reqwest::Client::builder().no_proxy().build().unwrap(),
        };
        connection
            .unload(CatalogType::Local, "coding:1", &path)
            .await
            .unwrap();
        server.join().unwrap();
        let error = connection
            .unload(CatalogType::Local, "coding:1", Path::new("relative"))
            .await
            .unwrap_err();
        assert!(error.to_string().contains("must be absolute"), "{error}");
        let invalid = PathBuf::from(format!("{}\0suffix", path.display()));
        let error = connection
            .unload(CatalogType::Local, "coding:1", &invalid)
            .await
            .unwrap_err();
        assert!(
            error.to_string().contains("must not contain NUL"),
            "{error}"
        );
    }

    #[tokio::test]
    async fn detailed_inventory_observes_paths_without_loading_and_rejects_invalid_states() {
        let (_dir, identity) = fixture();
        let path = identity.config.model_cache_dir;
        for (model_id, model_path, context_length, valid) in [
            ("coding:1", path.as_path(), None, true),
            ("", path.as_path(), Some(4096), false),
            ("coding:1", Path::new("relative"), Some(4096), false),
            ("coding:1", path.as_path(), Some(0), false),
        ] {
            let body = serde_json::json!([{
                "model_id": model_id,
                "model_path": model_path,
                "context_length": context_length
            }])
            .to_string();
            let (endpoint, server) = http_reply("200 OK", &body);
            let connection = SharedServiceConnection {
                endpoint,
                client: reqwest::Client::builder().no_proxy().build().unwrap(),
            };
            let result = connection.loaded_model_states(CatalogType::Local).await;
            if valid {
                let states = result.unwrap();
                assert_eq!(states[0].model_path, path);
                assert_eq!(states[0].context_length, None);
            } else {
                assert!(result.is_err());
            }
            assert!(server
                .join()
                .unwrap()
                .starts_with("GET /catalogs/local/models/loaded?details=true "));
        }
    }

    #[tokio::test]
    async fn load_rejects_wrong_identity_invalid_context_and_redirects() {
        let (_dir, identity) = fixture();
        let model_path = identity.config.model_cache_dir;
        for (status, model_id, context_length, returned_path, reason) in [
            (
                "200 OK",
                "different:1",
                4096,
                model_path.as_path(),
                "different model ID",
            ),
            (
                "200 OK",
                "coding:1",
                0,
                model_path.as_path(),
                "invalid loaded context",
            ),
            (
                "200 OK",
                "coding:1",
                4096,
                identity.config.app_data_dir.as_path(),
                "different model artifact",
            ),
            (
                "200 OK",
                "coding:1",
                4096,
                Path::new("relative"),
                "relative model path",
            ),
            ("302 Found", "coding:1", 4096, model_path.as_path(), "302"),
        ] {
            let body = serde_json::json!({
                "model_id": model_id,
                "model_path": returned_path,
                "context_length": context_length
            })
            .to_string();
            let (endpoint, server) = http_reply(status, &body);
            let connection = SharedServiceConnection {
                endpoint,
                client: reqwest::Client::builder().no_proxy().build().unwrap(),
            };
            let error = connection
                .load(CatalogType::Public, "coding:1", &model_path)
                .await
                .unwrap_err();
            assert!(error.to_string().contains(reason), "{error}");
            server.join().unwrap();
        }
    }

    #[tokio::test]
    async fn ready_endpoint_must_belong_to_recorded_process() {
        let (_dir, identity) = fixture();
        let (endpoint, server) = http_reply("200 OK", r#"{"pid":456}"#);
        let record = HostRecord {
            identity,
            endpoint,
            pid: 123,
        };
        assert!(connect_record(&record).await.is_err());
        server.join().unwrap();
    }

    #[tokio::test]
    async fn legacy_protocol_is_rejected_before_any_http_or_control_request() {
        let (_dir, identity) = fixture();
        let listener = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
        listener.set_nonblocking(true).unwrap();
        let mut legacy = identity.clone();
        legacy.protocol = PROTOCOL_VERSION - 1;
        let record = HostRecord {
            identity: legacy,
            pid: 123,
            endpoint: format!("http://{}", listener.local_addr().unwrap()),
        };
        fs::write(
            identity.config.directory.join("ready.json"),
            serde_json::to_vec(&record).unwrap(),
        )
        .unwrap();
        let lock = open_lock(&identity.config.directory.join("host.lock")).unwrap();
        lock.lock_exclusive().unwrap();
        let service = SharedService::new(identity.config).unwrap();
        let error = service.connect().await.err().unwrap();
        assert!(
            error
                .to_string()
                .contains("different shared host configuration"),
            "{error}"
        );
        let error = service.ensure_started().await.err().unwrap();
        assert!(
            error
                .to_string()
                .contains("different shared host configuration"),
            "{error}"
        );
        assert_eq!(
            listener.accept().unwrap_err().kind(),
            std::io::ErrorKind::WouldBlock
        );
    }

    #[tokio::test]
    async fn matching_process_without_native_artifact_contract_is_rejected() {
        let (_dir, identity) = fixture();
        for capability in [None, Some(false)] {
            let mut body = serde_json::json!({"pid":123});
            if let Some(value) = capability {
                body["artifact_qualified_model_management"] = value.into();
            }
            let (endpoint, server) = http_reply("200 OK", &body.to_string());
            let record = HostRecord {
                identity: identity.clone(),
                endpoint,
                pid: 123,
            };
            let error = connect_record(&record).await.err().unwrap();
            assert!(error.to_string().contains("artifact-qualified"), "{error}");
            assert!(server.join().unwrap().starts_with("GET /status "));
        }
    }
}
