//! Single integration test binary for the Foundry Local Rust SDK.
//!
//! All test modules are compiled into one binary so the native core is only
//! initialised once. The shared test helper releases its final manager handle
//! at process exit before the native library's static destructors run.
#![allow(deprecated)] // some suites still exercise the deprecated OpenAI facade

mod common;

mod audio_client_test;
mod catalog_test;
mod chat_client_test;
mod embedding_client_test;
mod live_audio_test;
mod manager_test;
mod model_test;
mod session_test;
mod web_service_test;
