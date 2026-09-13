use anyhow::{Context, Result, ensure};
use openssl::{memcmp, rand::rand_bytes, sha::sha256};
use serde::{Deserialize, Serialize};
use std::{fmt, fs, path::Path};
use zeroize::{Zeroize, ZeroizeOnDrop};

#[derive(Clone, Zeroize, ZeroizeOnDrop)]
pub struct Credential {
    pub device_id: String,
    identity: String,
    psk: [u8; 32],
    fingerprint: [u8; 32],
}

impl fmt::Debug for Credential {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Credential")
            .field("device_id", &self.device_id)
            .finish_non_exhaustive()
    }
}

#[derive(Serialize, Deserialize, Zeroize, ZeroizeOnDrop)]
struct Record {
    version: u32,
    device_id: String,
    identity: String,
    psk: String,
    fingerprint: String,
}

pub fn valid_device_id(id: &str) -> bool {
    id.len() == 16
        && id.starts_with("ADP-")
        && id.as_bytes()[4..]
            .iter()
            .all(|c| c.is_ascii_digit() || (b'A'..=b'F').contains(c))
}

impl Credential {
    pub fn new(device_id: &str, psk: [u8; 32]) -> Result<Self> {
        ensure!(
            valid_device_id(device_id),
            "设备编号必须为 ADP- 加 12 位大写十六进制字符"
        );
        let identity = format!("AIRDAP:{device_id}");
        let mut input = b"AirDAP network PSK v1".to_vec();
        input.extend_from_slice(identity.as_bytes());
        input.extend_from_slice(&psk);
        let fingerprint = sha256(&input);
        input.zeroize();
        Ok(Self {
            device_id: device_id.into(),
            identity,
            psk,
            fingerprint,
        })
    }
    pub fn generate(device_id: &str) -> Result<Self> {
        let mut key = [0; 32];
        rand_bytes(&mut key)?;
        let result = Self::new(device_id, key);
        key.zeroize();
        result
    }
    pub fn identity(&self) -> &str {
        &self.identity
    }
    pub fn key(&self) -> &[u8; 32] {
        &self.psk
    }
    pub fn fingerprint(&self) -> &[u8; 32] {
        &self.fingerprint
    }
    pub fn to_json(&self) -> Result<String> {
        Ok(serde_json::to_string_pretty(&Record {
            version: 1,
            device_id: self.device_id.clone(),
            identity: self.identity.clone(),
            psk: hex::encode(self.psk),
            fingerprint: hex::encode(self.fingerprint),
        })? + "\n")
    }
    pub fn from_json(text: &str) -> Result<Self> {
        let record: Record = serde_json::from_str(text).context("凭据 JSON 格式无效")?;
        ensure!(record.version == 1, "凭据版本不支持");
        let mut key = [0; 32];
        hex::decode_to_slice(&record.psk, &mut key).context("凭据密钥长度或格式无效")?;
        let candidate = Self::new(&record.device_id, key);
        key.zeroize();
        let candidate = candidate?;
        let fingerprint = hex::decode(&record.fingerprint).context("凭据指纹格式无效")?;
        ensure!(
            record.identity == candidate.identity
                && fingerprint.len() == 32
                && memcmp::eq(&fingerprint, &candidate.fingerprint),
            "凭据完整性校验失败"
        );
        Ok(candidate)
    }
    pub fn load(path: &Path) -> Result<Self> {
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            ensure!(
                fs::metadata(path)?.permissions().mode() & 0o077 == 0,
                "凭据文件权限必须为 0600"
            );
        }
        let mut text = fs::read_to_string(path)?;
        let result = Self::from_json(&text);
        text.zeroize();
        result
    }
}
