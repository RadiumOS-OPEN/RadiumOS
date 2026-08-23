const MASK51: u64 = (1 << 51) - 1;

pub(super) type Fe = [u64; 5];

pub(super) const BASEPOINT: [u8; 32] = [
    9, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
];

fn load64(bytes: &[u8]) -> u64 {
    u64::from_le_bytes([
        bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
    ])
}

pub(super) fn fe_from_bytes(s: &[u8; 32]) -> Fe {
    [
        load64(&s[0..8]) & MASK51,
        (load64(&s[6..14]) >> 3) & MASK51,
        (load64(&s[12..20]) >> 6) & MASK51,
        (load64(&s[19..27]) >> 1) & MASK51,
        (load64(&s[24..32]) >> 12) & MASK51,
    ]
}

pub(super) fn fe_to_bytes(f: &Fe) -> [u8; 32] {
    let mut t = *f;

    t[1] += t[0] >> 51;
    t[0] &= MASK51;
    t[2] += t[1] >> 51;
    t[1] &= MASK51;
    t[3] += t[2] >> 51;
    t[2] &= MASK51;
    t[4] += t[3] >> 51;
    t[3] &= MASK51;
    t[0] += (t[4] >> 51) * 19;
    t[4] &= MASK51;

    let mut q = (t[0] + 19) >> 51;
    q = (t[1] + q) >> 51;
    q = (t[2] + q) >> 51;
    q = (t[3] + q) >> 51;
    q = (t[4] + q) >> 51;

    t[0] += 19 * q;
    t[1] += t[0] >> 51;
    t[0] &= MASK51;
    t[2] += t[1] >> 51;
    t[1] &= MASK51;
    t[3] += t[2] >> 51;
    t[2] &= MASK51;
    t[4] += t[3] >> 51;
    t[3] &= MASK51;
    t[4] &= MASK51;

    let mut out = [0u8; 32];
    out[0..8].copy_from_slice(&(t[0] | (t[1] << 51)).to_le_bytes());
    out[8..16].copy_from_slice(&((t[1] >> 13) | (t[2] << 38)).to_le_bytes());
    out[16..24].copy_from_slice(&((t[2] >> 26) | (t[3] << 25)).to_le_bytes());
    out[24..32].copy_from_slice(&((t[3] >> 39) | (t[4] << 12)).to_le_bytes());
    out
}

fn fe_carry_norm(mut r: Fe) -> Fe {
    r[1] += r[0] >> 51;
    r[0] &= MASK51;
    r[2] += r[1] >> 51;
    r[1] &= MASK51;
    r[3] += r[2] >> 51;
    r[2] &= MASK51;
    r[4] += r[3] >> 51;
    r[3] &= MASK51;
    let top = r[4] >> 51;
    r[4] &= MASK51;
    r[0] += top * 19;
    let carry = r[0] >> 51;
    r[0] &= MASK51;
    r[1] += carry;
    r
}

pub(super) fn fe_add(a: &Fe, b: &Fe) -> Fe {
    let mut r = [0u64; 5];
    for i in 0..5 {
        r[i] = a[i] + b[i];
    }
    fe_carry_norm(r)
}

const TWO_P: Fe = [
    0xFFFFFFFFFFFDA,
    0xFFFFFFFFFFFFE,
    0xFFFFFFFFFFFFE,
    0xFFFFFFFFFFFFE,
    0xFFFFFFFFFFFFE,
];

pub(super) fn fe_sub(a: &Fe, b: &Fe) -> Fe {
    let mut r = [0u64; 5];
    for i in 0..5 {
        r[i] = a[i] + TWO_P[i] - b[i];
    }
    fe_carry_norm(r)
}

fn fe_carry(r: &mut [u128; 5]) -> Fe {
    let mut out = [0u64; 5];

    let mut carry = r[0] >> 51;
    out[0] = (r[0] & MASK51 as u128) as u64;
    r[1] += carry;
    carry = r[1] >> 51;
    out[1] = (r[1] & MASK51 as u128) as u64;
    r[2] += carry;
    carry = r[2] >> 51;
    out[2] = (r[2] & MASK51 as u128) as u64;
    r[3] += carry;
    carry = r[3] >> 51;
    out[3] = (r[3] & MASK51 as u128) as u64;
    r[4] += carry;
    let top = (r[4] >> 51) as u64;
    out[4] = (r[4] & MASK51 as u128) as u64;

    out[0] += top * 19;
    let carry = out[0] >> 51;
    out[0] &= MASK51;
    out[1] += carry;
    out
}

pub(super) fn fe_mul(a: &Fe, b: &Fe) -> Fe {
    let mut r = [0u128; 5];

    for j in 0..5 {
        for k in 0..5 {
            let product = (a[j] as u128) * (b[k] as u128);
            if j + k < 5 {
                r[j + k] += product;
            } else {
                r[j + k - 5] += 19 * product;
            }
        }
    }

    fe_carry(&mut r)
}

pub(super) fn fe_sq(a: &Fe) -> Fe {
    fe_mul(a, a)
}

pub(super) fn fe_mul_small(a: &Fe, c: u64) -> Fe {
    let mut r = [0u128; 5];
    for i in 0..5 {
        r[i] = (a[i] as u128) * (c as u128);
    }
    fe_carry(&mut r)
}

pub(super) fn fe_one() -> Fe {
    [1, 0, 0, 0, 0]
}

pub(super) fn fe_zero() -> Fe {
    [0; 5]
}

pub(super) fn fe_cswap(swap: u8, a: &mut Fe, b: &mut Fe) {
    let mask = 0u64.wrapping_sub(swap as u64);
    for i in 0..5 {
        let dummy = mask & (a[i] ^ b[i]);
        a[i] ^= dummy;
        b[i] ^= dummy;
    }
}

pub(super) fn fe_pow(e: &[u8; 32], base: &Fe) -> Fe {
    let mut acc = fe_one();

    for i in (0..255).rev() {
        acc = fe_sq(&acc);
        let bit = (e[i / 8] >> (i % 8)) & 1;
        let mask = 0u64.wrapping_sub(bit as u64);
        let product = fe_mul(&acc, base);
        for j in 0..5 {
            acc[j] = (acc[j] & !mask) | (product[j] & mask);
        }
    }
    acc
}

fn fe_invert(a: &Fe) -> Fe {
    let mut e = [0xFFu8; 32];
    e[0] = 0xEB;
    e[31] = 0x7F;
    fe_pow(&e, a)
}

pub(super) fn x25519(scalar: &[u8; 32], u: &[u8; 32]) -> [u8; 32] {
    let mut k = *scalar;
    k[0] &= 248;
    k[31] &= 127;
    k[31] |= 64;

    let mut u_clamped = *u;
    u_clamped[31] &= 127;
    let x1 = fe_from_bytes(&u_clamped);

    let mut x2 = fe_one();
    let mut z2 = fe_zero();
    let mut x3 = x1;
    let mut z3 = fe_one();
    let mut swap = 0u8;

    for t in (0..255).rev() {
        let kt = (k[t / 8] >> (t % 8)) & 1;
        swap ^= kt;
        fe_cswap(swap, &mut x2, &mut x3);
        fe_cswap(swap, &mut z2, &mut z3);
        swap = kt;

        let a = fe_add(&x2, &z2);
        let aa = fe_sq(&a);
        let b = fe_sub(&x2, &z2);
        let bb = fe_sq(&b);
        let e = fe_sub(&aa, &bb);
        let c = fe_add(&x3, &z3);
        let d = fe_sub(&x3, &z3);
        let da = fe_mul(&d, &a);
        let cb = fe_mul(&c, &b);

        x3 = fe_sq(&fe_add(&da, &cb));
        z3 = fe_mul(&x1, &fe_sq(&fe_sub(&da, &cb)));
        x2 = fe_mul(&aa, &bb);
        z2 = fe_mul(&e, &fe_add(&aa, &fe_mul_small(&e, 121665)));
    }

    fe_cswap(swap, &mut x2, &mut x3);
    fe_cswap(swap, &mut z2, &mut z3);

    fe_to_bytes(&fe_mul(&x2, &fe_invert(&z2)))
}

fn hex_decode(hex: &str) -> [u8; 32] {
    let mut out = [0u8; 32];
    let bytes = hex.as_bytes();
    for i in 0..32 {
        let hi = (bytes[i * 2] as char).to_digit(16).unwrap() as u8;
        let lo = (bytes[i * 2 + 1] as char).to_digit(16).unwrap() as u8;
        out[i] = hi << 4 | lo;
    }
    out
}

pub(super) fn selftest() -> bool {
    let out = x25519(
        &hex_decode("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4"),
        &hex_decode("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c"),
    );
    if out != hex_decode("c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552") {
        return false;
    }

    let out = x25519(
        &hex_decode("4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d"),
        &hex_decode("e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493"),
    );
    if out != hex_decode("95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957") {
        return false;
    }

    let mut k = hex_decode("0900000000000000000000000000000000000000000000000000000000000000");
    let mut u = k;
    for _ in 0..1000 {
        let next = x25519(&k, &u);
        u = k;
        k = next;
    }
    if k != hex_decode("684cf59ba83309552800ef566f2f4d3c1c3887c49360e3875f2eb94d99532c51") {
        return false;
    }

    let alice_priv = hex_decode("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
    let alice_pub_expected =
        hex_decode("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a");
    let alice_pub = x25519(&alice_priv, &BASEPOINT);
    if alice_pub != alice_pub_expected {
        return false;
    }

    let bob_priv = hex_decode("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb");
    let bob_pub_expected =
        hex_decode("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f");
    let bob_pub = x25519(&bob_priv, &BASEPOINT);
    if bob_pub != bob_pub_expected {
        return false;
    }

    let shared_expected =
        hex_decode("4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");
    x25519(&alice_priv, &bob_pub) == shared_expected
        && x25519(&bob_priv, &alice_pub) == shared_expected
}
