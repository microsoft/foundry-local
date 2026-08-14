//! The [`Response`] value type produced by processing a [`Request`].
//!
//! [`Request`]: crate::Request

use crate::detail::ffi::*;
use crate::detail::session::NativeResponse;
use crate::item::Item;

/// Why generation stopped for a [`Response`].
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Default)]
pub enum FinishReason {
    /// No finish reason reported.
    #[default]
    None,
    /// Generation ended due to an error.
    Error,
    /// The model emitted a stop condition (end-of-sequence / stop sequence).
    Stop,
    /// The maximum output length was reached.
    Length,
    /// The model requested one or more tool calls.
    ToolCalls,
}

impl FinishReason {
    pub(crate) fn from_native(value: flFinishReason) -> FinishReason {
        match value {
            FOUNDRY_LOCAL_FINISH_ERROR => FinishReason::Error,
            FOUNDRY_LOCAL_FINISH_STOP => FinishReason::Stop,
            FOUNDRY_LOCAL_FINISH_LENGTH => FinishReason::Length,
            FOUNDRY_LOCAL_FINISH_TOOL_CALLS => FinishReason::ToolCalls,
            _ => FinishReason::None,
        }
    }
}

/// Token accounting for a [`Response`].
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Default)]
pub struct Usage {
    /// Tokens consumed by the prompt / input.
    pub prompt_tokens: u32,
    /// Tokens produced in the completion / output.
    pub completion_tokens: u32,
    /// Total tokens (`prompt_tokens + completion_tokens`).
    pub total_tokens: u32,
}

impl Usage {
    /// Build usage from the native `(prompt, completion, total)` triple,
    /// clamping any negative sentinel values to zero.
    pub(crate) fn from_native(prompt: i64, completion: i64, total: i64) -> Usage {
        Usage {
            prompt_tokens: clamp_u32(prompt),
            completion_tokens: clamp_u32(completion),
            total_tokens: clamp_u32(total),
        }
    }
}

fn clamp_u32(v: i64) -> u32 {
    v.clamp(0, u32::MAX as i64) as u32
}

/// The result of processing a [`Request`](crate::Request): output items plus the
/// finish reason and token usage.
#[derive(Debug, Clone, PartialEq)]
pub struct Response {
    /// The output items, in order.
    pub items: Vec<Item>,
    /// Why generation stopped.
    pub finish_reason: FinishReason,
    /// Token usage for the request.
    pub usage: Usage,
}

impl Response {
    /// Snapshot a native response into an owned [`Response`].
    pub(crate) fn from_native(native: &NativeResponse) -> Response {
        let items = native.items();
        let finish_reason = FinishReason::from_native(native.finish_reason());
        let (prompt, completion, total) = native.usage();
        Response {
            items,
            finish_reason,
            usage: Usage::from_native(prompt, completion, total),
        }
    }

    /// The concatenated text of all textual output items.
    ///
    /// Handles both plain [`Item::Text`](crate::Item::Text) items and
    /// [`Item::Message`](crate::Item::Message) items (whose text content parts are
    /// concatenated); other item kinds are ignored. A convenience for chat
    /// responses, where the output is typically a single assistant message.
    pub fn text(&self) -> String {
        let mut out = String::new();
        for item in &self.items {
            match item {
                Item::Text { text, .. } => out.push_str(text),
                Item::Message(message) => out.push_str(&message.text()),
                _ => {}
            }
        }
        out
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn finish_reason_from_native() {
        assert_eq!(
            FinishReason::from_native(FOUNDRY_LOCAL_FINISH_STOP),
            FinishReason::Stop
        );
        assert_eq!(FinishReason::from_native(9999), FinishReason::None);
    }

    #[test]
    fn usage_clamps_negatives() {
        let u = Usage::from_native(-1, 5, i64::MAX);
        assert_eq!(u.prompt_tokens, 0);
        assert_eq!(u.completion_tokens, 5);
        assert_eq!(u.total_tokens, u32::MAX);
    }

    #[test]
    fn response_text_concatenates_text_and_message_items() {
        let resp = Response {
            items: vec![
                Item::text("a"),
                Item::bytes(vec![1]),
                Item::text("b"),
                Item::assistant_message(vec![Item::text("c")]),
            ],
            finish_reason: FinishReason::Stop,
            usage: Usage::default(),
        };
        assert_eq!(resp.text(), "abc");
    }
}
