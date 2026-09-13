"""Device operations for the local service; reuse the checked host protocols."""
from __future__ import annotations

import asyncio
from contextlib import contextmanager
import importlib
import importlib.util
import io
import os
from pathlib import Path
import re
import secrets
import sys
import time

TOOLS = Path(__file__).resolve().parents[1] / 'firmware' / 'tools'
sys.path.insert(0, str(TOOLS))
from airdap_network_credential import DEVICE_ID_PATTERN
from airdap_usbip import uart


class ServiceError(RuntimeError):
    """A deliberately public, credential-free explanation."""


def load_tool(filename):
    name = 'airdap_service_' + filename.replace('-', '_').replace('.py', '')
    if name not in sys.modules:
        spec = importlib.util.spec_from_file_location(name, TOOLS / filename)
        module = importlib.util.module_from_spec(spec)
        sys.modules[name] = module
        spec.loader.exec_module(module)
    return sys.modules[name]


shell = load_tool('airdap-shell.py')
usb_update = load_tool('airdap-update.py')
net_update = load_tool('airdap-network-update.py')
pair = load_tool('airdap-pair.py')
provision = load_tool('airdap-provision.py')


def device_id(value):
    if not isinstance(value, str) or not DEVICE_ID_PATTERN.fullmatch(value):
        raise ServiceError('请选择有效的物理 AirDAP 设备编号（ADP- 后接 12 位大写十六进制）。')
    return value


def wifi_fields(ssid, password, transport):
    if not isinstance(ssid, str) or not isinstance(password, str):
        raise ServiceError('SSID 和密码必须是文本。')
    if not 1 <= len(ssid.encode('utf-8')) <= 32 or len(password.encode('utf-8')) > 64:
        raise ServiceError('SSID 必须为 1–32 字节，密码最多 64 字节。')
    if any(ord(c) < 32 or ord(c) == 127 for c in ssid + password):
        raise ServiceError('SSID 和密码不能包含控制字符。')
    if transport == 'usb' and any(ord(c) > 126 for c in ssid + password):
        raise ServiceError('当前 USB shell 配网仅支持可打印 ASCII；中文 SSID 请使用蓝牙。')


def physical_devices():
    import usb.core
    import usb.util
    found = []
    for candidate in usb_update._find_airdap_devices(usb.core):
        try:
            serial = usb.util.get_string(candidate, candidate.iSerialNumber)
            if serial and DEVICE_ID_PATTERN.fullmatch(serial):
                configuration = candidate.get_active_configuration()
                found.append({'device_id': serial, 'transport': 'usb',
                              'usb_wifi': any(i.bInterfaceNumber == 3 for i in configuration)})
        finally:
            usb.util.dispose_resources(candidate)
    return found


@contextmanager
def usb_transport(serial, *, debug=False):
    import usb.core
    import usb.util
    device_id(serial)
    candidates = usb_update._find_airdap_devices(usb.core)
    try:
        selected = usb_update.select_airdap_device(candidates, serial,
            lambda d: usb.util.get_string(d, d.iSerialNumber))
        cls = shell.VendorShellTransport if debug else usb_update.DapOtaTransport
        transport = cls(selected, usb.util, usb_core=usb.core)
        try:
            transport.open()
            if debug:
                transport.start_session()
                shell.read_until_prompt(transport, 5)
            yield transport
        finally:
            transport.close()
    finally:
        for candidate in candidates:
            usb.util.dispose_resources(candidate)


def usb_wifi(transport, ssid, password, *, timeout=40):
    """Feed only validated prompt input, never shell command arguments/history."""
    wifi_fields(ssid, password, 'usb')
    transport.write(b'wifi set\n')
    try:
        shell.read_until(transport, b'SSID: ', 5, 'SSID prompt')
        transport.write(ssid.encode('ascii') + b'\n')
        shell.read_until(transport, b'Password: ', 5, 'password prompt')
        transport.write(password.encode('ascii') + b'\n')
        response = shell.read_until_prompt(transport, 10)
        if b'wifi: credentials saved; reconnect requested\n' not in response:
            raise ServiceError('设备未确认保存 Wi-Fi 配置，请检查 USB 连接后重试。')
    except BaseException:
        transport.write(b'\x03')
        raise
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        response = shell.run_command(transport, 'wifi status', 5)
        if re.search(rb'\bwifi=online\b', response):
            return {'wifi': 'online'}
        time.sleep(1)
    raise ServiceError('Wi-Fi 配置已保存，但未在时限内取得 IP；请检查 SSID、密码和路由器。')


AUTH_MODES = ['Open', 'WEP', 'WPA_PSK', 'WPA2_PSK', 'WPA_WPA2_PSK',
              'WPA2_ENTERPRISE', 'WPA3_PSK', 'WPA2_WPA3_PSK']


class UsbProvisioning:
    def __init__(self, serial):
        self.serial, self.context, self.link = serial, None, None

    async def open(self):
        self.context = usb_transport(self.serial, debug=True)
        self.link = await asyncio.to_thread(self.context.__enter__)
        response = await asyncio.to_thread(shell.run_command, self.link, 'wifi capabilities', 5)
        if b'wifi-provision=1' not in response:
            raise ServiceError('当前固件不支持 USB 配网会话，请升级包含 Wi-Fi 扫描和凭据接口的固件。')

    async def close(self):
        if self.link is not None:
            try:
                await asyncio.to_thread(self.context.__exit__, None, None, None)
            finally:
                self.link = None

    async def scan(self):
        response = await asyncio.to_thread(shell.run_command, self.link, 'wifi scan', 25)
        match = re.search(rb'(?m)^wifi-scan=(\d+)\r?$', response)
        if not match:
            raise ServiceError('设备扫描 Wi-Fi 失败；请确认无线电已开启且未处于蓝牙配网或连接重试中，再重试。')
        count = int(match[1])
        if count > 1024:
            raise ServiceError('设备返回的 Wi-Fi 数量无效。')
        found = []
        for index in range(count):
            response = await asyncio.to_thread(shell.run_command, self.link, f'wifi ap {index}', 5)
            match = re.search(rb'(?m)^ap=([0-9a-f]*),([0-9a-f]{12}),(\d+),(-?\d+),(\d+)\r?$', response)
            if not match:
                raise ServiceError('Wi-Fi 扫描结果读取失败，请重新扫描。')
            raw, bssid, channel, rssi, auth = match.groups()
            ssid = bytes.fromhex(raw.decode()).decode('utf-8', 'replace')
            mode = int(auth)
            found.append({'ssid':ssid, 'bssid':bssid.decode(), 'channel':int(channel),
                          'rssi':int(rssi), 'auth':AUTH_MODES[mode] if mode < len(AUTH_MODES) else f'模式 {mode}'})
        return sorted(found, key=lambda ap: ap['rssi'], reverse=True)

    async def pair(self, credential):
        def commit():
            self.link.write(b'wifi pair\n')
            try:
                shell.read_until(self.link, b'Network key: ', 5, 'network credential prompt')
                self.link.write(credential.psk.hex().encode() + b'\n')
                response = shell.read_until_prompt(self.link, 10)
                match = re.search(rb'(?m)^fingerprint=([0-9a-f]{64})\r?$', response)
                if not match or not secrets.compare_digest(bytes.fromhex(match[1].decode()), credential.fingerprint):
                    raise ServiceError('设备未确认网络凭据；本机凭据已保留，可在此窗口重试。')
                return {'paired':True, 'device_id':self.serial}
            except BaseException:
                self.link.write(b'\x03')
                raise
        return await asyncio.to_thread(commit)

    async def wifi(self, ssid, password):
        return await asyncio.to_thread(usb_wifi, self.link, ssid, password)


class BleProvisioning:
    def __init__(self, serial, esp):
        self.serial, self.esp = serial, esp
        self.link = self.security = self.heartbeat = None
        self.managed = False
        self.error = None
        self.lock = asyncio.Lock()
        self.stopping = asyncio.Event()

    async def control(self, command):
        """Version 2 controls share the Security 2 protected pairing endpoint."""
        for attempt in range(50):
            payload = bytes((2, command if attempt == 0 else 0))
            encrypted = self.security.encrypt_data(payload).decode('latin-1')
            reply = await self.link.send_data(pair.PAIRING_ENDPOINT, encrypted)
            result = self.security.decrypt_data(pair._latin1_bytes(reply))
            if len(result) != 4 or result[0] != 2:
                raise ServiceError('固件不支持持续蓝牙配网，请升级固件后重试。')
            if command == 4:  # The accepted stop runs on the device event loop.
                if result[2] != command or (not result[1] and not result[3]):
                    raise ServiceError('设备未确认退出配网；连接释放后会由设备超时清理。')
                return
            if result[1] == 0:
                if result[2] != command or result[3] != 1:
                    raise ServiceError('设备未接受配网会话操作，请取消后重新开启设备配网窗口。')
                return
            await asyncio.sleep(.1)
        raise ServiceError('设备配网会话响应超时。')

    async def open(self):
        async with asyncio.timeout(70):
            self.link = await self.esp.get_transport('ble', self.serial)
            if self.link is None:
                raise ServiceError('未连接到设备，请先开启设备蓝牙配网模式。')
            patch = await self.esp.get_sec_patch_ver(self.link)
            self.security = self.esp.get_security(2, patch, pair.PUBLIC_SEC2_USERNAME, pair.PUBLIC_SEC2_POP)
            if not self.security or not await self.esp.establish_session(self.link, self.security):
                raise ServiceError('蓝牙 Security 2 握手失败。')
            try:
                await self.control(1)
            except ServiceError:
                raise
            except Exception as error:
                raise ServiceError(f'蓝牙配网会话通信失败（{type(error).__name__}），请重新开启设备配网模式后重试。') from None
            self.managed = True
        self.heartbeat = asyncio.create_task(self.keepalive())

    async def keepalive(self):
        while True:
            try:
                await asyncio.wait_for(self.stopping.wait(), 25)
                return
            except TimeoutError:
                pass
            try:
                async with self.lock, asyncio.timeout(10):
                    await self.control(2)
            except Exception:
                self.error = '蓝牙配网连接已中断，请取消配网后重新连接；已保存的配置不会撤销。'
                return

    def check(self):
        if self.error:
            raise ServiceError(self.error)

    async def close(self):
        if self.heartbeat:
            self.stopping.set()
            await asyncio.gather(self.heartbeat, return_exceptions=True)
            self.heartbeat = None
        if self.link:
            try:
                if self.managed:
                    async with asyncio.timeout(5):
                        await self.control(4)
            finally:
                await self.link.disconnect()
                self.link = None
                self.managed = False

    async def scan(self):
        async with self.lock, asyncio.timeout(40):
            self.check()
            await self.control(2)
            records = await self.esp.scan_wifi_APs('ble', self.link, self.security)
            if records is None:
                raise ServiceError('Wi-Fi 扫描失败，请重试。')
            # Espressif exposes raw SSID bytes as latin-1 strings.
            return sorted([ap | {'ssid':ap['ssid'].encode('latin-1').decode('utf-8', 'replace')}
                           for ap in records], key=lambda ap: ap['rssi'], reverse=True)

    async def pair(self, credential):
        async with self.lock, asyncio.timeout(20):
            self.check()
            await self.control(2)
            encrypted = self.security.encrypt_data(b'\x01' + credential.psk).decode('latin-1')
            reply = await self.link.send_data(pair.PAIRING_ENDPOINT, encrypted)
            fingerprint = self.security.decrypt_data(pair._latin1_bytes(reply))
            if not secrets.compare_digest(fingerprint, credential.fingerprint):
                raise ServiceError('设备未确认网络凭据；本机凭据已保留，可重试。')
            return {'paired':True, 'device_id':self.serial}

    async def wifi(self, ssid, password):
        wifi_fields(ssid, password, 'ble')
        async with self.lock, asyncio.timeout(65):
            self.check()
            await self.control(3)  # Reset only the provisioning RAM state before retry/reconfigure.
            if not await self.esp.send_wifi_config(self.link, self.security, ssid, password):
                raise ServiceError('设备未接受 Wi-Fi 配置，可在此窗口重试。')
            if not await self.esp.apply_wifi_config(self.link, self.security):
                raise ServiceError('Wi-Fi 配置可能已提交，但设备未确认应用。')
            for _ in range(40):
                status = await self.esp.get_wifi_config(self.link, self.security)
                if status == 'connected':
                    return {'wifi':'online'}
                if status not in ('connecting', 'disconnected'):
                    raise ServiceError('Wi-Fi 连接失败，请检查密码后重试。')
                await asyncio.sleep(1)
            raise ServiceError('设备未在时限内取得 IP，请检查 Wi-Fi 后重试。')


class Devices:
    def __init__(self, *, idf_path=None, provisioning_dir=None):
        self.idf_path = idf_path
        self.provisioning_dir = provisioning_dir
        self.esp = None

    def ble_client(self):
        if self.esp is None:
            if self.provisioning_dir:
                pair.ESP_PROV_DIR = Path(self.provisioning_dir)
                pair.ESP_PROV_SCRIPT = pair.ESP_PROV_DIR / 'esp_prov.py'
            try:
                idf = Path(self.idf_path) if self.idf_path else pair.resolve_idf_path()
                previous = os.environ.get('IDF_PATH')
                try:
                    # Upstream proto/__init__.py reads the environment directly,
                    # even when its parent loader received an explicit path.
                    os.environ['IDF_PATH'] = str(idf)
                    esp = pair.load_esp_prov(idf)
                finally:
                    if previous is None:
                        os.environ.pop('IDF_PATH', None)
                    else:
                        os.environ['IDF_PATH'] = previous
                from airdap_esp_prov import AirDapBleClient
                importlib.import_module('transport.ble_cli').get_client = AirDapBleClient
            except (OSError, pair.PairingError, ImportError):
                raise ServiceError('蓝牙配网组件未就绪：请配置 ESP-IDF 和 network_provisioning 客户端路径。') from None
            esp.config_throw_except = True
            self.esp = esp
        return self.esp

    async def discover(self, transport):
        if transport == 'usb':
            return await asyncio.to_thread(physical_devices)
        from bleak import BleakScanner
        found = await provision.discover_airdap_devices(BleakScanner, timeout=8)
        return [{'device_id': d.name, 'address': d.address, 'transport': 'ble'} for d in found]

    async def open_provisioning(self, serial, transport):
        device_id(serial)
        session = UsbProvisioning(serial) if transport == 'usb' else BleProvisioning(serial, self.ble_client())
        try:
            await session.open()
        except BaseException:
            await session.close()
            raise
        return session

    async def wifi(self, serial, transport, ssid, password, credential=None):
        device_id(serial)
        wifi_fields(ssid, password, transport)
        if transport == 'usb':
            def configure():
                with usb_transport(serial, debug=True) as link:
                    return usb_wifi(link, ssid, password)
            return await asyncio.to_thread(configure)
        esp = self.ble_client()
        async with asyncio.timeout(100):
            link = await esp.get_transport('ble', serial)
            if link is None:
                raise ServiceError('未连接到蓝牙配网窗口，请在设备上开启配网模式。')
            try:
                patch = await esp.get_sec_patch_ver(link)
                security = esp.get_security(2, patch, pair.PUBLIC_SEC2_USERNAME, pair.PUBLIC_SEC2_POP)
                if security is None or not await esp.establish_session(link, security):
                    raise ServiceError('蓝牙 Security 2 握手失败。')
                if credential is not None:
                    encrypted = security.encrypt_data(b'\x01' + credential.psk).decode('latin-1')
                    response = await link.send_data(pair.PAIRING_ENDPOINT, encrypted)
                    fingerprint = security.decrypt_data(pair._latin1_bytes(response))
                    if not secrets.compare_digest(fingerprint, credential.fingerprint):
                        raise ServiceError('设备未确认网络凭据；请检查设备后重新配对。')
                if not await esp.send_wifi_config(link, security, ssid, password):
                    raise ServiceError('设备未接受 Wi-Fi 配置。')
                if not await esp.apply_wifi_config(link, security):
                    raise ServiceError('Wi-Fi 配置可能已保存，但设备未确认应用；请检查连接状态。')
                for _ in range(35):
                    status = await esp.get_wifi_config(link, security)
                    if status == 'connected':
                        return {'wifi': 'online', 'paired': credential is not None}
                    if status not in ('connecting', 'disconnected'):
                        raise ServiceError('Wi-Fi 配置已提交，但设备报告连接失败。')
                    await asyncio.sleep(1)
                raise ServiceError('Wi-Fi 配置已提交，但未在时限内取得 IP。')
            finally:
                await link.disconnect()

    async def pair(self, credential):
        async with asyncio.timeout(70):
            await pair.pair_device(credential, self.ble_client())
        return {'paired': True, 'device_id': credential.device_id}

    async def info(self, serial, host, credential, transport):
        if transport == 'usb':
            def query():
                with usb_transport(serial, debug=True) as link:
                    # Fixed read-only allowlist; never expose a remote shell endpoint.
                    details = {
                        command: shell.run_command(link, command, 5).decode('utf-8', 'replace')
                        for command in ('system-info', 'network-status', 'ota-status', 'uart-status')}
                    fields = dict(re.findall(r'(?m)^(firmware_version|uuid|capabilities)=([^\r\n]+)', details['system-info']))
                    return {'device_id': serial, 'transport': 'usb',
                            'firmware': fields.get('firmware_version', '未知'),
                            'uuid': fields.get('uuid', '未知'),
                            'capabilities': fields.get('capabilities', '未知'), 'details': details}
            return await asyncio.to_thread(query)
        def query():
            # HELLO without AUTH does not compete with an imported USB/IP owner.
            with uart.Client(host, credential, port=3260, bind=False) as client:
                uuid, capabilities, firmware = uart.dap._parse_hello(
                    client.request(1, b'', 1), serial)
                return {'device_id': serial, 'transport': 'network', 'host': host,
                        'firmware': firmware, 'uuid': uuid.hex(), 'capabilities': capabilities,
                        'tls': client.connection.version()}
        return await asyncio.to_thread(query)

    async def ota(self, serial, host, credential, transport, image, progress):
        version = net_update.image_version(io.BytesIO(image))
        def network_upload():
            # Existing upload validates capacity, every offset and device commit.
            class ProgressImage(io.BytesIO):
                def read(self, size=-1):
                    data = super().read(size)
                    progress(min(90, self.tell() * 90 // len(image)), '写入非活动分区')
                    return data
            with net_update.Client(host, credential) as client:
                before = net_update.upload(client, ProgressImage(image), len(image))
                progress(92, '镜像已提交，等待重启与启动确认')
                client.reboot()
            after = net_update.wait_updated(host, credential, before, version)
            return {'version': after.version, 'confirmed': after.confirmed,
                    'running_address': after.running_address}
        def usb_upload():
            with usb_transport(serial) as link:
                usb_update.upload_image(link, io.BytesIO(image), len(image),
                    progress=lambda n, total: progress(n * 90 // total, '写入非活动分区'))
            progress(92, '镜像已提交，等待 USB 重新枚举')
            import usb.core
            import usb.util
            find = lambda: usb_update._find_airdap_devices(usb.core)
            getter = lambda d: usb.util.get_string(d, d.iSerialNumber)
            usb_update.wait_for_disconnect(find, serial, getter, 15)
            deadline = time.monotonic() + 30
            while time.monotonic() < deadline:
                try:
                    with usb_transport(serial) as link:
                        after = link.query()
                except (usb_update.UpdateError, usb.core.USBError):
                    time.sleep(.5)
                    continue
                if after.running_version != version:
                    raise ServiceError('USB 重连后的固件版本与镜像不一致，可能发生回滚。')
                return {'version': version, 'verification': 'USB 重连及版本已核对；USB QUERY 不提供槽位启动确认字段。'}
            raise ServiceError('镜像已提交，但 USB 重连验证超时；请检查设备，不要自动重复写入。')
        return await asyncio.to_thread(usb_upload if transport == 'usb' else network_upload)
