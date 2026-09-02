//! The [`Request`] value type and its [`RequestOptions`].
//!
//! Like [`Item`], a `Request` is pure data: it owns its input items, an optional
//! borrowed streaming [`ItemQueue`], and optional [`RequestOptions`]. The native
//! `flRequest` is built transiently inside [`Session`](crate::Session) when the
//! request is processed.

use crate::detail::ffi::{
    FOUNDRY_LOCAL_PARAM_DO_SAMPLE, FOUNDRY_LOCAL_PARAM_EARLY_STOPPING,
    FOUNDRY_LOCAL_PARAM_FREQUENCY_PENALTY, FOUNDRY_LOCAL_PARAM_MAX_OUTPUT_TOKENS,
    FOUNDRY_LOCAL_PARAM_PRESENCE_PENALTY, FOUNDRY_LOCAL_PARAM_SEED,
    FOUNDRY_LOCAL_PARAM_TEMPERATURE, FOUNDRY_LOCAL_PARAM_TOOL_CHOICE, FOUNDRY_LOCAL_PARAM_TOP_K,
    FOUNDRY_LOCAL_PARAM_TOP_P,
};
use crate::item::Item;
use crate::item_queue::ItemQueue;

/// How a tool-enabled request should select among available tools.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Default)]
pub enum ToolChoice {
    /// Let the model decide whether and which tool to call.
    #[default]
    Auto,
    /// Never call a tool.
    None,
    /// Require the model to call a tool.
    Required,
}

impl ToolChoice {
    fn as_param(self) -> &'static str {
        match self {
            ToolChoice::Auto => "auto",
            ToolChoice::None => "none",
            ToolChoice::Required => "required",
        }
    }
}

/// Sampling / decoding parameters applied to a request or session.
///
/// Every field is optional; unset fields leave the model/engine default in place.
#[derive(Debug, Clone, PartialEq, Default)]
pub struct SearchOptions {
    /// Sampling temperature (higher = more random).
    pub temperature: Option<f32>,
    /// Nucleus-sampling probability mass in `(0, 1]`.
    pub top_p: Option<f32>,
    /// Top-k sampling cutoff.
    pub top_k: Option<i32>,
    /// Maximum number of tokens to generate.
    pub max_output_tokens: Option<i32>,
    /// Frequency penalty in `[-2.0, 2.0]`.
    pub frequency_penalty: Option<f32>,
    /// Presence penalty in `[-2.0, 2.0]`.
    pub presence_penalty: Option<f32>,
    /// Random seed for reproducible sampling.
    pub seed: Option<i64>,
    /// Stop as soon as a stop-sequence is matched.
    pub early_stopping: Option<bool>,
    /// Whether to sample (`false` = greedy decoding).
    pub do_sample: Option<bool>,
}

/// Options applied to a [`Request`] (or, via [`Session::set_options`], to every
/// request on a session).
///
/// Typed [`search`](Self::search) fields and [`tool_choice`](Self::tool_choice)
/// take precedence over [`additional_options`](Self::additional_options) on key
/// collision.
///
/// [`Session::set_options`]: crate::Session::set_options
#[derive(Debug, Clone, PartialEq, Default)]
pub struct RequestOptions {
    /// Sampling / decoding parameters.
    pub search: SearchOptions,
    /// Tool-selection mode for tool-enabled requests.
    pub tool_choice: Option<ToolChoice>,
    /// Passthrough key/value options for parameters not yet typed. Applied first,
    /// so typed fields win on key collision.
    pub additional_options: Vec<(String, String)>,
}

impl RequestOptions {
    /// Flatten the options into ordered `(key, value)` pairs for the native layer.
    ///
    /// `additional_options` are emitted first so that typed `search` fields and
    /// `tool_choice` override them on key collision (later writes win).
    pub(crate) fn to_pairs(&self) -> Vec<(String, String)> {
        let mut pairs: Vec<(String, String)> = self.additional_options.clone();
        let s = &self.search;
        if let Some(v) = s.temperature {
            pairs.push((FOUNDRY_LOCAL_PARAM_TEMPERATURE.to_string(), v.to_string()));
        }
        if let Some(v) = s.top_p {
            pairs.push((FOUNDRY_LOCAL_PARAM_TOP_P.to_string(), v.to_string()));
        }
        if let Some(v) = s.top_k {
            pairs.push((FOUNDRY_LOCAL_PARAM_TOP_K.to_string(), v.to_string()));
        }
        if let Some(v) = s.max_output_tokens {
            pairs.push((
                FOUNDRY_LOCAL_PARAM_MAX_OUTPUT_TOKENS.to_string(),
                v.to_string(),
            ));
        }
        if let Some(v) = s.frequency_penalty {
            pairs.push((
                FOUNDRY_LOCAL_PARAM_FREQUENCY_PENALTY.to_string(),
                v.to_string(),
            ));
        }
        if let Some(v) = s.presence_penalty {
            pairs.push((
                FOUNDRY_LOCAL_PARAM_PRESENCE_PENALTY.to_string(),
                v.to_string(),
            ));
        }
        if let Some(v) = s.seed {
            pairs.push((FOUNDRY_LOCAL_PARAM_SEED.to_string(), v.to_string()));
        }
        if let Some(v) = s.early_stopping {
            pairs.push((
                FOUNDRY_LOCAL_PARAM_EARLY_STOPPING.to_string(),
                bool_str(v).to_string(),
            ));
        }
        if let Some(v) = s.do_sample {
            pairs.push((
                FOUNDRY_LOCAL_PARAM_DO_SAMPLE.to_string(),
                bool_str(v).to_string(),
            ));
        }
        if let Some(tc) = self.tool_choice {
            pairs.push((
                FOUNDRY_LOCAL_PARAM_TOOL_CHOICE.to_string(),
                tc.as_param().to_string(),
            ));
        }
        pairs
    }
}

fn bool_str(v: bool) -> &'static str {
    if v {
        "true"
    } else {
        "false"
    }
}

/// A unit of work submitted to a [`Session`](crate::Session).
///
/// A request carries its input [`items`](Self::items), an optional streaming
/// [`input_queue`](Self::input_queue) (for incremental input such as live audio),
/// and optional [`options`](Self::options).
#[derive(Debug, Clone, Default)]
pub struct Request {
    /// The input items, in order.
    pub items: Vec<Item>,
    /// An optional streaming input queue. When present, its items are consumed in
    /// addition to [`items`](Self::items).
    pub input_queue: Option<ItemQueue>,
    /// Optional per-request options overriding any session options.
    pub options: Option<RequestOptions>,
}

impl Request {
    /// An empty request.
    pub fn new() -> Self {
        Self::default()
    }

    /// A request built from a list of input items.
    pub fn from_items(items: impl Into<Vec<Item>>) -> Self {
        Self {
            items: items.into(),
            input_queue: None,
            options: None,
        }
    }

    /// Append an input item (builder-style).
    pub fn with_item(mut self, item: Item) -> Self {
        self.items.push(item);
        self
    }

    /// Attach a streaming input queue (builder-style).
    pub fn with_input_queue(mut self, queue: ItemQueue) -> Self {
        self.input_queue = Some(queue);
        self
    }

    /// Attach per-request options (builder-style).
    pub fn with_options(mut self, options: RequestOptions) -> Self {
        self.options = Some(options);
        self
    }

    /// The flattened option pairs, or an empty vector if no options are set.
    pub(crate) fn option_pairs(&self) -> Vec<(String, String)> {
        self.options
            .as_ref()
            .map(RequestOptions::to_pairs)
            .unwrap_or_default()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn typed_fields_override_additional_options() {
        let opts = RequestOptions {
            search: SearchOptions {
                temperature: Some(0.5),
                max_output_tokens: Some(128),
                do_sample: Some(false),
                ..Default::default()
            },
            tool_choice: Some(ToolChoice::Required),
            additional_options: vec![("temperature".into(), "9.9".into())],
        };
        let pairs = opts.to_pairs();
        // additional_options are emitted first, typed fields after (later wins).
        assert_eq!(pairs[0], ("temperature".to_string(), "9.9".to_string()));
        assert!(pairs.iter().any(|(k, v)| k == "temperature" && v == "0.5"));
        assert!(pairs.iter().rposition(|(k, _)| k == "temperature").unwrap() > 0);
        assert!(pairs
            .iter()
            .any(|(k, v)| k == "max_output_tokens" && v == "128"));
        assert!(pairs.iter().any(|(k, v)| k == "do_sample" && v == "false"));
        assert!(pairs
            .iter()
            .any(|(k, v)| k == "tool_choice" && v == "required"));
    }

    #[test]
    fn builder_assembles_request() {
        let req = Request::from_items(vec![Item::text("hi")])
            .with_item(Item::text("there"))
            .with_options(RequestOptions::default());
        assert_eq!(req.items.len(), 2);
        assert!(req.options.is_some());
        assert!(req.input_queue.is_none());
    }
}
