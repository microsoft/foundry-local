//! Live audio transcription streaming session.
//!
//! Provides real-time audio streaming ASR (Automatic Speech Recognition).
//! Audio data from a microphone (or other source) is pushed in as PCM chunks
//! and transcription results are returned as an async [`Stream`](futures_core::Stream).
//!
//! # Example
//!
//! ```ignore
//! let audio_client = model.create_audio_client();
//! let mut session = audio_client.create_live_transcription_session();
//! session.settings.sample_rate = 16000;
//! session.settings.channels = 1;
//! session.settings.language = Some("en".into());
//!
//! session.start(None).await?;
//!
//! // Push audio from microphone callback
//! session.append(&pcm_bytes, None).await?;
//!
//! // Read results as async stream
//! use tokio_stream::StreamExt;
//! let mut stream = session.get_stream().await?;
//! while let Some(result) = stream.next().await {
//!     let result = result?;
//!     print!("{}", result.content[0].text);
//! }
//!
//! session.stop(None).await?;
//! ```
#![allow(deprecated)] // this module implements the deprecated OpenAI facade

use std::os::raw::c_int;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::pin::Pin;
use std::sync::Arc;
use std::task::{Context, Poll};

use tokio::sync::mpsc::{UnboundedReceiver, UnboundedSender};
use tokio_util::sync::CancellationToken;

use crate::detail::api::Api;
use crate::detail::ffi::{flItem, flStreamingCallbackData, FOUNDRY_LOCAL_ITEM_BYTES};
use crate::detail::items::{
    make_audio_item, read_speech_segment, read_text_item, SpeechSegmentText,
};
use crate::detail::native::NativeModel;
use crate::detail::session::{NativeItemQueue, NativeRequest, NativeSession};
use crate::detail::task::spawn_blocking;
use crate::error::{FoundryLocalError, Result};

// ── Types ────────────────────────────────────────────────────────────────────

/// Audio format settings for a live transcription session.
///
/// Must be configured before calling [`LiveAudioTranscriptionSession::start`].
/// Settings are frozen once the session starts.
#[derive(Debug, Clone)]
pub struct LiveAudioTranscriptionOptions {
    /// PCM sample rate in Hz. Default: 16000.
    pub sample_rate: u32,
    /// Number of audio channels. Default: 1 (mono).
    pub channels: u32,
    /// Optional BCP-47 language hint (e.g., `"en"`, `"zh"`).
    pub language: Option<String>,
}

impl Default for LiveAudioTranscriptionOptions {
    fn default() -> Self {
        Self {
            sample_rate: 16000,
            channels: 1,
            language: None,
        }
    }
}

/// Internal raw deserialization target matching the native core's JSON format.
#[derive(Debug, Clone, serde::Deserialize)]
struct LiveAudioTranscriptionRaw {
    #[serde(default)]
    is_final: bool,
    #[serde(default)]
    text: String,
    start_time: Option<f64>,
    end_time: Option<f64>,
    id: Option<String>,
}

/// A content part within a [`LiveAudioTranscriptionResponse`].
///
/// Mirrors the C# `ContentPart` shape from the OpenAI Realtime API so that
/// callers can access `result.content[0].text` or `result.content[0].transcript`
/// consistently across SDKs.
#[derive(Debug, Clone, serde::Serialize, serde::Deserialize)]
pub struct ContentPart {
    /// The transcribed text.
    pub text: String,
    /// Same as `text` — provided for OpenAI Realtime API compatibility.
    pub transcript: String,
}

/// Transcription result from a live audio streaming session.
///
/// Shaped to match the C# `LiveAudioTranscriptionResponse : ConversationItem`
/// so that callers access text via `result.content[0].text` or
/// `result.content[0].transcript`.
#[derive(Debug, Clone, serde::Serialize, serde::Deserialize)]
pub struct LiveAudioTranscriptionResponse {
    /// Content parts — typically a single element. Access text via
    /// `result.content[0].text` or `result.content[0].transcript`.
    pub content: Vec<ContentPart>,
    /// Whether this is a final or partial (interim) result.
    /// Nemotron models always return `true`; other models may return `false`
    /// for interim hypotheses that will be replaced by a subsequent final result.
    pub is_final: bool,
    /// Start time offset of this segment in the audio stream (seconds).
    pub start_time: Option<f64>,
    /// End time offset of this segment in the audio stream (seconds).
    pub end_time: Option<f64>,
    /// Unique identifier for this result (if available).
    pub id: Option<String>,
}

impl LiveAudioTranscriptionResponse {
    /// Parse a transcription response from the native core's JSON format.
    pub fn from_json(json: &str) -> Result<Self> {
        serde_json::from_str::<LiveAudioTranscriptionRaw>(json)
            .map(Self::from_raw)
            .map_err(FoundryLocalError::from)
    }

    fn from_raw(raw: LiveAudioTranscriptionRaw) -> Self {
        Self {
            content: vec![ContentPart {
                transcript: raw.text.clone(),
                text: raw.text,
            }],
            is_final: raw.is_final,
            start_time: raw.start_time,
            end_time: raw.end_time,
            id: raw.id,
        }
    }

    /// Build a response from a plain transcript string.
    fn from_text(text: String, is_final: bool) -> Self {
        Self {
            content: vec![ContentPart {
                transcript: text.clone(),
                text,
            }],
            is_final,
            start_time: None,
            end_time: None,
            id: None,
        }
    }

    /// Build a response from a SPEECH_SEGMENT item's content.
    fn from_segment(seg: SpeechSegmentText) -> Self {
        Self {
            content: vec![ContentPart {
                transcript: seg.text.clone(),
                text: seg.text,
            }],
            is_final: seg.is_final,
            start_time: seg.start_time_s,
            end_time: seg.end_time_s,
            id: None,
        }
    }
}

/// Structured error response from the native core.
#[derive(Debug, Clone, serde::Deserialize)]
pub struct CoreErrorResponse {
    /// Error code (e.g. `"ASR_SESSION_NOT_FOUND"`).
    pub code: String,
    /// Human-readable error message.
    pub message: String,
    /// Whether this error is transient (retryable).
    #[serde(rename = "isTransient", default)]
    pub is_transient: bool,
}

impl CoreErrorResponse {
    /// Attempt to parse a native error string as structured JSON.
    /// Returns `None` if the error is not valid JSON or doesn't match the schema.
    pub fn try_parse(error_string: &str) -> Option<Self> {
        serde_json::from_str(error_string).ok()
    }
}

// ── Stream type ──────────────────────────────────────────────────────────────

/// An async stream of [`LiveAudioTranscriptionResponse`] items.
///
/// Returned by [`LiveAudioTranscriptionSession::get_stream`].
/// Implements [`futures_core::Stream`].
pub struct LiveAudioTranscriptionStream {
    rx: UnboundedReceiver<Result<LiveAudioTranscriptionResponse>>,
}

impl futures_core::Stream for LiveAudioTranscriptionStream {
    type Item = Result<LiveAudioTranscriptionResponse>;

    fn poll_next(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Option<Self::Item>> {
        self.rx.poll_recv(cx)
    }
}

// ── Session state ────────────────────────────────────────────────────────────

#[derive(Default)]
struct SessionState {
    started: bool,
    stopped: bool,
    queue: Option<Arc<NativeItemQueue>>,
    output_rx: Option<UnboundedReceiver<Result<LiveAudioTranscriptionResponse>>>,
    worker: Option<tokio::task::JoinHandle<()>>,
}

// ── Session ──────────────────────────────────────────────────────────────────

/// Session for real-time audio streaming ASR (Automatic Speech Recognition).
///
/// Audio data from a microphone (or other source) is pushed in as PCM chunks
/// via [`append`](Self::append), and transcription results are returned as an
/// async [`Stream`](futures_core::Stream) via [`get_stream`](Self::get_stream).
///
/// Created via [`AudioClient::create_live_transcription_session`](super::AudioClient::create_live_transcription_session).
///
/// # Cancellation
///
/// All lifecycle methods accept an optional [`CancellationToken`]. Pass `None`
/// to use the default (no cancellation).
#[deprecated(
    since = "2.0.0",
    note = "The OpenAI direct clients are deprecated; use the Session API instead \
            (`AudioSession::new(&model)` with streaming)."
)]
pub struct LiveAudioTranscriptionSession {
    model: NativeModel,
    /// Audio format settings. Must be configured before calling [`start`](Self::start).
    /// Settings are frozen once the session starts.
    pub settings: LiveAudioTranscriptionOptions,
    state: tokio::sync::Mutex<SessionState>,
}

impl LiveAudioTranscriptionSession {
    pub(crate) fn new(_model_id: &str, model: NativeModel) -> Self {
        Self {
            model,
            settings: LiveAudioTranscriptionOptions::default(),
            state: tokio::sync::Mutex::new(SessionState::default()),
        }
    }

    /// Start a real-time audio streaming session.
    ///
    /// Must be called before [`append`](Self::append) or
    /// [`get_stream`](Self::get_stream). Settings are frozen after this call.
    pub async fn start(&self, ct: Option<CancellationToken>) -> Result<()> {
        let mut state = self.state.lock().await;

        if state.started {
            return Err(FoundryLocalError::Validation {
                reason: "Streaming session already started. Call stop() first.".into(),
            });
        }

        if let Some(token) = &ct {
            if token.is_cancelled() {
                return Err(FoundryLocalError::CommandExecution {
                    reason: "Start cancelled".into(),
                });
            }
        }

        let settings = self.settings.clone();
        let model = self.model.clone();
        let api = Arc::clone(&model.api);

        // Create the shared input queue up front so `append` can push into it.
        let queue = Arc::new(NativeItemQueue::new(api)?);

        let (output_tx, output_rx) =
            tokio::sync::mpsc::unbounded_channel::<Result<LiveAudioTranscriptionResponse>>();

        let worker_queue = Arc::clone(&queue);
        let worker = tokio::task::spawn_blocking(move || {
            run_worker(model, settings, worker_queue, output_tx);
        });

        state.started = true;
        state.stopped = false;
        state.queue = Some(queue);
        state.output_rx = Some(output_rx);
        state.worker = Some(worker);

        Ok(())
    }

    /// Push a chunk of raw PCM audio data to the streaming session.
    ///
    /// The data is copied internally so the caller can reuse the buffer.
    pub async fn append(&self, pcm_data: &[u8], ct: Option<CancellationToken>) -> Result<()> {
        if let Some(token) = &ct {
            if token.is_cancelled() {
                return Err(FoundryLocalError::CommandExecution {
                    reason: "Append cancelled".into(),
                });
            }
        }

        let queue = {
            let state = self.state.lock().await;
            if !state.started || state.stopped {
                return Err(FoundryLocalError::Validation {
                    reason: "No active streaming session. Call start() first.".into(),
                });
            }
            state
                .queue
                .clone()
                .ok_or_else(|| FoundryLocalError::Internal {
                    reason: "Input queue not available — session may be in an invalid state".into(),
                })?
        };

        let data = pcm_data.to_vec();
        spawn_blocking(move || queue.push_bytes(&data, FOUNDRY_LOCAL_ITEM_BYTES)).await
    }

    /// Get the async stream of transcription results.
    ///
    /// Results arrive as the native ASR engine processes audio data.
    /// Can only be called once per session (the receiver is moved out).
    pub async fn get_stream(&self) -> Result<LiveAudioTranscriptionStream> {
        let mut state = self.state.lock().await;
        let rx = state
            .output_rx
            .take()
            .ok_or_else(|| FoundryLocalError::Validation {
                reason: "No active streaming session, or stream already taken. \
                         Call start() first and only call get_stream() once."
                    .into(),
            })?;
        Ok(LiveAudioTranscriptionStream { rx })
    }

    /// Signal end-of-audio and stop the streaming session.
    ///
    /// Any remaining buffered audio is drained to the native engine first;
    /// final results are delivered through the transcription stream before it
    /// completes. The native stop always completes to avoid session leaks,
    /// even if the provided [`CancellationToken`] fires.
    pub async fn stop(&self, _ct: Option<CancellationToken>) -> Result<()> {
        let worker = {
            let mut state = self.state.lock().await;
            if !state.started || state.stopped {
                return Ok(());
            }
            state.stopped = true;
            if let Some(queue) = &state.queue {
                queue.mark_finished();
            }
            state.worker.take()
        };

        if let Some(handle) = worker {
            let _ = handle.await;
        }
        Ok(())
    }
}

impl Drop for LiveAudioTranscriptionSession {
    /// Best-effort cleanup if the session is dropped without calling
    /// [`stop`](Self::stop): mark the input queue finished so the blocking
    /// background worker drains and returns (releasing the native session),
    /// rather than leaking the worker thread. We hold `&mut self`, so the inner
    /// state is reachable synchronously via `get_mut` without locking.
    fn drop(&mut self) {
        let state = self.state.get_mut();
        if state.started && !state.stopped {
            state.stopped = true;
            if let Some(queue) = &state.queue {
                queue.mark_finished();
            }
            // Detach the worker: marking the queue finished lets its blocking
            // ProcessRequest return on its own, so we don't need to (and can't)
            // await it here.
            state.worker.take();
        }
    }
}

/// Streaming-callback context: forwards interim transcripts to the output channel.
struct LiveCtx {
    api: Arc<Api>,
    tx: UnboundedSender<Result<LiveAudioTranscriptionResponse>>,
}

unsafe extern "C" fn live_trampoline(
    data: flStreamingCallbackData,
    user_data: *mut std::ffi::c_void,
) -> c_int {
    if user_data.is_null() {
        return 0;
    }
    let result = catch_unwind(AssertUnwindSafe(|| {
        let ctx = &*(user_data as *const LiveCtx);
        let queue = data.item_queue;
        if queue.is_null() {
            return 0;
        }
        let item_api = ctx.api.item_api();
        loop {
            let mut item: *mut flItem = std::ptr::null_mut();
            if !(item_api.ItemQueue_TryPop)(queue, &mut item) {
                break;
            }
            if item.is_null() {
                continue;
            }
            // The native audio path now streams SPEECH_SEGMENT items; older cores
            // (and the OpenAI-JSON path) stream plain TEXT items. Handle both.
            let response = if let Some(text) = read_text_item(&ctx.api, item) {
                (!text.is_empty()).then(|| LiveAudioTranscriptionResponse::from_text(text, false))
            } else {
                read_speech_segment(&ctx.api, item)
                    .filter(|seg| !seg.text.is_empty())
                    .map(LiveAudioTranscriptionResponse::from_segment)
            };
            (item_api.Item_Release)(item);
            if let Some(response) = response {
                if ctx.tx.send(Ok(response)).is_err() {
                    return 1; // receiver dropped — cancel
                }
            }
        }
        0
    }));
    result.unwrap_or(1)
}

/// Blocking worker: builds the session/request, installs the streaming callback,
/// processes the audio queue to completion, then emits the final transcript.
fn run_worker(
    model: NativeModel,
    settings: LiveAudioTranscriptionOptions,
    queue: Arc<NativeItemQueue>,
    output_tx: UnboundedSender<Result<LiveAudioTranscriptionResponse>>,
) {
    let api = Arc::clone(&model.api);

    let run = (|| -> Result<()> {
        let session = NativeSession::create(&model)?;

        // Serialise this session's install→process→uninstall critical section.
        // The session is owned per-worker (un-shared), so the guard is
        // uncontended, but taking it keeps the "every native session op runs
        // under op_lock" invariant uniform across all streaming paths.
        let guard = session.lock_ops();

        let mut ctx = Box::new(LiveCtx {
            api: Arc::clone(&api),
            tx: output_tx.clone(),
        });
        let ctx_ptr = &mut *ctx as *mut LiveCtx as *mut std::ffi::c_void;
        session.set_streaming_callback(Some(live_trampoline), ctx_ptr)?;

        let request = NativeRequest::new(Arc::clone(&api))?;
        let format = make_audio_item(
            &api,
            &[],
            Some("pcm"),
            settings.sample_rate as i32,
            settings.channels as i32,
        )?;
        request.add_item(format, true)?;
        // The input queue stays owned by us (append pushes into it).
        request.add_item(queue.as_item_ptr(), false)?;

        let response = session.process_request(&request);

        // Always uninstall the streaming callback before `ctx` can be dropped —
        // on the error path too — so the native session never retains a dangling
        // `user_data` pointer into the freed context.
        let _ = session.set_streaming_callback(None, std::ptr::null_mut());
        drop(guard);
        let response = response?;

        // Aggregate the terminal transcript from the final response items. The
        // native audio path returns a SPEECH_RESULT item; the OpenAI-JSON path
        // (and older cores) return TEXT items.
        let mut final_text = String::new();
        for i in 0..response.item_count() {
            if let Some(text) = response
                .item_text(i)
                .or_else(|| response.item_speech_result_text(i))
            {
                final_text.push_str(&text);
            }
        }

        drop(ctx);

        if !final_text.is_empty() {
            let _ = output_tx.send(Ok(LiveAudioTranscriptionResponse::from_text(
                final_text, true,
            )));
        }
        Ok(())
    })();

    if let Err(e) = run {
        let _ = output_tx.send(Err(e));
    }
    // `queue` Arc clone and `session`/`request` drop here.
    drop(queue);
}
