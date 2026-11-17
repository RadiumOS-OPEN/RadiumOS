# Builds RadiumOS apps: make -C port APP=test/hello  ->  port/out/hello.app
# Needs the runtime from port/build_runtime.sh (sysroot with musl, libc++, libunwind; libbuiltins.a).
WORK    ?= /tmp/cxx
SYSROOT := $(WORK)/sysroot
TARGET  := --target=i686-linux-musl --sysroot=$(SYSROOT)
ARCH    := -march=i686 -mno-sse -mno-mmx -fno-pic -fno-pie -fno-stack-protector -funwind-tables
CFLAGS  ?= -O2
CXXFLAGS ?= -O2 -std=c++17
BASE    ?= 0x10000000
APP     ?= test/hello
NAME    := $(notdir $(APP))

out/$(NAME).app: out/glue.o out/vfs.o out/$(NAME).o
	clang++ $(TARGET) -static -nostartfiles -nostdlib -fuse-ld=lld -no-pie \
	  -Wl,--image-base=$(BASE) -Wl,-e,radium_app_main -Wl,--gc-sections -Wl,--build-id=none -Wl,-z,norelro \
	  out/glue.o out/vfs.o out/$(NAME).o -Wl,--start-group -L$(SYSROOT)/usr/lib -lc++ -lc++abi -lunwind -lc $(WORK)/lib/libbuiltins.a -Wl,--end-group -o $@

out/glue.o: rt/glue.c rt/radium_api.h
	@mkdir -p out
	clang $(TARGET) $(ARCH) $(CFLAGS) -c $< -o $@

out/vfs.o: rt/vfs.c rt/radium_api.h
	@mkdir -p out
	clang $(TARGET) $(ARCH) $(CFLAGS) -c $< -o $@

out/$(NAME).o: $(APP).cpp
	@mkdir -p out
	clang++ $(TARGET) $(ARCH) $(CXXFLAGS) -nostdinc++ -isystem $(SYSROOT)/usr/include/c++/v1 -c $< -o $@

clean:
	rm -rf out
