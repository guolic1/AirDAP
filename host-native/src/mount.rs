use anyhow::{Context, Result, ensure};
use std::{path::PathBuf, time::Duration};
use tokio::process::Command;

pub struct Mount {
    pub executable: Option<PathBuf>,
    pub port: u16,
    pub number: Option<u32>,
}
impl Mount {
    pub fn new(port: u16, executable: Option<PathBuf>) -> Self {
        let executable = executable
            .or_else(|| {
                let name = if cfg!(windows) { "usbip.exe" } else { "usbip" };
                std::env::split_paths(&std::env::var_os("PATH").unwrap_or_default())
                    .map(|p| p.join(name))
                    .find(|p| p.is_file())
            })
            .or_else(|| {
                if cfg!(windows) {
                    let p = PathBuf::from(
                        std::env::var_os("ProgramFiles")
                            .unwrap_or_else(|| "C:/Program Files".into()),
                    )
                    .join("USBip/usbip.exe");
                    p.is_file().then_some(p)
                } else {
                    None
                }
            });
        Self {
            executable,
            port,
            number: None,
        }
    }
    async fn run(&self, args: &[&str]) -> Result<String> {
        let exe = self
            .executable
            .as_ref()
            .context("未找到 USB/IP 客户端，请安装 usbip-win2 或 Linux usbip/vhci_hcd")?;
        let mut command = Command::new(exe);
        command.args(args).kill_on_drop(true);
        #[cfg(windows)]
        command.creation_flags(0x08000000);
        let output = tokio::time::timeout(Duration::from_secs(20), command.output())
            .await
            .context("USB/IP 客户端操作超时")??;
        ensure!(
            output.status.success(),
            "USB/IP 客户端操作失败，请检查驱动和权限"
        );
        Ok(String::from_utf8_lossy(&output.stdout).into())
    }
    async fn matching(&self) -> Result<Vec<u32>> {
        Ok(matching_ports(&self.run(&["port"]).await?, self.port))
    }
    pub async fn detach(&mut self) -> Result<()> {
        for number in self.matching().await? {
            if let Err(error) = self.run(&["detach", "-p", &number.to_string()]).await {
                // Closing the export can make VHCI remove it before the detach command.
                // Only accept the failed command after proving this export is gone.
                if self.matching().await?.contains(&number) {
                    return Err(error);
                }
            }
        }
        self.number = None;
        Ok(())
    }
    pub async fn attach(&mut self) -> Result<()> {
        self.detach().await?;
        let port = self.port.to_string();
        let mut args = vec!["-t", &port, "attach", "-r", "127.0.0.1", "-b", "1-1"];
        if cfg!(windows) {
            args.extend(["--once", "--terse"]);
        }
        let output = self.run(&args).await?;
        if cfg!(windows)
            && let Ok(number) = output.trim().parse()
        {
            self.number = Some(number);
            return Ok(());
        }
        let numbers = self.matching().await?;
        ensure!(numbers.len() == 1, "无法确认本服务的 USB/IP 挂载端口");
        self.number = Some(numbers[0]);
        Ok(())
    }
}
fn matching_ports(text: &str, port: u16) -> Vec<u32> {
    let endpoint = format!("usbip://127.0.0.1:{port}/1-1");
    let mut current = None;
    let mut found = Vec::new();
    for line in text.lines() {
        if let Some(rest) = line.strip_prefix("Port ") {
            current = rest.split_once(':').and_then(|(n, _)| n.parse().ok());
        }
        if line.trim().strip_prefix("->").map(str::trim) == Some(endpoint.as_str())
            && let Some(n) = current.take()
        {
            found.push(n);
        }
    }
    found
}
#[cfg(test)]
mod tests {
    use super::*;
    #[cfg(unix)]
    #[tokio::test]
    async fn detach_accepts_already_removed_port_but_preserves_real_failures() {
        use std::os::unix::fs::PermissionsExt;
        for removed in [true, false] {
            let dir = tempfile::tempdir().unwrap();
            let executable = dir.path().join("usbip");
            std::fs::write(
                &executable,
                format!(
                    r##"#!/bin/sh
state="$(dirname "$0")/removed"
case "$1" in
port) if [ ! -f "$state" ]; then printf 'Port 01: active\n -> usbip://127.0.0.1:3242/1-1\n'; fi;;
detach) {} ; exit 1;;
esac
"##,
                    if removed { "touch \"$state\"" } else { ":" }
                ),
            )
            .unwrap();
            std::fs::set_permissions(&executable, std::fs::Permissions::from_mode(0o700)).unwrap();
            let mut mount = Mount::new(3242, Some(executable));
            mount.number = Some(1);
            assert_eq!(mount.detach().await.is_ok(), removed);
            assert_eq!(mount.number, if removed { None } else { Some(1) });
        }
    }
    #[test]
    fn detach_only_exact_export() {
        let text = "Port 1: active\n -> usbip://127.0.0.1:3242/1-1\nPort 2: active\n -> usbip://127.0.0.1:3243/1-1\nPort 3: active\n -> usbip://127.0.0.1:3242/1-10\n";
        assert_eq!(matching_ports(text, 3242), vec![1]);
    }
}
