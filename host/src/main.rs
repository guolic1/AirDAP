use airdap_service::{service::Service, store::Store, web};
use anyhow::{Result, ensure};
use clap::Parser;
use std::{path::PathBuf, time::Duration};
use tokio::sync::watch;

#[derive(Parser, Clone)]
#[command(version, about = "AirDAP 原生 USB/IP 与本机设备管理服务")]
struct Args {
    #[arg(long)]
    data_dir: Option<PathBuf>,
    #[arg(long, default_value_t = 8080)]
    http_port: u16,
    #[arg(long, default_value_t = 3242)]
    usbip_port: u16,
    #[arg(long)]
    no_http: bool,
    #[arg(long)]
    usbip_executable: Option<PathBuf>,
    #[arg(long)]
    windows_service: bool,
    #[arg(long, default_value = "AirDAP")]
    service_name: String,
}
fn main() -> Result<()> {
    let args = Args::parse();
    ensure!(
        args.http_port > 0 && args.usbip_port > 0,
        "端口必须为 1..65535"
    );
    if args.windows_service {
        #[cfg(windows)]
        return scm::dispatch(&args.service_name);
        #[cfg(not(windows))]
        anyhow::bail!("--windows-service 仅适用于 Windows");
    }
    let (stop, rx) = watch::channel(false);
    runtime()?.block_on(async move {
        tokio::spawn(async move {
            #[cfg(unix)]
            {
                let mut term =
                    tokio::signal::unix::signal(tokio::signal::unix::SignalKind::terminate())
                        .expect("SIGTERM handler");
                tokio::select! {_ = tokio::signal::ctrl_c() => {}, _ = term.recv() => {}}
            }
            #[cfg(not(unix))]
            {
                let _ = tokio::signal::ctrl_c().await;
            }
            let _ = stop.send(true);
        });
        run(args, rx, || Ok(())).await
    })
}
fn runtime() -> Result<tokio::runtime::Runtime> {
    Ok(tokio::runtime::Builder::new_multi_thread()
        .worker_threads(2)
        .max_blocking_threads(4)
        .enable_all()
        .build()?)
}
async fn run(
    args: Args,
    mut stop: watch::Receiver<bool>,
    ready: impl FnOnce() -> Result<()>,
) -> Result<()> {
    let path = args.data_dir.unwrap_or_else(|| {
        let root = std::env::var_os(if cfg!(windows) {
            "LOCALAPPDATA"
        } else {
            "XDG_DATA_HOME"
        })
        .map(PathBuf::from)
        .unwrap_or_else(|| {
            PathBuf::from(std::env::var_os("HOME").unwrap_or_else(|| ".".into()))
                .join(".local/share")
        });
        root.join("AirDAP/service")
    });
    let service = Service::new(Store::open(&path)?, args.usbip_port, args.usbip_executable)?;
    let (http_stop, http_rx) = watch::channel(false);
    let web_task = if args.no_http {
        None
    } else {
        let listener = tokio::net::TcpListener::bind(("127.0.0.1", args.http_port)).await?;
        let router = web::router(service.clone(), args.http_port)?;
        eprintln!("AirDAP Web: http://airdap.localhost:{}", args.http_port);
        Some(tokio::spawn(async move {
            let mut rx = http_rx;
            axum::serve(listener, router)
                .with_graceful_shutdown(async move {
                    let _ = rx.wait_for(|v| *v).await;
                })
                .await
        }))
    };
    let maintenance = tokio::spawn(service.clone().maintain());
    service.lifecycle("started");
    let ready_result = ready();
    if ready_result.is_ok() {
        let _ = stop.wait_for(|v| *v).await;
    }
    service.begin_close();
    let _ = http_stop.send(true);
    // Do not abort maintenance or the active job: they may own an in-flight device write.
    let close = service.close().await;
    maintenance.await?;
    if let Some(task) = web_task {
        match tokio::time::timeout(Duration::from_secs(12), task).await {
            Ok(result) => {
                result??;
            }
            Err(_) => eprintln!("HTTP 客户端关闭超时，设备操作已结束"),
        }
    }
    ready_result?;
    service.lifecycle(if close.is_ok() {
        "stopped"
    } else {
        "cleanup-failed"
    });
    close
}

#[cfg(windows)]
mod scm {
    use super::*;
    use std::{
        ffi::OsString,
        sync::{
            Arc,
            atomic::{AtomicBool, Ordering},
        },
    };
    use windows_service::{
        define_windows_service,
        service::{
            ServiceControl, ServiceControlAccept, ServiceExitCode, ServiceState, ServiceStatus,
            ServiceType,
        },
        service_control_handler::{self, ServiceControlHandlerResult},
        service_dispatcher,
    };
    define_windows_service!(entry, service_main);
    pub fn dispatch(name: &str) -> Result<()> {
        service_dispatcher::start(name, entry)?;
        Ok(())
    }
    fn status(state: ServiceState, checkpoint: u32, error: u32) -> ServiceStatus {
        ServiceStatus {
            service_type: ServiceType::OWN_PROCESS,
            current_state: state,
            controls_accepted: if state == ServiceState::Running {
                ServiceControlAccept::STOP | ServiceControlAccept::SHUTDOWN
            } else {
                ServiceControlAccept::empty()
            },
            exit_code: ServiceExitCode::Win32(error),
            checkpoint,
            wait_hint: if matches!(
                state,
                ServiceState::StartPending | ServiceState::StopPending
            ) {
                Duration::from_secs(30)
            } else {
                Duration::ZERO
            },
            process_id: None,
        }
    }
    fn service_main(_: Vec<OsString>) {
        let args = Args::parse();
        let (stop, rx) = watch::channel(false);
        let stopping = Arc::new(AtomicBool::new(false));
        let flag = stopping.clone();
        let handle =
            match service_control_handler::register(
                &args.service_name,
                move |control| match control {
                    ServiceControl::Stop | ServiceControl::Shutdown => {
                        flag.store(true, Ordering::Release);
                        let _ = stop.send(true);
                        ServiceControlHandlerResult::NoError
                    }
                    ServiceControl::Interrogate => ServiceControlHandlerResult::NoError,
                    _ => ServiceControlHandlerResult::NotImplemented,
                },
            ) {
                Ok(h) => h,
                Err(_) => return,
            };
        if handle
            .set_service_status(status(ServiceState::StartPending, 1, 0))
            .is_err()
        {
            return;
        }
        let result = runtime().and_then(|rt| {
            rt.block_on(async {
                let monitor = tokio::spawn(async move {
                    let mut checkpoint = 1;
                    loop {
                        tokio::time::sleep(Duration::from_secs(5)).await;
                        if stopping.load(Ordering::Acquire) {
                            checkpoint += 1;
                            let _ = handle.set_service_status(status(
                                ServiceState::StopPending,
                                checkpoint,
                                0,
                            ));
                        }
                    }
                });
                let result = run(args, rx, || {
                    handle.set_service_status(status(ServiceState::Running, 0, 0))?;
                    Ok(())
                })
                .await;
                monitor.abort();
                result
            })
        });
        let _ =
            handle.set_service_status(status(ServiceState::Stopped, 0, u32::from(result.is_err())));
    }
}
