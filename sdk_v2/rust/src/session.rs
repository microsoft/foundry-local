//! The [`Session`] handle and its typed variants.
//!
//! A `Session` binds a loaded [`Model`] to a stateful native inference session.
//! Unlike the pure-data [`Item`]/[`Request`]/[`Response`] values, a session is
//! *handle-backed*: it owns a native lifetime and is shared via an `Arc`, so it
//! is cheap to clone and safe to use from multiple tasks.
//!
//! [`Item`]: crate::Item
//! [`Request`]: crate::Request
//! [`Response`]: crate::Response

use std::ops::Deref;
use std::pin::Pin;
use std::sync::Arc;
use std::task::{Context, Poll};

use tokio::sync::{mpsc::UnboundedReceiver, oneshot};

use crate::detail::api::{Api, Kvps};
use crate::detail::model::Model;
use crate::detail::session::{run_item_streaming, NativeItemQueue, NativeRequest, NativeSession};
use crate::detail::task::spawn_blocking;
use crate::error::{FoundryLocalError, Result};
use crate::item::Item;
use crate::item_queue::ItemQueue;
use crate::request::{Request, RequestOptions};
use crate::response::Response;

/// A stateful inference session bound to a [`Model`].
///
/// `Session` is the low-level, modality-agnostic entry point: submit a
/// [`Request`] of [`Item`]s and receive a [`Response`] of [`Item`]s. For
/// higher-level, task-specific ergonomics use [`ChatSession`],
/// [`EmbeddingsSession`], or [`AudioSession`], which [`Deref`] to `Session`.
///
/// Cloning a `Session` produces another handle to the same underlying native
/// session (shared conversation state).
///
/// [`Item`]: crate::Item
#[derive(Clone)]
pub struct Session {
    inner: Arc<NativeSession>,
}

impl Session {
    /// Open a session on a loaded model.
    ///
    /// The model must already be loaded (see [`Model::load`]). The session
    /// inherits the model's default generation parameters until overridden with
    /// [`set_options`](Self::set_options) or per-request options.
    ///
    /// [`Model::load`]: crate::Model::load
    pub async fn new(model: &Model) -> Result<Session> {
        let native = model.selected_native().clone();
        let inner = spawn_blocking(move || NativeSession::create(&native)).await?;
        Ok(Session {
            inner: Arc::new(inner),
        })
    }

    /// Process a request and return the complete response.
    ///
    /// Runs on a blocking worker thread; the returned future resolves when
    /// generation finishes.
    ///
    /// # Cancellation
    ///
    /// The returned future is *cancel-on-drop*: if it is dropped before it
    /// resolves — for example via [`tokio::time::timeout`], `tokio::select!`, or
    /// aborting the task — the in-flight native request is cancelled and
    /// generation stops as soon as possible rather than running to completion on
    /// the detached worker. This mirrors the drop-cancels-generation behaviour of
    /// [`process_streaming_request`](Self::process_streaming_request).
    pub async fn process_request(&self, request: Request) -> Result<Response> {
        let inner = Arc::clone(&self.inner);
        // Create the native request up front (cheap) so its cancel handle can be
        // shared with the drop guard before the blocking work begins.
        let native = Arc::new(NativeRequest::new(Arc::clone(&inner.api))?);
        let native_task = Arc::clone(&native);
        let handle = tokio::task::spawn_blocking(move || {
            let _guard = inner.lock_ops();
            populate_native_request(&inner.api, &native_task, &request)?;
            let response = inner.process_request(&native_task)?;
            Response::from_native(&response)
        });

        // Cancel the in-flight request if this future is dropped before the
        // worker finishes; disarmed on normal completion. See `CancelGuard`.
        let guard = CancelGuard::new(native);
        let joined = handle.await;
        guard.disarm();
        joined.map_err(|e| FoundryLocalError::Internal {
            reason: format!("blocking task join error: {e}"),
        })?
    }

    /// Process a request, streaming each output [`Item`](crate::Item) as it is
    /// produced.
    ///
    /// The returned [`ItemStream`] yields items until generation completes;
    /// dropping it cancels generation.
    pub fn process_streaming_request(&self, request: Request) -> ItemStream {
        let Request {
            items,
            input_queue,
            options,
        } = request;
        let option_pairs = options
            .as_ref()
            .map(RequestOptions::to_pairs)
            .unwrap_or_default();
        let input_queue = input_queue.map(ItemQueue::into_native);
        let (rx, response_rx) =
            run_item_streaming(Arc::clone(&self.inner), items, input_queue, option_pairs);
        ItemStream {
            rx,
            response_rx: Some(response_rx),
        }
    }

    /// Apply session-scoped options that persist across subsequent requests.
    pub async fn set_options(&self, options: RequestOptions) -> Result<()> {
        let inner = Arc::clone(&self.inner);
        spawn_blocking(move || {
            let pairs = options.to_pairs();
            let kvps = Kvps::from_pairs(Arc::clone(&inner.api), pairs)?;
            inner.set_options(kvps.as_ptr())
        })
        .await
    }

    /// Create an [`ItemQueue`] for streaming incremental input into a request.
    ///
    /// Attach the returned queue to a [`Request`] via
    /// [`Request::with_input_queue`], then push items as they become available.
    pub fn create_input_queue(&self) -> Result<ItemQueue> {
        let native = NativeItemQueue::new(Arc::clone(&self.inner.api))?;
        Ok(ItemQueue::from_native(Arc::new(native)))
    }
}

impl std::fmt::Debug for Session {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Session").finish_non_exhaustive()
    }
}

/// Populate a freshly-created native request from a pure-data [`Request`].
///
/// The native layer copies every buffer it needs, so the owned `Request` (and
/// its items) may be dropped as soon as processing returns. The `NativeRequest`
/// is created by the caller (rather than here) so a cancel handle can be shared
/// with the drop guard before the blocking population/processing begins.
fn populate_native_request(
    api: &Arc<Api>,
    native: &NativeRequest,
    request: &Request,
) -> Result<()> {
    for item in &request.items {
        native.add_item_value(item)?;
    }
    if let Some(queue) = &request.input_queue {
        native.add_input_queue(queue.native())?;
    }
    let pairs = request.option_pairs();
    if !pairs.is_empty() {
        let kvps = Kvps::from_pairs(Arc::clone(api), pairs)?;
        native.set_options(kvps.as_ptr())?;
    }
    Ok(())
}

/// Cancels an in-flight native request if dropped while still armed.
///
/// [`Session::process_request`] holds one of these across the `.await` on its
/// blocking worker. If the caller drops that future first (via
/// [`tokio::time::timeout`], `tokio::select!`, task abort, …) the guard fires
/// `Request_Cancel` so generation stops promptly instead of running to
/// completion on the detached worker. Disarmed once the worker completes
/// normally.
struct CancelGuard {
    native: Arc<NativeRequest>,
    armed: bool,
}

impl CancelGuard {
    fn new(native: Arc<NativeRequest>) -> Self {
        Self {
            native,
            armed: true,
        }
    }

    fn disarm(mut self) {
        self.armed = false;
    }
}

impl Drop for CancelGuard {
    fn drop(&mut self) {
        if self.armed {
            self.native.cancel();
        }
    }
}

/// The terminal-response oneshot was dropped before the worker sent a response.
fn worker_ended_without_response() -> FoundryLocalError {
    FoundryLocalError::Internal {
        reason: "streaming response worker ended without a terminal response".into(),
    }
}

/// An asynchronous stream of output [`Item`](crate::Item)s from
/// [`Session::process_streaming_request`].
///
/// Implements [`futures_core::Stream`]; use with the `futures`/`tokio-stream`
/// combinators (e.g. `while let Some(item) = stream.next().await`).
pub struct ItemStream {
    rx: UnboundedReceiver<Result<Item>>,
    response_rx: Option<oneshot::Receiver<Result<Response>>>,
}

impl ItemStream {
    /// Wait for and return the terminal response, including finish reason and
    /// token usage.
    ///
    /// This may be called after draining the item stream or instead of draining
    /// it. The terminal response can be taken only once.
    pub async fn response(&mut self) -> Result<Response> {
        if self.response_rx.is_none() {
            return Err(FoundryLocalError::Validation {
                reason: "the streaming response has already been taken".into(),
            });
        }

        loop {
            match self.rx.try_recv() {
                Ok(Ok(_)) => continue,
                Ok(Err(error)) => return Err(error),
                Err(tokio::sync::mpsc::error::TryRecvError::Empty) => {}
                Err(tokio::sync::mpsc::error::TryRecvError::Disconnected) => break,
            }

            let response_rx = self.response_rx.as_mut().expect("checked above");
            tokio::select! {
                response = &mut *response_rx => {
                    self.response_rx.take();
                    while let Ok(item) = self.rx.try_recv() {
                        item?;
                    }
                    return response.map_err(|_| worker_ended_without_response())?;
                }
                item = self.rx.recv() => {
                    match item {
                        Some(Ok(_)) => {}
                        Some(Err(error)) => return Err(error),
                        None => break,
                    }
                }
            }
        }

        let response = self
            .response_rx
            .as_mut()
            .expect("checked above")
            .await
            .map_err(|_| worker_ended_without_response())?;
        self.response_rx.take();
        response
    }
}

impl Unpin for ItemStream {}

impl futures_core::Stream for ItemStream {
    type Item = Result<Item>;

    fn poll_next(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Option<Self::Item>> {
        self.rx.poll_recv(cx)
    }
}

/// A tool the model may call, registered on a [`ChatSession`].
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ToolDefinition {
    /// The tool's unique name.
    pub name: String,
    /// An optional human/model-readable description of what the tool does.
    pub description: Option<String>,
    /// A JSON Schema string describing the tool's parameters.
    pub json_schema: String,
}

impl ToolDefinition {
    /// A tool definition with a name and JSON-schema parameter description.
    pub fn new(name: impl Into<String>, json_schema: impl Into<String>) -> Self {
        Self {
            name: name.into(),
            description: None,
            json_schema: json_schema.into(),
        }
    }

    /// Attach a description (builder-style).
    pub fn with_description(mut self, description: impl Into<String>) -> Self {
        self.description = Some(description.into());
        self
    }
}

/// Reject a model whose task is not one of `allowed` before a typed session is
/// created.
///
/// The native session constructor requires the model to already be loaded and
/// only rejects a wrong-task model *after* that load requirement is satisfied.
/// The model's task, however, is catalog metadata available without loading, so
/// validating it here surfaces a precise, task-specific error regardless of load
/// state and keeps the error identical whether or not the model happens to be
/// loaded. This mirrors the C#, JavaScript, and Python bindings, which validate
/// the task the same way for the same reason.
fn validate_session_task(model: &Model, session: &str, allowed: &[&str]) -> Result<()> {
    let info = model.info()?;

    check_task(info.task.as_deref(), session, allowed)
}

/// Pure task-compatibility check shared by [`validate_session_task`]; split out
/// so the accept/reject logic and error message can be unit-tested without a
/// native model.
fn check_task(task: Option<&str>, session: &str, allowed: &[&str]) -> Result<()> {
    let task = task.unwrap_or("");

    if !allowed.contains(&task) {
        let expected = allowed
            .iter()
            .map(|t| format!("'{t}'"))
            .collect::<Vec<_>>()
            .join(" or ");

        return Err(FoundryLocalError::Validation {
            reason: format!("{session} requires a model with task {expected}, but got '{task}'."),
        });
    }

    Ok(())
}

/// A chat-oriented [`Session`] with tool registration and turn management.
///
/// Dereferences to [`Session`], so all base methods
/// ([`process_request`](Session::process_request),
/// [`process_streaming_request`](Session::process_streaming_request), …) are
/// available directly.
#[derive(Clone)]
pub struct ChatSession {
    session: Session,
}

impl ChatSession {
    /// Open a chat session on a loaded model.
    ///
    /// Returns [`FoundryLocalError::Validation`] if the model's task is not
    /// `"chat-completion"` or `"vision-language-chat"`.
    pub async fn new(model: &Model) -> Result<ChatSession> {
        validate_session_task(
            model,
            "ChatSession",
            &["chat-completion", "vision-language-chat"],
        )?;

        Ok(ChatSession {
            session: Session::new(model).await?,
        })
    }

    /// Register a [`ToolDefinition`] for the lifetime of the session.
    pub async fn add_tool_definition(&self, definition: ToolDefinition) -> Result<()> {
        let inner = Arc::clone(&self.session.inner);
        spawn_blocking(move || {
            inner.add_tool_definition(
                &definition.name,
                definition.description.as_deref(),
                &definition.json_schema,
            )
        })
        .await
    }

    /// Remove a previously-registered tool by name. Returns whether one was removed.
    pub async fn remove_tool_definition(&self, name: impl Into<String>) -> Result<bool> {
        let inner = Arc::clone(&self.session.inner);
        let name = name.into();
        spawn_blocking(move || inner.remove_tool_definition(&name)).await
    }

    /// The number of completed conversation turns.
    pub fn turn_count(&self) -> usize {
        self.session.inner.turn_count()
    }

    /// Rewind the last `count` turns, dropping their messages and replies.
    pub async fn undo_turns(&self, count: usize) -> Result<()> {
        let inner = Arc::clone(&self.session.inner);
        spawn_blocking(move || inner.undo_turns(count)).await
    }

    /// Consume this handle, yielding the underlying base [`Session`].
    pub fn into_session(self) -> Session {
        self.session
    }
}

impl Deref for ChatSession {
    type Target = Session;

    fn deref(&self) -> &Session {
        &self.session
    }
}

/// An embeddings-oriented [`Session`] producing dense vectors for text input.
///
/// Dereferences to [`Session`].
#[derive(Clone)]
pub struct EmbeddingsSession {
    session: Session,
}

impl EmbeddingsSession {
    /// Open an embeddings session on a loaded model.
    ///
    /// Returns [`FoundryLocalError::Validation`] if the model's task is not
    /// `"embeddings"`.
    pub async fn new(model: &Model) -> Result<EmbeddingsSession> {
        validate_session_task(model, "EmbeddingsSession", &["embeddings"])?;

        Ok(EmbeddingsSession {
            session: Session::new(model).await?,
        })
    }

    /// Embed a single text input, returning its dense vector.
    pub async fn embed(&self, input: impl Into<String>) -> Result<Vec<f32>> {
        let vectors = self.embed_batch(vec![input.into()]).await?;
        vectors
            .into_iter()
            .next()
            .ok_or_else(|| FoundryLocalError::Validation {
                reason: "embeddings response contained no vectors".to_string(),
            })
    }

    /// Embed a batch of text inputs, returning one dense vector per input (in
    /// order).
    pub async fn embed_batch(&self, inputs: Vec<String>) -> Result<Vec<Vec<f32>>> {
        let items: Vec<Item> = inputs.iter().map(|s| Item::text(s.as_str())).collect();
        let request = Request::from_items(items);
        let response = self.session.process_request(request).await?;

        if response.items.len() != inputs.len() {
            return Err(FoundryLocalError::Validation {
                reason: format!(
                    "embeddings response returned {} vectors for {} inputs",
                    response.items.len(),
                    inputs.len()
                ),
            });
        }

        let mut vectors = Vec::with_capacity(response.items.len());
        for item in &response.items {
            let tensor = item
                .as_tensor()
                .ok_or_else(|| FoundryLocalError::Validation {
                    reason: "embeddings response item was not a tensor".to_string(),
                })?;
            let floats = tensor
                .as_f32()
                .ok_or_else(|| FoundryLocalError::Validation {
                    reason: "embeddings tensor was not float data".to_string(),
                })?;
            vectors.push(floats);
        }
        Ok(vectors)
    }

    /// Consume this handle, yielding the underlying base [`Session`].
    pub fn into_session(self) -> Session {
        self.session
    }
}

impl Deref for EmbeddingsSession {
    type Target = Session;

    fn deref(&self) -> &Session {
        &self.session
    }
}

/// An audio-oriented [`Session`] for speech tasks (e.g. transcription).
///
/// Dereferences to [`Session`]. Submit [`Item::audio_data`](crate::Item::audio_data)
/// / [`Item::audio_uri`](crate::Item::audio_uri) input and read
/// [`Item::SpeechResult`](crate::Item::SpeechResult) output, or use
/// [`transcribe`](Self::transcribe) for the common case.
#[derive(Clone)]
pub struct AudioSession {
    session: Session,
}

impl AudioSession {
    /// Open an audio session on a loaded model.
    ///
    /// Returns [`FoundryLocalError::Validation`] if the model's task is not
    /// `"automatic-speech-recognition"`.
    pub async fn new(model: &Model) -> Result<AudioSession> {
        validate_session_task(model, "AudioSession", &["automatic-speech-recognition"])?;

        Ok(AudioSession {
            session: Session::new(model).await?,
        })
    }

    /// Transcribe a single audio item, returning the recognized text.
    ///
    /// A convenience over [`process_request`](Session::process_request): submits
    /// `audio` and concatenates the text of every returned speech result.
    pub async fn transcribe(&self, audio: Item) -> Result<String> {
        let response = self
            .session
            .process_request(Request::from_items(vec![audio]))
            .await?;
        let mut text = String::new();
        for item in &response.items {
            if let Some(result) = item.as_speech_result() {
                text.push_str(&result.text);
            } else if let Some(t) = item.as_text() {
                text.push_str(t);
            }
        }
        Ok(text)
    }

    /// Consume this handle, yielding the underlying base [`Session`].
    pub fn into_session(self) -> Session {
        self.session
    }
}

impl Deref for AudioSession {
    type Target = Session;

    fn deref(&self) -> &Session {
        &self.session
    }
}

#[cfg(test)]
mod tests {
    use std::future::Future;
    use std::sync::Arc;
    use std::task::{Wake, Waker};

    use super::*;

    struct NoopWake;

    impl Wake for NoopWake {
        fn wake(self: Arc<Self>) {}
    }

    #[tokio::test]
    async fn item_stream_returns_terminal_response_once() {
        let (tx, rx) = tokio::sync::mpsc::unbounded_channel();
        let (response_tx, response_rx) = oneshot::channel();
        let expected = Response {
            items: vec![Item::text("complete")],
            finish_reason: crate::FinishReason::Stop,
            usage: crate::Usage {
                prompt_tokens: 3,
                completion_tokens: 1,
                total_tokens: 4,
            },
        };
        tx.send(Ok(Item::text("chunk one"))).unwrap();
        tx.send(Ok(Item::text("chunk two"))).unwrap();
        drop(tx);
        response_tx.send(Ok(expected.clone())).unwrap();

        let mut stream = ItemStream {
            rx,
            response_rx: Some(response_rx),
        };

        assert_eq!(stream.response().await.unwrap(), expected);
        assert!(stream.rx.is_empty());
        assert!(matches!(
            stream.response().await,
            Err(FoundryLocalError::Validation { .. })
        ));
    }

    #[tokio::test]
    async fn cancelling_response_await_keeps_terminal_response_available() {
        let (_tx, rx) = tokio::sync::mpsc::unbounded_channel();
        let (response_tx, response_rx) = oneshot::channel();
        let mut stream = ItemStream {
            rx,
            response_rx: Some(response_rx),
        };

        let mut response = Box::pin(stream.response());
        let waker = Waker::from(Arc::new(NoopWake));
        let mut context = Context::from_waker(&waker);
        assert!(matches!(
            response.as_mut().poll(&mut context),
            Poll::Pending
        ));
        drop(response);

        response_tx
            .send(Ok(Response {
                items: Vec::new(),
                finish_reason: crate::FinishReason::Stop,
                usage: crate::Usage::default(),
            }))
            .unwrap();
        assert_eq!(
            stream.response().await.unwrap().finish_reason,
            crate::FinishReason::Stop
        );
    }

    #[test]
    fn check_task_accepts_allowed_tasks() {
        assert!(check_task(
            Some("chat-completion"),
            "ChatSession",
            &["chat-completion", "vision-language-chat"]
        )
        .is_ok());
        assert!(check_task(
            Some("vision-language-chat"),
            "ChatSession",
            &["chat-completion", "vision-language-chat"]
        )
        .is_ok());
        assert!(check_task(Some("embeddings"), "EmbeddingsSession", &["embeddings"]).is_ok());
    }

    #[test]
    fn check_task_rejects_wrong_task() {
        let err = check_task(
            Some("chat-completion"),
            "EmbeddingsSession",
            &["embeddings"],
        )
        .expect_err("wrong task should be rejected");

        match err {
            FoundryLocalError::Validation { reason } => {
                assert!(reason.contains("EmbeddingsSession"), "reason: {reason}");
                assert!(reason.contains("'embeddings'"), "reason: {reason}");
                assert!(reason.contains("'chat-completion'"), "reason: {reason}");
            }
            other => panic!("expected Validation error, got: {other:?}"),
        }
    }

    #[test]
    fn check_task_treats_missing_task_as_mismatch() {
        let err = check_task(None, "AudioSession", &["automatic-speech-recognition"])
            .expect_err("missing task should be rejected");

        assert!(matches!(err, FoundryLocalError::Validation { .. }));
    }
}
