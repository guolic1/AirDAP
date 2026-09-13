from contextlib import contextmanager
import os
from pathlib import Path
import struct
import sys
import unittest
from unittest.mock import AsyncMock, MagicMock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from airdap_service_devices import Devices, ServiceError, usb_wifi, wifi_fields, usb_update, net_update
from airdap_service import Service, Store
import tempfile

DEVICE = 'ADP-001122334455'


class UsbWifiTests(unittest.TestCase):
    def test_prompt_input_and_dhcp_confirmation(self):
        link = MagicMock()
        with patch('airdap_service_devices.shell.read_until', return_value=b''), \
             patch('airdap_service_devices.shell.read_until_prompt', return_value=b'wifi: credentials saved; reconnect requested\n'), \
             patch('airdap_service_devices.shell.run_command', side_effect=[b'wifi=connecting\n', b'wifi=online\n']), \
             patch('airdap_service_devices.time.sleep'):
            self.assertEqual(usb_wifi(link, 'ExampleSSID', 'example-only'), {'wifi':'online'})
        self.assertEqual([call.args[0] for call in link.write.call_args_list],
                         [b'wifi set\n', b'ExampleSSID\n', b'example-only\n'])

    def test_saved_is_not_connected(self):
        with patch('airdap_service_devices.shell.read_until', return_value=b''), \
             patch('airdap_service_devices.shell.read_until_prompt', return_value=b'wifi: credentials saved; reconnect requested\n'):
            with self.assertRaisesRegex(ServiceError, '配置已保存'):
                usb_wifi(MagicMock(), 'ExampleSSID', '', timeout=0)

    def test_controls_and_usb_non_ascii_are_rejected_before_writing(self):
        for ssid, password in [('ok', 'bad\nrestart'), ('bad\x00', ''), ('中文', '')]:
            link = MagicMock()
            with self.assertRaises(ServiceError):
                usb_wifi(link, ssid, password)
            link.write.assert_not_called()
        wifi_fields('中文', '', 'ble')


class BleTests(unittest.IsolatedAsyncioTestCase):
    async def test_explicit_idf_path_is_visible_to_upstream_proto_loader(self):
        from airdap_service_devices import pair
        observed = []
        def load(path):
            observed.append(os.environ.get('IDF_PATH'))
            raise ImportError('stop before hardware dependencies')
        with patch.dict(os.environ, {}, clear=True), patch.object(pair, 'load_esp_prov', side_effect=load):
            with self.assertRaises(ServiceError):
                Devices(idf_path=Path('/configured/idf')).ble_client()
            self.assertEqual(observed, [str(Path('/configured/idf'))])
            self.assertNotIn('IDF_PATH', os.environ)

    async def test_wifi_session_disconnects_on_success_and_apply_failure(self):
        for accepted in (True, False):
            esp = MagicMock()
            link = MagicMock(disconnect=AsyncMock())
            esp.get_transport = AsyncMock(return_value=link)
            esp.get_sec_patch_ver = AsyncMock(return_value=1)
            esp.establish_session = AsyncMock(return_value=True)
            esp.send_wifi_config = AsyncMock(return_value=True)
            esp.apply_wifi_config = AsyncMock(return_value=accepted)
            esp.get_wifi_config = AsyncMock(return_value='connected')
            devices = Devices()
            devices.esp = esp
            if accepted:
                result = await devices.wifi(DEVICE, 'ble', '中文', 'example-only')
                self.assertEqual(result['wifi'], 'online')
            else:
                with self.assertRaisesRegex(ServiceError, '可能已保存'):
                    await devices.wifi(DEVICE, 'ble', '中文', 'example-only')
                esp.get_wifi_config.assert_not_awaited()
            link.disconnect.assert_awaited_once()

    async def test_wifi_is_not_success_before_dhcp(self):
        devices = Devices()
        esp = MagicMock()
        devices.esp = esp
        link = MagicMock(disconnect=AsyncMock())
        for name, value in [('get_transport',link),('get_sec_patch_ver',1),
                            ('establish_session',True),('send_wifi_config',True),
                            ('apply_wifi_config',True),('get_wifi_config','failed')]:
            setattr(esp, name, AsyncMock(return_value=value))
        with self.assertRaises(ServiceError):
            await devices.wifi(DEVICE, 'ble', 'ExampleSSID', '')
        link.disconnect.assert_awaited_once()


def image_bytes():
    image = bytearray(128)
    image[0] = 0xe9
    struct.pack_into('<H', image, 12, 9)
    struct.pack_into('<I', image, 32, 0xabcd5432)
    image[48:53] = b'vtest'
    return bytes(image)


class OtaTests(unittest.IsolatedAsyncioTestCase):
    async def test_uploaded_image_never_writes_until_matching_confirmation(self):
        with tempfile.TemporaryDirectory() as folder:
            devices = Devices()
            devices.ota = AsyncMock(return_value={'version':'vtest'})
            service = Service(Store(Path(folder)), devices=devices)
            await service.command('profile', {'device_id': DEVICE, 'host':'localhost'})
            meta = await service.stage_image(image_bytes())
            self.assertEqual(meta['version'], 'vtest')
            devices.ota.assert_not_awaited()
            for wrong in ({'confirm':False, 'sha256':meta['sha256']},
                          {'confirm':True, 'sha256':'stale'}):
                with self.assertRaises(ServiceError):
                    await service.command('ota', {'device_id':DEVICE, 'transport':'usb'} | wrong)
            devices.ota.assert_not_awaited()
            await service.command('ota', {'device_id':DEVICE, 'transport':'usb', 'confirm':True, 'sha256':meta['sha256']})
            await service.job_task
            devices.ota.assert_awaited_once()
            self.assertEqual(service.job['state'], 'succeeded')
            self.assertIsNone(service.image)

    async def test_network_update_uses_commit_reboot_and_startup_verification(self):
        events = []
        client = MagicMock()
        client.__enter__.return_value = client
        before = MagicMock()
        after = net_update.OtaInfo(1000, 2000, 1000, True, 'vtest')
        client.reboot.side_effect = lambda: events.append('reboot')
        with patch.object(net_update, 'Client', return_value=client), \
             patch.object(net_update, 'upload', side_effect=lambda *a: events.append('upload') or before), \
             patch.object(net_update, 'wait_updated', side_effect=lambda *a: events.append('verify') or after) as wait:
            result = await Devices().ota(DEVICE, 'localhost', object(), 'network', image_bytes(), lambda *_: None)
            self.assertEqual(events, ['upload','reboot','verify'])
            self.assertTrue(result['confirmed'])
            self.assertEqual(wait.call_args.args[2:4], (before, 'vtest'))

    async def test_usb_reconnect_to_old_version_is_a_failure(self):
        link = MagicMock()
        link.query.return_value = usb_update.OtaInfo(1, 1, 1000, 'old-version')
        @contextmanager
        def transport(*_args, **_kwargs):
            yield link
        # USB modules are only imported after writing; provide their read-only API shape
        # so this test also runs on Linux hosts without a PyUSB installation.
        import types
        usb = types.ModuleType('usb')
        usb.core = types.ModuleType('usb.core')
        usb.core.USBError = OSError
        usb.util = types.ModuleType('usb.util')
        with patch('airdap_service_devices.usb_transport', transport), \
             patch.object(usb_update, 'upload_image') as upload, \
             patch.object(usb_update, 'wait_for_disconnect'), \
             patch.dict(sys.modules, {'usb':usb, 'usb.core':usb.core, 'usb.util':usb.util}):
            with self.assertRaisesRegex(ServiceError, '版本与镜像不一致'):
                await Devices().ota(DEVICE, 'localhost', None, 'usb', image_bytes(), lambda *_: None)
            upload.assert_called_once()

    async def test_network_verification_failure_does_not_retry_upload(self):
        client = MagicMock()
        client.__enter__.return_value = client
        with patch.object(net_update, 'Client', return_value=client), \
             patch.object(net_update, 'upload') as upload, \
             patch.object(net_update, 'wait_updated', side_effect=net_update.UpdateError('not confirmed')):
            with self.assertRaises(net_update.UpdateError):
                await Devices().ota(DEVICE, 'localhost', object(), 'network', image_bytes(), lambda *_: None)
            upload.assert_called_once()
            client.reboot.assert_called_once()


if __name__ == '__main__':
    unittest.main()
