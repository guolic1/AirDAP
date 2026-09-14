use crate::{
    credential::{Credential, valid_device_id},
    provisioning::{AccessPoint, auth_name, validate_wifi},
};
use anyhow::{Context, Result, ensure};
use rusb::{DeviceHandle, GlobalContext};
use serde_json::{Value, json};
use std::{
    thread::sleep,
    time::{Duration, Instant},
};
use zeroize::Zeroizing;

const PROMPT: &[u8] = b"airdap> ";
pub fn discover() -> Result<Vec<Value>> {
    let mut found = Vec::new();
    for device in rusb::devices()?.iter() {
        let desc = device.device_descriptor()?;
        if desc.vendor_id() != 0x303a || !matches!(desc.product_id(), 0x4021 | 0x4022) {
            continue;
        }
        let handle = device
            .open()
            .context("无法访问 AirDAP USB 设备，请检查驱动和权限")?;
        let serial = handle.read_serial_number_string_ascii(&desc)?;
        if !valid_device_id(&serial) {
            continue;
        }
        let config = device.active_config_descriptor()?;
        let debug = if desc.product_id() == 0x4022 { 0 } else { 3 };
        found.push(json!({"device_id":serial,"transport":"usb","usb_wifi":config.interfaces().any(|i| i.number() == debug)}));
    }
    Ok(found)
}

pub struct UsbLink {
    handle: DeviceHandle<GlobalContext>,
    interface: u8,
    input: u8,
    output: u8,
    debug: bool,
}
impl Drop for UsbLink {
    fn drop(&mut self) {
        if self.debug {
            let _ = self
                .handle
                .write_bulk(self.output, &[4], Duration::from_millis(200));
        }
        let _ = self.handle.release_interface(self.interface);
    }
}
impl UsbLink {
    pub fn open(serial: &str, debug: bool) -> Result<Self> {
        ensure!(valid_device_id(serial), "设备编号无效");
        let mut selected = Vec::new();
        for device in rusb::devices()?.iter() {
            let desc = device.device_descriptor()?;
            if desc.vendor_id() != 0x303a || !matches!(desc.product_id(), 0x4021 | 0x4022) {
                continue;
            }
            let handle = device
                .open()
                .context("USB 设备无法打开，请检查驱动和权限")?;
            if handle.read_serial_number_string_ascii(&desc)? == serial {
                selected.push((device, desc, handle));
            }
        }
        ensure!(selected.len() == 1, "未找到唯一匹配的物理 AirDAP USB 设备");
        let (device, desc, handle) = selected.pop().unwrap();
        ensure!(
            debug || desc.product_id() == 0x4021,
            "当前 USB 模式没有 DAP OTA 接口，请切换 USB 模式或使用网络升级"
        );
        let interface = if debug && desc.product_id() == 0x4021 {
            3
        } else {
            0
        };
        let (output, input) = if debug { (4, 0x84) } else { (1, 0x81) };
        let config = device.active_config_descriptor()?;
        let descriptor = config
            .interfaces()
            .find(|i| i.number() == interface)
            .and_then(|i| i.descriptors().find(|d| d.setting_number() == 0))
            .context("固件缺少所需 USB 接口")?;
        ensure!(
            descriptor.class_code() == 255
                && descriptor.sub_class_code() == 0
                && descriptor.protocol_code() == 0,
            "USB 接口类型不匹配"
        );
        let endpoints: Vec<_> = descriptor.endpoint_descriptors().collect();
        ensure!(
            endpoints.len() == 2
                && endpoints[0].address() == output
                && endpoints[1].address() == input
                && endpoints
                    .iter()
                    .all(|e| e.transfer_type() == rusb::TransferType::Bulk
                        && e.max_packet_size() == 64),
            "USB 端点布局不匹配"
        );
        match handle.set_auto_detach_kernel_driver(true) {
            Ok(()) | Err(rusb::Error::NotSupported) => {}
            Err(e) => return Err(e.into()),
        }
        handle
            .claim_interface(interface)
            .context("USB 接口被占用或权限不足")?;
        let mut link = Self {
            handle,
            interface,
            input,
            output,
            debug,
        };
        if debug {
            link.write(&[0])?;
            link.read_until(PROMPT, Duration::from_secs(5))?;
        }
        Ok(link)
    }
    pub fn write(&mut self, data: &[u8]) -> Result<()> {
        let mut offset = 0;
        while offset < data.len() {
            let written =
                self.handle
                    .write_bulk(self.output, &data[offset..], Duration::from_secs(5))?;
            ensure!(written > 0, "USB 写入没有进展");
            offset += written;
        }
        Ok(())
    }
    pub fn read(&mut self, deadline: Duration) -> Result<Vec<u8>> {
        let mut buffer = vec![0; if self.debug { 512 } else { 508 }];
        let size = self.handle.read_bulk(self.input, &mut buffer, deadline)?;
        buffer.truncate(size);
        Ok(buffer)
    }
    pub fn read_until(&mut self, marker: &[u8], duration: Duration) -> Result<Vec<u8>> {
        let end = Instant::now() + duration;
        let mut result = Vec::new();
        loop {
            if result.windows(marker.len()).any(|s| s == marker) {
                return Ok(result);
            }
            ensure!(Instant::now() < end, "USB 配网响应超时");
            match self.read(
                end.saturating_duration_since(Instant::now())
                    .max(Duration::from_millis(1))
                    .min(Duration::from_millis(100)),
            ) {
                Ok(chunk) => result.extend(chunk),
                Err(e) if e.downcast_ref::<rusb::Error>() == Some(&rusb::Error::Timeout) => {}
                Err(e) => return Err(e),
            }
            ensure!(result.len() <= 65536, "USB 响应过大");
        }
    }
    pub fn command(&mut self, command: &str, duration: Duration) -> Result<String> {
        ensure!(
            !command.is_empty() && command.bytes().all(|c| (32..=126).contains(&c)),
            "USB 命令格式无效"
        );
        let line = format!("{command}\n");
        self.write(line.as_bytes())?;
        let mut response = self.read_until(line.as_bytes(), duration)?;
        let echo_end = response
            .windows(line.len())
            .position(|s| s == line.as_bytes())
            .unwrap()
            + line.len();
        if !response[echo_end..]
            .windows(PROMPT.len())
            .any(|s| s == PROMPT)
        {
            response.extend(self.read_until(PROMPT, duration)?);
        }
        Ok(String::from_utf8_lossy(&response).into())
    }
    pub fn exchange(&mut self, command: u8, payload: &[u8]) -> Result<Vec<u8>> {
        let mut request = vec![command];
        request.extend_from_slice(payload);
        ensure!(request.len() <= 508, "USB DAP 请求过大");
        self.write(&request)?;
        let response = self.read(Duration::from_secs(10))?;
        ensure!(
            response.len() >= 2 && response[0] == command,
            "USB OTA 响应类型无效"
        );
        ensure!(response[1] == 0, "USB OTA 操作失败，状态 {}", response[1]);
        Ok(response[2..].to_vec())
    }
}

pub struct UsbSession {
    link: UsbLink,
    serial: String,
}
impl UsbSession {
    pub fn open(serial: &str) -> Result<Self> {
        let mut link = UsbLink::open(serial, true)?;
        let response = link.command("wifi capabilities", Duration::from_secs(5))?;
        ensure!(
            response.lines().any(|l| l.trim_end() == "wifi-provision=1"),
            "固件不支持 USB 配网会话，请升级固件"
        );
        Ok(Self {
            link,
            serial: serial.into(),
        })
    }
    pub fn scan(&mut self) -> Result<Vec<AccessPoint>> {
        let response = self.link.command("wifi scan", Duration::from_secs(25))?;
        let count: usize = response
            .lines()
            .find_map(|line| line.trim_end().strip_prefix("wifi-scan="))
            .context("设备扫描 Wi-Fi 失败，请确认无线电已开启")?
            .parse()?;
        ensure!(count <= 1024, "设备 Wi-Fi 扫描数量无效");
        let mut aps = Vec::new();
        for index in 0..count {
            let response = self
                .link
                .command(&format!("wifi ap {index}"), Duration::from_secs(5))?;
            let record = response
                .lines()
                .find_map(|l| l.trim_end().strip_prefix("ap="))
                .context("Wi-Fi 扫描结果无效")?;
            aps.push(parse_ap(record)?);
        }
        aps.sort_by(|a, b| b.rssi.cmp(&a.rssi));
        Ok(aps)
    }
    pub fn pair(&mut self, credential: &Credential) -> Result<Value> {
        self.link.write(b"wifi pair\n")?;
        let result = (|| -> Result<Value> {
            self.link
                .read_until(b"Network key: ", Duration::from_secs(5))?;
            let key = Zeroizing::new(format!("{}\n", hex::encode(credential.key())));
            self.link.write(key.as_bytes())?;
            let response = self.link.read_until(PROMPT, Duration::from_secs(10))?;
            let text = String::from_utf8_lossy(&response);
            let fingerprint = text
                .lines()
                .find_map(|l| l.trim_end().strip_prefix("fingerprint="))
                .context("设备未确认网络凭据；本机凭据已保留")?;
            let fingerprint = hex::decode(fingerprint)?;
            ensure!(
                fingerprint.len() == 32
                    && openssl::memcmp::eq(&fingerprint, credential.fingerprint()),
                "设备凭据指纹不匹配；本机凭据已保留"
            );
            Ok(json!({"paired":true,"device_id":self.serial}))
        })();
        if result.is_err() {
            let _ = self.link.write(&[3]);
        }
        result
    }
    pub fn wifi(&mut self, ssid: &str, password: &str) -> Result<Value> {
        validate_wifi(ssid, password, true)?;
        self.link.write(b"wifi set\n")?;
        let submitted = (|| -> Result<()> {
            self.link.read_until(b"SSID: ", Duration::from_secs(5))?;
            self.link.write(format!("{ssid}\n").as_bytes())?;
            self.link
                .read_until(b"Password: ", Duration::from_secs(5))?;
            let input = Zeroizing::new(format!("{password}\n"));
            self.link.write(input.as_bytes())?;
            let response = self.link.read_until(PROMPT, Duration::from_secs(10))?;
            ensure!(
                String::from_utf8_lossy(&response)
                    .contains("wifi: credentials saved; reconnect requested\n"),
                "设备未确认保存 Wi-Fi 配置"
            );
            Ok(())
        })();
        if submitted.is_err() {
            let _ = self.link.write(&[3]);
        }
        submitted?;
        let deadline = Instant::now() + Duration::from_secs(40);
        while Instant::now() < deadline {
            let response = self.link.command("wifi status", Duration::from_secs(5))?;
            if response.split_whitespace().any(|p| p == "wifi=online") {
                return Ok(json!({"wifi":"online"}));
            }
            sleep(Duration::from_secs(1));
        }
        anyhow::bail!("Wi-Fi 已保存，但未在时限内取得 IP，请检查网络后重试")
    }
}

pub fn parse_ap(record: &str) -> Result<AccessPoint> {
    let fields: Vec<_> = record.split(',').collect();
    ensure!(fields.len() == 5, "USB Wi-Fi 记录格式错误");
    let ssid = hex::decode(fields[0])?;
    let bssid = hex::decode(fields[1])?;
    ensure!(
        ssid.len() <= 32 && bssid.len() == 6,
        "USB Wi-Fi 记录长度错误"
    );
    Ok(AccessPoint {
        ssid: String::from_utf8_lossy(&ssid).into(),
        bssid: hex::encode(bssid),
        channel: fields[2].parse()?,
        rssi: fields[3].parse()?,
        auth: auth_name(fields[4].parse()?),
    })
}
pub fn info(serial: &str) -> Result<Value> {
    let mut link = UsbLink::open(serial, true)?;
    let mut details = serde_json::Map::new();
    for command in ["system-info", "network-status", "ota-status", "uart-status"] {
        details.insert(
            command.into(),
            Value::String(link.command(command, Duration::from_secs(5))?),
        );
    }
    let system = details["system-info"].as_str().unwrap();
    let field = |name: &str| {
        system
            .lines()
            .find_map(|l| l.trim_end().strip_prefix(&format!("{name}=")))
            .unwrap_or("未知")
            .to_string()
    };
    Ok(
        json!({"device_id":serial,"transport":"usb","firmware":field("firmware_version"),"uuid":field("uuid"),"capabilities":field("capabilities"),"details":details}),
    )
}
