// <complete_code>
use foundry_local_sdk::{
    AudioSession, ChatSession, FoundryLocalConfig, FoundryLocalManager, Item, Request,
    RequestOptions, SearchOptions,
};
use std::io::{self, Write};

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    let manager = FoundryLocalManager::create(FoundryLocalConfig::new("note-taker"))?;

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

    let speech_model = manager.catalog().get_model("whisper-tiny").await?;
    if !speech_model.is_cached().await? {
        println!("Downloading speech model...");
        speech_model
            .download(Some(|progress: f64| {
                print!("\r  {progress:.1}%");
                io::stdout().flush().ok();
            }))
            .await?;
        println!();
    }
    speech_model.load().await?;
    println!("Speech model loaded.");

    let audio_session = AudioSession::new(&speech_model).await?;
    let transcription = audio_session
        .transcribe(Item::audio_uri("meeting-notes.wav", None::<String>, 0, 0))
        .await?;
    println!("\nTranscription:\n{}", transcription);
    drop(audio_session);
    speech_model.unload().await?;

    let chat_model = manager.catalog().get_model("qwen2.5-0.5b").await?;
    if !chat_model.is_cached().await? {
        println!("Downloading chat model...");
        chat_model
            .download(Some(|progress: f64| {
                print!("\r  {progress:.1}%");
                io::stdout().flush().ok();
            }))
            .await?;
        println!();
    }
    chat_model.load().await?;
    println!("Chat model loaded.");

    let chat_session = ChatSession::new(&chat_model).await?;
    chat_session
        .set_options(RequestOptions {
            search: SearchOptions {
                temperature: Some(0.7),
                max_output_tokens: Some(512),
                ..Default::default()
            },
            ..Default::default()
        })
        .await?;

    let response = chat_session
        .process_request(Request::from_items(vec![
            Item::system_message(vec![Item::text(
                "You are a note-taking assistant. Summarize the following transcription into organized, concise notes with bullet points.",
            )]),
            Item::user_message(vec![Item::text(transcription)]),
        ]))
        .await?;
    println!("\nSummary:\n{}", response.text());

    drop(chat_session);
    chat_model.unload().await?;
    println!("\nDone. Models unloaded.");
    Ok(())
}
// </complete_code>
