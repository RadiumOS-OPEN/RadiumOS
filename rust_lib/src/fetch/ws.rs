//! A wss:// websocket client (RFC 6455) on top of the fetch TLS and TCP code. One connection at a time: the TCP stack
//! has a single global socket, so while a websocket is open every other network request fails ("busy").
//! Nothing runs in the background: the caller must call rust_ws_recv often (keepalive pings are answered there).

use super::{
    command::{remember_error, BUSY},
    configuration, public_roots,
    native::{Rtc, Tcp},
    Error, Transport, Url,
};
use alloc::{format, string::String, sync::Arc, vec, vec::Vec};
use core::{slice, str, sync::atomic::Ordering};
use rustls::{
    client::UnbufferedClientConnection, pki_types::ServerName, time_provider::TimeProvider,
    unbuffered::ConnectionState,
};

const RECORD_BUFFER: usize = 18432;
const MAX_MESSAGE: usize = 256 * 1024;
const HANDSHAKE_MS: u32 = 15_000;
const GUID: &[u8] = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

struct TlsStream {
    tcp: Tcp,
    conn: UnbufferedClientConnection,
    incoming: Vec<u8>,
    used: usize,
    outgoing: Vec<u8>,
    written: usize,
    plain: Vec<u8>,
    pos: usize,
    closed: bool,
}

impl TlsStream {
    fn new(tcp: Tcp, host: &str) -> Result<Self, Error> {
        if Rtc.current_time().is_none() {
            return Err(Error::Message("RTC unavailable or invalid; cannot check certificate dates"));
        }
        let config = configuration(Arc::new(Rtc), public_roots())?;
        let name = ServerName::try_from(String::from(host)).map_err(|_| Error::Message("invalid server name"))?;
        Ok(Self {
            tcp,
            conn: UnbufferedClientConnection::new(config, name)?,
            incoming: vec![0u8; RECORD_BUFFER],
            used: 0,
            outgoing: vec![0u8; RECORD_BUFFER],
            written: 0,
            plain: Vec::new(),
            pos: 0,
            closed: false,
        })
    }

    /// Drives TLS: sends `send` as application data and moves received data into `plain`. With `handshake` it
    /// blocks until the handshake is done; otherwise it never waits for the network.
    fn run(&mut self, mut send: &[u8], handshake: bool) -> Result<(), Error> {
        let Self { tcp, conn, incoming, used, outgoing, written, plain, closed, .. } = self;
        loop {
            let status = conn.process_tls_records(&mut incoming[..*used]);
            let mut discard = status.discard;
            let mut need_read = false;
            match status.state? {
                ConnectionState::EncodeTlsData(mut state) => {
                    *written += state
                        .encode(&mut outgoing[*written..])
                        .map_err(|_| Error::Message("TLS output exceeds buffer"))?;
                }
                ConnectionState::TransmitTlsData(state) => {
                    tcp.touch();
                    tcp.write(&outgoing[..*written])?;
                    *written = 0;
                    state.done();
                }
                ConnectionState::WriteTraffic(mut state) => {
                    if !send.is_empty() {
                        let n = send.len().min(4000);
                        let len = state
                            .encrypt(&send[..n], &mut outgoing[..])
                            .map_err(|_| Error::Message("TLS record exceeds buffer"))?;
                        tcp.touch();
                        tcp.write(&outgoing[..len])?;
                        send = &send[n..];
                    } else if handshake {
                        return Ok(());
                    } else {
                        need_read = true;
                    }
                }
                ConnectionState::ReadTraffic(mut state) => {
                    while let Some(record) = state.next_record() {
                        let record = record?;
                        discard += record.discard;
                        if plain.len() + record.payload.len() > MAX_MESSAGE * 2 {
                            return Err(Error::Message("websocket receive buffer full"));
                        }
                        plain.extend_from_slice(record.payload);
                    }
                }
                ConnectionState::PeerClosed | ConnectionState::Closed => {
                    *closed = true;
                    return Ok(());
                }
                ConnectionState::BlockedHandshake => need_read = true,
                _ => return Err(Error::Message("unsupported TLS state")),
            }
            if discard > *used {
                return Err(Error::Message("invalid TLS buffer state"));
            }
            incoming.copy_within(discard..*used, 0);
            *used -= discard;
            if need_read {
                if *used == incoming.len() {
                    return Err(Error::Message("TLS record exceeds buffer"));
                }
                tcp.touch();
                let count = if handshake {
                    Some(tcp.read(&mut incoming[*used..])?)
                } else {
                    tcp.try_read(&mut incoming[*used..])?
                };
                match count {
                    None => return Ok(()),
                    Some(0) => {
                        if handshake {
                            return Err(Error::Message("connection closed during TLS handshake"));
                        }
                        *closed = true;
                        return Ok(());
                    }
                    Some(n) => *used += n,
                }
            }
        }
    }

    fn compact(&mut self) {
        if self.pos == self.plain.len() {
            self.plain.clear();
            self.pos = 0;
        } else if self.pos > 65536 {
            self.plain.drain(..self.pos);
            self.pos = 0;
        }
    }
}

struct Ws {
    tls: TlsStream,
    msg: Vec<u8>,
    msg_op: u8,
    sent_close: bool,
    close_code: u16,
}

impl Ws {
    fn send_frame(&mut self, op: u8, data: &[u8]) -> Result<(), Error> {
        let mut key = [0u8; 4];
        if !crate::prp::random_bytes(&mut key) {
            return Err(Error::Message("random source unavailable"));
        }
        let mut frame = Vec::with_capacity(data.len() + 14);
        frame.push(0x80 | op);
        match data.len() {
            0..=125 => frame.push(0x80 | data.len() as u8),
            126..=65535 => {
                frame.push(0x80 | 126);
                frame.extend_from_slice(&(data.len() as u16).to_be_bytes());
            }
            _ => {
                frame.push(0x80 | 127);
                frame.extend_from_slice(&(data.len() as u64).to_be_bytes());
            }
        }
        frame.extend_from_slice(&key);
        frame.extend(data.iter().enumerate().map(|(i, b)| b ^ key[i & 3]));
        self.tls.run(&frame, false)
    }

    fn send_close(&mut self, code: u16) {
        if !self.sent_close {
            self.sent_close = true;
            let _ = self.send_frame(8, &code.to_be_bytes());
        }
    }

    /// The next complete message already buffered, answering control frames on the way.
    fn next_message(&mut self) -> Result<Option<(u8, Vec<u8>)>, Error> {
        loop {
            let (op, fin, header, len) = {
                let buf = &self.tls.plain[self.tls.pos..];
                if buf.len() < 2 {
                    return Ok(None);
                }
                if buf[0] & 0x70 != 0 || buf[1] & 0x80 != 0 {
                    return Err(Error::Message("websocket protocol error"));
                }
                let (header, len) = match buf[1] & 0x7f {
                    126 => {
                        if buf.len() < 4 {
                            return Ok(None);
                        }
                        (4, u16::from_be_bytes([buf[2], buf[3]]) as usize)
                    }
                    127 => {
                        if buf.len() < 10 {
                            return Ok(None);
                        }
                        let mut raw = [0u8; 8];
                        raw.copy_from_slice(&buf[2..10]);
                        let len = u64::from_be_bytes(raw);
                        if len > MAX_MESSAGE as u64 {
                            return Err(Error::Message("websocket frame too large"));
                        }
                        (10, len as usize)
                    }
                    n => (2, n as usize),
                };
                if buf.len() < header + len {
                    return Ok(None);
                }
                (buf[0] & 0x0f, buf[0] & 0x80 != 0, header, len)
            };
            let start = self.tls.pos + header;
            let payload: Vec<u8> = self.tls.plain[start..start + len].to_vec();
            self.tls.pos = start + len;
            self.tls.compact();
            match op {
                0 | 1 | 2 => {
                    if op != 0 {
                        self.msg_op = op;
                        self.msg.clear();
                    } else if self.msg_op == 0 {
                        return Err(Error::Message("unexpected continuation frame"));
                    }
                    if self.msg.len() + payload.len() > MAX_MESSAGE {
                        self.send_close(1009);
                        return Err(Error::Message("websocket message too large"));
                    }
                    self.msg.extend_from_slice(&payload);
                    if fin {
                        let op = self.msg_op;
                        self.msg_op = 0;
                        return Ok(Some((op, core::mem::take(&mut self.msg))));
                    }
                }
                8 => {
                    self.close_code = if payload.len() >= 2 { u16::from_be_bytes([payload[0], payload[1]]) } else { 1005 };
                    self.send_close(self.close_code);
                    self.tls.closed = true;
                    return Err(Error::Message("websocket closed by server"));
                }
                9 if len <= 125 => self.send_frame(10, &payload)?,
                10 if len <= 125 => {}
                _ => return Err(Error::Message("websocket protocol error")),
            }
        }
    }

    fn recv(&mut self, timeout_ms: u32) -> Result<Option<(u8, Vec<u8>)>, Error> {
        let started = unsafe { crate::get_ticks() };
        let mut pumped = false;
        loop {
            if let Some(message) = self.next_message()? {
                return Ok(Some(message));
            }
            if self.tls.closed {
                return Err(Error::Message("websocket connection closed"));
            }
            if pumped && unsafe { crate::get_ticks() }.wrapping_sub(started) >= timeout_ms {
                return Ok(None);
            }
            self.tls.run(&[], false)?;
            pumped = true;
        }
    }
}

fn sha1(data: &[u8]) -> [u8; 20] {
    let mut h: [u32; 5] = [0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0];
    let mut msg = Vec::from(data);
    msg.push(0x80);
    while msg.len() % 64 != 56 {
        msg.push(0);
    }
    msg.extend_from_slice(&((data.len() as u64) * 8).to_be_bytes());
    for chunk in msg.chunks(64) {
        let mut w = [0u32; 80];
        for i in 0..16 {
            w[i] = u32::from_be_bytes([chunk[4 * i], chunk[4 * i + 1], chunk[4 * i + 2], chunk[4 * i + 3]]);
        }
        for i in 16..80 {
            w[i] = (w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16]).rotate_left(1);
        }
        let (mut a, mut b, mut c, mut d, mut e) = (h[0], h[1], h[2], h[3], h[4]);
        for (i, wi) in w.iter().enumerate() {
            let (f, k) = match i {
                0..=19 => ((b & c) | (!b & d), 0x5A827999u32),
                20..=39 => (b ^ c ^ d, 0x6ED9EBA1),
                40..=59 => ((b & c) | (b & d) | (c & d), 0x8F1BBCDC),
                _ => (b ^ c ^ d, 0xCA62C1D6),
            };
            let t = a.rotate_left(5).wrapping_add(f).wrapping_add(e).wrapping_add(k).wrapping_add(*wi);
            e = d;
            d = c;
            c = b.rotate_left(30);
            b = a;
            a = t;
        }
        for (slot, v) in h.iter_mut().zip([a, b, c, d, e]) {
            *slot = slot.wrapping_add(v);
        }
    }
    let mut out = [0u8; 20];
    for (i, v) in h.iter().enumerate() {
        out[4 * i..4 * i + 4].copy_from_slice(&v.to_be_bytes());
    }
    out
}

fn base64(input: &[u8]) -> String {
    const ALPHABET: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut out = String::new();
    for chunk in input.chunks(3) {
        let n = (chunk[0] as u32) << 16 | (*chunk.get(1).unwrap_or(&0) as u32) << 8 | *chunk.get(2).unwrap_or(&0) as u32;
        for i in 0..4 {
            if i <= chunk.len() {
                out.push(ALPHABET[(n >> (18 - 6 * i) & 63) as usize] as char);
            } else {
                out.push('=');
            }
        }
    }
    out
}

static mut WS: Option<Ws> = None;

fn slot() -> &'static mut Option<Ws> {
    unsafe { &mut *core::ptr::addr_of_mut!(WS) }
}

/// Interrupts on (the tick counter needs them) and the network quiet-flag set for the duration of one operation.
struct NetOp(u32);

impl NetOp {
    fn begin() -> Self {
        let flags: u32;
        unsafe {
            core::arch::asm!("pushfd", "pop {}", out(reg) flags);
            core::arch::asm!("sti");
        }
        crate::FETCH_NETWORK_SILENT.store(true, Ordering::Relaxed);
        Self(flags)
    }
}

impl Drop for NetOp {
    fn drop(&mut self) {
        if self.0 & (1 << 9) == 0 {
            unsafe { core::arch::asm!("cli") };
        }
        crate::FETCH_NETWORK_SILENT.store(false, Ordering::Relaxed);
    }
}

unsafe fn c_str<'a>(pointer: *const u8) -> Result<&'a str, Error> {
    if pointer.is_null() {
        return Err(Error::Message("null argument"));
    }
    for len in 0..4096 {
        if *pointer.add(len) == 0 {
            return str::from_utf8(slice::from_raw_parts(pointer, len)).map_err(|_| Error::Message("invalid UTF-8 argument"));
        }
    }
    Err(Error::Message("argument too long"))
}

fn open(url: &str, headers: &str) -> Result<Ws, Error> {
    let url = url.strip_prefix("wss://").ok_or(Error::Message("websocket URL must start with wss://"))?;
    let url = Url::parse(&format!("https://{url}"))?;
    let mut extra = String::new();
    for line in headers.split('\n') {
        let line = line.trim();
        if line.is_empty() {
            continue;
        }
        if line.bytes().any(|b| b < 32 || b == 127) || !line.contains(':') {
            return Err(Error::Message("invalid header line"));
        }
        extra.push_str(line);
        extra.push_str("\r\n");
    }
    let mut raw = [0u8; 16];
    if !crate::prp::random_bytes(&mut raw) {
        return Err(Error::Message("random source unavailable"));
    }
    let key = base64(&raw);
    let request = format!(
        "GET {} HTTP/1.1\r\nHost: {}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: {}\r\nSec-WebSocket-Version: 13\r\n{}\r\n",
        url.path,
        url.authority(),
        key,
        extra
    );

    let tcp = Tcp::connect(&url.host, url.port, 30_000)?;
    let mut tls = TlsStream::new(tcp, &url.host)?;
    tls.run(&[], true)?;
    tls.run(request.as_bytes(), false)?;

    let started = unsafe { crate::get_ticks() };
    let end = loop {
        if let Some(i) = tls.plain[tls.pos..].windows(4).position(|w| w == b"\r\n\r\n") {
            break tls.pos + i + 4;
        }
        if tls.plain.len() - tls.pos > 16384 {
            return Err(Error::Message("websocket handshake response too large"));
        }
        if tls.closed {
            return Err(Error::Message("connection closed during websocket handshake"));
        }
        if unsafe { crate::get_ticks() }.wrapping_sub(started) >= HANDSHAKE_MS {
            return Err(Error::Message("websocket handshake timed out"));
        }
        tls.run(&[], false)?;
    };
    let head = str::from_utf8(&tls.plain[tls.pos..end]).map_err(|_| Error::Message("invalid websocket handshake"))?;
    let mut lines = head.split("\r\n");
    let status = lines.next().unwrap_or("");
    if !status.starts_with("HTTP/1.1 101") {
        return Err(Error::Message("server refused the websocket upgrade"));
    }
    let mut seed = Vec::from(key.as_bytes());
    seed.extend_from_slice(GUID);
    let expected = base64(&sha1(&seed));
    let accepted = lines.any(|line| {
        line.split_once(':')
            .map_or(false, |(name, value)| name.eq_ignore_ascii_case("sec-websocket-accept") && value.trim() == expected)
    });
    if !accepted {
        return Err(Error::Message("bad Sec-WebSocket-Accept"));
    }
    tls.pos = end;
    tls.compact();
    Ok(Ws { tls, msg: Vec::new(), msg_op: 0, sent_close: false, close_code: 0 })
}

fn release() {
    *slot() = None;
    BUSY.store(false, Ordering::Release);
}

/// Opens a wss:// connection. `headers` are extra "Name: value" lines separated by \n. 0 on success, -1 on failure
/// (rust_http_error says why).
#[no_mangle]
pub unsafe extern "C" fn rust_ws_connect(url: *const u8, headers: *const u8) -> i32 {
    if BUSY.compare_exchange(false, true, Ordering::Acquire, Ordering::Relaxed).is_err() {
        remember_error("another network request is running");
        return -1;
    }
    let _op = NetOp::begin();
    let result = (|| {
        let headers = if headers.is_null() { "" } else { c_str(headers)? };
        open(c_str(url)?, headers)
    })();
    match result {
        Ok(ws) => {
            *slot() = Some(ws);
            0
        }
        Err(error) => {
            remember_error(&format!("{error}"));
            BUSY.store(false, Ordering::Release);
            -1
        }
    }
}

/// Sends one message (opcode 1 text, 2 binary). 0 on success, -1 on failure (the connection is then closed).
#[no_mangle]
pub unsafe extern "C" fn rust_ws_send(opcode: u32, data: *const u8, len: u32) -> i32 {
    let Some(ws) = slot().as_mut() else {
        remember_error("websocket is not connected");
        return -1;
    };
    if (opcode != 1 && opcode != 2) || (data.is_null() && len != 0) || len as usize > MAX_MESSAGE {
        remember_error("invalid websocket message");
        return -1;
    }
    let _op = NetOp::begin();
    let body: &[u8] = if len == 0 { &[] } else { slice::from_raw_parts(data, len as usize) };
    match ws.send_frame(opcode as u8, body) {
        Ok(()) => 0,
        Err(error) => {
            remember_error(&format!("{error}"));
            release();
            -1
        }
    }
}

/// Waits up to `timeout_ms` (0 = just check) for a message. Returns its length (opcode 1 or 2 in *opcode_out),
/// -1 when nothing arrived, -2 when the connection is closed (the connection is released; rust_ws_close_code has
/// the server's code) or failed, -3 when the message did not fit in `cap` (it is dropped).
#[no_mangle]
pub unsafe extern "C" fn rust_ws_recv(out: *mut u8, cap: u32, opcode_out: *mut u32, timeout_ms: u32) -> i32 {
    let Some(ws) = slot().as_mut() else {
        remember_error("websocket is not connected");
        return -2;
    };
    let _op = NetOp::begin();
    match ws.recv(timeout_ms) {
        Ok(None) => -1,
        Ok(Some((op, data))) => {
            if data.len() > cap as usize || out.is_null() {
                return -3;
            }
            core::ptr::copy_nonoverlapping(data.as_ptr(), out, data.len());
            if !opcode_out.is_null() {
                *opcode_out = op as u32;
            }
            data.len() as i32
        }
        Err(error) => {
            remember_error(&format!("{error}"));
            LAST_CLOSE_CODE = ws.close_code;
            release();
            -2
        }
    }
}

static mut LAST_CLOSE_CODE: u16 = 0;

/// The close code the server sent with its last close frame (0 if the connection ended some other way).
#[no_mangle]
pub unsafe extern "C" fn rust_ws_close_code() -> u32 {
    LAST_CLOSE_CODE as u32
}

#[no_mangle]
pub unsafe extern "C" fn rust_ws_is_open() -> i32 {
    slot().is_some() as i32
}

/// Sends a close frame with `code` (1000 = normal) and drops the connection.
#[no_mangle]
pub unsafe extern "C" fn rust_ws_close(code: u32) {
    if let Some(ws) = slot().as_mut() {
        let _op = NetOp::begin();
        ws.send_close(code as u16);
    }
    release();
}
