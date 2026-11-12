//! Native-boundary mocks: registry HTTP/authentication/caching live in C++, not Rust.
use super::*;
use crate::detail::manager::NativeManager;
use crate::types::CatalogType;
use std::cell::RefCell;

#[derive(Default)]
struct Calls {
    versions: Vec<u32>,
    legacy: Vec<flCatalogType>,
    options: Vec<(flCatalogType, CatalogOptions)>,
    allocated: usize,
    released: usize,
    statuses_released: usize,
    fail: bool,
    null_catalog: bool,
    null_pairs: bool,
}

thread_local! {
    static CALLS: RefCell<Calls> = RefCell::new(Calls::default());
}

unsafe extern "system" fn create_pairs(out: *mut *mut flKeyValuePairs) {
    CALLS.with(|calls| {
        let mut calls = calls.borrow_mut();
        calls.allocated += 1;
        if !calls.null_pairs {
            *out = Box::into_raw(Box::new(CatalogOptions::new())).cast();
        }
    });
}

unsafe extern "system" fn add_pair(
    pairs: *mut flKeyValuePairs,
    key: *const c_char,
    value: *const c_char,
) {
    let pairs = &mut *pairs.cast::<CatalogOptions>();
    pairs.insert(cstr_to_string(key).unwrap(), cstr_to_string(value).unwrap());
}

unsafe extern "system" fn release_pairs(pairs: *mut flKeyValuePairs) {
    drop(Box::from_raw(pairs.cast::<CatalogOptions>()));
    CALLS.with(|calls| calls.borrow_mut().released += 1);
}

unsafe extern "system" fn catalog_with_options(
    manager: *const flManager,
    kind: flCatalogType,
    pairs: *const flKeyValuePairs,
    out: *mut *mut flCatalog,
) -> flStatusPtr {
    assert!(!manager.is_null());
    CALLS.with(|calls| {
        let mut calls = calls.borrow_mut();
        calls
            .options
            .push((kind, (*pairs.cast::<CatalogOptions>()).clone()));
        if calls.fail {
            return std::ptr::NonNull::<flStatus>::dangling().as_ptr();
        }
        if !calls.null_catalog {
            *out = std::ptr::NonNull::<flCatalog>::dangling().as_ptr();
        }
        std::ptr::null_mut()
    })
}

unsafe extern "system" fn catalog_by_type(
    _: *const flManager,
    kind: flCatalogType,
    out: *mut *mut flCatalog,
) -> flStatusPtr {
    CALLS.with(|calls| calls.borrow_mut().legacy.push(kind));
    *out = std::ptr::NonNull::<flCatalog>::dangling().as_ptr();
    std::ptr::null_mut()
}

unsafe extern "system" fn create_manager(
    _: *const flConfiguration,
    out: *mut *mut flManager,
) -> flStatusPtr {
    *out = std::ptr::NonNull::<flManager>::dangling().as_ptr();
    std::ptr::null_mut()
}

unsafe extern "system" fn release_manager(_: *mut flManager) {}

unsafe extern "system" fn shutdown(_: *mut flManager) -> flStatusPtr {
    std::ptr::null_mut()
}

unsafe extern "system" fn release_status(_: *mut flStatus) {
    CALLS.with(|calls| calls.borrow_mut().statuses_released += 1);
}

unsafe extern "system" fn error_code(_: *const flStatus) -> flErrorCode {
    FOUNDRY_LOCAL_ERROR_NETWORK
}

unsafe extern "system" fn error_message(_: *const flStatus) -> *const c_char {
    b"registry unavailable\0".as_ptr().cast()
}

macro_rules! unused {
    (($($arg:ident: $ty:ty),*) $(-> $ret:ty)?) => {{
        unsafe extern "system" fn stub($($arg: $ty),*) $(-> $ret)? {
            $(let _ = $arg;)*
            unreachable!("unexpected native call")
        }
        stub
    }};
}

struct Fixture {
    manager: NativeManager,
    // Keep the mock table alive until after the manager and its Api are dropped.
    _table: Box<flApiV4Vtable>,
}

impl Fixture {
    fn new(version: u32) -> Self {
        CALLS.with(|calls| *calls.borrow_mut() = Calls::default());
        let table = Box::new(flApiV4Vtable {
            v3: flApiVtable {
                Status_Create: unused!((code: flErrorCode, message: *const c_char) -> flStatusPtr),
                Status_Release: release_status,
                Status_GetErrorCode: error_code,
                Status_GetErrorMessage: error_message,
                Manager_Create: create_manager,
                Manager_Release: release_manager,
                Manager_GetCatalog: unused!((manager: *const flManager, out: *mut *mut flCatalog) -> flStatusPtr),
                Manager_WebServiceStart: unused!((manager: *mut flManager) -> flStatusPtr),
                Manager_WebServiceUrls: unused!((manager: *const flManager, urls: *mut *const *const c_char, count: *mut usize) -> flStatusPtr),
                Manager_WebServiceStop: unused!((manager: *mut flManager) -> flStatusPtr),
                GetCatalogApi: unused!(() -> *const flCatalogApiVtable),
                GetConfigurationApi: unused!(() -> *const flConfigurationApiVtable),
                GetItemApi: unused!(() -> *const flItemApiVtable),
                GetInferenceApi: unused!(() -> *const flInferenceApiVtable),
                GetModelApi: unused!(() -> *const flModelApiVtable),
                CreateKeyValuePairs: create_pairs,
                AddKeyValuePair: add_pair,
                GetKeyValue: unused!((pairs: *const flKeyValuePairs, key: *const c_char) -> *const c_char),
                GetKeyValuePairs: unused!((pairs: *const flKeyValuePairs, keys: *mut *const *const c_char, values: *mut *const *const c_char, count: *mut usize)),
                RemoveKeyValuePair: unused!((pairs: *mut flKeyValuePairs, key: *const c_char)),
                KeyValuePairs_Release: release_pairs,
                ModelList_Release: unused!((list: *mut flModelList)),
                ModelList_Size: unused!((list: *const flModelList) -> usize),
                ModelList_GetAt: unused!((list: *const flModelList, index: usize) -> *mut flModel),
                Manager_GetDiscoverableEps: unused!((manager: *const flManager, eps: *mut *const flEpInfo, count: *mut usize) -> flStatusPtr),
                Manager_DownloadAndRegisterEps: unused!((manager: *mut flManager, names: *const *const c_char, count: usize, callback: flEpProgressCallback, data: *mut std::ffi::c_void) -> flStatusPtr),
                Manager_IsEpDownloadInProgress: unused!((manager: *const flManager) -> bool),
                Manager_Shutdown: shutdown,
                Manager_IsShutdownRequested: unused!((manager: *const flManager) -> bool),
                Manager_GetCatalogByType: catalog_by_type,
            },
            Manager_GetCatalogWithOptions: Some(catalog_with_options),
        });
        #[cfg(windows)]
        let library = libloading::os::windows::Library::this().unwrap().into();
        #[cfg(unix)]
        let library = libloading::os::unix::Library::this().into();
        let api = Arc::new(Api {
            _foundry_lib: library,
            _dep_libs: vec![],
            root: &table.v3,
            catalog: std::ptr::null(),
            config: std::ptr::null(),
            shared_host_setting: None,
            catalog_with_options: unsafe { read_catalog_with_options(&table.v3, version) },
            item: std::ptr::null(),
            inference: std::ptr::null(),
            model: std::ptr::null(),
        });
        let manager = NativeManager::create(api, std::ptr::null(), false).unwrap();
        Self {
            manager,
            _table: table,
        }
    }

    fn get(&self, options: &CatalogOptions) -> Result<*mut flCatalog> {
        self.manager
            .catalog_ptr_with_options(CatalogType::AzureRegistry.as_native(), options)
    }
}

fn settings() -> CatalogOptions {
    CatalogOptions::from([
        ("name".into(), "résearch-registry".into()),
        ("credential".into(), "azure_cli".into()),
    ])
}

#[test]
fn registry_forwards_strings_and_reuses_native_identity_without_a_binding_cache() {
    let fixture = Fixture::new(4);
    let options = settings();
    let first = fixture.get(&options).unwrap();
    assert_eq!(fixture.get(&options).unwrap(), first);
    let mut other = options.clone();
    other.insert("credential".into(), "workload_identity".into());
    fixture.get(&other).unwrap();
    CALLS.with(|calls| {
        let calls = calls.borrow();
        assert_eq!(
            calls.options,
            vec![(2, options.clone()), (2, options), (2, other)]
        );
        assert!(calls.legacy.is_empty());
        assert_eq!((calls.allocated, calls.released), (3, 3));
    });
}

#[test]
fn empty_options_preserve_public_and_local_legacy_routes_on_all_versions() {
    for version in [2, 3, 4] {
        let fixture = Fixture::new(version);
        for kind in [CatalogType::Public, CatalogType::Local] {
            fixture
                .manager
                .catalog_ptr_by_type(kind.as_native())
                .unwrap();
            fixture
                .manager
                .catalog_ptr_with_options(kind.as_native(), &CatalogOptions::new())
                .unwrap();
        }
        CALLS.with(|calls| {
            let calls = calls.borrow();
            assert_eq!(calls.legacy, [0, 0, 1, 1]);
            assert!(calls.options.is_empty());
            assert_eq!(calls.allocated, 0);
        });
    }
}

#[test]
fn legacy_runtimes_reject_registry_and_nonempty_options_without_allocation() {
    for version in [2, 3] {
        let fixture = Fixture::new(version);
        for options in [CatalogOptions::new(), settings()] {
            let error = fixture.get(&options).unwrap_err();
            assert_eq!(error.native_code(), Some(NativeErrorCode::NotImplemented));
            assert!(error.to_string().contains("version 4"));
        }
        assert!(fixture
            .manager
            .catalog_ptr_with_options(0, &settings())
            .is_err());
        CALLS.with(|calls| {
            let calls = calls.borrow();
            assert!(calls.legacy.is_empty());
            assert!(calls.options.is_empty());
            assert_eq!(calls.allocated, 0);
        });
    }
}

#[test]
fn invalid_option_strings_never_reach_native_code() {
    let fixture = Fixture::new(4);
    for (key, value) in [("", "x"), ("na\0me", "x"), ("name", "a\0b")] {
        let options = CatalogOptions::from([(key.into(), value.into())]);
        assert!(matches!(
            fixture.get(&options),
            Err(FoundryLocalError::Validation { .. })
        ));
    }
    CALLS.with(|calls| assert_eq!(calls.borrow().allocated, 0));
}

#[test]
fn native_errors_release_options_and_status_and_allow_retry() {
    let fixture = Fixture::new(4);
    CALLS.with(|calls| calls.borrow_mut().fail = true);
    let error = fixture.get(&settings()).unwrap_err();
    assert_eq!(error.native_code(), Some(NativeErrorCode::Network));
    assert_eq!(error.native_message(), Some("registry unavailable"));
    CALLS.with(|calls| calls.borrow_mut().fail = false);
    fixture.get(&settings()).unwrap();
    CALLS.with(|calls| {
        let calls = calls.borrow();
        assert_eq!((calls.allocated, calls.released), (2, 2));
        assert_eq!(calls.statuses_released, 1);
        assert_eq!(calls.options.len(), 2);
    });
}

#[test]
fn null_catalog_is_rejected_and_not_cached() {
    let fixture = Fixture::new(4);
    CALLS.with(|calls| calls.borrow_mut().null_catalog = true);
    assert!(matches!(
        fixture.get(&settings()),
        Err(FoundryLocalError::Internal { .. })
    ));
    CALLS.with(|calls| calls.borrow_mut().null_catalog = false);
    fixture.get(&settings()).unwrap();
    CALLS.with(|calls| {
        let calls = calls.borrow();
        assert_eq!((calls.allocated, calls.released), (2, 2));
    });
}

#[test]
fn null_options_allocation_is_rejected_before_use() {
    let fixture = Fixture::new(4);
    CALLS.with(|calls| calls.borrow_mut().null_pairs = true);
    assert!(matches!(
        fixture.get(&settings()),
        Err(FoundryLocalError::Internal { .. })
    ));
    CALLS.with(|calls| {
        let calls = calls.borrow();
        assert!(calls.options.is_empty());
        assert_eq!(calls.released, 0);
    });
}

#[test]
fn empty_registry_options_still_use_v4_for_native_validation() {
    let fixture = Fixture::new(4);
    fixture.get(&CatalogOptions::new()).unwrap();
    CALLS.with(|calls| assert_eq!(calls.borrow().options, [(2, CatalogOptions::new())]));
}

#[test]
fn legacy_tables_never_read_the_v4_slot() {
    for version in [2, 3] {
        assert!(unsafe { read_catalog_with_options(std::ptr::null(), version) }.is_none());
    }
}

#[test]
fn negotiation_prefers_v4_then_v3_then_v2() {
    unsafe extern "system" fn v4(version: u32) -> *const flApiVtable {
        CALLS.with(|calls| calls.borrow_mut().versions.push(version));
        std::ptr::NonNull::<flApiVtable>::dangling().as_ptr()
    }
    unsafe extern "system" fn v3(version: u32) -> *const flApiVtable {
        CALLS.with(|calls| calls.borrow_mut().versions.push(version));
        if version <= 3 {
            std::ptr::NonNull::<flApiVtable>::dangling().as_ptr()
        } else {
            std::ptr::null()
        }
    }
    unsafe extern "system" fn v2(version: u32) -> *const flApiVtable {
        CALLS.with(|calls| calls.borrow_mut().versions.push(version));
        if version == 2 {
            std::ptr::NonNull::<flApiVtable>::dangling().as_ptr()
        } else {
            std::ptr::null()
        }
    }
    for (get_api, expected) in [(v4 as FoundryLocalGetApiFn, 4), (v3, 3), (v2, 2)] {
        CALLS.with(|calls| *calls.borrow_mut() = Calls::default());
        assert_eq!(unsafe { negotiate_api(get_api) }.unwrap().1, expected);
        CALLS.with(|calls| {
            assert_eq!(
                calls.borrow().versions,
                (expected..=4).rev().collect::<Vec<_>>()
            )
        });
    }
}
