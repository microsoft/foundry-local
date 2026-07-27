#![allow(deprecated)] // re-exports the deprecated OpenAI facade types

mod audio_client;
mod chat_client;
mod embedding_client;
mod json_stream;
mod live_audio_session;

pub use self::audio_client::{
    AudioClient, AudioClientSettings, AudioTranscriptionResponse, AudioTranscriptionStream,
    TranscriptionSegment, TranscriptionWord,
};
pub use self::chat_client::{ChatClient, ChatClientSettings, ChatCompletionStream};
pub use self::embedding_client::EmbeddingClient;
pub use self::json_stream::JsonStream;
pub use self::live_audio_session::{
    ContentPart, CoreErrorResponse, LiveAudioTranscriptionOptions, LiveAudioTranscriptionResponse,
    LiveAudioTranscriptionSession, LiveAudioTranscriptionStream,
};
