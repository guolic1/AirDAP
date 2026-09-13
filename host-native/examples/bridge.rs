use airdap_service::{credential::Credential, usbip};
use clap::Parser;
use std::{
    path::PathBuf,
    sync::{Arc, atomic::AtomicBool},
};
use tokio::{net::TcpListener, sync::watch};
#[derive(Parser)]
struct Args {
    host: String,
    #[arg(long)]
    credential: PathBuf,
    #[arg(long, default_value_t = 3242)]
    port: u16,
    #[arg(long, default_value_t = 3260)]
    dap_port: u16,
    #[arg(long, default_value_t = 3261)]
    uart_port: u16,
}
#[tokio::main(flavor = "current_thread")]
async fn main() -> anyhow::Result<()> {
    let args = Args::parse();
    let listener = TcpListener::bind(("127.0.0.1", args.port)).await?;
    let credential = Credential::load(&args.credential)?;
    let (stop, rx) = watch::channel(false);
    tokio::spawn(async move {
        if tokio::signal::ctrl_c().await.is_ok() {
            let _ = stop.send(true);
        }
    });
    usbip::serve(
        listener,
        args.host,
        credential,
        [args.dap_port, args.uart_port],
        Arc::new(AtomicBool::new(false)),
        rx,
    )
    .await
}
