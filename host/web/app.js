'use strict';
const $ = id => document.getElementById(id);
const token = document.querySelector('meta[name="api-token"]').content;
let state = null, initial = true, dirty = false, submitting = false;
let lastInfo = '', lastDiscovered = '';
let lastBridgeError = '';
async function api(path, body, binary = false) {
  const headers = {'X-AirDAP-Token': token};
  const options = {headers, cache: 'no-store'};
  if (body !== undefined) {
    options.method = 'POST'; headers['Content-Type'] = binary ? 'application/octet-stream' : 'application/json';
    options.body = binary ? body : JSON.stringify(body);
  }
  const response = await fetch('/api/' + path, options);
  const result = await response.json();
  if (!response.ok) throw new Error(result.error || '服务请求失败');
  return result;
}
function notice(message) { $('notice').textContent = message || ''; $('notice').hidden = !message; }
function fields() { return {device_id: $('deviceId').value.trim(), host: $('host').value.trim()}; }
function selected() {
  if (dirty || !state?.profile.device_id || fields().device_id !== state.profile.device_id) throw new Error('请先保存当前设备连接信息。');
  return {device_id: state.profile.device_id};
}
async function action(fn) {
  if (submitting) return;
  submitting = true;
  try { notice(''); await fn(); await refresh(); } catch (error) { notice(error.message); }
  finally { submitting = false; }
}
function details(data) {
  const list = document.createElement('dl');
  const names = {device_id:'设备编号', host:'网络地址', firmware:'固件版本', uuid:'UUID', capabilities:'能力位', tls:'加密协议', transport:'查询通道', read_at:'读取时间', size:'镜像大小', version:'镜像版本', sha256:'SHA-256'};
  for (const [key, value] of Object.entries(data)) {
    if (key === 'details') continue;
    const dt = document.createElement('dt'), dd = document.createElement('dd');
    dt.textContent = names[key] || key; dd.textContent = String(value); list.append(dt, dd);
  }
  return list;
}
function render() {
  $('serviceBadge').textContent = '● 服务在线';
  const p = state.profile, b = state.bridge, busy = state.job.state === 'running';
  if (initial) { $('deviceId').value = p.device_id || ''; $('host').value = p.host || ''; $('autoAttach').checked = p.auto_attach !== false; initial = false; }
  $('bridgeState').textContent = b.imported ? '已挂载' : b.listening ? '等待挂载' : '已停止';
  $('bridgePort').textContent = `127.0.0.1:${b.port} · ${b.mount_port === null ? '无本服务挂载端口' : 'USB/IP 端口 ' + b.mount_port}`;
  $('deviceName').textContent = p.device_id || '尚未选择'; $('deviceHost').textContent = p.host || '保存设备后即可连接';
  $('firmwareVersion').textContent = state.info?.firmware || '尚未读取';
  $('infoSource').textContent = state.info ? '最近一次查询 · ' + state.info.transport : '点击读取设备信息';
  $('credentialStatus').textContent = state.credential_present ? '已保存该设备的网络凭据。' : '未保存网络凭据，请导入或通过蓝牙配对。';
  $('startBridge').disabled = busy || b.listening || !state.credential_present;
  $('stopBridge').disabled = busy || (!b.listening && !p.bridge_enabled && !b.error);
  for (const id of ['scanUsb','scanBle','infoNetwork','infoUsb','pairBle','stageImage']) $(id).disabled = busy || (['infoUsb','pairBle','stageImage'].includes(id) && b.listening);
  for (const form of ['profileForm','wifiForm']) for (const el of $(form).elements) el.disabled = busy || b.listening;
  $('runOta').disabled = busy || b.listening || !state.image;
  if (lastDiscovered !== JSON.stringify(state.discovered)) {
    lastDiscovered = JSON.stringify(state.discovered);
    $('discovered').replaceChildren();
    for (const d of state.discovered) {
      const button = document.createElement('button'); button.type = 'button'; button.className = 'secondary';
      button.textContent = d.device_id + ' · ' + d.transport.toUpperCase(); button.disabled = busy || b.listening;
      button.onclick = () => { $('deviceId').value = d.device_id; $('host').value = 'airdap-' + d.device_id.slice(4).toLowerCase() + '.local'; dirty = true; notice('已选择设备，请保存连接信息。'); };
      $('discovered').append(button);
    }
  }
  for (const button of $('discovered').querySelectorAll('button')) button.disabled = busy || b.listening;
  if (lastInfo !== JSON.stringify(state.info)) {
    lastInfo = JSON.stringify(state.info);
    $('deviceInfo').replaceChildren();
    if (state.info) {
      $('deviceInfo').append(details(state.info));
      for (const [key, value] of Object.entries(state.info.details || {})) {
        const block = document.createElement('details'), summary = document.createElement('summary'), pre = document.createElement('pre');
        summary.textContent = key; pre.textContent = value; block.append(summary, pre); $('deviceInfo').append(block);
      }
    } else {
      const empty = document.createElement('p'); empty.className = 'empty'; empty.textContent = '尚未读取设备信息'; $('deviceInfo').append(empty);
    }
  }
  if (state.image) $('imageInfo').replaceChildren(...details(state.image).childNodes);
  else { $('imageInfo').replaceChildren(); const dt = document.createElement('dt'), dd = document.createElement('dd'); dt.textContent = '待升级镜像'; dd.textContent = '尚未上传'; $('imageInfo').append(dt, dd); }
  const job = state.job;
  $('jobMessage').textContent = job.state === 'idle' ? '暂无操作' : `${job.action} · ${job.message}`;
  $('progress').value = job.progress || 0; $('jobResult').hidden = !job.result;
  $('jobResult').textContent = job.result ? JSON.stringify(job.result, null, 2) : '';
  if (b.error) notice(b.error);
  else if (lastBridgeError && $('notice').textContent === lastBridgeError) notice('');
  lastBridgeError = b.error || '';
}
async function refresh() { state = await api('state'); render(); }
$('profileForm').addEventListener('input', () => { dirty = true; });
$('profileForm').onsubmit = event => { event.preventDefault(); action(async () => {
  const record = {...fields(), auto_attach: $('autoAttach').checked};
  if ($('credential').files[0]) { if ($('credential').files[0].size > 8192) throw new Error('凭据文件过大。'); record.credential = JSON.parse(await $('credential').files[0].text()); }
  await api('profile', record); $('credential').value = ''; dirty = false;
}); };
for (const [id, transport] of [['scanUsb','usb'],['scanBle','ble']]) $(id).onclick = () => action(() => api('discover', {transport}));
for (const [id, transport] of [['infoNetwork','network'],['infoUsb','usb']]) $(id).onclick = () => action(() => api('info', {...selected(), transport}));
$('startBridge').onclick = () => action(() => { selected(); return api('start', {}); });
$('stopBridge').onclick = () => action(async () => { if (confirm('停止桥接会断开虚拟 USB 和 COM 口。请确认烧录和串口操作已经结束。')) await api('stop', {}); });
$('pairBle').onclick = () => action(async () => { const target = selected(); if (confirm(`为 ${target.device_id} 写入本机网络凭据？请先开启设备蓝牙配网窗口。其他主机的旧凭据可能失效。`)) await api('pair', {...target, confirm:true}); });
$('wifiForm').onsubmit = event => { event.preventDefault(); action(async () => {
  const target = selected(), ssid = $('ssid').value, password = $('password').value;
  if (!confirm(`将 ${target.device_id} 的 Wi-Fi 改为「${ssid}」？当前网络连接会中断。`)) return;
  try { await api('wifi', {...target, transport:$('wifiTransport').value, ssid, password, confirm:true}); }
  finally { $('password').value = ''; }
}); };
$('stageImage').onclick = () => action(async () => { const file = $('imageFile').files[0]; if (!file) throw new Error('请先选择 airdap.bin。'); if (file.size > 8*1024*1024) throw new Error('应用镜像不能超过 8 MiB。'); await api('image', await file.arrayBuffer(), true); });
$('runOta').onclick = () => action(async () => {
  const target = selected(), image = state.image;
  if (!image) throw new Error('请先上传镜像。');
  if (confirm(`升级设备 ${target.device_id}\n镜像版本：${image.version}\n大小：${image.size} 字节\nSHA-256：${image.sha256}\n\n设备将写入非活动分区并重启，确认继续？`)) await api('ota', {...target, transport:$('otaTransport').value, sha256:image.sha256, confirm:true});
});
async function poll() { try { await refresh(); } catch (error) { $('serviceBadge').textContent = '服务连接中断'; notice(error.message); } finally { setTimeout(poll, 1500); } }
poll();
