# RadiumOS

Bare-metal operating system for i686 architecture, written in Rust and C.

## Features

- **Kernel**: Preemptive multitasking scheduler with watchdog
- **Graphics**: VBE 800×600 framebuffer + VGA text mode (80×25)
- **Networking**: RTL8139 NIC with custom TCP/IP stack, DNS, HTTPS proxy
- **Filesystem**: AVFS (Autonomous Virtual FileSystem) RAM disk
- **Scripting**: RSH v2.3 (RadiumOS Shell Language)
- **Hardware**: PS4 DualShock 4 USB HID driver (UHCI + OHCI)
- **Emulation**: Complete NES emulator with 6502 CPU, PPU, Mappers 0-4
- **Security**: AES-128 CBC encryption/decryption
- **Tools**: Radium Geiger hex viewer, desktop environment, 15+ apps

## Building

```bash
make clean
make build
make run    # Launch in QEMU
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

## License

Proprietary / All Rights Reserved (or choose your license)
