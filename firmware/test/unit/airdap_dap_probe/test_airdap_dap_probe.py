from __future__ import annotations

import importlib.util
import struct
import sys
import unittest
from pathlib import Path
from unittest import mock


TOOLS_DIR = Path(__file__).resolve().parents[3] / "tools"
sys.path.insert(0, str(TOOLS_DIR))
SCRIPT = TOOLS_DIR / "airdap-dap-probe.py"
SPEC = importlib.util.spec_from_file_location("airdap_dap_probe", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
airdap_dap_probe = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = airdap_dap_probe
SPEC.loader.exec_module(airdap_dap_probe)


HEADER = struct.Struct(">4sBBHIIHH")
SESSION_ID = 9
UUID = bytes(range(16))
CAPABILITIES = 0x01020304
DEVICE_ID = "ADP-001122334455"
FIRMWARE_VERSION = "test-fw"


def response(message_type: int, sequence: int, payload: bytes = b"") -> bytes:
    return HEADER.pack(
        b"ADAP", 1, message_type, 0, SESSION_ID, sequence, len(payload), 0
    ) + payload


def hello_payload(*, device_id: str = DEVICE_ID) -> bytes:
    return (
        UUID
        + CAPABILITIES.to_bytes(4, "big")
        + device_id.encode("ascii")
        + FIRMWARE_VERSION.encode("utf-8")
    )


class FakeRawSocket:
    def __enter__(self) -> "FakeRawSocket":
        return self

    def __exit__(self, *args: object) -> None:
        return None


class FakeTlsSocket:
    incoming = b""
    instance: "FakeTlsSocket"

    def __init__(self) -> None:
        self.received = bytearray(self.incoming)
        self.sent = bytearray()
        FakeTlsSocket.instance = self

    def __enter__(self) -> "FakeTlsSocket":
        return self

    def __exit__(self, *args: object) -> None:
        return None

    def version(self) -> str:
        return "TLSv1.3"

    def cipher(self) -> tuple[str, str, int]:
        return ("TLS_AES_128_GCM_SHA256", "TLSv1.3", 128)

    def sendall(self, data: bytes) -> None:
        self.sent.extend(data)

    def recv(self, size: int) -> bytes:
        if not self.received:
            return b""
        copied = min(size, 3, len(self.received))
        data = bytes(self.received[:copied])
        del self.received[:copied]
        return data


class FakeContext:
    instance: "FakeContext"

    def __init__(self, protocol: object) -> None:
        self.protocol = protocol
        self.minimum_version = None
        self.maximum_version = None
        self.check_hostname = True
        self.verify_mode = None
        self.callback = None
        FakeContext.instance = self

    def set_psk_client_callback(self, callback: object) -> None:
        self.callback = callback

    def wrap_socket(self, raw: object, *, server_hostname: object) -> FakeTlsSocket:
        assert raw is not None and server_hostname is None
        return FakeTlsSocket()


def decode_sent_frames(data: bytes) -> list[tuple[int, int, bytes]]:
    frames: list[tuple[int, int, bytes]] = []
    offset = 0
    while offset < len(data):
        fields = HEADER.unpack_from(data, offset)
        magic, version, message_type, flags, session, sequence, length, reserved = fields
        assert magic == b"ADAP" and version == 1
        assert flags == 0 and reserved == 0 and session == SESSION_ID
        offset += HEADER.size
        payload = data[offset : offset + length]
        assert len(payload) == length
        offset += length
        frames.append((message_type, sequence, payload))
    return frames


class AirDapDapProbeTests(unittest.TestCase):
    def setUp(self) -> None:
        self.credential = airdap_dap_probe.NetworkCredential.create(
            DEVICE_ID, bytes(range(32))
        )
        auth_payload = (77).to_bytes(4, "big") + bytes(range(32))
        dap_info_payload = b"\x00\x08test-fw\x00"
        FakeTlsSocket.incoming = b"".join(
            (
                response(1, 1, hello_payload()),
                response(2, 2, auth_payload),
                response(4, 3, dap_info_payload),
                response(7, 4),
            )
        )

    def test_probe_authenticates_and_runs_dap_info(self) -> None:
        raw = FakeRawSocket()
        with mock.patch.object(
            airdap_dap_probe.ssl, "SSLContext", FakeContext
        ), mock.patch.object(
            airdap_dap_probe.socket, "create_connection", return_value=raw
        ) as connect, mock.patch.object(
            airdap_dap_probe.secrets, "randbelow", return_value=SESSION_ID - 1
        ):
            result = airdap_dap_probe.probe_dap(
                "192.0.2.10", 3260, self.credential, timeout=2.5
            )

        self.assertEqual(result.device_id, DEVICE_ID)
        self.assertEqual(result.uuid, UUID)
        self.assertEqual(result.capabilities, CAPABILITIES)
        self.assertEqual(result.firmware_version, FIRMWARE_VERSION)
        self.assertEqual(result.owner_session_id, 77)
        self.assertEqual(result.session_token, bytes(range(32)))
        self.assertEqual(result.tls_version, "TLSv1.3")
        self.assertEqual(result.cipher, "TLS_AES_128_GCM_SHA256")
        connect.assert_called_once_with(("192.0.2.10", 3260), timeout=2.5)
        self.assertEqual(
            decode_sent_frames(bytes(FakeTlsSocket.instance.sent)),
            [
                (1, 1, b""),
                (2, 2, b""),
                (3, 3, b"\x00\x09"),
                (7, 4, b""),
            ],
        )
        context = FakeContext.instance
        self.assertEqual(
            context.callback(None),
            (self.credential.identity, self.credential.psk),
        )

    def test_probe_rejects_hello_for_another_device(self) -> None:
        FakeTlsSocket.incoming = response(
            1, 1, hello_payload(device_id="ADP-AABBCCDDEEFF")
        )
        with mock.patch.object(
            airdap_dap_probe.ssl, "SSLContext", FakeContext
        ), mock.patch.object(
            airdap_dap_probe.socket,
            "create_connection",
            return_value=FakeRawSocket(),
        ), mock.patch.object(
            airdap_dap_probe.secrets, "randbelow", return_value=SESSION_ID - 1
        ):
            with self.assertRaisesRegex(airdap_dap_probe.ProbeError, "device"):
                airdap_dap_probe.probe_dap(
                    "192.0.2.10", 3260, self.credential
                )

    def test_probe_reports_wire_error(self) -> None:
        FakeTlsSocket.incoming = response(8, 1, b"\x00\x21")
        with mock.patch.object(
            airdap_dap_probe.ssl, "SSLContext", FakeContext
        ), mock.patch.object(
            airdap_dap_probe.socket,
            "create_connection",
            return_value=FakeRawSocket(),
        ), mock.patch.object(
            airdap_dap_probe.secrets, "randbelow", return_value=SESSION_ID - 1
        ):
            with self.assertRaisesRegex(airdap_dap_probe.ProbeError, "0x0021"):
                airdap_dap_probe.probe_dap(
                    "192.0.2.10", 3260, self.credential
                )


if __name__ == "__main__":
    unittest.main()
