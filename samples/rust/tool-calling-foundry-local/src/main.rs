// <complete_code>
// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

use std::io::{self, Write};

use foundry_local_sdk::{
    ChatSession, FoundryLocalConfig, FoundryLocalManager, Item, Request, RequestOptions,
    SearchOptions, TextKind, ToolChoice, ToolDefinition,
};
use serde_json::{json, Value};
use tokio_stream::StreamExt;

const ALIAS: &str = "qwen2.5-0.5b";

fn multiply_numbers(first: f64, second: f64) -> f64 {
    first * second
}

fn invoke_tool(name: &str, args: &Value) -> Result<String, Box<dyn std::error::Error>> {
    match name {
        "multiply_numbers" => {
            let first = args
                .get("first")
                .and_then(|v| v.as_f64())
                .ok_or("Missing numeric argument: first")?;
            let second = args
                .get("second")
                .and_then(|v| v.as_f64())
                .ok_or("Missing numeric argument: second")?;
            Ok(multiply_numbers(first, second).to_string())
        }
        _ => Err(format!("Unknown tool: {name}").into()),
    }
}

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    println!("Tool Calling with Foundry Local");
    println!("===============================\n");

    let manager = FoundryLocalManager::create(FoundryLocalConfig::new("foundry_local_samples"))?;

    manager
        .download_and_register_eps_with_progress(None, {
            let mut current_ep = String::new();
            move |ep_name: &str, percent: f64| {
                if ep_name != current_ep {
                    if !current_ep.is_empty() {
                        println!();
                    }
                    current_ep = ep_name.to_string();
                }
                print!("\r  {:<30}  {:5.1}%", ep_name, percent);
                io::stdout().flush().ok();
            }
        })
        .await?;
    println!();

    let model = manager.catalog().get_model(ALIAS).await?;
    println!("Model: {} (id: {})", model.alias(), model.id());

    if !model.is_cached().await? {
        println!("Downloading model...");
        model
            .download(Some(|progress: f64| {
                print!("\r  {progress:.1}%");
                io::stdout().flush().ok();
            }))
            .await?;
        println!();
    }

    println!("Loading model...");
    model.load().await?;
    println!("✓ Model loaded\n");

    let session = ChatSession::new(&model).await?;
    session
        .add_tool_definition(
            ToolDefinition::new(
                "multiply_numbers",
                json!({
                    "type": "object",
                    "properties": {
                        "first": { "type": "number", "description": "The first number in the operation" },
                        "second": { "type": "number", "description": "The second number in the operation" }
                    },
                    "required": ["first", "second"]
                })
                .to_string(),
            )
            .with_description("A tool for multiplying two numbers."),
        )
        .await?;

    let request = Request::from_items(vec![
        Item::system_message(vec![Item::text(
            "You are a helpful AI assistant. If necessary, you can use any provided tools to answer the question.",
        )]),
        Item::user_message(vec![Item::text("What is the answer to 7 multiplied by 6?")]),
    ])
    .with_options(RequestOptions {
        search: SearchOptions {
            max_output_tokens: Some(512),
            ..Default::default()
        },
        tool_choice: Some(ToolChoice::Required),
        ..Default::default()
    });

    println!("Chat completion response:");
    let mut tool_results = Vec::new();
    let mut stream = session.process_streaming_request(request);
    while let Some(item) = stream.next().await {
        let item = item?;
        if let Some(call) = item.as_tool_call() {
            let args: Value = serde_json::from_str(&call.arguments)?;
            println!("\nInvoking tool: {} with arguments {}", call.name, args);
            let result = invoke_tool(&call.name, &args)?;
            println!("Tool response: {result}");
            tool_results.push(Item::tool_result(call.call_id.clone(), result));
        } else if let foundry_local_sdk::Item::Text {
            text,
            kind: TextKind::Default,
        } = item
        {
            print!("{text}");
            io::stdout().flush().ok();
        }
    }
    println!();

    if tool_results.is_empty() {
        println!("No tool calls were requested.");
        drop(session);
        model.unload().await?;
        return Ok(());
    }

    println!("\nTool calls completed. Prompting model to continue conversation...\n");
    print!("Chat completion response: ");
    let mut stream = session.process_streaming_request(
        Request::from_items(tool_results).with_options(RequestOptions {
            search: SearchOptions {
                max_output_tokens: Some(512),
                ..Default::default()
            },
            tool_choice: Some(ToolChoice::None),
            ..Default::default()
        }),
    );
    while let Some(item) = stream.next().await {
        if let foundry_local_sdk::Item::Text {
            text,
            kind: TextKind::Default,
        } = item?
        {
            print!("{text}");
            io::stdout().flush().ok();
        }
    }
    println!("\n");

    drop(session);
    println!("Unloading model...");
    model.unload().await?;
    println!("Done.");
    Ok(())
}
// </complete_code>
