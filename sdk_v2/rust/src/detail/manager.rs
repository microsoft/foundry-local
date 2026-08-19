//! Owning wrapper around a native `flManager` plus its discovery / web-service /
//! execution-provider operations.

use std::os::raw::c_char;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};

use super::api::{cstr_to_string, Api};
use super::ffi::*;
use crate::error::{FoundryLocalError, Result};
use crate::types::EpInfo;

/// Serializes native manager creation against release.
///
/// The native core enforces a single live `flManager` (`Manager_Create` fails
/// with `INVALID_USAGE` while one exists). Because an [`Arc<NativeManager>`]'s
/// strong count reaches zero slightly before [`Drop`] finishes `Manager_Release`,
/// a concurrent `create` could otherwise observe "no instance" yet race the
/// in-flight release and be rejected. Holding this lock across both
/// `Manager_Create` and `Manager_Release` closes that window.
static NATIVE_LIFECYCLE: Mutex<()> = Mutex::new(());

/// Lock [`NATIVE_LIFECYCLE`], recovering from poisoning (the guarded `()` has no
/// invariants a panic could break).
fn lock_lifecycle() -> std::sync::MutexGuard<'static, ()> {
    NATIVE_LIFECYCLE
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner())
}

/// Owns a native `flManager`.
///
/// The native handle is released exactly once by [`Drop`] (via
/// [`teardown`](Self::teardown)) when the last owner is dropped — while the ORT
/// runtime is still alive and before the library's C++ static destructors run.
pub(crate) struct NativeManager {
    api: Arc<Api>,
    ptr: *mut flManager,
    /// Set once the native manager has been released, so `shutdown`, explicit
    /// `teardown`, and `Drop` all coordinate to release exactly once.
    released: AtomicBool,
}

// SAFETY: the native manager is thread-safe; shutdown is documented as callable
// from any thread.
unsafe impl Send for NativeManager {}
unsafe impl Sync for NativeManager {}

impl NativeManager {
    /// Create a manager from a fully-built native configuration.
    pub(crate) fn create(api: Arc<Api>, config: *const flConfiguration) -> Result<Self> {
        // Serialize against any in-flight native release (see `NATIVE_LIFECYCLE`).
        let _lifecycle = lock_lifecycle();
        let mut ptr: *mut flManager = std::ptr::null_mut();
        let status = unsafe { (api.root().Manager_Create)(config, &mut ptr) };
        api.check(status)?;
        if ptr.is_null() {
            return Err(FoundryLocalError::Internal {
                reason: "Manager_Create returned a null manager".into(),
            });
        }
        Ok(Self {
            api,
            ptr,
            released: AtomicBool::new(false),
        })
    }

    /// Clone the shared handle to the loaded native API.
    ///
    /// Used when rebuilding an outer manager wrapper around an already-live
    /// native manager, so the existing loaded library is reused rather than
    /// loaded again.
    pub(crate) fn api(&self) -> Arc<Api> {
        Arc::clone(&self.api)
    }

    /// The manager-owned catalog handle.
    pub(crate) fn catalog_ptr(&self) -> Result<*mut flCatalog> {
        let mut catalog: *mut flCatalog = std::ptr::null_mut();
        let status = unsafe { (self.api.root().Manager_GetCatalog)(self.ptr, &mut catalog) };
        self.api.check(status)?;
        if catalog.is_null() {
            return Err(FoundryLocalError::Internal {
                reason: "Manager_GetCatalog returned a null catalog".into(),
            });
        }
        Ok(catalog)
    }

    pub(crate) fn web_service_start(&self) -> Result<()> {
        let status = unsafe { (self.api.root().Manager_WebServiceStart)(self.ptr) };
        self.api.check(status)
    }

    pub(crate) fn web_service_stop(&self) -> Result<()> {
        let status = unsafe { (self.api.root().Manager_WebServiceStop)(self.ptr) };
        self.api.check(status)
    }

    pub(crate) fn web_service_urls(&self) -> Result<Vec<String>> {
        let mut urls: *const *const c_char = std::ptr::null();
        let mut count: usize = 0;
        let status =
            unsafe { (self.api.root().Manager_WebServiceUrls)(self.ptr, &mut urls, &mut count) };
        self.api.check(status)?;
        if urls.is_null() || count == 0 {
            return Ok(Vec::new());
        }
        let mut out = Vec::with_capacity(count);
        for i in 0..count {
            // SAFETY: `urls` points to `count` valid C-string pointers.
            if let Some(s) = unsafe { cstr_to_string(*urls.add(i)) } {
                out.push(s);
            }
        }
        Ok(out)
    }

    pub(crate) fn discover_eps(&self) -> Result<Vec<EpInfo>> {
        let mut eps: *const flEpInfo = std::ptr::null();
        let mut count: usize = 0;
        let status =
            unsafe { (self.api.root().Manager_GetDiscoverableEps)(self.ptr, &mut eps, &mut count) };
        self.api.check(status)?;
        if eps.is_null() || count == 0 {
            return Ok(Vec::new());
        }
        let mut out = Vec::with_capacity(count);
        for i in 0..count {
            // SAFETY: `eps` points to `count` valid flEpInfo structs.
            let ep = unsafe { &*eps.add(i) };
            out.push(EpInfo {
                name: unsafe { cstr_to_string(ep.name) }.unwrap_or_default(),
                is_registered: ep.is_registered,
            });
        }
        Ok(out)
    }

    /// Run an EP download/registration. Returns `Ok(None)` on full success or
    /// `Ok(Some(message))` describing a partial failure (a non-null native
    /// status). Returns `Err` if a requested EP name is not a valid C string.
    pub(crate) fn download_and_register_eps(
        &self,
        names: Option<&[&str]>,
        progress: Option<EpProgressCallback>,
        cancel_flag: Option<Arc<AtomicBool>>,
    ) -> Result<Option<String>> {
        // Build the optional C array of name pointers (kept alive across the
        // call). Convert fallibly: an interior NUL is a caller error. Silently
        // dropping such names could shrink a targeted request to an empty list,
        // which the ABI then treats as "download all discoverable EPs".
        let name_cstrings: Option<Vec<std::ffi::CString>> = match names {
            Some(ns) => Some(
                ns.iter()
                    .map(|n| {
                        std::ffi::CString::new(*n).map_err(|_| FoundryLocalError::Validation {
                            reason: format!(
                                "execution provider name contains an interior NUL byte: {n:?}"
                            ),
                        })
                    })
                    .collect::<Result<Vec<_>>>()?,
            ),
            None => None,
        };
        let name_ptrs: Option<Vec<*const c_char>> = name_cstrings
            .as_ref()
            .map(|cs| cs.iter().map(|c| c.as_ptr()).collect());

        let (names_ptr, names_len) = match &name_ptrs {
            Some(p) if !p.is_empty() => (p.as_ptr(), p.len()),
            _ => (std::ptr::null(), 0usize),
        };

        let mut ctx = EpCtx {
            progress,
            cancel_flag,
            cancelled: false,
        };
        let user_data = &mut ctx as *mut EpCtx as *mut std::ffi::c_void;
        let callback: flEpProgressCallback = Some(ep_trampoline);

        // SAFETY: name pointers and `ctx` outlive this blocking call.
        let status = unsafe {
            (self.api.root().Manager_DownloadAndRegisterEps)(
                self.ptr, names_ptr, names_len, callback, user_data,
            )
        };
        Ok(self.api.status_message(status))
    }

    /// Begin graceful shutdown of the native manager (`Manager_Shutdown`).
    ///
    /// Idempotent and safe to call from any thread. Does **not** release the
    /// native handle — that happens once the last owner is dropped, via
    /// [`teardown`](Self::teardown).
    pub(crate) fn shutdown(&self) -> Result<()> {
        if self.released.load(Ordering::Acquire) {
            return Ok(());
        }
        // SAFETY: `ptr` is a live manager handle (not yet released — guarded above).
        let status = unsafe { (self.api.root().Manager_Shutdown)(self.ptr) };
        self.api.check(status)
    }

    /// Run the prescribed teardown exactly once: `Manager_Shutdown` then
    /// `Manager_Release`.
    ///
    /// Invoked from [`Drop`] when the last owner is released, so the manager's
    /// C++ destructor runs *before* the library's static destructors — avoiding
    /// the spdlog teardown abort (`mutex lock failed`) documented for the other
    /// SDK bindings, and the WebGPU `ReleaseEpFactory` throw that a process-exit
    /// release would trigger. Releasing is always attempted, even if shutdown
    /// errored.
    pub(crate) fn teardown(&self) {
        if self.released.swap(true, Ordering::AcqRel) {
            return;
        }
        // Serialize against a concurrent `create` so the native core never sees
        // an overlapping create/release (see `NATIVE_LIFECYCLE`).
        let _lifecycle = lock_lifecycle();
        // SAFETY: `ptr` was created by Manager_Create and is released exactly
        // once (guarded by the `released` swap above).
        unsafe {
            let status = (self.api.root().Manager_Shutdown)(self.ptr);
            if !status.is_null() {
                (self.api.root().Status_Release)(status);
            }
            (self.api.root().Manager_Release)(self.ptr);
        }
    }
}

impl Drop for NativeManager {
    fn drop(&mut self) {
        self.teardown();
    }
}

/// Boxed `(ep_name, percent)` progress callback.
pub(crate) type EpProgressCallback = Box<dyn FnMut(&str, f64) + Send>;

struct EpCtx {
    progress: Option<EpProgressCallback>,
    cancel_flag: Option<Arc<AtomicBool>>,
    cancelled: bool,
}

unsafe extern "C" fn ep_trampoline(
    ep_name: *const c_char,
    value: f32,
    user_data: *mut std::ffi::c_void,
) -> std::os::raw::c_int {
    if user_data.is_null() {
        return 0;
    }
    let result = catch_unwind(AssertUnwindSafe(|| {
        let ctx = &mut *(user_data as *mut EpCtx);
        if ctx
            .cancel_flag
            .as_ref()
            .is_some_and(|f| f.load(Ordering::Relaxed))
        {
            ctx.cancelled = true;
            return 1;
        }
        if let Some(cb) = ctx.progress.as_mut() {
            let name = cstr_to_string(ep_name).unwrap_or_default();
            cb(&name, value as f64);
        }
        0
    }));
    result.unwrap_or(1)
}
