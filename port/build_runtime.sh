#!/bin/bash
# Builds the C/C++ runtime that RadiumOS "apps" (src/commands/apprun.c) link against:
#   musl (patched so every syscall goes to __radium_syscall), compiler-rt builtins, libc++, libc++abi, libunwind.
# Sources: /tmp/cxx/musl-1.2.5 and /tmp/cxx/llvm (llvm-project 18.1.3: libcxx libcxxabi libunwind runtimes cmake
# compiler-rt llvm/cmake third-party). Output: $WORK/sysroot (usr/include, usr/lib) and $WORK/lib/libbuiltins.a.
set -e
WORK=${WORK:-/tmp/cxx}
HERE=$(cd "$(dirname "$0")" && pwd)
SYSROOT=$WORK/sysroot
TRIPLE=i686-linux-musl
CMAKE=${CMAKE:-/tmp/ytv/bin/cmake}
CF="--target=$TRIPLE -march=i686 -mno-sse -mno-mmx -O2 -fno-pic -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables -funwind-tables"

stage_musl() {
    rm -rf "$WORK/musl-build" && cp -r "$WORK/musl-1.2.5" "$WORK/musl-build"
    cd "$WORK/musl-build"
    cp "$HERE/musl/syscall_arch.h" arch/i386/syscall_arch.h
    rm -f src/thread/i386/clone.s src/thread/i386/__set_thread_area.s src/thread/i386/syscall_cp.s src/thread/i386/__unmapself.s
    CC=clang AR=/usr/lib/llvm-18/bin/llvm-ar RANLIB=/usr/lib/llvm-18/bin/llvm-ranlib CFLAGS="$CF" ./configure --target=i386 --prefix=/usr --disable-shared --enable-static >/dev/null
    make -j16 >/dev/null
    make DESTDIR="$SYSROOT" install >/dev/null
    mkdir -p "$SYSROOT/usr/include/linux" && cp "$HERE/musl/linux/futex.h" "$SYSROOT/usr/include/linux/"
    echo "musl: done"
}

stage_builtins() {
    rm -rf "$WORK/build-builtins"
    $CMAKE -S "$WORK/llvm/compiler-rt/lib/builtins" -B "$WORK/build-builtins" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_ASM_COMPILER=clang \
        -DCMAKE_C_COMPILER_TARGET=$TRIPLE -DCMAKE_ASM_COMPILER_TARGET=$TRIPLE \
        -DCMAKE_C_FLAGS="$CF --sysroot=$SYSROOT" -DCMAKE_ASM_FLAGS="$CF --sysroot=$SYSROOT" -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
        -DCOMPILER_RT_DEFAULT_TARGET_ONLY=ON -DCOMPILER_RT_BUILD_BUILTINS=ON >/dev/null
    ninja -C "$WORK/build-builtins" >/dev/null
    mkdir -p "$WORK/lib"
    cp "$(find "$WORK/build-builtins" -name 'libclang_rt.builtins*.a' | head -1)" "$WORK/lib/libbuiltins.a"
    echo "builtins: done"
}

stage_cxx() {
    rm -rf "$WORK/build-rt"
    $CMAKE -S "$WORK/llvm/runtimes" -B "$WORK/build-rt" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_ASM_COMPILER=clang \
        -DCMAKE_C_COMPILER_TARGET=$TRIPLE -DCMAKE_CXX_COMPILER_TARGET=$TRIPLE -DCMAKE_ASM_COMPILER_TARGET=$TRIPLE \
        -DCMAKE_ASM_FLAGS="$CF --sysroot=$SYSROOT" \
        -DCMAKE_C_FLAGS="$CF --sysroot=$SYSROOT" -DCMAKE_CXX_FLAGS="$CF --sysroot=$SYSROOT" \
        -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY -DCMAKE_INSTALL_PREFIX="$SYSROOT/usr" \
        -DLLVM_ENABLE_RUNTIMES="libcxx;libcxxabi;libunwind" \
        -DLIBCXX_HAS_MUSL_LIBC=ON -DLIBCXX_ENABLE_SHARED=OFF -DLIBCXXABI_ENABLE_SHARED=OFF -DLIBUNWIND_ENABLE_SHARED=OFF \
        -DLIBCXX_ENABLE_THREADS=ON -DLIBCXXABI_ENABLE_THREADS=ON -DLIBUNWIND_ENABLE_THREADS=ON \
        -DLIBCXX_ENABLE_EXCEPTIONS=ON -DLIBCXXABI_ENABLE_EXCEPTIONS=ON \
        -DLIBCXXABI_USE_LLVM_UNWINDER=ON -DLIBCXXABI_STATICALLY_LINK_UNWINDER_IN_STATIC_LIBRARY=ON \
        -DLIBCXX_STATICALLY_LINK_ABI_IN_STATIC_LIBRARY=ON -DLIBCXX_USE_COMPILER_RT=ON -DLIBCXXABI_USE_COMPILER_RT=ON \
        -DLIBCXX_INCLUDE_TESTS=OFF -DLIBCXX_INCLUDE_BENCHMARKS=OFF -DLIBCXXABI_INCLUDE_TESTS=OFF -DLIBUNWIND_INCLUDE_TESTS=OFF \
        -DLIBCXX_ENABLE_ASSERTIONS=OFF -DLIBCXXABI_ENABLE_ASSERTIONS=OFF -DLIBUNWIND_ENABLE_ASSERTIONS=OFF \
        -DLIBCXX_ENABLE_LOCALIZATION=ON -DLIBCXX_ENABLE_FILESYSTEM=ON -DLIBCXX_ENABLE_RANDOM_DEVICE=ON \
        -DLIBCXX_ENABLE_UNICODE=ON -DLIBCXX_ENABLE_WIDE_CHARACTERS=ON \
        -DLIBCXX_CXX_ABI=libcxxabi -DLIBCXX_INSTALL_LIBRARY_DIR="$SYSROOT/usr/lib" >/dev/null
    ninja -C "$WORK/build-rt" install-cxx install-cxxabi install-unwind >/dev/null
    echo "libc++: done"
}

for s in "${@:-musl builtins cxx}"; do stage_$s; done
