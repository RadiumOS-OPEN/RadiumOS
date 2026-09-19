use super::{Error, Url};
use alloc::string::{String, ToString};

pub const HELP: &str = concat!(
    "Usage: fetch [options] http://host/path\n",
    "       fetch [options] https://host/path\n\n",
    "Options:\n",
    "  -h, --help             Show this help\n",
    "  -o, --output FILE      Save response to\n",
    "                         FILE\n",
    "      --overwrite        Replace an existing\n",
    "                         output file\n",
    "  -I, --head             Request headers\n",
    "                         only\n",
    "  -i, --include          Include headers in\n",
    "                         output\n",
    "  -L, --location         Follow HTTP(S)\n",
    "                         redirects\n",
    "  -f, --fail             Fail on HTTP status\n",
    "                         400 or higher\n",
    "  -s, --silent           Hide progress and\n",
    "                         metadata\n",
    "      --timeout SECONDS  Timeout: 1..300\n",
    "                         (default 30)\n",
    "      --max-size BYTES   Max body: 1..\n",
    "                         16777216 (default\n",
    "                         1048576)\n",
    "      --cacert FILE      Use CA certificates\n",
    "                         from FILE\n",
);

#[derive(Debug)]
pub struct Options {
    pub help: bool,
    pub url: Option<Url>,
    pub output: Option<String>,
    pub head: bool,
    pub include: bool,
    pub location: bool,
    pub fail: bool,
    pub silent: bool,
    pub overwrite: bool,
    pub timeout: u32,
    pub max_size: usize,
    pub cacert: Option<String>,
}

impl Options {
    pub fn parse(args: &[&str]) -> Result<Self, Error> {
        let mut options = Self {
            help: false,
            url: None,
            output: None,
            head: false,
            include: false,
            location: false,
            fail: false,
            silent: false,
            overwrite: false,
            timeout: 30,
            max_size: 1024 * 1024,
            cacert: None,
        };
        let mut timeout = None;
        let mut max_size = None;
        let mut flags = true;
        let mut args = args.iter().copied();
        while let Some(arg) = args.next() {
            if !flags || !arg.starts_with('-') {
                if options.url.is_some() {
                    return Err(Error::Message("fetch accepts exactly one URL"));
                }
                options.url = Some(Url::parse(arg)?);
                continue;
            }
            match arg {
                "--" => flags = false,
                "-h" | "--help" => options.help = true,
                "-I" | "--head" => options.head = true,
                "-i" | "--include" => options.include = true,
                "-L" | "--location" => options.location = true,
                "-f" | "--fail" => options.fail = true,
                "-s" | "--silent" => options.silent = true,
                "--overwrite" => options.overwrite = true,
                "-o" | "--output" | "--cacert" | "--timeout" | "--max-size" => {
                    let value = args
                        .next()
                        .filter(|value| {
                            !value.is_empty()
                                && !value.starts_with('-')
                                && !value.bytes().any(|b| b < 32 || b == 127)
                        })
                        .ok_or(Error::Message("missing or invalid option value"))?;
                    match arg {
                        "-o" | "--output" => set(&mut options.output, value.to_string())?,
                        "--cacert" => set(&mut options.cacert, value.to_string())?,
                        "--timeout" => set(&mut timeout, number(value, 300)? as u32)?,
                        "--max-size" => set(&mut max_size, number(value, 16 * 1024 * 1024)?)?,
                        _ => unreachable!(),
                    }
                }
                _ => return Err(Error::Message("unknown fetch option; use fetch --help")),
            }
        }
        options.timeout = timeout.unwrap_or(options.timeout);
        options.max_size = max_size.unwrap_or(options.max_size);
        if options.url.is_none() && !options.help {
            return Err(Error::Message("fetch requires an HTTP or HTTPS URL"));
        }
        if options.overwrite && options.output.is_none() {
            return Err(Error::Message("--overwrite requires --output"));
        }
        Ok(options)
    }
}

fn set<T: PartialEq>(slot: &mut Option<T>, value: T) -> Result<(), Error> {
    if slot.as_ref().is_some_and(|previous| *previous != value) {
        return Err(Error::Message("conflicting option values"));
    }
    *slot = Some(value);
    Ok(())
}

fn number(value: &str, maximum: usize) -> Result<usize, Error> {
    if !value.bytes().all(|b| b.is_ascii_digit()) {
        return Err(Error::Message(
            "option value must be a positive decimal number",
        ));
    }
    value
        .parse::<usize>()
        .ok()
        .filter(|&value| value > 0 && value <= maximum)
        .ok_or(Error::Message("option value is out of range"))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_options_and_defaults() {
        let options = Options::parse(&["https://example.com"]).unwrap();
        assert_eq!(options.timeout, 30);
        assert_eq!(options.max_size, 1024 * 1024);
        assert!(!options.location && !options.overwrite && !options.silent);
        assert!(Options::parse(&["http://example.com"]).is_ok());
        let options = Options::parse(&[
            "-o",
            "page.html",
            "--overwrite",
            "-I",
            "-i",
            "-L",
            "-f",
            "-s",
            "--timeout",
            "300",
            "--max-size",
            "16777216",
            "--cacert",
            "roots.pem",
            "--",
            "https://example.com/path",
        ])
        .unwrap();
        assert_eq!(options.output.as_deref(), Some("page.html"));
        assert_eq!(options.cacert.as_deref(), Some("roots.pem"));
        assert_eq!(options.timeout, 300);
        assert_eq!(options.max_size, 16 * 1024 * 1024);
        assert!(
            options.overwrite
                && options.head
                && options.include
                && options.location
                && options.fail
                && options.silent
        );
        assert_eq!(options.url.unwrap().path, "/path");
        assert!(Options::parse(&["--help"]).unwrap().help);
        assert!(Options::parse(&["-h"]).unwrap().url.is_none());
        assert!(
            Options::parse(&["--timeout", "1", "--max-size", "1", "https://example.com"]).is_ok()
        );
    }

    #[test]
    fn rejects_invalid_and_conflicting_options() {
        for args in [
            &[][..],
            &["ftp://example.com"],
            &["https://a.com", "https://b.com"],
            &["--insecure", "https://a.com"],
            &["--", "--help"],
            &["-o"],
            &["--output", "--head", "https://a.com"],
            &["--output", "", "https://a.com"],
            &["--cacert", "a\0b", "https://a.com"],
            &["--timeout", "0", "https://a.com"],
            &["--timeout", "301", "https://a.com"],
            &["--timeout", "+1", "https://a.com"],
            &["--timeout", "1.5", "https://a.com"],
            &["--max-size", "0", "https://a.com"],
            &["--max-size", "16777217", "https://a.com"],
            &["--max-size", "999999999999999999999999", "https://a.com"],
            &["--timeout", "1", "--timeout", "2", "https://a.com"],
            &["--max-size", "1", "--max-size", "2", "https://a.com"],
            &["-o", "a", "--output", "b", "https://a.com"],
            &["--cacert", "a", "--cacert", "b", "https://a.com"],
            &["--overwrite", "https://a.com"],
        ] {
            assert!(Options::parse(args).is_err(), "accepted {args:?}");
        }
        assert!(Options::parse(&["-o", "a", "--output", "a", "https://a.com"]).is_ok());
        assert!(Options::parse(&["--timeout", "1", "--timeout", "01", "https://a.com"]).is_ok());
    }
}
