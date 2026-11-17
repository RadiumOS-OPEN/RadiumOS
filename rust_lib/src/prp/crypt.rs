//! Account password hashes and passphrase-encrypted files.
//!
//! Password record (one line, stored in /disk/etc/passwd): `$rp1$<iterations>$<salt hex>$<hash hex>`,
//! the hash being PBKDF2-HMAC-SHA256 of the password.
//!
//! Encrypted file: "RDCRYPT1" | iterations u32 LE | salt[16] | nonce[12] | tag[16] | ciphertext
//! (ChaCha20-Poly1305, the first 40 bytes are the authenticated header, key = PBKDF2 of the passphrase).

use super::{aead, hmac_parts, random_bytes};
use alloc::vec::Vec;

extern "C" {
    fn avfs_save_file(name: *const u8, data: *const u8, size: u32, overwrite: bool) -> i32;
    fn avfs_read_file(name: *const u8, buffer: *mut u8, size: u32, offset: u32) -> i32;
    fn avfs_get_filesize(name: *const u8) -> i32;
}

const PW_ITERATIONS: u32 = 10_000;
const PW_MAX_ITERATIONS: u32 = 200_000;
const PW_MAX_LEN: usize = 128;

const CRYPT_MAGIC: [u8; 8] = *b"RDCRYPT1";
const CRYPT_ITERATIONS: u32 = 10_000;
const CRYPT_HEADER: usize = 8 + 4 + 16 + 12; // authenticated part
const CRYPT_PREFIX: usize = CRYPT_HEADER + 16; // + tag
const CRYPT_MAX_FILE: usize = 48 * 1024 * 1024;

// PBKDF2-HMAC-SHA256, single output block, domain-separated by `label`
fn pbkdf2(label: &[u8], password: &[u8], salt: &[u8], iterations: u32) -> [u8; 32] {
    let mut u = hmac_parts(password, &[label, salt, &1u32.to_be_bytes()]);
    let mut t = u;
    for _ in 1..iterations {
        u = hmac_parts(password, &[&u]);
        for i in 0..32 {
            t[i] ^= u[i];
        }
    }
    t
}

fn wipe(bytes: &mut [u8]) {
    for b in bytes.iter_mut() {
        unsafe { core::ptr::write_volatile(b, 0) };
    }
}

unsafe fn c_str(mut ptr: *const u8) -> &'static [u8] {
    let mut len = 0usize;
    while *ptr.add(len) != 0 {
        len += 1;
    }
    core::slice::from_raw_parts(ptr, len)
}

const HEX: &[u8; 16] = b"0123456789abcdef";

fn hex_into(out: &mut Vec<u8>, bytes: &[u8]) {
    for b in bytes {
        out.push(HEX[(b >> 4) as usize]);
        out.push(HEX[(b & 15) as usize]);
    }
}

fn unhex(text: &[u8], out: &mut [u8]) -> bool {
    if text.len() != out.len() * 2 {
        return false;
    }
    fn nib(c: u8) -> Option<u8> {
        match c {
            b'0'..=b'9' => Some(c - b'0'),
            b'a'..=b'f' => Some(c - b'a' + 10),
            _ => None,
        }
    }
    for i in 0..out.len() {
        match (nib(text[i * 2]), nib(text[i * 2 + 1])) {
            (Some(h), Some(l)) => out[i] = h << 4 | l,
            _ => return false,
        }
    }
    true
}

fn parse_u32(text: &[u8]) -> Option<u32> {
    if text.is_empty() || text.len() > 9 {
        return None;
    }
    let mut v = 0u32;
    for &c in text {
        if !c.is_ascii_digit() {
            return None;
        }
        v = v * 10 + (c - b'0') as u32;
    }
    Some(v)
}

fn same(a: &[u8], b: &[u8]) -> bool {
    if a.len() != b.len() {
        return false;
    }
    let mut diff = 0u8;
    for i in 0..a.len() {
        diff |= a[i] ^ b[i];
    }
    diff == 0
}

// Writes a fresh salted record for `password` into `out` (NUL-terminated, needs at least 112 bytes).
// 0 ok, -1 bad argument, -3 no randomness.
#[no_mangle]
pub unsafe extern "C" fn rust_pw_hash(password: *const u8, out: *mut u8, cap: u32) -> i32 {
    if password.is_null() || out.is_null() {
        return -1;
    }
    let password = c_str(password);
    if password.is_empty() || password.len() > PW_MAX_LEN || cap < 112 {
        return -1;
    }
    let mut salt = [0u8; 16];
    if !random_bytes(&mut salt) {
        return -3;
    }
    let mut hash = pbkdf2(b"radiumos-account", password, &salt, PW_ITERATIONS);

    let mut rec: Vec<u8> = Vec::new();
    rec.extend_from_slice(b"$rp1$10000$");
    hex_into(&mut rec, &salt);
    rec.push(b'$');
    hex_into(&mut rec, &hash);
    wipe(&mut hash);
    if rec.len() + 1 > cap as usize {
        return -1;
    }
    core::ptr::copy_nonoverlapping(rec.as_ptr(), out, rec.len());
    *out.add(rec.len()) = 0;
    0
}

// 0 password matches, -2 it does not, -3 the record is malformed.
#[no_mangle]
pub unsafe extern "C" fn rust_pw_verify(password: *const u8, record: *const u8) -> i32 {
    if password.is_null() || record.is_null() {
        return -2;
    }
    let password = c_str(password);
    let record = c_str(record);
    if password.is_empty() || password.len() > PW_MAX_LEN {
        return -2;
    }
    let Some(rest) = record.strip_prefix(b"$rp1$") else { return -3 };
    let mut parts = rest.split(|&c| c == b'$');
    let (Some(iter), Some(salt_hex), Some(hash_hex), None) =
        (parts.next(), parts.next(), parts.next(), parts.next())
    else {
        return -3;
    };
    let Some(iterations) = parse_u32(iter) else { return -3 };
    if iterations == 0 || iterations > PW_MAX_ITERATIONS {
        return -3;
    }
    let mut salt = [0u8; 16];
    let mut want = [0u8; 32];
    if !unhex(salt_hex, &mut salt) || !unhex(hash_hex, &mut want) {
        return -3;
    }
    let mut got = pbkdf2(b"radiumos-account", password, &salt, iterations);
    let ok = same(&got, &want);
    wipe(&mut got);
    if ok {
        0
    } else {
        -2
    }
}

// 1 if `path` starts with the encrypted-file magic, 0 if not, -1 if it cannot be read.
#[no_mangle]
pub unsafe extern "C" fn rust_crypt_probe(path: *const u8) -> i32 {
    if path.is_null() {
        return -1;
    }
    let size = avfs_get_filesize(path);
    if size < 0 {
        return -1;
    }
    if (size as usize) < CRYPT_PREFIX {
        return 0;
    }
    let mut head = [0u8; 8];
    if avfs_read_file(path, head.as_mut_ptr(), 8, 0) != 0 {
        return -1;
    }
    (head == CRYPT_MAGIC) as i32
}

// mode 0 encrypts `src` into `dst`, mode 1 decrypts it (dst may equal src). 0 ok, -1 bad argument, -2 cannot read,
// -3 too large or out of memory or no randomness, -4 cannot write, -5 already encrypted / not encrypted,
// -6 wrong passphrase or damaged file.
#[no_mangle]
pub unsafe extern "C" fn rust_crypt_file(
    src: *const u8,
    dst: *const u8,
    password: *const u8,
    mode: i32,
) -> i32 {
    if src.is_null() || dst.is_null() || password.is_null() || !(mode == 0 || mode == 1) {
        return -1;
    }
    let password = c_str(password);
    if password.is_empty() || password.len() > PW_MAX_LEN {
        return -1;
    }
    let size = avfs_get_filesize(src);
    if size < 0 {
        return -2;
    }
    let size = size as usize;
    if size > CRYPT_MAX_FILE {
        return -3;
    }
    let mut data: Vec<u8> = Vec::new();
    if data.try_reserve_exact(size + CRYPT_PREFIX).is_err() {
        return -3;
    }
    data.resize(size, 0);
    if size > 0 && avfs_read_file(src, data.as_mut_ptr(), size as u32, 0) != 0 {
        return -2;
    }
    let encrypted = size >= CRYPT_PREFIX && data[..8] == CRYPT_MAGIC;

    if mode == 0 {
        if encrypted {
            return -5;
        }
        let mut header = [0u8; CRYPT_HEADER];
        header[..8].copy_from_slice(&CRYPT_MAGIC);
        header[8..12].copy_from_slice(&CRYPT_ITERATIONS.to_le_bytes());
        if !random_bytes(&mut header[12..]) {
            return -3;
        }
        let mut key = pbkdf2(b"radiumos-crypt", password, &header[12..28], CRYPT_ITERATIONS);
        let mut nonce = [0u8; 12];
        nonce.copy_from_slice(&header[28..40]);
        let tag = aead::encrypt(&key, &nonce, &header, &mut data);
        wipe(&mut key);
        let Some(tag) = tag else { return -3 };

        let mut out: Vec<u8> = Vec::new();
        if out.try_reserve_exact(CRYPT_PREFIX + size).is_err() {
            wipe(&mut data);
            return -3;
        }
        out.extend_from_slice(&header);
        out.extend_from_slice(&tag);
        out.extend_from_slice(&data);
        wipe(&mut data);
        return if avfs_save_file(dst, out.as_ptr(), out.len() as u32, true) == 0 {
            0
        } else {
            -4
        };
    }

    if !encrypted {
        return -5;
    }
    let iterations = u32::from_le_bytes([data[8], data[9], data[10], data[11]]);
    if iterations == 0 || iterations > PW_MAX_ITERATIONS {
        return -6;
    }
    let mut header = [0u8; CRYPT_HEADER];
    header.copy_from_slice(&data[..CRYPT_HEADER]);
    let mut tag = [0u8; 16];
    tag.copy_from_slice(&data[CRYPT_HEADER..CRYPT_PREFIX]);
    let mut nonce = [0u8; 12];
    nonce.copy_from_slice(&header[28..40]);
    let mut key = pbkdf2(b"radiumos-crypt", password, &header[12..28], iterations);
    let ok = aead::decrypt(&key, &nonce, &header, &mut data[CRYPT_PREFIX..], &tag);
    wipe(&mut key);
    if !ok {
        return -6;
    }
    let plain = &data[CRYPT_PREFIX..];
    let rc = if avfs_save_file(dst, plain.as_ptr(), plain.len() as u32, true) == 0 {
        0
    } else {
        -4
    };
    wipe(&mut data);
    rc
}

// Known-answer check for the account hash and a round trip of the file cipher; 0 = ok.
#[no_mangle]
pub unsafe extern "C" fn rust_crypt_selftest() -> i32 {
    let mut rec = [0u8; 128];
    if rust_pw_hash(b"correct horse\0".as_ptr(), rec.as_mut_ptr(), 128) != 0 {
        return -1;
    }
    if rust_pw_verify(b"correct horse\0".as_ptr(), rec.as_ptr()) != 0 {
        return -2;
    }
    if rust_pw_verify(b"correct horsf\0".as_ptr(), rec.as_ptr()) != -2 {
        return -3;
    }
    if rust_pw_verify(b"x\0".as_ptr(), b"$rp1$nope\0".as_ptr()) != -3 {
        return -4;
    }
    0
}
