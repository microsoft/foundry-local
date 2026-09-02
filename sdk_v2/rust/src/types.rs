use serde::{Deserialize, Serialize};

/// Hardware device type for model execution.
#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
pub enum DeviceType {
    Invalid,
    #[default]
    CPU,
    GPU,
    NPU,
}

/// Prompt template describing how messages are formatted for the model.
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct PromptTemplate {
    #[serde(skip_serializing_if = "Option::is_none")]
    pub system: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub user: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub assistant: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub prompt: Option<String>,
}

/// Runtime information for a model (device type and execution provider).
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Runtime {
    pub device_type: DeviceType,
    pub execution_provider: String,
}

/// A single parameter key-value pair used in model settings.
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Parameter {
    pub name: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub value: Option<String>,
}

/// Model-level settings containing a list of parameters.
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ModelSettings {
    #[serde(skip_serializing_if = "Option::is_none")]
    pub parameters: Option<Vec<Parameter>>,
}

/// Full metadata for a model variant as returned by the catalog.
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ModelInfo {
    pub id: String,
    pub name: String,
    pub version: u64,
    pub alias: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub display_name: Option<String>,
    pub provider_type: String,
    pub uri: String,
    pub model_type: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub prompt_template: Option<PromptTemplate>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub publisher: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub model_settings: Option<ModelSettings>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub license: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub license_description: Option<String>,
    pub cached: bool,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub task: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub runtime: Option<Runtime>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub file_size_mb: Option<u64>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub supports_tool_calling: Option<bool>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub max_output_tokens: Option<u64>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub min_fl_version: Option<String>,
    #[serde(default)]
    pub created_at_unix: u64,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub context_length: Option<u64>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub input_modalities: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub output_modalities: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub capabilities: Option<String>,
}

/// Desired response format for chat completions.
///
/// Extends the standard OpenAI formats with the Foundry-specific
/// `LarkGrammar` variant.
#[derive(Debug, Clone)]
pub enum ChatResponseFormat {
    /// Plain text output (default).
    Text,
    /// JSON output (unstructured).
    JsonObject,
    /// JSON output constrained by a schema string.
    JsonSchema(String),
    /// Output constrained by a Lark grammar (Foundry extension).
    LarkGrammar(String),
}

/// Tool choice configuration for chat completions.
#[derive(Debug, Clone)]
pub enum ChatToolChoice {
    /// Model will not call any tool.
    None,
    /// Model decides whether to call a tool.
    Auto,
    /// Model must call at least one tool.
    Required,
    /// Model must call the named function.
    Function(String),
}

/// Information about an available execution provider.
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct EpInfo {
    /// The name of the execution provider.
    pub name: String,
    /// Whether this EP is currently registered and ready for use.
    pub is_registered: bool,
}

/// Result of a download-and-register execution-provider operation.
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct EpDownloadResult {
    /// Whether all requested EPs were successfully registered.
    pub success: bool,
    /// Human-readable status message.
    pub status: String,
    /// Names of EPs that were successfully registered.
    pub registered_eps: Vec<String>,
    /// Names of EPs that failed to register.
    pub failed_eps: Vec<String>,
}
