//! Tool-calling example demonstrating how to define tools with the `Session`
//! API, handle streamed `Item::ToolCall`s, execute the tool locally, and feed
//! results back for a multi-turn conversation.

use std::io::{self, Write};

use serde_json::{json, Value};
use tokio_stream::StreamExt;

use foundry_local_sdk::{
    ChatSession, FoundryLocalConfig, FoundryLocalError, FoundryLocalManager, Item, Request,
    RequestOptions, SearchOptions, ToolChoice, ToolDefinition,
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

#[tokio::main]
async fn main() -> Result<()> {
    // ── 1. Initialise ────────────────────────────────────────────────────
    let config = FoundryLocalConfig::new("foundry_local_samples");

    let manager = FoundryLocalManager::create(config)?;

    // ── 2. Load a model ──────────────────────────────────────────────────
    let models = manager.catalog().get_models().await?;
    let mut tool_model = None;
    for candidate in &models {
        if candidate.info()?.supports_tool_calling == Some(true) {
            tool_model = Some(candidate);
            break;
        }
    }
    let model = tool_model
        .or_else(|| models.first())
        .expect("No models available");

    if !model.is_cached().await? {
        println!("Downloading model '{}'…", model.alias());
        model.download(Some(|p: f64| println!("  {p:.1}%"))).await?;
    }
    println!("Loading model '{}'…", model.alias());
    model.load().await?;

    // ── 3. Open a chat session and register the tool ─────────────────────
    // Tools are registered once and remain available for every request on the
    // session. `json_schema` describes just the tool's parameters.
    let session = ChatSession::new(model).await?;
    let multiply_params = json!({
        "type": "object",
        "properties": {
            "a": { "type": "number", "description": "First operand" },
            "b": { "type": "number", "description": "Second operand" }
        },
        "required": ["a", "b"]
    });
    session
        .add_tool_definition(
            ToolDefinition::new("multiply", multiply_params.to_string())
                .with_description("Multiply two numbers together."),
        )
        .await?;

    // ── 4. First turn: force a tool call ─────────────────────────────────
    // `tool_choice = Required` makes the model call a tool. Streamed items
    // arrive as complete `Item::ToolCall`s — no delta reassembly needed.
    let request = Request::from_items(vec![
        Item::system_message(vec![Item::text(
            "You are a helpful calculator assistant. Use the multiply tool when asked to multiply.",
        )]),
        Item::user_message(vec![Item::text("What is 6 times 7?")]),
    ])
    .with_options(RequestOptions {
        search: SearchOptions {
            max_output_tokens: Some(512),
            ..Default::default()
        },
        tool_choice: Some(ToolChoice::Required),
        ..Default::default()
    });

    println!("Sending initial request…");
    let mut tool_results = Vec::new();
    let mut stream = session.process_streaming_request(request);
    while let Some(item) = stream.next().await {
        let item = item?;
        if let Some(call) = item.as_tool_call() {
            let args: Value = serde_json::from_str(&call.arguments).unwrap_or_else(|_| json!({}));
            println!("Tool call: {}({})", call.name, args);
            let result = invoke_tool(&call.name, &args)?;
            println!("Tool result: {result}");
            tool_results.push(Item::tool_result(call.call_id.clone(), result));
        } else if let Some(text) = item.as_text() {
            print!("{text}");
            io::stdout().flush().ok();
        }
    }

    if tool_results.is_empty() {
        println!("(model did not request any tool calls)");
        drop(session);
        model.unload().await?;
        return Ok(());
    }

    // ── 5. Second turn: feed the results back for a final answer ─────────
    // The session already retains the user question and the assistant's tool
    // call, so only the tool results need to be sent. `tool_choice = None`
    // asks the model to reply in natural language rather than call again.
    let follow_up = Request::from_items(tool_results).with_options(RequestOptions {
        search: SearchOptions {
            max_output_tokens: Some(512),
            ..Default::default()
        },
        tool_choice: Some(ToolChoice::None),
        ..Default::default()
    });

    println!("\nContinuing conversation…");
    print!("Assistant: ");
    let mut stream = session.process_streaming_request(follow_up);
    while let Some(item) = stream.next().await {
        if let Some(text) = item?.as_text() {
            print!("{text}");
            io::stdout().flush().ok();
        }
    }
    println!();

    // ── 6. Clean up──────────────────────────────────────────────────────
    // Drop the session first: the core refuses to unload a model that still
    // has a live session.
    drop(session);
    println!("\nUnloading model…");
    model.unload().await?;
    println!("Done.");

    Ok(())
}
