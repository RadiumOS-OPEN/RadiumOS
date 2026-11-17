use super::{
    configuration, configuration_insecure, exchange_plain, exchange_tls, get, get_plain, get_plain_range, get_range,
    native::{Rtc, Tcp},
    options::{Options, HELP},
    public_roots, Error, Url,
};
use alloc::{format, sync::Arc, vec, vec::Vec};
use core::{
    slice, str,
    sync::atomic::{AtomicBool, Ordering},
};
use rustls::{
    pki_types::{pem::PemObject, CertificateDer},
    time_provider::TimeProvider,
    RootCertStore,
};

pub(super) static BUSY: AtomicBool = AtomicBool::new(false);

extern "C" {
    fn avfs_save_file(name: *const u8, data: *const u8, size: u32, overwrite: bool) -> i32;
    fn avfs_get_cwd() -> *const u8;
    static terminal_column: usize;
}

struct CommandGuard(u32);

impl Drop for CommandGuard {
    fn drop(&mut self) {
        if self.0 & (1 << 9) == 0 {
            unsafe {
                core::arch::asm!("cli");
            }
        }
        crate::FETCH_NETWORK_SILENT.store(false, Ordering::Relaxed);
        BUSY.store(false, Ordering::Release);
    }
}

fn display(bytes: &[u8]) {
    for &byte in bytes {
        if byte != b'\n' && byte != b'\r' && unsafe { terminal_column } >= 44 {
            unsafe {
                crate::terminal_putchar(b'\n');
            }
        }
        if byte == b'\n' || byte == b'\t' || (32..127).contains(&byte) {
            unsafe {
                crate::terminal_putchar(byte);
            }
        } else if byte != b'\r' {
            unsafe {
                crate::terminal_putchar(b'.');
            }
        }
    }
}

unsafe fn argument<'a>(pointer: *const u8) -> Result<&'a str, Error> {
    if pointer.is_null() {
        return Err(Error::Message("null argument"));
    }
    for len in 0..4096 {
        if *pointer.add(len) == 0 {
            return str::from_utf8(slice::from_raw_parts(pointer, len))
                .map_err(|_| Error::Message("invalid UTF-8 argument"));
        }
    }
    Err(Error::Message("argument too long"))
}

fn filename(name: &str) -> Result<Vec<u8>, Error> {
    let cwd = unsafe { argument(avfs_get_cwd())? };
    if name.is_empty()
        || name.len()
            + if name.starts_with('/') {
                0
            } else {
                cwd.len() + 1
            }
            >= 512
        || name.bytes().any(|byte| byte < 32 || byte == 127)
        || name.split('/').any(|part| part.len() >= 128)
        || name.split('/').count() + cwd.split('/').count() >= 64
    {
        return Err(Error::Message("invalid or oversized AVFS path"));
    }
    let mut result = name.as_bytes().to_vec();
    result.push(0);
    Ok(result)
}

fn roots(path: Option<&str>) -> Result<RootCertStore, Error> {
    let Some(path) = path else {
        return Ok(public_roots());
    };
    let name = filename(path)?;
    let size = unsafe { crate::avfs_get_filesize(name.as_ptr()) };
    if size <= 0 || size > 256 * 1024 {
        return Err(Error::Message(
            "CA file missing, empty, or larger than 256 KiB",
        ));
    }
    let mut data = vec![0u8; size as usize];
    if unsafe { crate::avfs_read_file(name.as_ptr(), data.as_mut_ptr(), size as u32, 0) } != 0 {
        return Err(Error::Message("cannot read CA file"));
    }
    let mut roots = RootCertStore::empty();
    if data.starts_with(b"-----BEGIN") {
        for cert in CertificateDer::pem_slice_iter(&data) {
            roots.add(cert.map_err(|_| Error::Message("invalid PEM CA certificate"))?)?;
        }
    } else {
        roots.add(CertificateDer::from(data))?;
    }
    if roots.is_empty() {
        return Err(Error::Message("CA file contains no certificates"));
    }
    Ok(roots)
}

const DISK_RANGE_BYTES: usize = 16 * 1024 * 1024;
const FALLBACK_BODY_BYTES: u64 = 64 * 1024 * 1024;
const MEMORY_RESPONSE_LIMIT: usize = 16 * 1024 * 1024;

fn request_once(
    url: &Url,
    head: bool,
    range: Option<(u64, u64)>,
    max_size: usize,
    options: &Options,
    config: &mut Option<Arc<rustls::ClientConfig>>,
) -> Result<super::Response, Error> {
    let mut tcp = Tcp::connect(&url.host, url.port, options.timeout.saturating_mul(1000))?;
    let response = if url.is_https {
        if config.is_none() {
            config.replace(if options.insecure {
                configuration_insecure(Arc::new(Rtc))?
            } else {
                if Rtc.current_time().is_none() {
                    return Err(Error::Message(
                        "RTC unavailable or invalid; cannot check certificate dates",
                    ));
                }
                configuration(Arc::new(Rtc), roots(options.cacert.as_deref())?)?
            });
        }
        if let Some((start, end)) = range {
            get_range(&mut tcp, url, config.as_ref().unwrap().clone(), start, end, max_size)?
        } else {
            get(&mut tcp, url, config.as_ref().unwrap().clone(), head, max_size)?
        }
    } else if let Some((start, end)) = range {
        get_plain_range(&mut tcp, url, start, end, max_size)?
    } else {
        get_plain(&mut tcp, url, head, max_size)?
    };
    drop(tcp);
    Ok(response)
}

fn request_following_redirects(
    url: &mut super::Url,
    head: bool,
    range: Option<(u64, u64)>,
    max_size: usize,
    options: &Options,
    config: &mut Option<Arc<rustls::ClientConfig>>,
) -> Result<super::Response, Error> {
    let mut redirects = 0;
    loop {
        let response = request_once(url, head, range, max_size, options, config)?;
        if options.location && [301, 302, 303, 307, 308].contains(&response.status) {
            if redirects == 5 {
                return Err(Error::Message("too many redirects; limit is five"));
            }
            let location = response.location.as_deref()
                .ok_or(Error::Message("redirect has no Location header"))?;
            *url = url.redirect(location)?;
            redirects += 1;
            continue;
        }
        return Ok(response);
    }
}

fn fetch_to_disk(url: &mut Url, path: &str, options: &Options) -> Result<(), Error> {
    if options.head || options.include {
        return Err(Error::Message("--head/--include cannot be used with /disk/ output"));
    }
    let existing = crate::disk_store::file_size(path).map_err(Error::Message)?;
    if existing.is_some() && !options.overwrite {
        return Err(Error::Message("disk output exists; use --overwrite to replace it"));
    }

    crate::FETCH_NETWORK_SILENT.store(true, Ordering::Relaxed);
    let mut config = None;
    let head = request_following_redirects(
        url, true, None, 16 * 1024, options, &mut config,
    )?;
    if options.fail && head.status >= 400 {
        return Err(Error::Message("HTTP request failed; output was not written"));
    }
    if head.status != 200 {
        return Err(Error::Message("server did not provide a usable HEAD response"));
    }
    let total = head.content_length.ok_or(Error::Message("server omitted Content-Length"))?;
    if total == 0 {
        return Err(Error::Message("server reported an empty file"));
    }
    if total > crate::disk_store::capacity_bytes() {
        return Err(Error::Message("file is larger than the disk"));
    }
    if total > u32::MAX as u64 {
        return Err(Error::Message("file is larger than the 4 GiB file limit"));
    }
    // Saving to the disk has no size cap of its own: only the disk's capacity (or an explicit --max-size) refuses a file.
    if total > crate::disk_store::free_bytes().map_err(Error::Message)? {
        return Err(Error::Message("not enough free space on the disk"));
    }
    if options.max_size_set && total > options.max_size as u64 {
        return Err(Error::Message("download size exceeds --max-size"));
    }

    let mut writer = crate::disk_store::DiskWriter::begin(path).map_err(Error::Message)?;
    let mut offset = 0u64;
    while offset < total {
        let count = core::cmp::min(DISK_RANGE_BYTES as u64, total - offset);
        let end = offset + count - 1;
        // The first request may be answered with the whole file by a server that ignores Range.
        let limit = (if offset == 0 { core::cmp::min(total, FALLBACK_BODY_BYTES).max(count) } else { count }) as usize;
        let response = request_following_redirects(
            url, false, Some((offset, end)), limit, options, &mut config,
        )?;
        if options.fail && response.status >= 400 {
            return Err(Error::Message("HTTP range request failed"));
        }
        // A server without Range support answers 200 with the whole file.
        if response.status == 200 && offset == 0 && response.body.len() as u64 == total {
            writer.write(&response.body).map_err(Error::Message)?;
            offset = total;
            break;
        }
        if response.status != 206
            || response.content_range != Some((offset, end, total))
            || response.body.len() as u64 != count
        {
            return Err(Error::Message("server returned an invalid or incomplete byte range"));
        }
        writer.write(&response.body).map_err(Error::Message)?;
        offset += count;
    }
    writer.finish().map_err(Error::Message)?;
    if !options.silent {
        display(format!("fetch: HTTP 200, {} bytes saved to {}\n", total, path).as_bytes());
    }
    Ok(())
}

/// Executes one fetch invocation from already-decoded command arguments.
fn execute(args: &[&str]) -> Result<(), Error> {
    if args == ["--test-disk"] {
        crate::disk_store::self_test().map_err(Error::Message)?;
        display(b"disk: read/write self-test passed\n");
        return Ok(());
    }
    let options = Options::parse(args)?;
    if options.help {
        display(HELP.as_bytes());
        return Ok(());
    }
    let mut url = options.url.clone().ok_or(Error::Message("missing URL"))?;
    if let Some(path) = options.output.as_deref().filter(|path| path.starts_with("/disk/")) {
        return fetch_to_disk(&mut url, path, &options);
    }
    if options.max_size > MEMORY_RESPONSE_LIMIT {
        return Err(Error::Message("responses above 16 MiB require --output /disk/<file>"));
    }
    let output = options.output.as_deref().map(filename).transpose()?;
    if let Some(name) = &output {
        if !options.overwrite && unsafe { crate::avfs_file_exists(name.as_ptr()) } {
            return Err(Error::Message(
                "output file exists; use --overwrite to replace it",
            ));
        }
    }
    let mut config = None;
    let started = unsafe { crate::get_ticks() };
    let timeout = options.timeout * 1000;
    crate::FETCH_NETWORK_SILENT.store(true, Ordering::Relaxed);
    let mut redirects = 0;
    let response = loop {
        let remaining = timeout.saturating_sub(unsafe { crate::get_ticks() }.wrapping_sub(started));
        if remaining == 0 {
            return Err(Error::Message("fetch timed out"));
        }
        let mut tcp = Tcp::connect(&url.host, url.port, remaining)?;
        let response = if url.is_https {
            if config.is_none() {
                config = Some(if options.insecure {
                    configuration_insecure(Arc::new(Rtc))?
                } else {
                    if Rtc.current_time().is_none() {
                        return Err(Error::Message(
                            "RTC unavailable or invalid; cannot check certificate dates",
                        ));
                    }
                    configuration(Arc::new(Rtc), roots(options.cacert.as_deref())?)?
                });
            }
            get(
                &mut tcp,
                &url,
                config.as_ref().unwrap().clone(),
                options.head,
                options.max_size,
            )?
        } else {
            get_plain(&mut tcp, &url, options.head, options.max_size)?
        };
        drop(tcp);
        if options.location && [301, 302, 303, 307, 308].contains(&response.status) {
            if redirects == 5 {
                return Err(Error::Message("too many redirects; limit is five"));
            }
            let location = response
                .location
                .as_deref()
                .ok_or(Error::Message("redirect has no Location header"))?;
            url = url.redirect(location)?;
            redirects += 1;
            continue;
        }
        break response;
    };
    if options.fail && response.status >= 400 {
        display(format!("fetch: HTTP {}\n", response.status).as_bytes());
        return Err(Error::Message(
            "HTTP request failed; output was not written",
        ));
    }
    let data = if options.include || options.head {
        let mut data = response.headers;
        data.extend_from_slice(&response.body);
        data
    } else {
        response.body
    };
    if let Some(name) = output {
        if unsafe {
            avfs_save_file(
                name.as_ptr(),
                data.as_ptr(),
                data.len() as u32,
                options.overwrite,
            )
        } != 0
        {
            return Err(Error::Message(
                "cannot save output; check free space, directory, and overwrite flag",
            ));
        }
    } else {
        display(&data);
        if !data.ends_with(b"\n") {
            display(b"\n");
        }
    }
    if !options.silent {
        display(format!("fetch: HTTP {}, {} bytes\n", response.status, data.len()).as_bytes());
    }
    Ok(())
}

#[no_mangle]
pub unsafe extern "C" fn rust_fetch(argc: i32, argv: *const *const u8) -> i32 {
    if BUSY
        .compare_exchange(false, true, Ordering::Acquire, Ordering::Relaxed)
        .is_err()
    {
        display(b"fetch: another fetch is running\n");
        return -1;
    }
    let flags: u32;
    core::arch::asm!("pushfd", "pop {}", out(reg) flags);
    let _guard = CommandGuard(flags);
    core::arch::asm!("sti");
    let result = (|| {
        if argc < 1 || argc > 32 || argv.is_null() {
            return Err(Error::Message("invalid arguments"));
        }
        let mut args = Vec::new();
        for index in 1..argc as usize {
            args.push(argument(*argv.add(index))?);
        }
        execute(&args)
    })();
    match result {
        Ok(()) => 0,
        Err(error) => {
            display(format!("fetch: {error}\n").as_bytes());
            -1
        }
    }
}

/// Ctrl+C threw a running fetch away without unwinding it.
#[no_mangle]
pub extern "C" fn rust_fetch_abort() {
    crate::FETCH_NETWORK_SILENT.store(false, Ordering::Relaxed);
    BUSY.store(false, Ordering::Release);
}

#[no_mangle]
pub extern "C" fn rust_fetch_active() -> bool {
    BUSY.load(Ordering::Acquire)
}

#[no_mangle]
pub unsafe extern "C" fn rust_https_get(url: *const u8) -> i32 {
    rust_fetch(2, [b"fetch\0".as_ptr(), url].as_ptr())
}

#[no_mangle]
pub unsafe extern "C" fn rust_fetch_to_file(url: *const u8, output: *const u8) -> i32 {
    let args = [
        b"fetch\0".as_ptr(),
        b"--output\0".as_ptr(),
        output,
        b"--overwrite\0".as_ptr(),
        b"--location\0".as_ptr(),
        b"--fail\0".as_ptr(),
        b"--silent\0".as_ptr(),
        b"--max-size\0".as_ptr(),
        b"786432\0".as_ptr(),
        url,
    ];
    rust_fetch(args.len() as i32, args.as_ptr())
}

#[no_mangle]
pub unsafe extern "C" fn rust_test_https() -> i32 {
    rust_https_get(b"https://example.com/\0".as_ptr())
}

static mut HTTP_LAST_ERROR: [u8; 96] = [0; 96];

pub(super) unsafe fn remember_error(text: &str) {
    let bytes = text.as_bytes();
    let n = core::cmp::min(bytes.len(), HTTP_LAST_ERROR.len() - 1);
    HTTP_LAST_ERROR[..n].copy_from_slice(&bytes[..n]);
    HTTP_LAST_ERROR[n] = 0;
}

/// Copies the message of the last failed rust_http_request into `out` (NUL-terminated).
#[no_mangle]
pub unsafe extern "C" fn rust_http_error(out: *mut u8, cap: u32) {
    if out.is_null() || cap == 0 {
        return;
    }
    let mut i = 0usize;
    while i + 1 < cap as usize && HTTP_LAST_ERROR[i] != 0 {
        *out.add(i) = HTTP_LAST_ERROR[i];
        i += 1;
    }
    *out.add(i) = 0;
}

fn build_request(method: &str, url: &Url, headers: &str, body: &[u8]) -> Result<Vec<u8>, Error> {
    if method.is_empty() || !method.bytes().all(|b| b.is_ascii_uppercase()) {
        return Err(Error::Message("invalid HTTP method"));
    }
    let mut custom = alloc::string::String::new();
    let mut has_agent = false;
    for line in headers.split('\n') {
        let line = line.trim();
        if line.is_empty() {
            continue;
        }
        if line.bytes().any(|b| b < 32 || b == 127) || !line.contains(':') {
            return Err(Error::Message("invalid header line"));
        }
        if line.len() >= 11 && line[..11].eq_ignore_ascii_case("user-agent:") {
            has_agent = true;
        }
        custom.push_str(line);
        custom.push_str("\r\n");
    }
    let mut text = format!(
        "{} {} HTTP/1.1\r\nHost: {}\r\nAccept: application/json\r\nAccept-Encoding: identity\r\n",
        method, url.path, url.authority()
    );
    if !has_agent {
        text.push_str("User-Agent: RadiumOS-http/1.0\r\n");
    }
    text.push_str(&custom);
    if !body.is_empty() || matches!(method, "POST" | "PUT" | "PATCH") {
        text.push_str(&format!("Content-Length: {}\r\n", body.len()));
    }
    text.push_str("Connection: close\r\n\r\n");
    let mut bytes = text.into_bytes();
    bytes.extend_from_slice(body);
    Ok(bytes)
}

/// One HTTP or HTTPS request with a method, extra headers ("Name: value" lines separated by \n) and a body, for
/// JSON APIs. The response body is copied into `out` (at most `out_cap` bytes) and its length returned;
/// *status_out gets the HTTP status. -1 on a transport or protocol error (rust_http_error says why).
#[no_mangle]
pub unsafe extern "C" fn rust_http_request(
    method: *const u8,
    url: *const u8,
    headers: *const u8,
    body: *const u8,
    body_len: u32,
    out: *mut u8,
    out_cap: u32,
    status_out: *mut i32,
) -> i32 {
    if method.is_null() || url.is_null() || out.is_null() {
        return -1;
    }
    if !status_out.is_null() {
        *status_out = 0;
    }
    if BUSY
        .compare_exchange(false, true, Ordering::Acquire, Ordering::Relaxed)
        .is_err()
    {
        remember_error("another network request is running");
        return -1;
    }
    let flags: u32;
    core::arch::asm!("pushfd", "pop {}", out(reg) flags);
    let _guard = CommandGuard(flags);
    core::arch::asm!("sti");
    crate::FETCH_NETWORK_SILENT.store(true, Ordering::Relaxed);

    let result = (|| -> Result<super::Response, Error> {
        let method = argument(method)?;
        let url = Url::parse(argument(url)?)?;
        let headers = if headers.is_null() { "" } else { argument(headers)? };
        let body: &[u8] = if body.is_null() || body_len == 0 { &[] } else { slice::from_raw_parts(body, body_len as usize) };
        let request = build_request(method, &url, headers, body)?;
        let mut tcp = Tcp::connect(&url.host, url.port, 30_000)?;
        let response = if url.is_https {
            if Rtc.current_time().is_none() {
                return Err(Error::Message("RTC unavailable or invalid; cannot check certificate dates"));
            }
            let config = configuration(Arc::new(Rtc), roots(None)?)?;
            exchange_tls(&mut tcp, &url, config, &request, false, 1024 * 1024)?
        } else {
            exchange_plain(&mut tcp, &request, false, 1024 * 1024)?
        };
        drop(tcp);
        Ok(response)
    })();

    match result {
        Ok(response) => {
            let n = core::cmp::min(response.body.len(), out_cap as usize);
            core::ptr::copy_nonoverlapping(response.body.as_ptr(), out, n);
            if !status_out.is_null() {
                *status_out = response.status as i32;
            }
            n as i32
        }
        Err(error) => {
            remember_error(&format!("{error}"));
            -1
        }
    }
}
