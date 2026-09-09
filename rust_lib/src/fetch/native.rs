use alloc::vec::Vec;
use core::time::Duration;
use rustls::pki_types::UnixTime;
use rustls::time_provider::TimeProvider;

use super::{Error, Transport};

pub(super) struct Tcp {
    pending: Vec<u8>,
    offset: usize,
    started: u32,
    timeout_ms: u32,
    acknowledged: u32,
    closed: bool,
}

impl Tcp {
    pub(super) fn connect(host: &str, port: u16, timeout_ms: u32) -> Result<Self, Error> {
        let started = unsafe { crate::get_ticks() };
        let ip = match host.parse::<core::net::Ipv4Addr>() {
            Ok(ip) => ip.octets(),
            Err(_) => resolve(host.as_bytes(), started, timeout_ms)?,
        };
        if unsafe { crate::get_ticks() }.wrapping_sub(started) >= timeout_ms {
            return Err(Error::Message("fetch timed out"));
        }
        let remaining =
            timeout_ms.saturating_sub(unsafe { crate::get_ticks() }.wrapping_sub(started));
        if !unsafe { crate::tcp_connect_timeout(&ip, port, remaining) } {
            unsafe { crate::tcp_close() };
            return Err(Error::Message("TCP connection failed"));
        }
        let stream = Self {
            pending: Vec::new(),
            offset: 0,
            started,
            timeout_ms,
            acknowledged: unsafe { crate::TCP_CONNECTION.seq_num },
            closed: false,
        };
        stream.check_deadline()?;
        Ok(stream)
    }

    fn check_deadline(&self) -> Result<(), Error> {
        if unsafe { crate::get_ticks() }.wrapping_sub(self.started) >= self.timeout_ms {
            return Err(Error::Message("fetch timed out"));
        }
        Ok(())
    }

    fn poll(&mut self) -> Result<(), Error> {
        self.check_deadline()?;
        unsafe {
            crate::RX_RESPONSE_LENGTH = 0;
            if crate::rtl8139_receive_one() <= 0 {
                core::hint::spin_loop();
                return Ok(());
            }
            let len = crate::RX_RESPONSE_LENGTH as usize;
            if len > 4096 {
                return Err(Error::Message("invalid receive frame length"));
            }
            let frame = core::slice::from_raw_parts(
                core::ptr::addr_of!(crate::RX_RESPONSE_BUFFER).cast::<u8>(),
                len,
            );
            let remote_ip = crate::TCP_CONNECTION.remote_ip;
            let local_ip = crate::LOCAL_IP;
            let packet = match tcp_packet(
                frame,
                &remote_ip,
                &local_ip,
                crate::TCP_CONNECTION.remote_port,
                crate::TCP_CONNECTION.local_port,
            ) {
                Some(packet) => packet,
                None => return Ok(()),
            };
            let seq = word32(&packet[4..]);
            let ack = word32(&packet[8..]);
            let flags = packet[13];
            let expected = crate::TCP_CONNECTION.ack_num;
            if flags & 4 != 0 {
                if seq == expected {
                    self.closed = true;
                    return Err(Error::Message("TCP connection reset"));
                }
                return Ok(());
            }
            if flags & 0x10 == 0 || flags & 2 != 0 {
                return Ok(());
            }
            if (ack.wrapping_sub(crate::TCP_CONNECTION.seq_num) as i32) > 0 {
                return Ok(());
            }
            if (ack.wrapping_sub(self.acknowledged) as i32) > 0 {
                self.acknowledged = ack;
            }
            let payload = &packet[((packet[12] >> 4) as usize) * 4..];
            let distance = seq.wrapping_sub(expected) as i32;
            if distance <= 0 && !self.closed {
                let skip = expected.wrapping_sub(seq) as usize;
                if skip < payload.len() {
                    let fresh = &payload[skip..];
                    if self.pending.len() - self.offset + fresh.len() > 65536 {
                        return Err(Error::Message("TCP receive buffer full"));
                    }
                    if self.offset != 0 {
                        self.pending.drain(..self.offset);
                        self.offset = 0;
                    }
                    self.pending
                        .try_reserve(fresh.len())
                        .map_err(|_| Error::Message("out of memory"))?;
                    self.pending.extend_from_slice(fresh);
                    crate::TCP_CONNECTION.ack_num = expected.wrapping_add(fresh.len() as u32);
                }
                if flags & 1 != 0
                    && seq.wrapping_add(payload.len() as u32) == crate::TCP_CONNECTION.ack_num
                {
                    crate::TCP_CONNECTION.ack_num = crate::TCP_CONNECTION.ack_num.wrapping_add(1);
                    self.closed = true;
                }
            }
            // A duplicate ACK requests retransmission without accepting a gap into the TLS stream.
            if !payload.is_empty() || flags & 1 != 0 {
                crate::send_ack_packet(
                    crate::TCP_CONNECTION.local_port,
                    crate::TCP_CONNECTION.remote_port,
                );
            }
        }
        Ok(())
    }
}

impl Transport for Tcp {
    fn read(&mut self, output: &mut [u8]) -> Result<usize, Error> {
        self.check_deadline()?;
        if output.is_empty() {
            return Ok(0);
        }
        while self.offset == self.pending.len() {
            if self.closed {
                return Ok(0);
            }
            self.poll()?;
        }
        let count = output.len().min(self.pending.len() - self.offset);
        output[..count].copy_from_slice(&self.pending[self.offset..self.offset + count]);
        self.offset += count;
        if self.offset == self.pending.len() {
            self.pending.clear();
            self.offset = 0;
        }
        Ok(count)
    }

    fn write(&mut self, data: &[u8]) -> Result<(), Error> {
        for chunk in data.chunks(1200) {
            self.check_deadline()?;
            if self.closed {
                return Err(Error::Message("TCP connection closed"));
            }
            let seq = unsafe { crate::TCP_CONNECTION.seq_num };
            let end = seq.wrapping_add(chunk.len() as u32);
            let mut delivered = false;
            for _ in 0..4 {
                unsafe {
                    // Retransmit the same record bytes at the same TCP sequence number.
                    crate::TCP_CONNECTION.seq_num = seq;
                    if !crate::tcp_send_data(chunk) {
                        return Err(Error::Message("TCP send failed"));
                    }
                }
                let sent = unsafe { crate::get_ticks() };
                while unsafe { crate::get_ticks() }.wrapping_sub(sent) < 1000 {
                    self.poll()?;
                    if self.acknowledged == end {
                        delivered = true;
                        break;
                    }
                    if self.closed {
                        return Err(Error::Message("TCP connection closed"));
                    }
                }
                if delivered {
                    break;
                }
            }
            if !delivered {
                return Err(Error::Message("TCP acknowledgement timed out"));
            }
        }
        Ok(())
    }
}

impl Drop for Tcp {
    fn drop(&mut self) {
        unsafe { crate::tcp_close() };
    }
}

fn word16(bytes: &[u8]) -> u16 {
    u16::from_be_bytes([bytes[0], bytes[1]])
}

fn resolve(host: &[u8], started: u32, timeout_ms: u32) -> Result<[u8; 4], Error> {
    if host.len() > 253
        || host
            .split(|byte| *byte == b'.')
            .any(|label| label.is_empty() || label.len() > 63)
        || host
            .iter()
            .any(|byte| !byte.is_ascii_alphanumeric() && *byte != b'-' && *byte != b'.')
    {
        return Err(Error::Message("invalid DNS hostname"));
    }
    let mut random = [0u8; 4];
    if !crate::prp::random_bytes(&mut random) {
        return Err(Error::Message("DNS random source unavailable"));
    }
    let port = 49152 + (word16(&random[2..]) & 0x3fff);
    let mut query = [0u8; 512];
    let query_len = crate::build_dns_query(host, &mut query);
    if query_len == 0 {
        return Err(Error::Message("DNS query too large"));
    }
    query[..2].copy_from_slice(&random[..2]);
    let mut udp = [0u8; 520];
    udp[..2].copy_from_slice(&port.to_be_bytes());
    udp[2..4].copy_from_slice(&53u16.to_be_bytes());
    udp[4..6].copy_from_slice(&((query_len + 8) as u16).to_be_bytes());
    udp[8..8 + query_len].copy_from_slice(&query[..query_len]);
    let server = unsafe { crate::DNS_SERVER };
    let local = unsafe { crate::LOCAL_IP };
    let mut ip = [0u8; 576];
    let ip_len = unsafe { crate::build_ip_packet(&server, 17, &udp[..8 + query_len], &mut ip) };
    let mut frame = [0u8; 600];
    let frame_len = unsafe {
        crate::build_ethernet_frame(
            &[0x52, 0x54, 0, 0x12, 0x34, 0x56],
            0x0800,
            &ip[..ip_len],
            &mut frame,
        )
    };
    if ip_len == 0 || frame_len == 0 {
        return Err(Error::Message("DNS packet construction failed"));
    }
    let mut last_send = unsafe { crate::get_ticks() }.wrapping_sub(1000);
    let mut attempts = 0;
    loop {
        let now = unsafe { crate::get_ticks() };
        if now.wrapping_sub(started) >= timeout_ms {
            return Err(Error::Message("DNS lookup timed out"));
        }
        if now.wrapping_sub(last_send) >= 1000 && attempts < 4 {
            if unsafe { crate::rust_rtl8139_send(frame.as_ptr(), frame_len as u32) } != 0 {
                return Err(Error::Message("DNS send failed"));
            }
            last_send = now;
            attempts += 1;
        }
        unsafe {
            crate::RX_RESPONSE_LENGTH = 0;
            if crate::rtl8139_receive_one() <= 0 {
                core::hint::spin_loop();
                continue;
            }
            let len = crate::RX_RESPONSE_LENGTH as usize;
            if len > 4096 {
                continue;
            }
            let received = core::slice::from_raw_parts(
                core::ptr::addr_of!(crate::RX_RESPONSE_BUFFER).cast::<u8>(),
                len,
            );
            if let Some(address) =
                dns_response(received, &server, &local, port, &query[..query_len])
            {
                return Ok(address);
            }
        }
    }
}

fn dns_response(
    frame: &[u8],
    server: &[u8; 4],
    local: &[u8; 4],
    port: u16,
    query: &[u8],
) -> Option<[u8; 4]> {
    if frame.len() < 54 || frame[12..14] != [8, 0] || frame[14] >> 4 != 4 {
        return None;
    }
    let ip = &frame[14..];
    let ihl = (ip[0] as usize & 15) * 4;
    let total = word16(&ip[2..]) as usize;
    if ihl < 20
        || total < ihl + 20
        || total > ip.len()
        || ip[9] != 17
        || word16(&ip[6..]) & 0x3fff != 0
        || ip[12..16] != *server
        || ip[16..20] != *local
        || !valid_checksum(checksum(&ip[..ihl]))
    {
        return None;
    }
    let udp = &ip[ihl..total];
    if word16(udp) != 53
        || word16(&udp[2..]) != port
        || word16(&udp[4..]) as usize != udp.len()
        || (word16(&udp[6..]) != 0
            && !valid_checksum(checksum(&ip[12..20]) + 17 + udp.len() as u32 + checksum(udp)))
    {
        return None;
    }
    let response = &udp[8..];
    if response.len() < query.len()
        || response[..2] != query[..2]
        || word16(&response[2..]) & 0xfa0f != 0x8000
        || word16(&response[4..]) != 1
        || response[12..query.len()] != query[12..]
    {
        return None;
    }
    crate::parse_dns_response(response, response.len())
}

fn word32(bytes: &[u8]) -> u32 {
    u32::from_be_bytes([bytes[0], bytes[1], bytes[2], bytes[3]])
}

fn checksum(bytes: &[u8]) -> u32 {
    let mut sum = 0u32;
    for pair in bytes.chunks(2) {
        sum += ((pair[0] as u32) << 8) | pair.get(1).copied().unwrap_or(0) as u32;
    }
    sum
}

fn valid_checksum(mut sum: u32) -> bool {
    while sum > 0xffff {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    sum == 0xffff
}

pub(crate) fn tcp_packet<'a>(
    frame: &'a [u8],
    remote: &[u8; 4],
    local: &[u8; 4],
    remote_port: u16,
    local_port: u16,
) -> Option<&'a [u8]> {
    if frame.len() < 54 || frame[12..14] != [8, 0] || frame[14] >> 4 != 4 {
        return None;
    }
    let ip = &frame[14..];
    let ihl = ((ip[0] & 15) as usize) * 4;
    let total = word16(&ip[2..]) as usize;
    if ihl < 20
        || total < ihl + 20
        || total > ip.len()
        || ip[9] != 6
        || word16(&ip[6..]) & 0x3fff != 0
        || ip[12..16] != *remote
        || ip[16..20] != *local
        || !valid_checksum(checksum(&ip[..ihl]))
    {
        return None;
    }
    let tcp = &ip[ihl..total];
    let header = ((tcp[12] >> 4) as usize) * 4;
    if header < 20
        || header > tcp.len()
        || word16(tcp) != remote_port
        || word16(&tcp[2..]) != local_port
        || !valid_checksum(checksum(&ip[12..20]) + 6 + tcp.len() as u32 + checksum(tcp))
    {
        return None;
    }
    Some(tcp)
}

#[derive(Debug)]
pub(super) struct Rtc;

impl TimeProvider for Rtc {
    fn current_time(&self) -> Option<UnixTime> {
        for _ in 0..32 {
            let first = rtc_snapshot()?;
            let second = rtc_snapshot()?;
            if first == second {
                return rtc_time(first)
                    .map(|seconds| UnixTime::since_unix_epoch(Duration::from_secs(seconds)));
            }
        }
        None
    }
}

fn cmos(register: u8) -> u8 {
    unsafe {
        crate::outb(0x70, register);
        crate::inb(0x71)
    }
}

fn rtc_snapshot() -> Option<[u8; 8]> {
    for _ in 0..10000 {
        if cmos(0x0a) & 0x80 == 0 {
            let result = [
                cmos(0),
                cmos(2),
                cmos(4),
                cmos(7),
                cmos(8),
                cmos(9),
                cmos(0x32),
                cmos(0x0b),
            ];
            if cmos(0x0a) & 0x80 == 0 && cmos(0x0d) & 0x80 != 0 {
                return Some(result);
            }
        }
    }
    None
}

fn rtc_time(mut fields: [u8; 8]) -> Option<u64> {
    let status = fields[7];
    if status & 0x80 != 0 {
        return None;
    }
    let pm = fields[2] & 0x80 != 0;
    fields[2] &= 0x7f;
    if status & 4 == 0 {
        for field in &mut fields[..7] {
            if *field & 15 > 9 || *field >> 4 > 9 {
                return None;
            }
            *field = (*field >> 4) * 10 + (*field & 15);
        }
    }
    if status & 2 == 0 {
        if fields[2] == 0 || fields[2] > 12 {
            return None;
        }
        fields[2] = fields[2] % 12 + if pm { 12 } else { 0 };
    } else if pm {
        return None;
    }
    let year = fields[6] as u32 * 100 + fields[5] as u32;
    if !(1970..=9999).contains(&year)
        || fields[0] > 59
        || fields[1] > 59
        || fields[2] > 23
        || fields[4] == 0
        || fields[4] > 12
        || fields[3] == 0
    {
        return None;
    }
    let leap = |year: u32| year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    let months = [
        31u32,
        if leap(year) { 29 } else { 28 },
        31,
        30,
        31,
        30,
        31,
        31,
        30,
        31,
        30,
        31,
    ];
    if fields[3] as u32 > months[fields[4] as usize - 1] {
        return None;
    }
    let mut days = 0u64;
    for previous in 1970..year {
        days += if leap(previous) { 366 } else { 365 };
    }
    days += months[..fields[4] as usize - 1]
        .iter()
        .map(|days| *days as u64)
        .sum::<u64>();
    days += fields[3] as u64 - 1;
    Some(days * 86400 + fields[2] as u64 * 3600 + fields[1] as u64 * 60 + fields[0] as u64)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rtc_formats_and_invalid_dates() {
        assert_eq!(rtc_time([0, 0, 0, 1, 1, 70, 19, 6]), Some(0));
        assert_eq!(rtc_time([0, 0, 0x12, 1, 1, 0x70, 0x19, 0]), Some(0));
        assert_eq!(rtc_time([0, 0, 0x92, 1, 1, 0x70, 0x19, 0]), Some(43200));
        assert_eq!(rtc_time([0, 0, 0, 29, 2, 0, 20, 6]), Some(951782400));
        assert_eq!(rtc_time([0, 0, 0, 29, 2, 0, 21, 6]), None);
        assert_eq!(rtc_time([0x6a, 0, 0, 1, 1, 0x26, 0x20, 2]), None);
        assert_eq!(rtc_time([0, 0, 24, 1, 1, 26, 20, 6]), None);
    }

    #[test]
    fn packet_bounds_checksums_and_fragments() {
        let mut frame = [0u8; 64];
        frame[12..14].copy_from_slice(&[8, 0]);
        let ip = &mut frame[14..54];
        ip[0] = 0x45;
        ip[2..4].copy_from_slice(&40u16.to_be_bytes());
        ip[8] = 64;
        ip[9] = 6;
        ip[12..16].copy_from_slice(&[1, 2, 3, 4]);
        ip[16..20].copy_from_slice(&[10, 0, 2, 15]);
        ip[20..22].copy_from_slice(&443u16.to_be_bytes());
        ip[22..24].copy_from_slice(&50000u16.to_be_bytes());
        ip[32] = 0x50;
        ip[33] = 0x10;
        let sum = checksum(&ip[..20]);
        ip[10..12].copy_from_slice(&(!(sum as u16)).to_be_bytes());
        let sum = checksum(&ip[12..20]) + 6 + 20 + checksum(&ip[20..]);
        let sum = (sum & 0xffff) + (sum >> 16);
        ip[36..38].copy_from_slice(&(!(sum as u16)).to_be_bytes());
        assert_eq!(
            tcp_packet(&frame, &[1, 2, 3, 4], &[10, 0, 2, 15], 443, 50000)
                .unwrap()
                .len(),
            20
        );
        for len in 0..54 {
            assert!(
                tcp_packet(&frame[..len], &[1, 2, 3, 4], &[10, 0, 2, 15], 443, 50000).is_none()
            );
        }
        frame[20] = 0x20;
        assert!(tcp_packet(&frame, &[1, 2, 3, 4], &[10, 0, 2, 15], 443, 50000).is_none());
        frame[20] = 0;
        frame[40] ^= 1;
        assert!(tcp_packet(&frame, &[1, 2, 3, 4], &[10, 0, 2, 15], 443, 50000).is_none());
    }
}
