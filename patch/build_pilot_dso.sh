#!/usr/bin/env bash
set -euo pipefail

pilot_root=$(cd "$(dirname "$0")/.." && pwd)
pilot_sysroot=${PILOT_SYSROOT:?set PILOT_SYSROOT to a target sysroot}
pilot_cc=${PILOT_CC:-aarch64-linux-gnu-gcc-11}
pilot_output=${PILOT_OUT:-$pilot_root/build/pilot.so}
pilot_build=$(mktemp -d /tmp/pilot-dso-build.XXXXXX)
trap 'rm -rf "$pilot_build"' EXIT

for pilot_input in \
    "$pilot_root/patch/pilot_entry.c" \
    "$pilot_root/patch/pilot_fsys.c" \
    "$pilot_root/patch/pilot_time.c" \
    "$pilot_root/patch/pilot_api.c" \
    "$pilot_root/patch/pilot_memory.c" \
    "$pilot_root/patch/pilot_module_dir.c" \
    "$pilot_root/patch/pilot_modules.c"; do
    pilot_object="$pilot_build/$(basename "${pilot_input%.c}").o"
    "$pilot_cc" --sysroot="$pilot_sysroot" \
        -std=c11 -O2 -march=armv8-a -mno-outline-atomics \
        -fno-stack-protector -fPIC -fvisibility=hidden \
        -Wall -Wextra -Werror -c "$pilot_input" -o "$pilot_object"
done

mkdir -p "$(dirname "$pilot_output")"
"$pilot_cc" --sysroot="$pilot_sysroot" -shared -nostdlib \
    -Wl,-soname,pilot.so -Wl,--no-undefined -Wl,--no-as-needed \
    -L"$pilot_sysroot/lib" -o "$pilot_output" "$pilot_build"/*.o \
    -lgcc -l:libdl.so.2 -l:libpthread.so.0 -l:libc.so.6

sha256sum "$pilot_output"
stat -c 'size_bytes=%s' "$pilot_output"
