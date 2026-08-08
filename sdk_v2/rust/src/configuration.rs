use std::collections::HashMap;
use std::fmt;
use std::sync::Arc;

use crate::detail::api::{to_cstring, Api, Kvps};
use crate::detail::ffi::*;
use crate::error::{FoundryLocalError, Result};

/// Log level for the Foundry Local service.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum LogLevel {
    Trace,
    Debug,
    Info,
    Warn,
    Error,
    Fatal,
}

impl LogLevel {
    /// Map to the native `flLogLevel` value.
    fn as_native(&self) -> flLogLevel {
        match self {
            Self::Trace => FOUNDRY_LOCAL_LOG_VERBOSE,
            Self::Debug => FOUNDRY_LOCAL_LOG_DEBUG,
            Self::Info => FOUNDRY_LOCAL_LOG_INFO,
            Self::Warn => FOUNDRY_LOCAL_LOG_WARNING,
            Self::Error => FOUNDRY_LOCAL_LOG_ERROR,
            Self::Fatal => FOUNDRY_LOCAL_LOG_FATAL,
        }
    }
}

/// Application-level logger that the SDK can use to emit diagnostic messages.
///
/// This is a stub — the logger is stored in the configuration and passed
/// through to the manager, but it is not wired into the native core yet.
pub trait Logger: Send + Sync {
    /// Log a message at the given severity level.
    fn log(&self, level: LogLevel, message: &str);
}

/// User-facing configuration for initializing the Foundry Local SDK.
///
/// Construct with [`FoundryLocalConfig::new`] and customise via the builder
/// methods:
///
/// ```ignore
/// let config = FoundryLocalConfig::new("my_app")
///     .log_level(LogLevel::Debug)
///     .model_cache_dir("/path/to/cache");
/// ```
#[derive(Default)]
pub struct FoundryLocalConfig {
    app_name: String,
    app_data_dir: Option<String>,
    model_cache_dir: Option<String>,
    logs_dir: Option<String>,
    log_level: Option<LogLevel>,
    web_service_urls: Option<String>,
    service_endpoint: Option<String>,
    library_path: Option<String>,
    additional_settings: Option<HashMap<String, String>>,
    logger: Option<Box<dyn Logger>>,
}

impl fmt::Debug for FoundryLocalConfig {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("FoundryLocalConfig")
            .field("app_name", &self.app_name)
            .field("app_data_dir", &self.app_data_dir)
            .field("model_cache_dir", &self.model_cache_dir)
            .field("logs_dir", &self.logs_dir)
            .field("log_level", &self.log_level)
            .field("web_service_urls", &self.web_service_urls)
            .field("service_endpoint", &self.service_endpoint)
            .field("library_path", &self.library_path)
            .field("additional_settings", &self.additional_settings)
            .field("logger", &self.logger.as_ref().map(|_| ".."))
            .finish()
    }
}

impl FoundryLocalConfig {
    /// Create a new configuration with the given application name.
    ///
    /// All other fields default to `None`. Use the builder methods to
    /// customise:
    ///
    /// ```ignore
    /// let config = FoundryLocalConfig::new("my_app")
    ///     .log_level(LogLevel::Debug)
    ///     .model_cache_dir("/path/to/cache");
    /// ```
    pub fn new(app_name: impl Into<String>) -> Self {
        Self {
            app_name: app_name.into(),
            ..Self::default()
        }
    }

    /// Override the application-data directory.
    pub fn app_data_dir(mut self, dir: impl Into<String>) -> Self {
        self.app_data_dir = Some(dir.into());
        self
    }

    /// Override the model-cache directory.
    pub fn model_cache_dir(mut self, dir: impl Into<String>) -> Self {
        self.model_cache_dir = Some(dir.into());
        self
    }

    /// Override the logs directory.
    pub fn logs_dir(mut self, dir: impl Into<String>) -> Self {
        self.logs_dir = Some(dir.into());
        self
    }

    /// Set the log level.
    pub fn log_level(mut self, level: LogLevel) -> Self {
        self.log_level = Some(level);
        self
    }

    /// Set the web-service listen URLs (e.g. `"http://localhost:5273"`).
    pub fn web_service_urls(mut self, urls: impl Into<String>) -> Self {
        self.web_service_urls = Some(urls.into());
        self
    }

    /// Set an external service endpoint URL.
    pub fn service_endpoint(mut self, endpoint: impl Into<String>) -> Self {
        self.service_endpoint = Some(endpoint.into());
        self
    }

    /// Override the path to the native Foundry Local Core library.
    pub fn library_path(mut self, path: impl Into<String>) -> Self {
        self.library_path = Some(path.into());
        self
    }

    /// Add a single key-value pair to the additional settings map.
    pub fn additional_setting(mut self, key: impl Into<String>, value: impl Into<String>) -> Self {
        self.additional_settings
            .get_or_insert_with(HashMap::new)
            .insert(key.into(), value.into());
        self
    }

    /// Provide an application logger.
    ///
    /// **Not wired to native logging.** The native core's configuration ABI
    /// exposes only a log level and a logs directory (see [`log_level`] and
    /// [`logs_dir`]) — there is no logger-callback hook — so a custom [`Logger`]
    /// cannot currently receive the core's log records. The logger is stored for
    /// forward compatibility and any SDK-side use, but setting it has no effect
    /// on native logging today; use [`log_level`] / [`logs_dir`] to control core
    /// logging.
    ///
    /// [`log_level`]: Self::log_level
    /// [`logs_dir`]: Self::logs_dir
    pub fn logger(mut self, logger: impl Logger + 'static) -> Self {
        self.logger = Some(Box::new(logger));
        self
    }

    // ── Crate-internal helpers ───────────────────────────────────────────────

    /// The configured native library path override, if any.
    pub(crate) fn library_path_ref(&self) -> Option<&str> {
        self.library_path.as_deref()
    }

    /// Take ownership of the configured logger (consumed once by the manager).
    pub(crate) fn take_logger(&mut self) -> Option<Box<dyn Logger>> {
        self.logger.take()
    }

    /// Build a native `flConfiguration` from this configuration.
    ///
    /// Returns [`FoundryLocalError::InvalidConfiguration`] when `app_name` is
    /// empty or blank.
    pub(crate) fn build_native(&self, api: &Arc<Api>) -> Result<NativeConfig> {
        let app_name = self.app_name.trim();
        if app_name.is_empty() {
            return Err(FoundryLocalError::InvalidConfiguration {
                reason: "app_name must be set and non-empty".into(),
            });
        }

        let cfg = NativeConfig::create(Arc::clone(api), app_name)?;
        let c = api.config_api();

        if let Some(dir) = &self.app_data_dir {
            let s = to_cstring(dir)?;
            // SAFETY: ptr is valid; the native call copies the string.
            api.check(unsafe { (c.SetAppDataDir)(cfg.ptr, s.as_ptr()) })?;
        }
        if let Some(dir) = &self.model_cache_dir {
            let s = to_cstring(dir)?;
            api.check(unsafe { (c.SetModelCacheDir)(cfg.ptr, s.as_ptr()) })?;
        }
        if let Some(dir) = &self.logs_dir {
            let s = to_cstring(dir)?;
            api.check(unsafe { (c.SetLogsDir)(cfg.ptr, s.as_ptr()) })?;
        }
        if let Some(level) = self.log_level {
            api.check(unsafe { (c.SetDefaultLogLevel)(cfg.ptr, level.as_native()) })?;
        }
        if let Some(urls) = &self.web_service_urls {
            for url in urls.split(',').map(str::trim).filter(|u| !u.is_empty()) {
                let s = to_cstring(url)?;
                api.check(unsafe { (c.AddWebServiceEndpoint)(cfg.ptr, s.as_ptr()) })?;
            }
        }
        if let Some(endpoint) = &self.service_endpoint {
            let s = to_cstring(endpoint)?;
            api.check(unsafe { (c.SetExternalServiceUrl)(cfg.ptr, s.as_ptr()) })?;
        }
        if let Some(extra) = &self.additional_settings {
            if !extra.is_empty() {
                let kvps = Kvps::from_pairs(Arc::clone(api), extra.iter())?;
                api.check(unsafe { (c.SetAdditionalOptions)(cfg.ptr, kvps.as_ptr()) })?;
            }
        }

        Ok(cfg)
    }
}

/// Owning wrapper around a native `flConfiguration`, released on drop.
pub(crate) struct NativeConfig {
    api: Arc<Api>,
    ptr: *mut flConfiguration,
}

impl NativeConfig {
    fn create(api: Arc<Api>, app_name: &str) -> Result<Self> {
        let name = to_cstring(app_name)?;
        let mut ptr: *mut flConfiguration = std::ptr::null_mut();
        // SAFETY: `Create` writes a valid handle into `ptr` on success.
        let status = unsafe { (api.config_api().Create)(name.as_ptr(), &mut ptr) };
        api.check(status)?;
        Ok(Self { api, ptr })
    }

    pub(crate) fn as_ptr(&self) -> *const flConfiguration {
        self.ptr
    }
}

impl Drop for NativeConfig {
    fn drop(&mut self) {
        if !self.ptr.is_null() {
            // SAFETY: `ptr` was created by `Create` and not yet released.
            unsafe { (self.api.config_api().Configuration_Release)(self.ptr) };
            self.ptr = std::ptr::null_mut();
        }
    }
}
