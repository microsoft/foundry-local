// <complete_code>
use foundry_local_sdk::{
    ChatSession, FoundryLocalConfig, FoundryLocalManager, Item, Request, RequestOptions,
    SearchOptions, TextKind,
};
use std::io::{self, BufRead, Write};
use tokio_stream::StreamExt;

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    let manager = FoundryLocalManager::create(FoundryLocalConfig::new("chat-assistant"))?;

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
        .set_options(RequestOptions {
            search: SearchOptions {
                temperature: Some(0.7),
                max_output_tokens: Some(512),
                ..Default::default()
            },
            ..Default::default()
        })
        .await?;

    println!("\nChat assistant ready! Type 'quit' to exit.\n");

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

        print!("Assistant: ");
        io::stdout().flush()?;
        let mut items = Vec::new();
        if first_turn {
            items.push(Item::system_message(vec![Item::text(
                "You are a helpful, friendly assistant. Keep your responses concise and conversational. If you don't know something, say so.",
            )]));
            first_turn = false;
        }
        items.push(Item::user_message(vec![Item::text(input)]));

        let mut stream = session.process_streaming_request(Request::from_items(items));
        while let Some(item) = stream.next().await {
            if let foundry_local_sdk::Item::Text {
                text,
                kind: TextKind::Default,
            } = item?
            {
                print!("{text}");
                io::stdout().flush()?;
            }
        }
        println!("\n");
    }

    drop(session);
    model.unload().await?;
    println!("Model unloaded. Goodbye!");
    Ok(())
}
// </complete_code>
