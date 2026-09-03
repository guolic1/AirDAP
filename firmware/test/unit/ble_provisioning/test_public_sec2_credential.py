from __future__ import annotations

import hashlib
import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / "components" / "ble_provisioning" / "sec2_credentials.c"
HEADER = (
    ROOT
    / "components"
    / "ble_provisioning"
    / "private_include"
    / "airdap_sec2_credentials.h"
)
SRP_GENERATOR = 5
SRP_3072_MODULUS = int(
    "FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD129024E088A67CC"
    "74020BBEA63B139B22514A08798E3404DDEF9519B3CD3A431B302B0A6DF25F"
    "14374FE1356D6D51C245E485B576625E7EC6F44C42E9A637ED6B0BFF5CB6F4"
    "06B7EDEE386BFB5A899FA5AE9F24117C4B1FE649286651ECE45B3DC2007CB8"
    "A163BF0598DA48361C55D39A69163FA8FD24CF5F83655D23DCA3AD961C62F35"
    "6208552BB9ED529077096966D670C354E4ABC9804F1746C08CA18217C32905E"
    "462E36CE3BE39E772C180E86039B2783A2EC07A28FB5C55DF06F4C52C9DE2B"
    "CBF6955817183995497CEA956AE515D2261898FA051015728E5A8AAAC42DAD3"
    "3170D04507A33A85521ABDF1CBA64ECFB850458DBEF0A8AEA71575D060C7DB3"
    "970F85A6E1E4C7ABF5AE8CDB0933D71E8C94E04A25619DCEE3D2261AD2EE6"
    "BF12FFA06D98A0864D87602733EC86A64521F2B18177B200CBBE117577A615D"
    "6C770988C0BAD946E208E24FA074E5AB3143DB5BFCE0FD108E4B82D120A93AD"
    "2CAFFFFFFFFFFFFFFFF",
    16,
)


def macro(text: str, name: str) -> str:
    match = re.search(rf'^#define {name} "([^"]+)"$', text, re.MULTILINE)
    if match is None:
        raise AssertionError(f"missing {name}")
    return match.group(1)


def byte_array(text: str, name: str) -> bytes:
    match = re.search(
        rf"static const uint8_t {name}\[[^]]+\] = \{{([^}}]+)\}};",
        text,
        re.DOTALL,
    )
    if match is None:
        raise AssertionError(f"missing {name}")
    return bytes(int(value, 16) for value in re.findall(r"0x([0-9a-f]{2})", match.group(1)))


class PublicSecurity2CredentialTests(unittest.TestCase):
    def test_verifier_matches_published_username_and_pop(self) -> None:
        header = HEADER.read_text(encoding="utf-8")
        source = SOURCE.read_text(encoding="utf-8")
        username = macro(header, "AIRDAP_SEC2_USERNAME")
        password = macro(header, "AIRDAP_SEC2_POP")
        salt = byte_array(source, "public_salt")
        verifier = byte_array(source, "public_verifier")

        self.assertEqual(len(salt), 16)
        self.assertEqual(len(verifier), 384)
        identity_hash = hashlib.sha512(
            f"{username}:{password}".encode("utf-8")
        ).digest()
        private_key = int.from_bytes(
            hashlib.sha512(salt + identity_hash).digest(),
            "big",
        )
        expected = pow(SRP_GENERATOR, private_key, SRP_3072_MODULUS).to_bytes(
            384,
            "big",
        )
        self.assertEqual(verifier, expected)
        fingerprint = hashlib.sha256(
            b"AirDAP-Security2-v1" + salt + verifier
        ).hexdigest().upper()
        self.assertIn(f'"{fingerprint}"', source)


if __name__ == "__main__":
    unittest.main()
