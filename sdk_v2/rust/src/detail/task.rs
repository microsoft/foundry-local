//! Shared async helper for running blocking native calls.

use crate::error::{FoundryLocalError, Result};

/// Run a blocking native operation on the tokio blocking pool.
pub(crate) async fn spawn_blocking<T, F>(f: F) -> Result<T>
where
    F: FnOnce() -> Result<T> + Send + 'static,
    T: Send + 'static,
{
    tokio::task::spawn_blocking(f)
        .await
        .map_err(|e| FoundryLocalError::Internal {
            reason: format!("blocking task join error: {e}"),
        })?
}
