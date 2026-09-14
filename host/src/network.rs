use crate::credential::Credential;
use anyhow::{Context, Result, bail, ensure};
use openssl::{
    memcmp,
    rand::rand_bytes,
    ssl::{Ssl, SslContext, SslMethod, SslVerifyMode, SslVersion},
};
use serde::Serialize;
use std::{pin::Pin, time::Duration};
use tokio::{
    io::{AsyncReadExt, AsyncWriteExt},
    net::TcpStream,
    time::timeout,
};
use tokio_openssl::SslStream;
use zeroize::Zeroizing;

pub const MAX_PAYLOAD: usize = 4096;
pub const CIPHER: &str = "TLS_AES_128_GCM_SHA256";

pub fn encode_frame(kind: u8, session: u32, sequence: u32, payload: &[u8]) -> Result<Vec<u8>> {
    ensure!(
        session != 0 && sequence != 0 && payload.len() <= MAX_PAYLOAD,
        "invalid AirDAP frame fields"
    );
    let mut frame = Vec::with_capacity(20 + payload.len());
    frame.extend_from_slice(b"ADAP");
    frame.extend_from_slice(&[1, kind, 0, 0]);
    frame.extend_from_slice(&session.to_be_bytes());
    frame.extend_from_slice(&sequence.to_be_bytes());
    frame.extend_from_slice(&(payload.len() as u16).to_be_bytes());
    frame.extend_from_slice(&[0, 0]);
    frame.extend_from_slice(payload);
    Ok(frame)
}

fn check_header(header: &[u8], session: u32, sequence: u32) -> Result<usize> {
    ensure!(
        header.len() == 20 && &header[..4] == b"ADAP" && header[4] == 1,
        "invalid AirDAP frame header"
    );
    ensure!(
        header[6..8] == [0, 0] && header[18..20] == [0, 0],
        "invalid AirDAP reserved fields"
    );
    ensure!(
        header[8..12] == session.to_be_bytes() && header[12..16] == sequence.to_be_bytes(),
        "AirDAP response session or sequence mismatch"
    );
    let size = u16::from_be_bytes(header[16..18].try_into()?) as usize;
    ensure!(size <= MAX_PAYLOAD, "AirDAP response exceeds frame limit");
    Ok(size)
}

pub fn decode_response(frame: &[u8], expected: u8, session: u32, sequence: u32) -> Result<Vec<u8>> {
    ensure!(frame.len() >= 20, "truncated AirDAP frame");
    let size = check_header(&frame[..20], session, sequence)?;
    ensure!(frame.len() == 20 + size, "AirDAP response length mismatch");
    if frame[5] == 8 {
        ensure!(size == 2, "invalid AirDAP error frame");
        bail!(
            "AirDAP error 0x{:04X}",
            u16::from_be_bytes(frame[20..22].try_into()?)
        );
    }
    ensure!(frame[5] == expected, "unexpected AirDAP response type");
    Ok(frame[20..].to_vec())
}

#[derive(Clone, Debug, Serialize)]
pub struct Hello {
    pub device_id: String,
    pub uuid: String,
    pub capabilities: u32,
    pub firmware: String,
}

pub fn parse_hello(payload: &[u8], expected: &str) -> Result<Hello> {
    ensure!(payload.len() > 36, "invalid AirDAP HELLO length");
    ensure!(
        &payload[20..36] == expected.as_bytes(),
        "HELLO device identity mismatch"
    );
    let firmware = std::str::from_utf8(&payload[36..]).context("invalid HELLO firmware version")?;
    Ok(Hello {
        device_id: expected.into(),
        uuid: hex::encode(&payload[..16]),
        capabilities: u32::from_be_bytes(payload[16..20].try_into()?),
        firmware: firmware.into(),
    })
}

pub struct Client {
    stream: SslStream<TcpStream>,
    session: u32,
    sequence: u32,
    valid: bool,
    pub owner: u32,
    pub token: Zeroizing<Vec<u8>>,
    pub hello: Hello,
    deadline: Duration,
}

impl Client {
    pub async fn connect(
        host: &str,
        port: u16,
        credential: &Credential,
        token: Option<&[u8]>,
        deadline: Duration,
    ) -> Result<Self> {
        let mut context = SslContext::builder(SslMethod::tls_client())?;
        context.set_min_proto_version(Some(SslVersion::TLS1_3))?;
        context.set_max_proto_version(Some(SslVersion::TLS1_3))?;
        context.set_ciphersuites(CIPHER)?;
        // Authentication is by the external PSK and HELLO identity, not a certificate.
        context.set_verify(SslVerifyMode::NONE);
        let credential_copy = credential.clone();
        context.set_psk_client_callback(move |_, hint, identity, key| {
            let expected = credential_copy.identity().as_bytes();
            if hint.is_some_and(|h| h != expected)
                || identity.len() <= expected.len()
                || key.len() < 32
            {
                return Ok(0);
            }
            identity[..expected.len()].copy_from_slice(expected);
            identity[expected.len()] = 0;
            key[..32].copy_from_slice(credential_copy.key());
            Ok(32)
        });
        let socket = timeout(deadline, TcpStream::connect((host, port)))
            .await
            .context("TCP connection timed out")??;
        socket.set_nodelay(true)?;
        let mut stream = SslStream::new(Ssl::new(&context.build())?, socket)?;
        timeout(deadline, Pin::new(&mut stream).connect())
            .await
            .context("TLS handshake timed out")??;
        ensure!(
            stream.ssl().version_str() == "TLSv1.3"
                && stream
                    .ssl()
                    .current_cipher()
                    .is_some_and(|c| c.name() == CIPHER),
            "unexpected TLS version or cipher"
        );
        let mut random = [0; 4];
        rand_bytes(&mut random)?;
        let mut client = Self {
            stream,
            session: u32::from_be_bytes(random).max(1),
            sequence: 0,
            valid: true,
            owner: 0,
            token: Zeroizing::new(Vec::new()),
            deadline,
            hello: Hello {
                device_id: credential.device_id.clone(),
                uuid: String::new(),
                capabilities: 0,
                firmware: String::new(),
            },
        };
        let hello = client.request(1, &[], 1).await?;
        client.hello = parse_hello(&hello, &credential.device_id)?;
        if let Some(join) = token {
            ensure!(
                join.is_empty() || join.len() == 32,
                "invalid owner token length"
            );
            let response = client.request(2, join, 2).await?;
            ensure!(response.len() == 36, "invalid AUTH response");
            client.owner = u32::from_be_bytes(response[..4].try_into()?);
            ensure!(client.owner != 0, "reserved AUTH owner");
            client.token.extend_from_slice(&response[4..]);
            ensure!(
                join.is_empty() || memcmp::eq(join, &client.token),
                "AUTH owner token mismatch"
            );
        }
        Ok(client)
    }

    pub async fn request(&mut self, kind: u8, payload: &[u8], expected: u8) -> Result<Vec<u8>> {
        self.request_timeout(kind, payload, expected, self.deadline)
            .await
    }
    pub async fn request_timeout(
        &mut self,
        kind: u8,
        payload: &[u8],
        expected: u8,
        deadline: Duration,
    ) -> Result<Vec<u8>> {
        ensure!(
            self.valid,
            "AirDAP connection is unusable after an interrupted request"
        );
        self.sequence = self.sequence.wrapping_add(1).max(1);
        let request = encode_frame(kind, self.session, self.sequence, payload)?;
        // A dropped future or failed frame leaves the stream poisoned. Never replay writes.
        self.valid = false;
        let result = timeout(deadline, async {
            self.stream.write_all(&request).await?;
            let mut header = [0; 20];
            self.stream.read_exact(&mut header).await?;
            let size = check_header(&header, self.session, self.sequence)?;
            let mut frame = header.to_vec();
            frame.resize(20 + size, 0);
            self.stream.read_exact(&mut frame[20..]).await?;
            decode_response(&frame, expected, self.session, self.sequence)
        })
        .await
        .context("AirDAP request timed out")??;
        self.valid = true;
        Ok(result)
    }
    pub async fn control(&mut self, opcode: u8, data: &[u8]) -> Result<Vec<u8>> {
        let mut payload = vec![opcode];
        payload.extend_from_slice(data);
        let response = self.request(5, &payload, 6).await?;
        ensure!(
            response.first() == Some(&opcode),
            "CONTROL response opcode mismatch"
        );
        Ok(response[1..].to_vec())
    }
}
