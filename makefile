#!/usr/bin/make -f
# Compiler and tools configuration
NASM = nasm
CC = clang
CARGO = cargo
LD = $(shell command -v ld.lld 2>/dev/null || command -v ld.lld-19 2>/dev/null || printf '%s' ld.lld)
QEMU = qemu-system-i386
NM = $(shell command -v llvm-nm 2>/dev/null || command -v llvm-nm-19 2>/dev/null || printf '%s' llvm-nm)

# Compiler flags
# Added -MMD -MP for automatic header dependency tracking
NASMFLAGS = -f elf32
CFLAGS = -c -target i686-none-elf -ffreestanding -mno-sse -Wall -Wextra -Werror=implicit-function-declaration -MMD -MP
# Added --unresolved-symbols=ignore-all so the binary is produced for our NM verification script
LDFLAGS = -T linker.ld -static -nostdlib --unresolved-symbols=ignore-all --fatal-warnings

# Directories
SRC_DIR = src
BOOT_DIR = $(SRC_DIR)/boot
GRUB_DIR = $(SRC_DIR)/grub
RUST_LIB_DIR = rust_lib
ISO_DIR = iso
BACKUP_DIR = backups

# Source files
BOOT_ASM = $(BOOT_DIR)/boot.asm
GRUB_ASM = $(GRUB_DIR)/grub.asm
C_SOURCES = $(shell find $(SRC_DIR) -name '*.c')

# Object files and dependencies
BOOT_OBJ = $(BOOT_DIR)/boot.o
GRUB_OBJ = $(GRUB_DIR)/grub.o
C_OBJECTS = $(C_SOURCES:.c=.o)
C_DEPS = $(C_SOURCES:.c=.d)

# Check if Rust library exists
ifeq ($(wildcard $(RUST_LIB_DIR)/Cargo.toml),$(RUST_LIB_DIR)/Cargo.toml)
    RUST_LIB = $(RUST_LIB_DIR)/target/i686-radiumos/release/libradiumos_rust.a
    USE_RUST = yes
else
    RUST_LIB = 
    USE_RUST = no
endif

# Output files
KERNEL_BIN = os.bin
ISO_FILE = os.iso
DISK_IMG = disk.img

QEMU_FLAGS = -display sdl \
  -rtc base=utc,clock=host,driftfix=slew \
  -m 4G \
  -cdrom $(ISO_FILE) \
  -boot order=d \
  -machine pc \
  -smp 1 \
  -serial pty \
  -debugcon file:debug.log \
  -global isa-debugcon.iobase=0xE9 \
  -netdev user,id=net0 \
  -device rtl8139,netdev=net0 \
  -vga std \
  -global VGA.vgamem_mb=16 \
  -audiodev pa,id=audio0 \
  -machine pcspk-audiodev=audio0 \
  -d guest_errors

# Default target
.PHONY: all
all: check-rust-status verify-sources $(ISO_FILE) $(DISK_IMG) verify-symbols
	@echo "✓ Build complete and verified"

# Check Rust status
.PHONY: check-rust-status
check-rust-status:
ifeq ($(USE_RUST),yes)
	@echo "✓ Rust support enabled"
else
	@echo "⚠ Rust support disabled - run 'make init-rust' to enable"
endif

# ---------------------------------------------------------------------
# SOURCE VERIFICATION
# ---------------------------------------------------------------------
.PHONY: verify-sources
verify-sources:
	@echo "Verifying source file discovery..."
	@FOUND=$$(find . -maxdepth 2 -name '*.c' ! -path './$(SRC_DIR)/*' 2>/dev/null); \
	if [ -n "$$FOUND" ]; then \
		echo "⚠ WARNING: .c files found OUTSIDE $(SRC_DIR)/ -- these will NOT be built:"; \
		echo "$$FOUND" | sed 's/^/    /'; \
		echo "  Move them into $(SRC_DIR)/ (any subdirectory) to include them."; \
	fi
	@echo "  Discovered $(words $(C_SOURCES)) C source file(s)."
	@if [ -z "$(C_SOURCES)" ]; then \
		echo "  ⚠ WARNING: no C sources found under $(SRC_DIR)/"; \
	fi

# ---------------------------------------------------------------------
# SYMBOL VERIFICATION
# ---------------------------------------------------------------------
.PHONY: verify-symbols
verify-symbols: $(KERNEL_BIN)
	@echo "Verifying symbol resolution in $(KERNEL_BIN)..."
	@UNDEF=$$($(NM) -u $(KERNEL_BIN) 2>/dev/null | awk '{print $$2}' | sort -u); \
	if [ -n "$$UNDEF" ]; then \
		echo "✗ FATAL: Undefined symbols remain in kernel binary:"; \
		echo "$$UNDEF" | sed 's/^/    /'; \
		echo "  These functions are declared/called but never defined+linked."; \
		echo "  Check: is the .c defining them actually in $(SRC_DIR)/ ?"; \
		exit 1; \
	fi
	@echo "✓ No undefined symbols"

.PHONY: check-symbol
check-symbol: $(KERNEL_BIN)
	@if [ -z "$(SYM)" ]; then echo "Usage: make check-symbol SYM=funcname"; exit 1; fi
	@RESULT=$$($(NM) $(KERNEL_BIN) | grep -w "$(SYM)"); \
	if [ -z "$$RESULT" ]; then \
		echo "✗ '$(SYM)' not found in binary at all (not compiled/linked)"; \
	else \
		echo "$$RESULT"; \
		echo "$$RESULT" | grep -qi ' U ' && echo "⚠ '$(SYM)' is UNDEFINED (declared but never defined)"; \
		echo "$$RESULT" | grep -qi ' T \| t ' && echo "✓ '$(SYM)' is defined (has real code)"; \
	fi

# Build kernel binary
$(KERNEL_BIN): $(BOOT_OBJ) $(GRUB_OBJ) $(C_OBJECTS) $(RUST_LIB)
	@echo "Linking kernel..."
ifeq ($(USE_RUST),yes)
	$(LD) $(LDFLAGS) -o $@ $(BOOT_OBJ) $(GRUB_OBJ) $(C_OBJECTS) $(RUST_LIB)
else
	$(LD) $(LDFLAGS) -o $@ $(BOOT_OBJ) $(GRUB_OBJ) $(C_OBJECTS)
endif
	@echo "✓ Kernel binary created: $(KERNEL_BIN)"

$(BOOT_OBJ): $(BOOT_ASM)
	@echo "Building boot.asm"
	$(NASM) $(NASMFLAGS) $< -o $@

$(GRUB_OBJ): $(GRUB_ASM)
	@echo "Building grub.asm"
	$(NASM) $(NASMFLAGS) $< -o $@

# Build C object files (Headers tracked automatically via .d files)
%.o: %.c
	@echo "Compiling C: $<"
	$(CC) $(CFLAGS) $< -o $@

# Build Rust library with Cargo
ifeq ($(USE_RUST),yes)
$(RUST_LIB): $(wildcard $(RUST_LIB_DIR)/src/*.rs) i686-radiumos.json .cargo/config.toml
	@echo "Building Rust library with Cargo..."
	cd $(RUST_LIB_DIR) && $(CARGO) +nightly build --release -Zbuild-std=core,compiler_builtins -Zbuild-std-features=compiler-builtins-mem -Zjson-target-spec --target ../i686-radiumos.json
	@echo "✓ Rust library built"
endif

# Create bootable ISO
$(ISO_FILE): $(KERNEL_BIN)
	@echo "Making a bootable ISO"
	@mkdir -p $(ISO_DIR)/boot/grub
	@echo 'set timeout=0' > $(ISO_DIR)/boot/grub/grub.cfg
	@echo 'set default=0' >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo 'menuentry "RadiumOS" {' >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '  multiboot /boot/$(KERNEL_BIN)' >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '  boot' >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '}' >> $(ISO_DIR)/boot/grub/grub.cfg
	cp $(KERNEL_BIN) $(ISO_DIR)/boot/$(KERNEL_BIN)
	grub-mkrescue -o $@ $(ISO_DIR) 2>/dev/null
	@echo "✓ ISO created"

# Create FAT12 disk image
$(DISK_IMG):
	@echo "Creating FAT12 disk image"
	@rm -f $@
	dd if=/dev/zero of=$@ bs=1024 count=1440 2>/dev/null
	mkfs.fat -F 12 -n "RADIUMOS" $@
	@if command -v mtools >/dev/null 2>&1; then \
		echo "Hello from RadiumOS!" > test.txt; \
		mcopy -i $@ test.txt ::test.txt 2>/dev/null || true; \
		rm -f test.txt; \
	fi
	@echo "✓ Disk image created"

define SERIAL_LISTENER_SCRIPT
#!/bin/sh
PTS_PATH="$$1"
CLIPFILE="$$2"
CLIP_CMD=""
if command -v wl-copy >/dev/null 2>&1; then CLIP_CMD="wl-copy"; elif command -v xclip >/dev/null 2>&1; then CLIP_CMD="xclip -selection clipboard"; elif command -v xsel >/dev/null 2>&1; then CLIP_CMD="xsel -i -b"; fi
ACCUM=""
while IFS= read -r line || [ -n "$$line" ]; do
	CLEAN=$$(echo "$$line" | tr -d '\r')
	if [ -z "$$CLEAN" ]; then
		if [ -n "$$ACCUM" ]; then
			echo "      [Serial: Clipboard Updated]"
			if [ -n "$$CLIP_CMD" ]; then printf "%s" "$$ACCUM" | $$CLIP_CMD; else printf "%s" "$$ACCUM" > "$$CLIPFILE"; fi
			ACCUM=""
		fi
	else
		if [ -z "$$ACCUM" ]; then ACCUM="$$CLEAN"; else ACCUM="$${ACCUM}\n$$CLEAN"; fi
	fi
done < "$$PTS_PATH"
endef
export SERIAL_LISTENER_SCRIPT

.PHONY: run
run: all
	@echo "========================================="
	@echo "Booting RadiumOS"
	@echo "========================================="
	@command -v xclip >/dev/null 2>&1 || command -v wl-copy >/dev/null 2>&1 || echo "[!] WARNING: No clipboard tool (xclip/wl-copy) found."
	@QEMU_LOG=$$(mktemp); CLIP_FILE=$$(mktemp); trap "rm -f $$QEMU_LOG $$CLIP_FILE" EXIT; \
	$(QEMU) $(QEMU_FLAGS) > $$QEMU_LOG 2>&1 & QEMU_PID=$$!; echo "[1] QEMU PID: $$QEMU_PID"; \
	PTS_PATH=""; echo "[*] Waiting for Serial Port (PTY) path..."; \
	for i in 1 2 3 4 5 6 7 8 9 10; do \
		PTS_PATH=$$(grep -o "/dev/pts/[0-9]*" $$QEMU_LOG 2>/dev/null | tail -1); \
		if [ -n "$$PTS_PATH" ]; then echo "[2] Serial bridge active on $$PTS_PATH"; break; fi; \
		sleep 0.5; \
	done; \
	if [ -n "$$PTS_PATH" ]; then \
		echo "$$SERIAL_LISTENER_SCRIPT" | sh -s "$$PTS_PATH" "$$CLIP_FILE" & BRIDGE_PID=$$!; \
		echo "[*] Clipboard listener started (PID $$BRIDGE_PID)"; \
	else \
		echo "[!] No PTY detected."; BRIDGE_PID=0; \
	fi; \
	echo "[3] QEMU running. Waiting..."; wait $$QEMU_PID; \
	if [ "$$BRIDGE_PID" != "0" ]; then kill $$BRIDGE_PID 2>/dev/null; fi; \
	echo "[!] QEMU closed."

# Debug target
.PHONY: debug
debug: all
	@echo "========================================="
	@echo "Starting QEMU in GDB Debug Mode"
	@echo "Waiting for debugger connection on localhost:1234..."
	@echo "========================================="
	$(QEMU) $(QEMU_FLAGS) -s -S

# Snapshot/Backup target
.PHONY: backup
backup:
	@echo "Creating source snapshot..."
	@mkdir -p $(BACKUP_DIR)
	@tar -czf $(BACKUP_DIR)/radiumos_src_$$(date +%Y%m%d_%H%M%S).tar.gz \
		$(SRC_DIR)/ $(RUST_LIB_DIR)/src/ $(RUST_LIB_DIR)/Cargo.toml \
		Makefile linker.ld 2>/dev/null || true
	@echo "✓ Snapshot saved to $(BACKUP_DIR)/"

# Clean build artifacts
.PHONY: clean
clean:
	@echo "Cleaning up..."
	@rm -f $(BOOT_OBJ) $(GRUB_OBJ) $(C_OBJECTS) $(C_DEPS)
	@if [ -d "$(RUST_LIB_DIR)" ]; then cd $(RUST_LIB_DIR) && cargo clean 2>/dev/null || true; fi
	@rm -f $(KERNEL_BIN) $(ISO_FILE) $(DISK_IMG) test.txt debug.log
	@rm -rf $(ISO_DIR)/boot/$(KERNEL_BIN)
	@echo "✓ Clean complete"

.PHONY: rebuild
rebuild: clean all

.PHONY: check-rust
check-rust:
	@echo "Checking Rust toolchain..."
	@if ! command -v cargo >/dev/null 2>&1; then echo "✗ Cargo not found."; exit 1; fi
	@echo "✓ Cargo found: $$(cargo --version)"
	@if ! rustup toolchain list | grep -q nightly; then rustup toolchain install nightly; fi
	@echo "✓ Nightly toolchain available"
	@if ! rustup component list --toolchain nightly | grep -q "rust-src (installed)"; then rustup component add rust-src --toolchain nightly; fi
	@echo "✓ rust-src installed"

.PHONY: init-rust
init-rust: check-rust
	@echo "========================================="
	@echo "Initializing Rust Support for RadiumOS"
	@echo "========================================="
	@mkdir -p .cargo
	@if [ ! -f ".cargo/config.toml" ]; then \
		printf '[unstable]\nbuild-std = ["core", "compiler_builtins"]\nbuild-std-features = ["compiler-builtins-mem"]\n\n[build]\ntarget = "i686-radiumos.json"\n\n[target.i686-radiumos]\nrustflags = [\n    "-C", "relocation-model=static",\n    "-C", "code-model=kernel",\n]\n' > .cargo/config.toml; \
		echo "✓ Created .cargo/config.toml"; \
	fi
	@if [ ! -f "i686-radiumos.json" ]; then \
		printf '{\n  "llvm-target": "i686-unknown-none",\n  "data-layout": "e-m:e-p:32:32-p270:32:32-p271:32:32-p272:64:64-f64:32:64-f80:32-n8:16:32-S128",\n  "arch": "x86",\n  "target-endian": "little",\n  "target-pointer-width": 32,\n  "target-c-int-width": 32,\n  "os": "none",\n  "linker-flavor": "ld.lld",\n  "linker": "ld.lld",\n  "panic-strategy": "abort",\n  "disable-redzone": true,\n  "features": "-mmx,-sse,+soft-float",\n  "relocation-model": "static",\n  "code-model": "kernel"\n}\n' > i686-radiumos.json; \
		echo "✓ Created i686-radiumos.json"; \
	fi
	@if [ ! -d "$(RUST_LIB_DIR)" ]; then cargo new $(RUST_LIB_DIR) --lib --name radiumos_rust; fi
	@printf '[package]\nname = "radiumos_rust"\nversion = "0.1.0"\nedition = "2021"\n\n[lib]\ncrate-type = ["staticlib"]\n\n[profile.release]\npanic = "abort"\nlto = true\nopt-level = 2\n' > $(RUST_LIB_DIR)/Cargo.toml
	@echo "✓ Rust support initialized!"

.PHONY: info
info:
	@echo "========================================="
	@echo "RadiumOS Build Information"
	@echo "========================================="
	@echo "Rust Support: $(USE_RUST)"
	@echo "C Sources:    $(words $(C_SOURCES)) files"
	@for f in $(C_SOURCES); do echo "  - $$f"; done
	@echo "========================================="

.PHONY: help
help:
	@echo "RadiumOS Makefile"
	@echo ""
	@echo "Build & Run:"
	@echo "  make              - Build everything"
	@echo "  make run          - Build and run in QEMU"
	@echo "  make debug        - Run in QEMU, pause for GDB connection (:1234)"
	@echo "  make backup       - Create a tarball of your source code"
	@echo "  make rebuild      - Clean and rebuild"
	@echo ""
	@echo "Debugging build issues:"
	@echo "  make verify-sources        - List which .c files are picked up"
	@echo "  make verify-symbols        - Fail if any undefined symbols remain"
	@echo "  make check-symbol SYM=name - Check if a function is really linked"
	@echo ""
	@echo "Rust:"
	@echo "  make init-rust  - Setup Rust support"
	@echo "  make check-rust - Verify Rust toolchain"
	@echo ""
	@echo "Other:"
	@echo "  make clean - Clean build artifacts safely"
	@echo "  make info  - Show build info"

# Include auto-generated dependency files
-include $(C_DEPS)