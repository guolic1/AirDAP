from __future__ import annotations

import asyncio
import importlib.util
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


TOOLS_DIR = Path(__file__).resolve().parents[3] / "tools"
sys.path.insert(0, str(TOOLS_DIR))
SCRIPT = TOOLS_DIR / "airdap-pair.py"
SPEC = importlib.util.spec_from_file_location("airdap_pair", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
airdap_pair = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = airdap_pair
SPEC.loader.exec_module(airdap_pair)


class FakeSecurity:
    def __init__(self) -> None:
        self.request = b""

    def encrypt_data(self, data: bytes) -> bytes:
        self.request = data
        return b"E" + data

    def decrypt_data(self, data: bytes) -> bytes:
        if not data.startswith(b"R"):
            raise RuntimeError("bad fake response")
        return data[1:]


class FakeTransport:
    def __init__(self, fingerprint: bytes) -> None:
        self.fingerprint = fingerprint
        self.endpoint = ""
        self.encrypted_request = b""
        self.disconnected = False
        self.disconnect_error: Exception | None = None

    async def send_data(self, endpoint: str, data: str) -> str:
        self.endpoint = endpoint
        self.encrypted_request = data.encode("latin-1")
        return (b"R" + self.fingerprint).decode("latin-1")

    async def disconnect(self) -> None:
        self.disconnected = True
        if self.disconnect_error is not None:
            raise self.disconnect_error


class FakeEspProv:
    def __init__(self, fingerprint: bytes) -> None:
        self.transport = FakeTransport(fingerprint)
        self.security = FakeSecurity()
        self.transport_args: tuple[object, ...] | None = None

    async def get_transport(self, *args: object) -> FakeTransport:
        self.transport_args = args
        return self.transport

    async def get_sec_patch_ver(self, transport: object) -> int:
        assert transport is self.transport
        return 1

    def get_security(self, *args: object) -> FakeSecurity:
        self.security_args = args
        return self.security

    async def establish_session(self, transport: object, security: object) -> bool:
        assert transport is self.transport and security is self.security
        return True


class AirDapPairTests(unittest.TestCase):
    def test_credential_file_is_private_repeatable_and_never_replaced(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "device.json"
            key = bytes(range(32))
            with mock.patch.object(airdap_pair.secrets, "token_bytes", return_value=key):
                created = airdap_pair.load_or_create_credential(
                    path, "ADP-001122334455"
                )

            self.assertEqual(created.psk, key)
            self.assertEqual(created.identity, "AIRDAP:ADP-001122334455")
            self.assertEqual(path.stat().st_mode & 0o777, 0o600)
            self.assertEqual(
                created.fingerprint.hex(),
                "ee83da8d15269c54e2f7c547b55558e1"
                "2d35c778576e0b26f21b0b3c80a65134",
            )

            with mock.patch.object(
                airdap_pair.secrets,
                "token_bytes",
                side_effect=AssertionError("retry must not rotate locally"),
            ):
                loaded = airdap_pair.load_or_create_credential(
                    path, "ADP-001122334455"
                )
            self.assertEqual(loaded, created)

            with self.assertRaisesRegex(airdap_pair.PairingError, "belongs to"):
                airdap_pair.load_or_create_credential(path, "ADP-AABBCCDDEEFF")

    def test_insecure_existing_credential_file_is_rejected(self) -> None:
        if os.name == "nt":
            self.skipTest("POSIX mode bits are not a Windows ACL")
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "device.json"
            credential = airdap_pair.NetworkCredential.create(
                "ADP-001122334455", bytes(range(32))
            )
            path.write_text(credential.to_json(), encoding="utf-8")
            path.chmod(0o644)
            with self.assertRaisesRegex(airdap_pair.PairingError, "permissions"):
                airdap_pair.load_or_create_credential(path, "ADP-001122334455")

    def test_non_utf8_credential_file_is_reported_without_raw_decode_error(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "device.json"
            path.write_bytes(b"\xff\xfe")
            path.chmod(0o600)
            with self.assertRaisesRegex(airdap_pair.PairingError, "cannot read"):
                airdap_pair.load_or_create_credential(
                    path, "ADP-001122334455"
                )

    def test_failed_credential_write_removes_only_its_partial_file(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "device.json"
            credential = airdap_pair.NetworkCredential.create(
                "ADP-001122334455", bytes(range(32))
            )
            with mock.patch.object(
                airdap_pair.os,
                "write",
                side_effect=OSError("injected write failure"),
            ):
                with self.assertRaisesRegex(airdap_pair.PairingError, "cannot write"):
                    with mock.patch.object(
                        airdap_pair.secrets,
                        "token_bytes",
                        return_value=credential.psk,
                    ):
                        airdap_pair.load_or_create_credential(
                            path, credential.device_id
                        )
            self.assertFalse(path.exists())

    def test_security2_endpoint_receives_binary_psk_and_returns_fingerprint(self) -> None:
        credential = airdap_pair.NetworkCredential.create(
            "ADP-001122334455", bytes(range(32))
        )
        upstream = FakeEspProv(credential.fingerprint)

        fingerprint = asyncio.run(
            airdap_pair.pair_device(
                credential,
                upstream,
                ble_adapter="hci-test",
            )
        )

        self.assertEqual(fingerprint, credential.fingerprint)
        self.assertEqual(
            upstream.transport_args,
            ("ble", "ADP-001122334455", "hci-test"),
        )
        self.assertEqual(upstream.security_args, (2, 1, "wifiprov", "abcd1234"))
        self.assertEqual(upstream.transport.endpoint, "airdap-pair")
        self.assertEqual(
            upstream.security.request,
            bytes([1]) + credential.psk,
        )
        self.assertTrue(upstream.transport.disconnected)

    def test_mismatched_response_is_rejected_and_transport_is_closed(self) -> None:
        credential = airdap_pair.NetworkCredential.create(
            "ADP-001122334455", bytes(range(32))
        )
        upstream = FakeEspProv(bytes([0xFF]) * 32)

        with self.assertRaisesRegex(airdap_pair.PairingError, "fingerprint"):
            asyncio.run(airdap_pair.pair_device(credential, upstream))
        self.assertTrue(upstream.transport.disconnected)

    def test_disconnect_failure_is_wrapped_as_pairing_error(self) -> None:
        credential = airdap_pair.NetworkCredential.create(
            "ADP-001122334455", bytes(range(32))
        )
        upstream = FakeEspProv(credential.fingerprint)
        upstream.transport.disconnect_error = RuntimeError("injected disconnect failure")

        with self.assertRaisesRegex(airdap_pair.PairingError, "disconnect"):
            asyncio.run(airdap_pair.pair_device(credential, upstream))


if __name__ == "__main__":
    unittest.main()
