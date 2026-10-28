use std::path::PathBuf;

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    let mut args = std::env::args_os().skip(1);
    if args.next().as_deref() != Some(std::ffi::OsStr::new("--config")) {
        return Err("usage: foundry-local-host --config <SDK-generated-config.json>".into());
    }
    let path = args
        .next()
        .map(PathBuf::from)
        .ok_or("missing host configuration")?;
    if args.next().is_some() {
        return Err("unexpected host argument".into());
    }
    foundry_local_sdk::shared_service::run_host(&path).await?;
    Ok(())
}
