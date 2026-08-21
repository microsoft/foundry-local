//! Helpers for constructing and reading native `flItem`s.
//!
//! The OpenAI facade and live-audio session build TEXT (`OPENAI_JSON`), AUDIO,
//! and BYTES items, and read TEXT items back out of responses / streamed queues.

use std::ffi::c_void;
use std::os::raw::c_char;
use std::ptr;

use super::api::{cstr_to_string, to_cstring, Api};
use super::ffi::*;
use crate::error::{FoundryLocalError, Result};
use crate::item::{
    Audio, Image, Item, MediaSource, Message, MessageRole, SpeechResult, SpeechSegment,
    SpeechSegmentKind, SpeechWord, Tensor, TensorDataType, TextKind, ToolCall, ToolResult,
};

/// Create a TEXT item with the given subtype. The native layer copies the text.
pub(crate) fn make_text_item(
    api: &Api,
    text: &str,
    item_type: flTextItemType,
) -> Result<*mut flItem> {
    // Convert before Create so a NUL-conversion error can't leak the item.
    let c = to_cstring(text)?;
    let mut item: *mut flItem = ptr::null_mut();
    api.check(unsafe { (api.item_api().Create)(FOUNDRY_LOCAL_ITEM_TEXT, &mut item) })?;
    let data = flTextData {
        version: FOUNDRY_LOCAL_API_VERSION,
        text: c.as_ptr(),
        r#type: item_type,
    };
    // SAFETY: `item` is a valid TEXT item; the native call copies the string.
    let status = unsafe { (api.item_api().SetText)(item, &data) };
    if let Err(e) = api.check(status) {
        unsafe { (api.item_api().Item_Release)(item) };
        return Err(e);
    }
    Ok(item)
}

/// Create a TEXT item carrying an opaque OpenAI REST JSON payload.
pub(crate) fn make_openai_json_item(api: &Api, json: &str) -> Result<*mut flItem> {
    make_text_item(api, json, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON)
}

/// Create a byte-backed AUDIO item describing a PCM format (used as the live
/// audio format descriptor) or carrying audio bytes. The native layer copies
/// the data, so the caller's buffer need not outlive the call.
pub(crate) fn make_audio_item(
    api: &Api,
    data: &[u8],
    format: Option<&str>,
    sample_rate: i32,
    channels: i32,
) -> Result<*mut flItem> {
    // Convert before Create so a NUL-conversion error can't leak the item.
    let format_c = match format {
        Some(f) => Some(to_cstring(f)?),
        None => None,
    };

    let mut item: *mut flItem = ptr::null_mut();
    api.check(unsafe { (api.item_api().Create)(FOUNDRY_LOCAL_ITEM_AUDIO, &mut item) })?;

    // Like SetBytes, SetAudio does not copy the sample buffer — it borrows the
    // pointer (and frees it via the deleter when one is supplied). Transfer an
    // owned heap allocation so the buffer outlives this call; the format string
    // is copied natively, so a transient CString is fine.
    let (data_ptr, len, deleter): (*mut u8, usize, flAudioDataDeleter) = if data.is_empty() {
        (ptr::null_mut(), 0, None)
    } else {
        let boxed: Box<[u8]> = data.to_vec().into_boxed_slice();
        let len = boxed.len();
        (
            Box::into_raw(boxed) as *mut u8,
            len,
            Some(rust_audio_deleter),
        )
    };

    let audio = flAudioData {
        version: FOUNDRY_LOCAL_API_VERSION,
        data: data_ptr as *const std::ffi::c_void,
        mutable_data: data_ptr as *mut std::ffi::c_void,
        data_size: len,
        format: format_c.as_ref().map_or(ptr::null(), |c| c.as_ptr()),
        uri: ptr::null(),
        sample_rate,
        channels,
        deleter,
        deleter_user_data: ptr::null_mut(),
    };
    // SAFETY: `item` is a valid AUDIO item. On success the item owns `data_ptr`
    // (freed via the deleter); on failure we reclaim it here to avoid a leak.
    let status = unsafe { (api.item_api().SetAudio)(item, &audio) };
    if let Err(e) = api.check(status) {
        unsafe {
            if !data_ptr.is_null() {
                drop(Box::from_raw(ptr::slice_from_raw_parts_mut(data_ptr, len)));
            }
            (api.item_api().Item_Release)(item);
        }
        return Err(e);
    }
    Ok(item)
}

/// Deleter that reclaims a Rust-allocated `Box<[u8]>` owned by an AUDIO item.
/// Mirrors [`rust_bytes_deleter`]; see its docs for the ownership contract.
unsafe extern "C" fn rust_audio_deleter(
    data: *const flAudioData,
    _user_data: *mut std::ffi::c_void,
) {
    if data.is_null() {
        return;
    }
    let d = &*data;
    if !d.mutable_data.is_null() && d.data_size > 0 {
        let slice = ptr::slice_from_raw_parts_mut(d.mutable_data as *mut u8, d.data_size);
        drop(Box::from_raw(slice));
    }
}

/// Deleter that reclaims a Rust-allocated `Box<[u8]>` owned by a BYTES item.
///
/// The native item calls this on destruction. `mutable_data` is the pointer we
/// handed over via `Box::into_raw`, and `data_size` is its length; together they
/// reconstruct the boxed slice so it is dropped exactly once.
unsafe extern "C" fn rust_bytes_deleter(
    data: *const flBytesData,
    _user_data: *mut std::ffi::c_void,
) {
    if data.is_null() {
        return;
    }
    let d = &*data;
    if !d.mutable_data.is_null() && d.data_size > 0 {
        let slice = ptr::slice_from_raw_parts_mut(d.mutable_data as *mut u8, d.data_size);
        drop(Box::from_raw(slice));
    }
}

/// Create a BYTES item tagged with the given originating item type (e.g. AUDIO
/// for raw PCM chunks pushed into a live session).
///
/// The native `SetBytes` does **not** copy — it stores the pointer and (when a
/// deleter is supplied) takes ownership of the buffer, freeing it via the
/// deleter when the item is destroyed. The item may be consumed asynchronously
/// (e.g. drained from an `ItemQueue` by a streaming worker long after this
/// returns), so the buffer must outlive this call. We therefore transfer an
/// owned heap allocation to the item rather than lending a caller buffer.
pub(crate) fn make_bytes_item(
    api: &Api,
    data: &[u8],
    item_type: flItemType,
) -> Result<*mut flItem> {
    let mut item: *mut flItem = ptr::null_mut();
    api.check(unsafe { (api.item_api().Create)(FOUNDRY_LOCAL_ITEM_BYTES, &mut item) })?;

    if data.is_empty() {
        let bytes = flBytesData {
            version: FOUNDRY_LOCAL_API_VERSION,
            item_type,
            data: ptr::null(),
            mutable_data: ptr::null_mut(),
            data_size: 0,
            deleter: None,
            deleter_user_data: ptr::null_mut(),
        };
        let status = unsafe { (api.item_api().SetBytes)(item, &bytes) };
        if let Err(e) = api.check(status) {
            unsafe { (api.item_api().Item_Release)(item) };
            return Err(e);
        }
        return Ok(item);
    }

    // Transfer an owned copy to the item. `into_boxed_slice` guarantees
    // capacity == len, so the deleter can reconstruct it from (ptr, len).
    let boxed: Box<[u8]> = data.to_vec().into_boxed_slice();
    let len = boxed.len();
    let raw = Box::into_raw(boxed) as *mut u8;

    let bytes = flBytesData {
        version: FOUNDRY_LOCAL_API_VERSION,
        item_type,
        data: raw as *const std::ffi::c_void,
        mutable_data: raw as *mut std::ffi::c_void,
        data_size: len,
        deleter: Some(rust_bytes_deleter),
        deleter_user_data: ptr::null_mut(),
    };

    // SAFETY: `item` is a valid BYTES item. On success the item owns `raw` and
    // frees it via the deleter; on failure SetBytesData was not applied, so we
    // reclaim and drop the box here to avoid leaking it.
    let status = unsafe { (api.item_api().SetBytes)(item, &bytes) };
    if let Err(e) = api.check(status) {
        unsafe {
            let slice = ptr::slice_from_raw_parts_mut(raw, len);
            drop(Box::from_raw(slice));
            (api.item_api().Item_Release)(item);
        }
        return Err(e);
    }
    Ok(item)
}

/// Read the text of a TEXT item. Returns `None` for null/non-text items.
///
/// # Safety
/// `item` must be null or a valid item pointer alive for the duration of this call.
/// Returns `Ok(None)` for a null or non-TEXT item so callers can fall through to
/// another item type, and `Err` when the native getter itself fails so a genuine
/// read failure is propagated rather than silently dropped.
pub(crate) unsafe fn read_text_item(api: &Api, item: *const flItem) -> Result<Option<String>> {
    if item.is_null() {
        return Ok(None);
    }
    if (api.item_api().GetType)(item) != FOUNDRY_LOCAL_ITEM_TEXT {
        return Ok(None);
    }
    let mut data = flTextData {
        version: FOUNDRY_LOCAL_API_VERSION,
        text: ptr::null::<c_char>(),
        r#type: FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT,
    };
    api.check((api.item_api().GetText)(item, &mut data))?;
    Ok(cstr_to_string(data.text))
}

/// Text + timing read from a SPEECH_SEGMENT item (output-only).
pub(crate) struct SpeechSegmentText {
    pub text: String,
    pub is_final: bool,
    pub start_time_s: Option<f64>,
    pub end_time_s: Option<f64>,
}

/// Convert a native millisecond field to seconds, mapping the UNSET sentinel to `None`.
fn duration_ms_to_seconds(ms: i64) -> Option<f64> {
    if ms == FOUNDRY_LOCAL_DURATION_UNSET {
        None
    } else {
        Some(ms as f64 / 1000.0)
    }
}

/// Read a SPEECH_SEGMENT item (output-only).
///
/// Returns `Ok(None)` for a null or non-SPEECH_SEGMENT item so callers can fall
/// through to another item type, and `Err` when the native getter fails so a
/// genuine read failure is propagated rather than silently dropped.
///
/// # Safety
/// `item` must be null or a valid item pointer alive for the duration of this call.
pub(crate) unsafe fn read_speech_segment(
    api: &Api,
    item: *const flItem,
) -> Result<Option<SpeechSegmentText>> {
    if item.is_null() || (api.item_api().GetType)(item) != FOUNDRY_LOCAL_ITEM_SPEECH_SEGMENT {
        return Ok(None);
    }
    let mut data = flSpeechSegmentData {
        version: FOUNDRY_LOCAL_API_VERSION,
        kind: FOUNDRY_LOCAL_SPEECH_SEGMENT_NONE,
        text: ptr::null::<c_char>(),
        start_time_ms: FOUNDRY_LOCAL_DURATION_UNSET,
        end_time_ms: FOUNDRY_LOCAL_DURATION_UNSET,
        utterance_start: false,
        words: ptr::null::<flSpeechWord>(),
        words_count: 0,
        language: ptr::null::<c_char>(),
    };
    api.check((api.item_api().GetSpeechSegment)(item, &mut data))?;
    Ok(Some(SpeechSegmentText {
        text: cstr_to_string(data.text).unwrap_or_default(),
        // PARTIAL is an interim hypothesis; FINAL (and NONE entries) are stable.
        is_final: data.kind == FOUNDRY_LOCAL_SPEECH_SEGMENT_FINAL,
        start_time_s: duration_ms_to_seconds(data.start_time_ms),
        end_time_s: duration_ms_to_seconds(data.end_time_ms),
    }))
}

/// Read the concatenated transcript of a SPEECH_RESULT item (output-only).
///
/// Returns `Ok(None)` for a null or non-SPEECH_RESULT item so callers can fall
/// through to another item type, and `Err` when the native getter fails so a
/// genuine read failure is propagated rather than silently dropped.
///
/// # Safety
/// `item` must be null or a valid item pointer alive for the duration of this call.
pub(crate) unsafe fn read_speech_result_text(
    api: &Api,
    item: *const flItem,
) -> Result<Option<String>> {
    if item.is_null() || (api.item_api().GetType)(item) != FOUNDRY_LOCAL_ITEM_SPEECH_RESULT {
        return Ok(None);
    }
    let mut data = flSpeechResultData {
        version: FOUNDRY_LOCAL_API_VERSION,
        text: ptr::null::<c_char>(),
        language: ptr::null::<c_char>(),
        duration_ms: FOUNDRY_LOCAL_DURATION_UNSET,
        segments: ptr::null::<*const flItem>(),
        segments_count: 0,
    };
    api.check((api.item_api().GetSpeechResult)(item, &mut data))?;
    Ok(cstr_to_string(data.text))
}

// ── Additional native item builders (image / tensor / message / tool) ─────────

/// Deleter reclaiming a Rust-allocated `Box<[u8]>` owned by an IMAGE item.
/// Mirrors [`rust_bytes_deleter`]; `data_size` gives the boxed slice length.
unsafe extern "C" fn rust_image_deleter(data: *const flImageData, _user_data: *mut c_void) {
    if data.is_null() {
        return;
    }
    let d = &*data;
    if !d.mutable_data.is_null() && d.data_size > 0 {
        let slice = ptr::slice_from_raw_parts_mut(d.mutable_data as *mut u8, d.data_size);
        drop(Box::from_raw(slice));
    }
}

/// Deleter reclaiming a Rust-allocated `Box<[u8]>` owned by a TENSOR item.
///
/// Unlike bytes/image/audio, `flTensorData` carries no `data_size`, so the byte
/// length is smuggled through `deleter_user_data` as a boxed `usize`. Both the
/// data box and the length box are reclaimed here (each exactly once).
unsafe extern "C" fn rust_tensor_deleter(data: *const flTensorData, user_data: *mut c_void) {
    let len = if user_data.is_null() {
        0
    } else {
        *Box::from_raw(user_data as *mut usize)
    };
    if data.is_null() {
        return;
    }
    let d = &*data;
    if !d.mutable_data.is_null() && len > 0 {
        let slice = ptr::slice_from_raw_parts_mut(d.mutable_data as *mut u8, len);
        drop(Box::from_raw(slice));
    }
}

/// Create a TENSOR item. The native layer copies the shape but borrows (and, via
/// the deleter, takes ownership of) the element buffer, so we transfer an owned
/// heap allocation whose length travels with the deleter's user data.
pub(crate) fn make_tensor_item(api: &Api, tensor: &Tensor) -> Result<*mut flItem> {
    let mut item: *mut flItem = ptr::null_mut();
    api.check(unsafe { (api.item_api().Create)(FOUNDRY_LOCAL_ITEM_TENSOR, &mut item) })?;

    // `shape` only needs to outlive the SetTensor call (native copies it).
    let shape = tensor.shape.clone();

    let (data_ptr, len, deleter, user_data): (*mut u8, usize, flTensorDataDeleter, *mut c_void) =
        if tensor.data.is_empty() {
            (ptr::null_mut(), 0, None, ptr::null_mut())
        } else {
            let boxed: Box<[u8]> = tensor.data.clone().into_boxed_slice();
            let len = boxed.len();
            let raw = Box::into_raw(boxed) as *mut u8;
            let len_box = Box::into_raw(Box::new(len)) as *mut c_void;
            (raw, len, Some(rust_tensor_deleter), len_box)
        };

    let data = flTensorData {
        version: FOUNDRY_LOCAL_API_VERSION,
        data_type: tensor.data_type.to_native(),
        data: data_ptr as *const c_void,
        mutable_data: data_ptr as *mut c_void,
        shape: shape.as_ptr(),
        rank: shape.len(),
        deleter,
        deleter_user_data: user_data,
    };
    // SAFETY: `item` is a valid TENSOR item. On success it owns `data_ptr` (freed
    // via the deleter); on failure we reclaim both the data and length boxes.
    let status = unsafe { (api.item_api().SetTensor)(item, &data) };
    if let Err(e) = api.check(status) {
        unsafe {
            if !data_ptr.is_null() {
                drop(Box::from_raw(ptr::slice_from_raw_parts_mut(data_ptr, len)));
            }
            if !user_data.is_null() {
                drop(Box::from_raw(user_data as *mut usize));
            }
            (api.item_api().Item_Release)(item);
        }
        return Err(e);
    }
    Ok(item)
}

/// Create an IMAGE item from inline bytes or a URI. Native copies the format/URI
/// strings; inline bytes are transferred as an owned heap allocation.
pub(crate) fn make_image_item(api: &Api, image: &Image) -> Result<*mut flItem> {
    let format_c = match &image.format {
        Some(f) => Some(to_cstring(f)?),
        None => None,
    };
    let uri_c = match &image.source {
        MediaSource::Uri(u) => Some(to_cstring(u)?),
        MediaSource::Data(_) => None,
    };

    let mut item: *mut flItem = ptr::null_mut();
    api.check(unsafe { (api.item_api().Create)(FOUNDRY_LOCAL_ITEM_IMAGE, &mut item) })?;

    let (data_ptr, len, deleter): (*mut u8, usize, flImageDataDeleter) = match &image.source {
        MediaSource::Data(bytes) if !bytes.is_empty() => {
            let boxed: Box<[u8]> = bytes.clone().into_boxed_slice();
            let len = boxed.len();
            (
                Box::into_raw(boxed) as *mut u8,
                len,
                Some(rust_image_deleter),
            )
        }
        _ => (ptr::null_mut(), 0, None),
    };

    let data = flImageData {
        version: FOUNDRY_LOCAL_API_VERSION,
        data: data_ptr as *const c_void,
        mutable_data: data_ptr as *mut c_void,
        data_size: len,
        format: format_c.as_ref().map_or(ptr::null(), |c| c.as_ptr()),
        uri: uri_c.as_ref().map_or(ptr::null(), |c| c.as_ptr()),
        deleter,
        deleter_user_data: ptr::null_mut(),
    };
    // SAFETY: `item` is a valid IMAGE item; on failure we reclaim the data box.
    let status = unsafe { (api.item_api().SetImage)(item, &data) };
    if let Err(e) = api.check(status) {
        unsafe {
            if !data_ptr.is_null() {
                drop(Box::from_raw(ptr::slice_from_raw_parts_mut(data_ptr, len)));
            }
            (api.item_api().Item_Release)(item);
        }
        return Err(e);
    }
    Ok(item)
}

/// Create an AUDIO item that references external content by URI (no sample data).
pub(crate) fn make_audio_uri_item(
    api: &Api,
    uri: &str,
    format: Option<&str>,
    sample_rate: i32,
    channels: i32,
) -> Result<*mut flItem> {
    let uri_c = to_cstring(uri)?;
    let format_c = match format {
        Some(f) => Some(to_cstring(f)?),
        None => None,
    };

    let mut item: *mut flItem = ptr::null_mut();
    api.check(unsafe { (api.item_api().Create)(FOUNDRY_LOCAL_ITEM_AUDIO, &mut item) })?;

    let audio = flAudioData {
        version: FOUNDRY_LOCAL_API_VERSION,
        data: ptr::null(),
        mutable_data: ptr::null_mut(),
        data_size: 0,
        format: format_c.as_ref().map_or(ptr::null(), |c| c.as_ptr()),
        uri: uri_c.as_ptr(),
        sample_rate,
        channels,
        deleter: None,
        deleter_user_data: ptr::null_mut(),
    };
    let status = unsafe { (api.item_api().SetAudio)(item, &audio) };
    if let Err(e) = api.check(status) {
        unsafe { (api.item_api().Item_Release)(item) };
        return Err(e);
    }
    Ok(item)
}

/// Create a TOOL_CALL item. Native copies all three strings.
pub(crate) fn make_tool_call_item(api: &Api, tool_call: &ToolCall) -> Result<*mut flItem> {
    let call_id = to_cstring(&tool_call.call_id)?;
    let name = to_cstring(&tool_call.name)?;
    let arguments = to_cstring(&tool_call.arguments)?;

    let mut item: *mut flItem = ptr::null_mut();
    api.check(unsafe { (api.item_api().Create)(FOUNDRY_LOCAL_ITEM_TOOL_CALL, &mut item) })?;
    let data = flToolCallData {
        version: FOUNDRY_LOCAL_API_VERSION,
        call_id: call_id.as_ptr(),
        name: name.as_ptr(),
        arguments: arguments.as_ptr(),
    };
    let status = unsafe { (api.item_api().SetToolCall)(item, &data) };
    if let Err(e) = api.check(status) {
        unsafe { (api.item_api().Item_Release)(item) };
        return Err(e);
    }
    Ok(item)
}

/// Create a TOOL_RESULT item. Native copies both strings.
pub(crate) fn make_tool_result_item(api: &Api, tool_result: &ToolResult) -> Result<*mut flItem> {
    let call_id = to_cstring(&tool_result.call_id)?;
    let result = to_cstring(&tool_result.result)?;

    let mut item: *mut flItem = ptr::null_mut();
    api.check(unsafe { (api.item_api().Create)(FOUNDRY_LOCAL_ITEM_TOOL_RESULT, &mut item) })?;
    let data = flToolResultData {
        version: FOUNDRY_LOCAL_API_VERSION,
        call_id: call_id.as_ptr(),
        result: result.as_ptr(),
    };
    let status = unsafe { (api.item_api().SetToolResult)(item, &data) };
    if let Err(e) = api.check(status) {
        unsafe { (api.item_api().Item_Release)(item) };
        return Err(e);
    }
    Ok(item)
}

/// Release a batch of transient native items (used to unwind on error).
unsafe fn release_all(api: &Api, items: &[*mut flItem]) {
    for &it in items {
        if !it.is_null() {
            (api.item_api().Item_Release)(it);
        }
    }
}

/// Create a MESSAGE item. Its content parts are built as transient native items,
/// handed to `SetMessage` (which **deep-clones** them), then released. Only
/// TEXT/IMAGE/AUDIO parts are accepted by the native layer.
pub(crate) fn make_message_item(api: &Api, message: &Message) -> Result<*mut flItem> {
    // Build children first so a failure can't leave a half-populated message.
    let mut children: Vec<*mut flItem> = Vec::with_capacity(message.content.len());
    for part in &message.content {
        match item_to_native(api, part) {
            Ok(child) => children.push(child),
            Err(e) => {
                unsafe { release_all(api, &children) };
                return Err(e);
            }
        }
    }

    let name_c = match &message.name {
        Some(n) => match to_cstring(n) {
            Ok(c) => Some(c),
            Err(e) => {
                unsafe { release_all(api, &children) };
                return Err(e);
            }
        },
        None => None,
    };

    let mut item: *mut flItem = ptr::null_mut();
    if let Err(e) =
        api.check(unsafe { (api.item_api().Create)(FOUNDRY_LOCAL_ITEM_MESSAGE, &mut item) })
    {
        unsafe { release_all(api, &children) };
        return Err(e);
    }

    let child_ptrs: Vec<*const flItem> = children.iter().map(|p| *p as *const flItem).collect();
    let data = flMessageData {
        version: FOUNDRY_LOCAL_API_VERSION,
        role: message.role.to_native(),
        content_items: if child_ptrs.is_empty() {
            ptr::null()
        } else {
            child_ptrs.as_ptr()
        },
        content_items_count: child_ptrs.len(),
        name: name_c.as_ref().map_or(ptr::null(), |c| c.as_ptr()),
    };
    let status = unsafe { (api.item_api().SetMessage)(item, &data) };
    // Native deep-clones each part on success; on failure it retains nothing.
    // Either way the transient children are ours to release now.
    unsafe { release_all(api, &children) };
    if let Err(e) = api.check(status) {
        unsafe { (api.item_api().Item_Release)(item) };
        return Err(e);
    }
    Ok(item)
}

/// Build a native `flItem` from a public [`Item`]. The returned pointer is owned
/// by the caller (release via `Item_Release`, or transfer into a request/queue).
///
/// Speech items are output-only and rejected with a validation error.
pub(crate) fn item_to_native(api: &Api, item: &Item) -> Result<*mut flItem> {
    match item {
        Item::Text { text, kind } => make_text_item(api, text, kind.to_native()),
        Item::Message(m) => make_message_item(api, m),
        Item::Bytes(data) => make_bytes_item(api, data, FOUNDRY_LOCAL_ITEM_BYTES),
        Item::Tensor(t) => make_tensor_item(api, t),
        Item::Image(img) => make_image_item(api, img),
        Item::Audio(a) => match &a.source {
            MediaSource::Data(d) => {
                make_audio_item(api, d, a.format.as_deref(), a.sample_rate, a.channels)
            }
            MediaSource::Uri(u) => {
                make_audio_uri_item(api, u, a.format.as_deref(), a.sample_rate, a.channels)
            }
        },
        Item::ToolCall(c) => make_tool_call_item(api, c),
        Item::ToolResult(r) => make_tool_result_item(api, r),
        Item::SpeechSegment(_) | Item::SpeechResult(_) => Err(FoundryLocalError::Validation {
            reason: "speech items are output-only and cannot be added to a request".into(),
        }),
    }
}

// ── Full native item readers (native `flItem` → public `Item`) ────────────────

/// Copy `size` bytes from a borrowed native buffer into an owned `Vec`.
///
/// # Safety
/// `data`, when non-null, must point to at least `size` valid bytes.
unsafe fn copy_bytes(data: *const c_void, size: usize) -> Vec<u8> {
    if data.is_null() || size == 0 {
        Vec::new()
    } else {
        std::slice::from_raw_parts(data as *const u8, size).to_vec()
    }
}

/// Map a native millisecond field to `Option`, treating the UNSET sentinel as `None`.
fn opt_ms(ms: i64) -> Option<i64> {
    if ms == FOUNDRY_LOCAL_DURATION_UNSET {
        None
    } else {
        Some(ms)
    }
}

/// The number of bytes backing a tensor of `data_type` and `shape`, or `None`
/// when the element size is not statically known (e.g. STRING tensors).
fn tensor_byte_len(data_type: TensorDataType, shape: &[i64]) -> Option<usize> {
    let bits: usize = match data_type {
        TensorDataType::Uint8
        | TensorDataType::Int8
        | TensorDataType::Bool
        | TensorDataType::Float8E4M3FN
        | TensorDataType::Float8E4M3FNUZ
        | TensorDataType::Float8E5M2
        | TensorDataType::Float8E5M2FNUZ
        | TensorDataType::Float8E8M0 => 8,
        TensorDataType::Uint16
        | TensorDataType::Int16
        | TensorDataType::Float16
        | TensorDataType::BFloat16 => 16,
        TensorDataType::Float | TensorDataType::Int32 | TensorDataType::Uint32 => 32,
        TensorDataType::Int64
        | TensorDataType::Uint64
        | TensorDataType::Double
        | TensorDataType::Complex64 => 64,
        TensorDataType::Complex128 => 128,
        TensorDataType::Uint4 | TensorDataType::Int4 | TensorDataType::Float4E2M1 => 4,
        TensorDataType::String | TensorDataType::Undefined => return None,
    };
    let mut elements: i128 = 1;
    for &dim in shape {
        if dim < 0 {
            return None;
        }
        elements = elements.checked_mul(dim as i128)?;
    }
    let total_bits = elements.checked_mul(bits as i128)?;
    usize::try_from((total_bits + 7) / 8).ok()
}

/// Read a TEXT item into an owned [`Item::Text`] (preserving its [`TextKind`]).
unsafe fn read_text_full(api: &Api, item: *const flItem) -> Result<Item> {
    let mut data = flTextData {
        version: FOUNDRY_LOCAL_API_VERSION,
        text: ptr::null::<c_char>(),
        r#type: FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT,
    };
    api.check((api.item_api().GetText)(item, &mut data))?;
    Ok(Item::Text {
        text: cstr_to_string(data.text).unwrap_or_default(),
        kind: TextKind::from_native(data.r#type),
    })
}

/// Read a BYTES item into an owned [`Item::Bytes`].
unsafe fn read_bytes_full(api: &Api, item: *const flItem) -> Result<Item> {
    let mut data = flBytesData {
        version: FOUNDRY_LOCAL_API_VERSION,
        item_type: FOUNDRY_LOCAL_ITEM_BYTES,
        data: ptr::null(),
        mutable_data: ptr::null_mut(),
        data_size: 0,
        deleter: None,
        deleter_user_data: ptr::null_mut(),
    };
    api.check((api.item_api().GetBytes)(item, &mut data))?;
    Ok(Item::Bytes(copy_bytes(data.data, data.data_size)))
}

/// Read a TENSOR item into an owned [`Item::Tensor`].
unsafe fn read_tensor_full(api: &Api, item: *const flItem) -> Result<Item> {
    let mut data = flTensorData {
        version: FOUNDRY_LOCAL_API_VERSION,
        data_type: FOUNDRY_LOCAL_TENSOR_UNDEFINED,
        data: ptr::null(),
        mutable_data: ptr::null_mut(),
        shape: ptr::null(),
        rank: 0,
        deleter: None,
        deleter_user_data: ptr::null_mut(),
    };
    api.check((api.item_api().GetTensor)(item, &mut data))?;
    let shape: Vec<i64> = if data.shape.is_null() || data.rank == 0 {
        Vec::new()
    } else {
        std::slice::from_raw_parts(data.shape, data.rank).to_vec()
    };
    let data_type = TensorDataType::from_native(data.data_type);
    let bytes = match tensor_byte_len(data_type, &shape) {
        Some(len) => copy_bytes(data.data, len),
        None => Vec::new(),
    };
    Ok(Item::Tensor(Tensor {
        data_type,
        shape,
        data: bytes,
    }))
}

/// Read an IMAGE item into an owned [`Item::Image`].
unsafe fn read_image_full(api: &Api, item: *const flItem) -> Result<Item> {
    let mut data = flImageData {
        version: FOUNDRY_LOCAL_API_VERSION,
        data: ptr::null(),
        mutable_data: ptr::null_mut(),
        data_size: 0,
        format: ptr::null(),
        uri: ptr::null(),
        deleter: None,
        deleter_user_data: ptr::null_mut(),
    };
    api.check((api.item_api().GetImage)(item, &mut data))?;
    let source = if !data.data.is_null() && data.data_size > 0 {
        MediaSource::Data(copy_bytes(data.data, data.data_size))
    } else if let Some(uri) = cstr_to_string(data.uri) {
        MediaSource::Uri(uri)
    } else {
        MediaSource::Data(Vec::new())
    };
    Ok(Item::Image(Image {
        source,
        format: cstr_to_string(data.format),
    }))
}

/// Read an AUDIO item into an owned [`Item::Audio`].
unsafe fn read_audio_full(api: &Api, item: *const flItem) -> Result<Item> {
    let mut data = flAudioData {
        version: FOUNDRY_LOCAL_API_VERSION,
        data: ptr::null(),
        mutable_data: ptr::null_mut(),
        data_size: 0,
        format: ptr::null(),
        uri: ptr::null(),
        sample_rate: 0,
        channels: 0,
        deleter: None,
        deleter_user_data: ptr::null_mut(),
    };
    api.check((api.item_api().GetAudio)(item, &mut data))?;
    let source = if !data.data.is_null() && data.data_size > 0 {
        MediaSource::Data(copy_bytes(data.data, data.data_size))
    } else if let Some(uri) = cstr_to_string(data.uri) {
        MediaSource::Uri(uri)
    } else {
        MediaSource::Data(Vec::new())
    };
    Ok(Item::Audio(Audio {
        source,
        format: cstr_to_string(data.format),
        sample_rate: data.sample_rate,
        channels: data.channels,
    }))
}

/// Read a MESSAGE item into an owned [`Item::Message`], recursively decoding parts.
unsafe fn read_message_full(api: &Api, item: *const flItem) -> Result<Item> {
    let mut data = flMessageData {
        version: FOUNDRY_LOCAL_API_VERSION,
        role: FOUNDRY_LOCAL_ROLE_NONE,
        content_items: ptr::null(),
        content_items_count: 0,
        name: ptr::null(),
    };
    api.check((api.item_api().GetMessage)(item, &mut data))?;
    let mut content = Vec::with_capacity(data.content_items_count);
    if !data.content_items.is_null() {
        for i in 0..data.content_items_count {
            let part = *data.content_items.add(i);
            content.push(item_from_native(api, part)?);
        }
    }
    Ok(Item::Message(Message {
        role: MessageRole::from_native(data.role),
        content,
        name: cstr_to_string(data.name),
    }))
}

/// Read a TOOL_CALL item into an owned [`Item::ToolCall`].
unsafe fn read_tool_call_full(api: &Api, item: *const flItem) -> Result<Item> {
    let mut data = flToolCallData {
        version: FOUNDRY_LOCAL_API_VERSION,
        call_id: ptr::null(),
        name: ptr::null(),
        arguments: ptr::null(),
    };
    api.check((api.item_api().GetToolCall)(item, &mut data))?;
    Ok(Item::ToolCall(ToolCall {
        call_id: cstr_to_string(data.call_id).unwrap_or_default(),
        name: cstr_to_string(data.name).unwrap_or_default(),
        arguments: cstr_to_string(data.arguments).unwrap_or_default(),
    }))
}

/// Read a TOOL_RESULT item into an owned [`Item::ToolResult`].
unsafe fn read_tool_result_full(api: &Api, item: *const flItem) -> Result<Item> {
    let mut data = flToolResultData {
        version: FOUNDRY_LOCAL_API_VERSION,
        call_id: ptr::null(),
        result: ptr::null(),
    };
    api.check((api.item_api().GetToolResult)(item, &mut data))?;
    Ok(Item::ToolResult(ToolResult {
        call_id: cstr_to_string(data.call_id).unwrap_or_default(),
        result: cstr_to_string(data.result).unwrap_or_default(),
    }))
}

/// Read a SPEECH_SEGMENT item into an owned [`SpeechSegment`] value.
unsafe fn read_speech_segment_value(api: &Api, item: *const flItem) -> Result<SpeechSegment> {
    let mut data = flSpeechSegmentData {
        version: FOUNDRY_LOCAL_API_VERSION,
        kind: FOUNDRY_LOCAL_SPEECH_SEGMENT_NONE,
        text: ptr::null::<c_char>(),
        start_time_ms: FOUNDRY_LOCAL_DURATION_UNSET,
        end_time_ms: FOUNDRY_LOCAL_DURATION_UNSET,
        utterance_start: false,
        words: ptr::null::<flSpeechWord>(),
        words_count: 0,
        language: ptr::null::<c_char>(),
    };
    api.check((api.item_api().GetSpeechSegment)(item, &mut data))?;
    let mut words = Vec::with_capacity(data.words_count);
    if !data.words.is_null() {
        for i in 0..data.words_count {
            let w = &*data.words.add(i);
            let confidence = if w.confidence == FOUNDRY_LOCAL_CONFIDENCE_UNSET {
                None
            } else {
                Some(w.confidence)
            };
            words.push(SpeechWord {
                text: cstr_to_string(w.text).unwrap_or_default(),
                start_time_ms: opt_ms(w.start_time_ms),
                end_time_ms: opt_ms(w.end_time_ms),
                confidence,
                speaker_id: cstr_to_string(w.speaker_id),
            });
        }
    }
    Ok(SpeechSegment {
        kind: SpeechSegmentKind::from_native(data.kind),
        text: cstr_to_string(data.text).unwrap_or_default(),
        start_time_ms: opt_ms(data.start_time_ms),
        end_time_ms: opt_ms(data.end_time_ms),
        utterance_start: data.utterance_start,
        words,
        language: cstr_to_string(data.language),
    })
}

/// Read a SPEECH_RESULT item into an owned [`Item::SpeechResult`].
unsafe fn read_speech_result_full(api: &Api, item: *const flItem) -> Result<Item> {
    let mut data = flSpeechResultData {
        version: FOUNDRY_LOCAL_API_VERSION,
        text: ptr::null::<c_char>(),
        language: ptr::null::<c_char>(),
        duration_ms: FOUNDRY_LOCAL_DURATION_UNSET,
        segments: ptr::null::<*const flItem>(),
        segments_count: 0,
    };
    api.check((api.item_api().GetSpeechResult)(item, &mut data))?;
    let mut segments = Vec::with_capacity(data.segments_count);
    if !data.segments.is_null() {
        for i in 0..data.segments_count {
            let seg = *data.segments.add(i);
            segments.push(item_from_native(api, seg)?);
        }
    }
    Ok(Item::SpeechResult(SpeechResult {
        text: cstr_to_string(data.text).unwrap_or_default(),
        language: cstr_to_string(data.language),
        duration_ms: opt_ms(data.duration_ms),
        segments,
    }))
}

/// Decode a native `flItem` into an owned public [`Item`].
///
/// Returns an error for a null item, an unsupported item type, or a native
/// getter failure so callers never silently lose response data.
///
/// # Safety
/// `item` must be null or a valid item pointer alive for the duration of this call.
pub(crate) unsafe fn item_from_native(api: &Api, item: *const flItem) -> Result<Item> {
    if item.is_null() {
        return Err(FoundryLocalError::Internal {
            reason: "native response contained a null item".into(),
        });
    }
    let item_type = (api.item_api().GetType)(item);
    match item_type {
        FOUNDRY_LOCAL_ITEM_TEXT => read_text_full(api, item),
        FOUNDRY_LOCAL_ITEM_BYTES => read_bytes_full(api, item),
        FOUNDRY_LOCAL_ITEM_TENSOR => read_tensor_full(api, item),
        FOUNDRY_LOCAL_ITEM_MESSAGE => read_message_full(api, item),
        FOUNDRY_LOCAL_ITEM_IMAGE => read_image_full(api, item),
        FOUNDRY_LOCAL_ITEM_AUDIO => read_audio_full(api, item),
        FOUNDRY_LOCAL_ITEM_TOOL_CALL => read_tool_call_full(api, item),
        FOUNDRY_LOCAL_ITEM_TOOL_RESULT => read_tool_result_full(api, item),
        FOUNDRY_LOCAL_ITEM_SPEECH_SEGMENT => {
            read_speech_segment_value(api, item).map(Item::SpeechSegment)
        }
        FOUNDRY_LOCAL_ITEM_SPEECH_RESULT => read_speech_result_full(api, item),
        _ => Err(FoundryLocalError::Internal {
            reason: format!("native response contained unsupported item type {item_type}"),
        }),
    }
}
