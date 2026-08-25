//! Safe wrapper around the dynamically-loaded Foundry Local C ABI.
//!
//! [`Api`] loads the `foundry_local` shared library (pre-loading its ONNX Runtime
//! and GenAI dependencies first), resolves the root function table via
//! `FoundryLocalGetApi`, and caches the sub-API tables. It also provides the
//! `flStatus*` → [`FoundryLocalError`] mapping and small FFI string / key-value
//! helpers used throughout the `detail` layer.

use std::ffi::{CStr, CString};
use std::os::raw::c_char;
use std::path::{Path, PathBuf};
use std::sync::Arc;

use libloading::Library;

use super::ffi::*;
use crate::error::{FoundryLocalError, NativeErrorCode, Result};

// ── Library file names ───────────────────────────────────────────────────────

#[cfg(target_os = "windows")]
const LIB_FILE: &str = "foundry_local.dll";
#[cfg(target_os = "macos")]
const LIB_FILE: &str = "libfoundry_local.dylib";
#[cfg(all(unix, not(target_os = "macos")))]
const LIB_FILE: &str = "libfoundry_local.so";

#[cfg(target_os = "windows")]
const ORT_FILE: &str = "onnxruntime.dll";
#[cfg(target_os = "macos")]
const ORT_FILE: &str = "libonnxruntime.dylib";
#[cfg(all(unix, not(target_os = "macos")))]
const ORT_FILE: &str = "libonnxruntime.so";

#[cfg(target_os = "windows")]
const GENAI_FILE: &str = "onnxruntime-genai.dll";
#[cfg(target_os = "macos")]
const GENAI_FILE: &str = "libonnxruntime-genai.dylib";
#[cfg(all(unix, not(target_os = "macos")))]
const GENAI_FILE: &str = "libonnxruntime-genai.so";

// ── FFI string helpers ───────────────────────────────────────────────────────

/// Build a [`CString`], mapping an interior NUL to a validation error.
pub(crate) fn to_cstring(s: &str) -> Result<CString> {
    CString::new(s).map_err(|e| FoundryLocalError::Validation {
        reason: format!("string contains an interior NUL byte: {e}"),
    })
}

/// Read a borrowed C string into an owned `String`. Returns `None` for null.
///
/// # Safety
/// `ptr` must be null or point to a valid NUL-terminated string that stays alive
/// for the duration of this call.
pub(crate) unsafe fn cstr_to_string(ptr: *const c_char) -> Option<String> {
    if ptr.is_null() {
        None
    } else {
        Some(CStr::from_ptr(ptr).to_string_lossy().into_owned())
    }
}

fn map_error(code: flErrorCode, message: String) -> FoundryLocalError {
    let code = match code {
        FOUNDRY_LOCAL_OK => NativeErrorCode::Ok,
        FOUNDRY_LOCAL_ERROR_NOT_IMPLEMENTED => NativeErrorCode::NotImplemented,
        FOUNDRY_LOCAL_ERROR_INTERNAL => NativeErrorCode::Internal,
        FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT => NativeErrorCode::InvalidArgument,
        FOUNDRY_LOCAL_ERROR_INVALID_USAGE => NativeErrorCode::InvalidUsage,
        FOUNDRY_LOCAL_ERROR_OPERATION_CANCELLED => NativeErrorCode::OperationCancelled,
        FOUNDRY_LOCAL_ERROR_NETWORK => NativeErrorCode::Network,
        _ => NativeErrorCode::Unknown(code),
    };

    FoundryLocalError::Native { code, message }
}

// ── Api ──────────────────────────────────────────────────────────────────────

/// Loaded native library plus its resolved function tables.
///
/// `Api` is `Send + Sync`: the underlying library is thread-safe for distinct
/// handles, and the cached vtable pointers are read-only for the process lifetime.
pub(crate) struct Api {
    _foundry_lib: Library,
    _dep_libs: Vec<Library>,
    root: *const flApiVtable,
    catalog: *const flCatalogApiVtable,
    config: *const flConfigurationApiVtable,
    item: *const flItemApiVtable,
    inference: *const flInferenceApiVtable,
    model: *const flModelApiVtable,
}

// SAFETY: the native library is documented as thread-safe for independent
// handles; the cached vtable pointers are immutable for the library's lifetime.
unsafe impl Send for Api {}
unsafe impl Sync for Api {}

impl std::fmt::Debug for Api {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Api").finish_non_exhaustive()
    }
}

impl Api {
    /// Load the native library and resolve the API tables.
    ///
    /// `library_path` is an optional override pointing at the `foundry_local`
    /// shared library file or the directory containing it.
    pub(crate) fn load(library_path: Option<&str>) -> Result<Self> {
        let (lib_path, native_dir) = resolve_library_path(library_path)?;

        // Pre-load ONNX Runtime then GenAI so the dynamic loader resolves
        // foundry_local's dependencies regardless of rpath/search-path setup.
        let dep_libs = preload_dependencies(native_dir.as_deref());

        // SAFETY: `lib_path` was resolved from trusted configuration / build
        // inputs. Loading a shared library executes foreign initialisers.
        let foundry_lib = unsafe {
            load_library(&lib_path).map_err(|e| match e {
                FoundryLocalError::LibraryLoad { reason } => FoundryLocalError::LibraryLoad {
                    reason: format!(
                        "{reason}. Ensure the native Foundry Local library is available: set \
                         FOUNDRY_LOCAL_LIB_DIR to the directory containing {LIB_FILE} (alongside its \
                         onnxruntime and onnxruntime-genai libraries), pass it via \
                         FoundryLocalConfig::library_path, or place it on the loader search path."
                    ),
                },
                other => other,
            })?
        };

        // SAFETY: the library exports `FoundryLocalGetApi` with this signature.
        let root = unsafe {
            let get_api: libloading::Symbol<FoundryLocalGetApiFn> = foundry_lib
                .get(FOUNDRY_LOCAL_GET_API_SYMBOL)
                .map_err(|e| FoundryLocalError::LibraryLoad {
                    reason: format!("symbol 'FoundryLocalGetApi' not found: {e}"),
                })?;
            let ptr = get_api(FOUNDRY_LOCAL_API_VERSION);
            if ptr.is_null() {
                return Err(FoundryLocalError::LibraryLoad {
                    reason: format!(
                        "FoundryLocalGetApi({FOUNDRY_LOCAL_API_VERSION}) returned null (unsupported API version)"
                    ),
                });
            }
            ptr
        };

        // SAFETY: `root` is a valid, non-null vtable for the library's lifetime.
        let root_ref = unsafe { &*root };
        let catalog = unsafe { (root_ref.GetCatalogApi)() };
        let config = unsafe { (root_ref.GetConfigurationApi)() };
        let item = unsafe { (root_ref.GetItemApi)() };
        let inference = unsafe { (root_ref.GetInferenceApi)() };
        let model = unsafe { (root_ref.GetModelApi)() };

        for (name, ptr) in [
            ("CatalogApi", catalog as *const ()),
            ("ConfigurationApi", config as *const ()),
            ("ItemApi", item as *const ()),
            ("InferenceApi", inference as *const ()),
            ("ModelApi", model as *const ()),
        ] {
            if ptr.is_null() {
                return Err(FoundryLocalError::LibraryLoad {
                    reason: format!("native {name} table is null"),
                });
            }
        }

        Ok(Self {
            _foundry_lib: foundry_lib,
            _dep_libs: dep_libs,
            root,
            catalog,
            config,
            item,
            inference,
            model,
        })
    }

    #[inline]
    pub(crate) fn root(&self) -> &flApiVtable {
        // SAFETY: non-null and valid for the library's lifetime.
        unsafe { &*self.root }
    }
    #[inline]
    pub(crate) fn catalog_api(&self) -> &flCatalogApiVtable {
        unsafe { &*self.catalog }
    }
    #[inline]
    pub(crate) fn config_api(&self) -> &flConfigurationApiVtable {
        unsafe { &*self.config }
    }
    #[inline]
    pub(crate) fn item_api(&self) -> &flItemApiVtable {
        unsafe { &*self.item }
    }
    #[inline]
    pub(crate) fn inference_api(&self) -> &flInferenceApiVtable {
        unsafe { &*self.inference }
    }
    #[inline]
    pub(crate) fn model_api(&self) -> &flModelApiVtable {
        unsafe { &*self.model }
    }

    /// Convert a returned `flStatus*` into a `Result`. A non-null status is an error.
    pub(crate) fn check(&self, status: flStatusPtr) -> Result<()> {
        if status.is_null() {
            return Ok(());
        }
        let root = self.root();
        // SAFETY: `status` is a valid non-null status owned by us until released.
        unsafe {
            let code = (root.Status_GetErrorCode)(status);
            let message = cstr_to_string((root.Status_GetErrorMessage)(status)).unwrap_or_default();
            (root.Status_Release)(status);
            Err(map_error(code, message))
        }
    }

    /// Read a returned `flStatus*` as an optional message. `None` for success
    /// (null status). Releases the status. Used where a non-null status is a
    /// soft/partial failure rather than a hard error (e.g. EP registration).
    pub(crate) fn status_message(&self, status: flStatusPtr) -> Option<String> {
        if status.is_null() {
            return None;
        }
        let root = self.root();
        // SAFETY: `status` is a valid non-null status owned by us until released.
        unsafe {
            let message = cstr_to_string((root.Status_GetErrorMessage)(status)).unwrap_or_default();
            (root.Status_Release)(status);
            Some(message)
        }
    }
}

// ── KeyValuePairs RAII helper ────────────────────────────────────────────────

/// Owning wrapper around a native `flKeyValuePairs`, released on drop.
pub(crate) struct Kvps {
    api: Arc<Api>,
    ptr: *mut flKeyValuePairs,
}

impl Kvps {
    /// Create an empty key/value collection.
    pub(crate) fn new(api: Arc<Api>) -> Self {
        let mut ptr: *mut flKeyValuePairs = std::ptr::null_mut();
        // SAFETY: `CreateKeyValuePairs` writes a valid handle into `ptr`.
        unsafe { (api.root().CreateKeyValuePairs)(&mut ptr) };
        Self { api, ptr }
    }

    /// Build from an iterator of `(key, value)` string pairs.
    pub(crate) fn from_pairs<I, K, V>(api: Arc<Api>, pairs: I) -> Result<Self>
    where
        I: IntoIterator<Item = (K, V)>,
        K: AsRef<str>,
        V: AsRef<str>,
    {
        let mut kvps = Self::new(api);
        for (k, v) in pairs {
            kvps.set(k.as_ref(), v.as_ref())?;
        }
        Ok(kvps)
    }

    /// Add or replace a key/value pair.
    pub(crate) fn set(&mut self, key: &str, value: &str) -> Result<()> {
        let key = to_cstring(key)?;
        let value = to_cstring(value)?;
        // SAFETY: the native call copies both strings; our CStrings outlive the call.
        unsafe { (self.api.root().AddKeyValuePair)(self.ptr, key.as_ptr(), value.as_ptr()) };
        Ok(())
    }

    pub(crate) fn as_ptr(&self) -> *const flKeyValuePairs {
        self.ptr
    }
}

impl Drop for Kvps {
    fn drop(&mut self) {
        if !self.ptr.is_null() {
            // SAFETY: `ptr` was created by `CreateKeyValuePairs` and not yet released.
            unsafe { (self.api.root().KeyValuePairs_Release)(self.ptr) };
            self.ptr = std::ptr::null_mut();
        }
    }
}

/// Read a borrowed native `flKeyValuePairs` into an owned `Vec<(String, Option<String>)>`.
///
/// # Safety
/// `kvps` must be null or a valid pointer that stays alive for the duration of this call.
pub(crate) unsafe fn read_kvps(
    api: &Api,
    kvps: *const flKeyValuePairs,
) -> Vec<(String, Option<String>)> {
    if kvps.is_null() {
        return Vec::new();
    }
    let mut keys: *const *const c_char = std::ptr::null();
    let mut values: *const *const c_char = std::ptr::null();
    let mut count: usize = 0;
    (api.root().GetKeyValuePairs)(kvps, &mut keys, &mut values, &mut count);
    if keys.is_null() || count == 0 {
        return Vec::new();
    }
    let mut out = Vec::with_capacity(count);
    for i in 0..count {
        let key = cstr_to_string(*keys.add(i)).unwrap_or_default();
        let value = if values.is_null() {
            None
        } else {
            cstr_to_string(*values.add(i))
        };
        out.push((key, value));
    }
    out
}

// ── Library discovery & loading ──────────────────────────────────────────────

#[cfg(unix)]
unsafe fn load_library(path: &Path) -> Result<Library> {
    use libloading::os::unix::{Library as UnixLibrary, RTLD_GLOBAL, RTLD_NOW};
    UnixLibrary::open(Some(path), RTLD_NOW | RTLD_GLOBAL)
        .map(Library::from)
        .map_err(|e| FoundryLocalError::LibraryLoad {
            reason: format!("failed to load native library at {}: {e}", path.display()),
        })
}

#[cfg(windows)]
unsafe fn load_library(path: &Path) -> Result<Library> {
    Library::new(path).map_err(|e| FoundryLocalError::LibraryLoad {
        reason: format!("failed to load native library at {}: {e}", path.display()),
    })
}

/// Best-effort pre-load of ORT and GenAI from the native directory. Failures are
/// ignored: foundry_local may resolve them via rpath/search path instead.
fn preload_dependencies(native_dir: Option<&Path>) -> Vec<Library> {
    let mut libs = Vec::new();
    let Some(dir) = native_dir else {
        return libs;
    };

    #[allow(unused_mut)]
    let mut deps: Vec<&str> = vec![ORT_FILE, GENAI_FILE];
    // The single unified runtime package bundles the reg-free WinML 2.x runtime next
    // to foundry_local on Windows. Pre-load it by absolute path (best-effort; only if
    // present) so foundry_local's delay-loaded WinML EP resolves regardless of the
    // process DLL search path — mirroring the ORT/GenAI pre-load above.
    #[cfg(windows)]
    deps.push("Microsoft.Windows.AI.MachineLearning.dll");

    // Help GenAI's dlopen build find the exact ORT we are pre-loading.
    let ort_path = dir.join(ORT_FILE);
    if ort_path.exists() && std::env::var_os("ORT_LIB_PATH").is_none() {
        std::env::set_var("ORT_LIB_PATH", &ort_path);
    }

    for dep in deps {
        let dep_path = dir.join(dep);
        if dep_path.exists() {
            // SAFETY: pre-loading a known dependency from the trusted native dir.
            if let Ok(lib) = unsafe { load_library(&dep_path) } {
                libs.push(lib);
            }
        }
    }
    libs
}

/// Resolve the full path to the `foundry_local` library and its containing dir.
///
/// Search order:
/// 1. `library_path` override (a file or a directory).
/// 2. `FOUNDRY_LOCAL_LIB_DIR` environment variable (dev override).
/// 3. `FOUNDRY_NATIVE_DIR` compile-time path (set by `build.rs`).
/// 4. The directory of the current executable.
/// 5. Bare library name resolved via the system search path.
fn resolve_library_path(library_path: Option<&str>) -> Result<(PathBuf, Option<PathBuf>)> {
    // 1. Explicit override: a file path or a directory. An explicit override is
    //    authoritative — if it cannot resolve the library, fail rather than
    //    silently loading an unrelated one from the environment or system path.
    if let Some(p) = library_path {
        let path = Path::new(p);
        if path.is_file() {
            let dir = path.parent().map(Path::to_path_buf);
            return Ok((path.to_path_buf(), dir));
        }
        let candidate = path.join(LIB_FILE);
        if candidate.is_file() {
            return Ok((candidate, Some(path.to_path_buf())));
        }
        return Err(FoundryLocalError::LibraryLoad {
            reason: format!(
                "configured library_path {p:?} does not resolve the foundry_local library \
                 (expected an existing file, or a directory containing {LIB_FILE})"
            ),
        });
    }

    let mut dirs: Vec<PathBuf> = Vec::new();
    if let Ok(dir) = std::env::var("FOUNDRY_LOCAL_LIB_DIR") {
        if !dir.trim().is_empty() {
            dirs.push(PathBuf::from(dir));
        }
    }
    if let Some(dir) = option_env!("FOUNDRY_NATIVE_DIR") {
        dirs.push(PathBuf::from(dir));
    }
    if let Ok(exe) = std::env::current_exe() {
        if let Some(dir) = exe.parent() {
            dirs.push(dir.to_path_buf());
        }
    }

    for dir in &dirs {
        let candidate = dir.join(LIB_FILE);
        if candidate.is_file() {
            return Ok((candidate, Some(dir.clone())));
        }
    }

    // 5. Fall back to the bare library name (system loader search path).
    Ok((PathBuf::from(LIB_FILE), None))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn map_error_preserves_all_native_codes_and_messages() {
        let cases = [
            (FOUNDRY_LOCAL_OK, NativeErrorCode::Ok),
            (
                FOUNDRY_LOCAL_ERROR_NOT_IMPLEMENTED,
                NativeErrorCode::NotImplemented,
            ),
            (FOUNDRY_LOCAL_ERROR_INTERNAL, NativeErrorCode::Internal),
            (
                FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
                NativeErrorCode::InvalidArgument,
            ),
            (
                FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
                NativeErrorCode::InvalidUsage,
            ),
            (
                FOUNDRY_LOCAL_ERROR_OPERATION_CANCELLED,
                NativeErrorCode::OperationCancelled,
            ),
            (FOUNDRY_LOCAL_ERROR_NETWORK, NativeErrorCode::Network),
            (i32::MAX, NativeErrorCode::Unknown(i32::MAX)),
        ];

        for (raw_code, expected_code) in cases {
            let error = map_error(raw_code, "native message".into());
            assert_eq!(error.native_code(), Some(expected_code));
            assert_eq!(error.native_message(), Some("native message"));
        }
    }

    #[test]
    fn map_error_preserves_empty_native_message() {
        let error = map_error(FOUNDRY_LOCAL_ERROR_OPERATION_CANCELLED, String::new());

        assert_eq!(
            error.native_code(),
            Some(NativeErrorCode::OperationCancelled)
        );
        assert_eq!(error.native_message(), Some(""));
    }
}
