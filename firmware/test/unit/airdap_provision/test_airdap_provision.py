from __future__ import annotations

import importlib.util
import io
import sys
import tempfile
import types
import unittest
from pathlib import Path
from unittest import mock


SCRIPT = Path(__file__).resolve().parents[3] / "tools" / "airdap-provision.py"
CREDENTIAL_HEADER = (
    SCRIPT.parents[1]
    / "components"
    / "ble_provisioning"
    / "private_include"
    / "airdap_sec2_credentials.h"
)
SPEC = importlib.util.spec_from_file_location("airdap_provision", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
airdap_provision = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = airdap_provision
SPEC.loader.exec_module(airdap_provision)
BLE_SPEC = importlib.util.spec_from_file_location(
    "airdap_esp_prov", SCRIPT.with_name("airdap_esp_prov.py")
)
assert BLE_SPEC is not None and BLE_SPEC.loader is not None
airdap_esp_prov = importlib.util.module_from_spec(BLE_SPEC)
BLE_SPEC.loader.exec_module(airdap_esp_prov)


class FakeScanner:
    discovery: dict[str, tuple[object, object]] = {}
    calls: list[dict[str, object]] = []

    @classmethod
    async def discover(cls, **kwargs: object) -> dict[str, tuple[object, object]]:
        cls.calls.append(kwargs)
        return cls.discovery


def discovered_device(
    name: str | None,
    address: str,
    service_uuids: list[str],
    *,
    local_name: str | None = None,
) -> tuple[object, object]:
    device = types.SimpleNamespace(name=name, address=address)
    advertisement = types.SimpleNamespace(
        local_name=local_name,
        service_uuids=service_uuids,
    )
    return device, advertisement


class AirDapProvisionTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self) -> None:
        FakeScanner.discovery = {}
        FakeScanner.calls = []

    async def test_discovery_keeps_only_airdap_provisioning_services(self) -> None:
        service_uuid = airdap_provision.AIRDAP_PROVISIONING_SERVICE_UUID
        FakeScanner.discovery = {
            "ignored-name": discovered_device(
                "Headphones",
                "00:00:00:00:00:01",
                [service_uuid],
            ),
            "ignored-service": discovered_device(
                "ADP-001122334455",
                "00:00:00:00:00:02",
                ["0000180f-0000-1000-8000-00805f9b34fb"],
            ),
            "second": discovered_device(
                None,
                "00:00:00:00:00:04",
                [service_uuid.upper()],
                local_name="ADP-AABBCCDDEEFF",
            ),
            "first": discovered_device(
                "ADP-102030405060",
                "00:00:00:00:00:03",
                [service_uuid],
            ),
        }

        devices = await airdap_provision.discover_airdap_devices(
            FakeScanner,
            timeout=4.5,
        )

        self.assertEqual(
            devices,
            [
                airdap_provision.DiscoveredAirDap(
                    name="ADP-102030405060",
                    address="00:00:00:00:00:03",
                ),
                airdap_provision.DiscoveredAirDap(
                    name="ADP-AABBCCDDEEFF",
                    address="00:00:00:00:00:04",
                ),
            ],
        )
        self.assertEqual(FakeScanner.calls, [{"timeout": 4.5, "return_adv": True}])

    def test_single_device_is_selected_without_prompt(self) -> None:
        candidate = airdap_provision.DiscoveredAirDap(
            name="ADP-001122334455",
            address="00:00:00:00:00:01",
        )
        input_fn = mock.Mock(side_effect=AssertionError("unexpected prompt"))

        selected = airdap_provision.select_airdap_device(
            [candidate],
            input_fn=input_fn,
            output_fn=mock.Mock(),
        )

        self.assertEqual(selected, candidate)
        input_fn.assert_not_called()

    def test_multiple_devices_require_an_explicit_valid_selection(self) -> None:
        candidates = [
            airdap_provision.DiscoveredAirDap(
                name="ADP-001122334455",
                address="00:00:00:00:00:01",
            ),
            airdap_provision.DiscoveredAirDap(
                name="ADP-AABBCCDDEEFF",
                address="00:00:00:00:00:02",
            ),
        ]
        output = mock.Mock()
        input_fn = mock.Mock(side_effect=["bad", "3", "2"])

        selected = airdap_provision.select_airdap_device(
            candidates,
            input_fn=input_fn,
            output_fn=output,
        )

        self.assertEqual(selected, candidates[1])
        self.assertEqual(input_fn.call_count, 3)
        self.assertGreaterEqual(
            sum(call.args == ("Invalid selection; try again.",) for call in output.call_args_list),
            2,
        )

    def test_no_device_reports_how_to_open_the_ble_window(self) -> None:
        with self.assertRaisesRegex(
            airdap_provision.ProvisioningError,
            "BOOT_KEY.*three seconds",
        ):
            airdap_provision.select_airdap_device([])

    def test_configured_idf_path_is_used_without_shell_activation(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            idf_path = root / "esp-idf"
            (idf_path / "tools").mkdir(parents=True)
            (idf_path / "tools" / "idf.py").touch()
            proto = idf_path / "components" / "protocomm" / "python"
            proto.mkdir(parents=True)
            (proto / "session_pb2.py").touch()
            config_file = root / "idf-path.txt"
            config_file.write_text(f"{idf_path}\nmanaged\n", encoding="utf-8")

            resolved = airdap_provision.resolve_idf_path(
                {},
                config_file=config_file,
            )

        self.assertEqual(resolved, idf_path.resolve())

    def test_idf_path_environment_takes_precedence_over_saved_path(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            configured = root / "configured"
            environment = root / "environment"
            for idf_path in (configured, environment):
                (idf_path / "tools").mkdir(parents=True)
                (idf_path / "tools" / "idf.py").touch()
                proto = idf_path / "components" / "protocomm" / "python"
                proto.mkdir(parents=True)
                (proto / "session_pb2.py").touch()
            config_file = root / "idf-path.txt"
            config_file.write_text(f"{configured}\nexternal\n", encoding="utf-8")

            resolved = airdap_provision.resolve_idf_path(
                {"IDF_PATH": str(environment)},
                config_file=config_file,
            )

        self.assertEqual(resolved, environment.resolve())

    def test_missing_idf_configuration_reports_setup_command(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            config_file = Path(temporary) / "missing.txt"
            with self.assertRaisesRegex(
                airdap_provision.ProvisioningError,
                r"firmware/tools/setup\.py",
            ):
                airdap_provision.resolve_idf_path({}, config_file=config_file)

    def test_command_uses_public_security2_credential_without_wifi_password(self) -> None:
        command = airdap_provision.build_esp_prov_command(
            "ADP-001122334455",
            python_executable="python-test",
            esp_prov_script=Path("esp_prov.py"),
        )

        self.assertEqual(
            command,
            [
                "python-test",
                str(airdap_provision.ESP_PROV_LAUNCHER),
                "esp_prov.py",
                "--transport",
                "ble",
                "--service_name",
                "ADP-001122334455",
                "--sec_ver",
                "2",
                "--sec2_username",
                "wifiprov",
                "--sec2_pwd",
                "abcd1234",
            ],
        )
        self.assertNotIn("--passphrase", command)

    def test_public_security2_credential_matches_firmware(self) -> None:
        header = CREDENTIAL_HEADER.read_text(encoding="utf-8")

        self.assertIn(
            f'#define AIRDAP_SEC2_USERNAME "{airdap_provision.PUBLIC_SEC2_USERNAME}"',
            header,
        )
        self.assertIn(
            f'#define AIRDAP_SEC2_POP "{airdap_provision.PUBLIC_SEC2_POP}"',
            header,
        )

    def test_provisioning_runner_inherits_terminal_and_sets_idf_path(self) -> None:
        runner = mock.Mock(return_value=types.SimpleNamespace(returncode=7))
        idf_path = Path("/tmp/idf").resolve()
        with tempfile.TemporaryDirectory() as temporary:
            script = Path(temporary) / "esp_prov.py"
            script.touch()

            result = airdap_provision.run_provisioning_client(
                "ADP-001122334455",
                idf_path,
                esp_prov_script=script,
                runner=runner,
            )

        self.assertEqual(result, 7)
        args, kwargs = runner.call_args
        self.assertEqual(args[0][1], str(airdap_provision.ESP_PROV_LAUNCHER))
        self.assertEqual(args[0][2], str(script))
        self.assertEqual(kwargs["env"]["IDF_PATH"], str(idf_path))
        self.assertFalse(kwargs["check"])
        self.assertNotIn("shell", kwargs)
        self.assertNotIn("stdin", kwargs)
        self.assertNotIn("stdout", kwargs)

    def test_cli_reports_discovery_failure_without_traceback(self) -> None:
        stderr = io.StringIO()
        with mock.patch.object(
            airdap_provision,
            "main",
            side_effect=airdap_provision.ProvisioningError("Bluetooth unavailable"),
        ), mock.patch.object(airdap_provision.sys, "stderr", stderr):
            result = airdap_provision._run_cli([])

        self.assertEqual(result, 1)
        self.assertEqual(stderr.getvalue(), "airdap-provision: Bluetooth unavailable\n")
        self.assertNotIn("Traceback", stderr.getvalue())


class AirDapBleTransportTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        self.device, self.advertisement = discovered_device(
            "ADP-001122334455", "00:11:22:33:44:55", ["provision-service"]
        )
        self.characteristic = types.SimpleNamespace(
            uuid="session-characteristic",
            descriptors=[types.SimpleNamespace(
                uuid="00002901-0000-1000-8000-00805f9b34fb", handle=12
            )],
        )
        self.bleak_client = mock.Mock(
            connect=mock.AsyncMock(),
            disconnect=mock.AsyncMock(),
            pair=mock.AsyncMock(side_effect=AssertionError("OS pairing requested")),
            unpair=mock.AsyncMock(side_effect=AssertionError("OS bond deleted")),
            read_gatt_descriptor=mock.AsyncMock(return_value=b"prov-session"),
            write_gatt_char=mock.AsyncMock(),
            read_gatt_char=mock.AsyncMock(return_value=b"\x00\xff\x80"),
            services=mock.Mock(get_service=mock.Mock(return_value=types.SimpleNamespace(
                characteristics=[self.characteristic]
            ))),
        )
        self.backend = types.SimpleNamespace(
            BleakScanner=types.SimpleNamespace(discover=mock.AsyncMock(
                return_value={"device": (self.device, self.advertisement)}
            )),
            BleakClient=mock.Mock(return_value=self.bleak_client),
        )
        self.client = airdap_esp_prov.AirDapBleClient(self.backend)

    async def connect(self):
        return await self.client.connect(
            self.device.name, "hci0", ["prov-session"], "fallback-service"
        )

    async def test_unbonded_session_preserves_binary_protocol_and_disconnects(self):
        self.assertTrue(await self.connect())
        self.backend.BleakClient.assert_called_once_with(
            self.device, adapter="hci0", pair=False
        )
        endpoint = self.client.get_nu_lookup()["prov-session"]
        response = await self.client.send_data(endpoint, "\x00\x80\xff")
        self.assertEqual(response, "\x00\xff\x80")
        self.bleak_client.write_gatt_char.assert_awaited_once_with(
            endpoint, bytearray(b"\x00\x80\xff"), response=True
        )
        await self.client.disconnect()
        self.bleak_client.disconnect.assert_awaited_once()
        self.bleak_client.pair.assert_not_awaited()
        self.bleak_client.unpair.assert_not_awaited()
        self.assertIsNone(self.client.device)
        self.assertIsNone(self.client.get_nu_lookup())

    async def test_descriptor_failure_releases_connection_without_unpairing(self):
        self.bleak_client.read_gatt_descriptor.side_effect = OSError("read failed")
        with self.assertRaisesRegex(OSError, "read failed"):
            await self.connect()
        self.bleak_client.disconnect.assert_awaited_once()
        self.bleak_client.unpair.assert_not_awaited()
        self.assertIsNone(self.client.device)

    async def test_missing_service_releases_connection_without_unpairing(self):
        self.bleak_client.services.get_service.return_value = None
        with self.assertRaisesRegex(RuntimeError, "Provisioning service not found"):
            await self.connect()
        self.bleak_client.disconnect.assert_awaited_once()
        self.bleak_client.unpair.assert_not_awaited()


if __name__ == "__main__":
    unittest.main()
