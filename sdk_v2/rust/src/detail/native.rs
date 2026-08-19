//! Wrappers around native `flModel` / `flCatalog` handles.
//!
//! `flModel*` and `flCatalog*` handles are owned by the native manager and are
//! never released individually; they stay valid only while that manager is
//! alive. Each wrapper therefore holds a strong [`Arc<NativeManager>`] so a
//! handle can safely outlive the
//! [`FoundryLocalManager`](crate::FoundryLocalManager) that created it. A
//! `flModelList*`, by contrast, is owned by the caller: [`collect_models`]
//! eagerly extracts the contained handles and then releases the list.

use std::panic::{catch_unwind, AssertUnwindSafe};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;

use super::api::{cstr_to_string, Api};
use super::ffi::*;
use super::manager::NativeManager;
use crate::error::{FoundryLocalError, Result};

/// A handle to a manager-owned `flModel`, plus a strong reference to the owning
/// native manager so the catalog (and therefore this handle) stays alive for as
/// long as the model is held.
#[derive(Clone)]
pub(crate) struct NativeModel {
    pub(crate) api: Arc<Api>,
    pub(crate) ptr: *mut flModel,
    /// Keeps the native manager — which owns the catalog and this `flModel` —
    /// alive. Propagated to derived models/sessions; never dereferenced here.
    manager: Arc<NativeManager>,
}

// SAFETY: the `flModel` is owned by the native manager kept alive via `manager`,
// and the native implementation is thread-safe for independent operations.
unsafe impl Send for NativeModel {}
unsafe impl Sync for NativeModel {}

impl NativeModel {
    pub(crate) fn new(api: Arc<Api>, ptr: *mut flModel, manager: Arc<NativeManager>) -> Self {
        Self { api, ptr, manager }
    }

    /// Clone the keep-alive handle to the owning native manager.
    pub(crate) fn manager(&self) -> Arc<NativeManager> {
        Arc::clone(&self.manager)
    }

    pub(crate) fn info_ptr(&self) -> Result<*const flModelInfo> {
        let mut info: *const flModelInfo = std::ptr::null();
        // SAFETY: `ptr` is a valid catalog-owned model handle.
        let status = unsafe { (self.api.model_api().GetInfo)(self.ptr, &mut info) };
        self.api.check(status)?;
        Ok(info)
    }

    pub(crate) fn is_cached(&self) -> Result<bool> {
        let mut cached: std::os::raw::c_int = 0;
        let status = unsafe { (self.api.model_api().IsCached)(self.ptr, &mut cached) };
        self.api.check(status)?;
        Ok(cached != 0)
    }

    pub(crate) fn is_loaded(&self) -> Result<bool> {
        let mut loaded: std::os::raw::c_int = 0;
        let status = unsafe { (self.api.model_api().IsLoaded)(self.ptr, &mut loaded) };
        self.api.check(status)?;
        Ok(loaded != 0)
    }

    pub(crate) fn path(&self) -> Result<Option<String>> {
        let mut path: *const std::os::raw::c_char = std::ptr::null();
        let status = unsafe { (self.api.model_api().GetPath)(self.ptr, &mut path) };
        self.api.check(status)?;
        // SAFETY: `path`, when non-null, points to model-owned storage valid now.
        Ok(unsafe { cstr_to_string(path) })
    }

    pub(crate) fn load(&self) -> Result<()> {
        let status = unsafe { (self.api.model_api().Load)(self.ptr) };
        self.api.check(status)
    }

    pub(crate) fn unload(&self) -> Result<()> {
        let status = unsafe { (self.api.model_api().Unload)(self.ptr) };
        self.api.check(status)
    }

    pub(crate) fn remove_from_cache(&self) -> Result<()> {
        let status = unsafe { (self.api.model_api().RemoveFromCache)(self.ptr) };
        self.api.check(status)
    }

    pub(crate) fn get_variants(&self) -> Result<Vec<NativeModel>> {
        let mut list: *mut flModelList = std::ptr::null_mut();
        let status = unsafe { (self.api.model_api().GetVariants)(self.ptr, &mut list) };
        self.api.check(status)?;
        Ok(collect_models(&self.api, &self.manager, list))
    }

    /// Download the model, optionally reporting progress (0.0–100.0) and
    /// honouring a cancellation flag. Blocking.
    pub(crate) fn download(
        &self,
        progress: Option<Box<dyn FnMut(f64) + Send>>,
        cancel_flag: Option<Arc<AtomicBool>>,
    ) -> Result<()> {
        let mut ctx = DownloadCtx {
            progress,
            cancel_flag,
            cancelled: false,
        };
        let callback: flProgressCallback = Some(download_trampoline);
        let user_data = &mut ctx as *mut DownloadCtx as *mut std::ffi::c_void;
        // SAFETY: `ctx` outlives the blocking native call; the trampoline only
        // dereferences `user_data` for the duration of the call.
        let status = unsafe { (self.api.model_api().Download)(self.ptr, callback, user_data) };
        if ctx.cancelled {
            self.api.check(status).ok();
            return Err(FoundryLocalError::CommandExecution {
                reason: "Operation cancelled".into(),
            });
        }
        self.api.check(status)
    }
}

struct DownloadCtx {
    progress: Option<Box<dyn FnMut(f64) + Send>>,
    cancel_flag: Option<Arc<AtomicBool>>,
    cancelled: bool,
}

unsafe extern "C" fn download_trampoline(
    value: f32,
    user_data: *mut std::ffi::c_void,
) -> std::os::raw::c_int {
    if user_data.is_null() {
        return 0;
    }
    let result = catch_unwind(AssertUnwindSafe(|| {
        let ctx = &mut *(user_data as *mut DownloadCtx);
        if ctx
            .cancel_flag
            .as_ref()
            .is_some_and(|f| f.load(Ordering::Relaxed))
        {
            ctx.cancelled = true;
            return 1;
        }
        if let Some(cb) = ctx.progress.as_mut() {
            cb(value as f64);
        }
        0
    }));
    result.unwrap_or(1)
}

/// Eagerly extract all model handles from a `flModelList`, then release the list.
///
/// Each extracted handle is tagged with `manager` so it keeps the owning native
/// manager alive for as long as the handle is held.
pub(crate) fn collect_models(
    api: &Arc<Api>,
    manager: &Arc<NativeManager>,
    list: *mut flModelList,
) -> Vec<NativeModel> {
    if list.is_null() {
        return Vec::new();
    }
    let root = api.root();
    // SAFETY: `list` is a valid list handle we own until released below.
    let size = unsafe { (root.ModelList_Size)(list) };
    let mut out = Vec::with_capacity(size);
    for i in 0..size {
        let m = unsafe { (root.ModelList_GetAt)(list, i) };
        if !m.is_null() {
            out.push(NativeModel::new(Arc::clone(api), m, Arc::clone(manager)));
        }
    }
    unsafe { (root.ModelList_Release)(list) };
    out
}

/// A handle to a manager-owned `flCatalog`, plus a strong reference to the
/// owning native manager so the handle (and the models it produces) outlive the
/// [`FoundryLocalManager`](crate::FoundryLocalManager).
#[derive(Clone)]
pub(crate) struct NativeCatalog {
    pub(crate) api: Arc<Api>,
    pub(crate) ptr: *mut flCatalog,
    /// Keeps the native manager that owns this `flCatalog` alive; also tagged
    /// onto every model produced from this catalog.
    manager: Arc<NativeManager>,
}

// SAFETY: the `flCatalog` is owned by the native manager kept alive via
// `manager`, and the native implementation is thread-safe.
unsafe impl Send for NativeCatalog {}
unsafe impl Sync for NativeCatalog {}

impl NativeCatalog {
    pub(crate) fn new(api: Arc<Api>, ptr: *mut flCatalog, manager: Arc<NativeManager>) -> Self {
        Self { api, ptr, manager }
    }

    pub(crate) fn name(&self) -> Result<String> {
        let mut name: *const std::os::raw::c_char = std::ptr::null();
        let status = unsafe { (self.api.catalog_api().GetName)(self.ptr, &mut name) };
        self.api.check(status)?;
        Ok(unsafe { cstr_to_string(name) }.unwrap_or_default())
    }

    fn list(
        &self,
        f: unsafe extern "system" fn(*const flCatalog, *mut *mut flModelList) -> flStatusPtr,
    ) -> Result<Vec<NativeModel>> {
        let mut list: *mut flModelList = std::ptr::null_mut();
        let status = unsafe { f(self.ptr, &mut list) };
        self.api.check(status)?;
        Ok(collect_models(&self.api, &self.manager, list))
    }

    pub(crate) fn get_models(&self) -> Result<Vec<NativeModel>> {
        self.list(self.api.catalog_api().GetModels)
    }

    pub(crate) fn get_cached_models(&self) -> Result<Vec<NativeModel>> {
        self.list(self.api.catalog_api().GetCachedModels)
    }

    pub(crate) fn get_loaded_models(&self) -> Result<Vec<NativeModel>> {
        self.list(self.api.catalog_api().GetLoadedModels)
    }

    fn lookup(
        &self,
        f: unsafe extern "system" fn(
            *const flCatalog,
            *const std::os::raw::c_char,
            *mut *mut flModel,
        ) -> flStatusPtr,
        key: &str,
    ) -> Result<Option<NativeModel>> {
        let c = super::api::to_cstring(key)?;
        let mut model: *mut flModel = std::ptr::null_mut();
        let status = unsafe { f(self.ptr, c.as_ptr(), &mut model) };
        self.api.check(status)?;
        if model.is_null() {
            Ok(None)
        } else {
            Ok(Some(NativeModel::new(
                Arc::clone(&self.api),
                model,
                Arc::clone(&self.manager),
            )))
        }
    }

    pub(crate) fn get_model(&self, alias: &str) -> Result<Option<NativeModel>> {
        self.lookup(self.api.catalog_api().GetModel, alias)
    }

    pub(crate) fn get_model_variant(&self, model_id: &str) -> Result<Option<NativeModel>> {
        self.lookup(self.api.catalog_api().GetModelVariant, model_id)
    }

    pub(crate) fn get_latest_version(&self, model: &NativeModel) -> Result<NativeModel> {
        let mut latest: *mut flModel = std::ptr::null_mut();
        let status =
            unsafe { (self.api.catalog_api().GetLatestVersion)(self.ptr, model.ptr, &mut latest) };
        self.api.check(status)?;
        if latest.is_null() {
            return Err(FoundryLocalError::ModelOperation {
                reason: "Catalog returned no latest version for the model.".into(),
            });
        }
        Ok(NativeModel::new(
            Arc::clone(&self.api),
            latest,
            Arc::clone(&self.manager),
        ))
    }
}
