use airdap_service::{credential::Credential, network::Client};
use clap::Parser;
use std::{path::PathBuf, time::Duration};

#[derive(Parser)]
struct Args {
    host: String,
    #[arg(long)]
    credential: PathBuf,
    #[arg(long, default_value_t = 3260)]
    port: u16,
}

#[tokio::main(flavor = "current_thread")]
async fn main() -> anyhow::Result<()> {
    let args = Args::parse();
    let credential = Credential::load(&args.credential)?;
    // HELLO only: does not acquire the existing USB bridge's device ownership.
    let client = Client::connect(
        &args.host,
        args.port,
        &credential,
        None,
        Duration::from_secs(5),
    )
    .await?;
    println!("{}", serde_json::to_string_pretty(&client.hello)?);
    Ok(())
}
