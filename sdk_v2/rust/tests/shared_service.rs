//! Cross-process serving gate. Uses bundled tiny weights unless
//! FOUNDRY_SHARED_TEST_MODEL points to a real coding model for qualification.

use std::fs;
use std::io::{Read, Write};
use std::net::TcpStream;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::time::{Duration, Instant};

use foundry_local_sdk::shared_service::{SharedModelState, SharedService, SharedServiceConfig};
use foundry_local_sdk::{CatalogType, FoundryLocalConfig, FoundryLocalManager};

struct HostCleanup {
    directory: PathBuf,
}

#[cfg(windows)]
fn assert_process_exits(pid: u32) {
    use std::ffi::c_void;
    #[link(name = "kernel32")]
    extern "system" {
        fn OpenProcess(access: u32, inherit: i32, pid: u32) -> *mut c_void;
        fn WaitForSingleObject(handle: *mut c_void, milliseconds: u32) -> u32;
        fn CloseHandle(handle: *mut c_void) -> i32;
    }
    unsafe {
        let handle = OpenProcess(0x00100000, 0, pid);
        if handle.is_null() {
            assert_eq!(std::io::Error::last_os_error().raw_os_error(), Some(87));
            return;
        }
        let result = WaitForSingleObject(handle, 30_000);
        CloseHandle(handle);
        assert_eq!(result, 0, "host {pid} did not exit after shutdown");
    }
}

#[cfg(unix)]
fn assert_process_exits(pid: u32) {
    extern "C" {
        fn kill(pid: i32, signal: i32) -> i32;
    }
    let deadline = Instant::now() + Duration::from_secs(30);
    loop {
        if unsafe { kill(pid.try_into().unwrap(), 0) } != 0 {
            assert_eq!(std::io::Error::last_os_error().raw_os_error(), Some(3));
            return;
        }
        assert!(
            Instant::now() < deadline,
            "host {pid} did not exit after shutdown"
        );
        std::thread::sleep(Duration::from_millis(50));
    }
}

impl Drop for HostCleanup {
    fn drop(&mut self) {
        // Best-effort cleanup also runs on assertion failure. Normal test teardown
        // additionally verifies that the lifetime lock was released.
        let endpoint = fs::read(self.directory.join("ready.json"))
            .ok()
            .and_then(|bytes| serde_json::from_slice::<serde_json::Value>(&bytes).ok())
            .and_then(|record| record["endpoint"].as_str().map(str::to_owned));
        if let Some(address) =
            endpoint.and_then(|url| url.trim_start_matches("http://").parse().ok())
        {
            if let Ok(mut socket) = TcpStream::connect_timeout(&address, Duration::from_secs(5)) {
                let _ = socket.set_read_timeout(Some(Duration::from_secs(5)));
                let _ = socket.write_all(b"POST /shutdown HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                let mut response = Vec::new();
                let _ = socket.read_to_end(&mut response);
            }
        }
    }
}

#[test]
#[ignore = "helper executed by shared_host_reuses_weights_across_processes"]
fn shared_client_process() {
    let config: SharedServiceConfig =
        serde_json::from_str(&std::env::var("FOUNDRY_SHARED_TEST_CONFIG").unwrap()).unwrap();
    let output = PathBuf::from(std::env::var_os("FOUNDRY_SHARED_TEST_OUTPUT").unwrap());
    let model_path = PathBuf::from(std::env::var_os("FOUNDRY_SHARED_TEST_MODEL_PATH").unwrap());
    tokio::runtime::Runtime::new().unwrap().block_on(async {
        let service = SharedService::new(config).unwrap();
        let host = service.ensure_started().await.unwrap();
        let loaded = host
            .load(CatalogType::Local, "shared-coding:1", &model_path)
            .await
            .unwrap();
        fs::write(
            output,
            serde_json::to_vec(&serde_json::json!({
                "endpoint": host.inference_endpoint(CatalogType::Local),
                "loaded": loaded
            }))
            .unwrap(),
        )
        .unwrap();
    });
}

#[tokio::test]
#[ignore = "manual CUDA provider shutdown reproduction; requires a matching runtime"]
async fn cuda_host_shutdown_without_inference() {
    let config: SharedServiceConfig =
        serde_json::from_str(&std::env::var("FOUNDRY_SHARED_TEST_CONFIG").unwrap()).unwrap();
    let service = SharedService::new(config.clone()).unwrap();
    let cleanup = HostCleanup {
        directory: config.directory.clone(),
    };
    let model_override = std::env::var_os("FOUNDRY_SHARED_TEST_MODEL");
    let model_id = format!("shared-cuda-probe-{}:1", std::process::id());
    let (_manager, model_path) = if let Some(path) = &model_override {
        let manager = FoundryLocalManager::create(
            FoundryLocalConfig::new("cuda-shutdown-probe")
                .library_path(config.library.to_string_lossy())
                .app_data_dir(config.app_data_dir.to_string_lossy())
                .model_cache_dir(config.model_cache_dir.to_string_lossy())
                .service_endpoint("http://127.0.0.1:1"),
        )
        .unwrap();
        let catalog = manager.local_catalog().unwrap();
        let mut metadata = catalog.create_model_info().unwrap();
        metadata.set_string_property("task", "coding").unwrap();
        let model = catalog
            .register_model(&PathBuf::from(path).to_string_lossy(), &model_id, metadata)
            .await
            .unwrap();
        let path = model.path().await.unwrap();
        (Some(manager), Some(path))
    } else {
        (None, None)
    };
    let host = service.ensure_started().await.unwrap();
    assert!(host
        .loaded_models(CatalogType::Local)
        .await
        .unwrap()
        .is_empty());
    let record: serde_json::Value =
        serde_json::from_slice(&fs::read(config.directory.join("ready.json")).unwrap()).unwrap();
    let pid = record["pid"].as_u64().unwrap().try_into().unwrap();
    eprintln!("CUDA shutdown probe host PID: {pid}");
    if let Some(path) = std::env::var_os("FOUNDRY_SHARED_TEST_CONTINUE_FILE") {
        let deadline = Instant::now() + Duration::from_secs(120);
        while !Path::new(&path).exists() {
            assert!(Instant::now() < deadline, "debugger handshake timed out");
            tokio::time::sleep(Duration::from_millis(50)).await;
        }
    }
    if let Some(path) = model_path {
        host.load(CatalogType::Local, &model_id, &path)
            .await
            .unwrap();
        host.unload(CatalogType::Local, &model_id, &path)
            .await
            .unwrap();
    }
    drop(cleanup);
    assert_process_exits(pid);
}

#[tokio::test]
async fn shared_host_reuses_weights_across_processes() {
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../cpp/build/shared-serving/tests");
    fs::create_dir_all(&root).unwrap();
    let directory = tempfile::tempdir_in(&root).unwrap().keep();
    let dir = directory.as_path();
    eprintln!("Shared-serving gate diagnostics: {}", dir.display());
    let library = std::env::var_os("FOUNDRY_SHARED_TEST_LIBRARY")
        .map(PathBuf::from)
        .or_else(|| {
            std::env::var_os("FOUNDRY_LOCAL_LIB_DIR")
                .map(PathBuf::from)
                .or_else(|| option_env!("FOUNDRY_NATIVE_DIR").map(PathBuf::from))
                .map(|directory| {
                    directory.join(if cfg!(windows) {
                        "foundry_local.dll"
                    } else if cfg!(target_os = "macos") {
                        "libfoundry_local.dylib"
                    } else {
                        "libfoundry_local.so"
                    })
                })
        })
        .expect("configure the normal native runtime or set FOUNDRY_SHARED_TEST_LIBRARY");
    let real_model = std::env::var_os("FOUNDRY_SHARED_TEST_MODEL");
    let config = SharedServiceConfig {
        directory: dir.join("state"),
        executable: std::env::var_os("FOUNDRY_SHARED_TEST_HOST")
            .map(PathBuf::from)
            .unwrap_or_else(|| PathBuf::from(env!("CARGO_BIN_EXE_foundry-local-host"))),
        library,
        model_cache_dir: dir.join("models"),
        app_data_dir: dir.join("data"),
        execution_providers: if real_model.is_some() {
            vec!["CUDAExecutionProvider".into()]
        } else {
            Vec::new()
        },
    };
    let model_path =
        fs::canonicalize(real_model.as_ref().map(PathBuf::from).unwrap_or_else(|| {
            Path::new(env!("CARGO_MANIFEST_DIR")).join("../cpp/test/testdata/tiny-paged-attention")
        }))
        .unwrap();
    let manager = FoundryLocalManager::create(
        FoundryLocalConfig::new("shared-service-test")
            .library_path(config.library.to_string_lossy())
            .app_data_dir(config.app_data_dir.to_string_lossy())
            .model_cache_dir(config.model_cache_dir.to_string_lossy())
            .service_endpoint("http://127.0.0.1:1"),
    )
    .unwrap();
    let catalog = manager.local_catalog().unwrap();
    let mut metadata = catalog.create_model_info().unwrap();
    metadata.set_string_property("task", "coding").unwrap();
    metadata
        .set_int_property("supports_tool_calling", 1)
        .unwrap();
    metadata.set_int_property("supports_reasoning", 1).unwrap();
    if real_model.is_some() {
        // The live qualification fixture is Qwen; these are its protocol markers,
        // not product-wide assumptions about every coding model.
        for (key, value) in [
            ("tool_call_start", "<tool_call>"),
            ("tool_call_end", "</tool_call>"),
            ("reasoning_start", "<think>"),
            ("reasoning_end", "</think>"),
        ] {
            metadata.set_string_property(key, value).unwrap();
        }
    }
    let model = catalog
        .register_model(&model_path.to_string_lossy(), "shared-coding:1", metadata)
        .await
        .unwrap();
    let expected_model_path = model.path().await.unwrap();
    assert!(
        !model.is_loaded().await.unwrap(),
        "metadata client must not load inference weights"
    );
    let encoded_config = serde_json::to_string(&config).unwrap();
    let cleanup = HostCleanup {
        directory: config.directory.clone(),
    };
    let mut clients = Vec::new();
    for index in 0..2 {
        let output = dir.join(format!("client-{index}.txt"));
        let log = fs::File::create(dir.join(format!("client-{index}.log"))).unwrap();
        let child = Command::new(std::env::current_exe().unwrap())
            .args([
                "--exact",
                "shared_client_process",
                "--ignored",
                "--nocapture",
            ])
            .env("FOUNDRY_SHARED_TEST_CONFIG", &encoded_config)
            .env("FOUNDRY_SHARED_TEST_OUTPUT", &output)
            .env("FOUNDRY_SHARED_TEST_MODEL_PATH", &expected_model_path)
            .stdout(Stdio::from(log.try_clone().unwrap()))
            .stderr(Stdio::from(log))
            .spawn()
            .unwrap();
        clients.push((child, output));
    }
    let mut observations = Vec::new();
    for (mut client, output) in clients {
        let status = client.wait().unwrap();
        assert!(
            status.success(),
            "client failed: {}\nhost: {}",
            fs::read_to_string(output.with_extension("log")).unwrap_or_default(),
            fs::read_to_string(config.directory.join("host.log")).unwrap_or_default()
        );
        observations
            .push(serde_json::from_slice::<serde_json::Value>(&fs::read(output).unwrap()).unwrap());
    }
    assert_eq!(
        observations[0], observations[1],
        "both clients must reuse one serving process and observe the same loaded profile"
    );
    let loaded: SharedModelState =
        serde_json::from_value(observations[0]["loaded"].clone()).unwrap();
    assert_eq!(loaded.model_id, "shared-coding:1");
    assert_eq!(
        fs::canonicalize(&loaded.model_path).unwrap(),
        fs::canonicalize(&expected_model_path).unwrap()
    );
    assert!(loaded.context_length.is_some_and(|value| value > 0));
    let service = SharedService::new(config.clone()).unwrap();
    let host = service
        .connect()
        .await
        .unwrap()
        .expect("host survives both client exits");
    assert_eq!(
        host.loaded_models(CatalogType::Local).await.unwrap(),
        vec!["shared-coding:1"]
    );
    assert!(
        !model.is_loaded().await.unwrap(),
        "loaded-state query must not accidentally load a client model"
    );
    let states = host.loaded_model_states(CatalogType::Local).await.unwrap();
    assert_eq!(states.len(), 1);
    assert_eq!(states[0].model_id, loaded.model_id);
    assert_eq!(states[0].context_length, loaded.context_length);
    assert_eq!(
        fs::canonicalize(&states[0].model_path).unwrap(),
        fs::canonicalize(&expected_model_path).unwrap()
    );
    if real_model.is_none() {
        let other_path = dir.join("different-artifact");
        fs::create_dir(&other_path).unwrap();
        assert!(host
            .unload(CatalogType::Local, "shared-coding:1", &other_path)
            .await
            .is_err());
        assert_eq!(
            host.loaded_models(CatalogType::Local).await.unwrap(),
            vec!["shared-coding:1"]
        );
        host.unload(CatalogType::Local, "shared-coding:1", &expected_model_path)
            .await
            .unwrap();
        assert!(host
            .load(CatalogType::Local, "shared-coding:1", &other_path)
            .await
            .is_err());
        assert!(host
            .loaded_models(CatalogType::Local)
            .await
            .unwrap()
            .is_empty());
        host.load(CatalogType::Local, "shared-coding:1", &expected_model_path)
            .await
            .unwrap();
    }

    let client = reqwest::Client::builder()
        .no_proxy()
        .timeout(Duration::from_secs(300))
        .build()
        .unwrap();
    let inference = host.inference_endpoint(CatalogType::Local);
    if real_model.is_some() {
        let tools = serde_json::json!([{"type":"function","function":{
            "name":"multiply","description":"Multiply two integers",
            "parameters":{"type":"object","properties":{"a":{"type":"integer"},"b":{"type":"integer"}},
                "required":["a","b"],"additionalProperties":false}
        }}]);
        let prompt = "Call multiply with a=7 and b=9. If the tool fails, do not retry it. \
            Instead write a Python multiply(a, b) function and a unit test asserting multiply(7, 9) == 63.";
        let response: serde_json::Value = client
            .post(format!("{inference}/chat/completions"))
            .json(&serde_json::json!({
                "model": "shared-coding:1",
                "messages": [{"role":"user","content":prompt}],
                "tools": tools.clone(),
                "max_tokens":1024,"temperature":0
            }))
            .send()
            .await
            .unwrap()
            .error_for_status()
            .unwrap()
            .json()
            .await
            .unwrap();
        let message = &response["choices"][0]["message"];
        let calls = message["tool_calls"]
            .as_array()
            .expect("coding model must emit a valid tool call");
        assert_eq!(calls.len(), 1, "{response}");
        assert_eq!(calls[0]["function"]["name"], "multiply");
        let args: serde_json::Value =
            serde_json::from_str(calls[0]["function"]["arguments"].as_str().unwrap()).unwrap();
        assert_eq!(args, serde_json::json!({"a":7,"b":9}));
        let result: serde_json::Value = client.post(format!("{inference}/chat/completions"))
            .json(&serde_json::json!({
                "model":"shared-coding:1",
                "messages":[
                    {"role":"user","content":prompt},
                    message,
                    {"role":"tool","tool_call_id":calls[0]["id"],"content":"Tool execution failed: temporary arithmetic service error."}
                ],
                "tools": tools,
                "max_tokens":2048,"temperature":0
            }))
            .send().await.unwrap().error_for_status().unwrap().json().await.unwrap();
        let final_calls = &result["choices"][0]["message"]["tool_calls"];
        assert!(
            final_calls.is_null() || final_calls.as_array().is_some_and(Vec::is_empty),
            "{result}"
        );
        let content = result["choices"][0]["message"]["content"].as_str().unwrap();
        assert!(
            content.contains("def multiply") && content.contains("63"),
            "{result}"
        );
        fs::write(
            root.join("coding-workload-last.json"),
            serde_json::to_vec_pretty(
                &serde_json::json!({"loaded":loaded,"tool_call":response,"after_failed_tool":result}),
            )
            .unwrap(),
        )
        .unwrap();
    }

    let record: serde_json::Value =
        serde_json::from_slice(&fs::read(config.directory.join("ready.json")).unwrap()).unwrap();
    let host_pid: u32 = record["pid"].as_u64().unwrap().try_into().unwrap();
    host.unload(CatalogType::Local, "shared-coding:1", &expected_model_path)
        .await
        .unwrap();
    assert!(host
        .loaded_models(CatalogType::Local)
        .await
        .unwrap()
        .is_empty());
    drop(cleanup);
    let deadline = Instant::now() + Duration::from_secs(30);
    let lock = fs::OpenOptions::new()
        .read(true)
        .write(true)
        .open(config.directory.join("host.lock"))
        .unwrap();
    loop {
        match fs2::FileExt::try_lock_exclusive(&lock) {
            Ok(()) => break,
            Err(error) if error.raw_os_error() == fs2::lock_contended_error().raw_os_error() => {
                assert!(Instant::now() < deadline, "host did not finish shutdown");
                tokio::time::sleep(Duration::from_millis(50)).await;
            }
            Err(error) => panic!("cannot observe host termination: {error}"),
        }
    }
    fs2::FileExt::unlock(&lock).unwrap();
    assert!(service.connect().await.unwrap().is_none());
    assert_process_exits(host_pid);
}
