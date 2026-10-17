// <complete_code>
use foundry_local_sdk::{
    ChatSession, FoundryLocalConfig, FoundryLocalManager, Item, Request, RequestOptions,
    SearchOptions, ToolChoice, ToolDefinition,
};
use serde_json::{json, Value};
use std::io::{self, BufRead, Write};

fn execute_tool(name: &str, arguments: &Value) -> Value {
    match name {
        "get_weather" => {
            let location = arguments["location"].as_str().unwrap_or("unknown");
            let unit = arguments["unit"].as_str().unwrap_or("celsius");
            let temp = if unit == "celsius" { 22 } else { 72 };
            json!({
                "location": location,
                "temperature": temp,
                "unit": unit,
                "condition": "Sunny"
            })
        }
        "calculate" => {
            let expression = arguments["expression"].as_str().unwrap_or("");
            let is_valid = expression.chars().all(|c| "0123456789+-*/(). ".contains(c));
            if !is_valid {
                return json!({"error": "Invalid expression"});
            }
            match eval_expression(expression) {
                Ok(result) => json!({ "expression": expression, "result": result }),
                Err(e) => json!({ "error": e }),
            }
        }
        _ => json!({"error": format!("Unknown function: {}", name)}),
    }
}

fn eval_expression(expr: &str) -> Result<f64, String> {
    let expr = expr.replace(' ', "");
    let chars: Vec<char> = expr.chars().collect();
    let mut pos = 0;
    let result = parse_add(&chars, &mut pos)?;
    if pos < chars.len() {
        return Err("Unexpected character".to_string());
    }
    Ok(result)
}

fn parse_add(chars: &[char], pos: &mut usize) -> Result<f64, String> {
    let mut result = parse_mul(chars, pos)?;
    while *pos < chars.len() && (chars[*pos] == '+' || chars[*pos] == '-') {
        let op = chars[*pos];
        *pos += 1;
        let right = parse_mul(chars, pos)?;
        result = if op == '+' {
            result + right
        } else {
            result - right
        };
    }
    Ok(result)
}

fn parse_mul(chars: &[char], pos: &mut usize) -> Result<f64, String> {
    let mut result = parse_atom(chars, pos)?;
    while *pos < chars.len() && (chars[*pos] == '*' || chars[*pos] == '/') {
        let op = chars[*pos];
        *pos += 1;
        let right = parse_atom(chars, pos)?;
        result = if op == '*' {
            result * right
        } else {
            result / right
        };
    }
    Ok(result)
}

fn parse_atom(chars: &[char], pos: &mut usize) -> Result<f64, String> {
    if *pos < chars.len() && chars[*pos] == '(' {
        *pos += 1;
        let result = parse_add(chars, pos)?;
        if *pos < chars.len() && chars[*pos] == ')' {
            *pos += 1;
        }
        return Ok(result);
    }
    let start = *pos;
    while *pos < chars.len() && (chars[*pos].is_ascii_digit() || chars[*pos] == '.') {
        *pos += 1;
    }
    if start == *pos {
        return Err("Expected number".to_string());
    }
    let num_str: String = chars[start..*pos].iter().collect();
    num_str.parse::<f64>().map_err(|e| e.to_string())
}

async fn process_turn(session: &ChatSession, items: Vec<Item>) -> anyhow::Result<String> {
    let mut response = session
        .process_request(Request::from_items(items).with_options(RequestOptions {
            search: SearchOptions {
                temperature: Some(0.7),
                max_output_tokens: Some(512),
                ..Default::default()
            },
            tool_choice: Some(ToolChoice::Auto),
            ..Default::default()
        }))
        .await?;

    loop {
        let tool_calls: Vec<_> = response
            .items
            .iter()
            .filter_map(|item| item.as_tool_call().cloned())
            .collect();
        if tool_calls.is_empty() {
            return Ok(response.text());
        }

        let mut tool_results = Vec::new();
        for call in tool_calls {
            let args: Value = serde_json::from_str(&call.arguments)?;
            println!("  Tool call: {}({})", call.name, args);
            let result = execute_tool(&call.name, &args);
            tool_results.push(Item::tool_result(call.call_id, result.to_string()));
        }

        response = session
            .process_request(
                Request::from_items(tool_results).with_options(RequestOptions {
                    search: SearchOptions {
                        max_output_tokens: Some(512),
                        ..Default::default()
                    },
                    tool_choice: Some(ToolChoice::None),
                    ..Default::default()
                }),
            )
            .await?;
    }
}

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    let manager = FoundryLocalManager::create(FoundryLocalConfig::new("tool-calling-app"))?;

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

    let model = manager.catalog().get_model("qwen2.5-0.5b").await?;
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

    model.load().await?;
    println!("Model loaded and ready.");

    let session = ChatSession::new(&model).await?;
    session
        .add_tool_definition(
            ToolDefinition::new(
                "get_weather",
                json!({
                    "type": "object",
                    "properties": {
                        "location": { "type": "string", "description": "The city or location" },
                        "unit": {
                            "type": "string",
                            "enum": ["celsius", "fahrenheit"],
                            "description": "Temperature unit"
                        }
                    },
                    "required": ["location"]
                })
                .to_string(),
            )
            .with_description("Get the current weather for a location"),
        )
        .await?;
    session
        .add_tool_definition(
            ToolDefinition::new(
                "calculate",
                json!({
                    "type": "object",
                    "properties": {
                        "expression": {
                            "type": "string",
                            "description": "The math expression to evaluate"
                        }
                    },
                    "required": ["expression"]
                })
                .to_string(),
            )
            .with_description("Perform a math calculation"),
        )
        .await?;

    println!("\nTool-calling assistant ready! Type 'quit' to exit.\n");

    let stdin = io::stdin();
    let mut first_turn = true;
    loop {
        print!("You: ");
        io::stdout().flush()?;

        let mut input = String::new();
        stdin.lock().read_line(&mut input)?;
        let input = input.trim();
        if input.eq_ignore_ascii_case("quit") || input.eq_ignore_ascii_case("exit") {
            break;
        }

        let mut items = Vec::new();
        if first_turn {
            items.push(Item::system_message(vec![Item::text(
                "You are a helpful assistant with access to tools. Use them when needed to answer questions accurately.",
            )]));
            first_turn = false;
        }
        items.push(Item::user_message(vec![Item::text(input)]));

        let answer = process_turn(&session, items).await?;
        println!("Assistant: {answer}\n");
    }

    drop(session);
    model.unload().await?;
    println!("Model unloaded. Goodbye!");
    Ok(())
}
// </complete_code>
