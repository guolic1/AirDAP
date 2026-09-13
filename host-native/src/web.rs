use crate::service::Service;
use axum::{
    Router,
    body::{Body, to_bytes},
    extract::{Request, State},
    http::{Method, StatusCode},
    response::Response,
};
use serde_json::{Value, json};
use std::{sync::Arc, time::Duration};

#[derive(Clone)]
struct Web {
    service: Arc<Service>,
    port: u16,
    token: String,
    slots: Arc<tokio::sync::Semaphore>,
}
pub fn router(service: Arc<Service>, port: u16) -> anyhow::Result<Router> {
    let mut token = [0; 32];
    openssl::rand::rand_bytes(&mut token)?;
    Ok(Router::new().fallback(handle).with_state(Web {
        service,
        port,
        token: hex::encode(token),
        slots: Arc::new(tokio::sync::Semaphore::new(8)),
    }))
}
fn reply(status: u16, body: String, content_type: &str) -> Response {
    Response::builder().status(status)
        .header("Content-Type",content_type).header("Cache-Control","no-store").header("X-Content-Type-Options","nosniff")
        .header("Referrer-Policy","no-referrer").header("X-Frame-Options","DENY")
        .header("Content-Security-Policy","default-src 'self'; script-src 'self'; style-src 'self'; connect-src 'self'; img-src 'self' data:; frame-ancestors 'none'; base-uri 'none'; form-action 'self'")
        .body(Body::from(body)).unwrap()
}
fn json_reply(status: u16, value: Value) -> Response {
    reply(status, value.to_string(), "application/json; charset=utf-8")
}
fn error(status: u16, message: &str) -> Response {
    json_reply(status, json!({"error":message}))
}
async fn handle(State(web): State<Web>, request: Request) -> Response {
    let Ok(_slot) = web.slots.try_acquire() else {
        return error(503, "服务繁忙，请稍后重试");
    };
    let path = request.uri().path().to_owned();
    let headers = request.headers();
    let method = request.method().clone();
    let header = |key: &str| headers.get(key).and_then(|v| v.to_str().ok()).unwrap_or("");
    let host = header("host");
    let allowed_host = host == format!("127.0.0.1:{}", web.port)
        || host == format!("localhost:{}", web.port)
        || (web.port == 80 && matches!(host, "127.0.0.1" | "localhost"));
    let api = path.starts_with("/api/") || method != Method::GET;
    if !allowed_host
        || headers.get_all("host").iter().count() != 1
        || (headers.contains_key("origin") && header("origin") != format!("http://{host}"))
        || matches!(header("sec-fetch-site"), "cross-site" | "same-site")
        || (api
            && (header("x-airdap-token").len() != web.token.len()
                || !openssl::memcmp::eq(header("x-airdap-token").as_bytes(), web.token.as_bytes())))
    {
        return error(403, "仅允许本机管理页的同源请求，请刷新页面后重试");
    }
    if request.uri().query().is_some() {
        return error(404, "未知页面");
    }
    if method == Method::GET {
        return match path.as_str() {
            "/api/state" => json_reply(200, web.service.snapshot()),
            "/" => reply(
                200,
                include_str!("../../host/web/index.html").replace("__AIRDAP_TOKEN__", &web.token),
                "text/html; charset=utf-8",
            ),
            "/app.js" => reply(
                200,
                include_str!("../../host/web/app.js").into(),
                "text/javascript; charset=utf-8",
            ),
            "/style.css" => reply(
                200,
                include_str!("../../host/web/style.css").into(),
                "text/css; charset=utf-8",
            ),
            _ => error(404, "未找到页面"),
        };
    }
    if method != Method::POST {
        return error(StatusCode::METHOD_NOT_ALLOWED.as_u16(), "不支持的请求方法");
    }
    if !path.starts_with("/api/") {
        return error(404, "未知操作");
    }
    let image = path == "/api/image";
    let limit = if image { 8 * 1024 * 1024 } else { 16384 };
    if headers.contains_key("transfer-encoding")
        || headers.get_all("content-length").iter().count() != 1
    {
        return error(400, "请求长度无效");
    }
    let expected_type = if image {
        "application/octet-stream"
    } else {
        "application/json"
    };
    if header("content-type").split(';').next() != Some(expected_type) {
        return error(415, "请求内容类型无效");
    }
    let Ok(size) = header("content-length").parse::<usize>() else {
        return error(400, "请求长度无效");
    };
    if size == 0 || size > limit {
        return error(413, "请求过大或为空");
    }
    let bytes = match tokio::time::timeout(
        Duration::from_secs(10),
        to_bytes(request.into_body(), limit),
    )
    .await
    {
        Ok(Ok(bytes)) if bytes.len() == size => zeroize::Zeroizing::new(bytes.to_vec()),
        _ => return error(400, "请求不完整或超时"),
    };
    let result = if image {
        web.service.stage_image(bytes.to_vec())
    } else {
        let value: Value = match serde_json::from_slice(&bytes) {
            Ok(v) => v,
            Err(_) => return error(400, "请求 JSON 格式无效"),
        };
        web.service.command(&path[5..], value).await
    };
    match result {
        Ok(v) => json_reply(if v["accepted"] == true { 202 } else { 200 }, v),
        Err(e) => error(409, &e.to_string()),
    }
}
