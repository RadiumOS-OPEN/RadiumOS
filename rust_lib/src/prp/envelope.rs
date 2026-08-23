use super::aead;
use super::aead::poly1305_key;
use super::chacha20;
use super::poly1305::Poly1305;
use super::random_bytes;
use crate::{avfs_create_file, avfs_get_filesize, avfs_read_file, avfs_write_file};

pub(super) const HEADER_LEN: usize = 20;
const TAG_LEN: usize = 16;
const MAGIC: [u8; 4] = *b"PRP1";
const ALGO_CHACHA20_POLY1305: u8 = 1;

fn make_header(nonce: &[u8; 12]) -> [u8; HEADER_LEN] {
    let mut header = [0u8; HEADER_LEN];
    header[..4].copy_from_slice(&MAGIC);
    header[4] = ALGO_CHACHA20_POLY1305;
    header[8..].copy_from_slice(nonce);
    header
}

pub(super) fn sealed_len(plaintext_len: usize) -> usize {
    HEADER_LEN + TAG_LEN + plaintext_len
}

pub(super) fn plaintext_len(sealed_len: usize) -> Option<usize> {
    sealed_len.checked_sub(HEADER_LEN + TAG_LEN)
}

pub(super) fn seal(
    key: &[u8; 32],
    nonce: &[u8; 12],
    plaintext: &[u8],
    out: &mut [u8],
) -> Option<()> {
    if out.len() != sealed_len(plaintext.len()) {
        return None;
    }

    let header = make_header(nonce);
    out[..HEADER_LEN].copy_from_slice(&header);
    out[HEADER_LEN + TAG_LEN..].copy_from_slice(plaintext);

    let tag = aead::encrypt(key, nonce, &header, &mut out[HEADER_LEN + TAG_LEN..])?;
    out[HEADER_LEN..HEADER_LEN + TAG_LEN].copy_from_slice(&tag);
    Some(())
}

pub(super) fn open(key: &[u8; 32], sealed: &mut [u8], out: &mut [u8]) -> Option<()> {
    if sealed.len() < HEADER_LEN + TAG_LEN || out.len() != plaintext_len(sealed.len())? {
        return None;
    }

    let mut header = [0u8; HEADER_LEN];
    header.copy_from_slice(&sealed[..HEADER_LEN]);
    if header[..4] != MAGIC || header[4] != ALGO_CHACHA20_POLY1305 || header[5..8] != [0; 3] {
        return None;
    }
    let mut nonce = [0u8; 12];
    nonce.copy_from_slice(&header[8..]);

    let mut tag = [0u8; TAG_LEN];
    tag.copy_from_slice(&sealed[HEADER_LEN..HEADER_LEN + TAG_LEN]);
    let ciphertext = &mut sealed[HEADER_LEN + TAG_LEN..];
    if !aead::decrypt(key, &nonce, &header, ciphertext, &tag) {
        return None;
    }

    out.copy_from_slice(ciphertext);
    Some(())
}

use super::sha256;
use super::x25519;

const CHUNK: usize = 512;

pub(super) fn parse_hex_key(hex: &[u8]) -> Option<[u8; 32]> {
    if hex.len() != 64 {
        return None;
    }

    fn nibble(c: u8) -> Option<u8> {
        match c {
            b'0'..=b'9' => Some(c - b'0'),
            b'a'..=b'f' => Some(c - b'a' + 10),
            b'A'..=b'F' => Some(c - b'A' + 10),
            _ => None,
        }
    }

    let mut key = [0u8; 32];
    for i in 0..32 {
        key[i] = nibble(hex[i * 2])? << 4 | nibble(hex[i * 2 + 1])?;
    }
    Some(key)
}

fn stream_blocks(len: usize) -> u32 {
    (len as u32 + 63) / 64
}

pub(super) unsafe fn seal_file(input: &[u8], output: &[u8], key_hex: &[u8]) -> i32 {
    let key = match parse_hex_key(key_hex) {
        Some(key) => key,
        None => return -1,
    };

    let size = match avfs_get_filesize(input.as_ptr()) {
        s if s >= 0 => s as usize,
        _ => return -2,
    };
    let sealed_size = HEADER_LEN + TAG_LEN + size;
    if sealed_size < size {
        return -2;
    }

    let mut nonce = [0u8; 12];
    if !random_bytes(&mut nonce) {
        return -3;
    }

    let header = make_header(&nonce);
    if avfs_create_file(output.as_ptr(), sealed_size as u32) != 0 {
        return -4;
    }
    if avfs_write_file(output.as_ptr(), header.as_ptr(), HEADER_LEN as u32, 0) != 0 {
        return -4;
    }

    let poly_key = poly1305_key(&key, &nonce);
    let mut poly = Poly1305::new(&poly_key);
    poly.update(&header);

    let mut buffer = [0u8; CHUNK];
    let mut offset = 0usize;
    let mut counter: u32 = 1;
    while offset < size {
        let count = (size - offset).min(CHUNK);
        if avfs_read_file(
            input.as_ptr(),
            buffer.as_mut_ptr(),
            count as u32,
            offset as u32,
        ) != 0
        {
            return -5;
        }

        let chunk = &mut buffer[..count];
        if !chacha20::apply(&key, &nonce, counter, chunk) {
            return -3;
        }
        counter = match counter.checked_add(stream_blocks(count)) {
            Some(next) => next,
            None => return -3,
        };

        poly.update(chunk);
        if avfs_write_file(
            output.as_ptr(),
            chunk.as_ptr(),
            count as u32,
            (HEADER_LEN + TAG_LEN + offset) as u32,
        ) != 0
        {
            return -4;
        }
        offset += count;
    }

    poly.update(&(HEADER_LEN as u64).to_le_bytes());
    poly.update(&(size as u64).to_le_bytes());

    let tag = poly.finish();
    if avfs_write_file(
        output.as_ptr(),
        tag.as_ptr(),
        TAG_LEN as u32,
        HEADER_LEN as u32,
    ) != 0
    {
        return -4;
    }
    0
}

const PRPSIG_MAGIC: [u8; 7] = *b"PRPSIG1";
const TRAILER_LEN: usize = 7 + 32 + 64;

// envelopes signed after encryption carry a PRPSIG1 trailer past the ciphertext
unsafe fn signed_trailer_len(input: &[u8], size: usize) -> usize {
    if size >= TRAILER_LEN {
        let mut tail = [0u8; 7];
        if avfs_read_file(
            input.as_ptr(),
            tail.as_mut_ptr(),
            7,
            (size - TRAILER_LEN) as u32,
        ) == 0
            && tail == PRPSIG_MAGIC
        {
            return TRAILER_LEN;
        }
    }
    0
}

pub(super) unsafe fn open_file(input: &[u8], output: &[u8], key_hex: &[u8]) -> i32 {
    let key = match parse_hex_key(key_hex) {
        Some(key) => key,
        None => return -1,
    };

    let size = match avfs_get_filesize(input.as_ptr()) {
        s if s >= 0 => s as usize,
        _ => return -2,
    };
    let size = size - signed_trailer_len(input, size);
    if size < HEADER_LEN + TAG_LEN {
        return -2;
    }
    let plaintext_size = size - HEADER_LEN - TAG_LEN;

    let mut header = [0u8; HEADER_LEN];
    if avfs_read_file(input.as_ptr(), header.as_mut_ptr(), HEADER_LEN as u32, 0) != 0 {
        return -5;
    }
    if header[..4] != MAGIC || header[4] != ALGO_CHACHA20_POLY1305 || header[5..8] != [0; 3] {
        return -6;
    }
    let mut nonce = [0u8; 12];
    nonce.copy_from_slice(&header[8..]);

    let mut stored_tag = [0u8; TAG_LEN];
    if avfs_read_file(
        input.as_ptr(),
        stored_tag.as_mut_ptr(),
        TAG_LEN as u32,
        HEADER_LEN as u32,
    ) != 0
    {
        return -5;
    }

    let poly_key = poly1305_key(&key, &nonce);
    let mut poly = Poly1305::new(&poly_key);
    poly.update(&header);

    let mut buffer = [0u8; CHUNK];
    let mut offset = 0usize;
    while offset < plaintext_size {
        let count = (plaintext_size - offset).min(CHUNK);
        if avfs_read_file(
            input.as_ptr(),
            buffer.as_mut_ptr(),
            count as u32,
            (HEADER_LEN + TAG_LEN + offset) as u32,
        ) != 0
        {
            return -5;
        }
        poly.update(&buffer[..count]);
        offset += count;
    }
    poly.update(&(HEADER_LEN as u64).to_le_bytes());
    poly.update(&(plaintext_size as u64).to_le_bytes());

    let expected = poly.finish();
    if !aead::tags_equal(&expected, &stored_tag) {
        return -7;
    }

    if avfs_create_file(output.as_ptr(), plaintext_size as u32) != 0 {
        return -4;
    }

    let mut counter: u32 = 1;
    offset = 0;
    while offset < plaintext_size {
        let count = (plaintext_size - offset).min(CHUNK);
        if avfs_read_file(
            input.as_ptr(),
            buffer.as_mut_ptr(),
            count as u32,
            (HEADER_LEN + TAG_LEN + offset) as u32,
        ) != 0
        {
            return -5;
        }

        let chunk = &mut buffer[..count];
        if !chacha20::apply(&key, &nonce, counter, chunk) {
            return -3;
        }
        counter = match counter.checked_add(stream_blocks(count)) {
            Some(next) => next,
            None => return -3,
        };

        if avfs_write_file(output.as_ptr(), chunk.as_ptr(), count as u32, offset as u32) != 0 {
            return -4;
        }
        offset += count;
    }
    0
}

pub(super) unsafe fn seal_file_public(
    input: &[u8],
    output: &[u8],
    recipient_pub: &[u8; 32],
) -> i32 {
    let size = match avfs_get_filesize(input.as_ptr()) {
        s if s >= 0 => s as usize,
        _ => return -2,
    };
    const PREFIX: usize = HEADER_LEN + 32 + 12 + 48;
    let sealed_size = PREFIX + TAG_LEN + size;
    if sealed_size < size {
        return -2;
    }

    let mut data_nonce = [0u8; 12];
    let mut wrap_nonce = [0u8; 12];
    let mut session_key = [0u8; 32];
    let mut eph_priv = [0u8; 32];
    if !random_bytes(&mut data_nonce)
        || !random_bytes(&mut wrap_nonce)
        || !random_bytes(&mut session_key)
        || !random_bytes(&mut eph_priv)
    {
        return -3;
    }
    eph_priv[0] &= 248;
    eph_priv[31] &= 127;
    eph_priv[31] |= 64;
    let eph_pub = x25519::x25519(&eph_priv, &x25519::BASEPOINT);

    let mut header = make_header(&data_nonce);
    header[4] = 2;
    let shared = x25519::x25519(&eph_priv, recipient_pub);
    let kek = sha256(&shared);

    let mut aad = [0u8; HEADER_LEN + 32];
    aad[..HEADER_LEN].copy_from_slice(&header);
    aad[HEADER_LEN..].copy_from_slice(&eph_pub);

    let mut wrapped = [0u8; 48];
    wrapped[..32].copy_from_slice(&session_key);
    let wrap_tag = match aead::encrypt(&kek, &wrap_nonce, &aad, &mut wrapped[..32]) {
        Some(tag) => tag,
        None => return -3,
    };
    wrapped[32..].copy_from_slice(&wrap_tag);

    if avfs_create_file(output.as_ptr(), sealed_size as u32) != 0 {
        return -4;
    }
    let mut front = [0u8; PREFIX];
    front[..HEADER_LEN].copy_from_slice(&header);
    front[HEADER_LEN..HEADER_LEN + 32].copy_from_slice(&eph_pub);
    front[HEADER_LEN + 32..HEADER_LEN + 44].copy_from_slice(&wrap_nonce);
    front[HEADER_LEN + 44..].copy_from_slice(&wrapped);
    if avfs_write_file(output.as_ptr(), front.as_ptr(), PREFIX as u32, 0) != 0 {
        return -4;
    }

    let poly_key = poly1305_key(&session_key, &data_nonce);
    let mut poly = Poly1305::new(&poly_key);
    poly.update(&front);

    let mut buffer = [0u8; CHUNK];
    let mut offset = 0usize;
    let mut counter: u32 = 1;
    while offset < size {
        let count = (size - offset).min(CHUNK);
        if avfs_read_file(
            input.as_ptr(),
            buffer.as_mut_ptr(),
            count as u32,
            offset as u32,
        ) != 0
        {
            return -5;
        }

        let chunk = &mut buffer[..count];
        if !chacha20::apply(&session_key, &data_nonce, counter, chunk) {
            return -3;
        }
        counter = match counter.checked_add(stream_blocks(count)) {
            Some(next) => next,
            None => return -3,
        };

        poly.update(chunk);
        if avfs_write_file(
            output.as_ptr(),
            chunk.as_ptr(),
            count as u32,
            (PREFIX + offset) as u32,
        ) != 0
        {
            return -4;
        }
        offset += count;
    }

    poly.update(&(PREFIX as u64).to_le_bytes());
    poly.update(&(size as u64).to_le_bytes());
    let tag = poly.finish();
    if avfs_write_file(
        output.as_ptr(),
        tag.as_ptr(),
        TAG_LEN as u32,
        (PREFIX + size) as u32,
    ) != 0
    {
        return -4;
    }
    0
}

pub(super) unsafe fn open_file_private(input: &[u8], output: &[u8], private_key: &[u8; 32]) -> i32 {
    let size = match avfs_get_filesize(input.as_ptr()) {
        s if s >= 0 => s as usize,
        _ => return -2,
    };
    let size = size - signed_trailer_len(input, size);
    const PREFIX: usize = HEADER_LEN + 32 + 12 + 48;
    if size < PREFIX + TAG_LEN {
        return -2;
    }
    let plaintext_size = size - PREFIX - TAG_LEN;

    let mut front = [0u8; PREFIX];
    if avfs_read_file(input.as_ptr(), front.as_mut_ptr(), PREFIX as u32, 0) != 0 {
        return -5;
    }
    if front[..4] != MAGIC || front[4] != 2 || front[5..8] != [0; 3] {
        return -6;
    }
    let mut data_nonce = [0u8; 12];
    data_nonce.copy_from_slice(&front[8..HEADER_LEN]);
    let mut eph_pub = [0u8; 32];
    eph_pub.copy_from_slice(&front[HEADER_LEN..HEADER_LEN + 32]);
    let mut wrap_nonce = [0u8; 12];
    wrap_nonce.copy_from_slice(&front[HEADER_LEN + 32..HEADER_LEN + 44]);

    let shared = x25519::x25519(private_key, &eph_pub);
    let kek = sha256(&shared);

    let mut aad = [0u8; HEADER_LEN + 32];
    aad.copy_from_slice(&front[..HEADER_LEN + 32]);

    let mut wrapped = [0u8; 32];
    wrapped.copy_from_slice(&front[HEADER_LEN + 44..HEADER_LEN + 76]);
    let mut wrap_tag = [0u8; 16];
    wrap_tag.copy_from_slice(&front[HEADER_LEN + 76..]);
    if !aead::decrypt(&kek, &wrap_nonce, &aad, &mut wrapped, &wrap_tag) {
        return -7;
    }
    let session_key = wrapped;

    let poly_key = poly1305_key(&session_key, &data_nonce);
    let mut poly = Poly1305::new(&poly_key);
    poly.update(&front);

    let mut buffer = [0u8; CHUNK];
    let mut offset = 0usize;
    while offset < plaintext_size {
        let count = (plaintext_size - offset).min(CHUNK);
        if avfs_read_file(
            input.as_ptr(),
            buffer.as_mut_ptr(),
            count as u32,
            (PREFIX + offset) as u32,
        ) != 0
        {
            return -5;
        }
        poly.update(&buffer[..count]);
        offset += count;
    }
    poly.update(&(PREFIX as u64).to_le_bytes());
    poly.update(&(plaintext_size as u64).to_le_bytes());

    let mut stored_tag = [0u8; TAG_LEN];
    if avfs_read_file(
        input.as_ptr(),
        stored_tag.as_mut_ptr(),
        TAG_LEN as u32,
        (PREFIX + plaintext_size) as u32,
    ) != 0
    {
        return -5;
    }
    if !aead::tags_equal(&poly.finish(), &stored_tag) {
        return -7;
    }

    if avfs_create_file(output.as_ptr(), plaintext_size as u32) != 0 {
        return -4;
    }

    let mut counter: u32 = 1;
    offset = 0;
    while offset < plaintext_size {
        let count = (plaintext_size - offset).min(CHUNK);
        if avfs_read_file(
            input.as_ptr(),
            buffer.as_mut_ptr(),
            count as u32,
            (PREFIX + offset) as u32,
        ) != 0
        {
            return -5;
        }

        let chunk = &mut buffer[..count];
        if !chacha20::apply(&session_key, &data_nonce, counter, chunk) {
            return -3;
        }
        counter = match counter.checked_add(stream_blocks(count)) {
            Some(next) => next,
            None => return -3,
        };

        if avfs_write_file(output.as_ptr(), chunk.as_ptr(), count as u32, offset as u32) != 0 {
            return -4;
        }
        offset += count;
    }
    0
}

pub(super) fn seal_public(
    recipient_pub: &[u8; 32],
    plaintext: &[u8],
    out: &mut [u8],
) -> Option<()> {
    const PREFIX: usize = HEADER_LEN + 32 + 12 + 48;
    if out.len() != PREFIX + TAG_LEN + plaintext.len() {
        return None;
    }

    let mut data_nonce = [0u8; 12];
    let mut wrap_nonce = [0u8; 12];
    let mut session_key = [0u8; 32];
    let mut eph_priv = [0u8; 32];
    if !random_bytes(&mut data_nonce)
        || !random_bytes(&mut wrap_nonce)
        || !random_bytes(&mut session_key)
        || !random_bytes(&mut eph_priv)
    {
        return None;
    }
    eph_priv[0] &= 248;
    eph_priv[31] &= 127;
    eph_priv[31] |= 64;
    let eph_pub = x25519::x25519(&eph_priv, &x25519::BASEPOINT);

    let mut header = make_header(&data_nonce);
    header[4] = 2;
    let shared = x25519::x25519(&eph_priv, recipient_pub);
    let kek = sha256(&shared);

    let mut aad = [0u8; HEADER_LEN + 32];
    aad[..HEADER_LEN].copy_from_slice(&header);
    aad[HEADER_LEN..].copy_from_slice(&eph_pub);

    let mut wrapped = [0u8; 48];
    wrapped[..32].copy_from_slice(&session_key);
    let wrap_tag = aead::encrypt(&kek, &wrap_nonce, &aad, &mut wrapped[..32])?;
    wrapped[32..].copy_from_slice(&wrap_tag);

    out[PREFIX..].copy_from_slice(plaintext);
    chacha20::apply(&session_key, &data_nonce, 1, &mut out[PREFIX..]);

    out[..HEADER_LEN].copy_from_slice(&header);
    out[HEADER_LEN..HEADER_LEN + 32].copy_from_slice(&eph_pub);
    out[HEADER_LEN + 32..HEADER_LEN + 44].copy_from_slice(&wrap_nonce);
    out[HEADER_LEN + 44..PREFIX].copy_from_slice(&wrapped);

    let poly_key = poly1305_key(&session_key, &data_nonce);
    let mut poly = Poly1305::new(&poly_key);
    poly.update(&out[..PREFIX]);
    poly.update(&out[PREFIX..]);
    poly.update(&(PREFIX as u64).to_le_bytes());
    poly.update(&(plaintext.len() as u64).to_le_bytes());
    let tag = poly.finish();

    out[PREFIX + plaintext.len()..].copy_from_slice(&tag);
    Some(())
}

pub(super) fn selftest() -> bool {
    let key = [7u8; 32];
    let nonce = [
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c,
    ];
    const PLAINTEXT: &[u8; 14] = b"attack at dawn";

    let mut sealed = [0u8; HEADER_LEN + TAG_LEN + PLAINTEXT.len()];
    if seal(&key, &nonce, PLAINTEXT, &mut sealed).is_none() {
        return false;
    }

    let mut opened = [0u8; PLAINTEXT.len()];
    if open(&key, &mut sealed, &mut opened).is_none() {
        return false;
    }
    if opened != *PLAINTEXT {
        return false;
    }

    sealed[2] ^= 1;
    !open(&key, &mut sealed, &mut opened).is_some()
}
