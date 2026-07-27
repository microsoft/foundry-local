//! Single integration test binary for the Foundry Local Rust SDK.
//!
//! All test modules are compiled into one binary so the native core is only
//! initialised once (via the `OnceLock` singleton in `FoundryLocalManager`).
//! Running them as separate binaries causes "already initialized" errors
//! because the .NET native runtime retains state across process-level
//! library loads.

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
