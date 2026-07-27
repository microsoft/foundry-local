//! Foundry Local Rust SDK
//!
//! Local AI model inference powered by the Foundry Local Core engine.

mod catalog;
mod configuration;
mod error;
mod foundry_local_manager;
mod item;
mod item_queue;
mod request;
mod response;
mod session;
mod types;

pub(crate) mod detail;
pub mod openai;

pub use self::catalog::Catalog;
pub use self::configuration::{FoundryLocalConfig, LogLevel, Logger};
pub use self::detail::model::{DownloadBuilder, Model};
pub use self::error::FoundryLocalError;
pub use self::foundry_local_manager::{EpDownloadBuilder, FoundryLocalManager};
pub use self::item::{
    Audio, Image, Item, ItemType, MediaSource, Message, MessageRole, SpeechResult, SpeechSegment,
    SpeechSegmentKind, SpeechWord, Tensor, TensorDataType, TextKind, ToolCall, ToolResult,
};
pub use self::item_queue::ItemQueue;
pub use self::request::{Request, RequestOptions, SearchOptions, ToolChoice};
pub use self::response::{FinishReason, Response, Usage};
pub use self::session::{
    AudioSession, ChatSession, EmbeddingsSession, ItemStream, Session, ToolDefinition,
};
pub use self::types::{
    ChatResponseFormat, ChatToolChoice, DeviceType, EpDownloadResult, EpInfo, ModelInfo,
    ModelSettings, Parameter, PromptTemplate, Runtime,
};

// Re-export OpenAI request types so callers can construct typed messages.
pub use async_openai::types::chat::{
    ChatCompletionNamedToolChoice, ChatCompletionRequestAssistantMessage,
    ChatCompletionRequestMessage, ChatCompletionRequestSystemMessage,
    ChatCompletionRequestToolMessage, ChatCompletionRequestUserMessage,
    ChatCompletionToolChoiceOption, ChatCompletionTools, FunctionObject,
};

// Re-export OpenAI response types for convenience.
#[allow(deprecated)] // re-export includes the deprecated `LiveAudioTranscriptionSession`
pub use crate::openai::{
    AudioTranscriptionResponse, AudioTranscriptionStream, ChatCompletionStream, ContentPart,
    CoreErrorResponse, LiveAudioTranscriptionOptions, LiveAudioTranscriptionResponse,
    LiveAudioTranscriptionSession, LiveAudioTranscriptionStream, TranscriptionSegment,
    TranscriptionWord,
};
// Re-export OpenAI response types for convenience. The OpenAI `FinishReason` is
// re-exported as `ChatFinishReason` to avoid colliding with the core inference
// [`FinishReason`]; it remains available as `async_openai::types::chat::FinishReason`.
pub use async_openai::types::chat::{
    ChatChoice, ChatChoiceStream, ChatCompletionMessageToolCall,
    ChatCompletionMessageToolCallChunk, ChatCompletionMessageToolCalls,
    ChatCompletionResponseMessage, ChatCompletionStreamResponseDelta, CompletionUsage,
    CreateChatCompletionResponse, CreateChatCompletionStreamResponse,
    FinishReason as ChatFinishReason, FunctionCall, FunctionCallStream,
};
