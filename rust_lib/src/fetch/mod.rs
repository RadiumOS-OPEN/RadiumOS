#[cfg(target_os = "none")]
mod command;
#[cfg(target_os = "none")]
pub(crate) mod native;
mod options;
mod provider;
mod verify;

use alloc::{
    format,
    string::{String, ToString},
    sync::Arc,
    vec,
    vec::Vec,
};
use core::fmt;
use rustls::{
    client::UnbufferedClientConnection, pki_types::ServerName, time_provider::TimeProvider,
    unbuffered::ConnectionState, ClientConfig, RootCertStore,
};

#[derive(Debug)]
pub enum Error {
    Message(&'static str),
    Tls(rustls::Error),
}

impl fmt::Display for Error {
    fn fmt(&self, out: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Message(message) => out.write_str(message),
            Self::Tls(error) => write!(out, "TLS: {error}"),
        }
    }
}

impl From<rustls::Error> for Error {
    fn from(error: rustls::Error) -> Self {
        Self::Tls(error)
    }
}

pub trait Transport {
    fn read(&mut self, output: &mut [u8]) -> Result<usize, Error>;
    fn write(&mut self, data: &[u8]) -> Result<(), Error>;
}

#[derive(Debug, Clone)]
pub struct Url {
    pub host: String,
    pub port: u16,
    pub path: String,
}

impl Url {
    pub fn parse(input: &str) -> Result<Self, Error> {
        if input.len() > 2048
            || !input.is_ascii()
            || input.bytes().any(|b| b <= 32 || b == 127 || b == b'\\')
        {
            return Err(Error::Message("invalid URL characters or URL too long"));
        }
        let rest = input
            .strip_prefix("https://")
            .ok_or(Error::Message("URL must start with https://"))?;
        let rest = rest.split('#').next().unwrap_or(rest);
        let end = rest.find(['/', '?']).unwrap_or(rest.len());
        let authority = &rest[..end];
        let (host, port) = match authority.split_once(':') {
            Some((host, port)) => (
                host,
                port.parse::<u16>()
                    .ok()
                    .filter(|&p| p != 0)
                    .ok_or(Error::Message("invalid URL port"))?,
            ),
            None => (authority, 443),
        };
        if host.is_empty()
            || host.len() > 253
            || host.split('.').any(|label| {
                label.is_empty()
                    || label.len() > 63
                    || label.starts_with('-')
                    || label.ends_with('-')
                    || !label
                        .bytes()
                        .all(|b| b.is_ascii_alphanumeric() || b == b'-')
            })
        {
            return Err(Error::Message("invalid hostname; IPv6 is not supported"));
        }
        let path = match rest.as_bytes().get(end) {
            None => "/".to_string(),
            Some(b'?') => format!("/{}", &rest[end..]),
            _ => rest[end..].to_string(),
        };
        Ok(Self {
            host: host.to_ascii_lowercase(),
            port,
            path,
        })
    }

    pub fn authority(&self) -> String {
        if self.port == 443 {
            self.host.clone()
        } else {
            format!("{}:{}", self.host, self.port)
        }
    }

    pub fn redirect(&self, location: &str) -> Result<Self, Error> {
        if location.starts_with("https://") {
            return Self::parse(location);
        }
        if location.starts_with("//") {
            return Self::parse(&format!("https:{location}"));
        }
        if location.contains("://") || location.split('/').next().unwrap_or("").contains(':') {
            return Err(Error::Message("redirect to an unsupported scheme"));
        }
        let path = if location.starts_with('/') {
            location.to_string()
        } else if location.starts_with('?') {
            format!("{}{location}", self.path.split('?').next().unwrap_or("/"))
        } else if location.starts_with('#') || location.is_empty() {
            self.path.clone()
        } else {
            let base = self.path.split('?').next().unwrap_or("/");
            format!("{}{location}", &base[..base.rfind('/').unwrap_or(0) + 1])
        };
        let mut target = Self::parse(&format!("https://{}{path}", self.authority()))?;
        let (path, query) = target.path.split_once('?').unwrap_or((&target.path, ""));
        let mut segments = Vec::new();
        for part in path.split('/') {
            match part {
                "." => {}
                ".." => {
                    segments.pop();
                }
                _ => segments.push(part),
            }
        }
        let mut path = segments.join("/");
        if !path.starts_with('/') {
            path.insert(0, '/');
        }
        if (target.path.ends_with("/.") || target.path.ends_with("/..")) && !path.ends_with('/') {
            path.push('/');
        }
        if target.path.contains('?') {
            path.push('?');
            path.push_str(query);
        }
        target.path = path;
        Ok(target)
    }
}

pub fn configuration(
    time: Arc<dyn TimeProvider>,
    roots: RootCertStore,
) -> Result<Arc<ClientConfig>, Error> {
    let mut config = ClientConfig::builder_with_details(Arc::new(provider::provider()), time)
        .with_protocol_versions(&[&rustls::version::TLS13])?
        .with_root_certificates(roots)
        .with_no_client_auth();
    config.alpn_protocols = vec![b"http/1.1".to_vec()];
    config.resumption = rustls::client::Resumption::disabled();
    config.max_fragment_size = Some(1200);
    Ok(Arc::new(config))
}

pub fn public_roots() -> RootCertStore {
    RootCertStore {
        roots: webpki_roots::TLS_SERVER_ROOTS.to_vec(),
    }
}

pub struct Response {
    pub status: u16,
    pub headers: Vec<u8>,
    pub body: Vec<u8>,
    pub location: Option<String>,
}

const HEADER_LIMIT: usize = 16384;
const RECORD_BUFFER: usize = 18432;

pub fn get<T: Transport>(
    transport: &mut T,
    url: &Url,
    config: Arc<ClientConfig>,
    head: bool,
    max_size: usize,
) -> Result<Response, Error> {
    let name = ServerName::try_from(url.host.clone())
        .map_err(|_| Error::Message("invalid server name"))?;
    let mut connection = UnbufferedClientConnection::new(config, name)?;
    let request = format!("{} {} HTTP/1.1\r\nHost: {}\r\nUser-Agent: RadiumOS-fetch/1.0\r\nAccept: */*\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n",
        if head { "HEAD" } else { "GET" }, url.path, url.authority());
    let mut incoming = vec![0u8; RECORD_BUFFER];
    let mut outgoing = vec![0u8; RECORD_BUFFER];
    let mut used = 0;
    let mut written = 0;
    let mut sent = false;
    let mut response = Vec::new();
    loop {
        let status = connection.process_tls_records(&mut incoming[..used]);
        let mut discard = status.discard;
        let mut need_read = false;
        match status.state? {
            ConnectionState::EncodeTlsData(mut state) => {
                written += state
                    .encode(&mut outgoing[written..])
                    .map_err(|_| Error::Message("TLS output exceeds buffer"))?;
            }
            ConnectionState::TransmitTlsData(state) => {
                transport.write(&outgoing[..written])?;
                written = 0;
                state.done();
            }
            ConnectionState::WriteTraffic(mut state) => {
                if !sent {
                    let len = state
                        .encrypt(request.as_bytes(), &mut outgoing)
                        .map_err(|_| Error::Message("TLS request exceeds buffer"))?;
                    transport.write(&outgoing[..len])?;
                    sent = true;
                }
                need_read = true;
            }
            ConnectionState::ReadTraffic(mut state) => {
                while let Some(record) = state.next_record() {
                    let record = record?;
                    discard += record.discard;
                    let limit = max_size
                        .checked_add(1024 * 1024 + HEADER_LIMIT)
                        .ok_or(Error::Message("invalid size limit"))?;
                    if record.payload.len() > limit.saturating_sub(response.len()) {
                        return Err(Error::Message("response exceeds --max-size"));
                    }
                    response.extend_from_slice(record.payload);
                }
                if let Some(result) = parse_response(&response, head, max_size, false)? {
                    return Ok(result);
                }
            }
            ConnectionState::PeerClosed | ConnectionState::Closed => {
                return parse_response(&response, head, max_size, true)?
                    .ok_or(Error::Message("incomplete HTTP response"));
            }
            ConnectionState::BlockedHandshake => need_read = true,
            _ => return Err(Error::Message("unsupported TLS state")),
        }
        if discard > used {
            return Err(Error::Message("invalid TLS buffer state"));
        }
        incoming.copy_within(discard..used, 0);
        used -= discard;
        if need_read {
            if used == incoming.len() {
                return Err(Error::Message("TLS record exceeds buffer"));
            }
            let count = transport.read(&mut incoming[used..])?;
            if count == 0 {
                return Err(Error::Message(
                    "connection closed before authenticated response completion",
                ));
            }
            if count > incoming.len() - used {
                return Err(Error::Message("invalid transport read"));
            }
            used += count;
        }
    }
}

fn decimal(input: &str) -> Result<usize, Error> {
    if input.is_empty() || !input.bytes().all(|b| b.is_ascii_digit()) {
        return Err(Error::Message("invalid Content-Length"));
    }
    input
        .parse()
        .map_err(|_| Error::Message("Content-Length overflow"))
}

fn parse_response(
    bytes: &[u8],
    head: bool,
    max_size: usize,
    closed: bool,
) -> Result<Option<Response>, Error> {
    parse_response_at(bytes, head, max_size, closed, 0)
}

fn parse_response_at(
    bytes: &[u8],
    head: bool,
    max_size: usize,
    closed: bool,
    informational: u8,
) -> Result<Option<Response>, Error> {
    let end = match bytes.windows(4).position(|w| w == b"\r\n\r\n") {
        Some(end) if end + 4 <= HEADER_LIMIT => end + 4,
        Some(_) => return Err(Error::Message("HTTP headers too large")),
        None if bytes.len() >= HEADER_LIMIT => {
            return Err(Error::Message("HTTP headers too large"))
        }
        None => return Ok(None),
    };
    let text =
        core::str::from_utf8(&bytes[..end]).map_err(|_| Error::Message("invalid HTTP headers"))?;
    let mut lines = text.split("\r\n");
    let status_line = lines.next().unwrap_or("");
    let status_bytes = status_line.as_bytes();
    if !(status_line.starts_with("HTTP/1.1 ") || status_line.starts_with("HTTP/1.0 "))
        || status_bytes.len() < 12
        || !status_bytes[9..12].iter().all(u8::is_ascii_digit)
        || (status_bytes.len() > 12 && status_bytes[12] != b' ')
    {
        return Err(Error::Message("invalid HTTP status"));
    }
    let status = status_line[9..12]
        .parse::<u16>()
        .map_err(|_| Error::Message("invalid HTTP status"))?;
    if !(100..=599).contains(&status) {
        return Err(Error::Message("invalid HTTP status"));
    }
    if (100..200).contains(&status) {
        if informational == 8 {
            return Err(Error::Message("too many informational responses"));
        }
        if status == 101 {
            return Err(Error::Message("HTTP protocol upgrade is unsupported"));
        }
        // Informational responses precede the final response on the same connection.
        if bytes.len() == end {
            return Ok(None);
        }
        return parse_response_at(&bytes[end..], head, max_size, closed, informational + 1);
    }
    let mut length = None;
    let mut chunked = false;
    let mut location = None;
    for line in lines.filter(|line| !line.is_empty()) {
        let (name, value) = line
            .split_once(':')
            .ok_or(Error::Message("invalid HTTP header"))?;
        if name.is_empty()
            || !name
                .bytes()
                .all(|b| b.is_ascii_alphanumeric() || b"!#$%&'*+-.^_`|~".contains(&b))
            || value.bytes().any(|b| (b < 32 && b != b'\t') || b == 127)
        {
            return Err(Error::Message("invalid HTTP header"));
        }
        let value = value.trim_matches([' ', '\t']);
        if name.eq_ignore_ascii_case("content-length") {
            let value = decimal(value)?;
            if length.replace(value).is_some() {
                return Err(Error::Message("duplicate Content-Length"));
            }
        } else if name.eq_ignore_ascii_case("transfer-encoding") {
            if chunked || !value.eq_ignore_ascii_case("chunked") {
                return Err(Error::Message("unsupported Transfer-Encoding"));
            }
            chunked = true;
        } else if name.eq_ignore_ascii_case("location") {
            if location.replace(value.to_string()).is_some() {
                return Err(Error::Message("duplicate Location"));
            }
        } else if name.eq_ignore_ascii_case("content-encoding")
            && !value.eq_ignore_ascii_case("identity")
        {
            return Err(Error::Message("compressed response is unsupported"));
        }
    }
    if length.is_some() && chunked {
        return Err(Error::Message("ambiguous HTTP response framing"));
    }
    let data = &bytes[end..];
    let body = if head || status == 204 || status == 304 {
        Vec::new()
    } else if chunked {
        match decode_chunks(data, max_size)? {
            Some(body) => body,
            None => return Ok(None),
        }
    } else if let Some(length) = length {
        if length > max_size {
            return Err(Error::Message("response exceeds --max-size"));
        }
        if data.len() < length {
            return Ok(None);
        }
        data[..length].to_vec()
    } else {
        if data.len() > max_size {
            return Err(Error::Message("response exceeds --max-size"));
        }
        if !closed {
            return Ok(None);
        }
        data.to_vec()
    };
    Ok(Some(Response {
        status,
        headers: bytes[..end].to_vec(),
        body,
        location,
    }))
}

fn decode_chunks(mut bytes: &[u8], max_size: usize) -> Result<Option<Vec<u8>>, Error> {
    let mut body = Vec::new();
    loop {
        let end = match bytes.windows(2).position(|w| w == b"\r\n") {
            Some(end) if end <= 1024 => end,
            Some(_) => return Err(Error::Message("chunk header too large")),
            None if bytes.len() > 1024 => return Err(Error::Message("chunk header too large")),
            None => return Ok(None),
        };
        let size = bytes[..end].split(|&b| b == b';').next().unwrap_or(&[]);
        if size.is_empty() || !size.iter().all(u8::is_ascii_hexdigit) {
            return Err(Error::Message("invalid chunk length"));
        }
        let size = usize::from_str_radix(core::str::from_utf8(size).unwrap(), 16)
            .map_err(|_| Error::Message("chunk length overflow"))?;
        bytes = &bytes[end + 2..];
        if size == 0 {
            if bytes.starts_with(b"\r\n") {
                return Ok(Some(body));
            }
            return match bytes.windows(4).position(|w| w == b"\r\n\r\n") {
                Some(end) if end <= HEADER_LIMIT => Ok(Some(body)),
                _ if bytes.len() > HEADER_LIMIT => Err(Error::Message("HTTP trailers too large")),
                _ => Ok(None),
            };
        }
        if size > max_size.saturating_sub(body.len()) {
            return Err(Error::Message("response exceeds --max-size"));
        }
        if bytes.len() < size + 2 {
            return Ok(None);
        }
        if &bytes[size..size + 2] != b"\r\n" {
            return Err(Error::Message("invalid chunk terminator"));
        }
        body.extend_from_slice(&bytes[..size]);
        bytes = &bytes[size + 2..];
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn urls_and_http_framing() {
        let url = Url::parse("https://Example.com:8443/a/b?q=1#fragment").unwrap();
        assert_eq!(url.authority(), "example.com:8443");
        assert_eq!(url.path, "/a/b?q=1");
        assert_eq!(url.redirect("../c").unwrap().path, "/c");
        assert_eq!(url.redirect("?q=2").unwrap().path, "/a/b?q=2");
        for invalid in [
            "http://example.com",
            "https://u:p@example.com/",
            "https://example.com:0",
            "https://example.com:65536",
            "https://example.com/\r\nX:evil",
            "https://[::1]/",
        ] {
            assert!(Url::parse(invalid).is_err(), "{invalid}");
        }
        let message = b"HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello";
        for split in 0..message.len() {
            assert!(parse_response(&message[..split], false, 5, false)
                .unwrap()
                .is_none());
        }
        assert_eq!(
            parse_response(message, false, 5, false)
                .unwrap()
                .unwrap()
                .body,
            b"hello"
        );
        assert!(parse_response(message, false, 4, false).is_err());
        let chunked = b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nhe\r\n3\r\nllo\r\n0\r\n\r\n";
        for split in 0..chunked.len() {
            assert!(parse_response(&chunked[..split], false, 5, false)
                .unwrap()
                .is_none());
        }
        assert_eq!(
            parse_response(chunked, false, 5, false)
                .unwrap()
                .unwrap()
                .body,
            b"hello"
        );
        assert!(parse_response(
            b"HTTP/1.1 200 OK\r\nContent-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\n",
            false,
            5,
            false
        )
        .is_err());
        let close = b"HTTP/1.0 200 OK\r\n\r\nhello";
        assert!(parse_response(close, false, 5, false).unwrap().is_none());
        assert_eq!(
            parse_response(close, false, 5, true).unwrap().unwrap().body,
            b"hello"
        );
        assert!(parse_response(message, true, 0, false)
            .unwrap()
            .unwrap()
            .body
            .is_empty());
    }
}
