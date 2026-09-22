use thiserror::Error;

/// Stable error codes reported by the native Foundry Local library.
#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
#[non_exhaustive]
pub enum NativeErrorCode {
    Ok,
    NotImplemented,
    Internal,
    InvalidArgument,
    InvalidUsage,
    OperationCancelled,
    Network,
    Unknown(i32),
}

/// Errors that can occur when using the Foundry Local SDK.
#[derive(Debug, Error)]
pub enum FoundryLocalError {
    /// The native core library returned an error.
    #[error("native error ({code:?}): {message}")]
    Native {
        code: NativeErrorCode,
        message: String,
    },
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

impl FoundryLocalError {
    /// Returns the native error code, if this error originated in the native library.
    pub fn native_code(&self) -> Option<NativeErrorCode> {
        match self {
            Self::Native { code, .. } => Some(*code),
            _ => None,
        }
    }

    /// Returns the native error message, if this error originated in the native library.
    pub fn native_message(&self) -> Option<&str> {
        match self {
            Self::Native { message, .. } => Some(message),
            _ => None,
        }
    }
}

/// Convenience alias used throughout the SDK.
pub type Result<T> = std::result::Result<T, FoundryLocalError>;

#[cfg(test)]
mod tests {
    use super::{FoundryLocalError, NativeErrorCode};

    #[test]
    fn native_error_exposes_code_and_message() {
        let error = FoundryLocalError::Native {
            code: NativeErrorCode::Network,
            message: "connection failed".into(),
        };

        assert_eq!(error.native_code(), Some(NativeErrorCode::Network));
        assert_eq!(error.native_message(), Some("connection failed"));
    }

    #[test]
    fn non_native_error_has_no_native_details() {
        let error = FoundryLocalError::Validation {
            reason: "invalid".into(),
        };

        assert_eq!(error.native_code(), None);
        assert_eq!(error.native_message(), None);
    }
}
