//! Encryption of refresh tokens at rest. XChaCha20-Poly1305 (AEAD) with a
//! 32-byte master key from the environment. Output carries a key-id prefix so
//! the master key can be rotated without a flag-day. Tokens are NEVER stored or
//! logged in plaintext (see SECURITY.md).

use base64::Engine;
use chacha20poly1305::{
    aead::{Aead, KeyInit, OsRng},
    AeadCore, XChaCha20Poly1305, XNonce,
};

const KEY_ID: &str = "k1"; // bump when rotating master key

pub struct Cipher {
    inner: XChaCha20Poly1305,
}

impl Cipher {
    pub fn new(master_key: &[u8; 32]) -> Self {
        Cipher {
            inner: XChaCha20Poly1305::new(master_key.into()),
        }
    }

    /// Encrypt -> `k1:<base64(nonce)>:<base64(ciphertext)>`.
    pub fn seal(&self, plaintext: &str) -> anyhow::Result<String> {
        let nonce = XChaCha20Poly1305::generate_nonce(&mut OsRng);
        let ct = self
            .inner
            .encrypt(&nonce, plaintext.as_bytes())
            .map_err(|_| anyhow::anyhow!("encryption failed"))?;
        let b64 = base64::engine::general_purpose::STANDARD;
        Ok(format!("{KEY_ID}:{}:{}", b64.encode(nonce), b64.encode(ct)))
    }

    pub fn open(&self, sealed: &str) -> anyhow::Result<String> {
        let b64 = base64::engine::general_purpose::STANDARD;
        let mut parts = sealed.splitn(3, ':');
        let _kid = parts.next().ok_or_else(|| anyhow::anyhow!("bad sealed token"))?;
        let nonce_b = b64.decode(parts.next().ok_or_else(|| anyhow::anyhow!("no nonce"))?)?;
        let ct_b = b64.decode(parts.next().ok_or_else(|| anyhow::anyhow!("no ct"))?)?;
        let nonce = XNonce::from_slice(&nonce_b);
        let pt = self
            .inner
            .decrypt(nonce, ct_b.as_ref())
            .map_err(|_| anyhow::anyhow!("decryption failed"))?;
        Ok(String::from_utf8(pt)?)
    }
}

/// URL-safe random opaque token (relay session token, device codes, etc.).
pub fn random_token(bytes: usize) -> String {
    use rand::RngCore;
    let mut buf = vec![0u8; bytes];
    rand::thread_rng().fill_bytes(&mut buf);
    base64::engine::general_purpose::URL_SAFE_NO_PAD.encode(buf)
}

/// Short human-friendly user_code for device pairing (e.g. "WXYZ-1234").
pub fn user_code() -> String {
    use rand::Rng;
    const ALPHABET: &[u8] = b"ABCDEFGHJKLMNPQRSTUVWXYZ23456789"; // no ambiguous chars
    let mut rng = rand::thread_rng();
    let pick = |rng: &mut rand::rngs::ThreadRng, n: usize| -> String {
        (0..n)
            .map(|_| ALPHABET[rng.gen_range(0..ALPHABET.len())] as char)
            .collect()
    };
    format!("{}-{}", pick(&mut rng, 4), pick(&mut rng, 4))
}
