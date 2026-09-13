"""Persistent local AirDAP service and bounded, same-origin HTTP management API."""
from __future__ import annotations

import asyncio
import hashlib
import hmac
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import io
import ipaddress
import json
import logging
import os
from pathlib import Path
import re
import secrets
import shutil
import subprocess
import tempfile
import threading
import time

from airdap_service_devices import Devices, ServiceError, device_id, net_update, wifi_fields
from airdap_network_credential import NetworkCredential, load_credential, load_or_create_credential
from airdap_usbip import NetworkBackend, Server

WEB = Path(__file__).with_name('web')
MAX_IMAGE = 8 * 1024 * 1024
LOG = logging.getLogger('airdap-service')


def atomic_write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    fd, name = tempfile.mkstemp(prefix='.airdap-', dir=path.parent)
    try:
        with os.fdopen(fd, 'wb') as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(name, path)
    finally:
        if os.path.exists(name):
            os.unlink(name)


def valid_profile(value):
    if not isinstance(value, dict):
        raise ServiceError('设备配置必须是 JSON 对象。')
    serial = device_id(value.get('device_id'))
    host = value.get('host', '')
    if not isinstance(host, str) or not host or len(host) > 253:
        raise ServiceError('请填写设备 IP 或主机名，不要包含 URL、空格或端口。')
    try:
        ipaddress.ip_address(host)
    except ValueError:
        if any(not re.fullmatch(r'[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?', label)
               for label in host.rstrip('.').split('.')):
            raise ServiceError('请填写设备 IP 或主机名，不要包含 URL、空格或端口。')
    for key in ('bridge_enabled', 'auto_attach'):
        if key in value and type(value[key]) is not bool:
            raise ServiceError('自动启动选项必须是布尔值。')
    return {'device_id': serial, 'host': host, 'bridge_enabled': value.get('bridge_enabled', False),
            'auto_attach': value.get('auto_attach', True)}


class Store:
    def __init__(self, path):
        self.path = Path(path).resolve()
        self.path.mkdir(parents=True, exist_ok=True, mode=0o700)
        self.profile = {}
        config = self.path / 'config.json'
        if config.exists():
            self.profile = valid_profile(json.loads(config.read_text(encoding='utf-8')))

    def save(self, profile):
        profile = valid_profile(profile)
        atomic_write(self.path / 'config.json', json.dumps(profile).encode('utf-8'))
        self.profile = profile

    def credential_path(self, serial=None):
        return self.path / 'credentials' / (device_id(serial or self.profile.get('device_id')) + '.json')

    def credential(self):
        return load_credential(self.credential_path())


class Mount:
    """Only detach this service's loopback export, never all system USB/IP ports."""
    def __init__(self, port, executable=None):
        default = Path(os.environ.get('ProgramFiles', 'C:/Program Files')) / 'USBip' / 'usbip.exe'
        self.executable = executable or shutil.which('usbip') or (str(default) if os.name == 'nt' and default.is_file() else None)
        self.port = port
        self.number = None

    def run(self, *args):
        if not self.executable:
            raise ServiceError('未找到 USB/IP 客户端：Windows 需安装 usbip-win2；Linux 需安装 usbip 并加载 vhci_hcd。')
        result = subprocess.run([self.executable, *args], capture_output=True, timeout=20,
            creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
        if result.returncode:
            raise ServiceError('USB/IP 客户端操作失败，请检查驱动、权限和端口占用。')
        return result.stdout.decode('utf-8', 'replace')

    def matching_ports(self):
        text = self.run('port')
        result = []
        for block in re.split(r'(?m)^Port ', text)[1:]:
            match = re.match(r'(\d+):', block)
            endpoint = f'usbip://127.0.0.1:{self.port}/1-1'
            if match and re.search(r'(?m)^\s*->\s*' + re.escape(endpoint) + r'\s*$', block):
                result.append(int(match[1]))
        return result

    def attach(self):
        existing = self.matching_ports()
        if existing:
            # A stale import of this exact export can survive an abrupt process exit.
            raise ServiceError('本服务地址已有 USB/IP 挂载，请先停止桥接清理后再启动。')
        args = ['-t', str(self.port), 'attach', '-r', '127.0.0.1', '-b', '1-1']
        if os.name == 'nt':
            args += ['--once', '--terse']
        output = self.run(*args)
        if os.name == 'nt' and output.strip().isdigit():
            self.number = int(output.strip())
        else:
            matches = self.matching_ports()
            if len(matches) != 1:
                raise ServiceError('客户端返回后无法确认挂载端口，请查看系统 USB/IP 状态。')
            self.number = matches[0]

    def detach(self):
        # Recheck ownership in case a user manually detached and reused the port.
        for number in self.matching_ports():
            self.run('detach', '-p', str(number))
        self.number = None


class Service:
    def __init__(self, store, *, usbip_port=3242, devices=None, usbip_executable=None):
        self.store = store
        self.usbip_port = usbip_port
        self.devices = devices or Devices()
        self.mount = Mount(usbip_port, usbip_executable)
        self.listener = self.bridge = None
        self.owns_export = False
        self.job_task = None
        self.job = {'state': 'idle'}
        self.info = None
        self.discovered = []
        self.image = None
        self.image_meta = None
        self.bridge_error = None
        self.closing = False
        self.retry_at = 0
        self.provision_session = None
        self.provisioning = {'active': False, 'networks': None}

    def no_provisioning(self):
        if self.provision_session:
            raise ServiceError('请先在配网窗口点击“取消配网”，再进行此操作。')

    def idle(self):
        if self.closing or (self.job_task and not self.job_task.done()):
            raise ServiceError('当前操作尚未结束，请等待完成后再操作。')

    def stopped(self):
        if self.listener:
            raise ServiceError('请先停止 USB 桥接并关闭烧录器、串口程序，再进行此操作。')

    def start_job(self, action, operation):
        self.idle()
        self.job = {'action': action, 'state': 'running', 'progress': 0, 'message': '正在执行'}
        async def run():
            try:
                result = await operation()
                self.job.update(state='succeeded', progress=100, message='操作完成', result=result)
            except Exception as error:
                message = str(error) if isinstance(error, ServiceError) else (
                    f'操作失败（{type(error).__name__}）。请检查设备、连接和服务权限；'
                    '配网、配对或升级可能已部分提交，请核对设备状态后再操作。')
                self.job.update(state='failed', message=message)
                LOG.warning('%s failed (%s)', action, type(error).__name__)
        self.job_task = asyncio.create_task(run())

    async def state(self):
        credential_present = bool(self.store.profile and self.store.credential_path().exists())
        session_error = getattr(self.provision_session, 'error', None)
        self.provisioning['error'] = session_error if isinstance(session_error, str) else None
        return {'profile': self.store.profile.copy(), 'credential_present': credential_present,
                'bridge': {'listening': self.listener is not None,
                           'imported': bool(self.bridge and self.bridge.imported),
                           'port': self.usbip_port, 'mount_port': self.mount.number,
                           'error': self.bridge_error}, 'job': self.job.copy(),
                'info': self.info, 'discovered': self.discovered, 'image': self.image_meta,
                'provisioning': self.provisioning.copy(),
                'usbip_available': self.mount.executable is not None}

    async def start_bridge(self):
        self.no_provisioning()
        if self.listener:
            return
        credential = self.store.credential()
        bridge = Server(credential.device_id,
            lambda: NetworkBackend(self.store.profile['host'], credential))
        self.listener = await asyncio.start_server(bridge.handle, '127.0.0.1', self.usbip_port)
        self.owns_export = True
        self.bridge = bridge
        self.usbip_port = self.listener.sockets[0].getsockname()[1]
        self.mount.port = self.usbip_port
        self.bridge_error = None
        if self.store.profile['auto_attach']:
            await self.attach()

    async def attach(self):
        try:
            await asyncio.to_thread(self.mount.detach)
            await asyncio.to_thread(self.mount.attach)
            self.bridge_error = None
        except Exception as error:
            self.bridge_error = str(error) if isinstance(error, ServiceError) else 'USB/IP 挂载失败，请检查驱动和权限。'
            self.retry_at = time.monotonic() + 15
            raise ServiceError(self.bridge_error) from None

    async def stop_bridge(self):
        if self.listener:
            self.listener.close()
        if self.bridge:
            await self.bridge.close()
            self.bridge = None
        if self.listener:
            # Python 3.13+ also waits for client transports in wait_closed().
            # Close bridge sessions first, otherwise an imported VHCI never drains.
            await self.listener.wait_closed()
            self.listener = None
        if self.mount.executable and self.owns_export:
            try:
                await asyncio.to_thread(self.mount.detach)
                self.bridge_error = None
                self.owns_export = False
            except Exception:
                self.bridge_error = '桥接已停止，但系统 USB/IP 挂载清理失败；请检查驱动状态。'

    async def maintain(self):
        """Reconnect virtual USB; never replay OTA/Wi-Fi writes or old DAP URBs."""
        while not self.closing:
            await asyncio.sleep(2)
            if (self.listener and self.store.profile.get('auto_attach') and not self.bridge.imported
                    and not (self.job_task and not self.job_task.done()) and time.monotonic() >= self.retry_at):
                self.start_job('重新挂载 USB', self.attach)

    async def command(self, action, data):
        self.idle()
        if not isinstance(data, dict):
            raise ServiceError('请求必须是 JSON 对象。')
        if action in ('profile', 'start', 'discover', 'info', 'wifi', 'pair', 'ota'):
            self.no_provisioning()
        if action == 'profile':
            self.stopped()
            profile = valid_profile(data)
            profile['bridge_enabled'] = False  # Enabling a bridge is an explicit action.
            if data.get('credential') is not None:
                try:
                    credential = NetworkCredential.from_json(json.dumps(data['credential']))
                except Exception:
                    raise ServiceError('凭据文件格式或校验无效。') from None
                if credential.device_id != profile['device_id']:
                    raise ServiceError('凭据中的设备编号与所选设备不一致。')
                atomic_write(self.store.credential_path(profile['device_id']), credential.to_json().encode())
            self.store.save(profile)
            self.info = None
            return {'saved': True}
        if action in ('start', 'stop'):
            if not self.store.profile:
                raise ServiceError('请先保存设备连接信息。')
            if action == 'start':
                self.store.credential()
            self.store.save(self.store.profile | {'bridge_enabled': action == 'start'})
            self.start_job(action, self.start_bridge if action == 'start' else self.stop_bridge)
            return {'accepted': True}
        if action == 'discover':
            transport = data.get('transport')
            if transport not in ('usb', 'ble'):
                raise ServiceError('请选择 USB 或蓝牙发现。')
            async def discover():
                self.discovered = await self.devices.discover(transport)
                return {'count': len(self.discovered)}
            self.start_job('发现设备', discover)
            return {'accepted': True}
        serial = device_id(data.get('device_id'))
        if serial != self.store.profile.get('device_id'):
            raise ServiceError('设备选择已变化，请先保存所选设备，再重新操作。')
        host = self.store.profile['host']
        transport = data.get('transport')
        if action.startswith('provision-'):
            self.stopped()
            return await self.provision_command(action, serial, transport, data)
        if action == 'info':
            if transport not in ('network', 'usb'):
                raise ServiceError('请选择网络或 USB 信息查询。')
            if transport == 'usb':
                self.stopped()
            credential = self.store.credential() if transport == 'network' else None
            async def info():
                self.info = None
                self.info = await self.devices.info(serial, host, credential, transport)
                self.info['read_at'] = time.strftime('%Y-%m-%d %H:%M:%S UTC', time.gmtime())
                return self.info
            self.start_job('读取设备信息', info)
        elif action in ('wifi', 'pair', 'ota'):
            self.stopped()
            if data.get('confirm') is not True:
                raise ServiceError('请明确确认目标设备和即将写入的配置或固件。')
            if action == 'pair':
                credential = load_or_create_credential(self.store.credential_path(), serial)
                self.start_job('蓝牙配对', lambda: self.devices.pair(credential))
            elif action == 'wifi':
                if transport not in ('usb', 'ble'):
                    raise ServiceError('配网仅支持 USB 或蓝牙。')
                ssid, password = data.get('ssid'), data.get('password')
                wifi_fields(ssid, password, transport)
                self.start_job('配置 Wi-Fi', lambda: self.devices.wifi(serial, transport, ssid, password))
            else:
                if transport not in ('usb', 'network'):
                    raise ServiceError('升级仅支持 USB 或加密网络。')
                if self.image is None or data.get('sha256') != self.image_meta['sha256']:
                    raise ServiceError('镜像未上传或已变化，请重新核对镜像。')
                credential = self.store.credential() if transport == 'network' else None
                loop = asyncio.get_running_loop()
                def progress(percent, message):
                    loop.call_soon_threadsafe(lambda: self.job.update(progress=percent, message=message))
                async def ota():
                    try:
                        return await self.devices.ota(serial, host, credential, transport, self.image, progress)
                    finally:
                        self.image = self.image_meta = None
                self.start_job('OTA 升级', ota)
        else:
            raise ServiceError('未知操作。')
        return {'accepted': True}

    async def provision_command(self, action, serial, transport, data):
        if action == 'provision-start':
            self.no_provisioning()
            if transport not in ('usb', 'ble'):
                raise ServiceError('配网仅支持 USB 或蓝牙。')
            self.provisioning = {'active':False, 'device_id':serial,
                                 'transport':transport, 'networks':None}
            async def connect():
                self.provision_session = await self.devices.open_provisioning(serial, transport)
                self.provisioning['active'] = True
                return {'connected':True}
            self.start_job('连接配网设备', connect)
            return {'accepted':True}
        session = self.provision_session
        if not session or serial != self.provisioning['device_id'] or transport != self.provisioning['transport']:
            raise ServiceError('配网会话不存在或设备、通道已变化，请重新开始配网。')
        if action == 'provision-cancel':
            async def cancel():
                try:
                    await session.close()
                finally:
                    self.provision_session = None
                    self.provisioning = {'active':False, 'networks':None}
                return {'cancelled':True}
            self.start_job('取消配网', cancel)
        elif action == 'provision-scan':
            self.provisioning['networks'] = None
            async def scan():
                self.provisioning['networks'] = await session.scan()
                return {'count':len(self.provisioning['networks'])}
            self.start_job('扫描 Wi-Fi', scan)
        elif action in ('provision-pair', 'provision-wifi'):
            if data.get('confirm') is not True:
                raise ServiceError('请确认要写入的凭据或 Wi-Fi 配置。')
            if action == 'provision-pair':
                credential = load_or_create_credential(self.store.credential_path(), serial)
                self.start_job('建立网络连接凭据', lambda: session.pair(credential))
            else:
                ssid, password = data.get('ssid'), data.get('password')
                wifi_fields(ssid, password, transport)
                if not any(ap['ssid'] == ssid for ap in self.provisioning.get('networks') or []):
                    raise ServiceError('请先扫描并选择 Wi-Fi。')
                self.start_job('连接 Wi-Fi', lambda: session.wifi(ssid, password))
        else:
            raise ServiceError('未知配网操作。')
        return {'accepted':True}

    async def stage_image(self, image):
        self.idle()
        self.no_provisioning()
        if not 80 <= len(image) <= MAX_IMAGE:
            raise ServiceError('应用镜像大小无效。')
        try:
            version = net_update.image_version(io.BytesIO(image))
        except Exception:
            raise ServiceError('请选择 ESP-IDF 应用镜像 airdap.bin，不能使用合并镜像或 bootloader。') from None
        if int.from_bytes(image[12:14], 'little') != 9:
            raise ServiceError('镜像芯片类型不是 ESP32-S3。')
        self.image = image
        self.image_meta = {'size': len(image), 'version': version, 'sha256': hashlib.sha256(image).hexdigest()}
        return self.image_meta

    async def close(self):
        self.closing = True
        if self.job_task:
            await self.job_task  # Do not cancel an in-flight flash/configuration write.
        if self.provision_session:
            try:
                await self.provision_session.close()
            finally:
                self.provision_session = None
        await self.stop_bridge()


class WebServer(ThreadingHTTPServer):
    daemon_threads = True
    request_queue_size = 8

    def __init__(self, address, service, loop):
        if address[0] != '127.0.0.1':
            raise ValueError('Web management must bind IPv4 loopback')
        self.service, self.loop = service, loop
        self.token = secrets.token_urlsafe(32)
        self.slots = threading.BoundedSemaphore(8)
        super().__init__(address, WebHandler)

    def process_request(self, request, address):
        if not self.slots.acquire(blocking=False):
            self.shutdown_request(request)
            return
        request.settimeout(10)
        try:
            super().process_request(request, address)
        except BaseException:
            self.slots.release()
            raise

    def process_request_thread(self, request, address):
        try:
            super().process_request_thread(request, address)
        finally:
            self.slots.release()


class WebHandler(BaseHTTPRequestHandler):
    server_version = 'AirDAP'

    def log_message(self, *_):
        pass  # Request bodies/paths may contain sensitive user input.

    def reply(self, status, value, content_type='application/json; charset=utf-8'):
        body = value if isinstance(value, bytes) else json.dumps(value, ensure_ascii=False).encode('utf-8')
        self.send_response(status)
        for key, val in {'Content-Type': content_type, 'Content-Length': str(len(body)),
            'Cache-Control': 'no-store', 'X-Content-Type-Options': 'nosniff',
            'Referrer-Policy': 'no-referrer', 'X-Frame-Options': 'DENY',
            'Content-Security-Policy': "default-src 'self'; script-src 'self'; style-src 'self'; connect-src 'self'; img-src 'self' data:; frame-ancestors 'none'; base-uri 'none'; form-action 'self'"}.items():
            self.send_header(key, val)
        self.end_headers()
        self.wfile.write(body)

    def permitted(self, api):
        hosts = {f'127.0.0.1:{self.server.server_port}', f'localhost:{self.server.server_port}'}
        if self.server.server_port == 80:
            hosts.update(('127.0.0.1', 'localhost'))
        host = self.headers.get('Host', '')
        origin = self.headers.get('Origin')
        if (host not in hosts or (origin is not None and origin != 'http://' + host)
                or self.headers.get('Sec-Fetch-Site') in ('cross-site', 'same-site')
                or (api and not hmac.compare_digest(self.headers.get('X-AirDAP-Token', ''), self.server.token))):
            self.reply(403, {'error': '仅允许本机管理页的同源请求，请刷新页面后重试。'})
            return False
        return True

    def call(self, coroutine):
        return asyncio.run_coroutine_threadsafe(coroutine, self.server.loop).result(timeout=15)

    def do_GET(self):
        if not self.permitted(self.path.startswith('/api/')):
            return
        if self.path == '/api/state':
            self.reply(200, self.call(self.server.service.state()))
            return
        assets = {'/': ('index.html', 'text/html; charset=utf-8'),
                  '/app.js': ('app.js', 'text/javascript; charset=utf-8'),
                  '/style.css': ('style.css', 'text/css; charset=utf-8')}
        if self.path not in assets:
            self.reply(404, {'error': '未找到页面。'})
            return
        name, content_type = assets[self.path]
        content = (WEB / name).read_bytes()
        if name == 'index.html':
            content = content.replace(b'__AIRDAP_TOKEN__', self.server.token.encode())
        self.reply(200, content, content_type)

    def do_POST(self):
        if not self.permitted(True):
            return
        if self.headers.get('Transfer-Encoding') or len(self.headers.get_all('Content-Length', [])) != 1:
            self.reply(400, {'error': '请求长度无效。'})
            return
        image_upload = self.path == '/api/image'
        expected = 'application/octet-stream' if image_upload else 'application/json'
        if self.headers.get('Content-Type', '').split(';')[0] != expected:
            self.reply(415, {'error': '请求内容类型无效。'})
            return
        try:
            size = int(self.headers['Content-Length'])
            if not 0 < size <= (MAX_IMAGE if image_upload else 16384):
                self.reply(413, {'error': '请求过大或为空。'})
                return
            body = self.rfile.read(size)
            if len(body) != size:
                raise ValueError('short body')
            if image_upload:
                result = self.call(self.server.service.stage_image(body))
            elif self.path.startswith('/api/'):
                result = self.call(self.server.service.command(self.path[5:], json.loads(body)))
            else:
                self.reply(404, {'error': '未知操作。'})
                return
            self.reply(202 if result.get('accepted') else 200, result)
        except ServiceError as error:
            self.reply(409, {'error': str(error)})
        except (ValueError, UnicodeError):
            self.reply(400, {'error': '请求格式无效。'})
        except Exception as error:
            LOG.warning('HTTP operation failed (%s)', type(error).__name__)
            self.reply(500, {'error': '服务操作失败，请检查配置、凭据文件及权限。'})
