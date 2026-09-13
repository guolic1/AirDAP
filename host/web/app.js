'use strict';
const $ = id => document.getElementById(id);
const token = document.querySelector('meta[name="api-token"]').content;
let state = null, initial = true, dirty = false, submitting = false;
let lastInfo = '', lastDiscovered = '';
let lastBridgeError = '';
let provisionOpen = false, lastNetworks = '', selectedAp = null;
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
function notice(message) { $('provisionError').textContent = message || ''; $('provisionError').hidden = !message; $('notice').textContent = message || ''; $('notice').hidden = !message; }
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
  const configuring = !!state.provisioning?.active;
  if (initial) { $('deviceId').value = p.device_id || ''; $('host').value = p.host || ''; $('autoAttach').checked = p.auto_attach !== false; initial = false; }
  $('bridgeState').textContent = b.imported ? '已挂载' : b.listening ? '等待挂载' : '已停止';
  $('bridgePort').textContent = `127.0.0.1:${b.port} · ${b.mount_port === null ? '无本服务挂载端口' : 'USB/IP 端口 ' + b.mount_port}`;
  $('deviceName').textContent = p.device_id || '尚未选择'; $('deviceHost').textContent = p.host || '保存设备后即可连接';
  $('firmwareVersion').textContent = state.info?.firmware || '尚未读取';
  $('infoSource').textContent = state.info ? '最近一次查询 · ' + state.info.transport : '点击读取设备信息';
  $('credentialStatus').textContent = state.credential_present ? '已保存该设备的网络凭据。' : '未保存网络凭据，请导入或在配网窗口建立凭据。';
  $('startBridge').disabled = busy || configuring || b.listening || !state.credential_present;
  $('stopBridge').disabled = busy || (!b.listening && !p.bridge_enabled && !b.error);
  for (const id of ['scanUsb','scanBle','infoNetwork','infoUsb','stageImage']) $(id).disabled = busy || configuring || (['infoUsb','stageImage'].includes(id) && b.listening);
  for (const form of ['profileForm']) for (const el of $(form).elements) el.disabled = busy || configuring || b.listening;
  $('runOta').disabled = busy || configuring || b.listening || !state.image;
  if (lastDiscovered !== JSON.stringify(state.discovered)) {
    lastDiscovered = JSON.stringify(state.discovered);
    $('discovered').replaceChildren();
    for (const d of state.discovered) {
      const button = document.createElement('button'); button.type = 'button'; button.className = 'secondary';
      button.textContent = d.device_id + ' · ' + d.transport.toUpperCase(); button.disabled = busy || configuring || b.listening;
      button.onclick = () => { $('deviceId').value = d.device_id; $('host').value = 'airdap-' + d.device_id.slice(4).toLowerCase() + '.local'; dirty = true; notice('已选择设备，请保存连接信息。'); };
      $('discovered').append(button);
    }
  }
  for (const button of $('discovered').querySelectorAll('button')) button.disabled = busy || configuring || b.listening;
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
  renderProvisioning(busy);
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
function sessionTarget() {
  const p = state.provisioning;
  return {device_id:p.device_id, transport:p.transport};
}
function renderProvisioning(busy) {
  const p = state.provisioning || {active:false, networks:null};
  if (p.active) provisionOpen = true;
  if (provisionOpen && !$('provisionDialog').open) $('provisionDialog').showModal();
  $('beginProvision').disabled = busy || p.active || state.bridge.listening;
  $('wifiTransport').disabled = busy || p.active;
  $('provisionActions').hidden = !p.active;
  $('provisionBadge').textContent = p.active ? '配网连接已建立' : busy ? '正在连接' : '尚未连接';
  $('provisionDevice').textContent = `${p.device_id || state.profile.device_id || ''} · ${(p.transport || $('wifiTransport').value) === 'usb' ? 'USB' : '蓝牙'}`;
  $('provisionMessage').textContent = state.job.state === 'idle' ? '' : `${state.job.action} · ${state.job.message}`;
  if (state.job.state === 'succeeded' && state.job.result?.wifi === 'online') $('provisionMessage').textContent = 'Wi-Fi 已连接，设备已取得 IP。可以继续操作或取消配网。';
  if (state.job.state === 'succeeded' && state.job.result?.paired) $('provisionMessage').textContent = '本机网络凭据已保存并写入设备。可以继续连接 Wi-Fi。';
  if (provisionOpen && state.job.state === 'failed') {
    $('provisionError').textContent = state.job.message; $('provisionError').hidden = false;
  }
  if (p.error) { $('provisionError').textContent = p.error; $('provisionError').hidden = false; }
  for (const id of ['sessionPair','sessionScan','rescanWifi']) $(id).disabled = busy || !p.active;
  // Writes run to completion; cancellation releases the session afterwards.
  $('cancelProvision').disabled = busy;
  for (const el of $('sessionWifiForm').elements) el.disabled = busy;
  const serialized = JSON.stringify(p.networks);
  if (lastNetworks !== serialized) {
    lastNetworks = serialized; selectedAp = null;
    $('sessionWifiForm').hidden = true; $('sessionPassword').value = '';
    $('wifiList').replaceChildren();
    for (const ap of p.networks || []) {
      const button = document.createElement('button'); button.type = 'button'; button.className = 'wifi-ap secondary';
      const name = document.createElement('strong'), info = document.createElement('small');
      name.textContent = ap.ssid || '隐藏网络';
      info.textContent = `${ap.rssi} dBm · 信道 ${ap.channel} · ${ap.auth} · ${ap.bssid}`;
      button.append(name, info); button.dataset.selectable = String(!!ap.ssid);
      button.onclick = () => {
        selectedAp = ap; $('sessionSsid').value = ap.ssid; $('sessionPassword').value = '';
        $('sessionWifiForm').hidden = false;
        for (const row of $('wifiList').children) row.setAttribute('aria-pressed', String(row === button));
        $('sessionPassword').focus();
      };
      $('wifiList').append(button);
    }
  }
  $('wifiNetworks').hidden = p.networks === null;
  $('scanSummary').textContent = p.networks?.length ? `发现 ${p.networks.length} 个热点，点击名称输入密码。` : '未发现 Wi-Fi，可以重新扫描。';
  for (const button of $('wifiList').children) button.disabled = busy || button.dataset.selectable !== 'true';
}
$('beginProvision').onclick = () => action(async () => {
  const target = selected();
  await api('provision-start', {...target, transport:$('wifiTransport').value});
  provisionOpen = true; lastNetworks = ''; selectedAp = null;
});
$('sessionPair').onclick = () => action(async () => {
  if (confirm(`为 ${sessionTarget().device_id} 建立并保存本机网络凭据？不同凭据会使其他主机的旧凭据失效。`))
    await api('provision-pair', {...sessionTarget(), confirm:true});
});
for (const id of ['sessionScan','rescanWifi']) $(id).onclick = () => action(() => api('provision-scan', sessionTarget()));
$('sessionWifiForm').onsubmit = event => { event.preventDefault(); action(async () => {
  if (!selectedAp) throw new Error('请先选择 Wi-Fi。');
  const password = $('sessionPassword').value;
  try { await api('provision-wifi', {...sessionTarget(), ssid:selectedAp.ssid, password, confirm:true}); }
  finally { $('sessionPassword').value = ''; }
}); };
$('cancelProvision').onclick = () => action(async () => {
  if (state.provisioning?.active) {
    await api('provision-cancel', sessionTarget());
    do { await new Promise(resolve => setTimeout(resolve, 150)); state = await api('state'); }
    while (state.job.state === 'running');
  }
  provisionOpen = false; selectedAp = null; $('sessionPassword').value = ''; $('provisionDialog').close();
});
$('provisionDialog').addEventListener('cancel', event => event.preventDefault());
$('stageImage').onclick = () => action(async () => { const file = $('imageFile').files[0]; if (!file) throw new Error('请先选择 airdap.bin。'); if (file.size > 8*1024*1024) throw new Error('应用镜像不能超过 8 MiB。'); await api('image', await file.arrayBuffer(), true); });
$('runOta').onclick = () => action(async () => {
  const target = selected(), image = state.image;
  if (!image) throw new Error('请先上传镜像。');
  if (confirm(`升级设备 ${target.device_id}\n镜像版本：${image.version}\n大小：${image.size} 字节\nSHA-256：${image.sha256}\n\n设备将写入非活动分区并重启，确认继续？`)) await api('ota', {...target, transport:$('otaTransport').value, sha256:image.sha256, confirm:true});
});
async function poll() { try { await refresh(); } catch (error) { $('serviceBadge').textContent = '服务连接中断'; notice(error.message); } finally { setTimeout(poll, 1500); } }
poll();
