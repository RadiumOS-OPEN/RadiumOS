//! The browser's cookie jar (RFC 6265 basics) for one rbrowse session.
//!
//! Pure data structure: no I/O, no globals, the caller passes the clock. The
//! jar is fed by Set-Cookie headers (every hop of a redirect chain) and by
//! document.cookie assignments, and produces the Cookie header for a request
//! or the script-visible document.cookie string.
//!
//! Hostile servers can send anything, so every dimension is capped: name and
//! value together, domain, path, cookies per domain and in total, and the
//! length of the Cookie header we send back.

use alloc::{string::String, vec::Vec};

/// Name plus value (bytes), like browsers' 4096-byte limit.
pub const MAX_NAME_VALUE: usize = 4096;
/// Longest Path attribute kept; a longer one falls back to the default path.
pub const MAX_PATH: usize = 1024;
/// Cookies one domain (host or Domain attribute) may hold.
pub const MAX_PER_DOMAIN: usize = 50;
/// Cookies in the whole jar.
pub const MAX_TOTAL: usize = 300;
/// The Cookie header (or document.cookie) we produce stops growing here.
pub const MAX_HEADER: usize = 8192;
/// A Set-Cookie line longer than this is ignored outright.
pub const MAX_LINE: usize = 8192;

/// The Public Suffix List (see public_suffix_list.NOTICE): one rule per
/// line, ASCII, sorted by byte value, so it is searched in place.
const PSL: &str = include_str!("public_suffix_list.txt");

/// Is `rule` a line of the list? A binary search over the text: probe a
/// byte, back up to its line, compare.
fn psl_has(rule: &str) -> bool {
    let text = PSL.as_bytes();
    let (mut lo, mut hi) = (0usize, text.len());
    while lo < hi {
        let mid = lo + (hi - lo) / 2;
        let mut start = mid;
        while start > lo && text[start - 1] != b'\n' {
            start -= 1;
        }
        let end = text[start..].iter().position(|&b| b == b'\n').map_or(text.len(), |i| start + i);
        match text[start..end].cmp(rule.as_bytes()) {
            core::cmp::Ordering::Equal => return true,
            core::cmp::Ordering::Less => lo = end + 1,
            core::cmp::Ordering::Greater => hi = start,
        }
    }
    false
}

/// How many trailing labels of `host` form its public suffix, by the list's
/// algorithm: an exception rule wins, else the matching rule with the most
/// labels, else the last label alone ("*").
fn suffix_labels(host: &str) -> usize {
    let labels: Vec<&str> = host.split('.').collect();
    let n = labels.len();
    let mut best = 1;
    for i in 0..n {
        let tail = labels[i..].join(".");
        let count = n - i;
        if psl_has(&alloc::format!("!{tail}")) {
            return count - 1;
        }
        if count > best && psl_has(&tail) {
            best = count;
        }
        if i + 1 < n && count > best && psl_has(&alloc::format!("*.{}", labels[i + 1..].join("."))) {
            best = count;
        }
    }
    best
}

#[derive(Debug, Clone)]
pub struct Cookie {
    pub name: String,
    pub value: String,
    /// Lowercase, no leading dot. The request host for host-only cookies.
    pub domain: String,
    pub host_only: bool,
    pub path: String,
    pub secure: bool,
    pub http_only: bool,
    pub same_site: SameSite,
    /// Unix seconds; None = session cookie (lives until the jar is cleared).
    pub expires: Option<u64>,
    /// Insertion order, kept across replacements (RFC 6265 creation-time).
    created: u64,
}

/// SameSite (RFC 6265bis). A cookie that does not say is Lax, as in
/// current browsers: a page on another site cannot send it along with a
/// form POST, an image or a script request, only with a link it is opening.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SameSite {
    Strict,
    Lax,
    None,
}

/// How a request relates to the page that caused it.
#[derive(Debug, Clone, Copy)]
pub struct Context {
    /// The request goes to another site than the page's (see `site`).
    pub cross_site: bool,
    /// A top-level navigation with a safe method (a link, a GET form): Lax
    /// cookies go along even cross-site.
    pub lax_ok: bool,
}

impl Context {
    /// The user's own navigation (address bar, Back) or a page's same-site
    /// request: every cookie may go.
    pub const SAME_SITE: Context = Context { cross_site: false, lax_ok: true };
}

/// The registrable domain ("site") of a host: its public suffix (Public
/// Suffix List) plus one label. An IP address, a single label, or a host
/// that is itself a public suffix is its own site.
pub fn site(host: &str) -> &str {
    if is_ip(host) || !host.contains('.') {
        return host;
    }
    let keep = suffix_labels(host) + 1;
    let dots: Vec<usize> = host.match_indices('.').map(|(i, _)| i).collect();
    if keep > dots.len() {
        return host;
    }
    &host[dots[dots.len() - keep] + 1..]
}

/// Where a cookie operation happens: the request (or page) URL's parts.
#[derive(Debug, Clone, Copy)]
pub struct Origin<'a> {
    pub host: &'a str,
    /// The request path, query allowed (it is ignored).
    pub path: &'a str,
    pub https: bool,
}

#[derive(Debug, Default)]
pub struct Jar {
    list: Vec<Cookie>,
    serial: u64,
}

fn is_ip(host: &str) -> bool {
    !host.is_empty() && host.split('.').all(|label| !label.is_empty() && label.bytes().all(|b| b.is_ascii_digit()))
}

/// RFC 6265 5.1.3: host equals domain, or host ends with "." + domain and is
/// a name (not an IP address).
pub fn domain_match(host: &str, domain: &str) -> bool {
    if host == domain {
        return true;
    }
    host.len() > domain.len()
        && host.ends_with(domain)
        && host.as_bytes()[host.len() - domain.len() - 1] == b'.'
        && !is_ip(host)
}

fn strip_query(path: &str) -> &str {
    let end = path.find(['?', '#']).unwrap_or(path.len());
    &path[..end]
}

/// RFC 6265 5.1.4 default-path of a request path.
pub fn default_path(request_path: &str) -> &str {
    let path = strip_query(request_path);
    if !path.starts_with('/') {
        return "/";
    }
    match path.rfind('/') {
        Some(0) | None => "/",
        Some(last) => &path[..last],
    }
}

/// RFC 6265 5.1.4 path-match.
pub fn path_match(request_path: &str, cookie_path: &str) -> bool {
    let path = strip_query(request_path);
    let path = if path.is_empty() { "/" } else { path };
    if path == cookie_path {
        return true;
    }
    path.starts_with(cookie_path)
        && (cookie_path.ends_with('/') || path.as_bytes().get(cookie_path.len()) == Some(&b'/'))
}

fn is_public_suffix(domain: &str) -> bool {
    !domain.contains('.') || suffix_labels(domain) >= domain.split('.').count()
}

fn bad_chars(text: &str) -> bool {
    text.bytes().any(|b| (b < 0x20 && b != b'\t') || b == 0x7f)
}

/// Days since 1970-01-01 of a proleptic Gregorian date.
fn days_from_civil(year: i64, month: i64, day: i64) -> i64 {
    let y = if month <= 2 { year - 1 } else { year };
    let era = if y >= 0 { y } else { y - 399 } / 400;
    let yoe = y - era * 400;
    let mp = (month + 9) % 12;
    let doy = (153 * mp + 2) / 5 + day - 1;
    let doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    era * 146097 + doe - 719468
}

/// RFC 6265 5.1.1 cookie-date ("Wed, 21 Oct 2015 07:28:00 GMT",
/// "Mon, 01-Jan-1990 00:00:00 GMT", "Sunday, 06-Nov-94 08:49:37 GMT", ...)
/// to Unix seconds; None when it is not a date. Dates before 1970 clamp to 0.
pub fn parse_date(text: &str) -> Option<u64> {
    let mut time = None;
    let mut day = None;
    let mut month = None;
    let mut year = None;
    let delimiter = |b: u8| {
        b == 0x09 || (0x20..=0x2f).contains(&b) || (0x3b..=0x40).contains(&b)
            || (0x5b..=0x60).contains(&b) || (0x7b..=0x7e).contains(&b)
    };
    for token in text.as_bytes().split(|&b| delimiter(b)).filter(|t| !t.is_empty()) {
        let token = core::str::from_utf8(token).ok()?;
        let digits = token.bytes().take_while(u8::is_ascii_digit).count();
        if time.is_none() {
            let parts: Vec<&str> = token.splitn(3, ':').collect();
            if parts.len() == 3 {
                let field = |s: &str, trailing: bool| -> Option<u32> {
                    let n = s.bytes().take_while(u8::is_ascii_digit).count();
                    if n == 0 || n > 2 || (!trailing && n != s.len()) {
                        return None;
                    }
                    s[..n].parse().ok()
                };
                if let (Some(h), Some(m), Some(s)) =
                    (field(parts[0], false), field(parts[1], false), field(parts[2], true))
                {
                    time = Some((h, m, s));
                    continue;
                }
            }
        }
        if day.is_none() && (1..=2).contains(&digits) {
            day = token[..digits].parse::<u32>().ok();
            continue;
        }
        if month.is_none() && token.len() >= 3 {
            let lower = token[..3].to_ascii_lowercase();
            const MONTHS: [&str; 12] =
                ["jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"];
            if let Some(index) = MONTHS.iter().position(|m| *m == lower) {
                month = Some(index as u32 + 1);
                continue;
            }
        }
        if year.is_none() && (2..=4).contains(&digits) {
            year = token[..digits].parse::<u32>().ok();
            continue;
        }
    }
    let (hour, minute, second) = time?;
    let (day, month, mut year) = (day?, month?, year?);
    if (70..=99).contains(&year) {
        year += 1900;
    } else if year <= 69 {
        year += 2000;
    }
    if !(1..=31).contains(&day) || year < 1601 || hour > 23 || minute > 59 || second > 59 {
        return None;
    }
    let days = days_from_civil(year as i64, month as i64, day as i64);
    let seconds = days * 86400 + (hour * 3600 + minute * 60 + second) as i64;
    Some(seconds.max(0) as u64)
}

/// Before this (2000-01-01) an Expires date is "in the past" even when the
/// clock is unknown: deletion idioms use 1970 or 1990.
const SURELY_PAST: u64 = 946_684_800;

impl Jar {
    pub const fn new() -> Self {
        Self { list: Vec::new(), serial: 0 }
    }

    pub fn clear(&mut self) {
        self.list.clear();
    }

    pub fn len(&self) -> usize {
        self.list.len()
    }

    fn purge(&mut self, now: Option<u64>) {
        if let Some(now) = now {
            self.list.retain(|c| c.expires.map_or(true, |t| t > now));
        }
    }

    /// Stores one Set-Cookie value (or, with `script`, one document.cookie
    /// assignment) received at `at`. Returns true when the jar changed.
    pub fn set(&mut self, at: Origin<'_>, line: &str, now: Option<u64>, script: bool) -> bool {
        if line.len() > MAX_LINE {
            return false;
        }
        let mut parts = line.split(';');
        let pair = parts.next().unwrap_or("");
        let Some((name, value)) = pair.split_once('=') else { return false };
        let name = name.trim_matches([' ', '\t']);
        let value = value.trim_matches([' ', '\t']);
        if name.is_empty() || name.len() + value.len() > MAX_NAME_VALUE || bad_chars(name) || bad_chars(value) {
            return false;
        }
        let mut domain_attr: Option<String> = None;
        let mut path_attr: Option<&str> = None;
        let mut secure = false;
        let mut http_only = false;
        let mut same_site = SameSite::Lax;
        let mut max_age: Option<i64> = None;
        let mut expires_attr: Option<u64> = None;
        for attribute in parts {
            let (key, val) = match attribute.split_once('=') {
                Some((k, v)) => (k.trim_matches([' ', '\t']), v.trim_matches([' ', '\t'])),
                None => (attribute.trim_matches([' ', '\t']), ""),
            };
            if key.eq_ignore_ascii_case("expires") {
                if let Some(t) = parse_date(val) {
                    expires_attr = Some(t);
                }
            } else if key.eq_ignore_ascii_case("max-age") {
                let digits = val.strip_prefix('-').unwrap_or(val);
                if !digits.is_empty() && digits.bytes().all(|b| b.is_ascii_digit()) {
                    max_age = Some(if val.starts_with('-') {
                        -1
                    } else {
                        // Saturate absurd lifetimes instead of failing.
                        digits.parse::<i64>().unwrap_or(i64::MAX)
                    });
                }
            } else if key.eq_ignore_ascii_case("domain") {
                if !val.is_empty() && val.len() <= 253 {
                    let d = val.strip_prefix('.').unwrap_or(val).to_ascii_lowercase();
                    domain_attr = Some(d);
                }
            } else if key.eq_ignore_ascii_case("path") {
                path_attr = Some(val);
            } else if key.eq_ignore_ascii_case("secure") {
                secure = true;
            } else if key.eq_ignore_ascii_case("httponly") {
                http_only = true;
            } else if key.eq_ignore_ascii_case("samesite") {
                same_site = if val.eq_ignore_ascii_case("strict") {
                    SameSite::Strict
                } else if val.eq_ignore_ascii_case("none") {
                    SameSite::None
                } else {
                    SameSite::Lax
                };
            }
        }
        if script && http_only {
            return false;
        }
        // SameSite=None must be Secure (RFC 6265bis): browsers refuse it otherwise.
        if same_site == SameSite::None && !secure {
            return false;
        }
        // Only a secure origin may set a Secure cookie (RFC 6265bis).
        if secure && !at.https {
            return false;
        }
        let host = at.host;
        let (domain, host_only) = match domain_attr {
            Some(d) if d.is_empty() => (String::from(host), true),
            Some(d) => {
                if !d.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'-' || b == b'.') {
                    return false;
                }
                if is_public_suffix(&d) {
                    if d != host {
                        return false;
                    }
                    (d, true)
                } else if !domain_match(host, &d) || (is_ip(host) && d != host) {
                    return false;
                } else {
                    (d, false)
                }
            }
            None => (String::from(host), true),
        };
        let path = match path_attr {
            Some(p) if p.starts_with('/') && p.len() <= MAX_PATH && !bad_chars(p) => String::from(p),
            _ => String::from(default_path(at.path)),
        };
        // Cookie name prefixes.
        if name.starts_with("__Secure-") && !secure {
            return false;
        }
        if name.starts_with("__Host-") && (!secure || !host_only || path != "/") {
            return false;
        }
        // A non-secure origin may not shadow a secure cookie of the same name.
        if !at.https
            && self.list.iter().any(|c| {
                c.secure
                    && c.name == name
                    && (domain_match(&c.domain, &domain) || domain_match(&domain, &c.domain))
                    && path_match(&path, &c.path)
            })
        {
            return false;
        }
        let expired;
        let expires = match max_age {
            Some(age) if age <= 0 => {
                expired = true;
                None
            }
            Some(age) => {
                expired = false;
                now.map(|n| n.saturating_add(age as u64))
            }
            None => match expires_attr {
                Some(t) => match now {
                    Some(n) => {
                        expired = t <= n;
                        Some(t)
                    }
                    None => {
                        expired = t < SURELY_PAST;
                        None
                    }
                },
                None => {
                    expired = false;
                    None
                }
            },
        };
        let existing = self
            .list
            .iter()
            .position(|c| c.name == name && c.domain == domain && c.path == path && c.host_only == host_only);
        let mut created = None;
        if let Some(index) = existing {
            if script && self.list[index].http_only {
                return false;
            }
            created = Some(self.list[index].created);
            self.list.remove(index);
        }
        if expired {
            return existing.is_some();
        }
        let created = created.unwrap_or_else(|| {
            self.serial += 1;
            self.serial
        });
        self.purge(now);
        let cookie = Cookie {
            name: String::from(name),
            value: String::from(value),
            domain,
            host_only,
            path,
            secure,
            http_only,
            same_site,
            expires,
            created,
        };
        // Make room: the oldest cookie of the same domain, then the oldest overall.
        while self.list.iter().filter(|c| c.domain == cookie.domain).count() >= MAX_PER_DOMAIN {
            let oldest = self
                .list
                .iter()
                .enumerate()
                .filter(|(_, c)| c.domain == cookie.domain)
                .min_by_key(|(_, c)| c.created)
                .map(|(i, _)| i)
                .unwrap();
            self.list.remove(oldest);
        }
        while self.list.len() >= MAX_TOTAL {
            let oldest = self
                .list
                .iter()
                .enumerate()
                .min_by_key(|(_, c)| c.created)
                .map(|(i, _)| i)
                .unwrap();
            self.list.remove(oldest);
        }
        self.list.push(cookie);
        true
    }

    /// Feeds every Set-Cookie line of a raw HTTP response head. Lines end
    /// in CRLF exactly as the response parser splits them: a bare LF (which
    /// that parser lets through only inside the status line) must not start
    /// a header of its own.
    pub fn take_headers(&mut self, at: Origin<'_>, head: &[u8], now: Option<u64>) {
        let mut rest = head;
        let mut first = true;
        while !rest.is_empty() {
            let end = rest.windows(2).position(|w| w == b"\r\n").unwrap_or(rest.len());
            let line = &rest[..end];
            rest = &rest[(end + 2).min(rest.len())..];
            if core::mem::take(&mut first) {
                continue; // the status line
            }
            // Non-UTF-8 bytes in another header must not drop the cookies.
            let Ok(line) = core::str::from_utf8(line) else { continue };
            if let Some((name, value)) = line.split_once(':') {
                if name.eq_ignore_ascii_case("set-cookie") {
                    self.set(at, value.trim_matches([' ', '\t']), now, false);
                }
            }
        }
    }

    /// The "a=1; b=2" string for a request to `at` (script: what
    /// document.cookie shows, which leaves HttpOnly cookies out).
    pub fn header(&mut self, at: Origin<'_>, now: Option<u64>, script: bool) -> String {
        self.header_in(at, now, script, Context::SAME_SITE)
    }

    /// `header` for a request made in `ctx`: a cross-site request carries
    /// only SameSite=None cookies, and Lax ones too when it is a top-level
    /// safe navigation.
    pub fn header_in(&mut self, at: Origin<'_>, now: Option<u64>, script: bool, ctx: Context) -> String {
        self.purge(now);
        let mut matching: Vec<&Cookie> = self
            .list
            .iter()
            .filter(|c| {
                (if c.host_only { at.host == c.domain } else { domain_match(at.host, &c.domain) })
                    && path_match(at.path, &c.path)
                    && (!c.secure || at.https)
                    && !(script && c.http_only)
                    && (!ctx.cross_site
                        || c.same_site == SameSite::None
                        || (c.same_site == SameSite::Lax && ctx.lax_ok))
            })
            .collect();
        matching.sort_by(|a, b| b.path.len().cmp(&a.path.len()).then(a.created.cmp(&b.created)));
        let mut out = String::new();
        for cookie in matching {
            let extra = cookie.name.len() + 1 + cookie.value.len() + if out.is_empty() { 0 } else { 2 };
            if out.len() + extra > MAX_HEADER {
                continue;
            }
            if !out.is_empty() {
                out.push_str("; ");
            }
            out.push_str(&cookie.name);
            out.push('=');
            out.push_str(&cookie.value);
        }
        out
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const HTTPS: Origin<'static> = Origin { host: "www.example.com", path: "/a/b?x=1", https: true };
    const HTTP: Origin<'static> = Origin { host: "www.example.com", path: "/a/b", https: false };
    const NOW: Option<u64> = Some(1_800_000_000);

    fn at(host: &'static str, path: &'static str, https: bool) -> Origin<'static> {
        Origin { host, path, https }
    }

    #[test]
    fn dates() {
        assert_eq!(parse_date("Wed, 21 Oct 2015 07:28:00 GMT"), Some(1_445_412_480));
        assert_eq!(parse_date("Sun, 16-May-2027 22:20:36 GMT"), Some(1_810_506_036));
        assert_eq!(parse_date("Thu, 01 Jan 1970 00:00:00 GMT"), Some(0));
        assert_eq!(parse_date("Mon, 01-Jan-1990 00:00:00 GMT"), Some(631_152_000));
        assert_eq!(parse_date("Sunday, 06-Nov-94 08:49:37 GMT"), Some(784_111_777));
        assert_eq!(parse_date("not a date"), None);
        assert_eq!(parse_date("Wed, 32 Oct 2015 07:28:00 GMT"), None);
        assert_eq!(parse_date("Wed, 21 Oct 2015 25:28:00 GMT"), None);
    }

    #[test]
    fn paths() {
        assert_eq!(default_path("/a/b?x=/y"), "/a");
        assert_eq!(default_path("/a"), "/");
        assert_eq!(default_path("/"), "/");
        assert_eq!(default_path(""), "/");
        assert!(path_match("/a/b", "/a"));
        assert!(path_match("/a/", "/a/"));
        assert!(path_match("/a", "/a"));
        assert!(!path_match("/ab", "/a"));
        assert!(path_match("/a?q", "/a"));
        assert!(path_match("/anything", "/"));
        assert!(domain_match("www.example.com", "example.com"));
        assert!(!domain_match("wwwexample.com", "example.com"));
        assert!(!domain_match("example.com", "www.example.com"));
        assert!(!domain_match("1.2.3.4", "2.3.4"));
    }

    #[test]
    fn host_only_and_domain_cookies() {
        let mut jar = Jar::new();
        assert!(jar.set(HTTPS, "a=1", NOW, false));
        assert!(jar.set(HTTPS, "b=2; Domain=.Example.COM; Path=/", NOW, false));
        // a (default path /a) is more specific than b (/), so it comes first.
        assert_eq!(jar.header(at("www.example.com", "/a/c", true), NOW, false), "a=1; b=2");
        // a is host-only with default path /a: not sent to a sibling host or /x.
        assert_eq!(jar.header(at("api.example.com", "/a/c", true), NOW, false), "b=2");
        assert_eq!(jar.header(at("www.example.com", "/x", true), NOW, false), "b=2");
        assert_eq!(jar.header(at("example.org", "/", true), NOW, false), "");
        // Refused: foreign domain, a public suffix, a single label.
        assert!(!jar.set(HTTPS, "c=3; Domain=other.com", NOW, false));
        assert!(!jar.set(HTTPS, "c=3; Domain=com", NOW, false));
        assert!(!jar.set(at("x.co.uk", "/", true), "c=3; Domain=co.uk", NOW, false));
        assert!(!jar.set(HTTPS, "c=3; Domain=w.example.com", NOW, false));
        assert!(!jar.set(HTTPS, "noequals", NOW, false));
        assert!(!jar.set(HTTPS, "=v", NOW, false));
        assert!(!jar.set(HTTPS, "c=\x01", NOW, false));
        // A Domain equal to a single-label host stays host-only.
        assert!(jar.set(at("localhost", "/", false), "l=1; Domain=localhost", NOW, false));
        assert_eq!(jar.header(at("localhost", "/", false), NOW, false), "l=1");
        // IP hosts never get domain cookies of a suffix.
        assert!(!jar.set(at("10.0.2.2", "/", false), "i=1; Domain=0.2.2", NOW, false));
        assert!(jar.set(at("10.0.2.2", "/", false), "i=1", NOW, false));
        assert_eq!(jar.header(at("10.0.2.2", "/p", false), NOW, false), "i=1");
    }

    #[test]
    fn replace_expire_delete() {
        let mut jar = Jar::new();
        assert!(jar.set(HTTPS, "a=1; Path=/", NOW, false));
        assert!(jar.set(HTTPS, "b=1; Path=/", NOW, false));
        assert!(jar.set(HTTPS, "a=2; Path=/", NOW, false));
        // Replacement keeps the original creation order.
        assert_eq!(jar.header(HTTPS, NOW, false), "a=2; b=1");
        assert!(jar.set(HTTPS, "a=x; Path=/; Max-Age=0", NOW, false));
        assert_eq!(jar.header(HTTPS, NOW, false), "b=1");
        assert!(jar.set(HTTPS, "b=x; Path=/; Expires=Thu, 01 Jan 1970 00:00:00 GMT", NOW, false));
        assert_eq!(jar.header(HTTPS, NOW, false), "");
        assert!(jar.set(HTTPS, "c=1; Path=/; Max-Age=10", NOW, false));
        assert_eq!(jar.header(HTTPS, NOW, false), "c=1");
        assert_eq!(jar.header(HTTPS, Some(NOW.unwrap() + 11), false), "");
        // Max-Age wins over Expires.
        assert!(jar.set(HTTPS, "d=1; Path=/; Expires=Thu, 01 Jan 1970 00:00:00 GMT; Max-Age=100", NOW, false));
        assert_eq!(jar.header(HTTPS, NOW, false), "d=1");
        // Without a clock: past dates still delete, future ones are session cookies.
        assert!(jar.set(HTTPS, "e=1; Path=/; Expires=Fri, 17-Dec-2027 22:20:47 GMT", None, false));
        assert!(jar.header(HTTPS, None, false).contains("e=1"));
        assert!(jar.set(HTTPS, "e=1; Path=/; Expires=Mon, 01-Jan-1990 00:00:00 GMT", None, false));
        assert!(!jar.header(HTTPS, None, false).contains("e=1"));
        // Same name, different path: two cookies, longer path first.
        jar.clear();
        assert!(jar.set(HTTPS, "p=root; Path=/", NOW, false));
        assert!(jar.set(HTTPS, "p=deep; Path=/a", NOW, false));
        assert_eq!(jar.header(HTTPS, NOW, false), "p=deep; p=root");
    }

    #[test]
    fn secure_and_http_only() {
        let mut jar = Jar::new();
        assert!(!jar.set(HTTP, "s=1; Secure", NOW, false));
        assert!(jar.set(HTTPS, "s=1; Secure; Path=/", NOW, false));
        assert_eq!(jar.header(HTTP, NOW, false), "");
        assert_eq!(jar.header(HTTPS, NOW, false), "s=1");
        // http may not shadow it.
        assert!(!jar.set(HTTP, "s=2; Path=/", NOW, false));
        assert!(jar.set(HTTPS, "h=1; HttpOnly; Path=/", NOW, false));
        assert_eq!(jar.header(HTTPS, NOW, true), "s=1");
        assert_eq!(jar.header(HTTPS, NOW, false), "s=1; h=1");
        // Scripts cannot set or overwrite HttpOnly cookies.
        assert!(!jar.set(HTTPS, "x=1; HttpOnly", NOW, true));
        assert!(!jar.set(HTTPS, "h=2; Path=/", NOW, true));
        assert_eq!(jar.header(HTTPS, NOW, false), "s=1; h=1");
        assert!(jar.set(HTTPS, "j=1; Path=/", NOW, true));
        assert_eq!(jar.header(HTTPS, NOW, true), "s=1; j=1");
        // Prefixes.
        assert!(!jar.set(HTTPS, "__Secure-a=1", NOW, false));
        assert!(jar.set(HTTPS, "__Secure-a=1; Secure", NOW, false));
        assert!(!jar.set(HTTPS, "__Host-a=1; Secure; Path=/; Domain=example.com", NOW, false));
        assert!(!jar.set(HTTPS, "__Host-a=1; Secure", NOW, false));
        assert!(jar.set(HTTPS, "__Host-a=1; Secure; Path=/", NOW, false));
    }

    #[test]
    fn caps() {
        let mut jar = Jar::new();
        let big = "v".repeat(MAX_NAME_VALUE);
        assert!(!jar.set(HTTPS, &alloc::format!("a={big}"), NOW, false));
        for i in 0..(MAX_PER_DOMAIN + 10) {
            assert!(jar.set(HTTPS, &alloc::format!("n{i}=1; Path=/"), NOW, false));
        }
        assert_eq!(jar.len(), MAX_PER_DOMAIN);
        // The oldest went first.
        assert!(!jar.header(HTTPS, NOW, false).contains("n0="));
        assert!(jar.header(HTTPS, NOW, false).contains(&alloc::format!("n{}=", MAX_PER_DOMAIN + 9)));
        let hosts: Vec<String> = (0..10).map(|i| alloc::format!("h{i}.test")).collect();
        for host in &hosts {
            for i in 0..MAX_PER_DOMAIN {
                jar.set(Origin { host, path: "/", https: true }, &alloc::format!("n{i}=1"), NOW, false);
            }
        }
        assert_eq!(jar.len(), MAX_TOTAL);
        // The Cookie header stays bounded even when every cookie matches.
        jar.clear();
        let value = "x".repeat(4000);
        for i in 0..5 {
            assert!(jar.set(HTTPS, &alloc::format!("k{i}={value}; Path=/"), NOW, false));
        }
        assert!(jar.header(HTTPS, NOW, false).len() <= MAX_HEADER);
    }

    #[test]
    fn response_heads() {
        let mut jar = Jar::new();
        let head = b"HTTP/1.1 302 Found\r\nLocation: /next\r\nSet-Cookie: a=1; Path=/\r\nset-cookie:b=2\r\nX-Set-Cookie: c=3\r\n\r\n";
        jar.take_headers(at("example.com", "/x/y", false), head, NOW);
        assert_eq!(jar.header(at("example.com", "/x/z", false), NOW, false), "b=2; a=1");
        assert_eq!(jar.header(at("example.com", "/", false), NOW, false), "a=1");
        // A bare LF does not start a header.
        jar.clear();
        let head = b"HTTP/1.1 200 OK\nSet-Cookie: evil=1\r\nX: y\nSet-Cookie: evil2=1\r\nSet-Cookie: ok=1\r\n\r\n";
        jar.take_headers(at("example.com", "/", false), head, NOW);
        assert_eq!(jar.header(at("example.com", "/", false), NOW, false), "ok=1");
        // Google's real headers.
        let head = b"HTTP/1.1 303 See Other\r\nset-cookie: SOCS=CAESNQ; Domain=.google.com; Expires=Fri, 17-Dec-2027 22:20:47 GMT; Path=/; Secure; SameSite=lax\r\nset-cookie: AEC=Aaa9; expires=Sun, 16-May-2027 22:20:36 GMT; path=/; domain=.google.com; Secure; HttpOnly; SameSite=lax\r\n\r\n";
        jar.take_headers(at("consent.google.com", "/save", true), head, NOW);
        assert_eq!(jar.header(at("www.google.com", "/search?q=x", true), NOW, false), "SOCS=CAESNQ; AEC=Aaa9");
        assert_eq!(jar.header(at("www.google.com", "/search?q=x", true), NOW, true), "SOCS=CAESNQ");
        assert_eq!(jar.header(at("www.google.com", "/", false), NOW, false), "");
    }

    #[test]
    fn same_site() {
        let mut jar = Jar::new();
        assert!(jar.set(HTTPS, "lax=1", NOW, false));
        assert!(jar.set(HTTPS, "strict=1; SameSite=Strict", NOW, false));
        assert!(jar.set(HTTPS, "none=1; SameSite=None; Secure", NOW, false));
        assert!(!jar.set(HTTPS, "insecure_none=1; SameSite=None", NOW, false));
        let same = jar.header_in(HTTPS, NOW, false, Context::SAME_SITE);
        assert!(same.contains("lax=1") && same.contains("strict=1") && same.contains("none=1"));
        // another site's form POST, image or script: only None
        let sub = jar.header_in(HTTPS, NOW, false, Context { cross_site: true, lax_ok: false });
        assert_eq!(sub, "none=1");
        // another site's link: Lax as well, never Strict
        let nav = jar.header_in(HTTPS, NOW, false, Context { cross_site: true, lax_ok: true });
        assert!(nav.contains("lax=1") && nav.contains("none=1") && !nav.contains("strict"));
    }

    #[test]
    fn sites() {
        assert_eq!(site("www.google.com"), "google.com");
        assert_eq!(site("consent.google.com"), "google.com");
        assert_eq!(site("a.b.example.co.uk"), "example.co.uk");
        assert_eq!(site("co.uk"), "co.uk");
        assert_eq!(site("user.github.io"), "user.github.io");
        assert_eq!(site("a.b.foo.s3.amazonaws.com"), "foo.s3.amazonaws.com");
        assert_eq!(site("x.waw.pl"), "x.waw.pl");
        assert_eq!(site("deep.www.ck"), "www.ck");      // exception rule !www.ck under *.ck
        assert_eq!(site("a.b.c.ck"), "b.c.ck");         // wildcard *.ck
        assert_eq!(site("10.0.2.2.nip.io"), "nip.io");   // not on the list: one label above "io"
        assert!(is_public_suffix("co.uk") && is_public_suffix("com") && is_public_suffix("github.io"));
        assert!(!is_public_suffix("example.co.uk") && !is_public_suffix("google.com"));
        assert_eq!(site("10.0.2.2"), "10.0.2.2");
        assert_eq!(site("localhost"), "localhost");
    }
}
