// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// Foundry Local SDK - BYOM local registration E2E Demo
//
// Demonstrates the supported v2 BYOM flow:
//   1. Resolve/download a small catalog model.
//   2. Clone its model directory under a new location.
//   3. Explicitly register that directory in the LOCAL catalog.
//   4. Load it, run a typed ChatSession request, unload it.
//   5. Unregister the local model and delete the clone.

use std::error::Error;
use std::fs;
use std::io::{self, Write};
use std::path::{Path, PathBuf};
use std::time::{SystemTime, UNIX_EPOCH};

use foundry_local_sdk::{ChatSession, FoundryLocalConfig, FoundryLocalManager, Item, Request};

const PASS: &str = "\x1b[92m[PASS]\x1b[0m";
const INFO: &str = "\x1b[94m[INFO]\x1b[0m";
const SOURCE_ALIAS: &str = "qwen2.5-0.5b";
const BYOM_SUBDIR: &str = "byom_local_registration";
const PROMPT: &str = "What is the golden ratio? Answer in one short sentence.";

fn section(title: &str) {
    println!("\n{}\n  {title}\n{}", "=".repeat(72), "=".repeat(72));
}

fn copy_tree_with_hardlinks(src: &Path, dst: &Path) -> io::Result<()> {
    fs::create_dir_all(dst)?;
    for entry in fs::read_dir(src)? {
        let entry = entry?;
        let file_type = entry.file_type()?;
        let src_entry = entry.path();
        let dst_entry = dst.join(entry.file_name());
        if file_type.is_dir() {
            copy_tree_with_hardlinks(&src_entry, &dst_entry)?;
        } else if fs::hard_link(&src_entry, &dst_entry).is_err() {
            fs::copy(&src_entry, &dst_entry)?;
        }
    }
    Ok(())
}

fn clone_model_dir(src: &Path, dst: &Path, new_name: &str) -> Result<(), Box<dyn Error>> {
    copy_tree_with_hardlinks(src, dst)?;
    let src_inf = src.join("inference_model.json");
    let raw = fs::read_to_string(&src_inf)?;
    let mut value: serde_json::Value = serde_json::from_str(&raw)?;
    value["Name"] = serde_json::Value::String(new_name.to_string());

    let dst_inf = dst.join("inference_model.json");
    fs::remove_file(&dst_inf)?;
    fs::write(&dst_inf, serde_json::to_string(&value)?)?;
    Ok(())
}

#[tokio::main]
async fn main() -> Result<(), Box<dyn Error>> {
    let ts = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0);
    let byom_alias = format!("fl-local-byom-{ts}");
    let byom_model_id = format!("{byom_alias}:1");
    println!("{INFO} BYOM alias for this run: {byom_alias}");

    let manager = FoundryLocalManager::create(FoundryLocalConfig::new(
        "foundry_local_byom_local_registration",
    ))?;

    section("Step 1: Register execution providers");
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
                print!("\r  EP [{ep_name:<24}] {percent:6.2}%");
                io::stdout().flush().ok();
            }
        })
        .await?;
    println!();

    section(&format!(
        "Step 2: Resolve source catalog model '{SOURCE_ALIAS}'"
    ));
    let source = manager.catalog().get_model(SOURCE_ALIAS).await?;
    let mut cpu_variant = None;
    for variant in source.variants() {
        let info = variant.info()?;
        if info
            .runtime
            .as_ref()
            .is_some_and(|runtime| runtime.execution_provider == "CPUExecutionProvider")
        {
            cpu_variant = Some(variant);
            break;
        }
    }
    let cpu_variant =
        cpu_variant.ok_or("The BYOM demo requires a CPU execution-provider variant.")?;
    source.select_variant(&cpu_variant)?;
    println!("  resolved to variant id: {}", source.id());

    if !source.is_cached().await? {
        println!("  {INFO} source not cached; downloading...");
        source
            .download(Some(|progress: f64| {
                print!("\r  download: {progress:6.2}%");
                io::stdout().flush().ok();
            }))
            .await?;
        println!();
    }

    let src_path: PathBuf = source.path().await?;
    let publisher_dir = src_path
        .parent()
        .and_then(|p| p.parent())
        .ok_or("Cannot resolve publisher dir from source path")?
        .to_path_buf();
    let group_dir = publisher_dir.join(BYOM_SUBDIR);
    let clone_dir = group_dir.join(&byom_alias);

    section("Step 3: Clone source model directory");
    println!("  cloning -> {}", clone_dir.display());
    clone_model_dir(&src_path, &clone_dir, &byom_alias)?;

    let local_catalog = manager.local_catalog()?;
    let mut metadata = local_catalog.create_model_info()?;
    metadata
        .set_string_property("task", "chat-completion")?
        .set_string_property(
            "display_name",
            &format!("{byom_alias} local registration demo"),
        )?;

    let mut registered = None;
    let result: Result<(), Box<dyn Error>> = async {
        section("Step 4: Register BYOM in local catalog");
        let model = local_catalog
            .register_model(
                clone_dir.to_str().ok_or("Clone path is not valid UTF-8")?,
                &byom_model_id,
                metadata,
            )
            .await?;
        println!("  {PASS} registered {}", model.id());
        registered = Some(model);

        section("Step 5: Load and run typed ChatSession");
        let model = registered.as_ref().unwrap();
        model.load().await?;
        let session = ChatSession::new(model).await?;
        let response = session
            .process_request(Request::from_items(vec![Item::user_message(vec![
                Item::text(PROMPT),
            ])]))
            .await?;
        println!("  prompt: {PROMPT}");
        println!("  assistant: {}", response.text());
        drop(session);

        section("Step 6: Unload and unregister");
        model.unload().await?;
        local_catalog.unregister_model(&byom_model_id).await?;
        registered = None;
        println!("  {PASS} unregistered {byom_model_id}");
        Ok(())
    }
    .await;

    if let Some(model) = registered {
        model.unload().await?;
        local_catalog.unregister_model(&byom_model_id).await?;
    }
    fs::remove_dir_all(&clone_dir)?;
    if let Err(error) = fs::remove_dir(&group_dir) {
        if error.kind() != io::ErrorKind::DirectoryNotEmpty {
            return Err(error.into());
        }
    }

    result
}
