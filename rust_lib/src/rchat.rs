use alloc::{format, string::{String, ToString}, sync::Arc, vec, vec::Vec};
use core::{fmt::Write, slice, str, sync::atomic::{AtomicBool, Ordering}};
use rustls::time_provider::TimeProvider;
use zeroize::Zeroize;

use crate::{
    fetch::{
        configuration, configuration_insecure, exchange_plain, exchange_tls,
        native::{Rtc, Tcp},
        public_roots, Response, Url,
    },
    prp,
};

const MAX_PROFILES: usize = 4;
const CLIENT_ID_PREFIX: &[u8] = b"rchat-id-v1\0";
const KEY_BINDING_PREFIX: &[u8] = b"rchat-key-v1\0";
const REGISTRATION_PREFIX: &[u8] = b"rchat-register-v1\0";
const AUTH_PREFIX: &[u8] = b"rchat-auth-v1\0";
const MESSAGE_PREFIX: &[u8] = b"rchat-message-v1\0";
const HPKE_INFO: &[u8] = b"rchat-hpke-v1";
const NETWORK_TIMEOUT_MS: u32 = 15_000;
const RESPONSE_LIMIT: usize = 48 * 1024;
const SERVER_ADDRESS_MAX: usize = 280;

struct Profile {
    key: String,
    url: Url,
    trust_server_cert: bool,
    auth_private: [u8; 32],
    auth_public: [u8; 32],
    encryption_private: [u8; 32],
    encryption_public: [u8; 32],
    client_id: [u8; 16],
    encryption_key_signature: [u8; 64],
    enrolled: bool,
    access_token: Option<[u8; 32]>,
    token_issued_at: u32,
    cursor: String,
    pending_server_message_id: String,
    pending_cursor: String,
}

impl Drop for Profile {
    fn drop(&mut self) {
        self.auth_private.zeroize();
        self.encryption_private.zeroize();
        if let Some(token) = &mut self.access_token {
            token.zeroize();
        }
    }
}

static mut PROFILES: Option<Vec<Profile>> = None;
static mut ACTIVE_PROFILE: usize = usize::MAX;

unsafe fn c_string<'a>(pointer: *const u8, maximum: usize) -> Result<&'a str, ()> {
    if pointer.is_null() {
        return Err(());
    }
    for length in 0..=maximum {
        if *pointer.add(length) == 0 {
            return str::from_utf8(slice::from_raw_parts(pointer, length)).map_err(|_| ());
        }
    }
    Err(())
}

/// Removes trailing slashes from a server address before it is stored or parsed.
fn normalize_server_address(address: &str) -> &str {
    address.trim_end_matches('/')
}

/// Derives the normalized authority used to identify a server profile.
fn profile_key(address: &str, url: &Url) -> Result<String, ()> {
    if url.path != "/" {
        return Err(());
    }
    let address = normalize_server_address(address);
    let authority = address
        .strip_prefix("https://")
        .or_else(|| address.strip_prefix("http://"))
        .ok_or(())?;
    if authority.is_empty() || authority.contains(['/', '?', '#']) {
        return Err(());
    }
    Ok(authority.to_ascii_lowercase())
}

/// Creates a server profile with fresh authentication and encryption keys.
fn new_profile(key: String, url: Url) -> Result<Profile, ()> {
    let mut auth_private = [0u8; 32];
    let mut encryption_private = [0u8; 32];
    if !prp::random_bytes(&mut auth_private)
        || !prp::random_bytes(&mut encryption_private)
        || auth_private == encryption_private
    {
        auth_private.zeroize();
        encryption_private.zeroize();
        return Err(());
    }

    let auth_public = prp::ed25519_public_key(&auth_private);
    let encryption_public = prp::x25519(&encryption_private, &prp::X25519_BASEPOINT);

    let mut id_input = [0u8; 44];
    id_input[..CLIENT_ID_PREFIX.len()].copy_from_slice(CLIENT_ID_PREFIX);
    id_input[CLIENT_ID_PREFIX.len()..].copy_from_slice(&auth_public);
    let digest = prp::sha256(&id_input);
    let mut client_id = [0u8; 16];
    client_id.copy_from_slice(&digest[..16]);

    let mut binding_input = [0u8; 45];
    binding_input[..KEY_BINDING_PREFIX.len()].copy_from_slice(KEY_BINDING_PREFIX);
    binding_input[KEY_BINDING_PREFIX.len()..].copy_from_slice(&encryption_public);
    let encryption_key_signature = prp::ed25519_sign(&auth_private, &binding_input);

    Ok(Profile {
        key,
        url,
        trust_server_cert: false,
        auth_private,
        auth_public,
        encryption_private,
        encryption_public,
        client_id,
        encryption_key_signature,
        enrolled: false,
        access_token: None,
        token_issued_at: 0,
        cursor: String::new(),
        pending_server_message_id: String::new(),
        pending_cursor: String::new(),
    })
}

fn base64url(input: &[u8], output: &mut [u8]) -> Option<usize> {
    const ALPHABET: &[u8; 64] =
        b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    let required = (input.len() * 4 + 2) / 3;
    if output.len() < required + 1 {
        return None;
    }
    let mut source = 0;
    let mut target = 0;
    while source + 3 <= input.len() {
        let value = ((input[source] as u32) << 16)
            | ((input[source + 1] as u32) << 8)
            | input[source + 2] as u32;
        output[target] = ALPHABET[(value >> 18) as usize];
        output[target + 1] = ALPHABET[((value >> 12) & 63) as usize];
        output[target + 2] = ALPHABET[((value >> 6) & 63) as usize];
        output[target + 3] = ALPHABET[(value & 63) as usize];
        source += 3;
        target += 4;
    }
    match input.len() - source {
        1 => {
            let value = (input[source] as u32) << 16;
            output[target] = ALPHABET[(value >> 18) as usize];
            output[target + 1] = ALPHABET[((value >> 12) & 63) as usize];
            target += 2;
        }
        2 => {
            let value = ((input[source] as u32) << 16) | ((input[source + 1] as u32) << 8);
            output[target] = ALPHABET[(value >> 18) as usize];
            output[target + 1] = ALPHABET[((value >> 12) & 63) as usize];
            output[target + 2] = ALPHABET[((value >> 6) & 63) as usize];
            target += 3;
        }
        _ => {}
    }
    output[target] = 0;
    Some(target)
}

fn encoded(input: &[u8]) -> String {
    let mut output = vec![0u8; (input.len() * 4 + 2) / 3 + 1];
    let length = base64url(input, &mut output).expect("base64url output size");
    String::from_utf8_lossy(&output[..length]).into_owned()
}

fn base64url_value(byte: u8) -> Option<u8> {
    match byte {
        b'A'..=b'Z' => Some(byte - b'A'),
        b'a'..=b'z' => Some(byte - b'a' + 26),
        b'0'..=b'9' => Some(byte - b'0' + 52),
        b'-' => Some(62),
        b'_' => Some(63),
        _ => None,
    }
}

fn is_base64url(input: &str) -> bool {
    !input.is_empty() && input.bytes().all(|byte| base64url_value(byte).is_some())
}

fn decode_base64url<const N: usize>(input: &str) -> Option<[u8; N]> {
    if input.len() != (N * 4 + 2) / 3 {
        return None;
    }
    let mut output = [0u8; N];
    let mut bits = 0u32;
    let mut bit_count = 0usize;
    let mut written = 0usize;
    for byte in input.bytes() {
        bits = (bits << 6) | base64url_value(byte)? as u32;
        bit_count += 6;
        if bit_count >= 8 {
            bit_count -= 8;
            if written >= N {
                return None;
            }
            output[written] = (bits >> bit_count) as u8;
            written += 1;
            bits &= (1u32 << bit_count).wrapping_sub(1);
        }
    }
    if written != N || bits != 0 {
        return None;
    }
    Some(output)
}

fn decode_base64url_vec(input: &str, maximum: usize) -> Option<Vec<u8>> {
    if input.len() % 4 == 1 {
        return None;
    }
    let length = input.len() * 6 / 8;
    if length > maximum || input.len() != (length * 4 + 2) / 3 {
        return None;
    }
    let mut output = Vec::with_capacity(length);
    let mut bits = 0u32;
    let mut bit_count = 0usize;
    for byte in input.bytes() {
        bits = (bits << 6) | base64url_value(byte)? as u32;
        bit_count += 6;
        if bit_count >= 8 {
            bit_count -= 8;
            output.push((bits >> bit_count) as u8);
            bits &= (1u32 << bit_count).wrapping_sub(1);
        }
    }
    if output.len() != length || bits != 0 {
        return None;
    }
    Some(output)
}

fn json_escape(input: &str) -> String {
    let mut output = String::with_capacity(input.len() + 2);
    for character in input.chars() {
        match character {
            '"' => output.push_str("\\\""),
            '\\' => output.push_str("\\\\"),
            '\u{08}' => output.push_str("\\b"),
            '\u{0c}' => output.push_str("\\f"),
            '\n' => output.push_str("\\n"),
            '\r' => output.push_str("\\r"),
            '\t' => output.push_str("\\t"),
            character if character <= '\u{1f}' => {
                let _ = write!(&mut output, "\\u{:04x}", character as u32);
            }
            _ => output.push(character),
        }
    }
    output
}

fn json_string_field<'a>(body: &'a [u8], field: &str) -> Option<&'a str> {
    let text = str::from_utf8(body).ok()?;
    let bytes = text.as_bytes();
    let mut index = 0usize;
    let mut found = None;
    while index < bytes.len() {
        if bytes[index] != b'"' {
            index += 1;
            continue;
        }
        let key_start = index + 1;
        index = key_start;
        while index < bytes.len() && bytes[index] != b'"' {
            if bytes[index] == b'\\' {
                index += 1;
            }
            index += 1;
        }
        if index >= bytes.len() {
            return None;
        }
        let key_end = index;
        index += 1;
        while bytes.get(index).copied().is_some_and(|byte| byte.is_ascii_whitespace()) {
            index += 1;
        }
        if bytes.get(index) != Some(&b':') || &text[key_start..key_end] != field {
            continue;
        }
        index += 1;
        while bytes.get(index).copied().is_some_and(|byte| byte.is_ascii_whitespace()) {
            index += 1;
        }
        if bytes.get(index) != Some(&b'"') {
            return None;
        }
        let value_start = index + 1;
        index = value_start;
        while index < bytes.len() && bytes[index] != b'"' {
            if bytes[index] == b'\\' {
                return None;
            }
            index += 1;
        }
        if index >= bytes.len() || found.is_some() {
            return None;
        }
        found = Some(&text[value_start..index]);
        index += 1;
    }
    found
}

fn first_message(body: &[u8]) -> Option<Option<&[u8]>> {
    let marker = b"\"messages\"";
    let mut index = body.windows(marker.len()).position(|part| part == marker)? + marker.len();
    while body.get(index).copied().is_some_and(|byte| byte.is_ascii_whitespace()) {
        index += 1;
    }
    if body.get(index) != Some(&b':') {
        return None;
    }
    index += 1;
    while body.get(index).copied().is_some_and(|byte| byte.is_ascii_whitespace()) {
        index += 1;
    }
    if body.get(index) != Some(&b'[') {
        return None;
    }
    index += 1;
    while body.get(index).copied().is_some_and(|byte| byte.is_ascii_whitespace()) {
        index += 1;
    }
    if body.get(index) == Some(&b']') {
        return Some(None);
    }
    if body.get(index) != Some(&b'{') {
        return None;
    }
    let object_start = index;
    let mut depth = 0usize;
    let mut quoted = false;
    let mut escaped = false;
    while index < body.len() {
        let byte = body[index];
        if quoted {
            if escaped {
                escaped = false;
            } else if byte == b'\\' {
                escaped = true;
            } else if byte == b'"' {
                quoted = false;
            }
        } else if byte == b'"' {
            quoted = true;
        } else if byte == b'{' {
            depth += 1;
        } else if byte == b'}' {
            depth = depth.checked_sub(1)?;
            if depth == 0 {
                return Some(Some(&body[object_start..=index]));
            }
        }
        index += 1;
    }
    None
}

fn json_hex(byte: u8) -> Option<u32> {
    match byte {
        b'0'..=b'9' => Some((byte - b'0') as u32),
        b'a'..=b'f' => Some((byte - b'a' + 10) as u32),
        b'A'..=b'F' => Some((byte - b'A' + 10) as u32),
        _ => None,
    }
}

fn json_code_unit(bytes: &[u8], start: usize) -> Option<u16> {
    let mut value = 0u32;
    for byte in bytes.get(start..start + 4)? {
        value = (value << 4) | json_hex(*byte)?;
    }
    Some(value as u16)
}

fn decode_json_string(text: &str, start: usize) -> Option<(String, usize)> {
    let bytes = text.as_bytes();
    let mut index = start;
    let mut output = String::new();
    while index < bytes.len() {
        match bytes[index] {
            b'"' => return Some((output, index)),
            b'\\' => {
                index += 1;
                match *bytes.get(index)? {
                    b'"' => output.push('"'),
                    b'\\' => output.push('\\'),
                    b'/' => output.push('/'),
                    b'b' => output.push('\u{08}'),
                    b'f' => output.push('\u{0c}'),
                    b'n' => output.push('\n'),
                    b'r' => output.push('\r'),
                    b't' => output.push('\t'),
                    b'u' => {
                        index += 1;
                        let high = json_code_unit(bytes, index)?;
                        index += 4;
                        let codepoint = if (0xd800..=0xdbff).contains(&high) {
                            if bytes.get(index..index + 2)? != b"\\u" {
                                return None;
                            }
                            index += 2;
                            let low = json_code_unit(bytes, index)?;
                            if !(0xdc00..=0xdfff).contains(&low) {
                                return None;
                            }
                            index += 4;
                            0x10000
                                + (((high as u32 - 0xd800) << 10)
                                    | (low as u32 - 0xdc00))
                        } else {
                            if (0xdc00..=0xdfff).contains(&high) {
                                return None;
                            }
                            high as u32
                        };
                        output.push(char::from_u32(codepoint)?);
                        continue;
                    }
                    _ => return None,
                }
                index += 1;
            }
            byte if byte < 32 || byte >= 128 => {
                let character = text[index..].chars().next()?;
                if character <= '\u{1f}' {
                    return None;
                }
                output.push(character);
                index += character.len_utf8();
            }
            byte => {
                output.push(byte as char);
                index += 1;
            }
        }
    }
    None
}

fn json_unescaped_field(body: &[u8], field: &str) -> Option<String> {
    let text = str::from_utf8(body).ok()?;
    let bytes = text.as_bytes();
    let mut index = 0usize;
    let mut found = None;
    while index < bytes.len() {
        if bytes[index] != b'"' {
            index += 1;
            continue;
        }
        let (key, key_end) = decode_json_string(text, index + 1)?;
        index = key_end + 1;
        while bytes.get(index).copied().is_some_and(|byte| byte.is_ascii_whitespace()) {
            index += 1;
        }
        if bytes.get(index) != Some(&b':') || key != field {
            continue;
        }
        index += 1;
        while bytes.get(index).copied().is_some_and(|byte| byte.is_ascii_whitespace()) {
            index += 1;
        }
        if bytes.get(index) != Some(&b'"') || found.is_some() {
            return None;
        }
        let (value, value_end) = decode_json_string(text, index + 1)?;
        found = Some(value);
        index = value_end + 1;
    }
    found
}

struct NetworkGuard(u32);

static NETWORK_BUSY: AtomicBool = AtomicBool::new(false);

impl NetworkGuard {
    fn enter() -> Result<Self, ()> {
        if NETWORK_BUSY
            .compare_exchange(false, true, Ordering::Acquire, Ordering::Relaxed)
            .is_err()
        {
            return Err(());
        }
        let flags: u32;
        unsafe {
            core::arch::asm!("pushfd", "pop {}", out(reg) flags);
            crate::FETCH_NETWORK_SILENT.store(true, Ordering::Relaxed);
            core::arch::asm!("sti");
        }
        Ok(Self(flags))
    }
}

impl Drop for NetworkGuard {
    fn drop(&mut self) {
        if self.0 & (1 << 9) == 0 {
            unsafe { core::arch::asm!("cli") };
        }
        crate::FETCH_NETWORK_SILENT.store(false, Ordering::Relaxed);
        NETWORK_BUSY.store(false, Ordering::Release);
    }
}

/// Sends one rChat API request using the profile's configured transport policy.
fn api_request(
    profile: &Profile,
    method: &str,
    path: &str,
    body: Option<&str>,
    bearer: Option<&[u8; 32]>,
) -> Result<Response, ()> {
    let _guard = NetworkGuard::enter()?;
    let mut url = profile.url.clone();
    url.path = path.into();
    let tls_config = if url.is_https {
        Some(if profile.trust_server_cert {
            configuration_insecure(Arc::new(Rtc)).map_err(|_| ())?
        } else {
            if Rtc.current_time().is_none() {
                return Err(());
            }
            configuration(Arc::new(Rtc), public_roots()).map_err(|_| ())?
        })
    } else {
        None
    };
    let mut tcp = Tcp::connect(&url.host, url.port, NETWORK_TIMEOUT_MS).map_err(|_| ())?;
    let mut request = format!(
        "{method} {path} HTTP/1.1\r\nHost: {}\r\nUser-Agent: RadiumOS-rchat/1.0\r\nAccept: application/json\r\nAccept-Encoding: identity\r\nConnection: close\r\n",
        url.authority()
    );
    if let Some(token) = bearer {
        request.push_str("Authorization: Bearer ");
        let mut token_text = encoded(token);
        request.push_str(&token_text);
        token_text.zeroize();
        request.push_str("\r\n");
    }
    if let Some(body) = body {
        request.push_str("Content-Type: application/json\r\nContent-Length: ");
        request.push_str(&body.len().to_string());
        request.push_str("\r\n\r\n");
        request.push_str(body);
    } else {
        request.push_str("\r\n");
    }

    let response = if let Some(config) = tls_config {
        exchange_tls(
            &mut tcp,
            &url,
            config,
            request.as_bytes(),
            false,
            RESPONSE_LIMIT,
        )
        .map_err(|_| ())
    } else {
        exchange_plain(&mut tcp, request.as_bytes(), false, RESPONSE_LIMIT).map_err(|_| ())
    };
    request.zeroize();
    response
}

fn derive_client_id(auth_public: &[u8; 32]) -> [u8; 16] {
    let mut input = [0u8; 44];
    input[..CLIENT_ID_PREFIX.len()].copy_from_slice(CLIENT_ID_PREFIX);
    input[CLIENT_ID_PREFIX.len()..].copy_from_slice(auth_public);
    let digest = prp::sha256(&input);
    let mut id = [0u8; 16];
    id.copy_from_slice(&digest[..16]);
    id
}

fn ensure_session(profile: &mut Profile) -> Result<(), i32> {
    if !profile.enrolled {
        return Err(-2);
    }
    if profile.access_token.is_some()
        && unsafe { crate::get_ticks() }.wrapping_sub(profile.token_issued_at) < 15 * 60 * 1000
    {
        return Ok(());
    }
    if let Some(token) = &mut profile.access_token {
        token.zeroize();
    }
    profile.access_token = None;

    let client_id = encoded(&profile.client_id);
    let challenge_body = format!("{{\"client_id\":\"{client_id}\"}}");
    let response = api_request(
        profile,
        "POST",
        "/v1/auth/challenges",
        Some(&challenge_body),
        None,
    )
    .map_err(|_| -4)?;
    if response.status != 201 {
        return Err(-5);
    }
    let challenge_id = json_string_field(&response.body, "challenge_id")
        .and_then(decode_base64url::<16>)
        .ok_or(-6)?;
    let challenge = json_string_field(&response.body, "challenge")
        .and_then(decode_base64url::<32>)
        .ok_or(-6)?;

    let mut signing_input = Vec::with_capacity(AUTH_PREFIX.len() + 48);
    signing_input.extend_from_slice(AUTH_PREFIX);
    signing_input.extend_from_slice(&challenge_id);
    signing_input.extend_from_slice(&challenge);
    let signature = prp::ed25519_sign(&profile.auth_private, &signing_input);
    let session_body = format!(
        "{{\"client_id\":\"{}\",\"challenge_id\":\"{}\",\"signature\":\"{}\"}}",
        client_id,
        encoded(&challenge_id),
        encoded(&signature),
    );
    let response = api_request(
        profile,
        "POST",
        "/v1/auth/sessions",
        Some(&session_body),
        None,
    )
    .map_err(|_| -4)?;
    if response.status != 201 {
        return Err(-5);
    }
    let token = json_string_field(&response.body, "access_token")
        .and_then(decode_base64url::<32>)
        .ok_or(-6)?;
    profile.access_token = Some(token);
    profile.token_issued_at = unsafe { crate::get_ticks() };
    Ok(())
}

fn authenticated_request(
    profile: &mut Profile,
    method: &str,
    path: &str,
    body: Option<&str>,
) -> Result<Response, i32> {
    for _ in 0..2 {
        ensure_session(profile)?;
        let token = profile.access_token.as_ref().ok_or(-2)?;
        let response = api_request(profile, method, path, body, Some(token)).map_err(|_| -4)?;
        if response.status != 401 {
            return Ok(response);
        }
        if let Some(token) = &mut profile.access_token {
            token.zeroize();
        }
        profile.access_token = None;
    }
    Err(-5)
}

struct Recipient {
    auth_public: [u8; 32],
    encryption_public: [u8; 32],
}

fn lookup_recipient(profile: &mut Profile, requested_id: &[u8; 16]) -> Result<Recipient, i32> {
    let requested = encoded(requested_id);
    let path = format!("/v1/clients/{requested}");
    let response = authenticated_request(profile, "GET", &path, None)?;
    if response.status != 200 {
        return Err(-5);
    }
    let returned_id = json_string_field(&response.body, "client_id")
        .and_then(decode_base64url::<16>)
        .ok_or(-6)?;
    let auth_public = json_string_field(&response.body, "auth_public_key")
        .and_then(decode_base64url::<32>)
        .ok_or(-6)?;
    let encryption_public = json_string_field(&response.body, "encryption_public_key")
        .and_then(decode_base64url::<32>)
        .ok_or(-6)?;
    let binding = json_string_field(&response.body, "encryption_key_signature")
        .and_then(decode_base64url::<64>)
        .ok_or(-6)?;
    if returned_id != *requested_id || derive_client_id(&auth_public) != *requested_id {
        return Err(-6);
    }
    let mut binding_input = [0u8; 45];
    binding_input[..KEY_BINDING_PREFIX.len()].copy_from_slice(KEY_BINDING_PREFIX);
    binding_input[KEY_BINDING_PREFIX.len()..].copy_from_slice(&encryption_public);
    if !prp::ed25519_verify(&auth_public, &binding_input, &binding) {
        return Err(-6);
    }
    Ok(Recipient {
        auth_public,
        encryption_public,
    })
}

fn labeled_extract(salt: &[u8], suite: &[u8], label: &[u8], input: &[u8]) -> [u8; 32] {
    let mut labeled = Vec::with_capacity(7 + suite.len() + label.len() + input.len());
    labeled.extend_from_slice(b"HPKE-v1");
    labeled.extend_from_slice(suite);
    labeled.extend_from_slice(label);
    labeled.extend_from_slice(input);
    prp::hkdf_extract(salt, &labeled)
}

fn labeled_expand(
    secret: &[u8; 32],
    suite: &[u8],
    label: &[u8],
    info: &[u8],
    output: &mut [u8],
) -> bool {
    if output.len() > u16::MAX as usize {
        return false;
    }
    let mut labeled = Vec::with_capacity(2 + 7 + suite.len() + label.len() + info.len());
    labeled.extend_from_slice(&(output.len() as u16).to_be_bytes());
    labeled.extend_from_slice(b"HPKE-v1");
    labeled.extend_from_slice(suite);
    labeled.extend_from_slice(label);
    labeled.extend_from_slice(info);
    prp::hkdf_expand(secret, &labeled, output)
}

fn hpke_seal(
    recipient_public: &[u8; 32],
    aad: &[u8],
    plaintext: &[u8],
) -> Result<([u8; 32], Vec<u8>), i32> {
    const KEM_SUITE: &[u8] = b"KEM\x00\x20";
    const HPKE_SUITE: &[u8] = b"HPKE\x00\x20\x00\x01\x00\x03";
    let mut ephemeral_private = [0u8; 32];
    if !prp::random_bytes(&mut ephemeral_private) {
        return Err(-3);
    }
    let encapsulated = prp::x25519(&ephemeral_private, &prp::X25519_BASEPOINT);
    let mut dh = prp::x25519(&ephemeral_private, recipient_public);
    ephemeral_private.zeroize();
    if dh == [0u8; 32] {
        dh.zeroize();
        return Err(-6);
    }
    let mut eae_prk = labeled_extract(&[], KEM_SUITE, b"eae_prk", &dh);
    dh.zeroize();
    let mut kem_context = [0u8; 64];
    kem_context[..32].copy_from_slice(&encapsulated);
    kem_context[32..].copy_from_slice(recipient_public);
    let mut shared_secret = [0u8; 32];
    if !labeled_expand(
        &eae_prk,
        KEM_SUITE,
        b"shared_secret",
        &kem_context,
        &mut shared_secret,
    ) {
        eae_prk.zeroize();
        return Err(-3);
    }
    eae_prk.zeroize();

    let psk_id_hash = labeled_extract(&[], HPKE_SUITE, b"psk_id_hash", &[]);
    let info_hash = labeled_extract(&[], HPKE_SUITE, b"info_hash", HPKE_INFO);
    let mut context = [0u8; 65];
    context[1..33].copy_from_slice(&psk_id_hash);
    context[33..].copy_from_slice(&info_hash);
    let mut secret = labeled_extract(&shared_secret, HPKE_SUITE, b"secret", &[]);
    shared_secret.zeroize();
    let mut key = [0u8; 32];
    let mut nonce = [0u8; 12];
    if !labeled_expand(&secret, HPKE_SUITE, b"key", &context, &mut key)
        || !labeled_expand(&secret, HPKE_SUITE, b"base_nonce", &context, &mut nonce)
    {
        secret.zeroize();
        key.zeroize();
        return Err(-3);
    }
    secret.zeroize();
    let mut ciphertext = plaintext.to_vec();
    let tag = prp::aead_encrypt(&key, &nonce, aad, &mut ciphertext).ok_or(-3)?;
    key.zeroize();
    ciphertext.extend_from_slice(&tag);
    Ok((encapsulated, ciphertext))
}

fn hpke_open(
    recipient_private: &[u8; 32],
    recipient_public: &[u8; 32],
    encapsulated: &[u8; 32],
    aad: &[u8],
    ciphertext: &[u8],
) -> Result<Vec<u8>, i32> {
    const KEM_SUITE: &[u8] = b"KEM\x00\x20";
    const HPKE_SUITE: &[u8] = b"HPKE\x00\x20\x00\x01\x00\x03";
    if ciphertext.len() < 16 {
        return Err(-6);
    }
    let mut dh = prp::x25519(recipient_private, encapsulated);
    if dh == [0u8; 32] {
        dh.zeroize();
        return Err(-6);
    }
    let mut eae_prk = labeled_extract(&[], KEM_SUITE, b"eae_prk", &dh);
    dh.zeroize();
    let mut kem_context = [0u8; 64];
    kem_context[..32].copy_from_slice(encapsulated);
    kem_context[32..].copy_from_slice(recipient_public);
    let mut shared_secret = [0u8; 32];
    if !labeled_expand(
        &eae_prk,
        KEM_SUITE,
        b"shared_secret",
        &kem_context,
        &mut shared_secret,
    ) {
        eae_prk.zeroize();
        return Err(-3);
    }
    eae_prk.zeroize();
    let psk_id_hash = labeled_extract(&[], HPKE_SUITE, b"psk_id_hash", &[]);
    let info_hash = labeled_extract(&[], HPKE_SUITE, b"info_hash", HPKE_INFO);
    let mut context = [0u8; 65];
    context[1..33].copy_from_slice(&psk_id_hash);
    context[33..].copy_from_slice(&info_hash);
    let mut secret = labeled_extract(&shared_secret, HPKE_SUITE, b"secret", &[]);
    shared_secret.zeroize();
    let mut key = [0u8; 32];
    let mut nonce = [0u8; 12];
    if !labeled_expand(&secret, HPKE_SUITE, b"key", &context, &mut key)
        || !labeled_expand(&secret, HPKE_SUITE, b"base_nonce", &context, &mut nonce)
    {
        secret.zeroize();
        key.zeroize();
        return Err(-3);
    }
    secret.zeroize();

    let data_length = ciphertext.len() - 16;
    let mut plaintext = ciphertext[..data_length].to_vec();
    let mut tag = [0u8; 16];
    tag.copy_from_slice(&ciphertext[data_length..]);
    let opened = prp::aead_decrypt(&key, &nonce, aad, &mut plaintext, &tag);
    key.zeroize();
    if !opened {
        plaintext.zeroize();
        return Err(-6);
    }
    Ok(plaintext)
}

unsafe fn active_profile() -> Option<&'static Profile> {
    let profiles = PROFILES.as_ref()?;
    profiles.get(ACTIVE_PROFILE)
}

unsafe fn active_profile_mut() -> Option<&'static mut Profile> {
    let profiles = PROFILES.as_mut()?;
    profiles.get_mut(ACTIVE_PROFILE)
}

#[no_mangle]
/// Selects or creates the active profile for a server address.
///
/// # Safety
///
/// `address` must point to a readable, NUL-terminated string.
pub unsafe extern "C" fn rust_rchat_use_server(
    address: *const u8,
    trust_server_cert: i32,
) -> i32 {
    let address = match c_string(address, SERVER_ADDRESS_MAX) {
        Ok(address) => address,
        Err(()) => return -1,
    };
    let trust_server_cert = trust_server_cert != 0;
    let address = normalize_server_address(address);
    let url = match Url::parse(address) {
        Ok(url) => url,
        Err(_) => return -1,
    };
    let key = match profile_key(address, &url) {
        Ok(key) => key,
        Err(()) => return -1,
    };
    let profiles = PROFILES.get_or_insert_with(Vec::new);
    if let Some(index) = profiles.iter().position(|profile| profile.key == key) {
        profiles[index].url = url;
        profiles[index].trust_server_cert = trust_server_cert;
        ACTIVE_PROFILE = index;
        return 0;
    }
    if profiles.len() == MAX_PROFILES {
        return -2;
    }
    let mut profile = match new_profile(key, url) {
        Ok(profile) => profile,
        Err(()) => return -3,
    };
    profile.trust_server_cert = trust_server_cert;
    if profiles.iter().any(|existing| {
        profile.auth_private == existing.auth_private
            || profile.auth_private == existing.encryption_private
            || profile.encryption_private == existing.auth_private
            || profile.encryption_private == existing.encryption_private
    }) {
        return -3;
    }
    profiles.push(profile);
    ACTIVE_PROFILE = profiles.len() - 1;
    0
}

unsafe fn write_encoded(value: &[u8], output: *mut u8, capacity: u32) -> i32 {
    if output.is_null() {
        return -1;
    }
    let output = slice::from_raw_parts_mut(output, capacity as usize);
    base64url(value, output).map(|_| 0).unwrap_or(-1)
}

#[no_mangle]
pub unsafe extern "C" fn rust_rchat_client_id(output: *mut u8, capacity: u32) -> i32 {
    match active_profile() {
        Some(profile) => write_encoded(&profile.client_id, output, capacity),
        None => -2,
    }
}

#[no_mangle]
pub unsafe extern "C" fn rust_rchat_auth_public(output: *mut u8, capacity: u32) -> i32 {
    match active_profile() {
        Some(profile) => write_encoded(&profile.auth_public, output, capacity),
        None => -2,
    }
}

#[no_mangle]
pub unsafe extern "C" fn rust_rchat_encryption_public(output: *mut u8, capacity: u32) -> i32 {
    match active_profile() {
        Some(profile) => write_encoded(&profile.encryption_public, output, capacity),
        None => -2,
    }
}

#[no_mangle]
pub extern "C" fn rust_rchat_self_test() -> bool {
    let input = [0xa5; 128];
    let encoded = encoded(&input);
    encoded.len() == 171
        && decode_base64url_vec(&encoded, input.len()).as_deref() == Some(input.as_slice())
}

#[no_mangle]
/// Enrolls the active profile with a server-issued invite code.
///
/// # Safety
///
/// `invite` must point to a readable, NUL-terminated string.
pub unsafe extern "C" fn rust_rchat_enroll(invite: *const u8) -> i32 {
    let invite = match c_string(invite, 64) {
        Ok(invite)
            if (12..=64).contains(&invite.len())
                && invite.bytes().all(|byte| (32..=126).contains(&byte)) => invite,
        _ => return -1,
    };
    let profile = match active_profile_mut() {
        Some(profile) => profile,
        None => return -2,
    };

    let mut registration_input = Vec::with_capacity(18 + 2 + invite.len() + 32 + 32 + 64);
    registration_input.extend_from_slice(REGISTRATION_PREFIX);
    registration_input.extend_from_slice(&(invite.len() as u16).to_be_bytes());
    registration_input.extend_from_slice(invite.as_bytes());
    registration_input.extend_from_slice(&profile.auth_public);
    registration_input.extend_from_slice(&profile.encryption_public);
    registration_input.extend_from_slice(&profile.encryption_key_signature);
    let registration_signature = prp::ed25519_sign(&profile.auth_private, &registration_input);
    registration_input.zeroize();

    let mut escaped_invite = json_escape(invite);
    let body = format!(
        "{{\"invite_code\":\"{}\",\"auth_public_key\":\"{}\",\"encryption_public_key\":\"{}\",\"encryption_key_signature\":\"{}\",\"registration_signature\":\"{}\"}}",
        escaped_invite,
        encoded(&profile.auth_public),
        encoded(&profile.encryption_public),
        encoded(&profile.encryption_key_signature),
        encoded(&registration_signature),
    );
    escaped_invite.zeroize();
    let response = match api_request(profile, "POST", "/v1/clients", Some(&body), None) {
        Ok(response) => response,
        Err(()) => return -4,
    };
    if response.status != 200 && response.status != 201 {
        if response.body.windows(14).any(|window| window == b"invalid_invite") {
            return -7;
        }
        if response.body.windows(17).any(|window| window == b"invalid_signature") {
            return -8;
        }
        return -5;
    }
    let returned_id = match json_string_field(&response.body, "client_id")
        .and_then(decode_base64url::<16>)
    {
        Some(id) => id,
        None => return -6,
    };
    if returned_id != profile.client_id {
        return -6;
    }
    profile.enrolled = true;
    0
}

#[no_mangle]
pub unsafe extern "C" fn rust_rchat_is_enrolled() -> bool {
    active_profile().map(|profile| profile.enrolled).unwrap_or(false)
}

#[no_mangle]
pub unsafe extern "C" fn rust_rchat_check_contact(client_id: *const u8) -> i32 {
    let client_id = match c_string(client_id, 22)
        .ok()
        .and_then(decode_base64url::<16>)
    {
        Some(client_id) => client_id,
        None => return -1,
    };
    let profile = match active_profile_mut() {
        Some(profile) => profile,
        None => return -2,
    };
    lookup_recipient(profile, &client_id).map(|_| 0).unwrap_or_else(|error| error)
}

#[no_mangle]
pub unsafe extern "C" fn rust_rchat_send(client_id: *const u8, text: *const u8) -> i32 {
    let recipient_id = match c_string(client_id, 22)
        .ok()
        .and_then(decode_base64url::<16>)
    {
        Some(client_id) => client_id,
        None => return -1,
    };
    let text = match c_string(text, 4096) {
        Ok(text) if !text.is_empty() && text.len() <= 4096 => text,
        _ => return -1,
    };
    let profile = match active_profile_mut() {
        Some(profile) => profile,
        None => return -2,
    };
    let recipient = match lookup_recipient(profile, &recipient_id) {
        Ok(recipient) => recipient,
        Err(error) => return error,
    };

    let mut client_message_id = [0u8; 16];
    if !prp::random_bytes(&mut client_message_id) {
        return -3;
    }
    let mut aad = [0u8; 65];
    aad[..MESSAGE_PREFIX.len()].copy_from_slice(MESSAGE_PREFIX);
    aad[MESSAGE_PREFIX.len()..MESSAGE_PREFIX.len() + 16]
        .copy_from_slice(&profile.client_id);
    aad[MESSAGE_PREFIX.len() + 16..MESSAGE_PREFIX.len() + 32]
        .copy_from_slice(&recipient_id);
    aad[MESSAGE_PREFIX.len() + 32..].copy_from_slice(&client_message_id);

    let sent_at = match Rtc.current_time() {
        Some(time) => time.as_secs(),
        None => return -4,
    };
    let mut escaped_text = json_escape(text);
    let mut payload = format!("{{\"sent_at\":{sent_at},\"text\":\"{escaped_text}\"}}");
    escaped_text.zeroize();
    let mut signed = Vec::with_capacity(aad.len() + payload.len());
    signed.extend_from_slice(&aad);
    signed.extend_from_slice(payload.as_bytes());
    let signature = prp::ed25519_sign(&profile.auth_private, &signed);
    let mut plaintext = Vec::with_capacity(4 + payload.len() + signature.len());
    plaintext.extend_from_slice(&(payload.len() as u32).to_be_bytes());
    plaintext.extend_from_slice(payload.as_bytes());
    plaintext.extend_from_slice(&signature);
    let encrypted = hpke_seal(
        &recipient.encryption_public,
        &aad,
        &plaintext,
    );
    signed.zeroize();
    plaintext.zeroize();
    payload.zeroize();
    let (encapsulated, ciphertext) = match encrypted {
        Ok(message) => message,
        Err(error) => return error,
    };
    let body = format!(
        "{{\"recipient_id\":\"{}\",\"client_message_id\":\"{}\",\"enc\":\"{}\",\"ciphertext\":\"{}\"}}",
        encoded(&recipient_id),
        encoded(&client_message_id),
        encoded(&encapsulated),
        encoded(&ciphertext),
    );
    let response = match authenticated_request(profile, "POST", "/v1/messages", Some(&body)) {
        Ok(response) => response,
        Err(error) => return error,
    };
    if response.status == 200 || response.status == 201 { 0 } else { -5 }
}

unsafe fn write_text(value: &str, output: *mut u8, capacity: u32) -> Result<(), i32> {
    if output.is_null() || value.len() + 1 > capacity as usize {
        return Err(-1);
    }
    core::ptr::copy_nonoverlapping(value.as_ptr(), output, value.len());
    *output.add(value.len()) = 0;
    Ok(())
}

fn acknowledge_pending(profile: &mut Profile) -> Result<(), i32> {
    if profile.pending_server_message_id.is_empty() {
        return Ok(());
    }
    let body = format!(
        "{{\"server_message_ids\":[\"{}\"]}}",
        json_escape(&profile.pending_server_message_id)
    );
    let response = authenticated_request(profile, "POST", "/v1/messages/ack", Some(&body))?;
    if response.status != 200 {
        return Err(-5);
    }
    profile.cursor = core::mem::take(&mut profile.pending_cursor);
    profile.pending_server_message_id.clear();
    Ok(())
}

#[no_mangle]
pub unsafe extern "C" fn rust_rchat_ack_pending() -> i32 {
    match active_profile_mut() {
        Some(profile) => acknowledge_pending(profile).map(|_| 0).unwrap_or_else(|error| error),
        None => -2,
    }
}

#[no_mangle]
pub unsafe extern "C" fn rust_rchat_poll(
    sender_output: *mut u8,
    sender_capacity: u32,
    text_output: *mut u8,
    text_capacity: u32,
) -> i32 {
    let profile = match active_profile_mut() {
        Some(profile) => profile,
        None => return -2,
    };
    if !profile.pending_server_message_id.is_empty() {
        return -7;
    }
    let path = if profile.cursor.is_empty() {
        "/v1/messages?limit=1".into()
    } else {
        format!("/v1/messages?after={}&limit=1", profile.cursor)
    };
    let response = match authenticated_request(profile, "GET", &path, None) {
        Ok(response) => response,
        Err(error) => return error,
    };
    if response.status != 200 {
        return -5;
    }
    let cursor = match json_string_field(&response.body, "cursor") {
        Some(cursor) if cursor.len() <= 128 && is_base64url(cursor) => cursor.to_string(),
        _ => return -6,
    };
    let object = match first_message(&response.body) {
        Some(Some(object)) => object,
        Some(None) => {
            profile.cursor = cursor;
            return 1;
        }
        None => return -6,
    };
    let server_message_id = match json_string_field(object, "server_message_id") {
        Some(id) if id.len() <= 128 && is_base64url(id) => id,
        _ => return -6,
    };
    let sender_id = match json_string_field(object, "sender_id")
        .and_then(decode_base64url::<16>)
    {
        Some(id) => id,
        None => return -6,
    };
    let client_message_id = match json_string_field(object, "client_message_id")
        .and_then(decode_base64url::<16>)
    {
        Some(id) => id,
        None => return -6,
    };
    let encapsulated = match json_string_field(object, "enc")
        .and_then(decode_base64url::<32>)
    {
        Some(encapsulated) => encapsulated,
        None => return -6,
    };
    let ciphertext = match json_string_field(object, "ciphertext")
        .and_then(|value| decode_base64url_vec(value, 24_692))
    {
        Some(ciphertext) => ciphertext,
        None => return -6,
    };
    let sender = match lookup_recipient(profile, &sender_id) {
        Ok(sender) => sender,
        Err(error) => return error,
    };
    let mut aad = [0u8; 65];
    aad[..MESSAGE_PREFIX.len()].copy_from_slice(MESSAGE_PREFIX);
    aad[MESSAGE_PREFIX.len()..MESSAGE_PREFIX.len() + 16].copy_from_slice(&sender_id);
    aad[MESSAGE_PREFIX.len() + 16..MESSAGE_PREFIX.len() + 32]
        .copy_from_slice(&profile.client_id);
    aad[MESSAGE_PREFIX.len() + 32..].copy_from_slice(&client_message_id);
    let mut plaintext = match hpke_open(
        &profile.encryption_private,
        &profile.encryption_public,
        &encapsulated,
        &aad,
        &ciphertext,
    ) {
        Ok(plaintext) => plaintext,
        Err(error) => return error,
    };
    if plaintext.len() < 68 {
        plaintext.zeroize();
        return -6;
    }
    let payload_length = u32::from_be_bytes([
        plaintext[0],
        plaintext[1],
        plaintext[2],
        plaintext[3],
    ]) as usize;
    if payload_length > 24_608 || plaintext.len() != 4 + payload_length + 64 {
        plaintext.zeroize();
        return -6;
    }
    let payload = &plaintext[4..4 + payload_length];
    let mut signature = [0u8; 64];
    signature.copy_from_slice(&plaintext[4 + payload_length..]);
    let mut signed = Vec::with_capacity(aad.len() + payload.len());
    signed.extend_from_slice(&aad);
    signed.extend_from_slice(payload);
    let verified = prp::ed25519_verify(&sender.auth_public, &signed, &signature);
    signed.zeroize();
    if !verified {
        plaintext.zeroize();
        return -6;
    }
    let mut text = match json_unescaped_field(payload, "text") {
        Some(text) if !text.is_empty() && text.len() <= 4096 && !text.contains('\0') => text,
        _ => {
            plaintext.zeroize();
            return -6;
        }
    };
    plaintext.zeroize();

    let sender_id = encoded(&sender_id);
    let written = write_text(&sender_id, sender_output, sender_capacity).is_ok()
        && write_text(&text, text_output, text_capacity).is_ok();
    text.zeroize();
    if !written {
        return -1;
    }
    profile.pending_server_message_id = server_message_id.into();
    profile.pending_cursor = cursor;
    0
}
