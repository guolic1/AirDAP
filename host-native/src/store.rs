use crate::credential::{Credential, valid_device_id};
use anyhow::{Context, Result, ensure};
use serde::{Deserialize, Serialize};
use std::{
    fs::{self, File, OpenOptions},
    io::Write,
    net::IpAddr,
    path::{Path, PathBuf},
};
use zeroize::Zeroizing;

#[derive(Clone, Serialize, Deserialize)]
pub struct Profile {
    pub device_id: String,
    pub host: String,
    #[serde(default)]
    pub bridge_enabled: bool,
    #[serde(default = "yes")]
    pub auto_attach: bool,
}
fn yes() -> bool {
    true
}
impl Profile {
    pub fn validate(&self) -> Result<()> {
        ensure!(valid_device_id(&self.device_id), "设备编号无效");
        ensure!(
            !self.host.is_empty() && self.host.len() <= 253,
            "请输入设备 IP 或主机名"
        );
        ensure!(
            self.host.parse::<IpAddr>().is_ok()
                || self.host.trim_end_matches('.').split('.').all(|s| {
                    !s.is_empty()
                        && s.len() <= 63
                        && s.bytes().all(|c| c.is_ascii_alphanumeric() || c == b'-')
                        && s.as_bytes()[0].is_ascii_alphanumeric()
                        && s.as_bytes()[s.len() - 1].is_ascii_alphanumeric()
                }),
            "主机地址不能包含 URL、端口或空格"
        );
        Ok(())
    }
}
pub fn private_dir(path: &Path) -> Result<()> {
    #[cfg(unix)]
    {
        use std::os::unix::fs::DirBuilderExt;
        fs::DirBuilder::new()
            .recursive(true)
            .mode(0o700)
            .create(path)?;
    }
    #[cfg(not(unix))]
    fs::create_dir_all(path)?;
    Ok(())
}
pub fn atomic_write(path: &Path, bytes: &[u8]) -> Result<()> {
    let parent = path.parent().context("文件缺少父目录")?;
    private_dir(parent)?;
    let mut temp = tempfile::NamedTempFile::new_in(parent)?;
    temp.write_all(bytes)?;
    temp.as_file().sync_all()?;
    temp.persist(path)?;
    #[cfg(unix)]
    File::open(parent)?.sync_all()?;
    Ok(())
}
pub struct Store {
    pub path: PathBuf,
    _lock: File,
    log_lock: std::sync::Mutex<()>,
}
impl Store {
    pub fn open(path: &Path) -> Result<Self> {
        private_dir(path)?;
        let path = path.canonicalize()?;
        let lock = OpenOptions::new()
            .read(true)
            .write(true)
            .create(true)
            .truncate(false)
            .open(path.join("service.lock"))?;
        lock.try_lock().context("同一数据目录已有服务运行")?;
        Ok(Self {
            path,
            _lock: lock,
            log_lock: std::sync::Mutex::new(()),
        })
    }
    /// Only fixed operation names and outcomes belong here, never request bodies or errors
    /// from peripherals, which can contain SSIDs or other device-controlled content.
    pub fn event(&self, operation: &str, outcome: &str) {
        let _guard = self.log_lock.lock().unwrap();
        let result = (|| -> Result<()> {
            let path = self.path.join("service.log");
            if path.metadata().is_ok_and(|m| m.len() >= 2 * 1024 * 1024) {
                let older = self.path.join("service.log.2");
                if older.exists() {
                    fs::remove_file(&older)?;
                }
                let previous = self.path.join("service.log.1");
                if previous.exists() {
                    fs::rename(&previous, older)?;
                }
                fs::rename(&path, previous)?;
            }
            let mut options = OpenOptions::new();
            options.create(true).append(true);
            #[cfg(unix)]
            {
                use std::os::unix::fs::OpenOptionsExt;
                options.mode(0o600);
            }
            let mut file = options.open(path)?;
            let timestamp = std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)?
                .as_secs();
            writeln!(file, "{timestamp} {operation} {outcome}")?;
            Ok(())
        })();
        if result.is_err() {
            eprintln!("AirDAP 日志写入失败，请检查数据目录权限和磁盘空间");
        }
    }
    pub fn profile(&self) -> Result<Option<Profile>> {
        let path = self.path.join("config.json");
        if !path.exists() {
            return Ok(None);
        }
        let profile: Profile = serde_json::from_slice(&fs::read(path)?)?;
        profile.validate()?;
        Ok(Some(profile))
    }
    pub fn save(&self, profile: &Profile) -> Result<()> {
        profile.validate()?;
        atomic_write(
            &self.path.join("config.json"),
            &serde_json::to_vec(profile)?,
        )
    }
    pub fn credential_path(&self, id: &str) -> Result<PathBuf> {
        ensure!(valid_device_id(id), "设备编号无效");
        Ok(self.path.join("credentials").join(format!("{id}.json")))
    }
    pub fn credential(&self, id: &str) -> Result<Credential> {
        let credential = Credential::load(&self.credential_path(id)?)
            .context("无法读取网络凭据，请导入或建立本机凭据")?;
        ensure!(credential.device_id == id, "凭据与设备编号不匹配");
        Ok(credential)
    }
    pub fn save_credential(&self, credential: &Credential) -> Result<()> {
        let text = Zeroizing::new(credential.to_json()?);
        atomic_write(
            &self.credential_path(&credential.device_id)?,
            text.as_bytes(),
        )
    }
    pub fn create_credential(&self, id: &str) -> Result<Credential> {
        if self.credential_path(id)?.exists() {
            return self.credential(id);
        }
        let credential = Credential::generate(id)?;
        self.save_credential(&credential)?;
        Ok(credential)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn persists_compatible_credentials_and_excludes_second_process() {
        let dir = tempfile::tempdir().unwrap();
        let store = Store::open(dir.path()).unwrap();
        assert!(Store::open(dir.path()).is_err());
        let id = "ADP-001122334455";
        let c = store.create_credential(id).unwrap();
        assert_eq!(c.key(), store.create_credential(id).unwrap().key());
        assert!(store.credential_path("../../secret").is_err());
        let mut p = Profile {
            device_id: id.into(),
            host: "127.0.0.1".into(),
            bridge_enabled: false,
            auto_attach: true,
        };
        store.save(&p).unwrap();
        assert_eq!(store.profile().unwrap().unwrap().host, p.host);
        p.host = "https://localhost/".into();
        assert!(store.save(&p).is_err());
        drop(store);
        assert!(Store::open(dir.path()).is_ok());
    }
}
