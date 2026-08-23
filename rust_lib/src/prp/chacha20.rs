fn quarter_round(state: &mut [u32; 16], a: usize, b: usize, c: usize, d: usize) {
    state[a] = state[a].wrapping_add(state[b]);
    state[d] ^= state[a];
    state[d] = state[d].rotate_left(16);

    state[c] = state[c].wrapping_add(state[d]);
    state[b] ^= state[c];
    state[b] = state[b].rotate_left(12);

    state[a] = state[a].wrapping_add(state[b]);
    state[d] ^= state[a];
    state[d] = state[d].rotate_left(8);

    state[c] = state[c].wrapping_add(state[d]);
    state[b] ^= state[c];
    state[b] = state[b].rotate_left(7);
}

pub(super) fn block(key: &[u8; 32], counter: u32, nonce: &[u8; 12]) -> [u8; 64] {
    let mut state = [
        0x61707865, 0x3320646e, 0x79622d32, 0x6b206574, 0, 0, 0, 0, 0, 0, 0, 0, counter, 0, 0, 0,
    ];

    for i in 0..8 {
        let offset = i * 4;
        state[4 + i] = u32::from_le_bytes([
            key[offset],
            key[offset + 1],
            key[offset + 2],
            key[offset + 3],
        ]);
    }

    for i in 0..3 {
        let offset = i * 4;
        state[13 + i] = u32::from_le_bytes([
            nonce[offset],
            nonce[offset + 1],
            nonce[offset + 2],
            nonce[offset + 3],
        ]);
    }

    let initial = state;
    for _ in 0..10 {
        quarter_round(&mut state, 0, 4, 8, 12);
        quarter_round(&mut state, 1, 5, 9, 13);
        quarter_round(&mut state, 2, 6, 10, 14);
        quarter_round(&mut state, 3, 7, 11, 15);
        quarter_round(&mut state, 0, 5, 10, 15);
        quarter_round(&mut state, 1, 6, 11, 12);
        quarter_round(&mut state, 2, 7, 8, 13);
        quarter_round(&mut state, 3, 4, 9, 14);
    }

    let mut output = [0u8; 64];
    for i in 0..16 {
        let word = state[i].wrapping_add(initial[i]).to_le_bytes();
        output[i * 4..i * 4 + 4].copy_from_slice(&word);
    }
    output
}

pub(super) fn apply(key: &[u8; 32], nonce: &[u8; 12], mut counter: u32, data: &mut [u8]) -> bool {
    let mut offset = 0;

    while offset < data.len() {
        let stream = block(key, counter, nonce);
        let count = (data.len() - offset).min(64);
        for i in 0..count {
            data[offset + i] ^= stream[i];
        }
        offset += count;

        if offset < data.len() {
            match counter.checked_add(1) {
                Some(next) => counter = next,
                None => return false,
            }
        }
    }

    true
}

const BLOCK_EXPECTED: [u8; 64] = [
    0x10, 0xf1, 0xe7, 0xe4, 0xd1, 0x3b, 0x59, 0x15, 0x50, 0x0f, 0xdd, 0x1f, 0xa3, 0x20, 0x71, 0xc4,
    0xc7, 0xd1, 0xf4, 0xc7, 0x33, 0xc0, 0x68, 0x03, 0x04, 0x22, 0xaa, 0x9a, 0xc3, 0xd4, 0x6c, 0x4e,
    0xd2, 0x82, 0x64, 0x46, 0x07, 0x9f, 0xaa, 0x09, 0x14, 0xc2, 0xd7, 0x05, 0xd9, 0x8b, 0x02, 0xa2,
    0xb5, 0x12, 0x9c, 0xd1, 0xde, 0x16, 0x4e, 0xb9, 0xcb, 0xd0, 0x83, 0xe8, 0xa2, 0x50, 0x3c, 0x4e,
];

const STREAM_PLAINTEXT: &[u8; 114] = b"Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.";

const STREAM_EXPECTED: [u8; 114] = [
    0x6e, 0x2e, 0x35, 0x9a, 0x25, 0x68, 0xf9, 0x80, 0x41, 0xba, 0x07, 0x28, 0xdd, 0x0d, 0x69, 0x81,
    0xe9, 0x7e, 0x7a, 0xec, 0x1d, 0x43, 0x60, 0xc2, 0x0a, 0x27, 0xaf, 0xcc, 0xfd, 0x9f, 0xae, 0x0b,
    0xf9, 0x1b, 0x65, 0xc5, 0x52, 0x47, 0x33, 0xab, 0x8f, 0x59, 0x3d, 0xab, 0xcd, 0x62, 0xb3, 0x57,
    0x16, 0x39, 0xd6, 0x24, 0xe6, 0x51, 0x52, 0xab, 0x8f, 0x53, 0x0c, 0x35, 0x9f, 0x08, 0x61, 0xd8,
    0x07, 0xca, 0x0d, 0xbf, 0x50, 0x0d, 0x6a, 0x61, 0x56, 0xa3, 0x8e, 0x08, 0x8a, 0x22, 0xb6, 0x5e,
    0x52, 0xbc, 0x51, 0x4d, 0x16, 0xcc, 0xf8, 0x06, 0x81, 0x8c, 0xe9, 0x1a, 0xb7, 0x79, 0x37, 0x36,
    0x5a, 0xf9, 0x0b, 0xbf, 0x74, 0xa3, 0x5b, 0xe6, 0xb4, 0x0b, 0x8e, 0xed, 0xf2, 0x78, 0x5e, 0x42,
    0x87, 0x4d,
];

pub(super) fn selftest() -> bool {
    let mut state = [0u32; 16];
    state[0] = 0x11111111;
    state[1] = 0x01020304;
    state[2] = 0x9b8d6f43;
    state[3] = 0x01234567;

    quarter_round(&mut state, 0, 1, 2, 3);

    if state[0] != 0xea2a92f4
        || state[1] != 0xcb1cf8ce
        || state[2] != 0x4581472e
        || state[3] != 0x5881c4bb
    {
        return false;
    }

    let mut key = [0u8; 32];
    for (i, byte) in key.iter_mut().enumerate() {
        *byte = i as u8;
    }
    let nonce = [
        0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x4a, 0x00, 0x00, 0x00, 0x00,
    ];

    if block(&key, 1, &nonce) != BLOCK_EXPECTED {
        return false;
    }

    let nonce = [
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4a, 0x00, 0x00, 0x00, 0x00,
    ];
    let mut data = *STREAM_PLAINTEXT;
    if !apply(&key, &nonce, 1, &mut data) || data != STREAM_EXPECTED {
        return false;
    }

    apply(&key, &nonce, 1, &mut data) && data == *STREAM_PLAINTEXT
}
