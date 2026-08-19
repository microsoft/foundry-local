//! The [`Item`] discriminated union and its supporting value types.
//!
//! An [`Item`] is the unit of data exchanged with a [`Session`](crate::Session):
//! requests carry input items (text, messages, images, audio, tensors, tool
//! calls/results) and responses carry output items (text, tensors, tool calls,
//! speech results, …).
//!
//! Unlike the class hierarchies used by the C++/C#/Python SDKs, the Rust surface
//! models items as a plain, owned `enum`. Items are pure data: they hold no
//! native handle, are `Send + Sync + Clone`, and can be constructed, matched,
//! and inspected without a loaded native library. Conversion to and from the
//! native `flItem` representation happens transiently inside the session when a
//! request is processed (see [`crate::detail::items`]).

use crate::detail::ffi::*;

/// The kind of payload carried by an [`Item`].
///
/// Mirrors the native `flItemType` discriminant and is returned by
/// [`Item::item_type`].
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum ItemType {
    /// Opaque byte buffer ([`Item::Bytes`]).
    Bytes,
    /// Numeric tensor ([`Item::Tensor`]).
    Tensor,
    /// UTF-8 text ([`Item::Text`]).
    Text,
    /// Chat message with nested content parts ([`Item::Message`]).
    Message,
    /// Image, inline or by URI ([`Item::Image`]).
    Image,
    /// Audio, inline or by URI ([`Item::Audio`]).
    Audio,
    /// A single speech-recognition segment ([`Item::SpeechSegment`]).
    SpeechSegment,
    /// A complete speech-recognition result ([`Item::SpeechResult`]).
    SpeechResult,
    /// A model-issued tool/function call ([`Item::ToolCall`]).
    ToolCall,
    /// The result of executing a tool/function call ([`Item::ToolResult`]).
    ToolResult,
}

/// The subtype of a [`Item::Text`] payload.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Default)]
pub enum TextKind {
    /// Ordinary user- or model-visible text.
    #[default]
    Default,
    /// Model reasoning / chain-of-thought text.
    Reasoning,
    /// An opaque OpenAI-compatible REST JSON payload (used by the higher-level
    /// OpenAI facade). Rarely constructed directly.
    OpenAiJson,
}

impl TextKind {
    pub(crate) fn to_native(self) -> flTextItemType {
        match self {
            TextKind::Default => FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT,
            TextKind::Reasoning => FOUNDRY_LOCAL_TEXT_ITEM_TYPE_REASONING,
            TextKind::OpenAiJson => FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON,
        }
    }

    pub(crate) fn from_native(value: flTextItemType) -> TextKind {
        match value {
            FOUNDRY_LOCAL_TEXT_ITEM_TYPE_REASONING => TextKind::Reasoning,
            FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON => TextKind::OpenAiJson,
            _ => TextKind::Default,
        }
    }
}

/// The author role of a chat [`Message`].
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Default)]
pub enum MessageRole {
    /// No role specified (only seen when reading messages that omit a role).
    #[default]
    None,
    /// System / instruction message.
    System,
    /// End-user message.
    User,
    /// Model / assistant message.
    Assistant,
    /// Tool / function output message.
    Tool,
    /// Developer message (higher-priority instructions than `System`).
    Developer,
}

impl MessageRole {
    pub(crate) fn to_native(self) -> flMessageRole {
        match self {
            MessageRole::None => FOUNDRY_LOCAL_ROLE_NONE,
            MessageRole::System => FOUNDRY_LOCAL_ROLE_SYSTEM,
            MessageRole::User => FOUNDRY_LOCAL_ROLE_USER,
            MessageRole::Assistant => FOUNDRY_LOCAL_ROLE_ASSISTANT,
            MessageRole::Tool => FOUNDRY_LOCAL_ROLE_TOOL,
            MessageRole::Developer => FOUNDRY_LOCAL_ROLE_DEVELOPER,
        }
    }

    pub(crate) fn from_native(value: flMessageRole) -> MessageRole {
        match value {
            FOUNDRY_LOCAL_ROLE_SYSTEM => MessageRole::System,
            FOUNDRY_LOCAL_ROLE_USER => MessageRole::User,
            FOUNDRY_LOCAL_ROLE_ASSISTANT => MessageRole::Assistant,
            FOUNDRY_LOCAL_ROLE_TOOL => MessageRole::Tool,
            FOUNDRY_LOCAL_ROLE_DEVELOPER => MessageRole::Developer,
            _ => MessageRole::None,
        }
    }
}

/// The element data type of a [`Tensor`], mirroring ONNX tensor element types.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Default)]
#[allow(missing_docs)]
pub enum TensorDataType {
    #[default]
    Undefined,
    Float,
    Uint8,
    Int8,
    Uint16,
    Int16,
    Int32,
    Int64,
    String,
    Bool,
    Float16,
    Double,
    Uint32,
    Uint64,
    Complex64,
    Complex128,
    BFloat16,
    Float8E4M3FN,
    Float8E4M3FNUZ,
    Float8E5M2,
    Float8E5M2FNUZ,
    Uint4,
    Int4,
    Float4E2M1,
    Float8E8M0,
}

impl TensorDataType {
    pub(crate) fn to_native(self) -> flTensorDataType {
        match self {
            TensorDataType::Undefined => FOUNDRY_LOCAL_TENSOR_UNDEFINED,
            TensorDataType::Float => FOUNDRY_LOCAL_TENSOR_FLOAT,
            TensorDataType::Uint8 => FOUNDRY_LOCAL_TENSOR_UINT8,
            TensorDataType::Int8 => FOUNDRY_LOCAL_TENSOR_INT8,
            TensorDataType::Uint16 => FOUNDRY_LOCAL_TENSOR_UINT16,
            TensorDataType::Int16 => FOUNDRY_LOCAL_TENSOR_INT16,
            TensorDataType::Int32 => FOUNDRY_LOCAL_TENSOR_INT32,
            TensorDataType::Int64 => FOUNDRY_LOCAL_TENSOR_INT64,
            TensorDataType::String => FOUNDRY_LOCAL_TENSOR_STRING,
            TensorDataType::Bool => FOUNDRY_LOCAL_TENSOR_BOOL,
            TensorDataType::Float16 => FOUNDRY_LOCAL_TENSOR_FLOAT16,
            TensorDataType::Double => FOUNDRY_LOCAL_TENSOR_DOUBLE,
            TensorDataType::Uint32 => FOUNDRY_LOCAL_TENSOR_UINT32,
            TensorDataType::Uint64 => FOUNDRY_LOCAL_TENSOR_UINT64,
            TensorDataType::Complex64 => FOUNDRY_LOCAL_TENSOR_COMPLEX64,
            TensorDataType::Complex128 => FOUNDRY_LOCAL_TENSOR_COMPLEX128,
            TensorDataType::BFloat16 => FOUNDRY_LOCAL_TENSOR_BFLOAT16,
            TensorDataType::Float8E4M3FN => FOUNDRY_LOCAL_TENSOR_FLOAT8E4M3FN,
            TensorDataType::Float8E4M3FNUZ => FOUNDRY_LOCAL_TENSOR_FLOAT8E4M3FNUZ,
            TensorDataType::Float8E5M2 => FOUNDRY_LOCAL_TENSOR_FLOAT8E5M2,
            TensorDataType::Float8E5M2FNUZ => FOUNDRY_LOCAL_TENSOR_FLOAT8E5M2FNUZ,
            TensorDataType::Uint4 => FOUNDRY_LOCAL_TENSOR_UINT4,
            TensorDataType::Int4 => FOUNDRY_LOCAL_TENSOR_INT4,
            TensorDataType::Float4E2M1 => FOUNDRY_LOCAL_TENSOR_FLOAT4E2M1,
            TensorDataType::Float8E8M0 => FOUNDRY_LOCAL_TENSOR_FLOAT8E8M0,
        }
    }

    pub(crate) fn from_native(value: flTensorDataType) -> TensorDataType {
        match value {
            FOUNDRY_LOCAL_TENSOR_FLOAT => TensorDataType::Float,
            FOUNDRY_LOCAL_TENSOR_UINT8 => TensorDataType::Uint8,
            FOUNDRY_LOCAL_TENSOR_INT8 => TensorDataType::Int8,
            FOUNDRY_LOCAL_TENSOR_UINT16 => TensorDataType::Uint16,
            FOUNDRY_LOCAL_TENSOR_INT16 => TensorDataType::Int16,
            FOUNDRY_LOCAL_TENSOR_INT32 => TensorDataType::Int32,
            FOUNDRY_LOCAL_TENSOR_INT64 => TensorDataType::Int64,
            FOUNDRY_LOCAL_TENSOR_STRING => TensorDataType::String,
            FOUNDRY_LOCAL_TENSOR_BOOL => TensorDataType::Bool,
            FOUNDRY_LOCAL_TENSOR_FLOAT16 => TensorDataType::Float16,
            FOUNDRY_LOCAL_TENSOR_DOUBLE => TensorDataType::Double,
            FOUNDRY_LOCAL_TENSOR_UINT32 => TensorDataType::Uint32,
            FOUNDRY_LOCAL_TENSOR_UINT64 => TensorDataType::Uint64,
            FOUNDRY_LOCAL_TENSOR_COMPLEX64 => TensorDataType::Complex64,
            FOUNDRY_LOCAL_TENSOR_COMPLEX128 => TensorDataType::Complex128,
            FOUNDRY_LOCAL_TENSOR_BFLOAT16 => TensorDataType::BFloat16,
            FOUNDRY_LOCAL_TENSOR_FLOAT8E4M3FN => TensorDataType::Float8E4M3FN,
            FOUNDRY_LOCAL_TENSOR_FLOAT8E4M3FNUZ => TensorDataType::Float8E4M3FNUZ,
            FOUNDRY_LOCAL_TENSOR_FLOAT8E5M2 => TensorDataType::Float8E5M2,
            FOUNDRY_LOCAL_TENSOR_FLOAT8E5M2FNUZ => TensorDataType::Float8E5M2FNUZ,
            FOUNDRY_LOCAL_TENSOR_UINT4 => TensorDataType::Uint4,
            FOUNDRY_LOCAL_TENSOR_INT4 => TensorDataType::Int4,
            FOUNDRY_LOCAL_TENSOR_FLOAT4E2M1 => TensorDataType::Float4E2M1,
            FOUNDRY_LOCAL_TENSOR_FLOAT8E8M0 => TensorDataType::Float8E8M0,
            _ => TensorDataType::Undefined,
        }
    }
}

/// Whether a [`SpeechSegment`] is an interim hypothesis or a stable result.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Default)]
pub enum SpeechSegmentKind {
    /// Unspecified.
    #[default]
    None,
    /// An interim hypothesis that may still change.
    Partial,
    /// A stabilized, final segment.
    Final,
}

impl SpeechSegmentKind {
    pub(crate) fn from_native(value: flSpeechSegmentKind) -> SpeechSegmentKind {
        match value {
            FOUNDRY_LOCAL_SPEECH_SEGMENT_PARTIAL => SpeechSegmentKind::Partial,
            FOUNDRY_LOCAL_SPEECH_SEGMENT_FINAL => SpeechSegmentKind::Final,
            _ => SpeechSegmentKind::None,
        }
    }
}

/// The source of an [`Image`] or [`Audio`] payload: inline bytes or a URI.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum MediaSource {
    /// Inline, raw bytes.
    Data(Vec<u8>),
    /// A reference to external content by URI.
    Uri(String),
}

/// A chat message: an author [`role`](MessageRole) plus ordered content parts.
///
/// Content parts are themselves [`Item`]s, allowing multimodal messages (e.g. a
/// [`Item::Text`] alongside a [`Item::Image`]).
#[derive(Debug, Clone, PartialEq)]
pub struct Message {
    /// The author role.
    pub role: MessageRole,
    /// The ordered content parts of the message.
    pub content: Vec<Item>,
    /// Optional participant name.
    pub name: Option<String>,
}

impl Message {
    /// Create a message with the given role and content parts.
    pub fn new(role: MessageRole, content: impl Into<Vec<Item>>) -> Self {
        Self {
            role,
            content: content.into(),
            name: None,
        }
    }

    /// Set the participant name (builder-style).
    pub fn with_name(mut self, name: impl Into<String>) -> Self {
        self.name = Some(name.into());
        self
    }

    /// Whether the message consists of a single [`Item::Text`] content part.
    pub fn is_simple_text(&self) -> bool {
        matches!(self.content.as_slice(), [Item::Text { .. }])
    }

    /// The concatenated text of all [`Item::Text`] content parts.
    ///
    /// Non-text parts are ignored. Returns an empty string if there are none.
    pub fn text(&self) -> String {
        let mut out = String::new();
        for part in &self.content {
            if let Item::Text { text, .. } = part {
                out.push_str(text);
            }
        }
        out
    }
}

/// A numeric tensor: an element [`data_type`](TensorDataType), a `shape`, and the
/// raw little-endian element bytes.
#[derive(Debug, Clone, PartialEq)]
pub struct Tensor {
    /// The element data type.
    pub data_type: TensorDataType,
    /// The dimensions of the tensor.
    pub shape: Vec<i64>,
    /// The raw element bytes, row-major, little-endian.
    pub data: Vec<u8>,
}

impl Tensor {
    /// Interpret the raw bytes as `f32` elements.
    ///
    /// Returns `None` unless [`data_type`](Self::data_type) is
    /// [`TensorDataType::Float`] and the byte length is a multiple of four.
    pub fn as_f32(&self) -> Option<Vec<f32>> {
        if self.data_type != TensorDataType::Float || self.data.len() % 4 != 0 {
            return None;
        }
        Some(
            self.data
                .chunks_exact(4)
                .map(|b| f32::from_le_bytes([b[0], b[1], b[2], b[3]]))
                .collect(),
        )
    }
}

/// An image payload, inline or by URI, with an optional `format` hint (e.g. `png`).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Image {
    /// The image content.
    pub source: MediaSource,
    /// Optional format hint (e.g. `"png"`, `"jpeg"`).
    pub format: Option<String>,
}

/// An audio payload, inline or by URI, with format and PCM layout hints.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Audio {
    /// The audio content.
    pub source: MediaSource,
    /// Optional format hint (e.g. `"wav"`, `"pcm16"`).
    pub format: Option<String>,
    /// Sample rate in Hz, or `0` if unspecified.
    pub sample_rate: i32,
    /// Channel count, or `0` if unspecified.
    pub channels: i32,
}

/// A model-issued request to invoke a named tool/function.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ToolCall {
    /// Correlates the call with its [`ToolResult`].
    pub call_id: String,
    /// The name of the tool/function to invoke.
    pub name: String,
    /// The call arguments, as a JSON string.
    pub arguments: String,
}

/// The result of executing a [`ToolCall`], fed back to the model.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ToolResult {
    /// The `call_id` of the originating [`ToolCall`].
    pub call_id: String,
    /// The tool output, typically a JSON or text string.
    pub result: String,
}

/// One recognized word within a [`SpeechSegment`] (output-only).
#[derive(Debug, Clone, PartialEq)]
pub struct SpeechWord {
    /// The word text.
    pub text: String,
    /// Start offset from the beginning of the audio, in milliseconds.
    pub start_time_ms: Option<i64>,
    /// End offset from the beginning of the audio, in milliseconds.
    pub end_time_ms: Option<i64>,
    /// Recognition confidence in `[0, 1]`, if reported.
    pub confidence: Option<f32>,
    /// Diarization speaker id, if reported.
    pub speaker_id: Option<String>,
}

/// One speech-recognition segment (output-only).
#[derive(Debug, Clone, PartialEq)]
pub struct SpeechSegment {
    /// Whether this is an interim or final segment.
    pub kind: SpeechSegmentKind,
    /// The recognized text of the segment.
    pub text: String,
    /// Start offset from the beginning of the audio, in milliseconds.
    pub start_time_ms: Option<i64>,
    /// End offset from the beginning of the audio, in milliseconds.
    pub end_time_ms: Option<i64>,
    /// Whether this segment starts a new utterance.
    pub utterance_start: bool,
    /// Per-word timing/confidence, when available.
    pub words: Vec<SpeechWord>,
    /// Detected language (e.g. `"en"`), if reported.
    pub language: Option<String>,
}

/// A complete speech-recognition result (output-only).
#[derive(Debug, Clone, PartialEq)]
pub struct SpeechResult {
    /// The full concatenated transcript.
    pub text: String,
    /// Detected language (e.g. `"en"`), if reported.
    pub language: Option<String>,
    /// Total audio duration in milliseconds, if reported.
    pub duration_ms: Option<i64>,
    /// The constituent segments, each a [`Item::SpeechSegment`].
    pub segments: Vec<Item>,
}

/// A single unit of input or output data exchanged with a [`Session`](crate::Session).
///
/// `Item` is a pure-data, owned value: it holds no native handle and is
/// `Send + Sync + Clone`. Construct items with the associated functions (e.g.
/// [`Item::text`], [`Item::user_message`], [`Item::image_data`]) or the struct
/// variants directly, and inspect them by pattern matching.
#[derive(Debug, Clone, PartialEq)]
pub enum Item {
    /// UTF-8 text of a given [`TextKind`].
    Text {
        /// The text content.
        text: String,
        /// The text subtype.
        kind: TextKind,
    },
    /// A chat message with nested content parts.
    Message(Message),
    /// An opaque byte buffer.
    Bytes(Vec<u8>),
    /// A numeric tensor (e.g. an embedding vector).
    Tensor(Tensor),
    /// An image, inline or by URI.
    Image(Image),
    /// An audio clip, inline or by URI.
    Audio(Audio),
    /// A model-issued tool/function call.
    ToolCall(ToolCall),
    /// The result of executing a tool/function call.
    ToolResult(ToolResult),
    /// A speech-recognition segment (output-only).
    SpeechSegment(SpeechSegment),
    /// A speech-recognition result (output-only).
    SpeechResult(SpeechResult),
}

impl Item {
    /// The [`ItemType`] discriminant of this item.
    pub fn item_type(&self) -> ItemType {
        match self {
            Item::Text { .. } => ItemType::Text,
            Item::Message(_) => ItemType::Message,
            Item::Bytes(_) => ItemType::Bytes,
            Item::Tensor(_) => ItemType::Tensor,
            Item::Image(_) => ItemType::Image,
            Item::Audio(_) => ItemType::Audio,
            Item::ToolCall(_) => ItemType::ToolCall,
            Item::ToolResult(_) => ItemType::ToolResult,
            Item::SpeechSegment(_) => ItemType::SpeechSegment,
            Item::SpeechResult(_) => ItemType::SpeechResult,
        }
    }

    // ── Text constructors ────────────────────────────────────────────────────

    /// A default-kind [`Item::Text`].
    pub fn text(text: impl Into<String>) -> Self {
        Item::Text {
            text: text.into(),
            kind: TextKind::Default,
        }
    }

    /// A reasoning / chain-of-thought [`Item::Text`].
    pub fn reasoning(text: impl Into<String>) -> Self {
        Item::Text {
            text: text.into(),
            kind: TextKind::Reasoning,
        }
    }

    // ── Message constructors ─────────────────────────────────────────────────

    /// A [`Item::Message`] with the given role and content parts.
    pub fn message(role: MessageRole, content: impl Into<Vec<Item>>) -> Self {
        Item::Message(Message::new(role, content))
    }

    /// A `system` [`Item::Message`].
    pub fn system_message(content: impl Into<Vec<Item>>) -> Self {
        Item::message(MessageRole::System, content)
    }

    /// A `user` [`Item::Message`].
    pub fn user_message(content: impl Into<Vec<Item>>) -> Self {
        Item::message(MessageRole::User, content)
    }

    /// An `assistant` [`Item::Message`].
    pub fn assistant_message(content: impl Into<Vec<Item>>) -> Self {
        Item::message(MessageRole::Assistant, content)
    }

    /// A `developer` [`Item::Message`].
    pub fn developer_message(content: impl Into<Vec<Item>>) -> Self {
        Item::message(MessageRole::Developer, content)
    }

    /// A `tool` [`Item::Message`].
    pub fn tool_message(content: impl Into<Vec<Item>>) -> Self {
        Item::message(MessageRole::Tool, content)
    }

    // ── Binary / tensor constructors ─────────────────────────────────────────

    /// An [`Item::Bytes`] carrying an opaque byte buffer.
    pub fn bytes(data: impl Into<Vec<u8>>) -> Self {
        Item::Bytes(data.into())
    }

    /// An [`Item::Tensor`] from a data type, shape, and raw element bytes.
    pub fn tensor(
        data_type: TensorDataType,
        shape: impl Into<Vec<i64>>,
        data: impl Into<Vec<u8>>,
    ) -> Self {
        Item::Tensor(Tensor {
            data_type,
            shape: shape.into(),
            data: data.into(),
        })
    }

    /// A [`TensorDataType::Float`] [`Item::Tensor`] from `f32` elements.
    pub fn float_tensor(shape: impl Into<Vec<i64>>, data: &[f32]) -> Self {
        let mut bytes = Vec::with_capacity(data.len() * 4);
        for f in data {
            bytes.extend_from_slice(&f.to_le_bytes());
        }
        Item::Tensor(Tensor {
            data_type: TensorDataType::Float,
            shape: shape.into(),
            data: bytes,
        })
    }

    // ── Image / audio constructors ───────────────────────────────────────────

    /// An [`Item::Image`] from inline bytes and an optional format hint.
    pub fn image_data(data: impl Into<Vec<u8>>, format: Option<impl Into<String>>) -> Self {
        Item::Image(Image {
            source: MediaSource::Data(data.into()),
            format: format.map(Into::into),
        })
    }

    /// An [`Item::Image`] referencing external content by URI.
    pub fn image_uri(uri: impl Into<String>, format: Option<impl Into<String>>) -> Self {
        Item::Image(Image {
            source: MediaSource::Uri(uri.into()),
            format: format.map(Into::into),
        })
    }

    /// An [`Item::Audio`] from inline bytes, format hint, and PCM layout.
    pub fn audio_data(
        data: impl Into<Vec<u8>>,
        format: Option<impl Into<String>>,
        sample_rate: i32,
        channels: i32,
    ) -> Self {
        Item::Audio(Audio {
            source: MediaSource::Data(data.into()),
            format: format.map(Into::into),
            sample_rate,
            channels,
        })
    }

    /// An [`Item::Audio`] referencing external content by URI.
    pub fn audio_uri(
        uri: impl Into<String>,
        format: Option<impl Into<String>>,
        sample_rate: i32,
        channels: i32,
    ) -> Self {
        Item::Audio(Audio {
            source: MediaSource::Uri(uri.into()),
            format: format.map(Into::into),
            sample_rate,
            channels,
        })
    }

    // ── Tool constructors ────────────────────────────────────────────────────

    /// A [`Item::ToolCall`].
    pub fn tool_call(
        call_id: impl Into<String>,
        name: impl Into<String>,
        arguments: impl Into<String>,
    ) -> Self {
        Item::ToolCall(ToolCall {
            call_id: call_id.into(),
            name: name.into(),
            arguments: arguments.into(),
        })
    }

    /// A [`Item::ToolResult`].
    pub fn tool_result(call_id: impl Into<String>, result: impl Into<String>) -> Self {
        Item::ToolResult(ToolResult {
            call_id: call_id.into(),
            result: result.into(),
        })
    }

    // ── Accessors ────────────────────────────────────────────────────────────

    /// The text content, if this is a [`Item::Text`].
    pub fn as_text(&self) -> Option<&str> {
        match self {
            Item::Text { text, .. } => Some(text),
            _ => None,
        }
    }

    /// The [`Message`], if this is a [`Item::Message`].
    pub fn as_message(&self) -> Option<&Message> {
        match self {
            Item::Message(m) => Some(m),
            _ => None,
        }
    }

    /// The [`Tensor`], if this is a [`Item::Tensor`].
    pub fn as_tensor(&self) -> Option<&Tensor> {
        match self {
            Item::Tensor(t) => Some(t),
            _ => None,
        }
    }

    /// The [`ToolCall`], if this is a [`Item::ToolCall`].
    pub fn as_tool_call(&self) -> Option<&ToolCall> {
        match self {
            Item::ToolCall(c) => Some(c),
            _ => None,
        }
    }

    /// The [`SpeechResult`], if this is a [`Item::SpeechResult`].
    pub fn as_speech_result(&self) -> Option<&SpeechResult> {
        match self {
            Item::SpeechResult(r) => Some(r),
            _ => None,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn item_type_matches_variant() {
        assert_eq!(Item::text("hi").item_type(), ItemType::Text);
        assert_eq!(Item::bytes(vec![1, 2]).item_type(), ItemType::Bytes);
        assert_eq!(
            Item::tool_call("c", "f", "{}").item_type(),
            ItemType::ToolCall
        );
    }

    #[test]
    fn message_helpers() {
        let m = Item::user_message(vec![Item::text("hello"), Item::text(" world")]);
        let msg = m.as_message().unwrap();
        assert_eq!(msg.role, MessageRole::User);
        assert!(!msg.is_simple_text());
        assert_eq!(msg.text(), "hello world");

        let simple = Message::new(MessageRole::System, vec![Item::text("x")]);
        assert!(simple.is_simple_text());
    }

    #[test]
    fn float_tensor_round_trips_bytes() {
        let values = [1.0f32, -2.5, 3.25];
        let item = Item::float_tensor(vec![3], &values);
        let t = item.as_tensor().unwrap();
        assert_eq!(t.data_type, TensorDataType::Float);
        assert_eq!(t.shape, vec![3]);
        assert_eq!(t.as_f32().unwrap(), values);
    }

    #[test]
    fn native_enum_mappings_round_trip() {
        for kind in [TextKind::Default, TextKind::Reasoning, TextKind::OpenAiJson] {
            assert_eq!(TextKind::from_native(kind.to_native()), kind);
        }
        for role in [
            MessageRole::None,
            MessageRole::System,
            MessageRole::User,
            MessageRole::Assistant,
            MessageRole::Tool,
            MessageRole::Developer,
        ] {
            assert_eq!(MessageRole::from_native(role.to_native()), role);
        }
        for dt in [
            TensorDataType::Undefined,
            TensorDataType::Float,
            TensorDataType::Int64,
            TensorDataType::BFloat16,
            TensorDataType::Float8E8M0,
        ] {
            assert_eq!(TensorDataType::from_native(dt.to_native()), dt);
        }
    }
}
