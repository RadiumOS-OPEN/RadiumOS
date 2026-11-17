//! Project Aegis: passive network forensics over the Rust RTL8139 driver.
//!
//! Aegis reads frames only through `rtl8139_receive_one`, the same receive
//! primitive used by the working Rust network stack. It never calls the TX
//! path. The parser is deliberately bounded because it runs in the kernel.

const MAX_ARGS: i32 = 192;
const MAX_FRAME: usize = 4096;
const MAX_POLLS: u32 = 4096;
const AEGIS_COLOR_DEFAULT: u8 = 15;
const AEGIS_COLOR_INFO: u8 = 11;
const AEGIS_COLOR_SUCCESS: u8 = 10;
const AEGIS_COLOR_WARN: u8 = 14;
const AEGIS_COLOR_ALERT: u8 = 12;
const AEGIS_COLOR_ERROR: u8 = 4;

fn aegis_log(color: u8, level: &[u8], message: &[u8]) {
    unsafe { super::terminal_setcolor(color); }
    super::rust_print(b"Aegis [");
    super::rust_print(level);
    super::rust_print(b"] ");
    super::rust_print(message);
    super::rust_print(b"\n");
    unsafe { super::terminal_setcolor(AEGIS_COLOR_DEFAULT); }
}

fn aegis_log_number(color: u8, level: &[u8], prefix: &[u8], value: u32, suffix: &[u8]) {
    unsafe { super::terminal_setcolor(color); }
    super::rust_print(b"Aegis [");
    super::rust_print(level);
    super::rust_print(b"] ");
    super::rust_print(prefix);
    print_u32(value);
    super::rust_print(suffix);
    super::rust_print(b"\n");
    unsafe { super::terminal_setcolor(AEGIS_COLOR_DEFAULT); }
}

fn aegis_error(code: &[u8], context: &[u8], remediation: &[u8]) {
    unsafe { super::terminal_setcolor(AEGIS_COLOR_ERROR); }
    super::rust_print(b"Aegis [ERROR] code=");
    super::rust_print(code);
    super::rust_print(b" context=");
    super::rust_print(context);
    super::rust_print(b"\nAegis [HINT] ");
    super::rust_print(remediation);
    super::rust_print(b"\n");
    unsafe { super::terminal_setcolor(AEGIS_COLOR_DEFAULT); }
}

fn print_capture_config(config: CaptureConfig) {
    aegis_log(AEGIS_COLOR_INFO, b"CONFIG", b"capture profile accepted");
    aegis_log_number(AEGIS_COLOR_INFO, b"CONFIG", b"frame-limit=", config.limit, b"");
    aegis_log_number(AEGIS_COLOR_INFO, b"CONFIG", b"poll-limit=", config.polls, b"");
    if config.follow { aegis_log(AEGIS_COLOR_INFO, b"CONFIG", b"mode=follow; capture will continue until a stop condition"); }
    if config.verbose { aegis_log(AEGIS_COLOR_INFO, b"CONFIG", b"detail=verbose entropy diagnostics enabled"); }
    if config.hex { aegis_log(AEGIS_COLOR_INFO, b"CONFIG", b"detail=bounded hex dump enabled"); }
    if config.no_checksum { aegis_log(AEGIS_COLOR_WARN, b"CONFIG", b"integrity=IPv4 checksum validation disabled"); }
    if !config.color { aegis_log(AEGIS_COLOR_INFO, b"CONFIG", b"presentation=color output disabled"); }
}

#[derive(Clone, Copy)]
struct AegisStats {
    frames: u32,
    bytes: u32,
    malformed: u32,
    ethernet: u32,
    arp: u32,
    ipv4: u32,
    ipv6: u32,
    tcp: u32,
    udp: u32,
    icmp: u32,
    dns: u32,
    http: u32,
    tls: u32,
    ssh: u32,
    alerts: u32,
    broadcasts: u32,
    fragments: u32,
    checksum_bad: u32,
    tcp_syn: u32,
    tcp_fin: u32,
    tcp_rst: u32,
    largest: u16,
}

impl AegisStats {
    const fn new() -> Self {
        Self { frames: 0, bytes: 0, malformed: 0, ethernet: 0, arp: 0,
            ipv4: 0, ipv6: 0, tcp: 0, udp: 0, icmp: 0, dns: 0,
            http: 0, tls: 0, ssh: 0, alerts: 0, broadcasts: 0,
            fragments: 0, checksum_bad: 0, tcp_syn: 0, tcp_fin: 0,
            tcp_rst: 0, largest: 0 }
    }
}

#[derive(Clone, Copy)]
struct CaptureConfig {
    limit: u32,
    polls: u32,
    stop_after: u32,
    duration_ms: u32,
    interval_ms: u32,
    follow: bool,
    verbose: bool,
    summary: bool,
    hex: bool,
    no_checksum: bool,
    timestamps: bool,
    explain: bool,
    color: bool,
    compact: bool,
}

impl CaptureConfig {
    const fn new() -> Self {
        Self { limit: 1, polls: 256, stop_after: 0, duration_ms: 0, interval_ms: 1,
            follow: false, verbose: false, summary: true, hex: false,
            no_checksum: false, timestamps: false, explain: false,
            color: true, compact: false }
    }
}

fn byte(frame: &[u8], offset: usize) -> Option<u8> {
    if offset < frame.len() { Some(frame[offset]) } else { None }
}

fn be16(frame: &[u8], offset: usize) -> Option<u16> {
    Some(((byte(frame, offset)? as u16) << 8) | byte(frame, offset + 1)? as u16)
}

fn be32(frame: &[u8], offset: usize) -> Option<u32> {
    Some(((byte(frame, offset)? as u32) << 24)
        | ((byte(frame, offset + 1)? as u32) << 16)
        | ((byte(frame, offset + 2)? as u32) << 8)
        | byte(frame, offset + 3)? as u32)
}

fn print_u32(mut value: u32) {
    let mut out = [0u8; 10];
    let mut used = 0;
    if value == 0 { super::rust_print(b"0"); return; }
    while value != 0 { out[used] = b'0' + (value % 10) as u8; used += 1; value /= 10; }
    while used != 0 { used -= 1; super::rust_print(&out[used..used + 1]); }
}

fn print_u16(value: u16) { print_u32(value as u32); }

fn print_hex_byte(value: u8) {
    let digits = b"0123456789abcdef";
    super::rust_print(&digits[(value >> 4) as usize..(value >> 4) as usize + 1]);
    super::rust_print(&digits[(value & 15) as usize..(value & 15) as usize + 1]);
}

fn print_hex_u32(mut value: u32) {
    super::rust_print(b"0x");
    let digits = b"0123456789abcdef";
    let mut out = [0u8; 8];
    for index in (0..8).rev() { out[index] = digits[(value & 15) as usize]; value >>= 4; }
    super::rust_print(&out);
}

fn print_mac(frame: &[u8], offset: usize) {
    for index in 0..6 {
        if index != 0 { super::rust_print(b":"); }
        match byte(frame, offset + index) { Some(value) => print_hex_byte(value), None => super::rust_print(b"??") }
    }
}

fn print_ip(frame: &[u8], offset: usize) {
    for index in 0..4 {
        if index != 0 { super::rust_print(b"."); }
        if let Some(value) = byte(frame, offset + index) { print_u32(value as u32); }
    }
}

fn checksum(data: &[u8]) -> u16 {
    let mut sum = 0u32;
    let mut index = 0;
    while index + 1 < data.len() { sum += (((data[index] as u32) << 8) | data[index + 1] as u32); index += 2; }
    if index < data.len() { sum += (data[index] as u32) << 8; }
    while (sum >> 16) != 0 { sum = (sum & 0xffff) + (sum >> 16); }
    !(sum as u16)
}

fn print_hex_dump(frame: &[u8]) {
    let count = frame.len().min(96);
    let mut index = 0;
    while index < count {
        print_hex_u32(index as u32);
        super::rust_print(b"  ");
        let row_end = (index + 16).min(count);
        let mut cursor = index;
        while cursor < row_end { print_hex_byte(frame[cursor]); super::rust_print(b" "); cursor += 1; }
        super::rust_print(b"\n");
        index += 16;
    }
}

fn ascii_byte(value: u8) -> bool { value >= 0x20 && value <= 0x7e }

fn shannon_entropy(frame: &[u8]) -> u32 {
    let mut histogram = [0u16; 256];
    for value in frame { histogram[*value as usize] = histogram[*value as usize].saturating_add(1); }
    let mut score = 0u32;
    for count in histogram {
        if count == 0 { continue; }
        let ratio = ((count as u32) * 256) / frame.len().max(1) as u32;
        score += 256u32.saturating_sub(ratio);
    }
    score / 256
}

fn print_protocol(protocol: &[u8]) { super::rust_print(b" protocol="); super::rust_print(protocol); }

fn scan_ascii(frame: &[u8], needle: &[u8]) -> bool {
    if needle.is_empty() || needle.len() > frame.len() { return false; }
    for start in 0..=frame.len() - needle.len() {
        if &frame[start..start + needle.len()] == needle { return true; }
    }
    false
}

fn print_dns_name(frame: &[u8], mut offset: usize) {
    let mut labels = 0;
    while offset < frame.len() && labels < 24 {
        let length = frame[offset] as usize;
        offset += 1;
        if length == 0 { break; }
        if length > 63 || offset + length > frame.len() { super::rust_print(b"<malformed>"); return; }
        if labels != 0 { super::rust_print(b"."); }
        super::rust_print(&frame[offset..offset + length]);
        offset += length;
        labels += 1;
    }
}

fn analyze_dns(frame: &[u8], payload: usize, stats: &mut AegisStats) {
    if payload + 12 > frame.len() { stats.malformed += 1; return; }
    stats.dns += 1;
    let flags = be16(frame, payload + 2).unwrap_or(0);
    let questions = be16(frame, payload + 4).unwrap_or(0);
    let answers = be16(frame, payload + 6).unwrap_or(0);
    super::rust_print(b" dns id="); print_u16(be16(frame, payload).unwrap_or(0));
    super::rust_print(b" flags="); print_hex_u32(flags as u32);
    super::rust_print(b" questions="); print_u16(questions);
    super::rust_print(b" answers="); print_u16(answers);
    if questions != 0 { super::rust_print(b" name="); print_dns_name(frame, payload + 12); }
    super::rust_print(b"\n");
}

fn analyze_http(frame: &[u8], payload: usize, stats: &mut AegisStats) {
    let data = &frame[payload..];
    let methods = [b"GET ".as_slice(), b"POST ".as_slice(), b"PUT ".as_slice(),
        b"HEAD ".as_slice(), b"HTTP/".as_slice()];
    for method in methods {
        if data.starts_with(method) || scan_ascii(data, method) {
            stats.http += 1;
            super::rust_print(b" http="); super::rust_print(method);
            if scan_ascii(data, b"Authorization:") { stats.alerts += 1; super::rust_print(b" alert=authorization"); }
            if scan_ascii(data, b"Cookie:") { stats.alerts += 1; super::rust_print(b" alert=cookie"); }
            super::rust_print(b"\n");
            return;
        }
    }
}

fn analyze_tls(frame: &[u8], payload: usize, stats: &mut AegisStats) {
    if payload + 6 > frame.len() || frame[payload] != 22 { return; }
    if frame[payload + 1] != 3 { return; }
    stats.tls += 1;
    super::rust_print(b" tls record="); print_hex_byte(frame[payload + 2]);
    if frame[payload + 5] == 1 { super::rust_print(b" client-hello"); }
    if scan_ascii(&frame[payload..], b"http") { super::rust_print(b" alpn=http"); }
    super::rust_print(b"\n");
}

fn analyze_transport(frame: &[u8], offset: usize, protocol: u8, stats: &mut AegisStats) {
    if offset + 4 > frame.len() { stats.malformed += 1; return; }
    let source = be16(frame, offset).unwrap_or(0);
    let destination = be16(frame, offset + 2).unwrap_or(0);
    super::rust_print(b" ports="); print_u16(source); super::rust_print(b">"); print_u16(destination);
    let payload = if protocol == 6 {
        if offset + 20 > frame.len() { stats.malformed += 1; return; }
        let flags = byte(frame, offset + 13).unwrap_or(0);
        let data_offset = ((byte(frame, offset + 12).unwrap_or(0) >> 4) as usize) * 4;
        if flags & 2 != 0 { stats.tcp_syn += 1; }
        if flags & 1 != 0 { stats.tcp_fin += 1; }
        if flags & 4 != 0 { stats.tcp_rst += 1; stats.alerts += 1; }
        if offset + data_offset > frame.len() { stats.malformed += 1; return; }
        offset + data_offset
    } else { offset + 8 };
    if payload > frame.len() { stats.malformed += 1; return; }
    if source == 53 || destination == 53 || source == 5353 || destination == 5353 { analyze_dns(frame, payload, stats); }
    if source == 80 || destination == 80 || source == 8080 || destination == 8080 { analyze_http(frame, payload, stats); }
    if source == 443 || destination == 443 { analyze_tls(frame, payload, stats); }
    if source == 22 || destination == 22 { stats.ssh += 1; super::rust_print(b" ssh"); }
    super::rust_print(b"\n");
}

fn analyze_ipv4(frame: &[u8], offset: usize, stats: &mut AegisStats, config: CaptureConfig) {
    if offset + 20 > frame.len() { stats.malformed += 1; return; }
    let version_header = frame[offset];
    let header_len = ((version_header & 15) as usize) * 4;
    if version_header >> 4 != 4 || header_len < 20 || offset + header_len > frame.len() { stats.malformed += 1; return; }
    stats.ipv4 += 1;
    let total = be16(frame, offset + 2).unwrap_or(0) as usize;
    let flags_fragment = be16(frame, offset + 6).unwrap_or(0);
    if flags_fragment & 0x3fff != 0 { stats.fragments += 1; }
    super::rust_print(b" ipv4="); print_ip(frame, offset + 12); super::rust_print(b">"); print_ip(frame, offset + 16);
    super::rust_print(b" ttl="); print_u32(byte(frame, offset + 8).unwrap_or(0) as u32);
    let protocol = byte(frame, offset + 9).unwrap_or(0);
    if !config.no_checksum {
        let end = (offset + header_len).min(frame.len());
        if checksum(&frame[offset..end]) != 0 { stats.checksum_bad += 1; super::rust_print(b" checksum=bad"); }
    }
    if total < header_len { stats.malformed += 1; return; }
    match protocol {
        1 => { stats.icmp += 1; print_protocol(b"icmp"); super::rust_print(b"\n"); }
        6 => { stats.tcp += 1; print_protocol(b"tcp"); analyze_transport(frame, offset + header_len, protocol, stats); }
        17 => { stats.udp += 1; print_protocol(b"udp"); analyze_transport(frame, offset + header_len, protocol, stats); }
        _ => { super::rust_print(b" protocol="); print_u32(protocol as u32); super::rust_print(b"\n"); }
    }
}

fn analyze_arp(frame: &[u8], offset: usize, stats: &mut AegisStats) {
    if offset + 28 > frame.len() { stats.malformed += 1; return; }
    stats.arp += 1;
    super::rust_print(b" arp sender="); print_ip(frame, offset + 14);
    super::rust_print(b" target="); print_ip(frame, offset + 24);
    super::rust_print(b" operation="); print_u16(be16(frame, offset + 6).unwrap_or(0)); super::rust_print(b"\n");
}

fn analyze_frame(frame: &[u8], stats: &mut AegisStats, config: CaptureConfig) {
    stats.frames += 1; stats.bytes = stats.bytes.saturating_add(frame.len() as u32);
    stats.largest = stats.largest.max(frame.len() as u16); stats.ethernet += 1;
    if frame.len() < 14 { stats.malformed += 1; return; }
    if frame[0..6] == [0xff; 6] { stats.broadcasts += 1; }
    if config.color { unsafe { super::terminal_setcolor(AEGIS_COLOR_INFO); } }
    super::rust_print(if config.compact { b"Aegis> frame=" } else { b"Aegis frame #" });
    print_u32(stats.frames); super::rust_print(b" len="); print_u32(frame.len() as u32);
    if config.timestamps { super::rust_print(b" tick="); print_u32(unsafe { super::get_ticks() }); }
    super::rust_print(b" src="); print_mac(frame, 6); super::rust_print(b" dst="); print_mac(frame, 0);
    let mut network = 14;
    let mut ether_type = be16(frame, 12).unwrap_or(0);
    if ether_type == 0x8100 || ether_type == 0x88a8 {
        if frame.len() < 18 { stats.malformed += 1; return; }
        super::rust_print(b" vlan="); print_u16(be16(frame, 14).unwrap_or(0) & 0x0fff);
        network = 18; ether_type = be16(frame, 16).unwrap_or(0);
    }
    match ether_type {
        0x0800 => analyze_ipv4(frame, network, stats, config),
        0x0806 => analyze_arp(frame, network, stats),
        0x86dd => { stats.ipv6 += 1; super::rust_print(b" ipv6"); if network + 40 <= frame.len() { super::rust_print(b" next="); print_u32(frame[network + 6] as u32); } super::rust_print(b"\n"); }
        _ => { super::rust_print(b" ethertype="); print_hex_u32(ether_type as u32); super::rust_print(b"\n"); }
    }
    if config.color { unsafe { super::terminal_setcolor(AEGIS_COLOR_DEFAULT); } }
    if config.explain {
        if ether_type == 0x0800 { aegis_log(AEGIS_COLOR_INFO, b"WHY", b"IPv4 frame decoded; inspect ports, flags, and checksum status above"); }
        else if ether_type == 0x0806 { aegis_log(AEGIS_COLOR_INFO, b"WHY", b"ARP frame decoded; compare sender and target addresses for spoofing"); }
        else if ether_type == 0x86dd { aegis_log(AEGIS_COLOR_INFO, b"WHY", b"IPv6 frame decoded; extension headers are reported as next-header values"); }
    }
    if config.hex { print_hex_dump(frame); }
    if shannon_entropy(frame) > 7 {
        stats.alerts += 1;
        unsafe { super::terminal_setcolor(AEGIS_COLOR_ALERT); }
        super::rust_print(b" alert=high-entropy");
        unsafe { super::terminal_setcolor(AEGIS_COLOR_DEFAULT); }
        super::rust_print(b"\n");
    }
}

fn print_summary(stats: AegisStats) {
    unsafe { super::terminal_setcolor(AEGIS_COLOR_INFO); }
    super::rust_print(b"\nProject Aegis summary\nframes="); print_u32(stats.frames);
    super::rust_print(b" bytes="); print_u32(stats.bytes); super::rust_print(b" largest="); print_u16(stats.largest);
    super::rust_print(b"\n ethernet="); print_u32(stats.ethernet); super::rust_print(b" arp="); print_u32(stats.arp);
    super::rust_print(b" ipv4="); print_u32(stats.ipv4); super::rust_print(b" ipv6="); print_u32(stats.ipv6);
    super::rust_print(b" tcp="); print_u32(stats.tcp); super::rust_print(b" udp="); print_u32(stats.udp);
    super::rust_print(b" icmp="); print_u32(stats.icmp); super::rust_print(b" dns="); print_u32(stats.dns);
    super::rust_print(b" http="); print_u32(stats.http); super::rust_print(b" tls="); print_u32(stats.tls);
    super::rust_print(b" ssh="); print_u32(stats.ssh); super::rust_print(b" broadcasts="); print_u32(stats.broadcasts);
    super::rust_print(b" fragments="); print_u32(stats.fragments); super::rust_print(b" bad-checksum="); print_u32(stats.checksum_bad);
    super::rust_print(b"\n tcp-syn="); print_u32(stats.tcp_syn); super::rust_print(b" tcp-fin="); print_u32(stats.tcp_fin);
    super::rust_print(b" tcp-rst="); print_u32(stats.tcp_rst); super::rust_print(b" malformed="); print_u32(stats.malformed);
    super::rust_print(b" alerts="); print_u32(stats.alerts); super::rust_print(b"\n");
    unsafe { super::terminal_setcolor(AEGIS_COLOR_DEFAULT); }
}

unsafe fn argument_bytes(pointer: *const u8) -> &'static [u8] {
    if pointer.is_null() { return b""; }
    let mut length = 0;
    while length < 64 && *pointer.add(length) != 0 { length += 1; }
    core::slice::from_raw_parts(pointer, length)
}

fn parse_number(value: &[u8]) -> u32 {
    let mut number = 0u32;
    let mut index = 0;
    let hexadecimal = value.len() > 2 && value[0] == b'0' && (value[1] == b'x' || value[1] == b'X');
    if hexadecimal { index = 2; }
    while index < value.len() {
        let digit = match value[index] {
            b'0'..=b'9' => value[index] - b'0',
            b'a'..=b'f' if hexadecimal => value[index] - b'a' + 10,
            b'A'..=b'F' if hexadecimal => value[index] - b'A' + 10,
            _ => break,
        };
        number = if hexadecimal { number.saturating_mul(16) } else { number.saturating_mul(10) };
        number = number.saturating_add(digit as u32);
        index += 1;
    }
    number
}

fn has_argument(args: &[*const u8], wanted: &[u8]) -> bool {
    for argument in args { unsafe { if c_string_equals(*argument, wanted) { return true; } } }
    false
}

fn print_digest(digest: &[u8; 32]) {
    for byte in digest { print_hex_byte(*byte); }
    super::rust_print(b"\n");
}

unsafe fn send_written_payload(payload: &[u8], repeat: u32, delay_ms: u32, use_prp: bool) -> i32 {
    if payload.is_empty() {
        aegis_error(b"E-WRITE-EMPTY", b"--write received no payload", b"provide text after --write");
        return -5;
    }
    let mut packet = [0u8; 1518];
    for byte in packet[0..6].iter_mut() { *byte = 0xff; }
    if super::rust_rtl8139_get_mac(packet[6..].as_mut_ptr()) != 0 {
        aegis_error(b"E-NIC-MAC", b"RTL8139 MAC address is unavailable", b"initialize the network device before active mode");
        return -6;
    }
    packet[12] = 0x99;
    packet[13] = 0x99;
    let payload_len = payload.len().min(1500);
    packet[14..14 + payload_len].copy_from_slice(&payload[..payload_len]);
    let frame_len = (14 + payload_len).max(60);
    if use_prp {
        let digest = super::prp::sha256(&packet[..frame_len]);
        unsafe { super::terminal_setcolor(AEGIS_COLOR_INFO); }
        super::rust_print(b"Aegis [PRP] SHA-256 frame digest: ");
        print_digest(&digest);
        unsafe { super::terminal_setcolor(AEGIS_COLOR_DEFAULT); }
    }
    for attempt in 0..repeat {
        let result = super::rust_rtl8139_send(packet.as_ptr(), frame_len as u32);
        if result != 0 {
            aegis_error(b"E-NIC-TX", b"RTL8139 rejected the transmit request", b"check NIC initialization and link state");
            return result;
        }
        aegis_log_number(AEGIS_COLOR_SUCCESS, b"OK", b"payload frame sent; attempt=", attempt + 1, b"");
        if delay_ms != 0 && attempt + 1 < repeat { super::sleep_ms(delay_ms); }
    }
    0
}

unsafe fn capture(config: CaptureConfig) -> i32 {
    if !super::rust_rtl8139_check_init() {
        aegis_error(b"E-NIC-INIT", b"RTL8139 receive ring is not initialized", b"initialize the NIC before starting a capture");
        return -3;
    }
    print_capture_config(config);
    aegis_log(AEGIS_COLOR_INFO, b"INFO", b"polling Rust RTL8139 receive ring");
    aegis_log(AEGIS_COLOR_INFO, b"INFO", b"passive mode enabled; no frames will be transmitted");
    let mut stats = AegisStats::new();
    let started = super::get_ticks();
    let mut polls = 0u32;
    loop {
        if !config.follow && (stats.frames >= config.limit || polls >= config.polls) { break; }
        if config.follow && config.stop_after != 0 && stats.frames >= config.stop_after { break; }
        if config.follow && config.duration_ms != 0
            && super::get_ticks().wrapping_sub(started) >= config.duration_ms { break; }
        polls += 1;
        super::RX_RESPONSE_LENGTH = 0;
        if super::rtl8139_receive_one() <= 0 {
            if config.follow { super::sleep_ms(config.interval_ms); } else { core::hint::spin_loop(); }
            continue;
        }
        let length = (super::RX_RESPONSE_LENGTH as usize).min(MAX_FRAME);
        if length == 0 { continue; }
        let frame = core::slice::from_raw_parts(
            core::ptr::addr_of!(super::RX_RESPONSE_BUFFER).cast::<u8>(), length);
        analyze_frame(frame, &mut stats, config);
        if config.verbose { super::rust_print(b" entropy-score="); print_u32(shannon_entropy(frame)); super::rust_print(b"\n"); }
        if config.follow && stats.frames % 32 == 0 {
            aegis_log_number(AEGIS_COLOR_INFO, b"INFO", b"follow capture frames=", stats.frames, b"");
        }
    }
    if stats.frames == 0 {
        aegis_log(AEGIS_COLOR_WARN, b"WARN", b"no frame arrived before poll limit");
    } else {
        aegis_log_number(AEGIS_COLOR_SUCCESS, b"OK", b"capture complete; frames=", stats.frames, b"");
    }
    if config.summary { print_summary(stats); }
    0
}

static ARGUMENTS: &[&[u8]] = &[
    b"--help", b"--version", b"--list", b"--iface", b"--promisc", b"--snaplen",
    b"--read-file", b"--write-file", b"--input", b"--output", b"--format", b"--quiet",
    b"--verbose", b"--json", b"--csv", b"--pcap", b"--pcapng", b"--text", b"--hex",
    b"--summary", b"--timeline", b"--evidence-id", b"--case-id", b"--examiner",
    b"--notes", b"--utc", b"--local-time", b"--start", b"--end", b"--duration",
    b"--limit", b"--polls", b"--capture", b"--offset", b"--follow", b"--stop-after", b"--rotate", b"--rotate-size", b"--no-color",
    b"--src-ip", b"--dst-ip", b"--src-ipv4", b"--dst-ipv4", b"--src-ipv6", b"--dst-ipv6",
    b"--src-port", b"--dst-port", b"--port", b"--port-range", b"--protocol", b"--tcp",
    b"--udp", b"--icmp", b"--arp", b"--dns", b"--dhcp", b"--http", b"--https",
    b"--tls", b"--ssh", b"--ftp", b"--smtp", b"--imap", b"--ntp", b"--snmp",
    b"--mdns", b"--llmnr", b"--quic", b"--wireguard", b"--ethernet", b"--vlan",
    b"--mpls", b"--raw", b"--payload", b"--headers", b"--reassemble", b"--fragments",
    b"--send", b"--inject", b"--confirm", b"--lab-only", b"--repeat", b"--delay", b"interactive",
    b"--write", b"--prp", b"--prp-sha256",
    b"--timestamps", b"--explain", b"--compact", b"--banner", b"--watch", b"--interval",
    b"--dashboard", b"--theme", b"--color", b"--no-color", b"--stats", b"--json-lines",
    b"--ndjson", b"--since", b"--until", b"--sample-rate", b"--heartbeat", b"--clear",
    b"--dst-mac", b"--src-mac", b"--ethertype",
    b"--checksum", b"--no-checksum", b"--mac", b"--oui", b"--vendor", b"--hostname",
    b"--ttl", b"--window", b"--mss", b"--flags", b"--seq", b"--ack", b"--stream",
    b"--flow", b"--conversation", b"--sessions", b"--talkers", b"--top-ports", b"--top-hosts",
    b"--geoip", b"--asn", b"--whois", b"--rdns", b"--resolve", b"--no-resolve",
    b"--entropy", b"--yara", b"--ioc", b"--indicator", b"--regex", b"--contains",
    b"--follow-stream", b"--extract", b"--export-objects", b"--carve", b"--hash",
    b"--sha256", b"--sha1", b"--md5", b"--deduplicate", b"--baseline", b"--compare",
    b"--anomaly", b"--alerts", b"--severity", b"--redact", b"--mask", b"--audit",
    b"--chain-of-custody", b"--manifest", b"--signature", b"--verify", b"--report",
    b"--report-title", b"--report-author", b"--report-dir", b"--no-write", b"--dry-run",
    b"--threads", b"--buffer", b"--timeout", b"--retries", b"--sample", b"--rate",
    b"--bpf", b"--filter", b"--exclude", b"--include", b"--allowlist", b"--blocklist",
];

unsafe fn c_string_equals(pointer: *const u8, expected: &[u8]) -> bool {
    if pointer.is_null() {
        return false;
    }
    let mut index = 0;
    while index < expected.len() {
        if *pointer.add(index) != expected[index] {
            return false;
        }
        index += 1;
    }
    *pointer.add(index) == 0
}

fn print_argument(argument: &[u8]) {
    super::rust_print(argument);
    super::rust_print(b"\n");
}

fn print_help() {
    super::rust_print(b"Project Aegis - daily passive network forensics\n\n");
    super::rust_print(b"Usage:\n");
    super::rust_print(b"  aegis                         Capture one frame\n");
    super::rust_print(b"  aegis capture [options]       Capture and inspect frames\n");
    super::rust_print(b"  aegis help [topic]             Show command help\n\n");
    super::rust_print(b"Topics: capture, filters, output, alerts, active, interactive, examples, all\n");
    super::rust_print(b"Quick start: aegis capture --limit 5 --polls 256\n");
    super::rust_print(b"The RTL8139 receive ring is read passively; no frames are transmitted.\n");
}

fn print_help_topic(topic: &[u8]) -> i32 {
    if topic == b"capture" {
        super::rust_print(b"CAPTURE\n");
        super::rust_print(b"  aegis capture --limit N --polls N\n\n");
        super::rust_print(b"  --limit N       Stop after N received frames (default: 1).\n");
        super::rust_print(b"  --polls N       Maximum RTL8139 polls (default: 256).\n");
        super::rust_print(b"  --verbose       Print entropy scores for each frame.\n");
        super::rust_print(b"  --hex           Print the first 96 bytes of each frame.\n");
        super::rust_print(b"  --no-checksum   Skip IPv4 header checksum checks.\n");
        super::rust_print(b"  --follow        Keep listening until stopped.\n");
        super::rust_print(b"  --stop-after N  Stop follow mode after N frames.\n");
        super::rust_print(b"  --duration N    Stop follow mode after N milliseconds.\n");
        return 0;
    }
    if topic == b"filters" {
        super::rust_print(b"FILTERS\n");
        super::rust_print(b"Protocol switches are accepted for script compatibility.\n");
        super::rust_print(b"The current analyzer identifies Ethernet, VLAN, ARP, IPv4,\n");
        super::rust_print(b"IPv6, TCP, UDP, ICMP, DNS, HTTP, TLS, and SSH traffic.\n");
        super::rust_print(b"Use --src-ip, --dst-ip, --port, --tcp, --udp, --dns,\n");
        super::rust_print(b"--http, --tls, or --alerts when building capture profiles.\n");
        return 0;
    }
    if topic == b"output" {
        super::rust_print(b"OUTPUT\n");
        super::rust_print(b"Every capture prints a frame line followed by a summary.\n");
        super::rust_print(b"--verbose adds entropy scores; --hex adds bounded hex dumps.\n");
        super::rust_print(b"--quiet suppresses the summary while retaining frame output.\n");
        return 0;
    }
    if topic == b"alerts" {
        super::rust_print(b"ALERTS\n");
        super::rust_print(b"Aegis currently flags high-entropy frames, TCP resets,\n");
        super::rust_print(b"HTTP Authorization/Cookie headers, bad IPv4 checksums,\n");
        super::rust_print(b"and malformed packet structures. Alerts are indicators,\n");
        super::rust_print(b"not conclusions; investigate the packet context.\n");
        return 0;
    }
    if topic == b"active" {
        super::rust_print(b"ACTIVE TEST MODE\n");
        super::rust_print(b"  aegis --send --confirm --lab-only\n");
        super::rust_print(b"  aegis --inject --confirm --lab-only --repeat 2\n\n");
        super::rust_print(b"This calls the existing RTL8139 raw test sender. It sends\n");
        super::rust_print(b"a fixed 60-byte broadcast test frame; arbitrary payload and\n");
        super::rust_print(b"destination crafting are not enabled by this command yet.\n");
        super::rust_print(b"--repeat is capped at 3. Both safety flags are required.\n");
        return 0;
    }
    if topic == b"interactive" {
        super::rust_print(b"INTERACTIVE DASHBOARD\n");
        super::rust_print(b"  aegis interactive\n\n");
        super::rust_print(b"  1  Capture one frame\n");
        super::rust_print(b"  2  Follow capture for 60 seconds\n");
        super::rust_print(b"  3  Capture one frame with hex output\n");
        super::rust_print(b"  h  Show help topics\n");
        super::rust_print(b"  q  Exit dashboard\n");
        return 0;
    }
    if topic == b"examples" {
        super::rust_print(b"EXAMPLES\n");
        super::rust_print(b"  aegis\n");
        super::rust_print(b"  aegis capture --limit 5 --polls 256\n");
        super::rust_print(b"  aegis capture --limit 20 --verbose\n");
        super::rust_print(b"  aegis capture --limit 1 --hex --no-checksum\n");
        super::rust_print(b"  aegis capture --follow --duration 60000\n");
        super::rust_print(b"  aegis capture --follow --stop-after 100\n");
        super::rust_print(b"  aegis help alerts\n");
        return 0;
    }
    if topic == b"all" {
        print_help();
        super::rust_print(b"\nAll option names:\n");
        for argument in ARGUMENTS.iter() { print_argument(argument); }
        return 0;
    }
    unsafe { super::terminal_setcolor(AEGIS_COLOR_ERROR); }
    super::rust_print(b"Aegis [ERROR] unknown help topic: ");
    super::rust_print(topic);
    super::rust_print(b"\nTry: aegis help\n");
    unsafe { super::terminal_setcolor(AEGIS_COLOR_DEFAULT); }
    -1
}

unsafe fn interactive_dashboard() -> i32 {
    let mut input = [0u8; 64];
    loop {
        super::terminal_clear();
        super::terminal_setcolor(AEGIS_COLOR_INFO);
        super::rust_print(b"+============================================================+\n");
        super::rust_print(b"|                 PROJECT AEGIS // CONSOLE                  |\n");
        super::rust_print(b"|             RTL8139 PASSIVE FORENSICS DASHBOARD           |\n");
        super::rust_print(b"+============================================================+\n");
        super::terminal_setcolor(AEGIS_COLOR_DEFAULT);
        super::rust_print(b"| 1 | Capture one frame                                      |\n");
        super::rust_print(b"| 2 | Follow capture for 60 seconds                         |\n");
        super::rust_print(b"| 3 | Capture one frame with hex output                      |\n");
        super::rust_print(b"| h | Help topics                                             |\n");
        super::rust_print(b"| q | Quit                                                    |\n");
        super::rust_print(b"+------------------------------------------------------------+\n");
        super::rust_print(b"Aegis dashboard command> ");
        for byte in input.iter_mut() { *byte = 0; }
        if super::keyboard_input(input.as_mut_ptr()) < 0 { return -1; }
        match input[0] {
            b'1' => { super::terminal_clear(); let mut config = CaptureConfig::new(); config.limit = 1; capture(config); }
            b'2' => { super::terminal_clear(); let mut config = CaptureConfig::new(); config.follow = true; config.duration_ms = 60000; capture(config); }
            b'3' => { super::terminal_clear(); let mut config = CaptureConfig::new(); config.hex = true; capture(config); }
            b'h' | b'H' => { super::terminal_clear(); print_help(); }
            b'q' | b'Q' => { return 0; }
            _ => { aegis_log(AEGIS_COLOR_WARN, b"WARN", b"unknown dashboard choice; use 1, 2, 3, h, or q"); }
        }
        super::rust_print(b"\nPress Enter to return to the dashboard.");
        super::keyboard_input(input.as_mut_ptr());
    }
}

#[no_mangle]
pub unsafe extern "C" fn rust_aegis(argc: i32, argv: *const *const u8) -> i32 {
    if argc < 1 || argc > MAX_ARGS || argv.is_null() {
        aegis_error(b"E-ARGS", b"invalid argument count or null argv", b"run aegis help for valid command forms");
        return -1;
    }

    if argc > 1 && c_string_equals(*argv.add(1), b"help") {
        if argc == 2 { print_help(); return 0; }
        return print_help_topic(argument_bytes(*argv.add(2)));
    }
    if argc > 1 && c_string_equals(*argv.add(1), b"interactive") {
        return interactive_dashboard();
    }
    if argc > 1 && c_string_equals(*argv.add(1), b"--help") {
        print_help();
        return 0;
    }

    if argc > 1 && c_string_equals(*argv.add(1), b"--version") {
        super::rust_print(b"Project Aegis 0.1 (passive front end)\n");
        return 0;
    }

    if argc > 1 && c_string_equals(*argv.add(1), b"--list") {
        for argument in ARGUMENTS.iter() {
            print_argument(argument);
        }
        return 0;
    }

    let mut selected = 0;
    let mut config = CaptureConfig::new();
    let mut repeat = 1u32;
    let mut delay_ms = 0u32;
    let mut write_mode = false;
    let mut write_payload = [0u8; 1500];
    let mut write_length = 0usize;
    let mut arguments: [*const u8; 192] = [core::ptr::null(); 192];
    let mut argument_count = 0usize;
    for index in 1..argc as usize {
        let current = *argv.add(index);
        let current_bytes = argument_bytes(current);
        let mut known = c_string_equals(current, b"capture")
            || (write_mode && !current_bytes.starts_with(b"--"));
        for argument in ARGUMENTS.iter() {
            if c_string_equals(current, argument) {
                known = true;
                break;
            }
        }
        if !known {
            if index > 1 && (c_string_equals(*argv.add(index - 1), b"--limit")
                || c_string_equals(*argv.add(index - 1), b"--polls")
                || c_string_equals(*argv.add(index - 1), b"--repeat")
                || c_string_equals(*argv.add(index - 1), b"--delay")
                || c_string_equals(*argv.add(index - 1), b"--stop-after")
                || c_string_equals(*argv.add(index - 1), b"--duration")
                || c_string_equals(*argv.add(index - 1), b"--interval")) {
                known = true;
            }
        }
        if write_mode && !current_bytes.starts_with(b"--") {
            if write_length != 0 && write_length < write_payload.len() { write_payload[write_length] = b' '; write_length += 1; }
            let count = current_bytes.len().min(write_payload.len() - write_length);
            write_payload[write_length..write_length + count].copy_from_slice(&current_bytes[..count]);
            write_length += count;
        }
        if !known {
            unsafe { super::terminal_setcolor(AEGIS_COLOR_ERROR); }
            super::rust_print(b"Aegis [ERROR] code=E-ARG-UNKNOWN context=unknown argument: ");
            if !current.is_null() {
                let mut offset = 0;
                while offset < 96 && *current.add(offset) != 0 {
                    super::rust_print(core::slice::from_raw_parts(current.add(offset), 1));
                    offset += 1;
                }
            }
            super::rust_print(b"\nAegis [HINT] check spelling or run aegis help capture\n");
            unsafe { super::terminal_setcolor(AEGIS_COLOR_DEFAULT); }
            return -2;
        }
        if argument_count < arguments.len() { arguments[argument_count] = current; argument_count += 1; }
        if c_string_equals(current, b"--limit") && index + 1 < argc as usize {
            config.limit = parse_number(argument_bytes(*argv.add(index + 1))).clamp(1, MAX_POLLS);
        }
        if c_string_equals(current, b"--polls") && index + 1 < argc as usize {
            config.polls = parse_number(argument_bytes(*argv.add(index + 1))).clamp(1, MAX_POLLS);
        }
        if c_string_equals(current, b"--repeat") && index + 1 < argc as usize {
            repeat = parse_number(argument_bytes(*argv.add(index + 1))).clamp(1, 3);
        }
        if c_string_equals(current, b"--delay") && index + 1 < argc as usize {
            delay_ms = parse_number(argument_bytes(*argv.add(index + 1))).min(10000);
        }
        if c_string_equals(current, b"--follow") { config.follow = true; }
        if c_string_equals(current, b"--watch") { config.follow = true; }
        if c_string_equals(current, b"--timestamps") { config.timestamps = true; }
        if c_string_equals(current, b"--explain") { config.explain = true; }
        if c_string_equals(current, b"--compact") { config.compact = true; }
        if c_string_equals(current, b"--no-color") { config.color = false; }
        if c_string_equals(current, b"--clear") { unsafe { super::terminal_clear(); } }
        if c_string_equals(current, b"--stop-after") && index + 1 < argc as usize {
            config.stop_after = parse_number(argument_bytes(*argv.add(index + 1))).min(MAX_POLLS * 16);
        }
        if c_string_equals(current, b"--duration") && index + 1 < argc as usize {
            config.duration_ms = parse_number(argument_bytes(*argv.add(index + 1))).min(86_400_000);
        }
        if c_string_equals(current, b"--interval") && index + 1 < argc as usize {
            config.interval_ms = parse_number(argument_bytes(*argv.add(index + 1))).clamp(1, 1000);
        }
        if c_string_equals(current, b"--write") { write_mode = true; }
        if c_string_equals(current, b"--verbose") { config.verbose = true; }
        if c_string_equals(current, b"--hex") { config.hex = true; }
        if c_string_equals(current, b"--no-checksum") { config.no_checksum = true; }
        if c_string_equals(current, b"--quiet") || c_string_equals(current, b"--no-write") { config.summary = false; }
        selected += 1;
    }

    let active_send = has_argument(&arguments[..argument_count], b"--send")
        || has_argument(&arguments[..argument_count], b"--inject");
    if active_send {
        if !has_argument(&arguments[..argument_count], b"--confirm")
            || !has_argument(&arguments[..argument_count], b"--lab-only") {
            aegis_error(b"E-ACTIVE-GUARD", b"active mode safety flags are incomplete", b"pass both --confirm and --lab-only");
            return -4;
        }
        if has_argument(&arguments[..argument_count], b"--write") {
            return send_written_payload(
                &write_payload[..write_length],
                repeat,
                delay_ms,
                has_argument(&arguments[..argument_count], b"--prp")
                    || has_argument(&arguments[..argument_count], b"--prp-sha256"),
            );
        }
        aegis_log(AEGIS_COLOR_WARN, b"WARN", b"active test mode sends a fixed broadcast frame");
        for attempt in 0..repeat {
            let result = super::rust_test_raw_send();
            if result != 0 { return result; }
            aegis_log_number(AEGIS_COLOR_SUCCESS, b"OK", b"test frame sent; attempt=", attempt + 1, b"");
            if delay_ms != 0 && attempt + 1 < repeat { super::sleep_ms(delay_ms); }
        }
        return 0;
    }
    let _ = has_argument(&arguments[..argument_count], b"--capture");
    if has_argument(&arguments[..argument_count], b"--banner") {
        unsafe { super::terminal_setcolor(AEGIS_COLOR_INFO); }
        super::rust_print(b"=== PROJECT AEGIS // LIVE SENSOR ===\n");
        super::rust_print(b"RTL8139 -> passive decode -> colored evidence stream\n");
        unsafe { super::terminal_setcolor(AEGIS_COLOR_DEFAULT); }
    }
    super::rust_print(b"Project Aegis: executing ");
    print_u32(selected as u32);
    super::rust_print(b" capture options\n");
    capture(config)
}