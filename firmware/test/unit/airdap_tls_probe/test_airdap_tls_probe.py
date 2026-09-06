from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path
from unittest import mock


TOOLS_DIR = Path(__file__).resolve().parents[3] / "tools"
sys.path.insert(0, str(TOOLS_DIR))
SCRIPT = TOOLS_DIR / "airdap-tls-probe.py"
SPEC = importlib.util.spec_from_file_location("airdap_tls_probe", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
airdap_tls_probe = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = airdap_tls_probe
SPEC.loader.exec_module(airdap_tls_probe)


class FakeRawSocket:
    def __enter__(self) -> "FakeRawSocket":
        return self

    def __exit__(self, *args: object) -> None:
        return None


class FakeTlsSocket:
    negotiated_version = "TLSv1.3"
    negotiated_cipher = ("TLS_AES_128_GCM_SHA256", "TLSv1.3", 128)

    def __enter__(self) -> "FakeTlsSocket":
        return self

    def __exit__(self, *args: object) -> None:
        return None

    def version(self) -> str:
        return self.negotiated_version

    def cipher(self) -> tuple[str, str, int]:
        return self.negotiated_cipher


class FakeContext:
    instance: "FakeContext"

    def __init__(self, protocol: object) -> None:
        self.protocol = protocol
        self.minimum_version = None
        self.maximum_version = None
        self.check_hostname = True
        self.verify_mode = None
        self.callback = None
        self.wrapped = None
        FakeContext.instance = self

    def set_psk_client_callback(self, callback: object) -> None:
        self.callback = callback

    def wrap_socket(self, raw: object, *, server_hostname: object) -> FakeTlsSocket:
        self.wrapped = (raw, server_hostname)
        return FakeTlsSocket()


class AirDapTlsProbeTests(unittest.TestCase):
    def setUp(self) -> None:
        self.credential = airdap_tls_probe.NetworkCredential.create(
            "ADP-001122334455", bytes(range(32))
        )

    def test_probe_pins_tls13_psk_identity_and_cipher(self) -> None:
        raw = FakeRawSocket()
        with mock.patch.object(
            airdap_tls_probe.ssl, "SSLContext", FakeContext
        ), mock.patch.object(
            airdap_tls_probe.socket, "create_connection", return_value=raw
        ) as connect:
            result = airdap_tls_probe.probe_tls(
                "192.0.2.10", 3260, self.credential, timeout=2.5
            )

        self.assertEqual(result.version, "TLSv1.3")
        self.assertEqual(result.cipher, "TLS_AES_128_GCM_SHA256")
        connect.assert_called_once_with(("192.0.2.10", 3260), timeout=2.5)
        context = FakeContext.instance
        self.assertEqual(context.minimum_version, airdap_tls_probe.ssl.TLSVersion.TLSv1_3)
        self.assertEqual(context.maximum_version, airdap_tls_probe.ssl.TLSVersion.TLSv1_3)
        self.assertFalse(context.check_hostname)
        self.assertEqual(context.verify_mode, airdap_tls_probe.ssl.CERT_NONE)
        self.assertEqual(
            context.callback(None),
            (self.credential.identity, self.credential.psk),
        )
        self.assertEqual(context.wrapped, (raw, None))

    def test_probe_rejects_any_other_negotiated_suite(self) -> None:
        FakeTlsSocket.negotiated_cipher = ("TLS_AES_256_GCM_SHA384", "TLSv1.3", 256)
        try:
            with mock.patch.object(
                airdap_tls_probe.ssl, "SSLContext", FakeContext
            ), mock.patch.object(
                airdap_tls_probe.socket,
                "create_connection",
                return_value=FakeRawSocket(),
            ):
                with self.assertRaisesRegex(airdap_tls_probe.ProbeError, "cipher"):
                    airdap_tls_probe.probe_tls(
                        "192.0.2.10", 3260, self.credential
                    )
        finally:
            FakeTlsSocket.negotiated_cipher = (
                "TLS_AES_128_GCM_SHA256",
                "TLSv1.3",
                128,
            )


if __name__ == "__main__":
    unittest.main()
