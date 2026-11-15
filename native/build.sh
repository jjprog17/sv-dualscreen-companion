#!/bin/sh
# Builds the Scarlet and Violet modules for Linux x86_64 (desktop eden-cli) and Android arm64
# (the AYN Thor). Freestanding: no libc, no NDK, only clang + lld. The Oodle decoder is the
# vendored ooz (GPL-3.0), compiled against the small shims in shim/.
# Usage: ./build.sh [outdir]   (EDEN_ABI_DIR overrides native/include)
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ABI=${EDEN_ABI_DIR:-$HERE/include}
OUT=${1:-$HERE/out}
COMMON="-O2 -fPIC -ffreestanding -nostdlib -fno-stack-protector -fvisibility=hidden -fno-asynchronous-unwind-tables"
CFLAGS="-std=c11 $COMMON -Wall -Wextra -Werror -I$ABI"
CXXFLAGS="-std=c++17 $COMMON -fno-exceptions -fno-rtti -fno-strict-aliasing -fno-vectorize -fno-slp-vectorize -w -I$HERE/shim"
LDFLAGS="-shared -fuse-ld=lld -Wl,--version-script=$HERE/exports.map -Wl,--no-undefined \
 -Wl,-z,max-page-size=16384 -Wl,--hash-style=both -Wl,--build-id=sha1"
for plat in linux-x86_64:x86_64-linux-gnu android-arm64-v8a:aarch64-linux-android24; do
  p=${plat%%:*}; triple=${plat#*:}
  case $triple in aarch64*) ATOM=-mno-outline-atomics;; *) ATOM=;; esac
  OBJ="$OUT/obj/$p"; mkdir -p "$OBJ" "$OUT/$p"
  for f in kraken lzna bitknit oozlib; do
    clang++ --target=$triple $CXXFLAGS -c "$HERE/third_party/ooz/$f.cpp" -o "$OBJ/ooz_$f.o"
  done
  clang++ --target=$triple $CXXFLAGS -I$HERE -c "$HERE/third_party/bc_decoder/bc_decoder.cpp" -o "$OBJ/bc_decoder.o"
  clang++ --target=$triple $CXXFLAGS -I$HERE -c "$HERE/bc_wrap.cpp" -o "$OBJ/bc_wrap.o"
  clang --target=$triple $CFLAGS -c "$HERE/sv_alloc.c" -o "$OBJ/sv_alloc.o"
  clang --target=$triple $CFLAGS -c "$HERE/sv_game.c" -o "$OBJ/sv_game.o"
  for game in scarlet:0100A3D008C5C000 violet:01008F6008C5E000; do
    tid=${game#*:}
    clang --target=$triple $CFLAGS $ATOM -DSV_TITLE_ID=0x${tid}ULL -c "$HERE/sv_module.c" -o "$OBJ/sv_module_$tid.o"
    clang++ --target=$triple $COMMON $LDFLAGS -Wl,-soname,$tid.so -o "$OUT/$p/$tid.so" \
      "$OBJ/sv_module_$tid.o" "$OBJ/sv_game.o" "$OBJ/sv_alloc.o" "$OBJ"/ooz_*.o "$OBJ/bc_decoder.o" "$OBJ/bc_wrap.o"
    llvm-strip --strip-all "$OUT/$p/$tid.so"
  done
done
ls -l "$OUT"/linux-x86_64/ "$OUT"/android-arm64-v8a/
