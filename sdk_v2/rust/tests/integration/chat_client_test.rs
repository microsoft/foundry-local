use super::common;
use foundry_local_sdk::openai::ChatClient;
use foundry_local_sdk::{
    ChatCompletionMessageToolCalls, ChatCompletionRequestMessage,
    ChatCompletionRequestSystemMessage, ChatCompletionRequestToolMessage,
    ChatCompletionRequestUserMessage, ChatToolChoice,
};
use serde_json::json;
use std::collections::BTreeMap;
use std::sync::Arc;
use tokio_stream::StreamExt;

async fn setup_chat_client() -> (ChatClient, Arc<foundry_local_sdk::Model>) {
    let manager = common::get_test_manager();
    let catalog = manager.catalog();
    let model = catalog
        .get_model(common::TEST_MODEL_ALIAS)
        .await
        .expect("get_model failed");
    model.load().await.expect("model.load() failed");

    let client = model.create_chat_client().max_tokens(500).temperature(0.0);
    (client, model)
}

fn user_message(content: &str) -> ChatCompletionRequestMessage {
    ChatCompletionRequestUserMessage::from(content).into()
}

fn system_message(content: &str) -> ChatCompletionRequestMessage {
    ChatCompletionRequestSystemMessage::from(content).into()
}

fn assistant_message(content: &str) -> ChatCompletionRequestMessage {
    serde_json::from_value(json!({ "role": "assistant", "content": content }))
        .expect("failed to construct assistant message")
}

#[tokio::test]
async fn should_perform_chat_completion() {
    let (client, model) = setup_chat_client().await;
    let messages = vec![
        system_message("You are a helpful math assistant. Respond with just the answer."),
        user_message("What is 7*6?"),
    ];

    let response = client
        .complete_chat(&messages, None)
        .await
        .expect("complete_chat failed");
    let content = response
        .choices
        .first()
        .and_then(|c| c.message.content.as_deref())
        .unwrap_or("");
    println!("Response: {content}");

    assert!(
        content.contains("42"),
        "Expected response to contain '42', got: {content}"
    );

    model.unload().await.expect("model.unload() failed");
}

#[tokio::test]
async fn should_perform_streaming_chat_completion() {
    let (client, model) = setup_chat_client().await;
    let mut messages = vec![
        system_message("You are a helpful math assistant. Respond with just the answer."),
        user_message("What is 7*6?"),
    ];

    let mut first_result = String::new();
    let mut stream = client
        .complete_streaming_chat(&messages, None)
        .await
        .expect("streaming chat (first turn) setup failed");
    while let Some(chunk) = stream.next().await {
        let chunk = chunk.expect("stream chunk error");
        if let Some(choice) = chunk.choices.first() {
            if let Some(ref content) = choice.delta.content {
                first_result.push_str(content);
            }
        }
    }
    println!("First turn: {first_result}");

    assert!(
        first_result.contains("42"),
        "First turn should contain '42', got: {first_result}"
    );

    messages.push(assistant_message(&first_result));
    messages.push(user_message("Now add 25 to that result."));

    let mut second_result = String::new();
    let mut stream = client
        .complete_streaming_chat(&messages, None)
        .await
        .expect("streaming chat (follow-up) setup failed");
    while let Some(chunk) = stream.next().await {
        let chunk = chunk.expect("stream chunk error");
        if let Some(choice) = chunk.choices.first() {
            if let Some(ref content) = choice.delta.content {
                second_result.push_str(content);
            }
        }
    }
    println!("Follow-up: {second_result}");

    assert!(
        second_result.contains("67"),
        "Follow-up should contain '67', got: {second_result}"
    );

    model.unload().await.expect("model.unload() failed");
}

#[tokio::test]
async fn should_throw_when_completing_chat_with_empty_messages() {
    let (client, model) = setup_chat_client().await;
    let messages: Vec<ChatCompletionRequestMessage> = vec![];

    let result = client.complete_chat(&messages, None).await;
    assert!(result.is_err(), "Expected error for empty messages");

    model.unload().await.expect("model.unload() failed");
}

#[tokio::test]
async fn should_throw_when_completing_streaming_chat_with_empty_messages() {
    let (client, model) = setup_chat_client().await;
    let messages: Vec<ChatCompletionRequestMessage> = vec![];

    let result = client.complete_streaming_chat(&messages, None).await;
    assert!(
        result.is_err(),
        "Expected error for empty messages in streaming"
    );

    model.unload().await.expect("model.unload() failed");
}

// Note: The "invalid callback" test was removed because it was an exact
// duplicate of should_throw_when_completing_streaming_chat_with_empty_messages.

#[tokio::test]
async fn should_perform_tool_calling_chat_completion_non_streaming() {
    let (client, model) = setup_chat_client().await;
    let client = client.tool_choice(ChatToolChoice::Required);

    let tools = vec![common::get_multiply_tool()];
    let mut messages = vec![
        system_message("You are a math assistant. Use the multiply tool to answer."),
        user_message("What is 6 times 7?"),
    ];

    let response = client
        .complete_chat(&messages, Some(&tools))
        .await
        .expect("complete_chat with tools failed");

    let choice = response
        .choices
        .first()
        .expect("Expected at least one choice");
    let tool_calls = choice
        .message
        .tool_calls
        .as_ref()
        .expect("Expected tool_calls");
    assert!(
        !tool_calls.is_empty(),
        "Expected at least one tool call in the response"
    );

    let tool_call = match &tool_calls[0] {
        ChatCompletionMessageToolCalls::Function(tc) => tc,
        _ => panic!("Expected a function tool call"),
    };
    assert_eq!(
        tool_call.function.name, "multiply",
        "Expected tool call to 'multiply'"
    );

    let args: serde_json::Value = serde_json::from_str(&tool_call.function.arguments)
        .expect("Failed to parse tool call arguments");
    let a = args["a"].as_f64().unwrap_or(0.0);
    let b = args["b"].as_f64().unwrap_or(0.0);
    let product = (a * b) as i64;

    let tool_call_id = &tool_call.id;
    let assistant_msg: ChatCompletionRequestMessage = serde_json::from_value(json!({
        "role": "assistant",
        "content": null,
        "tool_calls": [{
            "id": tool_call_id,
            "type": "function",
            "function": {
                "name": tool_call.function.name,
                "arguments": tool_call.function.arguments,
            }
        }]
    }))
    .expect("failed to construct assistant message");
    messages.push(assistant_msg);
    messages.push(
        ChatCompletionRequestToolMessage {
            content: product.to_string().into(),
            tool_call_id: tool_call_id.clone(),
        }
        .into(),
    );
    messages.push(system_message(
        "Respond only with the answer generated by the tool. Do not call any tools.",
    ));

    let client = client.tool_choice(ChatToolChoice::None);

    let final_response = client
        .complete_chat(&messages, Some(&tools))
        .await
        .expect("follow-up complete_chat with tools failed");
    let content = final_response
        .choices
        .first()
        .and_then(|c| c.message.content.as_deref())
        .unwrap_or("");

    println!("Tool call result: {content}");

    assert!(
        content.contains("42"),
        "Final answer should contain '42', got: {content}"
    );

    model.unload().await.expect("model.unload() failed");
}

#[tokio::test]
async fn should_perform_tool_calling_chat_completion_streaming() {
    let (client, model) = setup_chat_client().await;
    let client = client.tool_choice(ChatToolChoice::Required);

    #[derive(Default)]
    struct StreamedToolCall {
        id: String,
        name: String,
        arguments: String,
    }

    let tools = vec![common::get_multiply_tool()];
    let mut messages = vec![
        system_message("You are a math assistant. Use the multiply tool to answer."),
        user_message("What is 6 times 7?"),
    ];

    let mut streamed_tool_calls = BTreeMap::<u32, StreamedToolCall>::new();

    let mut stream = client
        .complete_streaming_chat(&messages, Some(&tools))
        .await
        .expect("streaming tool call setup failed");

    while let Some(chunk) = stream.next().await {
        let chunk = chunk.expect("stream chunk error");
        if let Some(choice) = chunk.choices.first() {
            if let Some(ref tool_calls) = choice.delta.tool_calls {
                for call in tool_calls {
                    let accumulated = streamed_tool_calls.entry(call.index).or_default();
                    if let Some(ref func) = call.function {
                        if let Some(ref name) = func.name {
                            accumulated.name.push_str(name);
                        }
                        if let Some(ref args) = func.arguments {
                            accumulated.arguments.push_str(args);
                        }
                    }
                    if let Some(ref id) = call.id {
                        accumulated.id.push_str(id);
                    }
                }
            }
        }
    }

    assert!(
        !streamed_tool_calls.is_empty(),
        "Expected at least one streamed tool call"
    );

    for call in streamed_tool_calls.values() {
        assert_eq!(
            call.name, "multiply",
            "Expected streamed tool call to 'multiply'"
        );
        let args: serde_json::Value = serde_json::from_str(&call.arguments)
            .expect("streamed tool arguments should be valid JSON");
        let a = args["a"]
            .as_f64()
            .expect("multiply argument 'a' should be numeric");
        let b = args["b"]
            .as_f64()
            .expect("multiply argument 'b' should be numeric");
        assert_eq!(
            (a * b) as i64,
            42,
            "streamed multiply arguments should produce 42"
        );
    }

    let assistant_tool_calls = streamed_tool_calls
        .values()
        .map(|call| {
            json!({
                "id": call.id,
                "type": "function",
                "function": {
                    "name": call.name,
                    "arguments": call.arguments
                }
            })
        })
        .collect::<Vec<_>>();
    let assistant_msg: ChatCompletionRequestMessage = serde_json::from_value(json!({
        "role": "assistant",
        "tool_calls": assistant_tool_calls
    }))
    .expect("failed to construct assistant message");
    messages.push(assistant_msg);
    for call in streamed_tool_calls.values() {
        messages.push(
            ChatCompletionRequestToolMessage {
                content: "42".into(),
                tool_call_id: call.id.clone(),
            }
            .into(),
        );
    }
    messages.push(system_message(
        "Respond only with the answer generated by the tool. Do not call any tools.",
    ));

    let client = client.tool_choice(ChatToolChoice::None);

    let mut final_result = String::new();
    let mut stream = client
        .complete_streaming_chat(&messages, Some(&tools))
        .await
        .expect("streaming follow-up setup failed");
    while let Some(chunk) = stream.next().await {
        let chunk = chunk.expect("stream chunk error");
        if let Some(choice) = chunk.choices.first() {
            if let Some(ref content) = choice.delta.content {
                final_result.push_str(content);
            }
        }
    }
    println!("Streamed tool call result: {final_result}");

    assert!(
        final_result.contains("42"),
        "Streamed final answer should contain '42', got: {final_result}"
    );

    model.unload().await.expect("model.unload() failed");
}
