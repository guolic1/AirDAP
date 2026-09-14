//! Espressif Security 2: SRP-6a/SHA-512 (3072-bit group) and AES-256-GCM.
use anyhow::{Context, Result, ensure};
use openssl::{
    bn::{BigNum, BigNumContext},
    memcmp,
    rand::rand_bytes,
    sha::sha512,
    symm::{Cipher, decrypt_aead, encrypt_aead},
};
use zeroize::{Zeroize, Zeroizing};

fn minimal(bytes: &[u8]) -> &[u8] {
    let offset = bytes
        .iter()
        .position(|b| *b != 0)
        .unwrap_or(bytes.len().saturating_sub(1));
    &bytes[offset..]
}
fn hash(parts: &[&[u8]]) -> [u8; 64] {
    let input = Zeroizing::new(parts.concat());
    sha512(&input)
}

pub struct SrpClient {
    username: String,
    password: Zeroizing<String>,
    secret: BigNum,
    public: BigNum,
    n: BigNum,
    g: BigNum,
    proof: Option<[u8; 64]>,
    key: Zeroizing<Vec<u8>>,
}
impl Drop for SrpClient {
    fn drop(&mut self) {
        self.secret.clear();
    }
}
impl SrpClient {
    pub fn new(username: &str, password: &str) -> Result<Self> {
        let mut secret = Zeroizing::new([0; 32]);
        rand_bytes(&mut *secret)?;
        secret[0] |= 0x80;
        Self::from_secret(username, password, &secret[..])
    }
    fn from_secret(username: &str, password: &str, secret: &[u8]) -> Result<Self> {
        let n = BigNum::get_rfc3526_prime_3072()?;
        let g = BigNum::from_u32(5)?;
        let mut secret = BigNum::from_slice(secret)?;
        secret.set_const_time();
        let mut public = BigNum::new()?;
        let mut context = BigNumContext::new_secure()?;
        public.mod_exp(&g, &secret, &n, &mut context)?;
        Ok(Self {
            username: username.into(),
            password: Zeroizing::new(password.into()),
            secret,
            public,
            n,
            g,
            proof: None,
            key: Zeroizing::new(vec![]),
        })
    }
    pub fn public_key(&self) -> Result<Vec<u8>> {
        Ok(self.public.to_vec())
    }
    pub fn challenge(&mut self, salt: &[u8], server: &[u8]) -> Result<Vec<u8>> {
        self.proof = None;
        self.key.zeroize();
        ensure!(
            !salt.is_empty() && salt.len() <= 384 && !server.is_empty() && server.len() <= 384,
            "invalid SRP challenge length"
        );
        let b = BigNum::from_slice(server)?;
        let mut context = BigNumContext::new_secure()?;
        let mut remainder = BigNum::new()?;
        remainder.nnmod(&b, &self.n, &mut context)?;
        ensure!(remainder.num_bits() != 0, "invalid SRP server public value");
        let n = self.n.to_vec();
        let g = self.g.to_vec_padded(384)?;
        let a_padded = self.public.to_vec_padded(384)?;
        let b_padded = b.to_vec_padded(384)?;
        let u = BigNum::from_slice(&hash(&[&a_padded, &b_padded]))?;
        ensure!(u.num_bits() != 0, "invalid SRP scrambling value");
        let k = BigNum::from_slice(&hash(&[&n, &g]))?;
        let inner = Zeroizing::new(hash(&[
            self.username.as_bytes(),
            b":",
            self.password.as_bytes(),
        ]));
        // Espressif's reference encodes hash integers minimally when deriving x.
        let mut x = BigNum::from_slice(&hash(&[minimal(salt), minimal(&inner[..])]))?;
        x.set_const_time();
        let mut verifier = BigNum::new_secure()?;
        verifier.mod_exp(&self.g, &x, &self.n, &mut context)?;
        let mut kv = BigNum::new_secure()?;
        kv.mod_mul(&k, &verifier, &self.n, &mut context)?;
        let mut base = BigNum::new_secure()?;
        base.mod_sub(&b, &kv, &self.n, &mut context)?;
        let mut ux = BigNum::new_secure()?;
        ux.checked_mul(&u, &x, &mut context)?;
        let mut exponent = BigNum::new_secure()?;
        exponent.checked_add(&self.secret, &ux)?;
        exponent.set_const_time();
        let mut shared = BigNum::new_secure()?;
        shared.mod_exp(&base, &exponent, &self.n, &mut context)?;
        let shared_bytes = Zeroizing::new(shared.to_vec());
        self.key.extend_from_slice(&sha512(&shared_bytes));
        x.clear();
        verifier.clear();
        kv.clear();
        base.clear();
        ux.clear();
        exponent.clear();
        shared.clear();
        let mut xor = sha512(&n);
        let hg = sha512(&g);
        for (n, g) in xor.iter_mut().zip(hg) {
            *n ^= g;
        }
        let a = self.public.to_vec();
        let b = b.to_vec();
        let proof = hash(&[
            &xor,
            &sha512(self.username.as_bytes()),
            minimal(salt),
            &a,
            &b,
            &self.key,
        ]);
        self.proof = Some(hash(&[&a, &proof, &self.key]));
        Ok(proof.to_vec())
    }
    pub fn verify(&self, proof: &[u8], nonce: &[u8]) -> Result<SessionCipher> {
        ensure!(
            proof.len() == 64 && self.proof.as_ref().is_some_and(|p| memcmp::eq(p, proof)),
            "SRP device proof mismatch"
        );
        ensure!(
            nonce.len() == 12 && self.key.len() == 64,
            "invalid Security 2 key or nonce"
        );
        Ok(SessionCipher {
            key: Zeroizing::new(self.key[..32].try_into()?),
            nonce: nonce.try_into()?,
            valid: true,
        })
    }
}

pub struct SessionCipher {
    key: Zeroizing<[u8; 32]>,
    nonce: [u8; 12],
    valid: bool,
}
impl SessionCipher {
    fn advance(&mut self) -> Result<()> {
        let count = u32::from_be_bytes(self.nonce[8..].try_into()?);
        let Some(next) = count.checked_add(1) else {
            self.valid = false;
            anyhow::bail!("Security 2 nonce exhausted");
        };
        self.nonce[8..].copy_from_slice(&next.to_be_bytes());
        Ok(())
    }
    pub fn encrypt(&mut self, plain: &[u8]) -> Result<Vec<u8>> {
        ensure!(self.valid, "Security 2 session must be re-established");
        self.valid = false;
        let mut tag = [0; 16];
        let mut cipher = encrypt_aead(
            Cipher::aes_256_gcm(),
            &self.key[..],
            Some(&self.nonce),
            &[],
            plain,
            &mut tag,
        )?;
        cipher.extend_from_slice(&tag);
        self.advance()?;
        self.valid = true;
        Ok(cipher)
    }
    pub fn decrypt(&mut self, cipher: &[u8]) -> Result<Vec<u8>> {
        ensure!(self.valid, "Security 2 session must be re-established");
        self.valid = false;
        ensure!(cipher.len() >= 16, "invalid Security 2 ciphertext");
        let split = cipher.len() - 16;
        let plain = decrypt_aead(
            Cipher::aes_256_gcm(),
            &self.key[..],
            Some(&self.nonce),
            &[],
            &cipher[..split],
            &cipher[split..],
        )
        .context("Security 2 response authentication failed")?;
        self.advance()?;
        self.valid = true;
        Ok(plain)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn srp_matches_espressif_and_authenticates_gcm() {
        let fixture: serde_json::Value =
            serde_json::from_str(include_str!("../tests/fixtures/security2.json")).unwrap();
        let field = |key: &str| hex::decode(fixture[key].as_str().unwrap()).unwrap();
        let mut srp =
            SrpClient::from_secret("fixture-user", "fixture-password", &field("secret")).unwrap();
        assert_eq!(srp.public_key().unwrap(), field("public"));
        assert_eq!(
            srp.challenge(&field("salt"), &field("server")).unwrap(),
            field("proof")
        );
        assert!(srp.verify(&[0; 64], &[0; 12]).is_err());
        let mut cipher = srp.verify(&field("server_proof"), &[0; 12]).unwrap();
        let encrypted = cipher.encrypt(b"example").unwrap();
        let key = field("key");
        let mut expected_tag = [0; 16];
        let mut expected = openssl::symm::encrypt_aead(
            openssl::symm::Cipher::aes_256_gcm(),
            &key[..32],
            Some(&[0; 12]),
            &[],
            b"example",
            &mut expected_tag,
        )
        .unwrap();
        expected.extend_from_slice(&expected_tag);
        assert_eq!(encrypted, expected);
        let mut nonce = [0; 12];
        nonce[11] = 1;
        let mut tag = [0; 16];
        let mut reply = openssl::symm::encrypt_aead(
            openssl::symm::Cipher::aes_256_gcm(),
            &key[..32],
            Some(&nonce),
            &[],
            b"reply",
            &mut tag,
        )
        .unwrap();
        reply.extend_from_slice(&tag);
        assert_eq!(cipher.decrypt(&reply).unwrap(), b"reply");
        assert!(cipher.decrypt(&reply).is_err());
        assert!(cipher.encrypt(b"after corrupt response").is_err());
    }
    #[test]
    fn rejects_invalid_server_public_values() {
        let mut srp = SrpClient::new("fixture-user", "fixture-password").unwrap();
        assert!(srp.challenge(&[1; 16], &[0; 384]).is_err());
    }
}
