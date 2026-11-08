# =====================================================================
# RadiumOS Makefile
# =====================================================================

NASM = nasm
CC = clang
CARGO = cargo
LD = $(shell command -v ld.lld 2>/dev/null || command -v ld.lld-19 2>/dev/null || printf '%s' ld.lld)
QEMU = qemu-system-i386
NM = $(shell command -v llvm-nm 2>/dev/null || command -v llvm-nm-19 2>/dev/null || printf '%s' llvm-nm)

# Build C objects and the Rust library in parallel by default (override with -jN or MAKEFLAGS=-j1)
ifeq ($(filter -j%,$(MAKEFLAGS)),)
MAKEFLAGS += -j$(shell nproc 2>/dev/null || echo 4)
endif

# Audio Backend Selection: defaults to 'pipewire'
AUDIO ?= pipewire

# Flags
NASMFLAGS = -f elf32
CFLAGS = -c -target i686-none-elf -ffreestanding -mno-sse -O2 -fno-strict-aliasing -fwrapv -fstack-protector-strong -Wall -Wextra -Werror=implicit-function-declaration -MMD -MP
LDFLAGS = -T linker.ld -static -nostdlib --unresolved-symbols=ignore-all --fatal-warnings

# Directories
SRC_DIR = src
BOOT_DIR = $(SRC_DIR)/boot
GRUB_DIR = $(SRC_DIR)/grub
RUST_LIB_DIR = rust_lib
ISO_DIR = iso
BACKUP_DIR = backups

# Sources & Objects
BOOT_ASM = $(BOOT_DIR)/boot.asm
GRUB_ASM = $(GRUB_DIR)/grub.asm
C_SOURCES = $(shell find $(SRC_DIR) -name '*.c')
RUST_SOURCES = $(shell find $(RUST_LIB_DIR)/src -name '*.rs')

BOOT_OBJ = $(BOOT_DIR)/boot.o
GRUB_OBJ = $(GRUB_DIR)/grub.o
C_OBJECTS = $(C_SOURCES:.c=.o)
C_DEPS = $(C_SOURCES:.c=.d)

# Rust library
ifeq ($(wildcard $(RUST_LIB_DIR)/Cargo.toml),$(RUST_LIB_DIR)/Cargo.toml)
    RUST_LIB = $(RUST_LIB_DIR)/target/i686-radiumos/release/libradiumos_rust.a
    USE_RUST = yes
else
    RUST_LIB =
    USE_RUST = no
endif

# Output Artifacts
KERNEL_BIN = os.bin
ISO_FILE = os.iso
DISK_IMG = disk.img
FETCH_DISK_IMG = fetchdisk.img

# Guest RAM and extra QEMU options: make run QEMU_MEM=6G QEMU_FLAGS_EXTRA="-accel tcg"
QEMU_MEM ?= 4G
QEMU_FLAGS_EXTRA ?=

# QEMU Configuration Flags (Using SDL display + PipeWire audio)
QEMU_FLAGS = -display sdl \
  -accel kvm -accel tcg \
  -rtc base=utc,clock=host,driftfix=slew \
  -m $(QEMU_MEM) \
  -cdrom $(ISO_FILE) \
  -boot order=d \
  -machine pc \
  -cpu max \
  -smp 1 \
  -serial pty \
  -debugcon file:debug.log \
  -global isa-debugcon.iobase=0xE9 \
  -netdev user,id=net0 \
  -device rtl8139,netdev=net0 \
  -drive file=$(FETCH_DISK_IMG),format=raw,if=ide,index=0 \
  -vga std \
  -global VGA.vgamem_mb=16 \
  -audiodev $(AUDIO),id=audio0 \
  -device AC97,audiodev=audio0 \
  -d guest_errors $(QEMU_FLAGS_EXTRA)

# =====================================================================
# Main Build Rules
# =====================================================================

.DEFAULT_GOAL := all

.PHONY: help
help:
	@echo "RadiumOS build system"
	@echo ""
	@echo "One command:"
	@echo "  make full             build everything, install updates and games, boot QEMU"
	@echo ""
	@echo "Build:"
	@echo "  make / make all       kernel, ISO, disk images (verified)"
	@echo "  make rebuild          clean + all"
	@echo "  make clean            remove build output and images"
	@echo ""
	@echo "Test:"
	@echo "  make test             headless QEMU regression tests (ONLY=selftest|accounts|binary)"
	@echo ""
	@echo "Run:"
	@echo "  make run              build, write the update package to the disk, boot QEMU"
	@echo "  make debug            boot QEMU paused with a GDB server on :1234"
	@echo ""
	@echo "Updates:"
	@echo "  make update-package   build the signed update package in $(UPDATE_DIST)"
	@echo "  make append-update    bump VERSION, then update-package"
	@echo "  make rollback-update  restore VERSION.prev, then update-package"
	@echo "  make update-disk      install the package into $(FETCH_DISK_IMG) (QEMU must be closed)"
	@echo "                        STAGE_ONLY=1 only stages it for update apply inside RadiumOS"
	@echo "  make serve-update     serve the package over HTTP on port 8000"
	@echo ""
	@echo "Games (QEMU must be closed):"
	@echo "  make games            ac-install + doom-wad, skipping what is not available"
	@echo "  make ac-install       build AssaultCube (port/ac) and store ac.app + ac.pak"
	@echo "  make doom-wad WAD=f   store a Doom WAD in the fetch store"
	@echo ""
	@echo "Variables (make VAR=value):"
	@echo "  AUDIO=$(AUDIO)  QEMU_MEM=$(QEMU_MEM)  QEMU_FLAGS_EXTRA=..."
	@echo "  AC_DIR=$(AC_DIR)  DOOM_WAD=$(DOOM_WAD)  GAMES=$(GAMES) (0 = skip games in full)"
	@echo "  UPDATE_SIGN_KEY=$(UPDATE_SIGN_KEY)  -jN sets build parallelism (default: all cores)"

# Everything in order; the steps write the same disk image, so they must not run in parallel
# Headless regression tests in QEMU (tools/test/run_tests.py): `make test`, or ONLY=selftest|accounts|binary
.PHONY: test
test: $(KERNEL_BIN)
	python3 tools/test/run_tests.py $(if $(ONLY),--only $(ONLY),)

.PHONY: full
full:
	@$(MAKE) all
	@if [ "$(GAMES)" != "0" ]; then $(MAKE) games; fi
	@$(MAKE) run

.PHONY: all
all: check-rust-status verify-sources $(ISO_FILE) $(DISK_IMG) $(FETCH_DISK_IMG) verify-symbols
	@echo "✓ Build complete and verified"

.PHONY: check-rust-status
check-rust-status:
ifeq ($(USE_RUST),yes)
	@echo "✓ Rust support enabled"
else
	@echo "⚠ Rust support disabled - run 'make init-rust' to enable"
endif

.PHONY: verify-sources
verify-sources:
	@echo "Verifying source file discovery..."
	@FOUND=$$(find . -maxdepth 2 -name '*.c' ! -path './$(SRC_DIR)/*' 2>/dev/null); \
	if [ -n "$$FOUND" ]; then \
		echo "⚠ WARNING: .c files found OUTSIDE $(SRC_DIR)/ -- these will NOT be built:"; \
		echo "$$FOUND" | sed 's/^/    /'; \
	fi
	@echo "  Discovered $(words $(C_SOURCES)) C source file(s)."

.PHONY: verify-symbols
verify-symbols: $(KERNEL_BIN)
	@echo "Verifying symbol resolution in $(KERNEL_BIN)..."
	@UNDEF=$$($(NM) -u $(KERNEL_BIN) 2>/dev/null | awk '{print $$2}' | sort -u); \
	if [ -n "$$UNDEF" ]; then \
		echo "✗ FATAL: Undefined symbols remain in kernel binary:"; \
		echo "$$UNDEF" | sed 's/^/    /'; \
		exit 1; \
	fi
	@echo "✓ No undefined symbols"

# =====================================================================
# Compilation Rules
# =====================================================================

$(KERNEL_BIN): $(BOOT_OBJ) $(GRUB_OBJ) $(C_OBJECTS) $(RUST_LIB)
	@echo "Linking kernel..."
ifeq ($(USE_RUST),yes)
	$(LD) $(LDFLAGS) -o $@ $(BOOT_OBJ) $(GRUB_OBJ) $(C_OBJECTS) $(RUST_LIB)
else
	$(LD) $(LDFLAGS) -o $@ $(BOOT_OBJ) $(GRUB_OBJ) $(C_OBJECTS)
endif
	@echo "✓ Kernel binary created: $(KERNEL_BIN)"

$(BOOT_OBJ): $(BOOT_ASM)
	$(NASM) $(NASMFLAGS) $< -o $@

$(GRUB_OBJ): $(GRUB_ASM)
	$(NASM) $(NASMFLAGS) $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) $< -o $@

# The release version (VERSION, e.g. 5.2) is compiled into the update command.
src/commands/update.o: VERSION src/commands/update_key.h
src/commands/update.o: CFLAGS += -DRADIUM_VERSION=\"$(shell cat VERSION)\"

# The desktop's default wallpaper: the mascot at iso/boot/grub/splash.png (any format ImageMagick reads, AVIF included) made
# transparent and stored as a PNG that is linked into the kernel. Without ImageMagick the blob is empty (plain gradient).
WALLPAPER_SRC  = iso/boot/grub/splash.png
WALLPAPER_BLOB = build/wallpaper.png

$(WALLPAPER_BLOB): $(wildcard $(WALLPAPER_SRC))
	@mkdir -p build
	@if [ -f "$(WALLPAPER_SRC)" ] && convert "$(WALLPAPER_SRC)" -resize 640x640 -fuzz 9% -transparent white -trim +repage PNG32:$@ 2>/dev/null; then \
		echo "✓ Wallpaper image prepared"; \
	else \
		echo "⚠ No wallpaper image (needs ImageMagick and $(WALLPAPER_SRC)); using the gradient"; : > $@; \
	fi

src/commands/wallpaper_blob.o: $(WALLPAPER_BLOB)

# the tiling manager draws pixels by hand: optimized
src/commands/desktop.o: CFLAGS += -O2

# Doom (GPL, from ozkl/doomgeneric): its own C library headers (src/dglibc), optimized, warnings off
src/doom/%.o: CFLAGS += -I src/dglibc -w -O2 -DNORMALUNIX -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200

ifeq ($(USE_RUST),yes)
$(RUST_LIB): $(RUST_SOURCES) i686-radiumos.json .cargo/config.toml
	@echo "Building Rust library with Cargo..."
	cd $(RUST_LIB_DIR) && $(CARGO) +nightly build --release -Zbuild-std=core,alloc,compiler_builtins -Zbuild-std-features=compiler-builtins-mem -Z json-target-spec --target ../i686-radiumos.json
	@echo "✓ Rust library built"
endif

# =====================================================================
# Disk & ISO Images
# =====================================================================

# The GRUB menu is iso/boot/grub/grub.cfg (edit that file); it is copied into
# the ISO. Any PNG at iso/boot/grub/splash.png is included as the splash image.
# The kernel path inside grub.cfg is /boot/$(KERNEL_BIN).
GRUB_CFG_SRC = iso/boot/grub/grub.cfg
GRUB_MASCOT  = iso/boot/grub/splash.png

$(ISO_FILE): $(KERNEL_BIN) $(GRUB_CFG_SRC) makefile
	@echo "Making a bootable ISO..."
	@mkdir -p $(ISO_DIR)/boot/grub/fonts
	cp $(KERNEL_BIN) $(ISO_DIR)/boot/$(KERNEL_BIN)
	@if [ -f $(GRUB_MASCOT) ]; then \
		echo "✓ Splash image included"; \
	fi
	@cp /usr/share/grub/unicode.pf2 $(ISO_DIR)/boot/grub/fonts/unicode.pf2 2>/dev/null || true
	grub-mkrescue -o $@ $(ISO_DIR) 2>/dev/null
	@echo "✓ ISO created"
# ---------------------------------------------------------------------
# Updates without reflashing the ISO
# ---------------------------------------------------------------------

UPDATE_DIST = dist/update
# Everything below update/files (any depth) mirrors /disk on RadiumOS: update/files/mods/godmode.hls -> /disk/mods/godmode.hls
UPDATE_FILES = update/files
UPDATE_SOURCES := $(shell find $(UPDATE_FILES) -type f 2>/dev/null)
# PRP signing key for the manifest (python3 tools/prp_sign.py keygen); its public half is src/commands/update_key.h
UPDATE_SIGN_KEY ?= keys/update.key
# 1 = only stage the package in /disk/update (apply it inside RadiumOS with `update apply`)
STAGE_ONLY ?= 0

.PHONY: update-package
update-package: $(KERNEL_BIN) $(UPDATE_SOURCES)
	@python3 tools/mkupdate.py --out $(UPDATE_DIST) --kernel $(KERNEL_BIN) --files $(UPDATE_FILES) $(if $(wildcard $(UPDATE_SIGN_KEY)),--sign-key $(UPDATE_SIGN_KEY),)
	@if [ ! -f $(UPDATE_SIGN_KEY) ]; then echo "! $(UPDATE_SIGN_KEY) not found: package is UNSIGNED (online installs are refused). python3 tools/prp_sign.py keygen"; fi

.PHONY: append-update
append-update: $(KERNEL_BIN)
	@OLD=$$(cat VERSION); \
	MAJOR=$$(echo "$$OLD" | cut -d. -f1); \
	MINOR=$$(echo "$$OLD" | cut -d. -f2); \
	if [ "$$MINOR" -ge 9 ]; then \
		MAJOR=$$((MAJOR + 1)); \
		MINOR=0; \
	else \
		MINOR=$$((MINOR + 1)); \
	fi; \
	NEW="$$MAJOR.$$MINOR"; \
	echo "$$OLD" > VERSION.prev; \
	echo "$$NEW" > VERSION; \
	echo "↑ VERSION bumped $$OLD → $$NEW (previous saved to VERSION.prev)"
	@$(MAKE) update-package

.PHONY: rollback-update
rollback-update: $(KERNEL_BIN)
	@if [ ! -f VERSION.prev ]; then \
		echo "✗ No VERSION.prev found — nothing to roll back to"; exit 1; \
	fi; \
	CUR=$$(cat VERSION); \
	OLD=$$(cat VERSION.prev); \
	echo "$$OLD" > VERSION; \
	rm VERSION.prev; \
	echo "↓ VERSION rolled back $$CUR → $$OLD (VERSION.prev removed)"
	@$(MAKE) update-package

.PHONY: update-disk
update-disk: update-package $(FETCH_DISK_IMG)
	@if pgrep -f '^$(QEMU) .*file=$(FETCH_DISK_IMG)' >/dev/null; then \
		echo "✗ QEMU is running with $(FETCH_DISK_IMG); close it first, writing now would corrupt the disk"; exit 1; \
	fi
	@SIG=$$(od -An -tx1 -j510 -N2 $(FETCH_DISK_IMG) 2>/dev/null | tr -d ' \n'); \
	if [ "$$SIG" = "55aa" ]; then \
		MOUNT=$$(udisksctl loop-setup -f $(FETCH_DISK_IMG) --no-user-interaction 2>&1); \
		LOOP=$$(echo "$$MOUNT" | grep -o '/dev/loop[0-9]*' | head -1); \
		if [ -z "$$LOOP" ]; then echo "✗ Failed to set up loop device: $$MOUNT"; exit 1; fi; \
		MNT=$$(udisksctl mount -b $$LOOP --no-user-interaction 2>&1 | grep -o 'at .*' | sed 's/at //;s/\.$$//'); \
		if [ -z "$$MNT" ]; then \
			udisksctl loop-delete -b $$LOOP --no-user-interaction 2>/dev/null; \
			echo "✗ Failed to mount $$LOOP"; exit 1; \
		fi; \
		rm -rf $$MNT/update; \
		if [ "$(STAGE_ONLY)" = "1" ]; then \
			mkdir -p $$MNT/update; cp $(UPDATE_DIST)/* $$MNT/update/; \
			MSG="Package staged in $(FETCH_DISK_IMG):/update. In RadiumOS run: update apply, then reboot."; \
		elif python3 tools/disk_install.py $$MNT $(UPDATE_DIST); then \
			MSG="Installed into $(FETCH_DISK_IMG) (kernel, mods, apps). Just boot it."; \
		else \
			MSG="✗ Install failed"; FAILED=1; \
		fi; \
		sync; \
		udisksctl unmount -b $$LOOP --no-user-interaction; \
		udisksctl loop-delete -b $$LOOP --no-user-interaction; \
		echo "$$MSG"; \
		[ -z "$$FAILED" ] || exit 1; \
	else \
		echo "⚠ $(FETCH_DISK_IMG) not formatted yet — RadiumOS formats it on first boot. Boot once, then run make run again."; \
	fi

.PHONY: doom-wad
# Puts a Doom WAD of any size into the fetch store of fetchdisk.img: make doom-wad WAD=~/doom1.wad
doom-wad:
	@if [ -z "$(WAD)" ]; then echo "usage: make doom-wad WAD=/path/to/doom1.wad"; exit 1; fi
	@if pgrep -f '^$(QEMU) .*file=$(FETCH_DISK_IMG)' >/dev/null; then echo "✗ QEMU is running with $(FETCH_DISK_IMG); close it first"; exit 1; fi
	@python3 tools/store_put.py $(FETCH_DISK_IMG) /disk/$(notdir $(shell echo $(WAD) | tr A-Z a-z)) $(WAD)

GAMES ?= 1
DOOM_WAD ?= DOOM1.WAD

.PHONY: games
games:
	@if [ -d "$(AC_DIR)/source/src" ]; then $(MAKE) -j1 ac-install || echo "⚠ ac-install failed (see above), continuing"; else echo "- AssaultCube skipped: no clone in AC_DIR=$(AC_DIR)"; fi
	@if [ -f "$(DOOM_WAD)" ]; then $(MAKE) -j1 doom-wad WAD=$(DOOM_WAD) || echo "⚠ doom-wad failed (see above), continuing"; else echo "- Doom skipped: $(DOOM_WAD) not found"; fi

.PHONY: ac-install
# Builds AssaultCube (port/ac, needs the runtime from port/build_runtime.sh and a clone of github.com/assaultcube/AC in AC_DIR)
# and puts ac.app + ac.pak into the fetch store of fetchdisk.img (close QEMU first). In RadiumOS: app /disk/ac.app
AC_DIR ?= $(CURDIR)/port/ac/assaultcube
AC_PAK ?= $(CURDIR)/port/ac/out/ac.pak
ac-install:
	@$(MAKE) -C port/ac AC_DIR=$(AC_DIR) -j$$(nproc)
	@python3 port/tools/pack_dir.py $(AC_PAK) $(AC_DIR) config packages bot --skip packages/audio
	@if pgrep -f '^$(QEMU) .*file=$(FETCH_DISK_IMG)' >/dev/null; then echo "✗ QEMU is running with $(FETCH_DISK_IMG); close it first"; exit 1; fi
	@python3 tools/store_put.py $(FETCH_DISK_IMG) /disk/ac.app port/ac/out/ac.app
	@python3 tools/store_put.py $(FETCH_DISK_IMG) /disk/ac.pak $(AC_PAK)

.PHONY: serve-update
serve-update: update-package
	@echo "Serving $(UPDATE_DIST) on port 8000 (Ctrl-C to stop)"
	@cd $(UPDATE_DIST) && python3 -m http.server 8000

$(DISK_IMG):
	@echo "Creating FAT12 disk image..."
	@rm -f $@
	dd if=/dev/zero of=$@ bs=1024 count=1440 2>/dev/null
	mkfs.fat -F 12 -n "RADIUMOS" $@

$(FETCH_DISK_IMG):
	@echo "Creating sparse 5-GiB fetch disk..."
	truncate -s 5G $@

# =====================================================================
# Execution & Debugging
# =====================================================================

.PHONY: run
run: all update-disk
	@echo "========================================="
	@echo "Booting RadiumOS (Audio backend: $(AUDIO))"
	@echo "========================================="
	@QEMU_LOG=$$(mktemp); trap "rm -f $$QEMU_LOG" EXIT; \
	$(QEMU) $(QEMU_FLAGS) > $$QEMU_LOG 2>&1 & QEMU_PID=$$!; \
	PTS_PATH=""; \
	for i in 1 2 3 4 5 6 7 8 9 10; do \
		PTS_PATH=$$(grep -o "/dev/pts/[0-9]*" $$QEMU_LOG 2>/dev/null | tail -1); \
		if [ -n "$$PTS_PATH" ]; then break; fi; \
		sleep 0.5; \
	done; \
	if [ -n "$$PTS_PATH" ]; then \
		python3 tools/serial_bridge.py "$$PTS_PATH" & BRIDGE_PID=$$!; \
	else \
		BRIDGE_PID=0; \
	fi; \
	wait $$QEMU_PID; \
	if [ "$$BRIDGE_PID" != "0" ]; then kill $$BRIDGE_PID 2>/dev/null; fi; \
	echo "[!] QEMU closed."

.PHONY: debug
debug: all
	$(QEMU) $(QEMU_FLAGS) -s -S

.PHONY: clean
clean:
	@echo "Cleaning up..."
	@rm -f $(BOOT_OBJ) $(GRUB_OBJ) $(C_OBJECTS) $(C_DEPS)
	@if [ -d "$(RUST_LIB_DIR)" ]; then cd $(RUST_LIB_DIR) && cargo clean 2>/dev/null || true; fi
	@rm -f $(KERNEL_BIN) $(ISO_FILE) $(DISK_IMG) $(FETCH_DISK_IMG) test.txt debug.log
	@rm -rf $(ISO_DIR)/boot/$(KERNEL_BIN) build
	@echo "✓ Clean complete"

.PHONY: rebuild
rebuild: clean all

-include $(C_DEPS)