//! Top-level entry point for the Foundry Local SDK.
//!
//! [`FoundryLocalManager`] initialises the native core library, provides access
//! to the model [`Catalog`], and can start / stop the local web service. While a
//! handle is alive it is shared process-wide (see [`FoundryLocalManager::create`]).

use std::sync::atomic::AtomicBool;
use std::sync::{Arc, Mutex, OnceLock, Weak};

use tokio::sync::Mutex as AsyncMutex;

use crate::catalog::Catalog;
use crate::configuration::{FoundryLocalConfig, Logger};
use crate::detail::api::Api;
use crate::detail::manager::{EpProgressCallback, NativeManager};
use crate::detail::task::spawn_blocking;
use crate::error::{FoundryLocalError, Result};
use crate::types::{EpDownloadResult, EpInfo};

/// Process-wide bookkeeping for the manager singleton.
///
/// The native core permits only one live `flManager` and requires it to remain
/// valid until every `Model`/`Session` derived from it is destroyed. Those
/// derived handles keep the inner [`NativeManager`] alive via [`Arc`], but *not*
/// the outer [`FoundryLocalManager`]. Tracking only the outer would let its
/// [`Weak`] expire while the native manager is still alive through a derived
/// handle, so the next [`create`](FoundryLocalManager::create) would call
/// `Manager_Create` again and be rejected.
///
/// We therefore track both lifetimes:
/// * `outer` — the live wrapper, so concurrent callers share the *same* handle
///   (and thus a single web-service lock); and
/// * `native` — the inner manager, alive via the outer *or* any derived handle,
///   so a caller can reuse the existing native manager (rebuilding a fresh
///   wrapper) instead of attempting a second, rejected `Manager_Create`.
#[derive(Default)]
struct SharedInstance {
    outer: Weak<FoundryLocalManager>,
    native: Weak<NativeManager>,
}

/// Process-wide slot holding the shared-instance bookkeeping.
///
/// Both handles are [`Weak`], so the global never keeps the manager alive past
/// its last strong reference: teardown still runs deterministically via [`Drop`]
/// when the final strong reference (outer or derived) is released — while the
/// ORT runtime is alive and before the library's C++ static destructors run.
///
/// Wrapped in a [`OnceLock`] (rather than a `const` `Mutex::new`) to keep the
/// crate compatible with its minimum supported Rust version.
static INSTANCE: OnceLock<Mutex<SharedInstance>> = OnceLock::new();

/// The lazily-initialised slot holding the shared-instance bookkeeping.
fn instance_slot() -> &'static Mutex<SharedInstance> {
    INSTANCE.get_or_init(|| Mutex::new(SharedInstance::default()))
}

/// Primary entry point for interacting with Foundry Local.
///
/// Obtain a handle with [`FoundryLocalManager::create`]. While at least one
/// handle is alive, every caller shares the same instance.
pub struct FoundryLocalManager {
    native: Arc<NativeManager>,
    catalog: Catalog,
    urls: Mutex<Vec<String>>,
    /// Serialises web-service start/stop so concurrent callers cannot race on
    /// the native lifecycle state (which the C++ core mutates without a lock).
    web_service_lock: AsyncMutex<()>,
    /// Application logger (stub — not yet wired into the native core).
    _logger: Option<Box<dyn Logger>>,
}

type EpDownloadProgressCallback = Box<dyn FnMut(&str, f64) + Send + 'static>;

/// Builder for configuring and running execution provider downloads.
pub struct EpDownloadBuilder<'a> {
    manager: &'a FoundryLocalManager,
    names: Option<Vec<String>>,
    progress_callback: Option<EpDownloadProgressCallback>,
    cancel_flag: Option<Arc<AtomicBool>>,
}

impl<'a> EpDownloadBuilder<'a> {
    fn new(manager: &'a FoundryLocalManager) -> Self {
        Self {
            manager,
            names: None,
            progress_callback: None,
            cancel_flag: None,
        }
    }

    /// Download only the named execution providers.
    pub fn names<I, S>(mut self, names: I) -> Self
    where
        I: IntoIterator<Item = S>,
        S: Into<String>,
    {
        self.names = Some(names.into_iter().map(Into::into).collect());
        self
    }

    /// Report per-EP download progress as `(ep_name, percent)`.
    pub fn progress<F>(mut self, callback: F) -> Self
    where
        F: FnMut(&str, f64) + Send + 'static,
    {
        self.progress_callback = Some(Box::new(callback));
        self
    }

    /// Cancel the download when `cancel_flag` is set to `true`.
    pub fn cancel(mut self, cancel_flag: Arc<AtomicBool>) -> Self {
        self.cancel_flag = Some(cancel_flag);
        self
    }

    /// Run the configured execution provider download.
    pub async fn run(self) -> Result<EpDownloadResult> {
        self.manager
            .download_and_register_eps_impl(self.names, self.progress_callback, self.cancel_flag)
            .await
    }
}

impl FoundryLocalManager {
    /// Initialise the SDK and return a shared handle to the manager.
    ///
    /// While at least one handle is alive — this `Arc`, or any
    /// [`Model`](crate::Model), client, or session derived from it, each of
    /// which keeps the native manager alive — every call returns a handle to the
    /// **same** native manager and the `config` passed to later calls is ignored.
    /// If the `Arc` returned here is dropped while a derived handle is still
    /// alive, a subsequent call rebuilds a lightweight wrapper around the
    /// still-live native manager rather than creating a second one (the core
    /// permits only one). Once every handle is gone, the next call performs a
    /// fresh initialisation from the new `config`.
    ///
    /// Teardown runs via [`Drop`] when the final handle is released — not via a
    /// process-exit hook — so the native manager (and its EP unregistration)
    /// shuts down while the engine / ORT runtime is still alive. This matches
    /// the C++ SDK's local-`Manager` semantics and avoids the WebGPU
    /// `ReleaseEpFactory` teardown throw (ORT #29206).
    pub fn create(config: FoundryLocalConfig) -> Result<Arc<Self>> {
        // Hold the lock across the whole decision so the reuse check, any native
        // creation, and the slot update are atomic; concurrent callers then
        // observe and share the result. A poisoned lock is recoverable: the
        // guarded weak handles are valid regardless of panics.
        let mut slot = instance_slot()
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());

        // 1. A live outer wrapper already exists: share it.
        if let Some(existing) = slot.outer.upgrade() {
            return Ok(existing);
        }

        // 2. The outer wrapper is gone, but the native manager is still alive via
        //    a derived Model/client/session. Rebuild a wrapper around it rather
        //    than calling `Manager_Create` again (the core permits only one live
        //    manager and would reject a second creation).
        if let Some(native) = slot.native.upgrade() {
            let manager = Self::wrap_existing(native, config)?;
            slot.outer = Arc::downgrade(&manager);
            return Ok(manager);
        }

        // 3. Nothing is alive: perform a full initialisation.
        let manager = Self::initialise(config)?;
        slot.native = Arc::downgrade(&manager.native);
        slot.outer = Arc::downgrade(&manager);
        Ok(manager)
    }

    /// Perform a full initialisation: load the library, create the native
    /// manager, and build the catalog view and outer wrapper.
    fn initialise(mut config: FoundryLocalConfig) -> Result<Arc<Self>> {
        let api = Arc::new(Api::load(config.library_path_ref())?);
        let logger = config.take_logger();
        let native_config = config.build_native(&api)?;

        let native = Arc::new(NativeManager::create(
            Arc::clone(&api),
            native_config.as_ptr(),
        )?);

        let catalog_ptr = native.catalog_ptr()?;
        let catalog = Catalog::new(Arc::clone(&api), catalog_ptr, Arc::clone(&native))?;

        Ok(Arc::new(FoundryLocalManager {
            native,
            catalog,
            urls: Mutex::new(Vec::new()),
            web_service_lock: AsyncMutex::new(()),
            _logger: logger,
        }))
    }

    /// Build a fresh outer wrapper around an already-live native manager.
    ///
    /// Reuses the existing native manager (and its loaded library), so no second
    /// `Manager_Create` is attempted; only the outer-side state (catalog view,
    /// URL cache, web-service lock, logger) is rebuilt. The rest of `config` is
    /// ignored, matching the process-wide singleton contract.
    fn wrap_existing(
        native: Arc<NativeManager>,
        mut config: FoundryLocalConfig,
    ) -> Result<Arc<Self>> {
        let logger = config.take_logger();
        let catalog_ptr = native.catalog_ptr()?;
        let catalog = Catalog::new(native.api(), catalog_ptr, Arc::clone(&native))?;

        // The live native manager may already be running a web service (a prior
        // outer wrapper started it, then was dropped while a derived handle kept
        // the native alive). Seed the URL cache from native so `urls()` reflects
        // the running service instead of misreporting it as stopped. This is a
        // non-throwing native getter that returns empty when nothing is running.
        let urls = native.web_service_urls().unwrap_or_default();

        Ok(Arc::new(FoundryLocalManager {
            native,
            catalog,
            urls: Mutex::new(urls),
            web_service_lock: AsyncMutex::new(()),
            _logger: logger,
        }))
    }

    /// Access the model catalog.
    pub fn catalog(&self) -> &Catalog {
        &self.catalog
    }

    /// Begin a graceful shutdown of the local engine.
    ///
    /// Stops the web service, prevents new model loads, stops existing
    /// sessions, and unloads models. Idempotent and safe to call from any
    /// thread.
    ///
    /// Calling this is optional: the native manager is released automatically
    /// when the last handle is dropped. Use it when you want to deterministically
    /// wind the engine down before releasing the handle. After calling
    /// `shutdown`, the manager should not be used for further inference.
    pub fn shutdown(&self) -> Result<()> {
        self.native.shutdown()
    }

    /// URLs that the local web service is listening on.
    ///
    /// Empty until [`Self::start_web_service`] has been called.
    pub fn urls(&self) -> Result<Vec<String>> {
        let lock = self.urls.lock().map_err(|_| FoundryLocalError::Internal {
            reason: "Failed to acquire urls lock".into(),
        })?;
        Ok(lock.clone())
    }

    /// Start the local web service.
    ///
    /// The listening URLs are stored internally and can be retrieved via
    /// [`Self::urls`] after this method returns.
    pub async fn start_web_service(&self) -> Result<()> {
        let _guard = self.web_service_lock.lock().await;
        let native = Arc::clone(&self.native);
        let urls = spawn_blocking(move || {
            native.web_service_start()?;
            native.web_service_urls()
        })
        .await?;
        *self.urls.lock().map_err(|_| FoundryLocalError::Internal {
            reason: "Failed to acquire urls lock".into(),
        })? = urls;
        Ok(())
    }

    /// Stop the local web service.
    pub async fn stop_web_service(&self) -> Result<()> {
        let _guard = self.web_service_lock.lock().await;
        let native = Arc::clone(&self.native);
        spawn_blocking(move || native.web_service_stop()).await?;
        self.urls
            .lock()
            .map_err(|_| FoundryLocalError::Internal {
                reason: "Failed to acquire urls lock".into(),
            })?
            .clear();
        Ok(())
    }

    /// Discover available execution providers and their registration status.
    pub fn discover_eps(&self) -> Result<Vec<EpInfo>> {
        self.native.discover_eps()
    }

    /// Download and register execution providers.
    ///
    /// If `names` is `None` or empty, all available EPs are downloaded.
    /// Otherwise only the named EPs are downloaded and registered.
    pub async fn download_and_register_eps(
        &self,
        names: Option<&[&str]>,
    ) -> Result<EpDownloadResult> {
        let names = names.map(|n| n.iter().map(|s| s.to_string()).collect::<Vec<_>>());
        self.download_and_register_eps_impl(names, None, None).await
    }

    /// Download and register execution providers, reporting per-EP progress.
    ///
    /// If `names` is `None` or empty, all available EPs are downloaded.
    /// Otherwise only the named EPs are downloaded and registered.
    ///
    /// `progress_callback` receives `(ep_name, percent)` where `percent`
    /// ranges from 0.0 to 100.0 as each EP downloads.
    pub async fn download_and_register_eps_with_progress<F>(
        &self,
        names: Option<&[&str]>,
        progress_callback: F,
    ) -> Result<EpDownloadResult>
    where
        F: FnMut(&str, f64) + Send + 'static,
    {
        let names = names.map(|n| n.iter().map(|s| s.to_string()).collect::<Vec<_>>());
        self.download_and_register_eps_impl(names, Some(Box::new(progress_callback)), None)
            .await
    }

    /// Configure and run execution provider downloads with a builder.
    ///
    /// Use this for call sites that need names, progress, cancellation, or
    /// future download options.
    pub fn download_and_register_eps_builder(&self) -> EpDownloadBuilder<'_> {
        EpDownloadBuilder::new(self)
    }

    async fn download_and_register_eps_impl(
        &self,
        names: Option<Vec<String>>,
        progress_callback: Option<EpDownloadProgressCallback>,
        cancel_flag: Option<Arc<AtomicBool>>,
    ) -> Result<EpDownloadResult> {
        let native = Arc::clone(&self.native);

        // Snapshot requested EP names (default: all discoverable). A discovery
        // failure must propagate rather than collapse into an empty set, which
        // would silently invoke the "download all" path and report success for
        // zero requested providers.
        let requested: Vec<String> = match &names {
            Some(n) if !n.is_empty() => n.clone(),
            _ => native.discover_eps()?.into_iter().map(|e| e.name).collect(),
        };

        let (message, after) = spawn_blocking(move || {
            let name_refs: Option<Vec<&str>> = names
                .as_ref()
                .map(|n| n.iter().map(String::as_str).collect());
            let progress: Option<EpProgressCallback> =
                progress_callback.map(|cb| cb as EpProgressCallback);
            let message =
                native.download_and_register_eps(name_refs.as_deref(), progress, cancel_flag)?;
            // Re-query registration state to synthesise the per-EP result; an
            // observation failure here must surface, not be masked as empty.
            let after = native.discover_eps()?;
            Ok::<(Option<String>, Vec<EpInfo>), FoundryLocalError>((message, after))
        })
        .await?;

        let registered_eps: Vec<String> = requested
            .iter()
            .filter(|name| after.iter().any(|e| &e.name == *name && e.is_registered))
            .cloned()
            .collect();
        let failed_eps: Vec<String> = requested
            .iter()
            .filter(|name| !registered_eps.contains(*name))
            .cloned()
            .collect();

        let success = message.is_none() && failed_eps.is_empty();
        let status = match &message {
            None => "All requested execution providers were registered successfully.".to_string(),
            Some(msg) if msg.is_empty() => {
                "One or more execution providers failed to register.".to_string()
            }
            Some(msg) => msg.clone(),
        };

        let result = EpDownloadResult {
            success,
            status,
            registered_eps,
            failed_eps,
        };

        // Invalidate the catalog cache if any EP was newly registered so the next
        // access re-fetches models with the updated set of available EPs.
        if result.success || !result.registered_eps.is_empty() {
            let _ = self.catalog.update_models().await;
        }

        Ok(result)
    }
}
