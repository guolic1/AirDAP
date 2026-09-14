use crate::{
    credential::{Credential, valid_device_id},
    network::Client,
};
use anyhow::{Result, bail, ensure};
use std::{
    collections::VecDeque,
    sync::{
        Arc, Mutex,
        atomic::{AtomicBool, Ordering},
    },
    time::Duration,
};
use tokio::{
    io::{AsyncReadExt, AsyncWriteExt},
    net::{TcpListener, TcpStream, tcp::OwnedReadHalf},
    sync::{Mutex as AsyncMutex, Notify, Semaphore, mpsc, watch},
    task::JoinSet,
    time::{Instant, sleep, timeout},
};

const LIMIT: usize = 65536;
const DEVICE: u32 = 0x10001;
const STALL: i32 = -32;

fn bytes(text: &str) -> Vec<u8> {
    hex::decode(text.replace(' ', "")).expect("constant descriptor")
}
fn utf16(text: &str) -> Vec<u8> {
    text.encode_utf16().flat_map(u16::to_le_bytes).collect()
}
fn u32be(data: &[u8]) -> u32 {
    u32::from_be_bytes(data[..4].try_into().expect("validated length"))
}
fn u16le(data: &[u8]) -> u16 {
    u16::from_le_bytes(data[..2].try_into().expect("validated length"))
}

pub struct Descriptors {
    device: Vec<u8>,
    configuration: Vec<u8>,
    bos: Vec<u8>,
    strings: Vec<Vec<u8>>,
    pub ms_os: Vec<u8>,
}
impl Descriptors {
    pub fn new(id: &str) -> Result<Self> {
        ensure!(valid_device_id(id), "invalid USB serial identity");
        let device = bytes("12011002ef0201403a302140020101020301");
        let body = bytes(
            "0904000002ff000004 07050102400000 07058102400000 080b010202020005 090401000102020105 0524002001 0524010002 04240202 0524060102 07058203080010 09040200020a000000 07050302400000 07058302400000",
        );
        let mut configuration = vec![9, 2];
        configuration.extend_from_slice(&((9 + body.len()) as u16).to_le_bytes());
        configuration.extend_from_slice(&[3, 1, 0, 0x80, 50]);
        configuration.extend(body);
        let name = utf16("DeviceInterfaceGUIDs\0");
        let guid = utf16("{E00ECB98-DD2B-4E70-8471-A7223FADDAF9}\0\0");
        let mut ms_os = bytes(
            "0a00000000000306b200 080001000000a800 080002000000a000 1400030057494e55534200000000000000000000 840004000700",
        );
        ms_os.extend_from_slice(&(name.len() as u16).to_le_bytes());
        ms_os.extend(name);
        ms_os.extend_from_slice(&(guid.len() as u16).to_le_bytes());
        ms_os.extend(guid);
        let mut bos = bytes("050f2100011c100500df60ddd88945c74c9cd2659d9e648a9f00000306");
        bos.extend_from_slice(&(ms_os.len() as u16).to_le_bytes());
        bos.extend_from_slice(&[0x20, 0]);
        let mut strings = vec![vec![4, 3, 9, 4]];
        for text in [
            "AirDAP",
            "AirDAP CMSIS-DAP v2 (Network)",
            &format!("{id}-NET"),
            "CMSIS-DAP v2",
            "AirDAP Target UART",
        ] {
            let text = utf16(text);
            let mut value = vec![(text.len() + 2) as u8, 3];
            value.extend(text);
            strings.push(value);
        }
        Ok(Self {
            device,
            configuration,
            bos,
            strings,
            ms_os,
        })
    }
    pub fn get(&self, value: u16, language: u16) -> Result<Vec<u8>> {
        let kind = value >> 8;
        let index = (value & 255) as usize;
        if kind == 3
            && matches!(language, 0 | 0x409)
            && let Some(value) = self.strings.get(index)
        {
            return Ok(value.clone());
        }
        if index == 0 && language == 0 {
            match kind {
                1 => return Ok(self.device.clone()),
                2 => return Ok(self.configuration.clone()),
                15 => return Ok(self.bos.clone()),
                _ => {}
            }
        }
        bail!("unsupported descriptor")
    }
    pub fn record(&self) -> Vec<u8> {
        let mut record = vec![0; 288];
        record[..11].copy_from_slice(b"/airdap/1-1");
        record[256..259].copy_from_slice(b"1-1");
        for value in [1u32, 1, 2] {
            record.extend_from_slice(&value.to_be_bytes());
        }
        for value in [0x303au16, 0x4021, 0x0102] {
            record.extend_from_slice(&value.to_be_bytes());
        }
        record.extend_from_slice(&[0xef, 2, 1, 1, 1, 3]);
        record
    }
}

pub struct Urb {
    command: u32,
    pub seq: u32,
    direction: u32,
    ep: u32,
    flags: u32,
    pub size: usize,
    setup: [u8; 8],
    data: Vec<u8>,
    active: AtomicBool,
}
impl Urb {
    pub fn parse(h: &[u8; 48]) -> Result<Self> {
        let command = u32be(h);
        let direction = u32be(&h[12..]);
        let ep = u32be(&h[16..]);
        ensure!(
            u32be(&h[8..]) == DEVICE && direction <= 1,
            "invalid USB/IP device or direction"
        );
        let size = u32be(&h[24..]) as usize;
        ensure!(matches!(command, 1 | 2), "invalid USB/IP command");
        if command == 2 {
            ensure!(ep == 0, "invalid UNLINK endpoint");
        } else {
            ensure!(
                size <= LIMIT && matches!(u32be(&h[32..]), 0 | u32::MAX),
                "invalid URB size or isochronous transfer"
            );
        }
        Ok(Self {
            command,
            seq: u32be(&h[4..]),
            direction,
            ep,
            flags: u32be(&h[20..]),
            size,
            setup: h[40..48].try_into()?,
            data: Vec::new(),
            active: AtomicBool::new(false),
        })
    }
}

struct Backend {
    clients: [AsyncMutex<Client>; 2],
}
impl Backend {
    async fn connect(host: &str, credential: &Credential, ports: [u16; 2]) -> Result<Self> {
        let dap = Client::connect(
            host,
            ports[0],
            credential,
            Some(&[]),
            Duration::from_secs(5),
        )
        .await?;
        let uart = Client::connect(
            host,
            ports[1],
            credential,
            Some(&dap.token),
            Duration::from_secs(5),
        )
        .await?;
        ensure!(
            dap.owner == uart.owner
                && dap.token == uart.token
                && dap.hello.firmware == uart.hello.firmware,
            "DAP and UART session mismatch"
        );
        Ok(Self {
            clients: [AsyncMutex::new(dap), AsyncMutex::new(uart)],
        })
    }
    async fn configure(&self, coding: [u8; 7]) -> Result<()> {
        let mut client = self.clients[1].lock().await;
        ensure!(
            client.control(0x11, &[]).await?.is_empty(),
            "invalid UART ACQUIRE response"
        );
        let mut data = coding;
        data[..4].reverse();
        ensure!(
            client.control(0x12, &data).await?.is_empty(),
            "invalid UART CONFIGURE response"
        );
        Ok(())
    }
    async fn dap(&self, request: &[u8]) -> Result<Vec<u8>> {
        let response = self.clients[0].lock().await.request(3, request, 4).await?;
        ensure!(
            !response.is_empty()
                && response.len() <= 508
                && (response[0] == request[0] || response[0] == 255),
            "invalid CMSIS-DAP response"
        );
        Ok(response)
    }
    async fn write(&self, data: &[u8]) -> Result<()> {
        let mut client = self.clients[1].lock().await;
        let end = Instant::now() + Duration::from_secs(5);
        let mut offset = 0;
        while offset < data.len() {
            ensure!(Instant::now() < end, "UART write timed out");
            let chunk = &data[offset..data.len().min(offset + 256)];
            let response = client.control(0x13, chunk).await?;
            ensure!(response.len() == 2, "invalid UART WRITE response");
            let accepted = u16::from_be_bytes(response[..2].try_into()?) as usize;
            ensure!(
                accepted <= chunk.len(),
                "UART accepted more bytes than sent"
            );
            offset += accepted;
            if accepted == 0 {
                sleep(Duration::from_millis(10)).await;
            }
        }
        Ok(())
    }
    async fn read(&self, capacity: usize) -> Result<Vec<u8>> {
        let response = self.clients[1]
            .lock()
            .await
            .control(0x14, &(capacity as u16).to_be_bytes())
            .await?;
        ensure!(
            (4..=4 + capacity).contains(&response.len()),
            "invalid UART READ response"
        );
        Ok(response[4..].to_vec())
    }
    async fn heartbeat(&self) -> Result<()> {
        loop {
            sleep(Duration::from_millis(500)).await;
            for client in &self.clients {
                if let Ok(mut client) = client.try_lock() {
                    ensure!(
                        client
                            .request_timeout(7, &[], 7, Duration::from_millis(1500))
                            .await?
                            .is_empty(),
                        "invalid KEEPALIVE response"
                    );
                }
            }
        }
    }
}

struct State {
    pending: VecDeque<Arc<Urb>>,
    responses: VecDeque<Vec<u8>>,
    notification: Vec<u8>,
    coding: [u8; 7],
    line_state: u16,
    configuration: u8,
    sender: mpsc::Sender<Vec<u8>>,
    space: Arc<Notify>,
}
impl State {
    fn first_input(&self, ep: u32) -> Option<Arc<Urb>> {
        self.pending
            .iter()
            .find(|u| u.ep == ep && u.direction == 1)
            .cloned()
    }
    fn present(&self, urb: &Arc<Urb>) -> bool {
        self.pending.iter().any(|u| Arc::ptr_eq(u, urb))
    }
    fn send(&self, packet: Vec<u8>) -> Result<()> {
        self.sender
            .try_send(packet)
            .map_err(|_| anyhow::anyhow!("USB/IP peer stopped reading"))
    }
    fn complete(&mut self, urb: &Arc<Urb>, data: &[u8], status: i32) -> Result<()> {
        let Some(index) = self.pending.iter().position(|u| Arc::ptr_eq(u, urb)) else {
            return Ok(());
        };
        self.pending.remove(index);
        let size = if status != 0 {
            0
        } else if urb.direction == 1 {
            data.len()
        } else {
            urb.size
        };
        let mut packet = vec![0; 48];
        packet[..4].copy_from_slice(&3u32.to_be_bytes());
        packet[4..8].copy_from_slice(&urb.seq.to_be_bytes());
        packet[20..24].copy_from_slice(&status.to_be_bytes());
        packet[24..28].copy_from_slice(&(size as u32).to_be_bytes());
        if status == 0 && urb.direction == 1 {
            packet.extend_from_slice(data);
        }
        self.send(packet)
    }
    fn flush(&mut self) -> Result<()> {
        while !self.responses.is_empty() {
            let Some(urb) = self.first_input(1) else {
                break;
            };
            let mut response = self.responses.pop_front().expect("nonempty queue");
            let size = response.len().min(urb.size);
            self.complete(&urb, &response[..size], 0)?;
            response.drain(..size);
            if !response.is_empty() {
                self.responses.push_front(response);
            }
        }
        if self.responses.len() < 4 {
            self.space.notify_one();
        }
        if !self.notification.is_empty()
            && let Some(urb) = self.first_input(2)
        {
            let size = urb.size.min(self.notification.len());
            let data: Vec<u8> = self.notification.drain(..size).collect();
            self.complete(&urb, &data, 0)?;
        }
        Ok(())
    }
}

async fn receive(
    mut reader: OwnedReadHalf,
    state: Arc<Mutex<State>>,
    queues: [mpsc::Sender<Arc<Urb>>; 3],
) -> Result<()> {
    loop {
        let mut header = [0; 48];
        reader.read_exact(&mut header).await?;
        let mut urb = Urb::parse(&header)?;
        if urb.command == 2 {
            let mut state = state.lock().unwrap();
            let index = state.pending.iter().position(|u| u.seq == urb.flags);
            let target = index.and_then(|i| state.pending.remove(i));
            let mut response = vec![0; 48];
            response[..4].copy_from_slice(&4u32.to_be_bytes());
            response[4..8].copy_from_slice(&urb.seq.to_be_bytes());
            response[20..24]
                .copy_from_slice(&(if target.is_some() { -104i32 } else { 0 }).to_be_bytes());
            state.send(response)?;
            ensure!(
                !target.is_some_and(|u| u.active.load(Ordering::Relaxed)),
                "in-flight write cancelled; discard uncertain result"
            );
            continue;
        }
        if urb.direction == 0 {
            urb.data.resize(urb.size, 0);
            timeout(Duration::from_secs(5), reader.read_exact(&mut urb.data)).await??;
        }
        let urb = Arc::new(urb);
        let mut state = state.lock().unwrap();
        ensure!(
            state.pending.len() < 128 && !state.pending.iter().any(|u| u.seq == urb.seq),
            "duplicate or excess URB"
        );
        state.pending.push_back(urb.clone());
        if urb.ep == 0 || (matches!(urb.ep, 1 | 3) && urb.direction == 0) {
            let index = if urb.ep == 3 { 2 } else { urb.ep as usize };
            if queues[index].try_send(urb.clone()).is_err() {
                state.complete(&urb, &[], -12)?;
            }
        } else if matches!(urb.ep, 1..=3) && urb.direction == 1 {
            if urb.size == 0 {
                state.complete(&urb, &[], 0)?;
            } else {
                state.flush()?;
            }
        } else {
            state.complete(&urb, &[], STALL)?;
        }
    }
}

async fn control(
    urb: &Arc<Urb>,
    state: &Arc<Mutex<State>>,
    descriptors: &Descriptors,
    backend: &Backend,
) -> Result<Option<Vec<u8>>> {
    let [kind, request, ..] = urb.setup;
    let value = u16le(&urb.setup[2..]);
    let index = u16le(&urb.setup[4..]);
    let size = u16le(&urb.setup[6..]) as usize;
    if urb.direction != (kind >> 7) as u32
        || size != urb.size
        || (urb.direction == 0 && size != urb.data.len())
    {
        return Ok(None);
    }
    let response = match (kind, request, value, index, size) {
        (0x80, 6, _, _, _) => descriptors.get(value, index).ok(),
        (0xc0, 0x20, 0, 7, _) => Some(descriptors.ms_os.clone()),
        (0x80, 8, 0, 0, 1) => Some(vec![state.lock().unwrap().configuration]),
        (0, 9, 0..=1, 0, 0) => {
            let mut state = state.lock().unwrap();
            ensure!(
                value != 0 || state.configuration == 0,
                "USB deconfigured; reconnect required"
            );
            state.configuration = value as u8;
            Some(vec![])
        }
        (0, 5, 0..=127, 0, 0) => Some(vec![]),
        (0x80, 0, 0, 0, 2)
        | (0x81, 0, 0, 0..=2, 2)
        | (0x82, 0, 0, 0 | 0x80 | 1 | 0x81 | 0x82 | 3 | 0x83, 2) => Some(vec![0, 0]),
        (0x81, 10, 0, 0..=2, 1) => Some(vec![0]),
        (1, 11, 0, 0..=2, 0) | (2, 1, 0, 1 | 0x81 | 0x82 | 3 | 0x83, 0) => Some(vec![]),
        (0xa1, 0x21, 0, 1, 7) => Some(state.lock().unwrap().coding.to_vec()),
        (0x21, 0x20, 0, 1, 7) => {
            let coding: [u8; 7] = urb.data[..].try_into()?;
            let baud = u32::from_le_bytes(coding[..4].try_into()?);
            if !(1..=5000000).contains(&baud)
                || coding[4] > 2
                || coding[5] > 2
                || !(5..=8).contains(&coding[6])
            {
                return Ok(None);
            }
            let active = state.lock().unwrap().line_state & 1 != 0;
            if active {
                backend.configure(coding).await?;
            }
            state.lock().unwrap().coding = coding;
            Some(vec![])
        }
        (0x21, 0x22, 0..=3, 1, 0) => {
            let (old, coding) = {
                let state = state.lock().unwrap();
                (state.line_state, state.coding)
            };
            if value & 1 != 0 && old & 1 == 0 {
                backend.configure(coding).await?;
            }
            let mut state = state.lock().unwrap();
            state.line_state = value;
            state.notification = bytes("a1200000010002000000");
            state.flush()?;
            Some(vec![])
        }
        _ => None,
    };
    Ok(response.map(|mut data| {
        data.truncate(size);
        data
    }))
}

async fn worker(
    ep: u32,
    mut queue: mpsc::Receiver<Arc<Urb>>,
    state: Arc<Mutex<State>>,
    descriptors: Arc<Descriptors>,
    backend: Arc<Backend>,
) -> Result<()> {
    while let Some(urb) = queue.recv().await {
        {
            let state = state.lock().unwrap();
            if !state.present(&urb) {
                continue;
            }
            urb.active.store(true, Ordering::Relaxed);
        }
        if ep == 0 {
            let result = control(&urb, &state, &descriptors, &backend).await?;
            state.lock().unwrap().complete(
                &urb,
                result.as_deref().unwrap_or(&[]),
                if result.is_some() { 0 } else { STALL },
            )?;
        } else if ep == 1 {
            if !(1..=508).contains(&urb.size) {
                state.lock().unwrap().complete(&urb, &[], STALL)?;
                continue;
            }
            loop {
                let space = {
                    let state = state.lock().unwrap();
                    if state.responses.len() < 4 {
                        break;
                    }
                    state.space.clone()
                };
                space.notified().await;
            }
            if !state.lock().unwrap().present(&urb) {
                continue;
            }
            let response = backend.dap(&urb.data).await?;
            let mut state = state.lock().unwrap();
            state.responses.push_back(response);
            state.complete(&urb, &[], 0)?;
            state.flush()?;
        } else {
            if state.lock().unwrap().line_state & 1 == 0 {
                state.lock().unwrap().complete(&urb, &[], STALL)?;
                continue;
            }
            backend.write(&urb.data).await?;
            state.lock().unwrap().complete(&urb, &[], 0)?;
        }
    }
    Ok(())
}

async fn uart_reader(state: Arc<Mutex<State>>, backend: Arc<Backend>) -> Result<()> {
    let mut buffered = Vec::new();
    loop {
        let urb = state.lock().unwrap().first_input(3);
        if let Some(urb) = urb {
            if buffered.is_empty() {
                buffered = backend.read(urb.size.min(256)).await?;
            }
            let mut state = state.lock().unwrap();
            if let Some(urb) = state.first_input(3)
                && !buffered.is_empty()
            {
                let size = urb.size.min(buffered.len());
                state.complete(&urb, &buffered[..size], 0)?;
                buffered.drain(..size);
                continue;
            }
        }
        sleep(Duration::from_millis(10)).await;
    }
}

async fn session(socket: TcpStream, descriptors: Arc<Descriptors>, backend: Backend) -> Result<()> {
    let (reader, mut writer) = socket.into_split();
    let backend = Arc::new(backend);
    let (sender, mut receiver) = mpsc::channel::<Vec<u8>>(128);
    let state = Arc::new(Mutex::new(State {
        pending: VecDeque::new(),
        responses: VecDeque::new(),
        notification: vec![],
        coding: [0, 0xc2, 1, 0, 0, 0, 8],
        line_state: 0,
        configuration: 0,
        sender,
        space: Arc::new(Notify::new()),
    }));
    let mut tasks = JoinSet::new();
    let mut queues = Vec::new();
    for ep in [0, 1, 3] {
        let (tx, rx) = mpsc::channel(if ep == 1 { 4 } else { 32 });
        queues.push(tx);
        tasks.spawn(worker(
            ep,
            rx,
            state.clone(),
            descriptors.clone(),
            backend.clone(),
        ));
    }
    tasks.spawn(receive(
        reader,
        state.clone(),
        queues
            .try_into()
            .map_err(|_| anyhow::anyhow!("queue count"))?,
    ));
    tasks.spawn(uart_reader(state, backend.clone()));
    tasks.spawn(async move { backend.heartbeat().await });
    tasks.spawn(async move {
        while let Some(packet) = receiver.recv().await {
            timeout(Duration::from_secs(5), writer.write_all(&packet)).await??;
        }
        Ok(())
    });
    let result = tasks.join_next().await.unwrap()?;
    tasks.shutdown().await;
    result
}

struct ImportGuard(Arc<AtomicBool>);
impl Drop for ImportGuard {
    fn drop(&mut self) {
        self.0.store(false, Ordering::Release);
    }
}

async fn handle(
    mut socket: TcpStream,
    descriptors: Arc<Descriptors>,
    imported: Arc<AtomicBool>,
    host: String,
    credential: Credential,
    ports: [u16; 2],
) -> Result<()> {
    socket.set_nodelay(true)?;
    let mut header = [0; 8];
    timeout(Duration::from_secs(5), socket.read_exact(&mut header)).await??;
    ensure!(
        header[..2] == [1, 0x11] && header[4..] == [0; 4],
        "invalid USB/IP negotiation"
    );
    let operation = u16::from_be_bytes(header[2..4].try_into()?);
    if operation == 0x8005 {
        let available = !imported.load(Ordering::Acquire);
        let mut reply = bytes("0111000500000000");
        reply.extend_from_slice(&(available as u32).to_be_bytes());
        if available {
            reply.extend(descriptors.record());
            reply.extend(bytes("ff000000020201000a000000"));
        }
        timeout(Duration::from_secs(5), socket.write_all(&reply)).await??;
        return Ok(());
    }
    ensure!(operation == 0x8003, "unsupported USB/IP negotiation");
    let mut busid = [0; 32];
    timeout(Duration::from_secs(5), socket.read_exact(&mut busid)).await??;
    let mut expected = [0; 32];
    expected[..3].copy_from_slice(b"1-1");
    if busid != expected
        || imported
            .compare_exchange(false, true, Ordering::AcqRel, Ordering::Acquire)
            .is_err()
    {
        socket.write_all(&bytes("0111000300000001")).await?;
        return Ok(());
    }
    let _guard = ImportGuard(imported);
    let backend = match Backend::connect(&host, &credential, ports).await {
        Ok(backend) => backend,
        Err(error) => {
            socket.write_all(&bytes("0111000300000001")).await?;
            return Err(error);
        }
    };
    let mut reply = bytes("0111000300000000");
    reply.extend(descriptors.record());
    timeout(Duration::from_secs(5), socket.write_all(&reply)).await??;
    session(socket, descriptors, backend).await
}

pub async fn serve(
    listener: TcpListener,
    host: String,
    credential: Credential,
    ports: [u16; 2],
    imported: Arc<AtomicBool>,
    mut stop: watch::Receiver<bool>,
) -> Result<()> {
    let descriptors = Arc::new(Descriptors::new(&credential.device_id)?);
    let slots = Arc::new(Semaphore::new(8));
    let mut connections = JoinSet::new();
    loop {
        tokio::select! {
            _ = stop.changed() => {break;}
            Some(_) = connections.join_next(), if !connections.is_empty() => {}
            accepted = listener.accept() => {
                let (socket, _) = accepted?;
                let Ok(permit) = slots.clone().try_acquire_owned() else {continue;};
                let (descriptors, imported, host, credential) = (descriptors.clone(), imported.clone(), host.clone(), credential.clone());
                connections.spawn(async move {
                    let _permit = permit;
                    if let Err(error) = handle(socket, descriptors, imported, host, credential, ports).await {
                        eprintln!("USB/IP connection ended: {error}");
                    }
                });
            }
        }
    }
    connections.shutdown().await;
    imported.store(false, Ordering::Release);
    Ok(())
}
