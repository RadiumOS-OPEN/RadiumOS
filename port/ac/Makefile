# AssaultCube for RadiumOS: make -C port/ac  ->  port/ac/out/ac.app   (run with: app /disk/ac.app)
# Needs the runtime from port/build_runtime.sh; the AssaultCube clone lives in $(AC_DIR) (port/ac/assaultcube, fetched on demand).
WORK    ?= /tmp/cxx
HERE    := $(abspath .)
AC_DIR  ?= $(HERE)/assaultcube
AC_REV  := 13f0d8eea4822dee5c976661218d022be74342e3
SYSROOT := $(WORK)/sysroot
RT      := $(abspath ../rt)
SRC     := $(AC_DIR)/source/src
ENET    := $(AC_DIR)/source/enet
TARGET  := --target=i686-linux-musl --sysroot=$(SYSROOT)
ARCH    := -march=i686 -mno-sse -mno-mmx -fno-pic -fno-pie -fno-stack-protector -funwind-tables
INCS    := -I$(HERE)/include -I$(HERE) -I$(AC_DIR)/source/include -I$(SRC) -I$(SRC)/bot -I$(ENET)/include
CXXFLAGS ?= -DRADIUMOS -O2 -std=gnu++14 -fsigned-char -fno-strict-aliasing -w -fpermissive
OUT     := out

ENGINE := crypto audiomanager autodownload client clientgame clients2c command console docs editing entities http log main menus \
          oggstream openal packetqueue physics protocol rendercubes rendergl renderhud rendermodel renderparticles rendertext rndmap \
          scoreboard serverms server serverbrowser shadow soundlocation soundscheduler stream texture tools water weapon wizard world \
          worldio worldlight worldocull worldrender zip bot/bot bot/botmanager bot/bot_ai bot/bot_util bot/bot_waypoint bot/ac_bot bot/ac_bot_ai
ENETSRC := callbacks compress host list packet peer protocol unix
OBJS := $(addsuffix .o,$(addprefix $(OUT)/e_,$(subst /,_,$(ENGINE)))) $(addsuffix .o,$(addprefix $(OUT)/n_,$(ENETSRC))) \
        $(OUT)/softgl.o $(OUT)/sdl_radium.o $(OUT)/al_stub.o $(OUT)/launch.o $(OUT)/glue.o $(OUT)/vfs.o $(OUT)/sock.o $(OUT)/acmods.o

# the small changes AssaultCube needs on RadiumOS (no threads, init script after the main loop starts): ac.patch, applied once
.DEFAULT_GOAL := $(OUT)/ac.app
.PHONY: patch
patch: $(SRC)/main.cpp
	@cd $(AC_DIR) && (git apply --reverse --check $(HERE)/ac.patch >/dev/null 2>&1 || git apply $(HERE)/ac.patch)

$(SRC)/main.cpp:
	git clone https://github.com/assaultcube/AC $(AC_DIR) && git -C $(AC_DIR) checkout $(AC_REV)

$(OUT)/ac.app: $(OBJS)
	clang++ $(TARGET) -static -nostartfiles -nostdlib -fuse-ld=lld -no-pie \
	  -Wl,--image-base=0x10000000 -Wl,-e,radium_app_main -Wl,--gc-sections -Wl,--build-id=none -Wl,-z,norelro \
	  $(OBJS) -Wl,--start-group -L$(SYSROOT)/usr/lib -lc++ -lc++abi -lunwind -lz -lc $(WORK)/lib/libbuiltins.a -Wl,--end-group -o $@

$(OUT)/e_%.o: $(SRC)/%.cpp | patch
	@mkdir -p $(OUT)
	clang++ $(TARGET) $(ARCH) $(CXXFLAGS) $(INCS) -nostdinc++ -isystem $(SYSROOT)/usr/include/c++/v1 -c $< -o $@

# bot/foo.cpp -> out/e_bot_foo.o
$(OUT)/e_bot_%.o: $(SRC)/bot/%.cpp | patch
	@mkdir -p $(OUT)
	clang++ $(TARGET) $(ARCH) $(CXXFLAGS) $(INCS) -nostdinc++ -isystem $(SYSROOT)/usr/include/c++/v1 -c $< -o $@

$(OUT)/e_main.o: $(SRC)/main.cpp | patch
	@mkdir -p $(OUT)
	clang++ $(TARGET) $(ARCH) $(CXXFLAGS) -Dmain=ac_main $(INCS) -nostdinc++ -isystem $(SYSROOT)/usr/include/c++/v1 -c $< -o $@

$(OUT)/n_%.o: $(ENET)/%.c
	@mkdir -p $(OUT)
	clang $(TARGET) $(ARCH) -O2 -w -DHAS_FCNTL=1 -DHAS_SOCKLEN_T=1 -DHAS_POLL=1 -DHAS_GETNAMEINFO=1 -DHAS_GETADDRINFO=1 -DHAS_INET_PTON=1 -DHAS_INET_NTOP=1 -I$(ENET)/include -c $< -o $@

$(OUT)/softgl.o: softgl.cpp include/GL/gl.h
	@mkdir -p $(OUT)
	clang++ $(TARGET) $(ARCH) -O2 -std=c++17 -nostdinc++ -isystem $(SYSROOT)/usr/include/c++/v1 -I$(HERE)/include -c $< -o $@

$(OUT)/sdl_radium.o: sdl_radium.cpp
	@mkdir -p $(OUT)
	clang++ $(TARGET) $(ARCH) -O2 -std=gnu++14 -w $(INCS) -nostdinc++ -isystem $(SYSROOT)/usr/include/c++/v1 -c $< -o $@

$(OUT)/al_stub.o: al_stub.cpp
	@mkdir -p $(OUT)
	clang++ $(TARGET) $(ARCH) -O2 -std=gnu++14 -w $(INCS) -nostdinc++ -isystem $(SYSROOT)/usr/include/c++/v1 -c $< -o $@

$(OUT)/launch.o: launch.cpp
	@mkdir -p $(OUT)
	clang++ $(TARGET) $(ARCH) -O2 -std=gnu++14 -nostdinc++ -isystem $(SYSROOT)/usr/include/c++/v1 -c $< -o $@

$(OUT)/glue.o: $(RT)/glue.c $(RT)/radium_api.h
	@mkdir -p $(OUT)
	clang $(TARGET) $(ARCH) -O2 -c $< -o $@

$(OUT)/acmods.o: acmods.cpp $(RT)/radium_api.h | patch
	@mkdir -p $(OUT)
	clang++ $(TARGET) $(ARCH) $(CXXFLAGS) $(INCS) -I$(RT) -nostdinc++ -isystem $(SYSROOT)/usr/include/c++/v1 -c $< -o $@

$(OUT)/sock.o: $(RT)/sock.c $(RT)/radium_api.h
	@mkdir -p $(OUT)
	clang $(TARGET) $(ARCH) -O2 -c $< -o $@

$(OUT)/vfs.o: $(RT)/vfs.c $(RT)/radium_api.h Makefile
	@mkdir -p $(OUT)
	clang $(TARGET) $(ARCH) -O2 -DPACK_PATH='"/disk/ac.pak"' -DSAVE_PATH='"/disk/ac.sav"' -DPACK_MOUNT='"ac"' -c $< -o $@

clean:
	rm -rf $(OUT)

# ---- host test build (Linux x86-64): make -C port/ac host; then run in the AC clone: (cd port/ac/assaultcube && AC_HOME=/tmp/achome /tmp/achost/ac_host)
HOSTOUT := /tmp/achost
HOSTOBJS := $(addsuffix .o,$(addprefix $(HOSTOUT)/e_,$(subst /,_,$(ENGINE)))) $(addsuffix .o,$(addprefix $(HOSTOUT)/n_,$(ENETSRC))) \
            $(HOSTOUT)/acmods.o $(HOSTOUT)/softgl.o $(HOSTOUT)/sdl_radium.o $(HOSTOUT)/al_stub.o $(HOSTOUT)/launch.o $(HOSTOUT)/host_api.o
HOSTCXX := clang++ -DRADIUMOS -O1 -g -std=gnu++14 -fsigned-char -fno-strict-aliasing -fpermissive -w

host: $(HOSTOUT)/ac_host

$(HOSTOUT)/ac_host: $(HOSTOBJS)
	clang++ $(HOSTOBJS) -lz -lm -pthread -o $@

$(HOSTOUT)/e_main.o: $(SRC)/main.cpp | patch
	@mkdir -p $(HOSTOUT)
	$(HOSTCXX) -Dmain=ac_main $(INCS) -c $< -o $@
$(HOSTOUT)/e_bot_%.o: $(SRC)/bot/%.cpp | patch
	@mkdir -p $(HOSTOUT)
	$(HOSTCXX) $(INCS) -c $< -o $@
$(HOSTOUT)/e_%.o: $(SRC)/%.cpp | patch
	@mkdir -p $(HOSTOUT)
	$(HOSTCXX) $(INCS) -c $< -o $@
$(HOSTOUT)/n_%.o: $(ENET)/%.c
	@mkdir -p $(HOSTOUT)
	clang -O1 -w -DHAS_FCNTL=1 -DHAS_SOCKLEN_T=1 -DHAS_POLL=1 -DHAS_GETNAMEINFO=1 -DHAS_GETADDRINFO=1 -DHAS_INET_PTON=1 -DHAS_INET_NTOP=1 -I$(ENET)/include -c $< -o $@
$(HOSTOUT)/softgl.o: softgl.cpp
	@mkdir -p $(HOSTOUT)
	$(HOSTCXX) -I$(HERE)/include -c $< -o $@
$(HOSTOUT)/sdl_radium.o: sdl_radium.cpp
	@mkdir -p $(HOSTOUT)
	$(HOSTCXX) $(INCS) -c $< -o $@
$(HOSTOUT)/acmods.o: acmods.cpp | patch
	@mkdir -p $(HOSTOUT)
	$(HOSTCXX) $(INCS) -I$(RT) -c $< -o $@
$(HOSTOUT)/al_stub.o: al_stub.cpp
	@mkdir -p $(HOSTOUT)
	$(HOSTCXX) $(INCS) -c $< -o $@
$(HOSTOUT)/launch.o: launch.cpp
	@mkdir -p $(HOSTOUT)
	$(HOSTCXX) -c $< -o $@
$(HOSTOUT)/host_api.o: host/host_api.cpp
	@mkdir -p $(HOSTOUT)
	$(HOSTCXX) -c $< -o $@
