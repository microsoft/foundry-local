//! OpenAI-compatible chat completions client.
#![allow(deprecated)] // this module implements the deprecated OpenAI facade

use std::collections::HashMap;

use async_openai::types::chat::{
    ChatCompletionRequestMessage, ChatCompletionTools, CreateChatCompletionResponse,
    CreateChatCompletionStreamResponse,
};
use serde_json::{json, Value};

use crate::detail::native::NativeModel;
use crate::detail::session::{run_openai_json_streaming, NativeSession};
use crate::detail::task::spawn_blocking;
use crate::error::{FoundryLocalError, Result};
use crate::types::{ChatResponseFormat, ChatToolChoice};

use super::json_stream::JsonStream;

/// Tuning knobs for chat completion requests.
///
/// Use the chainable setter methods to configure, e.g.:
///
/// ```ignore
/// let client = model.create_chat_client()
///     .temperature(0.7)
///     .max_tokens(256);
/// ```
#[derive(Debug, Clone, Default)]
pub struct ChatClientSettings {
    frequency_penalty: Option<f64>,
    max_tokens: Option<u32>,
    n: Option<u32>,
    temperature: Option<f64>,
    presence_penalty: Option<f64>,
    top_p: Option<f64>,
    top_k: Option<u32>,
    random_seed: Option<u64>,
    response_format: Option<ChatResponseFormat>,
    tool_choice: Option<ChatToolChoice>,
}

impl ChatClientSettings {
    fn serialize(&self) -> Value {
        let mut map = serde_json::Map::new();

        if let Some(v) = self.frequency_penalty {
            map.insert("frequency_penalty".into(), json!(v));
        }
        if let Some(v) = self.max_tokens {
            map.insert("max_tokens".into(), json!(v));
        }
        if let Some(v) = self.n {
            map.insert("n".into(), json!(v));
        }
        if let Some(v) = self.presence_penalty {
            map.insert("presence_penalty".into(), json!(v));
        }
        if let Some(v) = self.temperature {
            map.insert("temperature".into(), json!(v));
        }
        if let Some(v) = self.top_p {
            map.insert("top_p".into(), json!(v));
        }

        if let Some(ref rf) = self.response_format {
            let mut rf_map = serde_json::Map::new();
            match rf {
                ChatResponseFormat::Text => {
                    rf_map.insert("type".into(), json!("text"));
                }
                ChatResponseFormat::JsonObject => {
                    rf_map.insert("type".into(), json!("json_object"));
                }
                ChatResponseFormat::JsonSchema(schema) => {
                    rf_map.insert("type".into(), json!("json_schema"));
                    rf_map.insert("json_schema".into(), json!(schema));
                }
                ChatResponseFormat::LarkGrammar(grammar) => {
                    rf_map.insert("type".into(), json!("lark_grammar"));
                    rf_map.insert("lark_grammar".into(), json!(grammar));
                }
            }
            map.insert("response_format".into(), Value::Object(rf_map));
        }

        if let Some(ref tc) = self.tool_choice {
            // Match the native `*-json` contract (see MapGuidance /
            // chat_completions_converter.cc): `none`/`auto`/`required` are plain
            // strings, while a named function is `{"type":"function",
            // "function":{"name":"…"}}`.
            let tc_value = match tc {
                ChatToolChoice::None => json!("none"),
                ChatToolChoice::Auto => json!("auto"),
                ChatToolChoice::Required => json!("required"),
                ChatToolChoice::Function(name) => json!({
                    "type": "function",
                    "function": { "name": name },
                }),
            };
            map.insert("tool_choice".into(), tc_value);
        }

        // Foundry-specific metadata for settings that don't map directly to
        // the OpenAI spec.
        let mut metadata: HashMap<String, String> = HashMap::new();
        if let Some(k) = self.top_k {
            metadata.insert("top_k".into(), k.to_string());
        }
        if let Some(s) = self.random_seed {
            metadata.insert("random_seed".into(), s.to_string());
        }
        if !metadata.is_empty() {
            map.insert("metadata".into(), json!(metadata));
        }

        Value::Object(map)
    }
}

/// A stream of [`CreateChatCompletionStreamResponse`] chunks.
///
/// Returned by [`ChatClient::complete_streaming_chat`].
pub type ChatCompletionStream = JsonStream<CreateChatCompletionStreamResponse>;

/// Client for OpenAI-compatible chat completions backed by a local model.
#[deprecated(
    since = "2.0.0",
    note = "The OpenAI direct clients are deprecated; use the Session API instead \
            (`ChatSession::new(&model)`)."
)]
pub struct ChatClient {
    model_id: String,
    model: NativeModel,
    settings: ChatClientSettings,
}

impl ChatClient {
    pub(crate) fn new(model_id: &str, model: NativeModel) -> Self {
        Self {
            model_id: model_id.to_owned(),
            model,
            settings: ChatClientSettings::default(),
        }
    }

    /// Set the frequency penalty.
    pub fn frequency_penalty(mut self, v: f64) -> Self {
        self.settings.frequency_penalty = Some(v);
        self
    }

    /// Set the maximum number of tokens to generate.
    pub fn max_tokens(mut self, v: u32) -> Self {
        self.settings.max_tokens = Some(v);
        self
    }

    /// Set the number of completions to generate.
    pub fn n(mut self, v: u32) -> Self {
        self.settings.n = Some(v);
        self
    }

    /// Set the sampling temperature.
    pub fn temperature(mut self, v: f64) -> Self {
        self.settings.temperature = Some(v);
        self
    }

    /// Set the presence penalty.
    pub fn presence_penalty(mut self, v: f64) -> Self {
        self.settings.presence_penalty = Some(v);
        self
    }

    /// Set the nucleus sampling probability.
    pub fn top_p(mut self, v: f64) -> Self {
        self.settings.top_p = Some(v);
        self
    }

    /// Set the top-k sampling parameter (Foundry extension).
    pub fn top_k(mut self, v: u32) -> Self {
        self.settings.top_k = Some(v);
        self
    }

    /// Set the random seed for reproducible results (Foundry extension).
    pub fn random_seed(mut self, v: u64) -> Self {
        self.settings.random_seed = Some(v);
        self
    }

    /// Set the desired response format.
    pub fn response_format(mut self, v: ChatResponseFormat) -> Self {
        self.settings.response_format = Some(v);
        self
    }

    /// Set the tool choice strategy.
    pub fn tool_choice(mut self, v: ChatToolChoice) -> Self {
        self.settings.tool_choice = Some(v);
        self
    }

    /// Perform a non-streaming chat completion.
    pub async fn complete_chat(
        &self,
        messages: &[ChatCompletionRequestMessage],
        tools: Option<&[ChatCompletionTools]>,
    ) -> Result<CreateChatCompletionResponse> {
        if messages.is_empty() {
            return Err(FoundryLocalError::Validation {
                reason: "messages must be a non-empty array".into(),
            });
        }

        let request = self.build_request(messages, tools, false)?;
        let request_json = serde_json::to_string(&request)?;
        let model = self.model.clone();

        let raw = spawn_blocking(move || {
            let session = NativeSession::create(&model)?;
            session.run_openai_json(&request_json)
        })
        .await?;

        let parsed: CreateChatCompletionResponse = serde_json::from_str(&raw)?;
        Ok(parsed)
    }

    /// Perform a streaming chat completion, returning a [`ChatCompletionStream`].
    ///
    /// Use the stream with `futures_core::StreamExt::next()` or
    /// `tokio_stream::StreamExt::next()`.
    pub async fn complete_streaming_chat(
        &self,
        messages: &[ChatCompletionRequestMessage],
        tools: Option<&[ChatCompletionTools]>,
    ) -> Result<ChatCompletionStream> {
        if messages.is_empty() {
            return Err(FoundryLocalError::Validation {
                reason: "messages must be a non-empty array".into(),
            });
        }

        let request = self.build_request(messages, tools, true)?;
        let request_json = serde_json::to_string(&request)?;
        let model = self.model.clone();

        let session = spawn_blocking(move || NativeSession::create(&model)).await?;
        let rx = run_openai_json_streaming(session, request_json, Box::new(normalize_chat_chunk));
        Ok(ChatCompletionStream::new(rx))
    }

    fn build_request(
        &self,
        messages: &[ChatCompletionRequestMessage],
        tools: Option<&[ChatCompletionTools]>,
        stream: bool,
    ) -> Result<Value> {
        let settings_value = self.settings.serialize();
        let mut map = match settings_value {
            Value::Object(m) => m,
            _ => serde_json::Map::new(),
        };

        map.insert("model".into(), json!(self.model_id));
        map.insert("messages".into(), serde_json::to_value(messages)?);

        if stream {
            map.insert("stream".into(), json!(true));
        }

        if let Some(t) = tools {
            map.insert("tools".into(), serde_json::to_value(t)?);
        }

        Ok(Value::Object(map))
    }
}

/// Normalize a streamed chat chunk so it parses as a
/// [`CreateChatCompletionStreamResponse`].
///
/// Foundry Local streams tool calls under `"message"` instead of the standard
/// `"delta"`; rewrite each such choice and ensure tool calls carry an `index`.
/// Chunks that are not valid JSON are passed through unchanged so the stream
/// surfaces the original parse error.
fn normalize_chat_chunk(text: String) -> Option<String> {
    let mut value: Value = match serde_json::from_str(&text) {
        Ok(v) => v,
        Err(_) => return Some(text),
    };

    if let Some(choices) = value.get_mut("choices").and_then(Value::as_array_mut) {
        for choice in choices {
            let Some(obj) = choice.as_object_mut() else {
                continue;
            };
            if obj.contains_key("message") && !obj.contains_key("delta") {
                if let Some(mut message) = obj.remove("message") {
                    if let Some(tool_calls) =
                        message.get_mut("tool_calls").and_then(Value::as_array_mut)
                    {
                        for (i, tc) in tool_calls.iter_mut().enumerate() {
                            if let Some(tc_obj) = tc.as_object_mut() {
                                tc_obj.entry("index").or_insert_with(|| json!(i));
                            }
                        }
                    }
                    obj.insert("delta".into(), message);
                }
            }
        }
    }

    serde_json::to_string(&value).ok().or(Some(text))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::types::{ChatResponseFormat, ChatToolChoice};

    fn serialize_with(f: impl FnOnce(&mut ChatClientSettings)) -> Value {
        let mut s = ChatClientSettings::default();
        f(&mut s);
        s.serialize()
    }

    #[test]
    fn response_format_json_schema_uses_snake_case_key() {
        // The native `*-json` converter (MapGuidance) reads `json_schema`, not
        // `jsonSchema`; a camelCase key is silently dropped.
        let v = serialize_with(|s| {
            s.response_format = Some(ChatResponseFormat::JsonSchema(
                "{\"type\":\"object\"}".into(),
            ));
        });
        let rf = &v["response_format"];
        assert_eq!(rf["type"], "json_schema");
        assert_eq!(rf["json_schema"], "{\"type\":\"object\"}");
        assert!(
            rf.get("jsonSchema").is_none(),
            "must not emit camelCase key"
        );
    }

    #[test]
    fn response_format_lark_grammar_uses_snake_case_key() {
        let v = serialize_with(|s| {
            s.response_format = Some(ChatResponseFormat::LarkGrammar("start: WORD+".into()));
        });
        let rf = &v["response_format"];
        assert_eq!(rf["type"], "lark_grammar");
        assert_eq!(rf["lark_grammar"], "start: WORD+");
        assert!(
            rf.get("larkGrammar").is_none(),
            "must not emit camelCase key"
        );
    }

    #[test]
    fn tool_choice_simple_modes_are_plain_strings() {
        // Native reads none/auto/required as JSON strings (tc.is_string()).
        for (choice, expected) in [
            (ChatToolChoice::None, "none"),
            (ChatToolChoice::Auto, "auto"),
            (ChatToolChoice::Required, "required"),
        ] {
            let v = serialize_with(|s| s.tool_choice = Some(choice));
            assert_eq!(v["tool_choice"], expected);
        }
    }

    #[test]
    fn tool_choice_function_nests_name_under_function() {
        // Native reads the target as tc["function"]["name"].
        let v = serialize_with(|s| {
            s.tool_choice = Some(ChatToolChoice::Function("get_weather".into()));
        });
        let tc = &v["tool_choice"];
        assert_eq!(tc["type"], "function");
        assert_eq!(tc["function"]["name"], "get_weather");
        assert!(
            tc.get("name").is_none(),
            "name must be nested under `function`"
        );
    }

    #[test]
    fn foundry_metadata_carries_top_k_and_random_seed() {
        let v = serialize_with(|s| {
            s.top_k = Some(40);
            s.random_seed = Some(7);
        });
        assert_eq!(v["metadata"]["top_k"], "40");
        assert_eq!(v["metadata"]["random_seed"], "7");
    }
}
