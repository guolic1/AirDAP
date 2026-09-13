"""Device operations for the local service; reuse the checked host protocols."""
from __future__ import annotations

import asyncio
from contextlib import contextmanager
import importlib
import importlib.util
import io
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
                self.esp = pair.load_esp_prov(Path(self.idf_path) if self.idf_path else pair.resolve_idf_path())
            except (OSError, pair.PairingError, ImportError):
                raise ServiceError('蓝牙配网组件未就绪：请配置 ESP-IDF 和 network_provisioning 客户端路径。') from None
            from airdap_esp_prov import AirDapBleClient
            importlib.import_module('transport.ble_cli').get_client = AirDapBleClient
            self.esp.config_throw_except = True
        return self.esp

    async def discover(self, transport):
        if transport == 'usb':
            return await asyncio.to_thread(physical_devices)
        from bleak import BleakScanner
        found = await provision.discover_airdap_devices(BleakScanner, timeout=8)
        return [{'device_id': d.name, 'address': d.address, 'transport': 'ble'} for d in found]

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
