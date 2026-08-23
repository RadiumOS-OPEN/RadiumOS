use super::{chacha20, poly1305::Poly1305};

pub(super) fn poly1305_key(key: &[u8; 32], nonce: &[u8; 12]) -> [u8; 32] {
    let block = chacha20::block(key, 0, nonce);
    let mut poly_key = [0u8; 32];
    poly_key.copy_from_slice(&block[..32]);
    poly_key
}

fn pad(poly: &mut Poly1305, len: usize) {
    let remainder = len % 16;
    if remainder != 0 {
        poly.update(&[0u8; 16][..16 - remainder]);
    }
}

fn authenticate(key: &[u8; 32], nonce: &[u8; 12], aad: &[u8], ciphertext: &[u8]) -> [u8; 16] {
    let poly_key = poly1305_key(key, nonce);
    let mut poly = Poly1305::new(&poly_key);

    poly.update(aad);
    pad(&mut poly, aad.len());
    poly.update(ciphertext);
    pad(&mut poly, ciphertext.len());
    poly.update(&(aad.len() as u64).to_le_bytes());
    poly.update(&(ciphertext.len() as u64).to_le_bytes());
    poly.finish()
}

fn stream_fits(len: usize) -> bool {
    let blocks = len / 64 + (len % 64 != 0) as usize;
    blocks <= u32::MAX as usize
}

pub(super) fn tags_equal(left: &[u8; 16], right: &[u8; 16]) -> bool {
    let mut difference = 0u8;
    for i in 0..16 {
        difference |= left[i] ^ right[i];
    }
    difference == 0
}

pub(super) fn encrypt(
    key: &[u8; 32],
    nonce: &[u8; 12],
    aad: &[u8],
    data: &mut [u8],
) -> Option<[u8; 16]> {
    if !stream_fits(data.len()) || !chacha20::apply(key, nonce, 1, data) {
        return None;
    }

    Some(authenticate(key, nonce, aad, data))
}

pub(super) fn decrypt(
    key: &[u8; 32],
    nonce: &[u8; 12],
    aad: &[u8],
    data: &mut [u8],
    tag: &[u8; 16],
) -> bool {
    if !stream_fits(data.len()) {
        return false;
    }

    let expected = authenticate(key, nonce, aad, data);
    if !tags_equal(&expected, tag) {
        return false;
    }

    chacha20::apply(key, nonce, 1, data)
}

const PLAINTEXT: &[u8; 114] = b"Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.";

const CIPHERTEXT: [u8; 114] = [
    0xd3, 0x1a, 0x8d, 0x34, 0x64, 0x8e, 0x60, 0xdb, 0x7b, 0x86, 0xaf, 0xbc, 0x53, 0xef, 0x7e, 0xc2,
    0xa4, 0xad, 0xed, 0x51, 0x29, 0x6e, 0x08, 0xfe, 0xa9, 0xe2, 0xb5, 0xa7, 0x36, 0xee, 0x62, 0xd6,
    0x3d, 0xbe, 0xa4, 0x5e, 0x8c, 0xa9, 0x67, 0x12, 0x82, 0xfa, 0xfb, 0x69, 0xda, 0x92, 0x72, 0x8b,
    0x1a, 0x71, 0xde, 0x0a, 0x9e, 0x06, 0x0b, 0x29, 0x05, 0xd6, 0xa5, 0xb6, 0x7e, 0xcd, 0x3b, 0x36,
    0x92, 0xdd, 0xbd, 0x7f, 0x2d, 0x77, 0x8b, 0x8c, 0x98, 0x03, 0xae, 0xe3, 0x28, 0x09, 0x1b, 0x58,
    0xfa, 0xb3, 0x24, 0xe4, 0xfa, 0xd6, 0x75, 0x94, 0x55, 0x85, 0x80, 0x8b, 0x48, 0x31, 0xd7, 0xbc,
    0x3f, 0xf4, 0xde, 0xf0, 0x8e, 0x4b, 0x7a, 0x9d, 0xe5, 0x76, 0xd2, 0x65, 0x86, 0xce, 0xc6, 0x4b,
    0x61, 0x16,
];

const TAG: [u8; 16] = [
    0x1a, 0xe1, 0x0b, 0x59, 0x4f, 0x09, 0xe2, 0x6a, 0x7e, 0x90, 0x2e, 0xcb, 0xd0, 0x60, 0x06, 0x91,
];

pub(super) fn selftest() -> bool {
    let mut key = [0u8; 32];
    for (i, byte) in key.iter_mut().enumerate() {
        *byte = 0x80 + i as u8;
    }
    let nonce = [
        0x07, 0x00, 0x00, 0x00, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47,
    ];
    let aad = [
        0x50, 0x51, 0x52, 0x53, 0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7,
    ];

    let mut data = *PLAINTEXT;
    let tag = match encrypt(&key, &nonce, &aad, &mut data) {
        Some(tag) => tag,
        None => return false,
    };
    if data != CIPHERTEXT || tag != TAG {
        return false;
    }

    let ciphertext = data;
    if !decrypt(&key, &nonce, &aad, &mut data, &tag) || data != *PLAINTEXT {
        return false;
    }

    let mut tampered = ciphertext;
    tampered[0] ^= 1;
    let unchanged = tampered;
    !decrypt(&key, &nonce, &aad, &mut tampered, &tag) && tampered == unchanged
}
