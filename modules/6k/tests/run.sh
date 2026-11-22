#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
MOD="$ROOT/modules/6k"
SYSROOT=${PILOT_A6700_SYSROOT:?set PILOT_A6700_SYSROOT to a target sysroot}
TMP=$(mktemp -d /tmp/pilot-6k-test.XXXXXX)
trap 'rm -rf "$TMP"' EXIT
cc -std=c11 -O2 -Wall -Wextra -Werror "$MOD/pilot_6k.c" "$MOD/tests/test_entry.c" -lpthread -o "$TMP/entry"
for scenario in ok encoder_fail buffers_fail; do "$TMP/entry" "$scenario"; done
aarch64-linux-gnu-gcc-11 --sysroot="$SYSROOT" -std=c11 -O2 -march=armv8-a -mno-outline-atomics -fno-stack-protector -Wall -Wextra -Werror \
    -Wl,--export-dynamic -Wl,--no-as-needed -Wl,--allow-shlib-undefined -Wl,-dynamic-linker,/lib/ld-linux-aarch64.so.1 -Wl,-rpath-link,"$SYSROOT/lib" -L"$SYSROOT/lib" \
    -o "$TMP/dso" "$MOD/tests/test_dso.c" -l:libdl.so.2 -l:libpthread.so.0 -l:libc.so.6 -static-libgcc
qemu-aarch64 -L "$SYSROOT" "$TMP/dso" "$ROOT/build/pilot_6k.so"
