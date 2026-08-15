# RadiumOS

Bare-metal operating system for i686 architecture, written in Rust and C.

## Features

- **Kernel**: Preemptive multitasking scheduler with watchdog
- **Graphics**: VBE 800×600 framebuffer + VGA text mode (80×25)
- **Networking**: RTL8139 NIC with custom TCP/IP stack, DNS, HTTPS proxy
- **Filesystem**: AVFS (Autonomous Virtual FileSystem) RAM disk
- **Scripting**: RSH v2.3 (RadiumOS Shell Language)
- **Hardware**: PS4 DualShock 4 USB HID driver (UHCI + OHCI)
- **Security**: AES-128 CBC encryption/decryption
- **Tools**: Radium Geiger hex viewer, desktop environment, 15+ apps

## Building

```bash
make clean
make run    # Launch in QEMU / If os is not built first then by default it will build then run :)
```

## Project Structure

- `src/` — Kernel and core OS code (Rust + C)
- `rust_lib/` — Rust standard library components
- `packages/` — RSH scripts and modules
- `makefile` — Build system
- `linker.ld` — Linker configuration for i686

## Author

**scp_2801** — Thorne  
RadiumOS Project Lead

NYAH -lynx
