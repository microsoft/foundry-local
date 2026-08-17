use thiserror::Error;

/// Errors that can occur when using the Foundry Local SDK.
#[derive(Debug, Error)]
pub enum FoundryLocalError {
    /// The native core library could not be loaded.
    #[error("library load error: {reason}")]
    LibraryLoad { reason: String },
    /// A command executed against the native core returned an error.
    #[error("command execution error: {reason}")]
    CommandExecution { reason: String },
    /// The provided configuration is invalid.
    #[error("invalid configuration: {reason}")]
    InvalidConfiguration { reason: String },
    /// A model operation failed (load, unload, download, etc.).
    #[error("model operation error: {reason}")]
    ModelOperation { reason: String },
    /// An HTTP request to the external service failed.
    #[error("HTTP request error: {0}")]
    HttpRequest(#[from] reqwest::Error),
    /// Serialization or deserialization of JSON data failed.
    #[error("serialization error: {0}")]
    Serialization(#[from] serde_json::Error),
    /// A validation check on user-supplied input failed.
    #[error("validation error: {reason}")]
    Validation { reason: String },
    /// An I/O error occurred.
    #[error("I/O error: {0}")]
    Io(#[from] std::io::Error),
    /// An internal SDK error (e.g. poisoned lock).
    #[error("internal error: {reason}")]
    Internal { reason: String },
}

/// Convenience alias used throughout the SDK.
pub type Result<T> = std::result::Result<T, FoundryLocalError>;
