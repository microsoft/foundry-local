//! Tool-calling example demonstrating how to define tools, handle
//! `tool_calls` in streaming responses, execute the tool locally,
//! and feed results back for a multi-turn conversation.

use std::collections::HashMap;
use std::io::{self, Write};

use serde_json::{json, Value};
use tokio_stream::StreamExt;

use foundry_local_sdk::{
    ChatCompletionRequestMessage, ChatCompletionRequestSystemMessage,
    ChatCompletionRequestToolMessage, ChatCompletionRequestUserMessage, ChatCompletionTools,
    ChatFinishReason, ChatToolChoice, FoundryLocalConfig, FoundryLocalError, FoundryLocalManager,
};

/// Convenience alias matching the SDK's internal Result type.
type Result<T> = std::result::Result<T, FoundryLocalError>;

/// A trivial tool that multiplies two numbers.
fn multiply(a: f64, b: f64) -> f64 {
    a * b
}

/// Dispatch a tool call by name and arguments.
fn invoke_tool(name: &str, arguments: &Value) -> Result<String> {
    match name {
        "multiply" => {
            let a = arguments.get("a").and_then(|v| v.as_f64()).unwrap_or(0.0);
            let b = arguments.get("b").and_then(|v| v.as_f64()).unwrap_or(0.0);
            let result = multiply(a, b);
            Ok(result.to_string())
        }
        _ => Ok(format!("Unknown tool: {name}")),
    }
}

#[derive(Default, Clone)]
struct StreamedToolCall {
    id: String,
    name: String,
    arguments: String,
}

#[derive(Default)]
struct ToolCallState {
    /// In-progress tool calls indexed by their stream position.
    pending: HashMap<u32, StreamedToolCall>,
    /// Finalized tool calls ready for execution.
    completed: Vec<Value>,
}

#[tokio::main]
async fn main() -> Result<()> {
    // ── 1. Initialise ────────────────────────────────────────────────────
    let config = FoundryLocalConfig::new("foundry_local_samples");

    let manager = FoundryLocalManager::create(config)?;

    // ── 2. Load a model ──────────────────────────────────────────────────
    let models = manager.catalog().get_models().await?;
    let model = models
        .iter()
        .find(|m| m.info().supports_tool_calling == Some(true))
        .or_else(|| models.first())
        .expect("No models available");

    if !model.is_cached().await? {
        println!("Downloading model '{}'…", model.alias());
        model.download(Some(|p: f64| println!("  {p:.1}%"))).await?;
    }
    println!("Loading model '{}'…", model.alias());
    model.load().await?;

    // ── 3. Create a chat client with tool_choice = required ──────────────
    let client = model
        .create_chat_client()
        .tool_choice(ChatToolChoice::Required)
        .max_tokens(512);

    let tools: Vec<ChatCompletionTools> = serde_json::from_value(json!([{
        "type": "function",
        "function": {
            "name": "multiply",
            "description": "Multiply two numbers together.",
            "parameters": {
                "type": "object",
                "properties": {
                    "a": { "type": "number", "description": "First operand" },
                    "b": { "type": "number", "description": "Second operand" }
                },
                "required": ["a", "b"]
            }
        }
    }]))
    .expect("Failed to parse tool definitions");

    let mut messages: Vec<ChatCompletionRequestMessage> = vec![
        ChatCompletionRequestSystemMessage::from(
            "You are a helpful calculator assistant. Use the multiply tool when asked to multiply.",
        )
        .into(),
        ChatCompletionRequestUserMessage::from("What is 6 times 7?").into(),
    ];

    // ── 4. First streaming call – expect tool_calls ──────────────────────
    println!("Sending initial request…");

    let mut state = ToolCallState::default();
    let mut stream = client
        .complete_streaming_chat(&messages, Some(&tools))
        .await?;

    while let Some(chunk) = stream.next().await {
        let chunk = chunk?;
        if let Some(choice) = chunk.choices.first() {
            if let Some(ref tool_calls) = choice.delta.tool_calls {
                for tc in tool_calls {
                    let idx = tc.index;
                    let entry = state.pending.entry(idx).or_default();
                    if let Some(ref func) = tc.function {
                        if let Some(ref name) = func.name {
                            entry.name = name.clone();
                        }
                        if let Some(ref args) = func.arguments {
                            entry.arguments.push_str(args);
                        }
                    }
                    if let Some(ref id) = tc.id {
                        entry.id = id.clone();
                    }
                }
            }

            if choice.finish_reason == Some(ChatFinishReason::ToolCalls) {
                for (_, call) in state.pending.drain() {
                    state.completed.push(json!({
                        "id": call.id,
                        "type": "function",
                        "function": {
                            "name": call.name,
                            "arguments": call.arguments,
                        }
                    }));
                }
            }
        }
    }
    // ── 5. Execute the tool(s)───────────────────────────────────────────
    for tc in &state.completed {
        let func = &tc["function"];
        let name = func["name"].as_str().unwrap_or_default();
        let args_str = func["arguments"].as_str().unwrap_or("{}");
        let args: Value = serde_json::from_str(args_str).unwrap_or(json!({}));

        println!("Tool call: {name}({args})");
        let result = invoke_tool(name, &args)?;
        println!("Tool result: {result}");

        // Append the assistant's tool_calls message and the tool result.
        let assistant_msg: ChatCompletionRequestMessage = serde_json::from_value(json!({
            "role": "assistant",
            "content": null,
            "tool_calls": [tc],
        }))
        .expect("Failed to construct assistant message");
        messages.push(assistant_msg);
        messages.push(
            ChatCompletionRequestToolMessage {
                content: result.into(),
                tool_call_id: tc["id"].as_str().unwrap_or_default().to_string(),
            }
            .into(),
        );
    }

    // ── 6. Continue the conversation with auto tool_choice ───────────────
    let client = client.tool_choice(ChatToolChoice::Auto);

    println!("\nContinuing conversation…");
    print!("Assistant: ");
    let mut stream = client
        .complete_streaming_chat(&messages, Some(&tools))
        .await?;
    while let Some(chunk) = stream.next().await {
        let chunk = chunk?;
        if let Some(choice) = chunk.choices.first() {
            if let Some(ref content) = choice.delta.content {
                print!("{content}");
                io::stdout().flush().ok();
            }
        }
    }
    println!();

    // ── 7. Clean up──────────────────────────────────────────────────────
    println!("\nUnloading model…");
    model.unload().await?;
    println!("Done.");

    Ok(())
}
