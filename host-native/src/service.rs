use crate::{
    credential::Credential,
    mount::Mount,
    network::Client,
    ota,
    provisioning::{self, BleSession},
    store::{Profile, Store},
    usb::{self, UsbSession},
    usbip,
};
use anyhow::{Context, Result, ensure};
use serde_json::{Value, json};
use std::{
    path::PathBuf,
    sync::{
        Arc, Mutex as StdMutex,
        atomic::{AtomicBool, Ordering},
    },
    time::{Duration, Instant},
};
use tokio::{
    net::TcpListener,
    sync::{Mutex, watch},
    task::JoinHandle,
};
use zeroize::Zeroizing;

struct State {
    profile: Option<Profile>,
    job: Value,
    info: Value,
    discovered: Value,
    image: Option<Arc<Vec<u8>>>,
    image_meta: Value,
    provisioning: Value,
    listening: bool,
    mount_port: Option<u32>,
    bridge_error: Option<String>,
}
enum Session {
    Ble(Box<BleSession>),
    Usb(Arc<StdMutex<UsbSession>>),
}
struct Runtime {
    bridge: Option<(watch::Sender<bool>, JoinHandle<Result<()>>)>,
    session: Option<Session>,
    mount: Mount,
    retry_at: Instant,
    heartbeat_at: Instant,
    owns_export: bool,
}
pub struct Service {
    store: Store,
    state: StdMutex<State>,
    runtime: Mutex<Runtime>,
    gate: Arc<Mutex<()>>,
    closing: AtomicBool,
    imported: Arc<AtomicBool>,
    port: u16,
    usbip_available: bool,
}
fn field<'a>(data: &'a Value, key: &str) -> Result<&'a str> {
    data[key].as_str().context("请求缺少必要字段或类型无效")
}
impl Service {
    pub fn new(store: Store, port: u16, executable: Option<PathBuf>) -> Result<Arc<Self>> {
        let profile = store.profile()?;
        let mount = Mount::new(port, executable);
        let usbip_available = mount.executable.is_some();
        Ok(Arc::new(Self {
            store,
            state: StdMutex::new(State {
                profile,
                job: json!({"state":"idle"}),
                info: Value::Null,
                discovered: json!([]),
                image: None,
                image_meta: Value::Null,
                provisioning: json!({"active":false,"networks":null}),
                listening: false,
                mount_port: None,
                bridge_error: None,
            }),
            runtime: Mutex::new(Runtime {
                bridge: None,
                session: None,
                mount,
                retry_at: Instant::now(),
                heartbeat_at: Instant::now(),
                owns_export: false,
            }),
            gate: Arc::new(Mutex::new(())),
            closing: AtomicBool::new(false),
            imported: Arc::new(AtomicBool::new(false)),
            port,
            usbip_available,
        }))
    }
    pub fn snapshot(&self) -> Value {
        let s = self.state.lock().unwrap();
        let credential_present = s.profile.as_ref().is_some_and(|p| {
            self.store
                .credential_path(&p.device_id)
                .is_ok_and(|p| p.is_file())
        });
        json!({"profile":s.profile.as_ref().map(|p| serde_json::to_value(p).unwrap()).unwrap_or(json!({})),"credential_present":credential_present,
            "bridge":{"listening":s.listening,"imported":self.imported.load(Ordering::Acquire),"port":self.port,"mount_port":s.mount_port,"error":s.bridge_error},
            "job":s.job,"info":s.info,"discovered":s.discovered,"image":s.image_meta,"provisioning":s.provisioning,"usbip_available":self.usbip_available})
    }
    fn profile(&self) -> Result<Profile> {
        self.state
            .lock()
            .unwrap()
            .profile
            .clone()
            .context("请先保存设备连接信息")
    }
    fn stopped(&self) -> Result<()> {
        ensure!(
            !self.state.lock().unwrap().listening,
            "请先停止 USB 桥接并关闭烧录器和串口程序"
        );
        Ok(())
    }
    fn no_provisioning(&self) -> Result<()> {
        ensure!(
            self.state.lock().unwrap().provisioning["active"] != true,
            "请先在配网窗口点击取消配网"
        );
        Ok(())
    }
    pub fn stage_image(&self, image: Vec<u8>) -> Result<Value> {
        let _guard = self.gate.try_lock().context("当前操作尚未结束")?;
        ensure!(!self.closing.load(Ordering::Acquire), "服务正在停止");
        self.no_provisioning()?;
        let meta = serde_json::to_value(ota::inspect(&image)?)?;
        let mut s = self.state.lock().unwrap();
        s.image = Some(Arc::new(image));
        s.image_meta = meta.clone();
        Ok(meta)
    }
    pub async fn command(self: &Arc<Self>, action: &str, data: Value) -> Result<Value> {
        let guard = self
            .gate
            .clone()
            .try_lock_owned()
            .context("当前操作尚未结束，请等待完成")?;
        ensure!(!self.closing.load(Ordering::Acquire), "服务正在停止");
        ensure!(data.is_object(), "请求必须是 JSON 对象");
        ensure!(
            matches!(
                action,
                "profile"
                    | "start"
                    | "stop"
                    | "discover"
                    | "info"
                    | "ota"
                    | "wifi"
                    | "pair"
                    | "provision-start"
                    | "provision-cancel"
                    | "provision-scan"
                    | "provision-pair"
                    | "provision-wifi"
            ),
            "未知操作"
        );
        if !action.starts_with("provision-") && action != "stop" {
            self.no_provisioning()?;
        }
        if action == "profile" {
            self.stopped()?;
            let mut profile: Profile =
                serde_json::from_value(data.clone()).context("设备配置格式无效")?;
            profile.validate()?;
            profile.bridge_enabled = false;
            if !data["credential"].is_null() {
                let text = Zeroizing::new(serde_json::to_string(&data["credential"])?);
                let credential = Credential::from_json(&text)?;
                ensure!(
                    credential.device_id == profile.device_id,
                    "凭据与所选设备不一致"
                );
                self.store.save_credential(&credential)?;
            }
            self.store.save(&profile)?;
            let mut s = self.state.lock().unwrap();
            s.profile = Some(profile);
            s.info = Value::Null;
            return Ok(json!({"saved":true}));
        }
        if action == "discover" {
            ensure!(
                matches!(field(&data, "transport")?, "usb" | "ble"),
                "请选择 USB 或蓝牙发现"
            );
        } else {
            let profile = self.profile()?;
            if !matches!(action, "start" | "stop") {
                ensure!(
                    field(&data, "device_id")? == profile.device_id,
                    "设备选择已变化，请先保存所选设备"
                );
            }
            if matches!(action, "start" | "stop") {
                if action == "start" {
                    self.store.credential(&profile.device_id)?;
                }
                let mut profile = profile;
                profile.bridge_enabled = action == "start";
                self.store.save(&profile)?;
                self.state.lock().unwrap().profile = Some(profile);
            }
            if action.starts_with("provision-")
                || matches!(action, "ota" | "pair" | "wifi")
                || (action == "info" && data["transport"] == "usb")
            {
                self.stopped()?;
            }
            if action.starts_with("provision-") {
                let s = self.state.lock().unwrap();
                let p = &s.provisioning;
                if action == "provision-start" {
                    ensure!(p["active"] != true, "请先取消当前配网会话");
                } else {
                    ensure!(
                        p["active"] == true
                            && p["device_id"] == data["device_id"]
                            && p["transport"] == data["transport"],
                        "配网会话不存在或设备、通道已变化"
                    );
                }
            }
            if matches!(
                action,
                "ota" | "pair" | "wifi" | "provision-pair" | "provision-wifi"
            ) {
                ensure!(
                    data["confirm"] == true,
                    "请明确确认目标设备和即将写入的配置或固件"
                );
            }
            if matches!(action, "wifi" | "provision-wifi") {
                provisioning::validate_wifi(
                    field(&data, "ssid")?,
                    field(&data, "password")?,
                    data["transport"] == "usb",
                )?;
                if action == "provision-wifi" {
                    ensure!(
                        self.state.lock().unwrap().provisioning["networks"]
                            .as_array()
                            .is_some_and(|aps| aps.iter().any(|ap| ap["ssid"] == data["ssid"])),
                        "请先扫描并选择 Wi-Fi"
                    );
                }
            }
            if action == "ota" {
                let s = self.state.lock().unwrap();
                ensure!(
                    s.image.is_some() && data["sha256"] == s.image_meta["sha256"],
                    "镜像未上传或已变化，请重新核对"
                );
            }
            if matches!(action, "provision-start" | "wifi") {
                ensure!(
                    matches!(field(&data, "transport")?, "usb" | "ble"),
                    "配网仅支持 USB 或蓝牙"
                );
            }
            if matches!(action, "info" | "ota") {
                ensure!(
                    matches!(field(&data, "transport")?, "usb" | "network"),
                    "请选择 USB 或网络通道"
                );
            }
        }
        let action = action.to_owned();
        self.store.event(&action, "started");
        let service = self.clone();
        self.state.lock().unwrap().job =
            json!({"action":label(&action),"state":"running","progress":0,"message":"正在执行"});
        tokio::spawn(async move {
            let _guard = guard;
            let result = service.perform(&action, &data).await;
            service.store.event(
                &action,
                if result.is_ok() {
                    "succeeded"
                } else {
                    "failed"
                },
            );
            let mut s = service.state.lock().unwrap();
            match result {
                Ok(value) => {
                    s.job["state"] = json!("succeeded");
                    s.job["progress"] = json!(100);
                    s.job["message"] = json!("操作完成");
                    s.job["result"] = value;
                }
                Err(error) => {
                    s.job["state"] = json!("failed");
                    s.job["message"] = json!(error.to_string());
                }
            }
        });
        Ok(json!({"accepted":true}))
    }
    async fn perform(self: &Arc<Self>, action: &str, data: &Value) -> Result<Value> {
        let mut rt = self.runtime.lock().await;
        if action == "start" {
            self.start_bridge(&mut rt).await?;
            return Ok(json!({"started":true}));
        }
        if action == "stop" {
            self.stop_bridge(&mut rt).await?;
            return Ok(json!({"stopped":true}));
        }
        if action == "discover" {
            let found = if data["transport"] == "usb" {
                tokio::task::spawn_blocking(usb::discover).await??
            } else {
                provisioning::discover().await?
            };
            let count = found.len();
            self.state.lock().unwrap().discovered = json!(found);
            return Ok(json!({"count":count}));
        }
        let p = self.profile()?;
        if action == "info" {
            self.state.lock().unwrap().info = Value::Null;
            let mut info = if data["transport"] == "usb" {
                let id = p.device_id.clone();
                tokio::task::spawn_blocking(move || usb::info(&id)).await??
            } else {
                let c = self.store.credential(&p.device_id)?;
                let client =
                    Client::connect(&p.host, 3260, &c, None, Duration::from_secs(5)).await?;
                let mut info = serde_json::to_value(client.hello)?;
                info["host"] = json!(p.host);
                info["transport"] = json!("network");
                info["tls"] = json!("TLSv1.3 PSK");
                info
            };
            info["read_at"] = json!(format!(
                "{} Unix UTC",
                std::time::SystemTime::now()
                    .duration_since(std::time::UNIX_EPOCH)?
                    .as_secs()
            ));
            self.state.lock().unwrap().info = info.clone();
            return Ok(info);
        }
        if action == "ota" {
            let image = self
                .state
                .lock()
                .unwrap()
                .image
                .clone()
                .context("镜像未暂存")?;
            let weak = Arc::downgrade(self);
            let progress: ota::Progress = Arc::new(move |percent, message| {
                if let Some(service) = weak.upgrade() {
                    let mut s = service.state.lock().unwrap();
                    s.job["progress"] = json!(percent);
                    s.job["message"] = json!(message);
                }
            });
            let result = if data["transport"] == "usb" {
                tokio::task::spawn_blocking(move || ota::usb_upload(&p.device_id, &image, progress))
                    .await?
            } else {
                match self.store.credential(&p.device_id) {
                    Ok(c) => ota::network_upload(&p.host, &c, &image, progress).await,
                    Err(e) => Err(e),
                }
            };
            let mut s = self.state.lock().unwrap();
            s.image = None;
            s.image_meta = Value::Null;
            return result;
        }
        if action == "provision-start" {
            self.state.lock().unwrap().provisioning = json!({"active":false,"device_id":p.device_id,"transport":data["transport"],"networks":null});
            rt.session = Some(open_session(&p.device_id, field(data, "transport")?).await?);
            rt.heartbeat_at = Instant::now() + Duration::from_secs(25);
            self.state.lock().unwrap().provisioning["active"] = json!(true);
            return Ok(json!({"connected":true}));
        }
        if action == "provision-cancel" {
            let result = close_session(rt.session.take()).await;
            self.state.lock().unwrap().provisioning = json!({"active":false,"networks":null});
            result?;
            return Ok(json!({"cancelled":true}));
        }
        if matches!(action, "wifi" | "pair") {
            let transport = if action == "pair" {
                "ble"
            } else {
                field(data, "transport")?
            };
            let mut session = open_session(&p.device_id, transport).await?;
            let result = self
                .session_operation(&mut session, action, data, &p.device_id)
                .await;
            let close = close_session(Some(session)).await;
            return match result {
                Ok(value) => {
                    close?;
                    Ok(value)
                }
                Err(error) => Err(error),
            };
        }
        let session = rt.session.as_mut().context("配网会话不存在，请重新开始")?;
        let result = self
            .session_operation(
                session,
                action.trim_start_matches("provision-"),
                data,
                &p.device_id,
            )
            .await;
        rt.heartbeat_at = Instant::now() + Duration::from_secs(25);
        result
    }
    async fn session_operation(
        &self,
        session: &mut Session,
        action: &str,
        data: &Value,
        id: &str,
    ) -> Result<Value> {
        match action {
            "scan" => {
                self.state.lock().unwrap().provisioning["networks"] = Value::Null;
                let aps = match session {
                    Session::Ble(s) => s.scan().await?,
                    Session::Usb(s) => {
                        let s = s.clone();
                        tokio::task::spawn_blocking(move || s.lock().unwrap().scan()).await??
                    }
                };
                let count = aps.len();
                self.state.lock().unwrap().provisioning["networks"] = json!(aps);
                Ok(json!({"count":count}))
            }
            "pair" => {
                let c = self.store.create_credential(id)?;
                match session {
                    Session::Ble(s) => s.pair(&c).await,
                    Session::Usb(s) => {
                        let s = s.clone();
                        tokio::task::spawn_blocking(move || s.lock().unwrap().pair(&c)).await?
                    }
                }
            }
            "wifi" => {
                let ssid = field(data, "ssid")?.to_owned();
                let password = Zeroizing::new(field(data, "password")?.to_owned());
                match session {
                    Session::Ble(s) => s.wifi(&ssid, &password).await,
                    Session::Usb(s) => {
                        let s = s.clone();
                        tokio::task::spawn_blocking(move || {
                            s.lock().unwrap().wifi(&ssid, &password)
                        })
                        .await?
                    }
                }
            }
            _ => anyhow::bail!("未知配网操作"),
        }
    }
    async fn start_bridge(&self, rt: &mut Runtime) -> Result<()> {
        if rt.bridge.is_some() {
            return Ok(());
        }
        let p = self.profile()?;
        let c = self.store.credential(&p.device_id)?;
        let listener = TcpListener::bind(("127.0.0.1", self.port))
            .await
            .context("USB/IP 端口被占用")?;
        let (stop, rx) = watch::channel(false);
        let imported = self.imported.clone();
        rt.bridge = Some((
            stop,
            tokio::spawn(async move {
                usbip::serve(listener, p.host, c, [3260, 3261], imported, rx).await
            }),
        ));
        rt.owns_export = true;
        {
            let mut s = self.state.lock().unwrap();
            s.listening = true;
            s.bridge_error = None;
        }
        if p.auto_attach {
            self.attach(rt).await?;
        }
        Ok(())
    }
    async fn attach(&self, rt: &mut Runtime) -> Result<()> {
        let result = rt.mount.attach().await;
        rt.retry_at = Instant::now() + Duration::from_secs(1);
        let mut s = self.state.lock().unwrap();
        s.mount_port = rt.mount.number;
        s.bridge_error = result.as_ref().err().map(ToString::to_string);
        result
    }
    async fn stop_bridge(&self, rt: &mut Runtime) -> Result<()> {
        if let Some((stop, task)) = rt.bridge.take() {
            let _ = stop.send(true);
            task.await??;
        }
        self.state.lock().unwrap().listening = false;
        let result = if rt.owns_export && rt.mount.executable.is_some() {
            rt.mount.detach().await
        } else {
            Ok(())
        };
        if result.is_ok() {
            rt.owns_export = false;
        }
        let mut s = self.state.lock().unwrap();
        s.mount_port = rt.mount.number;
        s.bridge_error = result.as_ref().err().map(ToString::to_string);
        result
    }
    pub async fn maintain(self: Arc<Self>) {
        if self.profile().is_ok_and(|p| p.bridge_enabled) {
            let _guard = self.gate.lock().await;
            let mut rt = self.runtime.lock().await;
            if let Err(e) = self.start_bridge(&mut rt).await {
                self.state.lock().unwrap().bridge_error = Some(e.to_string());
            }
        }
        while !self.closing.load(Ordering::Acquire) {
            tokio::time::sleep(Duration::from_millis(250)).await;
            let Ok(_guard) = self.gate.try_lock() else {
                continue;
            };
            let mut rt = self.runtime.lock().await;
            if rt.bridge.is_some()
                && self.profile().is_ok_and(|p| p.auto_attach)
                && !self.imported.load(Ordering::Acquire)
                && Instant::now() >= rt.retry_at
            {
                let _ = self.attach(&mut rt).await;
            }
            if Instant::now() >= rt.heartbeat_at {
                if let Some(Session::Ble(s)) = rt.session.as_mut()
                    && let Err(e) = s.control(2).await
                {
                    self.state.lock().unwrap().provisioning["error"] =
                        json!(format!("配网连接失效：{e}；请取消后重新开始"));
                }
                rt.heartbeat_at = Instant::now() + Duration::from_secs(25);
            }
        }
    }
    pub fn begin_close(&self) {
        self.closing.store(true, Ordering::Release);
    }
    pub fn lifecycle(&self, outcome: &str) {
        self.store.event("service", outcome);
    }
    pub async fn close(&self) -> Result<()> {
        self.begin_close();
        let _guard = self.gate.lock().await;
        let mut rt = self.runtime.lock().await;
        let session_result = close_session(rt.session.take()).await;
        let bridge_result = self.stop_bridge(&mut rt).await;
        bridge_result?;
        session_result
    }
}
async fn open_session(id: &str, transport: &str) -> Result<Session> {
    if transport == "ble" {
        Ok(Session::Ble(Box::new(BleSession::open(id).await?)))
    } else {
        let id = id.to_string();
        Ok(Session::Usb(Arc::new(StdMutex::new(
            tokio::task::spawn_blocking(move || UsbSession::open(&id)).await??,
        ))))
    }
}
async fn close_session(session: Option<Session>) -> Result<()> {
    match session {
        Some(Session::Ble(mut s)) => s.close().await,
        Some(Session::Usb(s)) => {
            tokio::task::spawn_blocking(move || drop(s)).await?;
            Ok(())
        }
        None => Ok(()),
    }
}
fn label(action: &str) -> &str {
    match action {
        "start" => "启动 USB 桥接",
        "stop" => "停止 USB 桥接",
        "discover" => "发现设备",
        "info" => "读取设备信息",
        "ota" => "OTA 升级",
        "provision-start" => "连接配网设备",
        "provision-cancel" => "取消配网",
        "provision-scan" => "扫描 Wi-Fi",
        "provision-pair" | "pair" => "建立网络连接凭据",
        "provision-wifi" | "wifi" => "连接 Wi-Fi",
        other => other,
    }
}
