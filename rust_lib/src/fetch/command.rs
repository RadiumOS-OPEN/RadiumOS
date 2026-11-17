use super::{
    browser_request, configuration, configuration_insecure,
    cookies::{self, Jar, Origin},
    exchange_plain, exchange_tls, get, get_plain, get_plain_range, get_range,
    native::{Rtc, Tcp},
    options::{Options, HELP},
    public_roots, Error, Url,
};
use alloc::{format, sync::Arc, vec, vec::Vec};
use core::{
    cell::RefCell,
    slice, str,
    sync::atomic::{AtomicBool, Ordering},
};
use critical_section::Mutex;
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

/// rbrowse's cookie jar: one per browser session (rbrowse clears it when it
/// starts and when it exits). Every browser request reads it, every
/// response hop of a redirect chain writes it, and document.cookie goes
/// through it as well. The `fetch` command and the JSON client never touch it.
static JAR: Mutex<RefCell<Jar>> = Mutex::new(RefCell::new(Jar::new()));

fn clock() -> Option<u64> {
    Rtc.current_time().map(|time| time.as_secs())
}

fn origin(url: &Url) -> Origin<'_> {
    Origin { host: &url.host, path: &url.path, https: url.is_https }
}

fn jar_header(url: &Url, now: Option<u64>, script: bool) -> alloc::string::String {
    critical_section::with(|cs| JAR.borrow_ref_mut(cs).header(origin(url), now, script))
}

const POST_BODY_LIMIT: usize = 64 * 1024;

/// GET (or POST `post` = (body, content type)) `url_text`, following up to
/// five redirects, with cookies. 301/302/303 after a POST continue as GET;
/// 307/308 repeat the POST. With `same_origin`, a redirect may not leave the
/// first URL's origin (scheme, host, port): the page reads that reply, and a
/// server must not be able to point the reader at another site or at the
/// local network. Returns the body and the URL the chain ended on.
fn http_body(
    url_text: &str,
    mut post: Option<(&[u8], &str)>,
    cap: usize,
    same_origin: bool,
) -> Result<(Vec<u8>, Url), Error> {
    let mut url = Url::parse(url_text)?;
    if let Some((body, content_type)) = post {
        if body.len() > POST_BODY_LIMIT {
            return Err(Error::Message("request body too large"));
        }
        if content_type.is_empty()
            || content_type.len() > 256
            || content_type.bytes().any(|b| b < 32 || b == 127)
        {
            return Err(Error::Message("invalid Content-Type"));
        }
    }
    let origin_key = (url.is_https, url.host.clone(), url.port);
    let started = unsafe { crate::get_ticks() };
    let timeout = 20000u32;
    let mut config = None;
    let mut redirects = 0u32;
    let now = clock();
    let response = loop {
        let remaining = timeout.saturating_sub(unsafe { crate::get_ticks() }.wrapping_sub(started));
        if remaining == 0 {
            return Err(Error::Message("fetch timed out"));
        }
        let request = browser_request(&url, &jar_header(&url, now, false), post);
        let mut tcp = Tcp::connect(&url.host, url.port, remaining)?;
        let response = if url.is_https {
            if config.is_none() {
                config = Some(if Rtc.current_time().is_none() {
                    configuration_insecure(Arc::new(Rtc))?
                } else {
                    configuration(Arc::new(Rtc), public_roots())?
                });
            }
            exchange_tls(&mut tcp, &url, config.as_ref().unwrap().clone(), &request, false, cap)?
        } else {
            exchange_plain(&mut tcp, &request, false, cap)?
        };
        drop(tcp);
        // Cookies of every hop count, a redirect's included: the next hop
        // (often the same site) expects them.
        critical_section::with(|cs| {
            JAR.borrow_ref_mut(cs).take_headers(origin(&url), &response.headers, now)
        });
        if [301, 302, 303, 307, 308].contains(&response.status) {
            if redirects == 5 {
                return Err(Error::Message("too many redirects"));
            }
            let location = response
                .location
                .as_deref()
                .ok_or(Error::Message("redirect has no Location header"))?;
            url = url.redirect(location)?;
            if same_origin
                && (url.is_https, url.host.as_str(), url.port)
                    != (origin_key.0, origin_key.1.as_str(), origin_key.2)
            {
                return Err(Error::Message("redirect to another origin refused"));
            }
            if matches!(response.status, 301 | 302 | 303) {
                post = None;
            }
            redirects += 1;
            continue;
        }
        break response;
    };
    Ok((response.body, url))
}

fn write_out(out: *mut u8, cap: i32, text: &str) {
    let n = text.len().min((cap as usize) - 1);
    unsafe {
        if n > 0 {
            core::ptr::copy_nonoverlapping(text.as_ptr(), out, n);
        }
        *out.add(n) = 0;
    }
}

const BROWSER_BODY_LIMIT: usize = 1024 * 1024;
const NIC_UNAVAILABLE: &str = "HTTP needs the network stack. Check the URL, NIC, and DNS.";

/// GET a page for rbrowse. Returns the body length, or a negative error.
/// The body is truncated to `cap` so a long page still opens. On failure the
/// reason is written into `out`. Certificate dates are checked when the RTC
/// is valid; otherwise HTTPS proceeds without date checks.
#[no_mangle]
pub unsafe extern "C" fn rbrowse_http_get(url: *const u8, out: *mut u8, cap: i32) -> i32 {
    rbrowse_http(url, core::ptr::null(), -1, core::ptr::null(), 0, out, cap, core::ptr::null_mut(), 0)
}

/// rbrowse_http_get for a script's fetch(): redirects must stay on the
/// URL's own origin (see http_body).
#[no_mangle]
pub unsafe extern "C" fn rbrowse_http_get_same_origin(url: *const u8, out: *mut u8, cap: i32) -> i32 {
    rbrowse_http(url, core::ptr::null(), -1, core::ptr::null(), 1, out, cap, core::ptr::null_mut(), 0)
}

/// The browser's request: GET when `body_len` < 0, else POST of
/// body[0..body_len) with Content-Type `content_type` (NULL:
/// application/x-www-form-urlencoded). Cookies from the session jar go out
/// and come back on every hop. `same_origin` as in http_body. Returns the
/// response body length in `out` (truncated to cap - 1, NUL-terminated), or
/// a negative error with the reason in `out`. When `final_url` is non-null
/// it receives the URL the redirect chain ended on (NUL-terminated; empty if
/// it does not fit).
#[no_mangle]
pub unsafe extern "C" fn rbrowse_http(
    url: *const u8,
    body: *const u8,
    body_len: i32,
    content_type: *const u8,
    same_origin: i32,
    out: *mut u8,
    cap: i32,
    final_url: *mut u8,
    final_cap: i32,
) -> i32 {
    if url.is_null() || out.is_null() || cap < 2 || (body_len > 0 && body.is_null()) {
        return -1;
    }
    if !final_url.is_null() && final_cap > 0 {
        *final_url = 0;
    }
    if !crate::nic_is_ready() {
        write_out(out, cap, NIC_UNAVAILABLE);
        return -1;
    }
    if BUSY
        .compare_exchange(false, true, Ordering::Acquire, Ordering::Relaxed)
        .is_err()
    {
        write_out(out, cap, "another fetch is running");
        return -2;
    }
    let flags: u32;
    core::arch::asm!("pushfd", "pop {}", out(reg) flags);
    let _guard = CommandGuard(flags);
    core::arch::asm!("sti");
    crate::FETCH_NETWORK_SILENT.store(true, Ordering::Relaxed);
    let result = (|| {
        let text = unsafe { argument(url) }?;
        let post = if body_len < 0 {
            None
        } else {
            let data: &[u8] = if body_len == 0 {
                &[]
            } else {
                unsafe { slice::from_raw_parts(body, body_len as usize) }
            };
            let kind = if content_type.is_null() {
                "application/x-www-form-urlencoded"
            } else {
                unsafe { argument(content_type) }?
            };
            Some((data, kind))
        };
        http_body(text, post, BROWSER_BODY_LIMIT, same_origin != 0)
    })();
    match result {
        Ok((body, end)) => {
            let n = body.len().min((cap as usize) - 1);
            if n > 0 {
                core::ptr::copy_nonoverlapping(body.as_ptr(), out, n);
            }
            *out.add(n) = 0;
            if !final_url.is_null() && final_cap > 0 {
                let href = end.href();
                if href.len() < final_cap as usize {
                    core::ptr::copy_nonoverlapping(href.as_ptr(), final_url, href.len());
                    *final_url.add(href.len()) = 0;
                }
            }
            n as i32
        }
        Err(error) => {
            write_out(out, cap, &alloc::format!("{error}"));
            -2
        }
    }
}

/// A NUL-terminated string of at most `limit` bytes, or None.
unsafe fn c_text<'a>(pointer: *const u8, limit: usize) -> Option<&'a str> {
    if pointer.is_null() {
        return None;
    }
    for len in 0..=limit {
        if *pointer.add(len) == 0 {
            return str::from_utf8(slice::from_raw_parts(pointer, len)).ok();
        }
    }
    None
}

/// document.cookie for the page at `url`: the cookies a request to it would
/// send, minus HttpOnly ones, into out (NUL-terminated, truncated to cap - 1).
/// Returns the length, or -1 when the URL has no cookies (not http/https).
#[no_mangle]
pub unsafe extern "C" fn rbrowse_cookie_get(url: *const u8, out: *mut u8, cap: i32) -> i32 {
    if out.is_null() || cap < 1 {
        return -1;
    }
    *out = 0;
    let Some(url) = c_text(url, 4096).and_then(|text| Url::parse(text).ok()) else {
        return -1;
    };
    let text = jar_header(&url, clock(), true);
    if cap < 2 {
        return 0;
    }
    write_out(out, cap, &text);
    text.len().min(cap as usize - 1) as i32
}

/// document.cookie = text for the page at `url`. Scripts can neither see nor
/// set HttpOnly cookies. Returns 1 when the jar changed, else 0.
#[no_mangle]
pub unsafe extern "C" fn rbrowse_cookie_set(url: *const u8, text: *const u8) -> i32 {
    let Some(url) = c_text(url, 4096).and_then(|text| Url::parse(text).ok()) else {
        return 0;
    };
    let Some(text) = c_text(text, cookies::MAX_LINE) else {
        return 0;
    };
    let now = clock();
    critical_section::with(|cs| JAR.borrow_ref_mut(cs).set(origin(&url), text, now, true)) as i32
}

/// Forgets every cookie (rbrowse calls it when a session starts and ends).
#[no_mangle]
pub extern "C" fn rbrowse_cookie_clear() {
    critical_section::with(|cs| JAR.borrow_ref_mut(cs).clear());
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

/// The most a page fetched for the desktop browser may be.
const PAGE_LIMIT: usize = 24 * 1024 * 1024;

/// Copies `text` into a NUL-terminated C buffer of `cap` bytes, truncating if needed.
unsafe fn put_c(out: *mut u8, cap: u32, text: &[u8]) {
    if out.is_null() || cap == 0 {
        return;
    }
    let n = core::cmp::min(text.len(), cap as usize - 1);
    core::ptr::copy_nonoverlapping(text.as_ptr(), out, n);
    *out.add(n) = 0;
}

/// The media type of a response ("text/html" from "Content-Type: text/html; charset=utf-8"), lower case.
fn content_type(headers: &[u8]) -> Vec<u8> {
    for line in headers.split(|&b| b == b'\n') {
        let line = line.strip_suffix(b"\r").unwrap_or(line);
        let Some(colon) = line.iter().position(|&b| b == b':') else { continue };
        if !line[..colon].eq_ignore_ascii_case(b"content-type") {
            continue;
        }
        let value = &line[colon + 1..];
        let value = value.split(|&b| b == b';').next().unwrap_or(value);
        return value.trim_ascii().to_ascii_lowercase();
    }
    Vec::new()
}

/// One GET for the desktop's web browser: redirects followed (at most five) and up to 24 MB read. Copies at most
/// `out_cap` bytes of the body into `out` and returns the body's full length; *status gets the HTTP status,
/// `final_url` the address the body was read from and `content_type` its media type (both NUL-terminated, cut to
/// fit). -1 on a transport or protocol error (rust_http_error says why).
#[no_mangle]
pub unsafe extern "C" fn rust_http_get_page(
    url: *const u8,
    out: *mut u8,
    out_cap: u32,
    status: *mut i32,
    final_url: *mut u8,
    final_cap: u32,
    content_type_out: *mut u8,
    content_type_cap: u32,
) -> i32 {
    if !status.is_null() {
        *status = 0;
    }
    put_c(final_url, final_cap, b"");
    put_c(content_type_out, content_type_cap, b"");
    if url.is_null() || (out.is_null() && out_cap > 0) {
        return -1;
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

    let result = (|| -> Result<(super::Response, Url), Error> {
        let text = argument(url)?;
        let mut options = Options::parse(&["--location", text])?;
        options.max_size = PAGE_LIMIT;
        let mut url = options.url.take().ok_or(Error::Message("missing URL"))?;
        let mut config = None;
        let response = request_following_redirects(&mut url, false, None, PAGE_LIMIT, &options, &mut config)?;
        Ok((response, url))
    })();

    match result {
        Ok((response, url)) => {
            let n = core::cmp::min(response.body.len(), out_cap as usize);
            if n > 0 {
                core::ptr::copy_nonoverlapping(response.body.as_ptr(), out, n);
            }
            if !status.is_null() {
                *status = response.status as i32;
            }
            let address = format!("{}://{}{}", url.scheme(), url.authority(), url.path);
            put_c(final_url, final_cap, address.as_bytes());
            put_c(content_type_out, content_type_cap, &content_type(&response.headers));
            core::cmp::min(response.body.len(), i32::MAX as usize) as i32
        }
        Err(error) => {
            remember_error(&format!("{error}"));
            -1
        }
    }
}
