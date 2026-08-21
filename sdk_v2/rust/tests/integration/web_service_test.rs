use super::common;
use serde_json::json;

/// Start the web service, make a non-streaming POST to v1/chat/completions,
/// verify we get a valid response, then stop the service.
#[tokio::test]
async fn should_complete_chat_via_rest_api() {
    let _ws = common::web_service_guard().await;
    let manager = common::get_test_manager();
    let catalog = manager.catalog();
    let model = catalog
        .get_model(common::TEST_MODEL_ALIAS)
        .await
        .expect("get_model failed");
    model.load().await.expect("model.load() failed");

    manager
        .start_web_service()
        .await
        .expect("start_web_service failed");
    let urls = manager.urls().expect("urls() should succeed");
    let base_url = urls.first().expect("no URL returned").trim_end_matches('/');

    let client = reqwest::Client::new();
    let resp = client
        .post(format!("{base_url}/v1/chat/completions"))
        .json(&json!({
            "model": model.id(),
            "messages": [
                { "role": "system", "content": "You are a helpful math assistant. Respond with just the answer." },
                { "role": "user", "content": "What is 7*6?" }
            ],
            "max_tokens": 500,
            "temperature": 0.0,
            "stream": false
        }))
        .send()
        .await
        .expect("HTTP request failed");

    assert!(
        resp.status().is_success(),
        "Expected 2xx, got {}",
        resp.status()
    );

    let body: serde_json::Value = resp.json().await.expect("failed to parse response JSON");
    let content = body
        .pointer("/choices/0/message/content")
        .and_then(|v| v.as_str())
        .unwrap_or("");

    println!("REST response: {content}");

    assert!(
        content.contains("42"),
        "Expected response to contain '42', got: {content}"
    );

    manager
        .stop_web_service()
        .await
        .expect("stop_web_service failed");
    model.unload().await.expect("model.unload() failed");
}

/// Start the web service, make a streaming POST to v1/chat/completions,
/// collect SSE chunks, verify we get a valid streamed response.
#[tokio::test]
async fn should_stream_chat_via_rest_api() {
    let _ws = common::web_service_guard().await;
    let manager = common::get_test_manager();
    let catalog = manager.catalog();
    let model = catalog
        .get_model(common::TEST_MODEL_ALIAS)
        .await
        .expect("get_model failed");
    model.load().await.expect("model.load() failed");

    manager
        .start_web_service()
        .await
        .expect("start_web_service failed");
    let urls = manager.urls().expect("urls() should succeed");
    let base_url = urls.first().expect("no URL returned").trim_end_matches('/');

    let client = reqwest::Client::new();
    let mut response = client
        .post(format!("{base_url}/v1/chat/completions"))
        .json(&json!({
            "model": model.id(),
            "messages": [
                { "role": "system", "content": "You are a helpful math assistant. Respond with just the answer." },
                { "role": "user", "content": "What is 7*6?" }
            ],
            "max_tokens": 500,
            "temperature": 0.0,
            "stream": true
        }))
        .send()
        .await
        .expect("HTTP request failed");

    assert!(
        response.status().is_success(),
        "Expected 2xx, got {}",
        response.status()
    );

    // SSE chunks from `response.chunk()` do not align with line or UTF-8
    // boundaries, so buffer raw bytes and parse only complete `\n`-terminated
    // lines, keeping any trailing partial line for the next chunk.
    let mut full_text = String::new();
    let mut buf: Vec<u8> = Vec::new();
    let mut done = false;
    while !done {
        let Some(chunk) = response.chunk().await.expect("chunk read failed") else {
            break;
        };
        buf.extend_from_slice(&chunk);
        while let Some(nl) = buf.iter().position(|&b| b == b'\n') {
            let line_bytes: Vec<u8> = buf.drain(..=nl).collect();
            let line = String::from_utf8_lossy(&line_bytes);
            let line = line.trim();
            if let Some(data) = line.strip_prefix("data:") {
                let data = data.trim();
                if data == "[DONE]" {
                    done = true;
                    break;
                }
                if let Ok(parsed) = serde_json::from_str::<serde_json::Value>(data) {
                    if let Some(content) = parsed
                        .pointer("/choices/0/delta/content")
                        .and_then(|v| v.as_str())
                    {
                        full_text.push_str(content);
                    }
                }
            }
        }
    }

    println!("REST streamed response: {full_text}");

    assert!(
        full_text.contains("42"),
        "Expected streamed response to contain '42', got: {full_text}"
    );

    manager
        .stop_web_service()
        .await
        .expect("stop_web_service failed");
    model.unload().await.expect("model.unload() failed");
}

/// urls() should return the listening addresses after start_web_service.
#[tokio::test]
async fn should_expose_urls_after_start() {
    let _ws = common::web_service_guard().await;
    let manager = common::get_test_manager();

    manager
        .start_web_service()
        .await
        .expect("start_web_service failed");

    let urls = manager.urls().expect("urls() should succeed");
    println!("Web service URLs: {urls:?}");
    assert!(!urls.is_empty(), "urls() should return URLs after start");

    manager
        .stop_web_service()
        .await
        .expect("stop_web_service failed");
}
