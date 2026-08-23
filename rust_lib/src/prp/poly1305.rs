const MASK: u64 = (1 << 26) - 1;

fn load32(input: &[u8], offset: usize) -> u64 {
    u32::from_le_bytes([
        input[offset],
        input[offset + 1],
        input[offset + 2],
        input[offset + 3],
    ]) as u64
}

pub(super) struct Poly1305 {
    r: [u64; 5],
    h: [u64; 5],
    pad: [u32; 4],
    buffer: [u8; 16],
    buffer_len: usize,
}

impl Poly1305 {
    pub(super) fn new(key: &[u8; 32]) -> Self {
        let t0 = load32(key, 0);
        let t1 = load32(key, 4);
        let t2 = load32(key, 8);
        let t3 = load32(key, 12);

        Self {
            r: [
                t0 & 0x3ffffff,
                ((t0 >> 26) | (t1 << 6)) & 0x3ffff03,
                ((t1 >> 20) | (t2 << 12)) & 0x3ffc0ff,
                ((t2 >> 14) | (t3 << 18)) & 0x3f03fff,
                (t3 >> 8) & 0x00fffff,
            ],
            h: [0; 5],
            pad: [
                load32(key, 16) as u32,
                load32(key, 20) as u32,
                load32(key, 24) as u32,
                load32(key, 28) as u32,
            ],
            buffer: [0; 16],
            buffer_len: 0,
        }
    }

    fn process_block(&mut self, block: &[u8; 16], full: bool) {
        let t0 = load32(block, 0);
        let t1 = load32(block, 4);
        let t2 = load32(block, 8);
        let t3 = load32(block, 12);

        self.h[0] += t0 & MASK;
        self.h[1] += ((t0 >> 26) | (t1 << 6)) & MASK;
        self.h[2] += ((t1 >> 20) | (t2 << 12)) & MASK;
        self.h[3] += ((t2 >> 14) | (t3 << 18)) & MASK;
        self.h[4] += (t3 >> 8) | if full { 1 << 24 } else { 0 };

        let r = self.r;
        let s1 = r[1] * 5;
        let s2 = r[2] * 5;
        let s3 = r[3] * 5;
        let s4 = r[4] * 5;

        let mut d0 =
            self.h[0] * r[0] + self.h[1] * s4 + self.h[2] * s3 + self.h[3] * s2 + self.h[4] * s1;
        let mut d1 =
            self.h[0] * r[1] + self.h[1] * r[0] + self.h[2] * s4 + self.h[3] * s3 + self.h[4] * s2;
        let mut d2 = self.h[0] * r[2]
            + self.h[1] * r[1]
            + self.h[2] * r[0]
            + self.h[3] * s4
            + self.h[4] * s3;
        let mut d3 = self.h[0] * r[3]
            + self.h[1] * r[2]
            + self.h[2] * r[1]
            + self.h[3] * r[0]
            + self.h[4] * s4;
        let mut d4 = self.h[0] * r[4]
            + self.h[1] * r[3]
            + self.h[2] * r[2]
            + self.h[3] * r[1]
            + self.h[4] * r[0];

        let mut carry = d0 >> 26;
        self.h[0] = d0 & MASK;
        d1 += carry;
        carry = d1 >> 26;
        self.h[1] = d1 & MASK;
        d2 += carry;
        carry = d2 >> 26;
        self.h[2] = d2 & MASK;
        d3 += carry;
        carry = d3 >> 26;
        self.h[3] = d3 & MASK;
        d4 += carry;
        carry = d4 >> 26;
        self.h[4] = d4 & MASK;
        self.h[0] += carry * 5;
        carry = self.h[0] >> 26;
        self.h[0] &= MASK;
        self.h[1] += carry;
    }

    pub(super) fn update(&mut self, mut input: &[u8]) {
        if self.buffer_len > 0 {
            let count = input.len().min(16 - self.buffer_len);
            self.buffer[self.buffer_len..self.buffer_len + count].copy_from_slice(&input[..count]);
            self.buffer_len += count;
            input = &input[count..];

            if self.buffer_len < 16 {
                return;
            }

            let block = self.buffer;
            self.process_block(&block, true);
            self.buffer_len = 0;
        }

        while input.len() >= 16 {
            let mut block = [0u8; 16];
            block.copy_from_slice(&input[..16]);
            self.process_block(&block, true);
            input = &input[16..];
        }

        self.buffer[..input.len()].copy_from_slice(input);
        self.buffer_len = input.len();
    }

    pub(super) fn finish(mut self) -> [u8; 16] {
        if self.buffer_len > 0 {
            let mut block = [0u8; 16];
            block[..self.buffer_len].copy_from_slice(&self.buffer[..self.buffer_len]);
            block[self.buffer_len] = 1;
            self.process_block(&block, false);
        }

        let mut carry = self.h[1] >> 26;
        self.h[1] &= MASK;
        self.h[2] += carry;
        carry = self.h[2] >> 26;
        self.h[2] &= MASK;
        self.h[3] += carry;
        carry = self.h[3] >> 26;
        self.h[3] &= MASK;
        self.h[4] += carry;
        carry = self.h[4] >> 26;
        self.h[4] &= MASK;
        self.h[0] += carry * 5;
        carry = self.h[0] >> 26;
        self.h[0] &= MASK;
        self.h[1] += carry;

        let mut g = [0u64; 5];
        g[0] = self.h[0] + 5;
        carry = g[0] >> 26;
        g[0] &= MASK;
        for i in 1..4 {
            g[i] = self.h[i] + carry;
            carry = g[i] >> 26;
            g[i] &= MASK;
        }
        g[4] = self.h[4].wrapping_add(carry).wrapping_sub(1 << 26);

        let g_mask = (g[4] >> 63).wrapping_sub(1);
        let h_mask = !g_mask;
        for (h, g) in self.h.iter_mut().zip(g.iter()) {
            *h = (*h & h_mask) | (*g & g_mask);
        }

        let mut words = [
            (self.h[0] | (self.h[1] << 26)) & 0xffffffff,
            ((self.h[1] >> 6) | (self.h[2] << 20)) & 0xffffffff,
            ((self.h[2] >> 12) | (self.h[3] << 14)) & 0xffffffff,
            ((self.h[3] >> 18) | (self.h[4] << 8)) & 0xffffffff,
        ];

        let mut carry = 0u64;
        for (word, pad) in words.iter_mut().zip(self.pad.iter()) {
            *word += *pad as u64 + carry;
            carry = *word >> 32;
            *word &= 0xffffffff;
        }

        let mut tag = [0u8; 16];
        for (i, word) in words.iter().enumerate() {
            tag[i * 4..i * 4 + 4].copy_from_slice(&(*word as u32).to_le_bytes());
        }
        tag
    }
}

pub(super) fn authenticate(message: &[u8], key: &[u8; 32]) -> [u8; 16] {
    let mut poly = Poly1305::new(key);
    poly.update(message);
    poly.finish()
}

pub(super) fn selftest() -> bool {
    let key = [
        0x85, 0xd6, 0xbe, 0x78, 0x57, 0x55, 0x6d, 0x33, 0x7f, 0x44, 0x52, 0xfe, 0x42, 0xd5, 0x06,
        0xa8, 0x01, 0x03, 0x80, 0x8a, 0xfb, 0x0d, 0xb2, 0xfd, 0x4a, 0xbf, 0xf6, 0xaf, 0x41, 0x49,
        0xf5, 0x1b,
    ];
    let expected = [
        0xa8, 0x06, 0x1d, 0xc1, 0x30, 0x51, 0x36, 0xc6, 0xc2, 0x2b, 0x8b, 0xaf, 0x0c, 0x01, 0x27,
        0xa9,
    ];

    authenticate(b"Cryptographic Forum Research Group", &key) == expected
}
