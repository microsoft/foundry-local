//! Safe wrappers over native `flRequest`, `flResponse`, and `flSession`, plus the
//! OpenAI-JSON request/response and streaming bridges used by the OpenAI facade.

use std::os::raw::c_int;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::ptr;
use std::sync::Arc;

use tokio::sync::{
    mpsc::{UnboundedReceiver, UnboundedSender},
    oneshot,
};

use super::api::{Api, Kvps};
use super::ffi::*;
use super::items::{
    item_from_native, item_to_native, make_bytes_item, make_openai_json_item,
    read_speech_result_text, read_text_item,
};
use super::manager::NativeManager;
use super::native::NativeModel;
use crate::error::{FoundryLocalError, Result};
use crate::item::Item;

/// Per-item transform applied to streamed TEXT payloads before they are emitted.
pub(crate) type StreamTransform = Box<dyn Fn(String) -> Option<String> + Send>;

// ── Request ──────────────────────────────────────────────────────────────────

pub(crate) struct NativeRequest {
    api: Arc<Api>,
    ptr: *mut flRequest,
}

// SAFETY: the raw `flRequest` pointer is only *mutated* (add_item / set_options /
// process) from the single blocking worker that owns the request. The one method
// callable from another thread is `cancel`, which the native layer implements as
// an atomic-flag store (`Request_Cancel`), so concurrent cancel-vs-process is
// well-defined. This mirrors the Send+Sync story of `NativeSession`.
unsafe impl Send for NativeRequest {}
unsafe impl Sync for NativeRequest {}

impl NativeRequest {
    pub(crate) fn new(api: Arc<Api>) -> Result<Self> {
        let mut ptr: *mut flRequest = ptr::null_mut();
        api.check(unsafe { (api.inference_api().Request_Create)(&mut ptr) })?;
        Ok(Self { api, ptr })
    }

    /// Add an item, transferring ownership to the request.
    pub(crate) fn add_item(&self, item: *mut flItem, take_ownership: bool) -> Result<()> {
        let status =
            unsafe { (self.api.inference_api().Request_AddItem)(self.ptr, item, take_ownership) };
        self.api.check(status)
    }

    /// Build a native item from a public [`Item`] and add it, transferring
    /// ownership to the request. Releases the transient item if the add fails.
    pub(crate) fn add_item_value(&self, item: &Item) -> Result<()> {
        let native = item_to_native(&self.api, item)?;
        if let Err(e) = self.add_item(native, true) {
            // The add did not take ownership on failure — reclaim to avoid a leak.
            unsafe { (self.api.item_api().Item_Release)(native) };
            return Err(e);
        }
        Ok(())
    }

    /// Add a streaming input queue to the request as a *borrowed* item: the queue
    /// remains owned by its [`NativeItemQueue`] and must outlive processing.
    pub(crate) fn add_input_queue(&self, queue: &NativeItemQueue) -> Result<()> {
        self.add_item(queue.as_item_ptr(), false)
    }

    /// Apply request-scoped options from a native key/value collection.
    pub(crate) fn set_options(&self, options: *const flKeyValuePairs) -> Result<()> {
        let status = unsafe { (self.api.inference_api().Request_SetOptions)(self.ptr, options) };
        self.api.check(status)
    }

    /// Signal cancellation of an in-flight request.
    ///
    /// The native layer records this as an atomic flag (`Request_Cancel`), so it
    /// is safe to call from a thread other than the one running
    /// [`NativeSession::process_request`]; in-progress generation stops as soon
    /// as possible. Best-effort: any status is released and the error ignored,
    /// which is appropriate for the drop-cancel path.
    pub(crate) fn cancel(&self) {
        let status = unsafe { (self.api.inference_api().Request_Cancel)(self.ptr) };
        let _ = self.api.check(status);
    }
}

impl Drop for NativeRequest {
    fn drop(&mut self) {
        if !self.ptr.is_null() {
            unsafe { (self.api.inference_api().Request_Release)(self.ptr) };
            self.ptr = ptr::null_mut();
        }
    }
}

// ── Response ─────────────────────────────────────────────────────────────────

pub(crate) struct NativeResponse {
    api: Arc<Api>,
    ptr: *mut flResponse,
}

impl NativeResponse {
    pub(crate) fn item_count(&self) -> usize {
        unsafe { (self.api.inference_api().Response_GetItemCount)(self.ptr) }
    }

    /// Read the text payload of the response item at `idx`.
    ///
    /// Returns `Ok(None)` if the item is not a TEXT item, and `Err` if fetching
    /// the item or reading its text fails, so conversion failures propagate.
    pub(crate) fn item_text(&self, idx: usize) -> Result<Option<String>> {
        let mut item: *const flItem = ptr::null();
        let status =
            unsafe { (self.api.inference_api().Response_GetItem)(self.ptr, idx, &mut item) };
        self.api.check(status)?;
        unsafe { read_text_item(&self.api, item) }
    }

    /// Read the transcript of the response item at `idx`.
    ///
    /// Returns `Ok(None)` if the item is not a SPEECH_RESULT item, and `Err` if
    /// fetching the item or reading it fails, so conversion failures propagate.
    pub(crate) fn item_speech_result_text(&self, idx: usize) -> Result<Option<String>> {
        let mut item: *const flItem = ptr::null();
        let status =
            unsafe { (self.api.inference_api().Response_GetItem)(self.ptr, idx, &mut item) };
        self.api.check(status)?;
        unsafe { read_speech_result_text(&self.api, item) }
    }

    /// Read the response item at `idx` into an owned [`Item`].
    pub(crate) fn item(&self, idx: usize) -> Result<Item> {
        let mut item: *const flItem = ptr::null();
        let status =
            unsafe { (self.api.inference_api().Response_GetItem)(self.ptr, idx, &mut item) };
        self.api.check(status)?;
        unsafe { item_from_native(&self.api, item) }
    }

    /// Collect all response items into owned [`Item`]s.
    pub(crate) fn items(&self) -> Result<Vec<Item>> {
        let count = self.item_count();
        let mut out = Vec::with_capacity(count);
        for i in 0..count {
            out.push(self.item(i)?);
        }
        Ok(out)
    }

    /// The native finish-reason discriminant for the response.
    pub(crate) fn finish_reason(&self) -> flFinishReason {
        unsafe { (self.api.inference_api().Response_GetFinishReason)(self.ptr) }
    }

    /// Token usage as `(prompt, completion, total)`.
    pub(crate) fn usage(&self) -> Result<(i64, i64, i64)> {
        let mut usage = flUsage {
            version: FOUNDRY_LOCAL_API_VERSION,
            prompt_tokens: 0,
            completion_tokens: 0,
            total_tokens: 0,
        };
        let status = unsafe { (self.api.inference_api().Response_GetUsage)(self.ptr, &mut usage) };
        self.api.check(status)?;
        Ok((
            usage.prompt_tokens,
            usage.completion_tokens,
            usage.total_tokens,
        ))
    }
}

impl Drop for NativeResponse {
    fn drop(&mut self) {
        if !self.ptr.is_null() {
            unsafe { (self.api.inference_api().Response_Release)(self.ptr) };
            self.ptr = ptr::null_mut();
        }
    }
}

// ── ItemQueue ────────────────────────────────────────────────────────────────

/// Owning wrapper around a native input `flItemQueue`.
///
/// In the C ABI an `ItemQueue` *is* an `Item` (same pointer, castable), so it can
/// be added to a request directly and released via `Item_Release`.
pub(crate) struct NativeItemQueue {
    api: Arc<Api>,
    ptr: *mut flItemQueue,
}

// SAFETY: the native item queue is documented as thread-safe (multi-producer /
// multi-consumer); pushing from any thread is supported.
unsafe impl Send for NativeItemQueue {}
unsafe impl Sync for NativeItemQueue {}

impl NativeItemQueue {
    pub(crate) fn new(api: Arc<Api>) -> Result<Self> {
        let mut ptr: *mut flItemQueue = ptr::null_mut();
        api.check(unsafe { (api.item_api().ItemQueue_Create)(&mut ptr) })?;
        Ok(Self { api, ptr })
    }

    /// The queue as an `flItem*` (for adding to a request).
    pub(crate) fn as_item_ptr(&self) -> *mut flItem {
        self.ptr as *mut flItem
    }

    /// Push an item, transferring ownership into the queue.
    ///
    /// `ItemQueue_Push` takes ownership of `item` *unconditionally* for a
    /// non-null queue and item: the native side moves the raw pointer into a
    /// `unique_ptr` before enqueuing, so even if enqueuing fails the item is
    /// already (or will be) freed. Callers must therefore **not** release `item`
    /// on a returned error — doing so would double-free.
    pub(crate) fn push_item(&self, item: *mut flItem) -> Result<()> {
        self.api
            .check(unsafe { (self.api.item_api().ItemQueue_Push)(self.ptr, item) })
    }

    /// Create a BYTES item from `data` and push it into the queue.
    pub(crate) fn push_bytes(&self, data: &[u8], item_type: flItemType) -> Result<()> {
        let item = make_bytes_item(&self.api, data, item_type)?;
        // `push_item` consumes `item` on every path (see its docs); do not
        // release it here on error.
        self.push_item(item)
    }

    /// Build a native item from a public [`Item`] and push it into the queue.
    pub(crate) fn push_value(&self, item: &Item) -> Result<()> {
        let native = item_to_native(&self.api, item)?;
        // `push_item` consumes `native` on every path (see its docs).
        self.push_item(native)
    }

    /// Pop the next item, if any, decoding it into an owned [`Item`].
    ///
    /// Returns `None` when the queue is currently empty. Ownership of the popped
    /// native item transfers to us, so it is released after decoding.
    pub(crate) fn try_pop_value(&self) -> Result<Option<Item>> {
        let mut item: *mut flItem = ptr::null_mut();
        let popped = unsafe { (self.api.item_api().ItemQueue_TryPop)(self.ptr, &mut item) };
        if !popped || item.is_null() {
            return Ok(None);
        }
        let decoded = unsafe { item_from_native(&self.api, item) };
        unsafe { (self.api.item_api().Item_Release)(item) };
        decoded.map(Some)
    }

    /// The number of items currently buffered in the queue.
    pub(crate) fn size(&self) -> usize {
        unsafe { (self.api.item_api().ItemQueue_Size)(self.ptr) }
    }

    /// Whether the queue has been marked finished (no further pushes expected).
    pub(crate) fn is_finished(&self) -> bool {
        unsafe { (self.api.item_api().ItemQueue_IsFinished)(self.ptr) }
    }

    /// Signal that no more items will be pushed.
    pub(crate) fn mark_finished(&self) {
        unsafe { (self.api.item_api().ItemQueue_MarkFinished)(self.ptr) };
    }
}

impl Drop for NativeItemQueue {
    fn drop(&mut self) {
        if !self.ptr.is_null() {
            // The queue is an Item; release via the polymorphic Item destructor.
            unsafe { (self.api.item_api().Item_Release)(self.ptr as *mut flItem) };
            self.ptr = ptr::null_mut();
        }
    }
}

// ── Session ──────────────────────────────────────────────────────────────────

pub(crate) struct NativeSession {
    pub(crate) api: Arc<Api>,
    ptr: *mut flSession,
    /// Serialises native operations on this session. The public
    /// [`Session`](crate::Session) / [`ChatSession`](crate::ChatSession) API
    /// wraps a session in an `Arc` and hands out clones, so several tasks may
    /// touch the same native session concurrently. The native layer is not
    /// internally synchronised for concurrent use, so every operation that
    /// mutates or drives the session takes this lock first — except the raw
    /// [`process_request`](Self::process_request), which the streaming paths
    /// invoke while already holding the lock across install→process→uninstall.
    op_lock: std::sync::Mutex<()>,
    /// Keeps the native manager (which owns the model this session was created
    /// from) alive for the session's lifetime; never dereferenced here.
    _manager: Arc<NativeManager>,
}

// SAFETY: a session is used from a single worker at a time; the native layer is
// thread-safe for the create/process/release lifecycle used here. The owning
// native manager is kept alive via `_manager`.
unsafe impl Send for NativeSession {}
unsafe impl Sync for NativeSession {}

impl NativeSession {
    /// Create a session bound to the given model variant.
    pub(crate) fn create(model: &NativeModel) -> Result<Self> {
        let api = Arc::clone(&model.api);
        let mut ptr: *mut flSession = ptr::null_mut();
        api.check(unsafe { (api.inference_api().Session_Create)(model.ptr, &mut ptr) })?;
        Ok(Self {
            api,
            ptr,
            op_lock: std::sync::Mutex::new(()),
            _manager: model.manager(),
        })
    }

    /// Acquire the per-session operation lock, serialising native calls that
    /// touch this session. Poison-resilient: a panic while another caller holds
    /// the guard does not wedge the session, because the guard protects a `()`
    /// and the native state it orders is unaffected by Rust-side unwinding.
    pub(crate) fn lock_ops(&self) -> std::sync::MutexGuard<'_, ()> {
        self.op_lock
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
    }

    pub(crate) fn set_streaming_callback(
        &self,
        callback: flStreamingCallback,
        user_data: *mut std::ffi::c_void,
    ) -> Result<()> {
        let status = unsafe {
            (self.api.inference_api().Session_SetStreamingCallback)(self.ptr, callback, user_data)
        };
        self.api.check(status)
    }

    /// Apply session-scoped options from a native key/value collection.
    pub(crate) fn set_options(&self, options: *const flKeyValuePairs) -> Result<()> {
        let _guard = self.lock_ops();
        let status = unsafe { (self.api.inference_api().Session_SetOptions)(self.ptr, options) };
        self.api.check(status)
    }

    /// Register a tool definition for the lifetime of the session.
    pub(crate) fn add_tool_definition(
        &self,
        name: &str,
        description: Option<&str>,
        json_schema: &str,
    ) -> Result<()> {
        let _guard = self.lock_ops();
        let name_c = super::api::to_cstring(name)?;
        // The C ABI rejects a null description (INVALID_ARGUMENT), so send an
        // empty C string when the caller did not supply one.
        let desc_c = super::api::to_cstring(description.unwrap_or(""))?;
        let schema_c = super::api::to_cstring(json_schema)?;
        let def = flToolDefinition {
            version: FOUNDRY_LOCAL_API_VERSION,
            name: name_c.as_ptr(),
            description: desc_c.as_ptr(),
            json_schema: schema_c.as_ptr(),
        };
        let status =
            unsafe { (self.api.inference_api().Session_AddToolDefinition)(self.ptr, &def) };
        self.api.check(status)
    }

    /// Remove a previously-registered tool by name. Returns whether one was removed.
    pub(crate) fn remove_tool_definition(&self, name: &str) -> Result<bool> {
        let _guard = self.lock_ops();
        let name_c = super::api::to_cstring(name)?;
        let mut removed = false;
        let status = unsafe {
            (self.api.inference_api().Session_RemoveToolDefinition)(
                self.ptr,
                name_c.as_ptr(),
                &mut removed,
            )
        };
        self.api.check(status)?;
        Ok(removed)
    }

    /// The number of completed turns.
    pub(crate) fn turn_count(&self) -> usize {
        let _guard = self.lock_ops();
        unsafe { (self.api.inference_api().Session_GetTurnCount)(self.ptr) }
    }

    /// Rewind the last `count` turns, dropping their messages and replies.
    pub(crate) fn undo_turns(&self, count: usize) -> Result<()> {
        let _guard = self.lock_ops();
        let status = unsafe { (self.api.inference_api().Session_UndoTurns)(self.ptr, count) };
        self.api.check(status)
    }

    pub(crate) fn process_request(&self, request: &NativeRequest) -> Result<NativeResponse> {
        let mut resp: *mut flResponse = ptr::null_mut();
        let status = unsafe {
            (self.api.inference_api().Session_ProcessRequest)(self.ptr, request.ptr, &mut resp)
        };
        self.api.check(status)?;
        Ok(NativeResponse {
            api: Arc::clone(&self.api),
            ptr: resp,
        })
    }

    /// Run a non-streaming OpenAI-JSON request and return the response payload.
    ///
    /// The request JSON is sent as a single `OPENAI_JSON` TEXT item; the response
    /// payload is the text of the first response item. Blocking.
    pub(crate) fn run_openai_json(&self, request_json: &str) -> Result<String> {
        let _guard = self.lock_ops();
        let request = NativeRequest::new(Arc::clone(&self.api))?;
        let item = make_openai_json_item(&self.api, request_json)?;
        request.add_item(item, true)?;
        let response = self.process_request(&request)?;
        if response.item_count() == 0 {
            return Err(FoundryLocalError::CommandExecution {
                reason: "Native response contained no items".into(),
            });
        }
        response
            .item_text(0)?
            .ok_or_else(|| FoundryLocalError::CommandExecution {
                reason: "Native response item was not readable text".into(),
            })
    }
}

impl Drop for NativeSession {
    fn drop(&mut self) {
        if !self.ptr.is_null() {
            unsafe { (self.api.inference_api().Session_Release)(self.ptr) };
            self.ptr = ptr::null_mut();
        }
    }
}

// ── Streaming bridge ─────────────────────────────────────────────────────────

struct StreamCtx {
    api: Arc<Api>,
    tx: UnboundedSender<Result<String>>,
    transform: StreamTransform,
}

unsafe extern "C" fn stream_trampoline(
    data: flStreamingCallbackData,
    user_data: *mut std::ffi::c_void,
) -> c_int {
    if user_data.is_null() {
        return 0;
    }
    let result = catch_unwind(AssertUnwindSafe(|| {
        let ctx = &*(user_data as *const StreamCtx);
        let queue = data.item_queue;
        if queue.is_null() {
            return 0;
        }
        let item_api = ctx.api.item_api();
        loop {
            let mut item: *mut flItem = ptr::null_mut();
            let popped = (item_api.ItemQueue_TryPop)(queue, &mut item);
            if !popped {
                break;
            }
            if item.is_null() {
                continue;
            }
            // Ownership of `item` transferred to us — read then release.
            let text = read_text_item(&ctx.api, item);
            (item_api.Item_Release)(item);

            let text = match text {
                Ok(text) => text,
                Err(error) => {
                    let _ = ctx.tx.send(Err(error));
                    return 1; // read failure — stop generation and surface the error
                }
            };

            if let Some(text) = text {
                if let Some(transformed) = (ctx.transform)(text) {
                    if ctx.tx.send(Ok(transformed)).is_err() {
                        return 1; // receiver dropped — cancel generation
                    }
                }
            }
        }
        0
    }));
    result.unwrap_or(1)
}

/// Run a streaming OpenAI-JSON request, returning a channel of transformed
/// per-item TEXT payloads.
///
/// `transform` is applied to each streamed item's text (return `None` to skip
/// an item). The session is created and processed on a blocking worker thread;
/// the channel closes when generation completes or errors.
pub(crate) fn run_openai_json_streaming(
    session: NativeSession,
    request_json: String,
    transform: StreamTransform,
) -> UnboundedReceiver<Result<String>> {
    let (tx, rx) = tokio::sync::mpsc::unbounded_channel::<Result<String>>();

    tokio::task::spawn_blocking(move || {
        let ctx = Box::new(StreamCtx {
            api: Arc::clone(&session.api),
            tx: tx.clone(),
            transform,
        });
        let ctx_ptr = &*ctx as *const StreamCtx as *mut std::ffi::c_void;

        // Serialise the whole install→process→uninstall critical section: the
        // session may be shared via the public API, and the native streaming
        // callback slot is per-session state the native layer does not lock.
        let guard = session.lock_ops();

        if let Err(e) = session.set_streaming_callback(Some(stream_trampoline), ctx_ptr) {
            let _ = tx.send(Err(e));
            return;
        }

        let run = (|| -> Result<()> {
            let request = NativeRequest::new(Arc::clone(&session.api))?;
            let item = make_openai_json_item(&session.api, &request_json)?;
            request.add_item(item, true)?;
            let _response = session.process_request(&request)?;
            Ok(())
        })();
        if let Err(e) = run {
            let _ = tx.send(Err(e));
        }

        // Uninstall the callback, then release the op-lock before dropping ctx.
        let _ = session.set_streaming_callback(None, ptr::null_mut());
        drop(guard);
        drop(ctx);
        drop(session);
    });

    rx
}

// ── Generic item streaming bridge ─────────────────────────────────────────────

struct ItemStreamCtx {
    api: Arc<Api>,
    tx: UnboundedSender<Result<Item>>,
}

/// Clone a streaming error so the same failure can be delivered on both the item
/// channel and the terminal-response channel.
///
/// `FoundryLocalError` is not `Clone` because a few variants wrap non-`Clone`
/// external errors (`reqwest`, `serde_json`, `std::io`). Every string-backed
/// variant is duplicated as its own variant so error identity is preserved —
/// collapsing them into `Internal` would, for example, rewrite a `Validation`
/// from `add_item_value` into `Internal` with a doubly-prefixed message. Only the
/// non-clonable external-error variants (which do not arise on the streaming
/// request path) degrade to `Internal` carrying the original message.
fn duplicate_stream_error(error: &FoundryLocalError) -> FoundryLocalError {
    match error {
        FoundryLocalError::Native { code, message } => FoundryLocalError::Native {
            code: *code,
            message: message.clone(),
        },
        FoundryLocalError::LibraryLoad { reason } => FoundryLocalError::LibraryLoad {
            reason: reason.clone(),
        },
        FoundryLocalError::CommandExecution { reason } => FoundryLocalError::CommandExecution {
            reason: reason.clone(),
        },
        FoundryLocalError::InvalidConfiguration { reason } => {
            FoundryLocalError::InvalidConfiguration {
                reason: reason.clone(),
            }
        }
        FoundryLocalError::ModelOperation { reason } => FoundryLocalError::ModelOperation {
            reason: reason.clone(),
        },
        FoundryLocalError::Validation { reason } => FoundryLocalError::Validation {
            reason: reason.clone(),
        },
        FoundryLocalError::Internal { reason } => FoundryLocalError::Internal {
            reason: reason.clone(),
        },
        // Non-clonable external-error variants. These do not occur on the streaming
        // request path; degrade to Internal with the original message rather than
        // losing it entirely.
        FoundryLocalError::HttpRequest(_)
        | FoundryLocalError::Serialization(_)
        | FoundryLocalError::Io(_) => FoundryLocalError::Internal {
            reason: error.to_string(),
        },
    }
}

unsafe extern "C" fn item_stream_trampoline(
    data: flStreamingCallbackData,
    user_data: *mut std::ffi::c_void,
) -> c_int {
    if user_data.is_null() {
        return 0;
    }
    let result = catch_unwind(AssertUnwindSafe(|| {
        let ctx = &*(user_data as *const ItemStreamCtx);
        let queue = data.item_queue;
        if queue.is_null() {
            return 0;
        }
        let item_api = ctx.api.item_api();
        loop {
            let mut item: *mut flItem = ptr::null_mut();
            let popped = (item_api.ItemQueue_TryPop)(queue, &mut item);
            if !popped {
                break;
            }
            if item.is_null() {
                continue;
            }
            // Ownership of `item` transferred to us — decode then release.
            let decoded = item_from_native(&ctx.api, item);
            (item_api.Item_Release)(item);
            match decoded {
                Ok(decoded) => {
                    if ctx.tx.send(Ok(decoded)).is_err() {
                        return 1; // receiver dropped — cancel generation
                    }
                }
                Err(error) => {
                    let _ = ctx.tx.send(Err(error));
                    return 1;
                }
            }
        }
        0
    }));
    result.unwrap_or(1)
}

/// Run a request on a blocking worker, streaming each output item back through a
/// channel.
///
/// Input is provided as owned `items` plus an optional borrowed `input_queue`;
/// `option_pairs` are applied as request options. The channel closes when
/// generation completes or errors; dropping the receiver cancels generation.
pub(crate) fn run_item_streaming(
    session: Arc<NativeSession>,
    items: Vec<Item>,
    input_queue: Option<Arc<NativeItemQueue>>,
    option_pairs: Vec<(String, String)>,
) -> (
    UnboundedReceiver<Result<Item>>,
    oneshot::Receiver<Result<crate::response::Response>>,
) {
    let (tx, rx) = tokio::sync::mpsc::unbounded_channel::<Result<Item>>();
    let (response_tx, response_rx) = oneshot::channel();

    tokio::task::spawn_blocking(move || {
        let ctx = Box::new(ItemStreamCtx {
            api: Arc::clone(&session.api),
            tx: tx.clone(),
        });
        let ctx_ptr = &*ctx as *const ItemStreamCtx as *mut std::ffi::c_void;

        // Serialise the whole install→process→uninstall critical section: the
        // session may be shared via the public API, and the native streaming
        // callback slot is per-session state the native layer does not lock.
        let guard = session.lock_ops();

        if let Err(e) = session.set_streaming_callback(Some(item_stream_trampoline), ctx_ptr) {
            let _ = tx.send(Err(duplicate_stream_error(&e)));
            let _ = response_tx.send(Err(e));
            return;
        }

        let run = (|| -> Result<crate::response::Response> {
            let request = NativeRequest::new(Arc::clone(&session.api))?;
            for item in &items {
                request.add_item_value(item)?;
            }
            if let Some(queue) = &input_queue {
                request.add_input_queue(queue)?;
            }
            if !option_pairs.is_empty() {
                let kvps = Kvps::from_pairs(
                    Arc::clone(&session.api),
                    option_pairs.iter().map(|(k, v)| (k.as_str(), v.as_str())),
                )?;
                request.set_options(kvps.as_ptr())?;
            }
            let response = session.process_request(&request)?;
            crate::response::Response::from_native(&response)
        })();
        if let Err(e) = &run {
            let _ = tx.send(Err(duplicate_stream_error(e)));
        }

        // Release the worker's hold on the native session BEFORE publishing the
        // terminal response. A caller may await `response()` without draining the
        // item stream, then drop its `Session` and unload the model as soon as the
        // response resolves. If the worker still held a session reference at that
        // point, `ModelLoadManager::UnloadModel` would reject the unload with
        // "session(s) still using it". Uninstall the callback, release the op-lock,
        // and drop the worker's ctx/channel/session references first so the caller
        // observes a fully-released session once the response is published.
        let _ = session.set_streaming_callback(None, ptr::null_mut());
        drop(guard);
        drop(ctx);
        drop(tx);
        drop(session);

        let _ = response_tx.send(run);
    });

    (rx, response_rx)
}
