# stb_image (vendored)

- Version: stb_image v2.30
- Source: https://github.com/nothings/stb, commit 013ac3beddff3dbffafd5177e7972067cd2b5083
  (2024-05-31), file `stb_image.h`, unmodified
  (sha256 594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3).
- License: public domain or MIT, at your option (see `LICENSE`, copied from the
  end of the header).

`stb_image.c` is the only translation unit that includes it. It configures stb for
rbrowse's `<img>` support (src/rbrowse/image.c):

- formats: PNG, JPEG (baseline and progressive), GIF (first frame) and BMP. HDR,
  PSD, PIC, PNM and TGA are compiled out.
- no stdio, no SIMD, no thread-locals (the kernel has no TLS), no float paths.
- `STBI_MAX_DIMENSIONS` is 4096; image.c applies stricter caps before decoding.
- malloc/realloc/free go to rbrowse's dedicated image decode arena
  (`rb_imgmem_*` in image.c), never to the kernel heap or the QuickJS arena.
- `STBI_ASSERT` traps, so a broken invariant ends in rbrowse's fault guard
  ("Page crashed") instead of running on with bad state.

The kernel builds it at -O2 with the QuickJS compat libc headers and `-fwrapv`
(makefile): on corrupt JPEG coefficients stb's integer IDCT overflows `int`,
which only garbles pixels; `-fwrapv` makes that defined behaviour. The host
fuzzer (tools/rbrowse/fuzz_image.c) uses the same flag.
