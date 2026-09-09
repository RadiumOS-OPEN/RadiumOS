use super::{sha256, Sha256};

pub(crate) fn hmac_parts(key: &[u8], parts: &[&[u8]]) -> [u8; 32] {
    let mut block = [0u8; 64];
    if key.len() > block.len() {
        block[..32].copy_from_slice(&sha256(key));
    } else {
        block[..key.len()].copy_from_slice(key);
    }
    for byte in &mut block {
        *byte ^= 0x36;
    }
    let mut inner = Sha256::new();
    inner.update(&block);
    for part in parts {
        inner.update(part);
    }
    for byte in &mut block {
        *byte ^= 0x36 ^ 0x5c;
    }
    let mut outer = Sha256::new();
    outer.update(&block);
    outer.update(&inner.finish());
    outer.finish()
}

pub(crate) fn hmac_sha256(key: &[u8], data: &[u8]) -> [u8; 32] {
    hmac_parts(key, &[data])
}

pub(crate) fn hkdf_extract(salt: &[u8], ikm: &[u8]) -> [u8; 32] {
    hmac_sha256(salt, ikm)
}

pub(crate) fn hkdf_expand(prk: &[u8; 32], info: &[u8], output: &mut [u8]) -> bool {
    if output.len() > 255 * 32 {
        return false;
    }
    let mut previous = [0u8; 32];
    let mut previous_len = 0;
    for (i, chunk) in output.chunks_mut(32).enumerate() {
        previous = hmac_parts(prk, &[&previous[..previous_len], info, &[(i + 1) as u8]]);
        chunk.copy_from_slice(&previous[..chunk.len()]);
        previous_len = 32;
    }
    true
}

pub(crate) fn hkdf_expand_label(
    secret: &[u8; 32],
    label: &[u8],
    context: &[u8],
    output: &mut [u8],
) -> bool {
    if label.is_empty() || label.len() > 249 || context.len() > 255 || output.len() > 255 * 32 {
        return false;
    }
    // RFC 8446 HkdfLabel includes the protocol prefix in its one-byte label length.
    let mut info = [0u8; 514];
    info[..2].copy_from_slice(&(output.len() as u16).to_be_bytes());
    info[2] = (6 + label.len()) as u8;
    info[3..9].copy_from_slice(b"tls13 ");
    info[9..9 + label.len()].copy_from_slice(label);
    info[9 + label.len()] = context.len() as u8;
    let len = 10 + label.len() + context.len();
    info[10 + label.len()..len].copy_from_slice(context);
    hkdf_expand(secret, &info[..len], output)
}

pub(super) fn selftest() -> bool {
    // RFC 4231 case 6 and RFC 5869 cases 1 and 3.
    if hmac_sha256(
        &[0xaa; 131],
        b"Test Using Larger Than Block-Size Key - Hash Key First",
    ) != [
        0x60, 0xe4, 0x31, 0x59, 0x1e, 0xe0, 0xb6, 0x7f, 0x0d, 0x8a, 0x26, 0xaa, 0xcb, 0xf5, 0xb7,
        0x7f, 0x8e, 0x0b, 0xc6, 0x21, 0x37, 0x28, 0xc5, 0x14, 0x05, 0x46, 0x04, 0x0f, 0x0e, 0xe3,
        0x7f, 0x54,
    ] {
        return false;
    }
    let prk = hkdf_extract(&[0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12], &[0x0b; 22]);
    if prk
        != [
            0x07, 0x77, 0x09, 0x36, 0x2c, 0x2e, 0x32, 0xdf, 0x0d, 0xdc, 0x3f, 0x0d, 0xc4, 0x7b,
            0xba, 0x63, 0x90, 0xb6, 0xc7, 0x3b, 0xb5, 0x0f, 0x9c, 0x31, 0x22, 0xec, 0x84, 0x4a,
            0xd7, 0xc2, 0xb3, 0xe5,
        ]
    {
        return false;
    }
    let mut output = [0u8; 42];
    if !hkdf_expand(
        &prk,
        &[0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9],
        &mut output,
    ) || output
        != [
            0x3c, 0xb2, 0x5f, 0x25, 0xfa, 0xac, 0xd5, 0x7a, 0x90, 0x43, 0x4f, 0x64, 0xd0, 0x36,
            0x2f, 0x2a, 0x2d, 0x2d, 0x0a, 0x90, 0xcf, 0x1a, 0x5a, 0x4c, 0x5d, 0xb0, 0x2d, 0x56,
            0xec, 0xc4, 0xc5, 0xbf, 0x34, 0x00, 0x72, 0x08, 0xd5, 0xb8, 0x87, 0x18, 0x58, 0x65,
        ]
    {
        return false;
    }
    let empty_salt = hkdf_extract(b"", &[0x0b; 22]);
    if empty_salt
        != [
            0x19, 0xef, 0x24, 0xa3, 0x2c, 0x71, 0x7b, 0x16, 0x7f, 0x33, 0xa9, 0x1d, 0x6f, 0x64,
            0x8b, 0xdf, 0x96, 0x59, 0x67, 0x76, 0xaf, 0xdb, 0x63, 0x77, 0xac, 0x43, 0x4c, 0x1c,
            0x29, 0x3c, 0xcb, 0x04,
        ]
        || !hkdf_expand(&empty_salt, b"", &mut output)
        || output
            != [
                0x8d, 0xa4, 0xe7, 0x75, 0xa5, 0x63, 0xc1, 0x8f, 0x71, 0x5f, 0x80, 0x2a, 0x06, 0x3c,
                0x5a, 0x31, 0xb8, 0xa1, 0x1f, 0x5c, 0x5e, 0xe1, 0x87, 0x9e, 0xc3, 0x45, 0x4e, 0x5f,
                0x3c, 0x73, 0x8d, 0x2d, 0x9d, 0x20, 0x13, 0x95, 0xfa, 0xa4, 0xb6, 0x1a, 0x96, 0xc8,
            ]
    {
        return false;
    }
    let mut oversized = [0xa5u8; 8161];
    if hkdf_expand(&prk, b"", &mut oversized) || oversized != [0xa5; 8161] {
        return false;
    }
    if hkdf_expand_label(&prk, &[0; 250], b"", &mut output)
        || hkdf_expand_label(&prk, b"key", &[0; 256], &mut output)
        || hkdf_expand_label(&prk, b"", b"", &mut output)
    {
        return false;
    }
    // Independently generated with Python's hmac and hashlib.
    let mut key = [0u8; 32];
    if !hkdf_expand_label(&[0; 32], b"key", b"", &mut key)
        || key
            != [
                0x0a, 0xc2, 0x08, 0x43, 0x3b, 0x00, 0x7c, 0xc5, 0x37, 0x60, 0xc8, 0xc2, 0xeb, 0xa0,
                0x9c, 0x42, 0xf1, 0xea, 0xb3, 0x39, 0x0d, 0xd0, 0x10, 0xa8, 0x94, 0x55, 0x4e, 0xe5,
                0x94, 0xf0, 0xd7, 0xc6,
            ]
    {
        return false;
    }
    hkdf_expand(&prk, b"", &mut [])
}
