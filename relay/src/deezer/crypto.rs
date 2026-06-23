//! Deezer track decryption: Blowfish-CBC over every 3rd 2048-byte chunk, with a
//! key derived from the track id. Pure + deterministic (unit-tested).

use blowfish::cipher::{BlockDecrypt, KeyInit};
use blowfish::Blowfish;
use md5::{Digest, Md5};

/// Fixed Deezer secret used to derive the per-track Blowfish key.
const SECRET: &[u8; 16] = b"g4el58wc0zvf9na1";
/// Fixed IV for the Blowfish-CBC chunks.
const IV: [u8; 8] = [0, 1, 2, 3, 4, 5, 6, 7];
const CHUNK: usize = 2048;

/// Derive the 16-byte Blowfish key for a track id (Deezer's scheme):
/// `key[i] = md5hex[i] ^ md5hex[i+16] ^ SECRET[i]`, md5hex = lowercase hex of
/// md5(track_id ascii).
pub fn blowfish_key(track_id: &str) -> [u8; 16] {
    let digest = Md5::digest(track_id.as_bytes());
    let mut hex = [0u8; 32];
    const HEXCH: &[u8; 16] = b"0123456789abcdef";
    for (i, b) in digest.iter().enumerate() {
        hex[i * 2] = HEXCH[(b >> 4) as usize];
        hex[i * 2 + 1] = HEXCH[(b & 0x0f) as usize];
    }
    let mut key = [0u8; 16];
    for i in 0..16 {
        key[i] = hex[i] ^ hex[i + 16] ^ SECRET[i];
    }
    key
}

/// Decrypt a downloaded Deezer track in place-ish: every 3rd 2048-byte chunk is
/// Blowfish-CBC encrypted; the rest is plaintext. Trailing partial chunk (< 2048)
/// is always plaintext.
pub fn decrypt_track(track_id: &str, data: &[u8]) -> Vec<u8> {
    let key = blowfish_key(track_id);
    // Default Blowfish byte order is big-endian, matching OpenSSL BF-CBC / Deezer.
    let cipher: Blowfish = Blowfish::new_from_slice(&key).expect("blowfish key length");
    let mut out = Vec::with_capacity(data.len());
    for (i, chunk) in data.chunks(CHUNK).enumerate() {
        if i % 3 == 0 && chunk.len() == CHUNK {
            out.extend_from_slice(&decrypt_chunk(&cipher, chunk));
        } else {
            out.extend_from_slice(chunk);
        }
    }
    out
}

fn decrypt_chunk<C: BlockDecrypt>(cipher: &C, chunk: &[u8]) -> Vec<u8> {
    use blowfish::cipher::generic_array::GenericArray;
    let mut out = vec![0u8; chunk.len()];
    let mut prev = IV;
    for (i, block) in chunk.chunks(8).enumerate() {
        let ct: [u8; 8] = block.try_into().unwrap();
        let mut b = GenericArray::clone_from_slice(block);
        cipher.decrypt_block(&mut b);
        let o = &mut out[i * 8..i * 8 + 8];
        for k in 0..8 {
            o[k] = b[k] ^ prev[k];
        }
        prev = ct;
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn key_is_deterministic_16_bytes() {
        let k1 = blowfish_key("3135556");
        let k2 = blowfish_key("3135556");
        assert_eq!(k1, k2);
        assert_eq!(k1.len(), 16);
        // different ids -> different keys
        assert_ne!(blowfish_key("3135556"), blowfish_key("3135557"));
    }

    #[test]
    fn non_encrypted_chunks_passthrough() {
        // chunks 1 and 2 (i%3 != 0) must be returned verbatim.
        let mut data = vec![0u8; CHUNK * 3];
        for (i, b) in data.iter_mut().enumerate() {
            *b = (i % 251) as u8;
        }
        let out = decrypt_track("3135556", &data);
        assert_eq!(out.len(), data.len());
        // chunk index 1 and 2 unchanged
        assert_eq!(out[CHUNK..CHUNK * 3], data[CHUNK..CHUNK * 3]);
        // chunk 0 was decrypted -> almost certainly changed
        assert_ne!(out[0..CHUNK], data[0..CHUNK]);
    }
}
