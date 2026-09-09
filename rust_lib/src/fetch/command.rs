use super::{
    configuration, get,
    native::{Rtc, Tcp},
    options::{Options, HELP},
    public_roots, Error,
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

static BUSY: AtomicBool = AtomicBool::new(false);

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

fn execute(args: &[&str]) -> Result<(), Error> {
    let options = Options::parse(args)?;
    if options.help {
        display(HELP.as_bytes());
        return Ok(());
    }
    let mut url = options.url.ok_or(Error::Message("missing URL"))?;
    let output = options.output.as_deref().map(filename).transpose()?;
    if let Some(name) = &output {
        if !options.overwrite && unsafe { crate::avfs_file_exists(name.as_ptr()) } {
            return Err(Error::Message(
                "output file exists; use --overwrite to replace it",
            ));
        }
    }
    if Rtc.current_time().is_none() {
        return Err(Error::Message(
            "RTC unavailable or invalid; cannot check certificate dates",
        ));
    }
    let config = configuration(Arc::new(Rtc), roots(options.cacert.as_deref())?)?;
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
        let response = get(
            &mut tcp,
            &url,
            config.clone(),
            options.head,
            options.max_size,
        )?;
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

#[no_mangle]
pub extern "C" fn rust_fetch_active() -> bool {
    BUSY.load(Ordering::Acquire)
}

#[no_mangle]
pub unsafe extern "C" fn rust_https_get(url: *const u8) -> i32 {
    rust_fetch(2, [b"fetch\0".as_ptr(), url].as_ptr())
}

#[no_mangle]
pub unsafe extern "C" fn rust_test_https() -> i32 {
    rust_https_get(b"https://example.com/\0".as_ptr())
}
