use crate::{
    credential::{Credential, valid_device_id},
    security2::{SessionCipher, SrpClient},
};
use anyhow::{Context, Result, ensure};
use btleplug::{
    api::{Central, Characteristic, Manager as _, Peripheral as _, ScanFilter, WriteType},
    platform::{Manager, Peripheral},
};
use prost::Message;
use serde::Serialize;
use serde_json::{Value, json};
use std::{collections::BTreeMap, time::Duration};
use tokio::time::{sleep, timeout};
use zeroize::Zeroizing;

#[derive(Clone, Debug, Serialize)]
pub struct AccessPoint {
    pub ssid: String,
    pub bssid: String,
    pub channel: u32,
    pub rssi: i32,
    pub auth: String,
}
pub fn auth_name(mode: u32) -> String {
    [
        "Open",
        "WEP",
        "WPA_PSK",
        "WPA2_PSK",
        "WPA_WPA2_PSK",
        "WPA2_ENTERPRISE",
        "WPA3_PSK",
        "WPA2_WPA3_PSK",
    ]
    .get(mode as usize)
    .map(|s| (*s).into())
    .unwrap_or_else(|| format!("模式 {mode}"))
}
pub fn validate_wifi(ssid: &str, password: &str, usb: bool) -> Result<()> {
    ensure!(
        !ssid.is_empty() && ssid.len() <= 32 && !ssid.contains('\0'),
        "Wi-Fi 名称必须为 1–32 字节"
    );
    ensure!(
        password.len() <= 64 && !password.contains('\0'),
        "Wi-Fi 密码长度无效"
    );
    ensure!(
        !ssid
            .chars()
            .chain(password.chars())
            .any(|c| c < ' ' || c == '\u{7f}'),
        "SSID 和密码不能包含控制字符"
    );
    if usb {
        ensure!(
            ssid.bytes()
                .chain(password.bytes())
                .all(|b| (32..=126).contains(&b)),
            "USB 配网目前仅支持可打印 ASCII 名称和密码，请改用蓝牙"
        );
    }
    Ok(())
}

// Wire tags follow ESP-IDF protocomm and network_provisioning .proto contracts.
// Opaque oneof bodies keep the envelope shared without including unused Thread/security schemes.
#[derive(Clone, PartialEq, Message)]
struct Payload {
    #[prost(uint32, tag = "1")]
    msg: u32,
    #[prost(uint32, tag = "2")]
    status: u32,
    #[prost(oneof = "Body", tags = "10,11,12,13,14,15,20,21,22,23")]
    body: Option<Body>,
}
#[derive(Clone, PartialEq, prost::Oneof)]
enum Body {
    #[prost(bytes, tag = "10")]
    F10(Vec<u8>),
    #[prost(bytes, tag = "11")]
    F11(Vec<u8>),
    #[prost(bytes, tag = "12")]
    F12(Vec<u8>),
    #[prost(bytes, tag = "13")]
    F13(Vec<u8>),
    #[prost(bytes, tag = "14")]
    F14(Vec<u8>),
    #[prost(bytes, tag = "15")]
    F15(Vec<u8>),
    #[prost(bytes, tag = "20")]
    F20(Vec<u8>),
    #[prost(bytes, tag = "21")]
    F21(Vec<u8>),
    #[prost(bytes, tag = "22")]
    F22(Vec<u8>),
    #[prost(bytes, tag = "23")]
    F23(Vec<u8>),
}
impl Body {
    fn into_bytes(self, expected: u32) -> Result<Vec<u8>> {
        let (tag, data) = match self {
            Self::F10(d) => (10, d),
            Self::F11(d) => (11, d),
            Self::F12(d) => (12, d),
            Self::F13(d) => (13, d),
            Self::F14(d) => (14, d),
            Self::F15(d) => (15, d),
            Self::F20(d) => (20, d),
            Self::F21(d) => (21, d),
            Self::F22(d) => (22, d),
            Self::F23(d) => (23, d),
        };
        ensure!(tag == expected, "配网响应类型不匹配");
        Ok(data)
    }
}
fn payload(msg: u32, body: Body) -> Payload {
    Payload {
        msg,
        status: 0,
        body: Some(body),
    }
}
fn response(data: &[u8], msg: u32, tag: u32) -> Result<Vec<u8>> {
    let p = Payload::decode(data).context("配网响应格式无效")?;
    ensure!(p.msg == msg && p.status == 0, "设备拒绝配网操作");
    p.body.context("配网响应缺少内容")?.into_bytes(tag)
}
#[derive(Clone, PartialEq, Message)]
struct Session {
    #[prost(uint32, tag = "2")]
    version: u32,
    #[prost(message, optional, tag = "12")]
    payload: Option<Payload>,
}
#[derive(Clone, PartialEq, Message)]
struct AuthStart {
    #[prost(bytes, tag = "1")]
    username: Vec<u8>,
    #[prost(bytes, tag = "2")]
    public: Vec<u8>,
}
#[derive(Clone, PartialEq, Message)]
struct AuthChallenge {
    #[prost(uint32, tag = "1")]
    status: u32,
    #[prost(bytes, tag = "2")]
    public: Vec<u8>,
    #[prost(bytes, tag = "3")]
    salt: Vec<u8>,
}
#[derive(Clone, PartialEq, Message)]
struct AuthProof {
    #[prost(bytes, tag = "1")]
    proof: Vec<u8>,
}
#[derive(Clone, PartialEq, Message)]
struct AuthFinish {
    #[prost(uint32, tag = "1")]
    status: u32,
    #[prost(bytes, tag = "2")]
    proof: Vec<u8>,
    #[prost(bytes, tag = "3")]
    nonce: Vec<u8>,
}
#[derive(Clone, PartialEq, Message)]
struct ScanStart {
    #[prost(bool, tag = "1")]
    blocking: bool,
    #[prost(uint32, tag = "4")]
    period_ms: u32,
}
#[derive(Clone, PartialEq, Message)]
struct ScanStatus {
    #[prost(bool, tag = "1")]
    finished: bool,
    #[prost(uint32, tag = "2")]
    count: u32,
}
#[derive(Clone, PartialEq, Message)]
struct ScanRange {
    #[prost(uint32, tag = "1")]
    start: u32,
    #[prost(uint32, tag = "2")]
    count: u32,
}
#[derive(Clone, PartialEq, Message)]
struct ScanEntry {
    #[prost(bytes, tag = "1")]
    ssid: Vec<u8>,
    #[prost(uint32, tag = "2")]
    channel: u32,
    #[prost(int32, tag = "3")]
    rssi: i32,
    #[prost(bytes, tag = "4")]
    bssid: Vec<u8>,
    #[prost(uint32, tag = "5")]
    auth: u32,
}
#[derive(Clone, PartialEq, Message)]
struct ScanResult {
    #[prost(message, repeated, tag = "1")]
    entries: Vec<ScanEntry>,
}
#[derive(Clone, PartialEq, Message)]
struct WifiConfig {
    #[prost(bytes, tag = "1")]
    ssid: Vec<u8>,
    #[prost(bytes, tag = "2")]
    password: Vec<u8>,
}
#[derive(Clone, PartialEq, Message)]
struct Status {
    #[prost(uint32, tag = "1")]
    status: u32,
}
#[derive(Clone, PartialEq, Message)]
struct Connected {
    #[prost(string, tag = "1")]
    ip: String,
}
#[derive(Clone, PartialEq, Message)]
struct WifiStatus {
    #[prost(uint32, tag = "1")]
    status: u32,
    #[prost(uint32, tag = "2")]
    station: u32,
    #[prost(message, optional, tag = "11")]
    connected: Option<Connected>,
}

async fn peripherals() -> Result<Vec<Peripheral>> {
    let adapters = Manager::new()
        .await
        .context("无法访问蓝牙服务")?
        .adapters()
        .await?;
    ensure!(!adapters.is_empty(), "未找到可用蓝牙适配器");
    let mut active = Vec::new();
    for adapter in adapters {
        if let Err(error) = adapter.start_scan(ScanFilter::default()).await {
            for started in &active {
                let _ = btleplug::api::Central::stop_scan(started).await;
            }
            return Err(error.into());
        }
        active.push(adapter);
    }
    sleep(Duration::from_secs(8)).await;
    let mut found = Vec::new();
    let mut failure = None;
    for adapter in active {
        let result = adapter.peripherals().await;
        let stopped = adapter.stop_scan().await;
        match result {
            Ok(items) => found.extend(items),
            Err(e) => failure = Some(e),
        }
        if let Err(e) = stopped {
            failure = Some(e);
        }
    }
    if let Some(error) = failure {
        return Err(error.into());
    }
    Ok(found)
}
pub async fn discover() -> Result<Vec<Value>> {
    let mut found = Vec::new();
    for peripheral in peripherals().await? {
        if let Some(properties) = peripheral.properties().await?
            && let Some(name) = properties.local_name.filter(|name| valid_device_id(name))
        {
            found.push(json!({"device_id":name,"address":peripheral.address().to_string(),"transport":"ble"}));
        }
    }
    Ok(found)
}

pub struct BleSession {
    peripheral: Peripheral,
    endpoints: BTreeMap<String, Characteristic>,
    cipher: Option<SessionCipher>,
    valid: bool,
    managed: bool,
}
impl BleSession {
    pub async fn open(id: &str) -> Result<Self> {
        ensure!(valid_device_id(id), "设备编号无效");
        let mut matches = Vec::new();
        for p in peripherals().await? {
            if p.properties().await?.and_then(|p| p.local_name).as_deref() == Some(id) {
                matches.push(p);
            }
        }
        ensure!(
            matches.len() == 1,
            "未找到唯一设备，请开启所选设备的蓝牙配网模式"
        );
        let peripheral = matches.pop().unwrap();
        let mut session = Self {
            peripheral,
            endpoints: BTreeMap::new(),
            cipher: None,
            valid: true,
            managed: false,
        };
        let result = timeout(Duration::from_secs(60), session.initialize()).await;
        match result {
            Ok(Ok(())) => Ok(session),
            other => {
                let _ = session.peripheral.disconnect().await;
                match other {
                    Ok(Err(e)) => Err(e),
                    _ => anyhow::bail!("蓝牙配网握手超时"),
                }
            }
        }
    }
    async fn initialize(&mut self) -> Result<()> {
        self.peripheral.connect().await.context("蓝牙连接失败")?;
        self.peripheral.discover_services().await?;
        for characteristic in self.peripheral.characteristics() {
            for descriptor in &characteristic.descriptors {
                if descriptor.uuid == uuid::Uuid::from_u128(0x0000290100001000800000805f9b34fb) {
                    let value = self.peripheral.read_descriptor(descriptor).await?;
                    let name = std::str::from_utf8(&value)?.trim_end_matches('\0');
                    if [
                        "prov-session",
                        "proto-ver",
                        "prov-config",
                        "prov-scan",
                        "airdap-pair",
                    ]
                    .contains(&name)
                    {
                        ensure!(
                            self.endpoints
                                .insert(name.into(), characteristic.clone())
                                .is_none(),
                            "蓝牙配网端点重复"
                        );
                    }
                }
            }
        }
        for name in [
            "prov-session",
            "proto-ver",
            "prov-config",
            "prov-scan",
            "airdap-pair",
        ] {
            ensure!(
                self.endpoints.contains_key(name),
                "设备缺少蓝牙配网端点 {name}"
            );
        }
        let version: Value = serde_json::from_slice(&self.raw("proto-ver", b"---").await?)?;
        ensure!(
            version["prov"]["sec_patch_ver"] == 1,
            "设备需支持 Security 2 patch 1，请升级固件"
        );
        // These are the firmware's public development credentials, not a host network PSK.
        let mut srp = SrpClient::new("wifiprov", "abcd1234")?;
        let start = AuthStart {
            username: b"wifiprov".to_vec(),
            public: srp.public_key()?,
        };
        let challenge = self
            .session_exchange(0, Body::F20(start.encode_to_vec()), 1, 21)
            .await?;
        let challenge = AuthChallenge::decode(&challenge[..])?;
        ensure!(challenge.status == 0, "设备拒绝 Security 2 握手");
        let proof = AuthProof {
            proof: srp.challenge(&challenge.salt, &challenge.public)?,
        };
        let finish = self
            .session_exchange(2, Body::F22(proof.encode_to_vec()), 3, 23)
            .await?;
        let finish = AuthFinish::decode(&finish[..])?;
        ensure!(finish.status == 0, "设备拒绝 Security 2 证明");
        self.cipher = Some(srp.verify(&finish.proof, &finish.nonce)?);
        self.control(1).await?;
        self.managed = true;
        Ok(())
    }
    async fn raw(&self, endpoint: &str, data: &[u8]) -> Result<Vec<u8>> {
        let characteristic = self.endpoints.get(endpoint).context("配网端点不存在")?;
        timeout(Duration::from_secs(30), async {
            self.peripheral
                .write(characteristic, data, WriteType::WithResponse)
                .await?;
            let result = self.peripheral.read(characteristic).await?;
            ensure!(result.len() <= 8192, "蓝牙配网响应过大");
            Ok(result)
        })
        .await
        .context("蓝牙配网通信超时")?
    }
    async fn session_exchange(
        &self,
        msg: u32,
        body: Body,
        reply: u32,
        tag: u32,
    ) -> Result<Vec<u8>> {
        let request = Session {
            version: 2,
            payload: Some(payload(msg, body)),
        }
        .encode_to_vec();
        let response_data = self.raw("prov-session", &request).await?;
        let session = Session::decode(&response_data[..])?;
        ensure!(session.version == 2, "Security 2 版本不匹配");
        response(
            &session
                .payload
                .context("缺少 Security 2 响应")?
                .encode_to_vec(),
            reply,
            tag,
        )
    }
    async fn encrypted(&mut self, endpoint: &str, data: &[u8]) -> Result<Vec<u8>> {
        ensure!(self.valid, "蓝牙会话已中断，请取消后重新配网");
        self.valid = false;
        let cipher = self
            .cipher
            .as_mut()
            .context("Security 2 会话未建立")?
            .encrypt(data)?;
        let response = self.raw(endpoint, &cipher).await?;
        let result = self.cipher.as_mut().unwrap().decrypt(&response)?;
        self.valid = true;
        Ok(result)
    }
    pub async fn control(&mut self, command: u8) -> Result<()> {
        for attempt in 0..50 {
            let reply = self
                .encrypted("airdap-pair", &[2, if attempt == 0 { command } else { 0 }])
                .await?;
            ensure!(reply.len() == 4 && reply[0] == 2, "固件不支持持续配网会话");
            if command == 4 || reply[1] == 0 {
                ensure!(
                    reply[2] == command && (reply[1] != 0 || reply[3] == 1),
                    "设备未接受配网会话操作"
                );
                return Ok(());
            }
            sleep(Duration::from_millis(100)).await;
        }
        anyhow::bail!("配网会话响应超时")
    }
    pub async fn scan(&mut self) -> Result<Vec<AccessPoint>> {
        self.control(2).await?;
        let start = payload(
            0,
            Body::F10(
                ScanStart {
                    blocking: true,
                    period_ms: 120,
                }
                .encode_to_vec(),
            ),
        );
        response(
            &self.encrypted("prov-scan", &start.encode_to_vec()).await?,
            1,
            11,
        )?;
        let status = self
            .encrypted("prov-scan", &payload(2, Body::F12(vec![])).encode_to_vec())
            .await?;
        let status = ScanStatus::decode(&response(&status, 3, 13)?[..])?;
        ensure!(
            status.finished && status.count <= 1024,
            "设备 Wi-Fi 扫描未完成或结果数量无效"
        );
        let mut aps = Vec::new();
        for start in (0..status.count).step_by(4) {
            let range = ScanRange {
                start,
                count: 4.min(status.count - start),
            };
            let reply = self
                .encrypted(
                    "prov-scan",
                    &payload(4, Body::F14(range.encode_to_vec())).encode_to_vec(),
                )
                .await?;
            let reply = ScanResult::decode(&response(&reply, 5, 15)?[..])?;
            ensure!(
                reply.entries.len() == range.count as usize,
                "Wi-Fi 扫描结果数量不匹配"
            );
            for ap in reply.entries {
                ensure!(
                    ap.ssid.len() <= 32 && ap.bssid.len() == 6,
                    "Wi-Fi 扫描结果字段无效"
                );
                aps.push(AccessPoint {
                    ssid: String::from_utf8_lossy(&ap.ssid).into(),
                    bssid: hex::encode(ap.bssid),
                    channel: ap.channel,
                    rssi: ap.rssi,
                    auth: auth_name(ap.auth),
                });
            }
        }
        aps.sort_by(|a, b| b.rssi.cmp(&a.rssi));
        Ok(aps)
    }
    pub async fn pair(&mut self, credential: &Credential) -> Result<Value> {
        self.control(2).await?;
        let mut request = Zeroizing::new(vec![1]);
        request.extend_from_slice(credential.key());
        let fingerprint = self.encrypted("airdap-pair", &request).await?;
        ensure!(
            fingerprint.len() == 32 && openssl::memcmp::eq(&fingerprint, credential.fingerprint()),
            "设备未确认网络凭据；本机凭据已保留，可重试"
        );
        Ok(json!({"paired":true,"device_id":credential.device_id}))
    }
    pub async fn wifi(&mut self, ssid: &str, password: &str) -> Result<Value> {
        validate_wifi(ssid, password, false)?;
        self.control(3).await?;
        let mut config = WifiConfig {
            ssid: ssid.as_bytes().to_vec(),
            password: password.as_bytes().to_vec(),
        };
        let config_data = Zeroizing::new(config.encode_to_vec());
        use zeroize::Zeroize;
        config.password.zeroize();
        let request = Zeroizing::new(payload(2, Body::F12(config_data.to_vec())).encode_to_vec());
        let reply = self.encrypted("prov-config", &request).await?;
        ensure!(
            Status::decode(&response(&reply, 3, 13)?[..])?.status == 0,
            "设备未接受 Wi-Fi 配置"
        );
        let reply = self
            .encrypted(
                "prov-config",
                &payload(4, Body::F14(vec![])).encode_to_vec(),
            )
            .await?;
        ensure!(
            Status::decode(&response(&reply, 5, 15)?[..])?.status == 0,
            "设备未确认应用 Wi-Fi 配置"
        );
        for _ in 0..40 {
            let reply = self
                .encrypted(
                    "prov-config",
                    &payload(0, Body::F10(vec![])).encode_to_vec(),
                )
                .await?;
            let status = WifiStatus::decode(&response(&reply, 1, 11)?[..])?;
            ensure!(
                status.status == 0 && status.station <= 2,
                "Wi-Fi 连接失败，请检查密码后重试"
            );
            if status.station == 0 {
                let ip: std::net::Ipv4Addr = status
                    .connected
                    .context("缺少 Wi-Fi 联网信息")?
                    .ip
                    .parse()?;
                ensure!(!ip.is_unspecified(), "设备尚未取得 IP");
                return Ok(json!({"wifi":"online"}));
            }
            sleep(Duration::from_secs(1)).await;
        }
        anyhow::bail!("设备未在时限内取得 IP，请检查 Wi-Fi 后重试")
    }
    pub async fn close(&mut self) -> Result<()> {
        let result = if self.managed && self.valid {
            self.control(4).await
        } else {
            Ok(())
        };
        let disconnected = self.peripheral.disconnect().await;
        self.managed = false;
        self.valid = false;
        self.cipher = None;
        result?;
        disconnected?;
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn wire_envelopes_and_config_validation() {
        assert_eq!(
            hex::encode(payload(2, Body::F12(vec![])).encode_to_vec()),
            "08026200"
        );
        assert!(response(&payload(3, Body::F13(vec![])).encode_to_vec(), 3, 13).is_ok());
        assert!(response(&payload(3, Body::F11(vec![])).encode_to_vec(), 3, 13).is_err());
        assert!(validate_wifi("中文", "example-only", false).is_ok());
        assert!(validate_wifi("中文", "example-only", true).is_err());
        assert!(validate_wifi("SSID\ncommand", "example-only", true).is_err());
    }
}
