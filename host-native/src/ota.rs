use crate::{credential::Credential, network::Client, usb::UsbLink};
use anyhow::{Context, Result, ensure};
use serde::Serialize;
use serde_json::{Value, json};
use std::{
    sync::Arc,
    time::{Duration, Instant},
};

pub type Progress = Arc<dyn Fn(u32, &str) + Send + Sync>;
#[derive(Debug, Clone, Serialize)]
pub struct ImageInfo {
    pub version: String,
    pub size: usize,
    pub sha256: String,
}
pub fn inspect(image: &[u8]) -> Result<ImageInfo> {
    ensure!(
        (80..=8 * 1024 * 1024).contains(&image.len())
            && image[0] == 0xe9
            && image[32..36] == 0xabcd5432u32.to_le_bytes()
            && image[12..14] == 9u16.to_le_bytes(),
        "请选择 ESP32-S3 的 ESP-IDF 应用镜像 airdap.bin"
    );
    let raw = image[48..80].split(|b| *b == 0).next().unwrap();
    let version = std::str::from_utf8(raw).context("应用版本格式无效")?;
    ensure!(!version.is_empty(), "应用镜像缺少版本");
    Ok(ImageInfo {
        version: version.into(),
        size: image.len(),
        sha256: hex::encode(openssl::sha::sha256(image)),
    })
}

#[derive(Debug, Serialize)]
pub struct OtaInfo {
    pub capacity: u32,
    pub running_address: u32,
    pub update_address: u32,
    pub confirmed: bool,
    pub version: String,
}
impl OtaInfo {
    pub fn parse(data: &[u8], hello_version: &str) -> Result<Self> {
        ensure!(
            (16..=47).contains(&data.len()) && data[0] == 1 && data[1] == 1 && data[14] <= 1,
            "OTA QUERY 格式或回滚能力无效"
        );
        let capacity = u32::from_be_bytes(data[2..6].try_into()?);
        let running_address = u32::from_be_bytes(data[6..10].try_into()?);
        let update_address = u32::from_be_bytes(data[10..14].try_into()?);
        ensure!(
            capacity == 0x3f0000
                && matches!(
                    (running_address, update_address),
                    (0x20000, 0x410000) | (0x410000, 0x20000)
                ),
            "OTA 布局与固件 A/B 分区约定不匹配"
        );
        let version = std::str::from_utf8(&data[15..])?.to_string();
        ensure!(version == hello_version, "OTA QUERY 与 HELLO 版本不匹配");
        Ok(Self {
            capacity,
            running_address,
            update_address,
            confirmed: data[14] == 1,
            version,
        })
    }
    pub fn verify(&self, before: &Self, version: &str) -> Result<()> {
        ensure!(
            self.running_address == before.update_address
                && self.running_address != before.running_address,
            "设备没有启动此前的非活动槽"
        );
        ensure!(
            self.version == version && self.confirmed,
            "设备启动版本不符或尚未确认，可能发生回滚"
        );
        Ok(())
    }
}
async fn command(client: &mut Client, opcode: u8, data: &[u8]) -> Result<Vec<u8>> {
    let result = client.control(opcode, data).await?;
    ensure!(!result.is_empty(), "OTA 响应缺少状态");
    ensure!(result[0] == 0, "OTA 操作失败，状态 {}", result[0]);
    Ok(result[1..].to_vec())
}
async fn empty(client: &mut Client, opcode: u8, data: &[u8]) -> Result<()> {
    ensure!(
        command(client, opcode, data).await?.is_empty(),
        "OTA 响应包含意外数据"
    );
    Ok(())
}
async fn query(client: &mut Client) -> Result<OtaInfo> {
    OtaInfo::parse(&command(client, 0x30, &[]).await?, &client.hello.firmware)
}

pub async fn network_upload(
    host: &str,
    credential: &Credential,
    image: &[u8],
    progress: Progress,
) -> Result<Value> {
    let meta = inspect(image)?;
    let mut client =
        Client::connect(host, 3260, credential, Some(&[]), Duration::from_secs(10)).await?;
    let before = query(&mut client).await?;
    ensure!(
        image.len() <= before.capacity as usize,
        "镜像超过非活动分区容量"
    );
    empty(&mut client, 0x31, &(image.len() as u32).to_be_bytes()).await?;
    let result = async {
        let mut offset = 0;
        for chunk in image.chunks(4091) {
            let mut request = (offset as u32).to_be_bytes().to_vec();
            request.extend_from_slice(chunk);
            let reply = command(&mut client, 0x32, &request).await?;
            offset += chunk.len();
            ensure!(
                reply == (offset as u32).to_be_bytes(),
                "OTA WRITE 偏移不匹配"
            );
            progress((offset * 90 / image.len()) as u32, "写入非活动分区");
        }
        empty(&mut client, 0x33, &[]).await
    }
    .await;
    if let Err(error) = result {
        let cleanup = empty(&mut client, 0x34, &[]).await;
        return Err(error.context(if cleanup.is_ok() {
            "OTA 失败，已中止"
        } else {
            "OTA 失败，中止未获确认；连接将关闭，禁止自动重放"
        }));
    }
    progress(92, "镜像已提交，等待重启与启动确认");
    // A reset can close TCP before its ACK. Success still requires a new authenticated
    // QUERY proving the previously inactive slot, expected version and confirmation.
    let reboot_ack = empty(&mut client, 0x35, &[]).await;
    drop(client);
    let deadline = tokio::time::Instant::now() + Duration::from_secs(45);
    let mut last = String::from("设备尚未重新连接");
    while tokio::time::Instant::now() < deadline {
        match Client::connect(host, 3260, credential, Some(&[]), Duration::from_secs(5)).await {
            Ok(mut client) => match query(&mut client).await {
                Ok(after) => match after.verify(&before, &meta.version) {
                    Ok(()) => {
                        return Ok(
                            json!({"version":after.version,"confirmed":true,"running_address":after.running_address}),
                        );
                    }
                    Err(error) => last = error.to_string(),
                },
                Err(error) => last = error.to_string(),
            },
            Err(error) => last = error.to_string(),
        }
        tokio::time::sleep(Duration::from_millis(250)).await;
    }
    anyhow::bail!(
        "镜像已提交但启动未验证：{last}；重启应答{}，请检查设备，不要重复写入",
        if reboot_ack.is_ok() {
            "已收到"
        } else {
            "未收到"
        }
    )
}

fn usb_query(link: &mut UsbLink) -> Result<(u32, String)> {
    let data = link.exchange(0x80, &[])?;
    ensure!(
        data.len() >= 7 && data[0] == 1 && data[1] & 1 != 0 && data.len() == 7 + data[6] as usize,
        "USB OTA QUERY 格式无效"
    );
    let version = std::str::from_utf8(&data[7..])?.to_string();
    ensure!(!version.is_empty(), "USB OTA 缺少版本");
    Ok((u32::from_le_bytes(data[2..6].try_into()?), version))
}
#[derive(Default)]
struct UsbReconnect {
    disconnected: bool,
    last_error: Option<String>,
}
impl UsbReconnect {
    fn observe(&mut self, observation: Result<Option<String>>, expected: &str) -> Result<bool> {
        match observation {
            Ok(None) => self.disconnected = true,
            Ok(Some(version)) if self.disconnected => {
                ensure!(
                    version == expected,
                    "USB 重连后的版本与镜像不符，可能发生回滚"
                );
                return Ok(true);
            }
            Ok(Some(_)) => {}
            Err(error) => self.last_error = Some(error.to_string()),
        }
        Ok(false)
    }
}
pub fn usb_upload(serial: &str, image: &[u8], progress: Progress) -> Result<Value> {
    let meta = inspect(image)?;
    let mut link = UsbLink::open(serial, false)?;
    let (capacity, _) = usb_query(&mut link)?;
    ensure!(
        image.len() <= capacity as usize,
        "镜像超过 USB OTA 非活动槽容量"
    );
    ensure!(link.exchange(3, &[])?.is_empty(), "DAP 断开确认无效");
    ensure!(
        link.exchange(0x81, &(image.len() as u32).to_le_bytes())?
            .is_empty(),
        "USB OTA BEGIN 响应无效"
    );
    let result = (|| -> Result<()> {
        let mut offset = 0;
        for chunk in image.chunks(496) {
            let mut request = (offset as u32).to_le_bytes().to_vec();
            request.extend_from_slice(&(chunk.len() as u16).to_le_bytes());
            request.extend_from_slice(chunk);
            let reply = link.exchange(0x82, &request)?;
            offset += chunk.len();
            ensure!(
                reply == (offset as u32).to_le_bytes(),
                "USB OTA WRITE 偏移不匹配"
            );
            progress((offset * 90 / image.len()) as u32, "写入非活动分区");
        }
        ensure!(
            link.exchange(0x83, &[])?.is_empty(),
            "USB OTA COMMIT 响应无效"
        );
        Ok(())
    })();
    if let Err(error) = result {
        let cleanup = link.exchange(0x84, &[]);
        return Err(error.context(if cleanup.is_ok() {
            "USB OTA 失败，已中止"
        } else {
            "USB OTA 失败，中止未确认，禁止自动重放"
        }));
    }
    progress(92, "镜像已提交，等待 USB 重新枚举");
    // Reset can interrupt the host's transfer completion. Never replay it: prove
    // a real disappearance followed by a successful QUERY for the same serial.
    let reboot_error = link.write(&[0x85]).err();
    drop(link);
    let end = Instant::now() + Duration::from_secs(45);
    let mut reconnect = UsbReconnect::default();
    while Instant::now() < end {
        let observation = (|| -> Result<Option<String>> {
            if !crate::usb::discover()?
                .iter()
                .any(|d| d["device_id"] == serial)
            {
                return Ok(None);
            }
            let mut link = UsbLink::open(serial, false)?;
            Ok(Some(usb_query(&mut link)?.1))
        })();
        if reconnect.observe(observation, &meta.version)? {
            return Ok(
                json!({"version":meta.version,"verification":"USB 重连及版本已核对；USB QUERY 不提供槽位启动确认字段。"}),
            );
        }
        std::thread::sleep(Duration::from_millis(250));
    }
    anyhow::bail!(
        "镜像已提交，但 USB 重连验证超时：{}；重启传输{}。请检查设备，不要重复写入",
        reconnect
            .last_error
            .as_deref()
            .unwrap_or("未观察到完整的断开、重连与版本确认"),
        if reboot_error.is_some() {
            "未获确认"
        } else {
            "已完成"
        }
    )
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn usb_reconnect_retries_enumeration_errors_without_faking_disconnect() {
        let mut state = UsbReconnect::default();
        assert!(!state.observe(Ok(Some("vtest".into())), "vtest").unwrap());
        assert!(
            !state
                .observe(Err(anyhow::anyhow!("Pipe error")), "vtest")
                .unwrap()
        );
        assert!(!state.observe(Ok(Some("vtest".into())), "vtest").unwrap());
        assert!(!state.observe(Ok(None), "vtest").unwrap());
        assert!(
            !state
                .observe(Err(anyhow::anyhow!("USB enumeration pending")), "vtest")
                .unwrap()
        );
        assert!(state.observe(Ok(Some("wrong".into())), "vtest").is_err());
        assert!(state.observe(Ok(Some("vtest".into())), "vtest").unwrap());
    }
    #[test]
    fn validates_image_and_fixed_slot_boot_confirmation() {
        let mut image = vec![0; 80];
        image[0] = 0xe9;
        image[12..14].copy_from_slice(&9u16.to_le_bytes());
        image[32..36].copy_from_slice(&0xabcd5432u32.to_le_bytes());
        image[48..53].copy_from_slice(b"vtest");
        assert_eq!(inspect(&image).unwrap().version, "vtest");
        image[12] = 0;
        assert!(inspect(&image).is_err());
        let make = |running: u32, inactive: u32, confirmed| {
            let mut bytes = vec![1, 1];
            for value in [0x3f0000u32, running, inactive] {
                bytes.extend(value.to_be_bytes());
            }
            bytes.push(confirmed);
            bytes.extend(b"vtest");
            OtaInfo::parse(&bytes, "vtest")
        };
        let before = make(0x20000, 0x410000, 1).unwrap();
        assert!(before.verify(&before, "vtest").is_err());
        assert!(
            make(0x410000, 0x20000, 1)
                .unwrap()
                .verify(&before, "vtest")
                .is_ok()
        );
        assert!(
            make(0x410000, 0x20000, 0)
                .unwrap()
                .verify(&before, "vtest")
                .is_err()
        );
        assert!(make(0x10000, 0x410000, 1).is_err());
    }
}
