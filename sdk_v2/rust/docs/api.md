# Foundry Local Rust SDK — Public API Reference

> Reference for the public API in `sdk_v2/rust/src`. Maintained by hand until the
> crate is published; see [GENERATE-DOCS.md](../GENERATE-DOCS.md).

## Table of Contents

- [Entry Point](#entry-point)
  - [FoundryLocalManager](#foundrylocalmanager)
  - [FoundryLocalConfig](#foundrylocalconfig)
  - [Logger](#logger)
  - [LogLevel](#loglevel)
- [Model Catalog](#model-catalog)
  - [Catalog](#catalog)
  - [Model](#model)
- [OpenAI Clients](#openai-clients)
  - [ChatClient](#chatclient)
  - [ChatCompletionStream](#chatcompletionstream)
  - [EmbeddingClient](#embeddingclient)
  - [EmbeddingResponse](#embeddingresponse)
  - [AudioClient](#audioclient)
  - [AudioTranscriptionStream](#audiotranscriptionstream)
  - [AudioTranscriptionResponse](#audiotranscriptionresponse)
  - [TranscriptionSegment](#transcriptionsegment)
  - [TranscriptionWord](#transcriptionword)
  - [JsonStream\<T\>](#jsonstreamt)
- [Inference API](#inference-api)
  - [Session](#session)
  - [ChatSession](#chatsession)
  - [EmbeddingsSession](#embeddingssession)
  - [AudioSession](#audiosession)
  - [ItemQueue](#itemqueue)
  - [ItemStream](#itemstream)
  - [Item](#item)
  - [Message](#message)
  - [Tensor](#tensor)
  - [Request](#request)
  - [RequestOptions](#requestoptions)
  - [Response](#response)
  - [FinishReason](#finishreason)
  - [ToolDefinition](#tooldefinition)
- [Types](#types)
  - [ModelInfo](#modelinfo)
  - [ChatResponseFormat](#chatresponseformat)
  - [ChatToolChoice](#chattoolchoice)
  - [DeviceType](#devicetype)
  - [PromptTemplate](#prompttemplate)
  - [Runtime](#runtime)
  - [ModelSettings](#modelsettings)
  - [Parameter](#parameter)
- [Error Handling](#error-handling)
  - [FoundryLocalError](#foundrylocalerror)
  - [NativeErrorCode](#nativeerrorcode)
- [Re-exported OpenAI Types](#re-exported-openai-types)

---

## Entry Point

### FoundryLocalManager

Primary entry point for interacting with Foundry Local. Shared instance — while any handle is alive, `create()` returns the same instance; it is torn down when the last handle is dropped.

```rust
pub struct FoundryLocalManager { /* private fields */ }
```

| Method | Signature | Description |
|--------|-----------|-------------|
| `create` | `fn create(config: FoundryLocalConfig) -> Result<Arc<Self>, FoundryLocalError>` | Initialise the SDK. Returns a shared handle: while any handle is alive, all calls return the same instance (config ignored after the first). Once the last handle is dropped the native manager is torn down via `Drop`, and a later call builds a fresh instance. |
| `catalog` | `fn catalog(&self) -> &Catalog` | Access the model catalog. |
| `urls` | `fn urls(&self) -> Result<Vec<String>, FoundryLocalError>` | URLs the local web service is listening on. Empty until `start_web_service` is called. |
| `start_web_service` | `async fn start_web_service(&self) -> Result<(), FoundryLocalError>` | Start the local web service. Retrieve listening URLs via `urls()`. |
| `stop_web_service` | `async fn stop_web_service(&self) -> Result<(), FoundryLocalError>` | Stop the local web service. |

---

### FoundryLocalConfig

User-facing configuration for initializing the SDK. Fields are private; use
the builder methods to customise.

```rust
pub struct FoundryLocalConfig { /* private fields */ }
```

| Method | Signature | Description |
|--------|-----------|-------------|
| `new` | `fn new(app_name: impl Into<String>) -> Self` | Create a new configuration. All optional fields default to `None`. |
| `app_data_dir` | `fn app_data_dir(self, dir: impl Into<String>) -> Self` | Override the application-data directory. |
| `model_cache_dir` | `fn model_cache_dir(self, dir: impl Into<String>) -> Self` | Override the model-cache directory. |
| `logs_dir` | `fn logs_dir(self, dir: impl Into<String>) -> Self` | Override the logs directory. |
| `log_level` | `fn log_level(self, level: LogLevel) -> Self` | Set the log level. |
| `web_service_urls` | `fn web_service_urls(self, urls: impl Into<String>) -> Self` | Set the web-service listen URLs. |
| `service_endpoint` | `fn service_endpoint(self, endpoint: impl Into<String>) -> Self` | Set an external service endpoint URL. |
| `catalog_url` | `fn catalog_url(self, url: impl Into<String>) -> Self` | Add a catalog source URL (no filter override). May be called multiple times. |
| `catalog_url_with_filter` | `fn catalog_url_with_filter(self, url: impl Into<String>, filter_override: impl Into<String>) -> Self` | Add a catalog source URL with a filter override. |
| `catalog_region` | `fn catalog_region(self, region: impl Into<String>) -> Self` | Set the Azure region used by the catalog service. |
| `library_path` | `fn library_path(self, path: impl Into<String>) -> Self` | Override the path to the native core library. |
| `additional_setting` | `fn additional_setting(self, key: impl Into<String>, value: impl Into<String>) -> Self` | Add a key-value pair to additional settings. |
| `logger` | `fn logger(self, logger: impl Logger + 'static) -> Self` | Provide an application logger (stub — not yet wired into native core). |

**Example:**
```rust
let config = FoundryLocalConfig::new("my_app")
    .log_level(LogLevel::Debug)
    .model_cache_dir("/path/to/cache");
```

---

### LogLevel

```rust
pub enum LogLevel {
    Trace,
    Debug,
    Info,
    Warn,
    Error,
    Fatal,
}
```

---

### Logger

Application logger trait. Implement this to receive SDK log messages.

> **Note:** Stub — not yet wired into the native core. Stored in configuration for future use.

```rust
pub trait Logger: Send + Sync {
    fn log(&self, level: LogLevel, message: &str);
}
```

---

### Catalog

Discovers, caches, and looks up available models.

```rust
pub struct Catalog { /* private fields */ }
```

| Method | Signature | Description |
|--------|-----------|-------------|
| `name` | `fn name(&self) -> &str` | Catalog name as reported by the native core. |
| `update_models` | `async fn update_models(&self) -> Result<(), FoundryLocalError>` | Refresh catalog if cache expired or invalidated. |
| `get_models` | `async fn get_models(&self) -> Result<Vec<Arc<Model>>, FoundryLocalError>` | Return all known models. |
| `get_model` | `async fn get_model(&self, alias: &str) -> Result<Arc<Model>, FoundryLocalError>` | Look up a model by alias. |
| `get_model_variant` | `async fn get_model_variant(&self, id: &str) -> Result<Arc<Model>, FoundryLocalError>` | Look up a variant by unique id. |
| `get_cached_models` | `async fn get_cached_models(&self) -> Result<Vec<Arc<Model>>, FoundryLocalError>` | Return only variants cached on disk. |
| `get_loaded_models` | `async fn get_loaded_models(&self) -> Result<Vec<Arc<Model>>, FoundryLocalError>` | Return model variants currently loaded in memory. |
| `get_model_versions` | `async fn get_model_versions(&self, model_alias: &str, model_name: Option<&str>, max_versions: u32) -> Result<Vec<Arc<Model>>, FoundryLocalError>` | Return all versions for an alias, optionally filtered to a single model name. `max_versions` caps the results per model name; `0` returns all. |

---

### Model

Groups one or more variants sharing the same alias. By default, the cached variant is selected.

```rust
pub struct Model { /* private fields */ }
```

| Method | Signature | Description |
|--------|-----------|-------------|
| `alias` | `fn alias(&self) -> &str` | Alias shared by all variants. |
| `id` | `fn id(&self) -> &str` | Unique identifier of the selected variant. |
| `variants` | `fn variants(&self) -> Vec<Arc<Model>>` | All variants in this model. |
| `select_variant` | `fn select_variant(&self, variant: &Model) -> Result<(), FoundryLocalError>` | Select a variant from `variants()`. |
| `select_variant_by_id` | `fn select_variant_by_id(&self, id: &str) -> Result<(), FoundryLocalError>` | Select a variant by its unique id string. |
| `is_cached` | `async fn is_cached(&self) -> Result<bool, FoundryLocalError>` | Whether the selected variant is cached on disk. |
| `is_loaded` | `async fn is_loaded(&self) -> Result<bool, FoundryLocalError>` | Whether the selected variant is loaded in memory. |
| `download` | `async fn download<F>(&self, progress: Option<F>) -> Result<(), FoundryLocalError>` | Download the selected variant. `F: FnMut(f64) + Send + 'static` — receives progress as a percentage (0.0–100.0). |
| `path` | `async fn path(&self) -> Result<PathBuf, FoundryLocalError>` | Local file-system path of the selected variant. |
| `load` | `async fn load(&self) -> Result<(), FoundryLocalError>` | Load the selected variant into memory. |
| `unload` | `async fn unload(&self) -> Result<(), FoundryLocalError>` | Unload the selected variant from memory. |
| `remove_from_cache` | `async fn remove_from_cache(&self) -> Result<(), FoundryLocalError>` | Remove the selected variant from the local cache. |
| `create_chat_client` | `fn create_chat_client(&self) -> ChatClient` | **Deprecated** — use `ChatSession::new(&model)`. Create a ChatClient bound to the selected variant. |
| `create_audio_client` | `fn create_audio_client(&self) -> AudioClient` | **Deprecated** — use `AudioSession::new(&model)`. Create an AudioClient bound to the selected variant. |

---

## OpenAI Clients

> **Deprecated.** The OpenAI direct clients (`ChatClient`, `EmbeddingClient`, `AudioClient`,
> `LiveAudioTranscriptionSession`) are deprecated in favor of the Session API
> ([`ChatSession`](#chatsession), [`EmbeddingsSession`](#embeddingssession),
> [`AudioSession`](#audiosession)). They remain available for backward compatibility.

### ChatClient

OpenAI-compatible chat completions backed by a local model. Uses a consuming builder pattern.

```rust
pub struct ChatClient { /* private fields */ }
```

**Builder methods** (all `mut self -> Self`):

| Method | Signature | Description |
|--------|-----------|-------------|
| `frequency_penalty` | `fn frequency_penalty(mut self, v: f64) -> Self` | Set the frequency penalty. |
| `max_tokens` | `fn max_tokens(mut self, v: u32) -> Self` | Maximum tokens to generate. |
| `n` | `fn n(mut self, v: u32) -> Self` | Number of completions. |
| `temperature` | `fn temperature(mut self, v: f64) -> Self` | Sampling temperature. |
| `presence_penalty` | `fn presence_penalty(mut self, v: f64) -> Self` | Presence penalty. |
| `top_p` | `fn top_p(mut self, v: f64) -> Self` | Nucleus sampling probability. |
| `top_k` | `fn top_k(mut self, v: u32) -> Self` | Top-k sampling *(Foundry extension)*. |
| `random_seed` | `fn random_seed(mut self, v: u64) -> Self` | Random seed for reproducibility *(Foundry extension)*. |
| `response_format` | `fn response_format(mut self, v: ChatResponseFormat) -> Self` | Desired response format. |
| `tool_choice` | `fn tool_choice(mut self, v: ChatToolChoice) -> Self` | Tool choice strategy. |

**Completion methods:**

| Method | Signature | Description |
|--------|-----------|-------------|
| `complete_chat` | `async fn complete_chat(&self, messages: &[ChatCompletionRequestMessage], tools: Option<&[ChatCompletionTools]>) -> Result<CreateChatCompletionResponse, FoundryLocalError>` | Non-streaming chat completion. |
| `complete_streaming_chat` | `async fn complete_streaming_chat(&self, messages: &[ChatCompletionRequestMessage], tools: Option<&[ChatCompletionTools]>) -> Result<ChatCompletionStream, FoundryLocalError>` | Streaming chat completion. |

**Example:**
```rust
let client = model.create_chat_client()
    .temperature(0.7)
    .max_tokens(256);
```

---

### ChatCompletionStream

```rust
pub type ChatCompletionStream = JsonStream<CreateChatCompletionStreamResponse>;
```

A stream of `CreateChatCompletionStreamResponse` chunks. Use with `StreamExt::next()`.

---

### EmbeddingClient

OpenAI-compatible embedding generation backed by a local model.

| Method | Description |
|---|---|
| `new(model_id, core)` | *(internal)* Create a new client |
| `generate_embedding(input: &str) -> Result<CreateEmbeddingResponse>` | Generate embedding for a single input |
| `generate_embeddings(inputs: &[&str]) -> Result<CreateEmbeddingResponse>` | Generate embeddings for multiple inputs |

Returns `async_openai::types::embeddings::CreateEmbeddingResponse`:

| Field | Type | Description |
|---|---|---|
| `model` | `String` | Model used for generation |
| `object` | `String` | Object type (always `"list"`) |
| `data` | `Vec<Embedding>` | List of embedding results |
| `usage` | `Usage` | Token usage information |

Each `Embedding` in `data`:

| Field | Type | Description |
|---|---|---|
| `index` | `u32` | Index of this embedding in the batch |
| `embedding` | `Vec<f32>` | The embedding vector (float32) |

---

### AudioClient

OpenAI-compatible audio transcription backed by a local model.

```rust
pub struct AudioClient { /* private fields */ }
```

**Builder methods** (all `mut self -> Self`):

| Method | Signature | Description |
|--------|-----------|-------------|
| `language` | `fn language(mut self, lang: impl Into<String>) -> Self` | Language hint for transcription. |
| `temperature` | `fn temperature(mut self, v: f64) -> Self` | Sampling temperature. |

**Transcription methods:**

| Method | Signature | Description |
|--------|-----------|-------------|
| `transcribe` | `async fn transcribe(&self, audio_file_path: impl AsRef<Path>) -> Result<AudioTranscriptionResponse, FoundryLocalError>` | Transcribe an audio file. |
| `transcribe_streaming` | `async fn transcribe_streaming(&self, audio_file_path: impl AsRef<Path>) -> Result<AudioTranscriptionStream, FoundryLocalError>` | Streaming transcription. |

**Example:**
```rust
let client = model.create_audio_client()
    .language("en")
    .temperature(0.2);
```

---

### AudioTranscriptionStream

```rust
pub type AudioTranscriptionStream = JsonStream<AudioTranscriptionResponse>;
```

A stream of `AudioTranscriptionResponse` chunks. Use with `StreamExt::next()`.

---

### AudioTranscriptionResponse

```rust
pub struct AudioTranscriptionResponse {
    pub text: String,                                      // The transcribed text
    pub language: Option<String>,                          // Language of input audio (if detected)
    pub duration: Option<f64>,                             // Duration in seconds (if available)
    pub segments: Option<Vec<TranscriptionSegment>>,       // Transcription segments (if available)
    pub words: Option<Vec<TranscriptionWord>>,             // Words with timestamps (if available)
}
```

Derives: `Debug`, `Clone`, `Deserialize`, `Serialize`

---

### TranscriptionSegment

A segment of a transcription, as returned by the OpenAI-compatible API.

```rust
pub struct TranscriptionSegment {
    pub id: i32,
    pub seek: i32,
    pub start: f64,
    pub end: f64,
    pub text: String,
    pub tokens: Option<Vec<i32>>,
    pub temperature: Option<f64>,
    pub avg_logprob: Option<f64>,
    pub compression_ratio: Option<f64>,
    pub no_speech_prob: Option<f64>,
}
```

Derives: `Debug`, `Clone`, `Deserialize`, `Serialize`

---

### TranscriptionWord

A word with timing information, as returned by the OpenAI-compatible API.

```rust
pub struct TranscriptionWord {
    pub word: String,
    pub start: f64,
    pub end: f64,
}
```

Derives: `Debug`, `Clone`, `Deserialize`, `Serialize`

---

### JsonStream\<T\>

Generic stream that deserializes each received JSON string chunk into `T`. Empty chunks are silently skipped.

```rust
pub struct JsonStream<T> { /* private fields */ }

impl<T> Unpin for JsonStream<T> {}
impl<T: DeserializeOwned> Stream for JsonStream<T> {
    type Item = Result<T, FoundryLocalError>;
}
```

---

## Inference API

The low-level, modality-agnostic inference API, mirroring the C++/C#/Python/JS SDKs
but expressed idiomatically in Rust. Submit a [`Request`](#request) of
[`Item`](#item)s to a [`Session`](#session) and receive a [`Response`](#response)
of `Item`s.

The design splits types by whether they own a native lifetime:

- **Pure-data values** — `Item`, `Request`, `Response`, and their option/enum
  types are plain owned data: `Send + Sync + Clone`, holding no native handle.
  Build them freely, move them across threads, and drop them at will; the native
  layer copies whatever it needs when a request is processed.
- **Handle-backed** — `Session` (and the typed `ChatSession` /
  `EmbeddingsSession` / `AudioSession`) and `ItemQueue` wrap a shared native
  object (`Arc`), so cloning yields another handle to the same underlying
  session/queue.

### Session

A stateful inference session bound to a loaded [`Model`](#model). The base,
modality-agnostic entry point.

```rust
pub struct Session { /* private fields */ }
```

| Method | Signature | Description |
|--------|-----------|-------------|
| `new` | `async fn new(model: &Model) -> Result<Session, FoundryLocalError>` | Open a session on a loaded model. |
| `process_request` | `async fn process_request(&self, request: Request) -> Result<Response, FoundryLocalError>` | Process a request and return the complete response. Cancel-on-drop: dropping the future (e.g. via `tokio::time::timeout` or `select!`) cancels the in-flight request. |
| `process_streaming_request` | `fn process_streaming_request(&self, request: Request) -> ItemStream` | Process a request, streaming each output item as it is produced. Dropping the stream cancels generation. |
| `set_options` | `async fn set_options(&self, options: RequestOptions) -> Result<(), FoundryLocalError>` | Apply session-scoped options that persist across subsequent requests. |
| `create_input_queue` | `fn create_input_queue(&self) -> Result<ItemQueue, FoundryLocalError>` | Create an [`ItemQueue`](#itemqueue) for streaming incremental input into a request. |

### ChatSession

A chat-oriented session adding tool registration and turn management.
Dereferences to [`Session`](#session), so all base methods are available.

```rust
pub struct ChatSession { /* private fields */ }
impl Deref for ChatSession { type Target = Session; }
```

| Method | Signature | Description |
|--------|-----------|-------------|
| `new` | `async fn new(model: &Model) -> Result<ChatSession, FoundryLocalError>` | Open a chat session on a loaded model. Returns `Validation` if the model's task is not `chat-completion` or `vision-language-chat`. |
| `add_tool_definition` | `async fn add_tool_definition(&self, definition: ToolDefinition) -> Result<(), FoundryLocalError>` | Register a tool for the lifetime of the session. |
| `remove_tool_definition` | `async fn remove_tool_definition(&self, name: impl Into<String>) -> Result<bool, FoundryLocalError>` | Remove a tool by name; returns whether one was removed. |
| `turn_count` | `fn turn_count(&self) -> usize` | The number of completed conversation turns. |
| `undo_turns` | `async fn undo_turns(&self, count: usize) -> Result<(), FoundryLocalError>` | Rewind the last `count` turns. |
| `into_session` | `fn into_session(self) -> Session` | Consume this handle, yielding the base session. |

### EmbeddingsSession

An embeddings-oriented session producing dense vectors for text input.
Dereferences to [`Session`](#session).

```rust
pub struct EmbeddingsSession { /* private fields */ }
impl Deref for EmbeddingsSession { type Target = Session; }
```

| Method | Signature | Description |
|--------|-----------|-------------|
| `new` | `async fn new(model: &Model) -> Result<EmbeddingsSession, FoundryLocalError>` | Open an embeddings session on a loaded model. Returns `Validation` if the model's task is not `embeddings`. |
| `embed` | `async fn embed(&self, input: impl Into<String>) -> Result<Vec<f32>, FoundryLocalError>` | Embed a single text input. |
| `embed_batch` | `async fn embed_batch(&self, inputs: Vec<String>) -> Result<Vec<Vec<f32>>, FoundryLocalError>` | Embed a batch of inputs, one vector per input (in order). |
| `into_session` | `fn into_session(self) -> Session` | Consume this handle, yielding the base session. |

### AudioSession

An audio-oriented session for speech tasks (e.g. transcription).
Dereferences to [`Session`](#session).

```rust
pub struct AudioSession { /* private fields */ }
impl Deref for AudioSession { type Target = Session; }
```

| Method | Signature | Description |
|--------|-----------|-------------|
| `new` | `async fn new(model: &Model) -> Result<AudioSession, FoundryLocalError>` | Open an audio session on a loaded model. Returns `Validation` if the model's task is not `automatic-speech-recognition`. |
| `transcribe` | `async fn transcribe(&self, audio: Item) -> Result<String, FoundryLocalError>` | Transcribe a single audio item, returning the recognized text. |
| `into_session` | `fn into_session(self) -> Session` | Consume this handle, yielding the base session. |

### ItemQueue

A thread-safe, multi-producer / multi-consumer queue of [`Item`](#item)s for
streaming incremental input into a request (e.g. live audio). Create one from a
session via [`Session::create_input_queue`](#session) and attach it to a request
with [`Request::with_input_queue`](#request). Cloning yields another handle to the
same underlying queue.

```rust
pub struct ItemQueue { /* private fields */ }
```

| Method | Signature | Description |
|--------|-----------|-------------|
| `push` | `fn push(&self, item: &Item) -> Result<(), FoundryLocalError>` | Push an item, transferring a native copy into the queue. |
| `try_pop` | `fn try_pop(&self) -> Result<Option<Item>, FoundryLocalError>` | Pop the next item (`Ok(None)` if currently empty), or `Err` if converting the native item fails. |
| `len` | `fn len(&self) -> usize` | Number of items currently buffered. |
| `is_empty` | `fn is_empty(&self) -> bool` | Whether the queue currently holds no items. |
| `mark_finished` | `fn mark_finished(&self)` | Signal that no more items will be pushed. |
| `is_finished` | `fn is_finished(&self) -> bool` | Whether `mark_finished` has been called. |

### ItemStream

An asynchronous stream of output [`Item`](#item)s from
[`Session::process_streaming_request`](#session). Implements
[`futures_core::Stream`]; dropping it cancels generation.

```rust
pub struct ItemStream { /* private fields */ }

impl Unpin for ItemStream {}
impl Stream for ItemStream {
    type Item = Result<Item, FoundryLocalError>;
}
```

| Method | Signature | Description |
|--------|-----------|-------------|
| `response` | `async fn response(&mut self) -> Result<Response, FoundryLocalError>` | Await the terminal [`Response`](#response) (finish reason, usage, final items). May be called after draining the item stream or instead of draining it; the terminal response can be taken only once. |

### Item

A single unit of input or output data exchanged with a session. A pure-data,
owned enum (`Send + Sync + Clone`) — construct with the associated functions and
inspect by pattern matching.

```rust
pub enum Item {
    Text { text: String, kind: TextKind },
    Message(Message),
    Bytes(Vec<u8>),
    Tensor(Tensor),
    Image(Image),
    Audio(Audio),
    ToolCall(ToolCall),
    ToolResult(ToolResult),
    SpeechSegment(SpeechSegment), // output-only
    SpeechResult(SpeechResult),   // output-only
}
```

**Constructors:**

| Method | Signature | Description |
|--------|-----------|-------------|
| `text` | `fn text(text: impl Into<String>) -> Item` | A `Default`-kind text item. |
| `reasoning` | `fn reasoning(text: impl Into<String>) -> Item` | A `Reasoning`-kind text item. |
| `message` | `fn message(role: MessageRole, content: impl Into<Vec<Item>>) -> Item` | A chat message with content parts. |
| `system_message` / `user_message` / `assistant_message` / `developer_message` / `tool_message` | `fn(content: impl Into<Vec<Item>>) -> Item` | Role-specific message constructors. |
| `bytes` | `fn bytes(data: impl Into<Vec<u8>>) -> Item` | An opaque byte buffer. |
| `tensor` | `fn tensor(data_type: TensorDataType, shape: impl Into<Vec<i64>>, data: impl Into<Vec<u8>>) -> Item` | A numeric tensor from raw bytes. |
| `float_tensor` | `fn float_tensor(shape: impl Into<Vec<i64>>, data: &[f32]) -> Item` | A `Float`-typed tensor from `f32` values. |
| `image_data` / `image_uri` | `fn(…, format: Option<impl Into<String>>) -> Item` | An inline or URI-referenced image. |
| `audio_data` / `audio_uri` | `fn(…) -> Item` | An inline or URI-referenced audio clip. |
| `tool_call` | `fn tool_call(call_id: impl Into<String>, name: impl Into<String>, arguments: impl Into<String>) -> Item` | A model-issued tool call. |
| `tool_result` | `fn tool_result(call_id: impl Into<String>, result: impl Into<String>) -> Item` | The result of executing a tool call. |

**Accessors:**

| Method | Signature | Description |
|--------|-----------|-------------|
| `item_type` | `fn item_type(&self) -> ItemType` | The discriminant of this item. |
| `as_text` | `fn as_text(&self) -> Option<&str>` | The text, if this is a `Text` item. |
| `as_message` | `fn as_message(&self) -> Option<&Message>` | The message, if this is a `Message` item. |
| `as_tensor` | `fn as_tensor(&self) -> Option<&Tensor>` | The tensor, if this is a `Tensor` item. |
| `as_tool_call` | `fn as_tool_call(&self) -> Option<&ToolCall>` | The tool call, if this is a `ToolCall` item. |
| `as_speech_result` | `fn as_speech_result(&self) -> Option<&SpeechResult>` | The speech result, if this is a `SpeechResult` item. |

Supporting enums: `ItemType`, `TextKind` (`Default`, `Reasoning`, `OpenAiJson`),
`MessageRole` (`None`, `System`, `User`, `Assistant`, `Tool`, `Developer`),
`TensorDataType`, `SpeechSegmentKind`, and `MediaSource` (`Data(Vec<u8>)` /
`Uri(String)`).

### Message

A chat message with a role and nested content parts.

```rust
pub struct Message {
    pub role: MessageRole,
    pub content: Vec<Item>,
    pub name: Option<String>,
}
```

| Method | Signature | Description |
|--------|-----------|-------------|
| `new` | `fn new(role: MessageRole, content: impl Into<Vec<Item>>) -> Message` | Construct a message. |
| `with_name` | `fn with_name(mut self, name: impl Into<String>) -> Message` | Attach a participant name (builder-style). |
| `is_simple_text` | `fn is_simple_text(&self) -> bool` | Whether the content is a single text part. |
| `text` | `fn text(&self) -> String` | The concatenated text of all text content parts. |

### Tensor

A numeric tensor payload (e.g. an embedding vector).

```rust
pub struct Tensor {
    pub data_type: TensorDataType,
    pub shape: Vec<i64>,
    pub data: Vec<u8>,
}
```

| Method | Signature | Description |
|--------|-----------|-------------|
| `as_f32` | `fn as_f32(&self) -> Option<Vec<f32>>` | Reinterpret the bytes as `f32` values, if `data_type` is `Float`. |

### Request

A unit of work submitted to a session. Pure data: owned input items, an optional
streaming [`ItemQueue`](#itemqueue), and optional [`RequestOptions`](#requestoptions).

```rust
pub struct Request {
    pub items: Vec<Item>,
    pub input_queue: Option<ItemQueue>,
    pub options: Option<RequestOptions>,
}
```

| Method | Signature | Description |
|--------|-----------|-------------|
| `new` | `fn new() -> Request` | An empty request. |
| `from_items` | `fn from_items(items: impl Into<Vec<Item>>) -> Request` | A request from a list of input items. |
| `with_item` | `fn with_item(mut self, item: Item) -> Request` | Append an input item (builder-style). |
| `with_input_queue` | `fn with_input_queue(mut self, queue: ItemQueue) -> Request` | Attach a streaming input queue (builder-style). |
| `with_options` | `fn with_options(mut self, options: RequestOptions) -> Request` | Attach per-request options (builder-style). |

### RequestOptions

Sampling / decoding parameters applied to a request (or, via
[`Session::set_options`](#session), to a session). Typed `search` fields and
`tool_choice` take precedence over `additional_options` on key collision.

```rust
pub struct RequestOptions {
    pub search: SearchOptions,
    pub tool_choice: Option<ToolChoice>,
    pub additional_options: Vec<(String, String)>,
}

pub struct SearchOptions {
    pub temperature: Option<f32>,
    pub top_p: Option<f32>,
    pub top_k: Option<i32>,
    pub max_output_tokens: Option<i32>,
    pub frequency_penalty: Option<f32>,
    pub presence_penalty: Option<f32>,
    pub seed: Option<i64>,
    pub early_stopping: Option<bool>,
    pub do_sample: Option<bool>,
}

pub enum ToolChoice { Auto, None, Required }
```

### Response

The result of processing a request: output items plus the finish reason and token
usage.

```rust
pub struct Response {
    pub items: Vec<Item>,
    pub finish_reason: FinishReason,
    pub usage: Usage,
}

pub struct Usage {
    pub prompt_tokens: u32,
    pub completion_tokens: u32,
    pub total_tokens: u32,
}
```

| Method | Signature | Description |
|--------|-----------|-------------|
| `text` | `fn text(&self) -> String` | The concatenated text of all text output items. |

### FinishReason

Why generation stopped. (Distinct from the OpenAI facade's finish reason, which is
re-exported as [`ChatFinishReason`](#re-exported-openai-types).)

```rust
pub enum FinishReason { None, Error, Stop, Length, ToolCalls }
```

### ToolDefinition

A tool the model may call, registered on a [`ChatSession`](#chatsession).

```rust
pub struct ToolDefinition {
    pub name: String,
    pub description: Option<String>,
    pub json_schema: String,
}
```

| Method | Signature | Description |
|--------|-----------|-------------|
| `new` | `fn new(name: impl Into<String>, json_schema: impl Into<String>) -> ToolDefinition` | A tool with a name and JSON-schema parameters. |
| `with_description` | `fn with_description(mut self, description: impl Into<String>) -> ToolDefinition` | Attach a description (builder-style). |

---

## Types

### ModelInfo

Full metadata for a model variant as returned by the catalog.

```rust
pub struct ModelInfo {
    pub id: String,
    pub name: String,
    pub version: u64,
    pub alias: String,
    pub display_name: Option<String>,
    pub provider_type: String,
    pub uri: String,
    pub model_type: String,
    pub prompt_template: Option<PromptTemplate>,
    pub publisher: Option<String>,
    pub model_settings: Option<ModelSettings>,
    pub license: Option<String>,
    pub license_description: Option<String>,
    pub cached: bool,
    pub task: Option<String>,
    pub runtime: Option<Runtime>,
    pub file_size_mb: Option<u64>,
    pub supports_tool_calling: Option<bool>,
    pub max_output_tokens: Option<u64>,
    pub min_fl_version: Option<String>,
    pub created_at_unix: u64,
}
```

Derives: `Debug`, `Clone`, `Deserialize`

---

### ChatResponseFormat

```rust
pub enum ChatResponseFormat {
    Text,                   // Plain text output (default)
    JsonObject,             // JSON output (unstructured)
    JsonSchema(String),     // JSON constrained by schema string
    LarkGrammar(String),    // Lark grammar constraint (Foundry extension)
}
```

---

### ChatToolChoice

```rust
pub enum ChatToolChoice {
    None,               // Model will not call any tool
    Auto,               // Model decides whether to call a tool
    Required,           // Model must call at least one tool
    Function(String),   // Model must call the named function
}
```

---

### DeviceType

```rust
pub enum DeviceType {
    Invalid,
    CPU,
    GPU,
    NPU,
}
```

---

### PromptTemplate

```rust
pub struct PromptTemplate {
    pub system: Option<String>,
    pub user: Option<String>,
    pub assistant: Option<String>,
    pub prompt: Option<String>,
}
```

---

### Runtime

```rust
pub struct Runtime {
    pub device_type: DeviceType,
    pub execution_provider: String,
}
```

---

### ModelSettings

```rust
pub struct ModelSettings {
    pub parameters: Option<Vec<Parameter>>,
}
```

---

### Parameter

```rust
pub struct Parameter {
    pub name: String,
    pub value: Option<String>,
}
```

---

## Error Handling

### FoundryLocalError

```rust
pub enum FoundryLocalError {
    /// The native core library returned an error. Carries a stable code.
    Native { code: NativeErrorCode, message: String },

    /// The native core library could not be loaded.
    LibraryLoad { reason: String },

    /// A command executed against the native core returned an error.
    CommandExecution { reason: String },

    /// The provided configuration is invalid.
    InvalidConfiguration { reason: String },

    /// A model operation failed (load, unload, download, etc.).
    ModelOperation { reason: String },

    /// An HTTP request to the external service failed.
    HttpRequest(reqwest::Error),

    /// Serialization or deserialization of JSON data failed.
    Serialization(serde_json::Error),

    /// A validation check on user-supplied input failed.
    Validation { reason: String },

    /// An I/O error occurred.
    Io(std::io::Error),

    /// An internal SDK error (e.g. poisoned lock).
    Internal { reason: String },
}
```

| Method | Signature | Description |
|--------|-----------|-------------|
| `native_code` | `fn native_code(&self) -> Option<NativeErrorCode>` | The stable native error code for a `Native` error, else `None`. |
| `native_message` | `fn native_message(&self) -> Option<&str>` | The native error message for a `Native` error, else `None`. |

Implements: `Display`, `Error`, `From<serde_json::Error>`, `From<std::io::Error>`, `From<reqwest::Error>`

#### NativeErrorCode

Stable error codes reported by the native Foundry Local library. Surfaced via
[`FoundryLocalError::native_code`](#foundrylocalerror) so callers can branch on a
failure without matching message strings.

```rust
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
```

> **Note:** The `Result<T>` type alias (`std::result::Result<T, FoundryLocalError>`) is defined
> in `error.rs` for internal SDK use but is **not** re-exported from the crate root.
> Public API signatures use `Result<T, FoundryLocalError>` explicitly to avoid shadowing
> the standard `Result`.

---

## Re-exported OpenAI Types

The following types from `async_openai` are re-exported at the crate root for convenience:

**Request types:**
- `ChatCompletionRequestMessage`
- `ChatCompletionRequestSystemMessage`
- `ChatCompletionRequestUserMessage`
- `ChatCompletionRequestAssistantMessage`
- `ChatCompletionRequestToolMessage`
- `ChatCompletionTools`
- `ChatCompletionToolChoiceOption`
- `ChatCompletionNamedToolChoice`
- `FunctionObject`

**Response types:**
- `CreateChatCompletionResponse`
- `CreateChatCompletionStreamResponse`
- `ChatChoice`
- `ChatChoiceStream`
- `ChatCompletionResponseMessage`
- `ChatCompletionStreamResponseDelta`
- `CompletionUsage`
- `ChatFinishReason` (OpenAI finish reason; re-exported under this name to avoid
  colliding with the core inference [`FinishReason`](#finishreason))

**Tool call types:**
- `ChatCompletionMessageToolCall`
- `ChatCompletionMessageToolCallChunk`
- `ChatCompletionMessageToolCalls`
- `FunctionCall`
- `FunctionCallStream`
