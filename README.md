# RadiumOS

Bare-metal operating system for i686 architecture, written in Rust and C.

## Features

- **Kernel**: Preemptive multitasking scheduler with watchdog, stack guards, Ctrl+C handling, TSC-based timers and boot profiling
- **Graphics**: VBE framebuffer console (up to 1280×720) with a scaler, animated overlays, boot page and wallpaper support, plus legacy VGA text mode and Mode 13h
- **Vulkan**: Software Vulkan implementation (`rust_lib/src/vulkan`) with a SPIR-V interpreter and rasteriser; `vulkaninfo` / `vkdemo` commands
- **Desktop**: RadiumDE, a small Xfce-style desktop environment (`desktop`)
- **Networking**: RTL8139 NIC with custom TCP/IP stack, UDP sockets, DNS, HTTPS proxy and WebSocket support
- **Filesystem**: AVFS RAM disk, FAT32 `/disk` volume, a fetch/disk store, `fsck`, `diskpart` and `mv`/`find`/`tree`/`stat`/`du`/`diff` and other core utilities
- **Users & security**: Multi-user accounts with login, `sudo`, passphrase file encryption (`crypt`), AES-128 CBC, a kernel guard (`guard`), `secaudit`, Ed25519-signed updates and a `selftest` regression command
- **Services**: `rsd`, a small systemd-style service manager using `NAME.svc` unit files
- **Updates**: `update` command for installing signed update packages without rebuilding (`make update-package`, `make serve-update`)
- **Scripting**: RSH v2.3 (RadiumOS Shell Language) with shell variables and text filters
- **Helios**: Compiled systems language targeting the RXE v6 format (see [helios.md](helios.md))
- **Media & decoding**: PNG and generic image decoding, MP4 / H.264 / AAC support, emoji rendering
- **Apps & games**: Static ELF app runner (C++ on musl), Doom (doomgeneric, with Helios mods), AssaultCube port, Discord client and a text-based YouTube Music client
- **Hardware**: Keyboard driver (most keybinds working!), clipboard support
- **Tools**: Radium Geiger hex viewer; helper scripts in `tools/`

## Building

### Prerequisites

System tools:

- `nasm`, `clang`, `lld` (for `ld.lld`), `llvm` (for `llvm-nm`), `qemu-system-i386` / `qemu-system-x86`, `grub-pc-bin`/`grub2` (for `grub-mkrescue`), `xorriso`, `mtools`

```bash
# Debian / Ubuntu
sudo apt install nasm clang lld llvm qemu-system-x86 grub-pc-bin xorriso mtools

# Fedora
sudo dnf install nasm clang lld llvm qemu-system-x86 grub2-tools xorriso mtools

# Arch
sudo pacman -S nasm clang lld llvm qemu-system-x86 grub xorriso mtools
```

Rust toolchain — required for the Rust component (`rust_lib/`):

- [`rustup`](https://rustup.rs). A distro-packaged `cargo`/`rustc` (e.g.
  `apt install cargo`) will **not** work, since it doesn't understand the
  `+nightly` toolchain syntax used by the build:

  ```bash
  curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y
  source "$HOME/.cargo/env"
  ```

- The `nightly` toolchain and `rust-src` component — installed automatically
  by `make init-rust` / `make check-rust` (see below), or manually:

  ```bash
  rustup toolchain install nightly
  rustup component add rust-src --toolchain nightly
  ```

- Rust crate dependencies (pinned in `rust_lib/Cargo.lock`, with local
  crates under `rust_lib/vendor/`) are fetched automatically by Cargo on first build —
  no manual step needed.

```bash
make init-rust   # first-time only: sets up nightly + rust-src + target spec
make clean
make run    # Launch in QEMU / If os is not built first then by default it will build then run :)
```

### Troubleshooting

<details>
<summary><code>error: no such command: `+nightly`</code></summary>

Your `cargo` isn't rustup's shim. Run `which cargo` — if it doesn't point into
`~/.cargo/bin`, install rustup and make sure `~/.cargo/bin` comes first in
`PATH`:

```bash
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y
source "$HOME/.cargo/env"
```
</details>

<details>
<summary><code>error: `.json` target specs require -Zjson-target-spec</code></summary>

Recent nightly toolchains require this flag explicitly on the command line
for custom JSON target specs like `i686-radiumos.json`. This is handled
automatically by the Makefile — if you see this error, update to the latest
Makefile from this repo.
</details>

<details>
<summary><code>error: ... does not exist, unable to build with the standard library</code></summary>

The `rust-src` component isn't installed for your nightly toolchain:

```bash
rustup component add rust-src --toolchain nightly
```

`make check-rust` (run automatically by `make init-rust`) checks for and
installs this for you.
</details>

### Useful make targets

Run `make help` for the full list. Highlights:

- `make verify-symbols` — fail the build if any undefined symbols remain in the kernel binary
- `make check-symbol SYM=funcname` — check whether a specific function is compiled and linked in
- `make info` — list discovered C sources and current Rust-support status
- `make full` — build everything, install updates and games, then boot QEMU
- `make test` — headless QEMU regression tests (`ONLY=selftest|accounts|binary`)
- `make debug` — boot QEMU paused with a GDB server on `:1234`
- `make update-package` / `make append-update` / `make rollback-update` — build, bump or roll back the signed update package
- `make games` — install AssaultCube and a Doom WAD (`make doom-wad WAD=file`) into the fetch store
- `make check-rust` / `make init-rust` — verify or set up the nightly Rust toolchain

## Project Structure

- `src/` — Kernel and core OS code (C)
- `rust_lib/` — Rust standard library components (With Rust SRC) and vendored crates
- `packages/` — RSH scripts and modules
- `tools/` — Build, signing, disk-install and asset-generation scripts
- `update/` — Update package payload
- `port/` — Ported applications (AssaultCube, musl runtime)
- `makefile` — Build system
- `linker.ld` — Linker configuration for i686

## Author

**scp_2801** — Thorne  
RadiumOS Project Lead

NYAH -lynx
