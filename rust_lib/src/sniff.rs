// Packet capture + dissection backing the Helios `wireshark.hls` app.
// The RTL8139 driver taps every frame it receives or sends into a ring buffer here (no allocation on that path);
// everything else (dissection, display filters, pcap I/O) runs on demand from Helios natives.

use alloc::boxed::Box;
use alloc::format;
use alloc::string::String;
use alloc::vec;
use alloc::vec::Vec;

use crate::helios::{native_arg, native_arg_str, native_return_str, register_native_fn, NativeFn};

extern "C" {
    fn avfs_save_file(name: *const u8, data: *const u8, size: u32, overwrite: bool) -> i32;
    fn avfs_read_file(name: *const u8, buffer: *mut u8, size: u32, offset: u32) -> i32;
    fn avfs_get_filesize(name: *const u8) -> i32;
}

// ─── Capture ring ─────────────────────────────────────────────────────────────

const SLOTS: usize = 128;
const SNAP: usize = 1536;

struct Slot { len: u16, orig: u16, tx: bool, ts: u32, data: [u8; SNAP] }
impl Slot {
    const EMPTY: Slot = Slot { len: 0, orig: 0, tx: false, ts: 0, data: [0; SNAP] };
}

struct Cap {
    on: bool,
    total: u32,
    bytes: u32,
    start: u32,
    lost: u32,
    counts: [u32; 10], // ARP IPv4 IPv6 TCP UDP ICMP DNS HTTP TLS other
    slots: [Slot; SLOTS],
}

static mut CAP: Cap = Cap {
    on: false, total: 0, bytes: 0, start: 0, lost: 0, counts: [0; 10], slots: [Slot::EMPTY; SLOTS],
};

fn cap() -> &'static mut Cap { unsafe { &mut *core::ptr::addr_of_mut!(CAP) } }
fn cap_ro() -> &'static Cap { unsafe { &*core::ptr::addr_of!(CAP) } }

fn first(c: &Cap) -> u32 {
    if c.total == 0 { 0 } else if c.total as usize > SLOTS { c.total - SLOTS as u32 + 1 } else { 1 }
}

fn slot(k: u32) -> Option<&'static Slot> {
    let c = cap_ro();
    if k == 0 || k > c.total || k < first(c) { return None; }
    Some(&c.slots[(k - 1) as usize % SLOTS])
}

fn clear(c: &mut Cap, now: u32) {
    c.total = 0; c.bytes = 0; c.lost = 0; c.counts = [0; 10]; c.start = now;
}

fn push(c: &mut Cap, frame: &[u8], orig: usize, tx: bool, ts: u32) {
    let i = c.total as usize % SLOTS;
    let n = frame.len().min(SNAP);
    {
        let s = &mut c.slots[i];
        s.data[..n].copy_from_slice(&frame[..n]);
        s.len = n as u16;
        s.orig = orig.min(65535) as u16;
        s.tx = tx;
        s.ts = ts;
    }
    c.total += 1;
    c.bytes = c.bytes.wrapping_add(orig as u32);
    if c.total as usize > SLOTS { c.lost += 1; }
    let l = parse(&c.slots[i].data[..n]).layers;
    let idx = if l & L_ARP != 0 { 0 }
        else if l & L_TCP != 0 {
            if l & L_HTTP != 0 { 7 } else if l & L_TLS != 0 { 8 } else { 3 }
        }
        else if l & L_UDP != 0 { if l & L_DNS != 0 { 6 } else { 4 } }
        else if l & (L_ICMP | L_ICMP6) != 0 { 5 }
        else if l & L_IP4 != 0 { 1 }
        else if l & L_IP6 != 0 { 2 }
        else { 9 };
    c.counts[idx] += 1;
}

// Called from the NIC driver for every frame; must stay allocation-free.
pub fn tap(frame: &[u8], tx: bool) {
    let c = cap();
    if !c.on { return; }
    let now = unsafe { super::get_ticks() };
    push(c, frame, frame.len(), tx, now);
}

// ─── Byte helpers ─────────────────────────────────────────────────────────────

fn rd16(b: &[u8], i: usize) -> u16 {
    if i + 1 < b.len() { ((b[i] as u16) << 8) | b[i + 1] as u16 } else { 0 }
}
fn rd32(b: &[u8], i: usize) -> u32 { ((rd16(b, i) as u32) << 16) | rd16(b, i + 2) as u32 }
fn g16(b: &[u8], i: usize) -> Option<usize> {
    if i + 1 < b.len() { Some(((b[i] as usize) << 8) | b[i + 1] as usize) } else { None }
}

fn mac(b: &[u8]) -> String {
    format!("{:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}", b[0], b[1], b[2], b[3], b[4], b[5])
}
fn ip4(b: &[u8]) -> String { format!("{}.{}.{}.{}", b[0], b[1], b[2], b[3]) }

fn ip6(b: &[u8]) -> String {
    let mut g = [0u16; 8];
    for i in 0..8 { g[i] = rd16(b, i * 2); }
    let (mut bs, mut bl) = (0usize, 0usize);
    let mut i = 0;
    while i < 8 {
        if g[i] == 0 {
            let s = i;
            while i < 8 && g[i] == 0 { i += 1; }
            if i - s > bl { bs = s; bl = i - s; }
        } else { i += 1; }
    }
    if bl < 2 { bl = 0; }
    let mut out = String::new();
    let mut i = 0;
    while i < 8 {
        if bl > 0 && i == bs { out.push_str("::"); i += bl; continue; }
        if !out.is_empty() && !out.ends_with(':') { out.push(':'); }
        out.push_str(&format!("{:x}", g[i]));
        i += 1;
    }
    out
}

fn mac_u64(b: &[u8]) -> u64 { b[..6].iter().fold(0u64, |a, &x| (a << 8) | x as u64) }

fn sanitize(b: &[u8], max: usize) -> String {
    b.iter().take(max).map(|&c| if c >= 0x20 && c < 0x7f { c as char } else { '.' }).collect()
}

fn first_line(d: &[u8], max: usize) -> String {
    let end = d.iter().position(|&c| c == b'\r' || c == b'\n').unwrap_or(d.len());
    sanitize(&d[..end], max)
}

// ─── Dissector ────────────────────────────────────────────────────────────────

const L_ETH: u32 = 1;
const L_ARP: u32 = 1 << 1;
const L_IP4: u32 = 1 << 2;
const L_IP6: u32 = 1 << 3;
const L_TCP: u32 = 1 << 4;
const L_UDP: u32 = 1 << 5;
const L_ICMP: u32 = 1 << 6;
const L_ICMP6: u32 = 1 << 7;
const L_DNS: u32 = 1 << 8;
const L_HTTP: u32 = 1 << 9;
const L_TLS: u32 = 1 << 10;
const L_DHCP: u32 = 1 << 11;
const L_SSH: u32 = 1 << 12;
const L_NTP: u32 = 1 << 13;
const L_VLAN: u32 = 1 << 14;
const L_BAD: u32 = 1 << 15;

#[derive(Clone, Copy, Default)]
struct P {
    layers: u32, etype: u16,
    eth_dst: [u8; 6], eth_src: [u8; 6],
    l3: usize, end: usize,
    ip_src: [u8; 4], ip_dst: [u8; 4], ip6_src: [u8; 16], ip6_dst: [u8; 16],
    ip_proto: u8, ttl: u8, ihl: usize, frag: u16, mf: bool,
    l4: usize, sport: u16, dport: u16, flags: u8, seq: u32, ack: u32, win: u16,
    app: usize, app_len: usize,
}

fn parse(f: &[u8]) -> P {
    let mut p = P::default();
    p.end = f.len();
    if f.len() < 14 { p.layers |= L_BAD; return p; }
    p.layers |= L_ETH;
    p.eth_dst.copy_from_slice(&f[0..6]);
    p.eth_src.copy_from_slice(&f[6..12]);
    let mut et = rd16(f, 12);
    let mut off = 14usize;
    if et == 0x8100 || et == 0x88a8 {
        if f.len() < 18 { p.layers |= L_BAD; return p; }
        p.layers |= L_VLAN;
        et = rd16(f, 16);
        off = 18;
    }
    p.etype = et;
    p.l3 = off;
    match et {
        0x0806 => { p.layers |= L_ARP; if f.len() < off + 28 { p.layers |= L_BAD; } }
        0x0800 => parse_ip4(f, &mut p),
        0x86dd => parse_ip6(f, &mut p),
        _ => {}
    }
    p
}

fn parse_ip4(f: &[u8], p: &mut P) {
    let o = p.l3;
    if f.len() < o + 20 || f[o] >> 4 != 4 { p.layers |= L_BAD; return; }
    let ihl = ((f[o] & 15) as usize) * 4;
    if ihl < 20 || f.len() < o + ihl { p.layers |= L_BAD; return; }
    p.layers |= L_IP4;
    p.ihl = ihl;
    let tot = rd16(f, o + 2) as usize;
    p.end = if tot >= ihl { f.len().min(o + tot) } else { f.len() };
    let ff = rd16(f, o + 6);
    p.frag = (ff & 0x1fff) * 8;
    p.mf = ff & 0x2000 != 0;
    p.ttl = f[o + 8];
    p.ip_proto = f[o + 9];
    p.ip_src.copy_from_slice(&f[o + 12..o + 16]);
    p.ip_dst.copy_from_slice(&f[o + 16..o + 20]);
    p.l4 = o + ihl;
    if p.frag == 0 { parse_l4(f, p); }
}

fn parse_ip6(f: &[u8], p: &mut P) {
    let o = p.l3;
    if f.len() < o + 40 || f[o] >> 4 != 6 { p.layers |= L_BAD; return; }
    p.layers |= L_IP6;
    p.end = f.len().min(o + 40 + rd16(f, o + 4) as usize);
    p.ttl = f[o + 7];
    p.ip6_src.copy_from_slice(&f[o + 8..o + 24]);
    p.ip6_dst.copy_from_slice(&f[o + 24..o + 40]);
    let mut nh = f[o + 6];
    let mut off = o + 40;
    for _ in 0..4 {
        match nh {
            0 | 43 | 60 => {
                if off + 8 > p.end { break; }
                let l = (f[off + 1] as usize + 1) * 8;
                nh = f[off];
                off += l;
            }
            44 => {
                if off + 8 > p.end { break; }
                p.frag = (rd16(f, off + 2) >> 3) * 8;
                nh = f[off];
                off += 8;
            }
            _ => break,
        }
    }
    p.ip_proto = nh;
    p.l4 = off;
    if p.frag == 0 && off <= p.end { parse_l4(f, p); }
}

fn is_http(d: &[u8]) -> bool {
    const M: [&[u8]; 9] = [b"GET ", b"POST ", b"HEAD ", b"PUT ", b"DELETE ", b"OPTIONS ", b"PATCH ", b"CONNECT ", b"HTTP/1."];
    M.iter().any(|m| d.starts_with(m))
}

fn parse_l4(f: &[u8], p: &mut P) {
    let o = p.l4;
    let end = p.end;
    match p.ip_proto {
        6 => {
            if o + 20 > end { p.layers |= L_BAD; return; }
            p.layers |= L_TCP;
            p.sport = rd16(f, o);
            p.dport = rd16(f, o + 2);
            p.seq = rd32(f, o + 4);
            p.ack = rd32(f, o + 8);
            p.flags = f[o + 13];
            p.win = rd16(f, o + 14);
            let doff = ((f[o + 12] >> 4) as usize) * 4;
            p.app = if doff >= 20 && o + doff <= end { o + doff } else { end };
            p.app_len = end - p.app;
            let d = &f[p.app..end];
            if is_http(d) { p.layers |= L_HTTP; }
            else if d.len() >= 5 && (0x14..=0x17).contains(&d[0]) && d[1] == 3 && d[2] <= 4 { p.layers |= L_TLS; }
            else if d.starts_with(b"SSH-") { p.layers |= L_SSH; }
        }
        17 => {
            if o + 8 > end { p.layers |= L_BAD; return; }
            p.layers |= L_UDP;
            p.sport = rd16(f, o);
            p.dport = rd16(f, o + 2);
            let ul = rd16(f, o + 4) as usize;
            let e = if ul >= 8 { end.min(o + ul) } else { end };
            p.app = o + 8;
            p.app_len = e - p.app;
            let (sp, dp) = (p.sport, p.dport);
            let has = |port: u16| sp == port || dp == port;
            if (has(53) || has(5353) || has(5355)) && p.app_len >= 12 { p.layers |= L_DNS; }
            else if (has(67) || has(68)) && p.app_len >= 240 { p.layers |= L_DHCP; }
            else if has(123) && p.app_len >= 48 { p.layers |= L_NTP; }
        }
        1 => { if o + 4 > end { p.layers |= L_BAD; } else { p.layers |= L_ICMP; } }
        58 => { if o + 4 > end { p.layers |= L_BAD; } else { p.layers |= L_ICMP6; } }
        _ => {}
    }
}

struct D { proto: &'static str, src: String, dst: String, info: String }

fn ipproto_name(n: u8) -> &'static str {
    match n { 1 => "ICMP", 2 => "IGMP", 6 => "TCP", 17 => "UDP", 41 => "IPv6", 47 => "GRE", 50 => "ESP",
        58 => "ICMPv6", 89 => "OSPF", 132 => "SCTP", _ => "IP" }
}

fn flags_str(fl: u8) -> String {
    const N: [(u8, &str); 8] = [(0x01, "FIN"), (0x02, "SYN"), (0x04, "RST"), (0x08, "PSH"),
        (0x10, "ACK"), (0x20, "URG"), (0x40, "ECE"), (0x80, "CWR")];
    let mut s = String::new();
    for (bit, name) in N.iter() {
        if fl & bit != 0 { if !s.is_empty() { s.push_str(", "); } s.push_str(name); }
    }
    if s.is_empty() { s.push_str("<None>"); }
    s
}

fn qtype(t: usize) -> String {
    match t {
        1 => String::from("A"), 2 => String::from("NS"), 5 => String::from("CNAME"), 6 => String::from("SOA"),
        12 => String::from("PTR"), 15 => String::from("MX"), 16 => String::from("TXT"), 28 => String::from("AAAA"),
        33 => String::from("SRV"), 65 => String::from("HTTPS"), 255 => String::from("ANY"),
        _ => format!("TYPE{}", t),
    }
}

// `m` is the whole DNS message so compression pointers resolve against it.
fn dns_name(m: &[u8], start: usize, out: &mut String) -> Option<usize> {
    let mut pos = start;
    let mut ret = 0usize;
    let mut jumped = false;
    let mut hops = 0;
    loop {
        let l = *m.get(pos)? as usize;
        if l == 0 { if !jumped { ret = pos + 1; } break; }
        if l & 0xC0 == 0xC0 {
            let b2 = *m.get(pos + 1)? as usize;
            if !jumped { ret = pos + 2; }
            jumped = true;
            pos = ((l & 0x3F) << 8) | b2;
            hops += 1;
            if hops > 8 { return None; }
            continue;
        }
        if l > 63 { return None; }
        let s = m.get(pos + 1..pos + 1 + l)?;
        if !out.is_empty() { out.push('.'); }
        out.push_str(&sanitize(s, 63));
        pos += 1 + l;
        if out.len() > 120 { return None; }
    }
    Some(ret)
}

fn dns_info(m: &[u8]) -> String {
    if m.len() < 12 { return String::from("Malformed DNS message"); }
    let id = rd16(m, 0);
    let fl = rd16(m, 2);
    let qd = rd16(m, 4) as usize;
    let an = rd16(m, 6) as usize;
    let resp = fl & 0x8000 != 0;
    let mut s = format!("Standard query{} 0x{:04x}", if resp { " response" } else { "" }, id);
    if resp {
        s.push_str(match fl & 15 { 0 => "", 1 => " Format error", 2 => " Server failure", 3 => " No such name",
            4 => " Not implemented", 5 => " Refused", _ => " Error" });
    }
    let mut pos = 12usize;
    for q in 0..qd.min(4) {
        let mut name = String::new();
        match dns_name(m, pos, &mut name) {
            Some(np) => {
                if q == 0 { s.push_str(&format!(" {} {}", qtype(rd16(m, np) as usize), name)); }
                pos = np + 4;
            }
            None => return s,
        }
    }
    if resp {
        for _ in 0..an.min(4) {
            let mut tmp = String::new();
            let np = match dns_name(m, pos, &mut tmp) { Some(x) => x, None => break };
            if np + 10 > m.len() { break; }
            let t = rd16(m, np) as usize;
            let rl = rd16(m, np + 8) as usize;
            let rd = np + 10;
            if rd + rl > m.len() { break; }
            let mut v = String::new();
            match t {
                1 if rl == 4 => v = ip4(&m[rd..rd + 4]),
                28 if rl == 16 => v = ip6(&m[rd..rd + 16]),
                2 | 5 | 12 => { dns_name(m, rd, &mut v); }
                _ => {}
            }
            s.push_str(&format!(" {} {}", qtype(t), v));
            pos = rd + rl;
        }
    }
    s
}

fn dhcp_info(d: &[u8]) -> String {
    let mut kind = "message";
    let mut i = 240usize;
    let mut guard = 0;
    while i + 1 < d.len() && guard < 64 {
        let code = d[i];
        if code == 255 { break; }
        if code == 0 { i += 1; guard += 1; continue; }
        let l = d[i + 1] as usize;
        if code == 53 && i + 2 < d.len() {
            kind = match d[i + 2] { 1 => "Discover", 2 => "Offer", 3 => "Request", 4 => "Decline", 5 => "ACK",
                6 => "NAK", 7 => "Release", 8 => "Inform", _ => "message" };
        }
        i += 2 + l;
        guard += 1;
    }
    format!("DHCP {} - Transaction ID 0x{:08x}", kind, rd32(d, 4))
}

fn ntp_info(d: &[u8]) -> String {
    let mode = match d[0] & 7 { 1 => "symmetric active", 2 => "symmetric passive", 3 => "client", 4 => "server",
        5 => "broadcast", _ => "reserved" };
    format!("NTP Version {}, {}", (d[0] >> 3) & 7, mode)
}

fn tls_label(d: &[u8]) -> &'static str {
    match rd16(d, 1) { 0x0301 => "TLSv1", 0x0302 => "TLSv1.1", 0x0303 => "TLSv1.2", 0x0304 => "TLSv1.3", _ => "TLS" }
}

fn tls_sni(p: &[u8]) -> Option<String> {
    let mut i = 5 + 4 + 2 + 32;
    i += 1 + *p.get(i)? as usize;
    i += 2 + g16(p, i)?;
    i += 1 + *p.get(i)? as usize;
    let total = g16(p, i)?;
    i += 2;
    let end = (i + total).min(p.len());
    while i + 4 <= end {
        let t = g16(p, i)?;
        let l = g16(p, i + 2)?;
        i += 4;
        if t == 0 {
            let nl = g16(p, i + 3)?;
            let name = p.get(i + 5..i + 5 + nl)?;
            return Some(sanitize(name, 80));
        }
        i += l;
    }
    None
}

fn tls_info(d: &[u8]) -> String {
    match d[0] {
        0x14 => String::from("Change Cipher Spec"),
        0x15 => String::from("Alert"),
        0x17 => String::from("Application Data"),
        _ => match d.get(5).copied() {
            Some(1) => match tls_sni(d) { Some(s) => format!("Client Hello (SNI={})", s), None => String::from("Client Hello") },
            Some(2) => String::from("Server Hello"),
            Some(4) => String::from("New Session Ticket"),
            Some(8) => String::from("Encrypted Extensions"),
            Some(11) => String::from("Certificate"),
            Some(12) => String::from("Server Key Exchange"),
            Some(13) => String::from("Certificate Request"),
            Some(14) => String::from("Server Hello Done"),
            Some(15) => String::from("Certificate Verify"),
            Some(16) => String::from("Client Key Exchange"),
            Some(20) => String::from("Finished"),
            _ => String::from("Handshake"),
        },
    }
}

fn icmp_info(f: &[u8], p: &P) -> String {
    let o = p.l4;
    let (t, c) = (f[o], f[o + 1]);
    if p.layers & L_ICMP6 != 0 {
        return match t {
            128 | 129 => format!("Echo (ping) {} id=0x{:04x}, seq={}, hop limit={}", if t == 128 { "request" } else { "reply" },
                rd16(f, o + 4), rd16(f, o + 6), p.ttl),
            1 => String::from("Destination unreachable"),
            2 => String::from("Packet too big"),
            3 => String::from("Time exceeded"),
            130 => String::from("Multicast Listener Query"),
            131 | 143 => String::from("Multicast Listener Report"),
            133 => String::from("Router Solicitation"),
            134 => String::from("Router Advertisement"),
            135 => String::from("Neighbor Solicitation"),
            136 => String::from("Neighbor Advertisement"),
            _ => format!("Type {} Code {}", t, c),
        };
    }
    match t {
        0 | 8 => format!("Echo (ping) {} id=0x{:04x}, seq={}, ttl={}", if t == 8 { "request" } else { "reply" },
            rd16(f, o + 4), rd16(f, o + 6), p.ttl),
        3 => format!("Destination unreachable ({})", match c {
            0 => "Network unreachable", 1 => "Host unreachable", 2 => "Protocol unreachable", 3 => "Port unreachable",
            4 => "Fragmentation needed", 13 => "Administratively filtered", _ => "other" }),
        4 => String::from("Source quench"),
        5 => String::from("Redirect"),
        11 => String::from("Time-to-live exceeded"),
        _ => format!("Type {} Code {}", t, c),
    }
}

fn arp_info(f: &[u8], p: &P) -> String {
    let o = p.l3;
    if f.len() < o + 28 { return String::from("Truncated ARP packet"); }
    let spa = ip4(&f[o + 14..o + 18]);
    let tpa = ip4(&f[o + 24..o + 28]);
    match rd16(f, o + 6) {
        1 => if spa == tpa { format!("ARP Announcement for {}", spa) } else { format!("Who has {}? Tell {}", tpa, spa) },
        2 => format!("{} is at {}", spa, mac(&f[o + 8..o + 14])),
        op => format!("ARP opcode {}", op),
    }
}

fn describe(f: &[u8], p: &P) -> D {
    let l = p.layers;
    if l & L_ETH == 0 {
        return D { proto: "Malformed", src: String::new(), dst: String::new(), info: String::from("Frame shorter than an Ethernet header") };
    }
    let mut d = D { proto: "ETH", src: mac(&p.eth_src), dst: mac(&p.eth_dst), info: String::new() };
    if p.eth_dst == [0xff; 6] { d.dst = String::from("Broadcast"); }
    if l & L_ARP != 0 { d.proto = "ARP"; d.info = arp_info(f, p); return d; }
    if l & L_IP4 != 0 { d.proto = "IPv4"; d.src = ip4(&p.ip_src); d.dst = ip4(&p.ip_dst); }
    else if l & L_IP6 != 0 { d.proto = "IPv6"; d.src = ip6(&p.ip6_src); d.dst = ip6(&p.ip6_dst); }
    else {
        d.info = match p.etype {
            0x888e => String::from("EAP over LAN"),
            0x88cc => String::from("Link Layer Discovery Protocol"),
            t if t <= 1500 => { d.proto = "LLC"; String::from("802.3 frame") }
            t => format!("Ethertype 0x{:04x}", t),
        };
        if l & L_BAD != 0 { d.info = String::from("Malformed or truncated frame"); }
        return d;
    }
    if l & L_BAD != 0 { d.info = String::from("Malformed or truncated packet"); return d; }
    if p.frag != 0 || p.mf {
        d.info = format!("Fragmented IP protocol (proto={} {}, off={})", ipproto_name(p.ip_proto), p.ip_proto, p.frag);
        return d;
    }
    let app = &f[p.app.min(f.len())..(p.app + p.app_len).min(f.len())];
    if l & L_TCP != 0 {
        d.proto = "TCP";
        let ack = if p.flags & 0x10 != 0 { format!(" Ack={}", p.ack) } else { String::new() };
        d.info = format!("{} -> {} [{}] Seq={}{} Win={} Len={}", p.sport, p.dport, flags_str(p.flags), p.seq, ack, p.win, p.app_len);
        if l & L_HTTP != 0 { d.proto = "HTTP"; d.info = first_line(app, 90); }
        else if l & L_TLS != 0 { d.proto = tls_label(app); d.info = tls_info(app); }
        else if l & L_SSH != 0 { d.proto = "SSH"; d.info = first_line(app, 90); }
    } else if l & L_UDP != 0 {
        d.proto = "UDP";
        d.info = format!("{} -> {} Len={}", p.sport, p.dport, p.app_len);
        if l & L_DNS != 0 {
            d.proto = if p.sport == 5353 || p.dport == 5353 { "MDNS" } else if p.sport == 5355 || p.dport == 5355 { "LLMNR" } else { "DNS" };
            d.info = dns_info(app);
        } else if l & L_DHCP != 0 { d.proto = "DHCP"; d.info = dhcp_info(app); }
        else if l & L_NTP != 0 { d.proto = "NTP"; d.info = ntp_info(app); }
    } else if l & L_ICMP != 0 { d.proto = "ICMP"; d.info = icmp_info(f, p); }
    else if l & L_ICMP6 != 0 { d.proto = "ICMPv6"; d.info = icmp_info(f, p); }
    else {
        d.proto = ipproto_name(p.ip_proto);
        d.info = format!("Protocol {}", p.ip_proto);
    }
    d
}

fn row_color(p: &P) -> u8 {
    let l = p.layers;
    if l & L_BAD != 0 || (l & L_TCP != 0 && p.flags & 0x04 != 0) { 12 }
    else if l & L_ARP != 0 { 14 }
    else if l & L_DNS != 0 { 11 }
    else if l & L_HTTP != 0 { 10 }
    else if l & L_TLS != 0 { 3 }
    else if l & L_TCP != 0 { 13 }
    else if l & L_UDP != 0 { 9 }
    else if l & (L_ICMP | L_ICMP6) != 0 { 5 }
    else { 7 }
}

fn row(k: u32, s: &Slot, width: usize) -> String {
    let f = &s.data[..s.len as usize];
    let p = parse(f);
    let d = describe(f, &p);
    let t = s.ts.wrapping_sub(cap_ro().start);
    let mut r = format!("{:>4} {:>8} {:<15} {:<15} {:<7} {:>4} {}", k, format!("{}.{:03}", t / 1000, t % 1000),
        d.src, d.dst, d.proto, s.orig, d.info);
    r.truncate(width.max(20));
    r
}

fn ln(o: &mut String, ind: usize, t: String) {
    for _ in 0..ind * 2 { o.push(' '); }
    o.push_str(&t);
    o.push('\n');
}

fn addr_kind(b: &[u8]) -> &'static str {
    if b[..6] == [0xff; 6] { " (Broadcast)" } else if b[0] & 1 != 0 { " (Multicast)" } else { "" }
}

fn etype_name(t: u16) -> &'static str {
    match t { 0x0800 => "IPv4", 0x0806 => "ARP", 0x86dd => "IPv6", 0x8100 => "802.1Q VLAN", 0x888e => "EAPOL",
        0x88cc => "LLDP", _ => "Unknown" }
}

fn ip4_csum_ok(h: &[u8]) -> bool {
    let mut s: u32 = 0;
    let mut i = 0;
    while i + 1 < h.len() { s += rd16(h, i) as u32; i += 2; }
    while s >> 16 != 0 { s = (s & 0xffff) + (s >> 16); }
    s == 0xffff
}

fn detail(k: u32, s: &Slot) -> String {
    let f = &s.data[..s.len as usize];
    let p = parse(f);
    let d = describe(f, &p);
    let l = p.layers;
    let t = s.ts.wrapping_sub(cap_ro().start);
    let mut o = String::new();
    ln(&mut o, 0, format!("Frame {}: {} bytes on wire ({} bits), {} bytes captured, {}", k, s.orig, s.orig as u32 * 8, s.len,
        if s.tx { "sent" } else { "received" }));
    ln(&mut o, 1, format!("Arrival: +{}.{:03} s since capture start", t / 1000, t % 1000));
    if l & L_ETH == 0 { ln(&mut o, 0, String::from("Malformed frame (shorter than an Ethernet header)")); return o; }
    ln(&mut o, 0, format!("Ethernet II, Src: {}, Dst: {}", mac(&p.eth_src), mac(&p.eth_dst)));
    ln(&mut o, 1, format!("Destination: {}{}", mac(&p.eth_dst), addr_kind(&p.eth_dst)));
    ln(&mut o, 1, format!("Source: {}{}", mac(&p.eth_src), addr_kind(&p.eth_src)));
    if l & L_VLAN != 0 { ln(&mut o, 1, format!("802.1Q VLAN ID: {}", rd16(f, 14) & 0x0fff)); }
    ln(&mut o, 1, format!("Type: {} (0x{:04x})", etype_name(p.etype), p.etype));
    let x = p.l3;
    if l & L_ARP != 0 && f.len() >= x + 28 {
        ln(&mut o, 0, format!("Address Resolution Protocol ({})", if rd16(f, x + 6) == 1 { "request" } else if rd16(f, x + 6) == 2 { "reply" } else { "other" }));
        ln(&mut o, 1, format!("Hardware type: {}  Protocol type: 0x{:04x}", rd16(f, x), rd16(f, x + 2)));
        ln(&mut o, 1, format!("Opcode: {}", rd16(f, x + 6)));
        ln(&mut o, 1, format!("Sender: {} / {}", mac(&f[x + 8..x + 14]), ip4(&f[x + 14..x + 18])));
        ln(&mut o, 1, format!("Target: {} / {}", mac(&f[x + 18..x + 24]), ip4(&f[x + 24..x + 28])));
    }
    if l & L_IP4 != 0 {
        ln(&mut o, 0, format!("Internet Protocol Version 4, Src: {}, Dst: {}", ip4(&p.ip_src), ip4(&p.ip_dst)));
        ln(&mut o, 1, format!("Header Length: {} bytes", p.ihl));
        ln(&mut o, 1, format!("Differentiated Services: 0x{:02x}", f[x + 1]));
        ln(&mut o, 1, format!("Total Length: {}", rd16(f, x + 2)));
        ln(&mut o, 1, format!("Identification: 0x{:04x} ({})", rd16(f, x + 4), rd16(f, x + 4)));
        let ff = rd16(f, x + 6);
        ln(&mut o, 1, format!("Flags: 0x{:x}{}{}", ff >> 13, if ff & 0x4000 != 0 { " Don't Fragment" } else { "" },
            if ff & 0x2000 != 0 { " More Fragments" } else { "" }));
        ln(&mut o, 1, format!("Fragment Offset: {}", p.frag));
        ln(&mut o, 1, format!("Time to Live: {}", p.ttl));
        ln(&mut o, 1, format!("Protocol: {} ({})", ipproto_name(p.ip_proto), p.ip_proto));
        ln(&mut o, 1, format!("Header Checksum: 0x{:04x} [{}]", rd16(f, x + 10),
            if ip4_csum_ok(&f[x..x + p.ihl]) { "valid" } else { "INVALID" }));
    } else if l & L_IP6 != 0 {
        ln(&mut o, 0, format!("Internet Protocol Version 6, Src: {}, Dst: {}", ip6(&p.ip6_src), ip6(&p.ip6_dst)));
        ln(&mut o, 1, format!("Flow Label: 0x{:05x}", rd32(f, x) & 0xfffff));
        ln(&mut o, 1, format!("Payload Length: {}", rd16(f, x + 4)));
        ln(&mut o, 1, format!("Next Header: {} ({})", ipproto_name(p.ip_proto), p.ip_proto));
        ln(&mut o, 1, format!("Hop Limit: {}", p.ttl));
    }
    let y = p.l4;
    if l & L_TCP != 0 {
        ln(&mut o, 0, format!("Transmission Control Protocol, Src Port: {}, Dst Port: {}, Seq: {}, Ack: {}, Len: {}",
            p.sport, p.dport, p.seq, p.ack, p.app_len));
        ln(&mut o, 1, format!("Header Length: {} bytes", p.app - y));
        ln(&mut o, 1, format!("Flags: 0x{:03x} ({})", ((f[y + 12] as u16 & 1) << 8) | p.flags as u16, flags_str(p.flags)));
        ln(&mut o, 1, format!("Window: {}", p.win));
        ln(&mut o, 1, format!("Checksum: 0x{:04x}", rd16(f, y + 16)));
        ln(&mut o, 1, format!("Urgent Pointer: {}", rd16(f, y + 18)));
    } else if l & L_UDP != 0 {
        ln(&mut o, 0, format!("User Datagram Protocol, Src Port: {}, Dst Port: {}", p.sport, p.dport));
        ln(&mut o, 1, format!("Length: {}", rd16(f, y + 4)));
        ln(&mut o, 1, format!("Checksum: 0x{:04x}", rd16(f, y + 6)));
    } else if l & (L_ICMP | L_ICMP6) != 0 {
        ln(&mut o, 0, String::from(if l & L_ICMP6 != 0 { "Internet Control Message Protocol v6" } else { "Internet Control Message Protocol" }));
        ln(&mut o, 1, format!("Type: {}  Code: {}", f[y], f[y + 1]));
        ln(&mut o, 1, format!("Checksum: 0x{:04x}", rd16(f, y + 2)));
        ln(&mut o, 1, icmp_info(f, &p));
    }
    let app = &f[p.app.min(f.len())..(p.app + p.app_len).min(f.len())];
    if l & L_HTTP != 0 {
        ln(&mut o, 0, String::from("Hypertext Transfer Protocol"));
        for line in app.split(|&b| b == b'\n').take(14) {
            let line = if line.ends_with(b"\r") { &line[..line.len() - 1] } else { line };
            if line.is_empty() { break; }
            ln(&mut o, 1, sanitize(line, 100));
        }
    } else if l & L_DNS != 0 {
        ln(&mut o, 0, String::from("Domain Name System"));
        ln(&mut o, 1, d.info.clone());
    } else if l & L_TLS != 0 {
        ln(&mut o, 0, String::from("Transport Layer Security"));
        ln(&mut o, 1, d.info.clone());
    } else if l & (L_DHCP | L_NTP | L_SSH) != 0 {
        ln(&mut o, 0, String::from(d.proto));
        ln(&mut o, 1, d.info.clone());
    } else if l & (L_TCP | L_UDP) != 0 && p.app_len > 0 {
        ln(&mut o, 0, format!("Data ({} bytes)", p.app_len));
    }
    if l & (L_IP4 | L_IP6) != 0 && p.end < f.len() {
        ln(&mut o, 0, format!("Ethernet padding/trailer: {} bytes", f.len() - p.end));
    }
    o
}

fn hexdump(f: &[u8]) -> String {
    let mut o = String::new();
    for (ci, ch) in f.chunks(16).enumerate() {
        o.push_str(&format!("{:04x} ", ci * 16));
        for i in 0..16 {
            if i == 8 { o.push(' '); }
            if i < ch.len() { o.push_str(&format!(" {:02x}", ch[i])); } else { o.push_str("   "); }
        }
        o.push_str("  ");
        for (i, &b) in ch.iter().enumerate() {
            if i == 8 { o.push(' '); }
            o.push(if b >= 0x20 && b < 0x7f { b as char } else { '.' });
        }
        o.push('\n');
    }
    o
}

// ─── Display filter (Wireshark-style subset) ──────────────────────────────────

#[derive(Clone, Copy, PartialEq)]
enum Op { Eq, Ne, Gt, Lt, Ge, Le }

#[derive(Clone, Copy)]
enum Field {
    IpAddr, IpSrc, IpDst, IpTtl, IpProto, EthAddr, EthSrc, EthDst, EthType,
    TcpPort, TcpSrc, TcpDst, UdpPort, UdpSrc, UdpDst, Port, Len, TcpBit(u8),
}

enum Expr {
    Not(Box<Expr>),
    And(Box<Expr>, Box<Expr>),
    Or(Box<Expr>, Box<Expr>),
    Layer(u32),
    Cmp(Field, Op, u64),
    Contains(Vec<u8>),
}

enum Tok { Lp, Rp, Not, And, Or, Op(Op), Word(String), Str(Vec<u8>) }

fn field_from_name(n: &str) -> Option<Field> {
    Some(match n {
        "ip.addr" => Field::IpAddr, "ip.src" => Field::IpSrc, "ip.dst" => Field::IpDst,
        "ip.ttl" => Field::IpTtl, "ip.proto" => Field::IpProto,
        "eth.addr" => Field::EthAddr, "eth.src" => Field::EthSrc, "eth.dst" => Field::EthDst, "eth.type" => Field::EthType,
        "tcp.port" => Field::TcpPort, "tcp.srcport" => Field::TcpSrc, "tcp.dstport" => Field::TcpDst,
        "udp.port" => Field::UdpPort, "udp.srcport" => Field::UdpSrc, "udp.dstport" => Field::UdpDst,
        "port" => Field::Port, "frame.len" | "len" => Field::Len,
        "tcp.flags.fin" => Field::TcpBit(0x01), "tcp.flags.syn" => Field::TcpBit(0x02),
        "tcp.flags.reset" => Field::TcpBit(0x04), "tcp.flags.push" => Field::TcpBit(0x08),
        "tcp.flags.ack" => Field::TcpBit(0x10),
        _ => return None,
    })
}

fn layer_from_name(n: &str) -> Option<u32> {
    Some(match n {
        "eth" | "ethernet" => L_ETH, "arp" => L_ARP, "ip" => L_IP4, "ipv6" => L_IP6, "tcp" => L_TCP, "udp" => L_UDP,
        "icmp" => L_ICMP, "icmpv6" => L_ICMP6, "dns" | "mdns" => L_DNS, "http" => L_HTTP, "tls" | "ssl" => L_TLS,
        "dhcp" | "bootp" => L_DHCP, "ssh" => L_SSH, "ntp" => L_NTP, "vlan" => L_VLAN,
        _ => return None,
    })
}

fn parse_num(s: &str) -> Option<u64> {
    if let Some(h) = s.strip_prefix("0x") { u64::from_str_radix(h, 16).ok() } else { s.parse::<u64>().ok() }
}

fn parse_val(fd: Field, s: &str) -> Result<u64, &'static str> {
    match fd {
        Field::IpAddr | Field::IpSrc | Field::IpDst => {
            let mut v = 0u64;
            let mut n = 0;
            for part in s.split('.') {
                let b = part.parse::<u8>().map_err(|_| "bad IPv4 address")?;
                v = (v << 8) | b as u64;
                n += 1;
            }
            if n != 4 { return Err("bad IPv4 address"); }
            Ok(v)
        }
        Field::EthAddr | Field::EthSrc | Field::EthDst => {
            let mut v = 0u64;
            let mut n = 0;
            for part in s.split(':') {
                let b = u8::from_str_radix(part, 16).map_err(|_| "bad MAC address")?;
                v = (v << 8) | b as u64;
                n += 1;
            }
            if n != 6 { return Err("bad MAC address"); }
            Ok(v)
        }
        _ => parse_num(s).ok_or("bad number"),
    }
}

fn tokenize(s: &str) -> Result<Vec<Tok>, &'static str> {
    let b = s.as_bytes();
    let mut i = 0;
    let mut t = Vec::new();
    while i < b.len() {
        let next = b.get(i + 1).copied();
        match b[i] {
            b' ' | b'\t' => i += 1,
            b'(' => { t.push(Tok::Lp); i += 1; }
            b')' => { t.push(Tok::Rp); i += 1; }
            b'!' => if next == Some(b'=') { t.push(Tok::Op(Op::Ne)); i += 2; } else { t.push(Tok::Not); i += 1; },
            b'=' => { t.push(Tok::Op(Op::Eq)); i += if next == Some(b'=') { 2 } else { 1 }; }
            b'>' => if next == Some(b'=') { t.push(Tok::Op(Op::Ge)); i += 2; } else { t.push(Tok::Op(Op::Gt)); i += 1; },
            b'<' => if next == Some(b'=') { t.push(Tok::Op(Op::Le)); i += 2; } else { t.push(Tok::Op(Op::Lt)); i += 1; },
            b'&' => if next == Some(b'&') { t.push(Tok::And); i += 2; } else { return Err("use && for and"); },
            b'|' => if next == Some(b'|') { t.push(Tok::Or); i += 2; } else { return Err("use || for or"); },
            b'"' => {
                let mut j = i + 1;
                while j < b.len() && b[j] != b'"' { j += 1; }
                if j >= b.len() { return Err("unterminated string"); }
                t.push(Tok::Str(b[i + 1..j].to_vec()));
                i = j + 1;
            }
            _ => {
                let mut j = i;
                while j < b.len() && !matches!(b[j], b' ' | b'\t' | b'(' | b')' | b'!' | b'=' | b'<' | b'>' | b'&' | b'|' | b'"') { j += 1; }
                let w = &s[i..j];
                t.push(if w.eq_ignore_ascii_case("and") { Tok::And }
                    else if w.eq_ignore_ascii_case("or") { Tok::Or }
                    else if w.eq_ignore_ascii_case("not") { Tok::Not }
                    else if w.eq_ignore_ascii_case("eq") { Tok::Op(Op::Eq) }
                    else if w.eq_ignore_ascii_case("ne") { Tok::Op(Op::Ne) }
                    else if w.eq_ignore_ascii_case("gt") { Tok::Op(Op::Gt) }
                    else if w.eq_ignore_ascii_case("lt") { Tok::Op(Op::Lt) }
                    else if w.eq_ignore_ascii_case("ge") { Tok::Op(Op::Ge) }
                    else if w.eq_ignore_ascii_case("le") { Tok::Op(Op::Le) }
                    else { Tok::Word(String::from(w)) });
                i = j;
            }
        }
    }
    Ok(t)
}

struct Parser { t: Vec<Tok>, i: usize }

impl Parser {
    fn or(&mut self) -> Result<Expr, &'static str> {
        let mut l = self.and()?;
        while matches!(self.t.get(self.i), Some(Tok::Or)) {
            self.i += 1;
            let r = self.and()?;
            l = Expr::Or(Box::new(l), Box::new(r));
        }
        Ok(l)
    }

    fn and(&mut self) -> Result<Expr, &'static str> {
        let mut l = self.not()?;
        while matches!(self.t.get(self.i), Some(Tok::And)) {
            self.i += 1;
            let r = self.not()?;
            l = Expr::And(Box::new(l), Box::new(r));
        }
        Ok(l)
    }

    fn not(&mut self) -> Result<Expr, &'static str> {
        if matches!(self.t.get(self.i), Some(Tok::Not)) {
            self.i += 1;
            return Ok(Expr::Not(Box::new(self.not()?)));
        }
        self.primary()
    }

    fn primary(&mut self) -> Result<Expr, &'static str> {
        match self.t.get(self.i) {
            Some(Tok::Lp) => {
                self.i += 1;
                let e = self.or()?;
                if !matches!(self.t.get(self.i), Some(Tok::Rp)) { return Err("missing )"); }
                self.i += 1;
                Ok(e)
            }
            Some(Tok::Word(w)) => {
                let lw = w.to_ascii_lowercase();
                self.i += 1;
                self.word(&lw)
            }
            _ => Err("expected a protocol or field"),
        }
    }

    fn word(&mut self, w: &str) -> Result<Expr, &'static str> {
        if matches!(self.t.get(self.i), Some(Tok::Word(k)) if k.eq_ignore_ascii_case("contains")) {
            if w != "frame" && w != "data" { return Err("contains only works on frame"); }
            self.i += 1;
            let v = match self.t.get(self.i) {
                Some(Tok::Str(s)) => s.clone(),
                Some(Tok::Word(s)) => s.as_bytes().to_vec(),
                _ => return Err("contains needs a string"),
            };
            self.i += 1;
            return Ok(Expr::Contains(v));
        }
        if let Some(Tok::Op(op)) = self.t.get(self.i) {
            let op = *op;
            self.i += 1;
            let fd = field_from_name(w).ok_or("unknown field")?;
            let val = match self.t.get(self.i) { Some(Tok::Word(v)) => v.clone(), _ => return Err("missing value") };
            self.i += 1;
            return Ok(Expr::Cmp(fd, op, parse_val(fd, &val)?));
        }
        if let Some(l) = layer_from_name(w) { return Ok(Expr::Layer(l)); }
        if let Some(Field::TcpBit(m)) = field_from_name(w) { return Ok(Expr::Cmp(Field::TcpBit(m), Op::Eq, 1)); }
        Err("unknown protocol or field")
    }
}

fn compile(s: &str) -> Result<Expr, &'static str> {
    let mut p = Parser { t: tokenize(s)?, i: 0 };
    let e = p.or()?;
    if p.i != p.t.len() { return Err("unexpected token"); }
    Ok(e)
}

fn field_vals(fd: Field, f: &[u8], p: &P) -> ([u64; 2], usize) {
    let l = p.layers;
    let (ip4, tcp, udp) = (l & L_IP4 != 0, l & L_TCP != 0, l & L_UDP != 0);
    let s4 = u32::from_be_bytes(p.ip_src) as u64;
    let d4 = u32::from_be_bytes(p.ip_dst) as u64;
    let (sp, dp) = (p.sport as u64, p.dport as u64);
    match fd {
        Field::IpAddr if ip4 => ([s4, d4], 2),
        Field::IpSrc if ip4 => ([s4, 0], 1),
        Field::IpDst if ip4 => ([d4, 0], 1),
        Field::IpTtl if ip4 => ([p.ttl as u64, 0], 1),
        Field::IpProto if ip4 => ([p.ip_proto as u64, 0], 1),
        Field::EthAddr if l & L_ETH != 0 => ([mac_u64(&p.eth_src), mac_u64(&p.eth_dst)], 2),
        Field::EthSrc if l & L_ETH != 0 => ([mac_u64(&p.eth_src), 0], 1),
        Field::EthDst if l & L_ETH != 0 => ([mac_u64(&p.eth_dst), 0], 1),
        Field::EthType if l & L_ETH != 0 => ([p.etype as u64, 0], 1),
        Field::TcpPort if tcp => ([sp, dp], 2),
        Field::TcpSrc if tcp => ([sp, 0], 1),
        Field::TcpDst if tcp => ([dp, 0], 1),
        Field::UdpPort if udp => ([sp, dp], 2),
        Field::UdpSrc if udp => ([sp, 0], 1),
        Field::UdpDst if udp => ([dp, 0], 1),
        Field::Port if tcp || udp => ([sp, dp], 2),
        Field::Len => ([f.len() as u64, 0], 1),
        Field::TcpBit(m) if tcp => ([(p.flags & m != 0) as u64, 0], 1),
        _ => ([0, 0], 0),
    }
}

fn eval(e: &Expr, f: &[u8], p: &P) -> bool {
    match e {
        Expr::Not(x) => !eval(x, f, p),
        Expr::And(a, b) => eval(a, f, p) && eval(b, f, p),
        Expr::Or(a, b) => eval(a, f, p) || eval(b, f, p),
        Expr::Layer(m) => p.layers & m != 0,
        Expr::Contains(n) => !n.is_empty() && f.windows(n.len()).any(|w| w == &n[..]),
        Expr::Cmp(fd, op, v) => {
            let (vals, cnt) = field_vals(*fd, f, p);
            if cnt == 0 { return false; }
            let vals = &vals[..cnt];
            match op {
                Op::Eq => vals.iter().any(|&x| x == *v),
                Op::Ne => !vals.iter().any(|&x| x == *v),
                Op::Gt => vals.iter().any(|&x| x > *v),
                Op::Lt => vals.iter().any(|&x| x < *v),
                Op::Ge => vals.iter().any(|&x| x >= *v),
                Op::Le => vals.iter().any(|&x| x <= *v),
            }
        }
    }
}

struct Ui { filter: Option<Box<Expr>>, text: String, err: &'static str, view: Vec<u32> }
static mut UI: Ui = Ui { filter: None, text: String::new(), err: "", view: Vec::new() };
fn ui() -> &'static mut Ui { unsafe { &mut *core::ptr::addr_of_mut!(UI) } }

fn visible(k: u32) -> bool {
    match slot(k) {
        Some(s) => {
            let f = &s.data[..s.len as usize];
            match &ui().filter { Some(e) => eval(e, f, &parse(f)), None => true }
        }
        None => false,
    }
}

fn build_view() -> usize {
    let c = cap_ro();
    let u = ui();
    u.view.clear();
    if c.total > 0 {
        for k in first(c)..=c.total { if visible(k) { u.view.push(k); } }
    }
    u.view.len()
}

// ─── pcap I/O ─────────────────────────────────────────────────────────────────

unsafe fn path_ptr(a: *const i32, n: i32) -> Option<*const u8> {
    let p = native_arg(a, n, 0) as usize;
    if p < 0x1000 { None } else { Some(p as *const u8) }
}

fn pcap_bytes(list: &[u32]) -> Vec<u8> {
    let c = cap_ro();
    let mut out: Vec<u8> = Vec::with_capacity(24 + list.len() * 1000);
    out.extend_from_slice(&0xa1b2c3d4u32.to_le_bytes());
    out.extend_from_slice(&2u16.to_le_bytes());
    out.extend_from_slice(&4u16.to_le_bytes());
    out.extend_from_slice(&0u32.to_le_bytes());
    out.extend_from_slice(&0u32.to_le_bytes());
    out.extend_from_slice(&65535u32.to_le_bytes());
    out.extend_from_slice(&1u32.to_le_bytes());
    for &k in list {
        if let Some(s) = slot(k) {
            let t = s.ts.wrapping_sub(c.start);
            out.extend_from_slice(&(t / 1000).to_le_bytes());
            out.extend_from_slice(&((t % 1000) * 1000).to_le_bytes());
            out.extend_from_slice(&(s.len as u32).to_le_bytes());
            out.extend_from_slice(&(s.orig as u32).to_le_bytes());
            out.extend_from_slice(&s.data[..s.len as usize]);
        }
    }
    out
}

// ─── Natives ──────────────────────────────────────────────────────────────────

fn arg(a: *const i32, n: i32, i: i32) -> i32 { unsafe { native_arg(a, n, i) } }

fn ret_str(s: &str) -> i32 { unsafe { native_return_str(s.as_bytes()) } }

fn nic_ready() -> bool { unsafe { (*core::ptr::addr_of!(super::RTL8139_DEVICE)).is_some() } }

// cap_start(keep): begins capturing; keep=1 resumes without clearing. -1 if no NIC.
fn n_cap_start(a: *const i32, n: i32) -> i32 {
    if !nic_ready() { return -1; }
    let c = cap();
    if arg(a, n, 0) == 0 || c.total == 0 { clear(c, unsafe { super::get_ticks() }); }
    c.on = true;
    0
}
fn n_cap_stop(_a: *const i32, _n: i32) -> i32 { cap().on = false; 0 }
fn n_cap_clear(_a: *const i32, _n: i32) -> i32 { clear(cap(), unsafe { super::get_ticks() }); build_view(); 0 }

// cap_poll() -> frames drained from the NIC ring
fn n_cap_poll(_a: *const i32, _n: i32) -> i32 {
    if !cap().on { return 0; }
    unsafe { super::RX_RESPONSE_LENGTH = 0; }
    super::rtl8139_receive(usize::MAX).max(0)
}

fn n_cap_total(_a: *const i32, _n: i32) -> i32 { cap_ro().total as i32 }
fn n_cap_first(_a: *const i32, _n: i32) -> i32 { first(cap_ro()) as i32 }

// cap_stat(i): 0 frames, 1 bytes, 2 retained, 3 overwritten, 4 capturing, 5 NIC present, 6 shown,
// 7 ARP, 8 IPv4, 9 IPv6, 10 TCP, 11 UDP, 12 ICMP, 13 DNS, 14 HTTP, 15 TLS, 16 other, 17 ms since start
fn n_cap_stat(a: *const i32, n: i32) -> i32 {
    let c = cap_ro();
    match arg(a, n, 0) {
        0 => c.total as i32,
        1 => c.bytes as i32,
        2 => (c.total - first(c) + if c.total > 0 { 1 } else { 0 }) as i32,
        3 => c.lost as i32,
        4 => c.on as i32,
        5 => nic_ready() as i32,
        6 => ui().view.len() as i32,
        i @ 7..=16 => c.counts[(i - 7) as usize] as i32,
        17 => {
            let now = unsafe { super::get_ticks() };
            now.wrapping_sub(c.start) as i32
        }
        _ => 0,
    }
}

// cap_filter(text) -> 0 ok (empty clears), -1 syntax error (see cap_filter_err)
fn n_cap_filter(a: *const i32, n: i32) -> i32 {
    let raw = unsafe { native_arg_str(a, n, 0) };
    let txt = core::str::from_utf8(raw).unwrap_or("").trim();
    let u = ui();
    if txt.is_empty() { u.filter = None; u.text.clear(); u.err = ""; return 0; }
    match compile(txt) {
        Ok(e) => { u.filter = Some(Box::new(e)); u.text = String::from(txt); u.err = ""; 0 }
        Err(m) => { u.err = m; -1 }
    }
}
fn n_cap_filter_err(_a: *const i32, _n: i32) -> i32 { ret_str(ui().err) }
fn n_cap_filter_text(_a: *const i32, _n: i32) -> i32 { ret_str(&ui().text) }

// cap_view() rebuilds the list of packets passing the filter; cap_view_at(i) -> packet number or -1
fn n_cap_view(_a: *const i32, _n: i32) -> i32 { build_view() as i32 }
fn n_cap_view_at(a: *const i32, n: i32) -> i32 {
    let i = arg(a, n, 0);
    if i < 0 { return -1; }
    ui().view.get(i as usize).map_or(-1, |&k| k as i32)
}

fn n_pkt_visible(a: *const i32, n: i32) -> i32 { visible(arg(a, n, 0) as u32) as i32 }

// pkt_header(width) / pkt_row(no, width): one list line, cut to width
fn n_pkt_header(a: *const i32, n: i32) -> i32 {
    let mut r = format!("{:>4} {:>8} {:<15} {:<15} {:<7} {:>4} {}", "No.", "Time", "Source", "Destination", "Proto", "Len", "Info");
    r.truncate((arg(a, n, 0).max(0) as usize).max(20));
    ret_str(&r)
}
fn n_pkt_row(a: *const i32, n: i32) -> i32 {
    let k = arg(a, n, 0) as u32;
    match slot(k) { Some(s) => ret_str(&row(k, s, arg(a, n, 1).max(20) as usize)), None => ret_str("") }
}
fn n_pkt_color(a: *const i32, n: i32) -> i32 {
    match slot(arg(a, n, 0) as u32) { Some(s) => row_color(&parse(&s.data[..s.len as usize])) as i32, None => 15 }
}
fn n_pkt_detail(a: *const i32, n: i32) -> i32 {
    let k = arg(a, n, 0) as u32;
    match slot(k) { Some(s) => ret_str(&detail(k, s)), None => ret_str("") }
}
fn n_pkt_hex(a: *const i32, n: i32) -> i32 {
    match slot(arg(a, n, 0) as u32) { Some(s) => ret_str(&hexdump(&s.data[..s.len as usize])), None => ret_str("") }
}

// cap_convs() -> top conversations by bytes among retained packets
fn n_cap_convs(_a: *const i32, _n: i32) -> i32 {
    struct Cv { a: String, b: String, proto: &'static str, pk: u32, by: u32 }
    let c = cap_ro();
    let mut v: Vec<Cv> = Vec::new();
    if c.total > 0 {
        for k in first(c)..=c.total {
            let s = match slot(k) { Some(s) => s, None => continue };
            let f = &s.data[..s.len as usize];
            let p = parse(f);
            let d = describe(f, &p);
            if d.src.is_empty() { continue; }
            let proto = if p.layers & L_TCP != 0 { "TCP" } else if p.layers & L_UDP != 0 { "UDP" }
                else if p.layers & L_ICMP != 0 { "ICMP" } else if p.layers & L_ICMP6 != 0 { "ICMPv6" }
                else if p.layers & L_ARP != 0 { "ARP" } else { "other" };
            let (x, y) = if d.src <= d.dst { (d.src, d.dst) } else { (d.dst, d.src) };
            match v.iter_mut().find(|e| e.a == x && e.b == y && e.proto == proto) {
                Some(e) => { e.pk += 1; e.by += s.orig as u32; }
                None => v.push(Cv { a: x, b: y, proto, pk: 1, by: s.orig as u32 }),
            }
        }
    }
    v.sort_by(|x, y| y.by.cmp(&x.by));
    let mut out = String::new();
    for e in v.iter().take(12) {
        out.push_str(&format!("{:<15} <-> {:<15} {:<6} {:>5} pkts {:>7} bytes\n", e.a, e.b, e.proto, e.pk, e.by));
    }
    ret_str(&out)
}

// cap_save(path, shown_only) -> packets written or -1
fn n_cap_save(a: *const i32, n: i32) -> i32 {
    unsafe {
        let path = match path_ptr(a, n) { Some(p) => p, None => return -1 };
        let list: Vec<u32> = if arg(a, n, 1) != 0 { build_view(); ui().view.clone() } else {
            let c = cap_ro();
            if c.total == 0 { Vec::new() } else { (first(c)..=c.total).collect() }
        };
        let bytes = pcap_bytes(&list);
        if avfs_save_file(path, bytes.as_ptr(), bytes.len() as u32, true) != 0 { return -1; }
        list.len() as i32
    }
}

// cap_load(path) -> packets loaded; -1 unreadable, -2 not an Ethernet pcap
fn n_cap_load(a: *const i32, n: i32) -> i32 {
    unsafe {
        let path = match path_ptr(a, n) { Some(p) => p, None => return -1 };
        let size = avfs_get_filesize(path);
        if size < 24 || size > 4 * 1024 * 1024 { return -1; }
        let mut buf = vec![0u8; size as usize];
        if avfs_read_file(path, buf.as_mut_ptr(), size as u32, 0) < 0 { return -1; }
        let magic = u32::from_le_bytes([buf[0], buf[1], buf[2], buf[3]]);
        let (le, nano) = match magic {
            0xa1b2c3d4 => (true, false), 0xa1b23c4d => (true, true),
            0xd4c3b2a1 => (false, false), 0x4d3cb2a1 => (false, true),
            _ => return -2,
        };
        let rd = |i: usize| -> u32 {
            let b = [buf[i], buf[i + 1], buf[i + 2], buf[i + 3]];
            if le { u32::from_le_bytes(b) } else { u32::from_be_bytes(b) }
        };
        if rd(20) & 0xffff != 1 { return -2; }
        let c = cap();
        c.on = false;
        clear(c, 0);
        let mut pos = 24usize;
        let mut base: Option<u64> = None;
        while pos + 16 <= buf.len() {
            let frac = if nano { rd(pos + 4) / 1_000_000 } else { rd(pos + 4) / 1000 };
            let ms = rd(pos) as u64 * 1000 + frac as u64;
            let incl = rd(pos + 8) as usize;
            let orig = rd(pos + 12) as usize;
            pos += 16;
            if pos + incl > buf.len() { break; }
            let b0 = *base.get_or_insert(ms);
            push(c, &buf[pos..pos + incl], orig, false, ms.wrapping_sub(b0) as u32);
            pos += incl;
        }
        build_view();
        c.total as i32
    }
}

pub unsafe fn register() {
    let table: [(&[u8], NativeFn); 21] = [
        (b"cap_start\0", n_cap_start), (b"cap_stop\0", n_cap_stop), (b"cap_clear\0", n_cap_clear),
        (b"cap_poll\0", n_cap_poll), (b"cap_total\0", n_cap_total), (b"cap_first\0", n_cap_first),
        (b"cap_stat\0", n_cap_stat), (b"cap_filter\0", n_cap_filter), (b"cap_filter_err\0", n_cap_filter_err),
        (b"cap_filter_text\0", n_cap_filter_text), (b"cap_view\0", n_cap_view), (b"cap_view_at\0", n_cap_view_at),
        (b"pkt_visible\0", n_pkt_visible), (b"pkt_header\0", n_pkt_header), (b"pkt_row\0", n_pkt_row),
        (b"pkt_color\0", n_pkt_color), (b"pkt_detail\0", n_pkt_detail), (b"pkt_hex\0", n_pkt_hex),
        (b"cap_convs\0", n_cap_convs), (b"cap_save\0", n_cap_save), (b"cap_load\0", n_cap_load),
    ];
    for (name, func) in table.iter() { register_native_fn(name.as_ptr(), *func); }
}
