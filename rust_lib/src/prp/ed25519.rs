use super::sha512::{sha512, Sha512};
use super::x25519::{
    fe_add, fe_from_bytes, fe_mul, fe_mul_small, fe_one, fe_pow, fe_sq, fe_sub, fe_to_bytes, Fe,
};

const D: [u8; 32] = [
    0xa3, 0x78, 0x59, 0x13, 0xca, 0x4d, 0xeb, 0x75, 0xab, 0xd8, 0x41, 0x41, 0x4d, 0x0a, 0x70, 0x00,
    0x98, 0xe8, 0x79, 0x77, 0x79, 0x40, 0xc7, 0x8c, 0x73, 0xfe, 0x6f, 0x2b, 0xee, 0x6c, 0x03, 0x52,
];

const SQRT_M1: [u8; 32] = [
    0xb0, 0xa0, 0x0e, 0x4a, 0x27, 0x1b, 0xee, 0xc4, 0x78, 0xe4, 0x2f, 0xad, 0x06, 0x18, 0x43, 0x2f,
    0xa7, 0xd7, 0xfb, 0x3d, 0x99, 0x00, 0x4d, 0x2b, 0x0b, 0xdf, 0xc1, 0x4f, 0x80, 0x24, 0x83, 0x2b,
];

// base point, y = 4/5
const BASE_Y: [u8; 32] = [
    0x58, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
    0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
];

// group order L, little-endian
const L: [u8; 32] = [
    0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10,
];

#[derive(Clone, Copy)]
struct Point {
    x: Fe,
    y: Fe,
    z: Fe,
    t: Fe,
}

fn fe_zero() -> Fe {
    [0; 5]
}

fn d_fe() -> Fe {
    fe_from_bytes(&D)
}

fn point_identity() -> Point {
    Point {
        x: fe_zero(),
        y: fe_one(),
        z: fe_one(),
        t: fe_zero(),
    }
}

fn point_add(p: &Point, q: &Point) -> Point {
    let a = fe_mul(&fe_sub(&p.y, &p.x), &fe_sub(&q.y, &q.x));
    let b = fe_mul(&fe_add(&p.y, &p.x), &fe_add(&q.y, &q.x));
    let t2 = fe_add(&q.t, &q.t);
    let c = fe_mul(&p.t, &fe_mul(&t2, &d_fe()));
    let z2 = fe_add(&q.z, &q.z);
    let d = fe_mul(&p.z, &z2);
    let e = fe_sub(&b, &a);
    let f = fe_sub(&d, &c);
    let g = fe_add(&d, &c);
    let h = fe_add(&b, &a);

    Point {
        x: fe_mul(&e, &f),
        y: fe_mul(&g, &h),
        z: fe_mul(&f, &g),
        t: fe_mul(&e, &h),
    }
}

fn point_double(p: &Point) -> Point {
    // ref10 ge_p2_dbl + ge_p1p1_to_p3
    let a = fe_sq(&p.x);
    let b = fe_sq(&p.y);
    let c = fe_mul_small(&fe_sq(&p.z), 2);
    let t0 = fe_sq(&fe_add(&p.x, &p.y));
    let y_out = fe_add(&a, &b);
    let z_out = fe_sub(&b, &a);
    let x_out = fe_sub(&t0, &y_out);
    let t_out = fe_sub(&c, &z_out);

    Point {
        x: fe_mul(&x_out, &t_out),
        y: fe_mul(&y_out, &z_out),
        z: fe_mul(&z_out, &t_out),
        t: fe_mul(&x_out, &y_out),
    }
}

fn point_mul(scalar: &[u8; 32], base: &Point) -> Point {
    let mut acc = point_identity();
    for i in (0..256).rev() {
        acc = point_double(&acc);
        let bit = (scalar[i / 8] >> (i % 8)) & 1;
        if bit == 1 {
            acc = point_add(&acc, base);
        }
    }
    acc
}

fn fe_eq(a: &Fe, b: &Fe) -> bool {
    fe_to_bytes(a) == fe_to_bytes(b)
}

fn fe_invert(a: &Fe) -> Fe {
    // p-2 = 2^255 - 21
    let mut e = [0xFFu8; 32];
    e[0] = 0xEB;
    e[31] = 0x7F;
    fe_pow(&e, a)
}

fn point_compress(p: &Point) -> [u8; 32] {
    let zinv = fe_invert(&p.z);
    let x = fe_mul(&p.x, &zinv);
    let y = fe_mul(&p.y, &zinv);
    let mut out = fe_to_bytes(&y);
    let x_bytes = fe_to_bytes(&x);
    out[31] |= (x_bytes[0] & 1) << 7;
    out
}

fn point_decompress(bytes: &[u8; 32]) -> Option<Point> {
    let mut y_bytes = *bytes;
    let sign = y_bytes[31] >> 7;
    y_bytes[31] &= 0x7f;
    let y = fe_from_bytes(&y_bytes);

    let yy = fe_sq(&y);
    let u = fe_sub(&yy, &fe_one());
    let v = fe_add(&fe_mul(&d_fe(), &yy), &fe_one());

    let v3 = fe_mul(&fe_sq(&v), &v);
    let v7 = fe_mul(&fe_sq(&v3), &v);
    let mut e = [0xFFu8; 32];
    e[0] = 0xFD;
    e[31] = 0x0F;
    let mut x = fe_mul(&fe_mul(&u, &v3), &fe_pow(&e, &fe_mul(&u, &v7)));

    if !fe_eq(&fe_mul(&v, &fe_sq(&x)), &u) {
        x = fe_mul(&x, &fe_from_bytes(&SQRT_M1));
        if !fe_eq(&fe_mul(&v, &fe_sq(&x)), &u) {
            return None;
        }
    }

    let x_bytes = fe_to_bytes(&x);
    if (x_bytes[0] & 1) != sign {
        x = fe_sub(&fe_zero(), &x);
    }

    Some(Point {
        x,
        y,
        z: fe_one(),
        t: fe_mul(&x, &y),
    })
}

fn base_point() -> Point {
    match point_decompress(&BASE_Y) {
        Some(p) => p,
        None => point_identity(),
    }
}

fn sc_ge_l(acc: &[u8; 33]) -> bool {
    if acc[32] != 0 {
        return true;
    }
    for i in (0..32).rev() {
        if acc[i] > L[i] {
            return true;
        }
        if acc[i] < L[i] {
            return false;
        }
    }
    true
}

fn sc_sub_l(acc: &mut [u8; 33]) {
    let mut borrow = 0i16;
    for i in 0..32 {
        let diff = acc[i] as i16 - L[i] as i16 - borrow;
        acc[i] = if diff < 0 {
            (diff + 256) as u8
        } else {
            diff as u8
        };
        borrow = if diff < 0 { 1 } else { 0 };
    }
    acc[32] = (acc[32] as i16 - borrow).max(0) as u8;
}

pub(super) fn sc_reduce512(input: &[u8; 64]) -> [u8; 32] {
    let mut acc = [0u8; 33];
    for bit in (0..512).rev() {
        let mut carry = ((input[bit / 8] >> (bit % 8)) & 1) as u16;
        for byte in acc.iter_mut() {
            let v = ((*byte as u16) << 1) | carry;
            *byte = v as u8;
            carry = v >> 8;
        }
        if sc_ge_l(&acc) {
            sc_sub_l(&mut acc);
        }
    }
    let mut out = [0u8; 32];
    out.copy_from_slice(&acc[..32]);
    out
}

pub(super) fn sc_mul_add(a: &[u8; 32], b: &[u8; 32], c: &[u8; 32]) -> [u8; 32] {
    let mut product = [0u8; 64];
    for i in 0..32 {
        let mut carry = 0u16;
        for j in 0..32 {
            let v = product[i + j] as u16 + (a[i] as u16) * (b[j] as u16) + carry;
            product[i + j] = v as u8;
            carry = v >> 8;
        }
        let mut k = i + 32;
        while carry > 0 && k < 64 {
            let v = product[k] as u16 + carry;
            product[k] = v as u8;
            carry = v >> 8;
            k += 1;
        }
    }
    let mut carry = 0u16;
    for i in 0..64 {
        let cv = if i < 32 { c[i] as u16 } else { 0 };
        let v = product[i] as u16 + cv + carry;
        product[i] = v as u8;
        carry = v >> 8;
    }
    sc_reduce512(&product)
}

fn expand_private(private: &[u8; 32]) -> ([u8; 32], [u8; 32]) {
    let h = sha512(private);
    let mut scalar = [0u8; 32];
    scalar.copy_from_slice(&h[..32]);
    scalar[0] &= 248;
    scalar[31] &= 63;
    scalar[31] |= 64;
    let mut prefix = [0u8; 32];
    prefix.copy_from_slice(&h[32..]);
    (scalar, prefix)
}

pub(super) fn public_key(private: &[u8; 32]) -> [u8; 32] {
    let (scalar, _) = expand_private(private);
    point_compress(&point_mul(&scalar, &base_point()))
}

pub(super) fn compress_scalar_mul(scalar: &[u8; 32]) -> [u8; 32] {
    point_compress(&point_mul(scalar, &base_point()))
}

pub(super) fn expand_prefix(private: &[u8; 32]) -> [u8; 32] {
    let (_, prefix) = expand_private(private);
    prefix
}

pub(super) fn check_s_below_l(s: &[u8; 32]) -> bool {
    let mut not_less = false;
    for i in (0..32).rev() {
        if s[i] > L[i] {
            not_less = true;
            break;
        }
        if s[i] < L[i] {
            return true;
        }
    }
    !not_less
}

// left = S*B, right = R + k*A, equal as compressed points
pub(super) fn verify_commit(
    s: &[u8; 32],
    big_r: &[u8; 32],
    public: &[u8; 32],
    k: &[u8; 32],
) -> bool {
    let a_point = match point_decompress(public) {
        Some(p) => p,
        None => return false,
    };
    let r_point = match point_decompress(big_r) {
        Some(p) => p,
        None => return false,
    };

    let left = point_mul(s, &base_point());
    let right = point_add(&r_point, &point_mul(k, &a_point));
    point_compress(&left) == point_compress(&right)
}

fn hex_decode(hex: &[u8]) -> [u8; 32] {
    let mut out = [0u8; 32];
    for i in 0..32 {
        let hi = (hex[i * 2] as char).to_digit(16).unwrap() as u8;
        let lo = (hex[i * 2 + 1] as char).to_digit(16).unwrap() as u8;
        out[i] = hi << 4 | lo;
    }
    out
}

fn hex_decode64(hex: &[u8]) -> [u8; 64] {
    let mut out = [0u8; 64];
    for i in 0..64 {
        let hi = (hex[i * 2] as char).to_digit(16).unwrap() as u8;
        let lo = (hex[i * 2 + 1] as char).to_digit(16).unwrap() as u8;
        out[i] = hi << 4 | lo;
    }
    out
}

pub(super) fn selftest() -> bool {
    // RFC 8032 test 1: empty message
    let private = hex_decode(b"9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60");
    let expected_pub =
        hex_decode(b"d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a");
    if public_key(&private) != expected_pub {
        return false;
    }

    let expected_sig = hex_decode64(b"e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b");

    let mut hasher = Sha512::new();
    hasher.update(&expand_prefix(&private));
    let r = sc_reduce512(&hasher.finish());
    let big_r = compress_scalar_mul(&r);

    let mut hasher = Sha512::new();
    hasher.update(&big_r);
    hasher.update(&expected_pub);
    let k = sc_reduce512(&hasher.finish());
    let (scalar, _) = expand_private(&private);
    let s = sc_mul_add(&k, &scalar, &r);

    let mut sig = [0u8; 64];
    sig[..32].copy_from_slice(&big_r);
    sig[32..].copy_from_slice(&s);
    if sig != expected_sig {
        return false;
    }

    if !verify_commit(&s, &big_r, &expected_pub, &k) {
        return false;
    }

    let mut bad_s = s;
    bad_s[0] ^= 1;
    !verify_commit(&bad_s, &big_r, &expected_pub, &k)
}
