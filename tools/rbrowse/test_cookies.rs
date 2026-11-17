// Host tests of rbrowse's cookie jar (rust_lib/src/fetch/cookies.rs). The
// crate itself only builds for the kernel, so this compiles the module alone:
//   T=$(mktemp -d); rustc --edition 2021 --test tools/rbrowse/test_cookies.rs -o $T/tc && $T/tc
extern crate alloc;

#[path = "../../rust_lib/src/fetch/cookies.rs"]
mod cookies;
