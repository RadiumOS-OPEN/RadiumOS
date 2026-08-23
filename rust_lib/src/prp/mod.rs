//! Pretty Radiastic Privacy

mod aead;
mod chacha20;
mod ed25519;
mod envelope;
mod poly1305;
mod sha512;
mod x25519;

extern "C" {
    fn cpu_rdrand32(value: *mut u32) -> i32;
}

use crate::{
    avfs_append_file, avfs_create_file, avfs_file_exists, avfs_get_filesize, avfs_read_file,
    avfs_remove_file, avfs_write_file,
};

fn choose(x: u32, y: u32, z: u32) -> u32 {
    (x & y) ^ (!x & z)
}

fn majority(x: u32, y: u32, z: u32) -> u32 {
    (x & y) ^ (x & z) ^ (y & z)
}

fn big_sigma0(x: u32) -> u32 {
    x.rotate_right(2) ^ x.rotate_right(13) ^ x.rotate_right(22)
}

fn big_sigma1(x: u32) -> u32 {
    x.rotate_right(6) ^ x.rotate_right(11) ^ x.rotate_right(25)
}

fn small_sigma0(x: u32) -> u32 {
    x.rotate_right(7) ^ x.rotate_right(18) ^ (x >> 3)
}

fn small_sigma1(x: u32) -> u32 {
    x.rotate_right(17) ^ x.rotate_right(19) ^ (x >> 10)
}

const INITIAL_STATE: [u32; 8] = [
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
];

const ROUND_CONSTANTS: [u32; 64] = [
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
];

fn message_schedule(block: &[u8; 64]) -> [u32; 64] {
    let mut words = [0u32; 64];

    for i in 0..16 {
        let offset = i * 4;
        words[i] = u32::from_be_bytes([
            block[offset],
            block[offset + 1],
            block[offset + 2],
            block[offset + 3],
        ]);
    }

    for i in 16..64 {
        words[i] = small_sigma1(words[i - 2])
            .wrapping_add(words[i - 7])
            .wrapping_add(small_sigma0(words[i - 15]))
            .wrapping_add(words[i - 16]);
    }

    words
}

fn compress(state: &mut [u32; 8], block: &[u8; 64]) {
    let words = message_schedule(block);
    let mut a = state[0];
    let mut b = state[1];
    let mut c = state[2];
    let mut d = state[3];
    let mut e = state[4];
    let mut f = state[5];
    let mut g = state[6];
    let mut h = state[7];

    for i in 0..64 {
        let temp1 = h
            .wrapping_add(big_sigma1(e))
            .wrapping_add(choose(e, f, g))
            .wrapping_add(ROUND_CONSTANTS[i])
            .wrapping_add(words[i]);
        let temp2 = big_sigma0(a).wrapping_add(majority(a, b, c));

        h = g;
        g = f;
        f = e;
        e = d.wrapping_add(temp1);
        d = c;
        c = b;
        b = a;
        a = temp1.wrapping_add(temp2);
    }

    state[0] = state[0].wrapping_add(a);
    state[1] = state[1].wrapping_add(b);
    state[2] = state[2].wrapping_add(c);
    state[3] = state[3].wrapping_add(d);
    state[4] = state[4].wrapping_add(e);
    state[5] = state[5].wrapping_add(f);
    state[6] = state[6].wrapping_add(g);
    state[7] = state[7].wrapping_add(h);
}

struct Sha256 {
    state: [u32; 8],
    buffer: [u8; 64],
    buffer_len: usize,
    message_len: u64,
}

impl Sha256 {
    fn new() -> Self {
        Self {
            state: INITIAL_STATE,
            buffer: [0u8; 64],
            buffer_len: 0,
            message_len: 0,
        }
    }

    fn update(&mut self, mut input: &[u8]) {
        self.message_len = self.message_len.wrapping_add(input.len() as u64);

        if self.buffer_len > 0 {
            let count = input.len().min(64 - self.buffer_len);
            self.buffer[self.buffer_len..self.buffer_len + count].copy_from_slice(&input[..count]);
            self.buffer_len += count;
            input = &input[count..];

            if self.buffer_len < 64 {
                return;
            }

            compress(&mut self.state, &self.buffer);
            self.buffer_len = 0;
        }

        while input.len() >= 64 {
            let mut block = [0u8; 64];
            block.copy_from_slice(&input[..64]);
            compress(&mut self.state, &block);
            input = &input[64..];
        }

        self.buffer[..input.len()].copy_from_slice(input);
        self.buffer_len = input.len();
    }

    fn finish(mut self) -> [u8; 32] {
        let bit_len = self.message_len.wrapping_mul(8).to_be_bytes();
        self.buffer[self.buffer_len] = 0x80;
        self.buffer_len += 1;

        if self.buffer_len > 56 {
            self.buffer[self.buffer_len..].fill(0);
            compress(&mut self.state, &self.buffer);
            self.buffer = [0u8; 64];
        } else {
            self.buffer[self.buffer_len..56].fill(0);
        }

        self.buffer[56..].copy_from_slice(&bit_len);
        compress(&mut self.state, &self.buffer);

        let mut digest = [0u8; 32];
        for (i, word) in self.state.iter().enumerate() {
            digest[i * 4..i * 4 + 4].copy_from_slice(&word.to_be_bytes());
        }
        digest
    }
}

pub(super) fn sha256(input: &[u8]) -> [u8; 32] {
    let mut hasher = Sha256::new();
    hasher.update(input);
    hasher.finish()
}

const EMPTY_SHA256: [u8; 32] = [
    0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14, 0x9a, 0xfb, 0xf4, 0xc8, 0x99, 0x6f, 0xb9, 0x24,
    0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b, 0x93, 0x4c, 0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55,
];

const ABC_SHA256: [u8; 32] = [
    0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
    0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad,
];

const LONG_MESSAGE: &[u8] = b"abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";

const LONG_SHA256: [u8; 32] = [
    0xcf, 0x5b, 0x16, 0xa7, 0x78, 0xaf, 0x83, 0x80, 0x03, 0x6c, 0xe5, 0x9e, 0x7b, 0x04, 0x92, 0x37,
    0x0b, 0x24, 0x9b, 0x11, 0xe8, 0xf0, 0x7a, 0x51, 0xaf, 0xac, 0x45, 0x03, 0x7a, 0xfe, 0xe9, 0xd1,
];

pub(super) fn random_bytes(output: &mut [u8]) -> bool {
    let mut success = true;

    for chunk in output.chunks_mut(4) {
        let mut value = 0u32;
        if unsafe { cpu_rdrand32(&mut value) } == 0 {
            success = false;
            break;
        }

        let bytes = value.to_ne_bytes();
        chunk.copy_from_slice(&bytes[..chunk.len()]);
    }

    if !success {
        output.fill(0);
    }
    success
}

#[no_mangle]
pub unsafe extern "C" fn rust_prp_random(output: *mut u8, len: u32) -> i32 {
    if len == 0 {
        return 0;
    }
    if output.is_null() {
        return -1;
    }

    let output = core::slice::from_raw_parts_mut(output, len as usize);
    if random_bytes(output) {
        0
    } else {
        -1
    }
}

#[no_mangle]
pub unsafe extern "C" fn rust_prp_sha256(data: *const u8, len: u32, output: *mut u8) -> i32 {
    if output.is_null() || (data.is_null() && len != 0) {
        return -1;
    }

    let input = if len == 0 {
        &[]
    } else {
        core::slice::from_raw_parts(data, len as usize)
    };
    let digest = sha256(input);
    core::ptr::copy_nonoverlapping(digest.as_ptr(), output, digest.len());
    0
}

#[no_mangle]
pub unsafe extern "C" fn rust_prp_sha256_file(filename: *const u8, output: *mut u8) -> i32 {
    if filename.is_null() || output.is_null() {
        return -1;
    }

    let size = super::avfs_get_filesize(filename);
    if size < 0 {
        return -1;
    }

    let mut hasher = Sha256::new();
    let mut buffer = [0u8; 512];
    let mut offset = 0u32;
    let size = size as u32;

    while offset < size {
        let count = (size - offset).min(buffer.len() as u32);
        if super::avfs_read_file(filename, buffer.as_mut_ptr(), count, offset) != 0 {
            return -2;
        }
        hasher.update(&buffer[..count as usize]);
        offset += count;
    }

    let digest = hasher.finish();
    core::ptr::copy_nonoverlapping(digest.as_ptr(), output, digest.len());
    0
}

#[no_mangle]
pub unsafe extern "C" fn rust_prp_seal_file(
    input: *const u8,
    output: *const u8,
    key_hex: *const u8,
) -> i32 {
    if input.is_null() || output.is_null() || key_hex.is_null() {
        return -1;
    }

    let input = c_str(input);
    let output = c_str(output);
    let key_hex = c_str(key_hex);
    envelope::seal_file(input, output, key_hex)
}

#[no_mangle]
pub unsafe extern "C" fn rust_prp_open_file(
    input: *const u8,
    output: *const u8,
    key_hex: *const u8,
) -> i32 {
    if input.is_null() || output.is_null() || key_hex.is_null() {
        return -1;
    }

    let input = c_str(input);
    let output = c_str(output);
    let key_hex = c_str(key_hex);
    envelope::open_file(input, output, key_hex)
}

#[no_mangle]
pub unsafe extern "C" fn rust_prp_seal_text(
    text: *const u8,
    len: u32,
    key_hex: *const u8,
    output: *const u8,
) -> i32 {
    if text.is_null() || key_hex.is_null() || output.is_null() {
        return -1;
    }

    let key = match envelope::parse_hex_key(c_str(key_hex)) {
        Some(key) => key,
        None => return -1,
    };
    let text = core::slice::from_raw_parts(text, len as usize);

    let mut nonce = [0u8; 12];
    if !random_bytes(&mut nonce) {
        return -3;
    }

    let sealed_len = envelope::sealed_len(text.len());
    if sealed_len > 1024 {
        return -2;
    }
    let mut sealed = [0u8; 1024];
    if envelope::seal(&key, &nonce, text, &mut sealed[..sealed_len]).is_none() {
        return -2;
    }

    if avfs_create_file(output, sealed_len as u32) != 0 {
        return -4;
    }
    if avfs_write_file(output, sealed.as_ptr(), sealed_len as u32, 0) != 0 {
        return -4;
    }
    0
}

use sha512::Sha512;

const PRPSIG_MAGIC: [u8; 7] = *b"PRPSIG1";
const TRAILER_LEN: usize = 7 + 32 + 64;

#[no_mangle]
pub unsafe extern "C" fn rust_prp_fingerprint(keyfile: *const u8, output: *mut u8) -> i32 {
    if keyfile.is_null() || output.is_null() {
        return -1;
    }
    let filename = c_str(keyfile);

    let size = avfs_get_filesize(filename.as_ptr());
    if size < 0 {
        return -2;
    }

    let mut buf = [0u8; 66];
    let read_len = (size as usize).min(66);
    if avfs_read_file(filename.as_ptr(), buf.as_mut_ptr(), read_len as u32, 0) != 0 {
        return -3;
    }

    let key: [u8; 32] = if read_len >= 39 && (buf[..7] == *b"PRPPUB1" || buf[..7] == *b"PRPPRV1") {
        let mut k = [0u8; 32];
        k.copy_from_slice(&buf[7..39]);
        k
    } else if read_len >= 64 {
        match envelope::parse_hex_key(&buf[..64]) {
            Some(k) => k,
            None => return -1,
        }
    } else {
        return -1;
    };

    let digest = sha256(&key);
    core::ptr::copy_nonoverlapping(digest.as_ptr(), output, digest.len());
    0
}

unsafe fn hash_file_into(hasher: &mut Sha512, filename: &[u8]) -> i32 {
    let size = avfs_get_filesize(filename.as_ptr());
    if size < 0 {
        return -1;
    }
    let mut buffer = [0u8; 512];
    let mut offset = 0u32;
    let size = size as u32;
    while offset < size {
        let count = (size - offset).min(buffer.len() as u32);
        if avfs_read_file(filename.as_ptr(), buffer.as_mut_ptr(), count, offset) != 0 {
            return -2;
        }
        hasher.update(&buffer[..count as usize]);
        offset += count;
    }
    0
}

#[no_mangle]
pub unsafe extern "C" fn rust_prp_sign(file: *const u8, private_key: *const u8) -> i32 {
    if file.is_null() || private_key.is_null() {
        return -1;
    }
    let filename = c_str(file);
    let mut private = [0u8; 32];
    core::ptr::copy_nonoverlapping(private_key, private.as_mut_ptr(), 32);

    let size = avfs_get_filesize(filename.as_ptr());
    if size < 0 {
        return -2;
    }
    if size as usize >= TRAILER_LEN {
        let mut tail = [0u8; TRAILER_LEN];
        if avfs_read_file(
            filename.as_ptr(),
            tail.as_mut_ptr(),
            TRAILER_LEN as u32,
            size as u32 - TRAILER_LEN as u32,
        ) == 0
            && tail[..7] == PRPSIG_MAGIC
        {
            return -5;
        }
    }

    let h = sha512::sha512(&private);
    let mut scalar = [0u8; 32];
    scalar.copy_from_slice(&h[..32]);
    scalar[0] &= 248;
    scalar[31] &= 63;
    scalar[31] |= 64;
    let mut prefix = [0u8; 32];
    prefix.copy_from_slice(&h[32..]);

    let public = ed25519::public_key(&private);

    let mut hasher = Sha512::new();
    hasher.update(&prefix);
    if hash_file_into(&mut hasher, filename) != 0 {
        return -2;
    }
    let r = ed25519::sc_reduce512(&hasher.finish());
    let big_r = ed25519::compress_scalar_mul(&r);

    let mut hasher = Sha512::new();
    hasher.update(&big_r);
    hasher.update(&public);
    if hash_file_into(&mut hasher, filename) != 0 {
        return -2;
    }
    let k = ed25519::sc_reduce512(&hasher.finish());
    let s = ed25519::sc_mul_add(&k, &scalar, &r);

    let mut trailer = [0u8; TRAILER_LEN];
    trailer[..7].copy_from_slice(&PRPSIG_MAGIC);
    trailer[7..39].copy_from_slice(&public);
    trailer[39..71].copy_from_slice(&big_r);
    trailer[71..].copy_from_slice(&s);

    if avfs_append_file(filename.as_ptr(), trailer.as_ptr(), TRAILER_LEN as u32) != 0 {
        return -4;
    }
    0
}

#[no_mangle]
pub unsafe extern "C" fn rust_prp_verify(file: *const u8, expected_pub: *const u8) -> i32 {
    if file.is_null() {
        return -1;
    }
    let filename = c_str(file);

    let size = avfs_get_filesize(filename.as_ptr());
    if size < 0 || (size as usize) < TRAILER_LEN {
        return -6;
    }

    let mut trailer = [0u8; TRAILER_LEN];
    if avfs_read_file(
        filename.as_ptr(),
        trailer.as_mut_ptr(),
        TRAILER_LEN as u32,
        size as u32 - TRAILER_LEN as u32,
    ) != 0
    {
        return -2;
    }
    if trailer[..7] != PRPSIG_MAGIC {
        return -6;
    }

    let mut public = [0u8; 32];
    public.copy_from_slice(&trailer[7..39]);
    if !expected_pub.is_null() {
        let mut want = [0u8; 32];
        core::ptr::copy_nonoverlapping(expected_pub, want.as_mut_ptr(), 32);
        if public != want {
            return -8;
        }
    }

    let mut big_r = [0u8; 32];
    big_r.copy_from_slice(&trailer[39..71]);
    let mut s = [0u8; 32];
    s.copy_from_slice(&trailer[71..]);
    if !ed25519::check_s_below_l(&s) {
        return -7;
    }

    let content_size = size as usize - TRAILER_LEN;
    let mut hasher = Sha512::new();
    hasher.update(&big_r);
    hasher.update(&public);
    let mut buffer = [0u8; 512];
    let mut offset = 0usize;
    while offset < content_size {
        let count = (content_size - offset).min(buffer.len());
        if avfs_read_file(
            filename.as_ptr(),
            buffer.as_mut_ptr(),
            count as u32,
            offset as u32,
        ) != 0
        {
            return -2;
        }
        hasher.update(&buffer[..count]);
        offset += count;
    }
    let k = ed25519::sc_reduce512(&hasher.finish());

    if ed25519::verify_commit(&s, &big_r, &public, &k) {
        0
    } else {
        -7
    }
}

const PRPPRV_MAGIC: [u8; 7] = *b"PRPPRV1";
const PRPPUB_MAGIC: [u8; 7] = *b"PRPPUB1";

#[no_mangle]
pub unsafe extern "C" fn rust_prp_keygen(name: *const u8) -> i32 {
    if name.is_null() {
        return -1;
    }

    let mut private = [0u8; 32];
    if !random_bytes(&mut private) {
        return -3;
    }
    private[0] &= 248;
    private[31] &= 127;
    private[31] |= 64;

    let public = x25519::x25519(&private, &x25519::BASEPOINT);

    let name = c_str(name);
    let mut prv_name = [0u8; 128];
    let mut pub_name = [0u8; 128];
    if name.len() + 4 >= 128 {
        return -1;
    }
    prv_name[..name.len()].copy_from_slice(name);
    prv_name[name.len()..name.len() + 4].copy_from_slice(b".prv");
    pub_name[..name.len()].copy_from_slice(name);
    pub_name[name.len()..name.len() + 4].copy_from_slice(b".pub");

    let mut prv_blob = [0u8; 39];
    prv_blob[..7].copy_from_slice(&PRPPRV_MAGIC);
    prv_blob[7..].copy_from_slice(&private);
    let mut pub_blob = [0u8; 39];
    pub_blob[..7].copy_from_slice(&PRPPUB_MAGIC);
    pub_blob[7..].copy_from_slice(&public);

    for (file, blob) in [
        (&prv_name[..name.len() + 4], &prv_blob),
        (&pub_name[..name.len() + 4], &pub_blob),
    ] {
        if avfs_file_exists(file.as_ptr()) {
            avfs_remove_file(file.as_ptr());
        }
        if avfs_create_file(file.as_ptr(), blob.len() as u32) != 0 {
            return -4;
        }
        if avfs_write_file(file.as_ptr(), blob.as_ptr(), blob.len() as u32, 0) != 0 {
            return -4;
        }
    }
    0
}

#[no_mangle]
pub unsafe extern "C" fn rust_prp_seal_file_pub(
    input: *const u8,
    output: *const u8,
    recipient_pub: *const u8,
) -> i32 {
    if input.is_null() || output.is_null() || recipient_pub.is_null() {
        return -1;
    }

    let input = c_str(input);
    let output = c_str(output);
    let mut pub_key = [0u8; 32];
    core::ptr::copy_nonoverlapping(recipient_pub, pub_key.as_mut_ptr(), 32);
    envelope::seal_file_public(input, output, &pub_key)
}

#[no_mangle]
pub unsafe extern "C" fn rust_prp_open_file_prv(
    input: *const u8,
    output: *const u8,
    private_key: *const u8,
) -> i32 {
    if input.is_null() || output.is_null() || private_key.is_null() {
        return -1;
    }

    let input = c_str(input);
    let output = c_str(output);
    let mut prv_key = [0u8; 32];
    core::ptr::copy_nonoverlapping(private_key, prv_key.as_mut_ptr(), 32);
    envelope::open_file_private(input, output, &prv_key)
}

#[no_mangle]
pub unsafe extern "C" fn rust_prp_seal_text_pub(
    text: *const u8,
    len: u32,
    recipient_pub: *const u8,
    output: *const u8,
) -> i32 {
    if text.is_null() || recipient_pub.is_null() || output.is_null() {
        return -1;
    }

    let text = core::slice::from_raw_parts(text, len as usize);
    let mut pub_key = [0u8; 32];
    core::ptr::copy_nonoverlapping(recipient_pub, pub_key.as_mut_ptr(), 32);

    const PREFIX: usize = 20 + 32 + 12 + 48;
    if PREFIX + 16 + text.len() > 1024 {
        return -2;
    }
    let mut sealed = [0u8; 1024];
    if envelope::seal_public(&pub_key, text, &mut sealed[..PREFIX + 16 + text.len()]).is_none() {
        return -3;
    }

    if avfs_create_file(output, (PREFIX + 16 + text.len()) as u32) != 0 {
        return -4;
    }
    if avfs_write_file(
        output,
        sealed.as_ptr(),
        (PREFIX + 16 + text.len()) as u32,
        0,
    ) != 0
    {
        return -4;
    }
    0
}

unsafe fn c_str(mut ptr: *const u8) -> &'static [u8] {
    let mut len = 0usize;
    while *ptr.add(len) != 0 {
        len += 1;
    }
    core::slice::from_raw_parts(ptr, len)
}

#[no_mangle]
pub extern "C" fn rust_prp_selftest() -> i32 {
    if sha256(b"") != EMPTY_SHA256 {
        return -1;
    }

    if sha256(b"abc") != ABC_SHA256 {
        return -1;
    }

    if sha256(LONG_MESSAGE) != LONG_SHA256 {
        return -1;
    }

    let mut chunked = Sha256::new();
    chunked.update(&LONG_MESSAGE[..7]);
    chunked.update(&LONG_MESSAGE[7..38]);
    chunked.update(&LONG_MESSAGE[38..]);
    if chunked.finish() != LONG_SHA256 {
        return -1;
    }

    if !chacha20::selftest() {
        return -1;
    }

    if !poly1305::selftest() {
        return -1;
    }

    if !aead::selftest() {
        return -1;
    }

    if !envelope::selftest() {
        return -1;
    }

    if !x25519::selftest() {
        return -1;
    }

    if !sha512::selftest() {
        return -1;
    }

    if !ed25519::selftest() {
        return -1;
    }

    0
}
