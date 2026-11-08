# winrun for RadiumOS: make -C port/win  ->  port/win/out/winrun.app   (run with: app /disk/winrun.app PROGRAM.EXE)
# Needs the runtime from port/build_runtime.sh (musl sysroot + libbuiltins.a).
WORK    ?= /tmp/cxx
SYSROOT := $(WORK)/sysroot
TARGET  := --target=i686-linux-musl --sysroot=$(SYSROOT)
ARCH    := -march=i686 -mno-sse -mno-mmx -fno-pic -fno-pie -fno-stack-protector -funwind-tables
RT      := ../rt
OUT     := out
OBJS    := $(OUT)/glue.o $(OUT)/vfs.o $(OUT)/winrun.o $(OUT)/pe.o $(OUT)/k32.o $(OUT)/crt.o $(OUT)/gui.o

$(OUT)/winrun.app: $(OBJS)
	clang $(TARGET) -static -nostartfiles -nostdlib -fuse-ld=lld -no-pie \
	  -Wl,--image-base=0x10000000 -Wl,-e,radium_app_main -Wl,--gc-sections -Wl,--build-id=none -Wl,-z,norelro \
	  $(OBJS) -Wl,--start-group -L$(SYSROOT)/usr/lib -lc $(WORK)/lib/libbuiltins.a -Wl,--end-group -o $@

$(OUT)/glue.o: $(RT)/glue.c $(RT)/radium_api.h
	@mkdir -p $(OUT)
	clang $(TARGET) $(ARCH) -O2 -c $< -o $@

$(OUT)/vfs.o: $(RT)/vfs.c $(RT)/radium_api.h
	@mkdir -p $(OUT)
	clang $(TARGET) $(ARCH) -O2 -c $< -o $@

$(OUT)/%.o: %.c winapi.h $(RT)/radium_api.h
	@mkdir -p $(OUT)
	clang $(TARGET) $(ARCH) -O2 -Wall -Wno-unused-function -Wno-incompatible-pointer-types -Wno-pointer-sign -c $< -o $@

clean:
	rm -rf $(OUT)
