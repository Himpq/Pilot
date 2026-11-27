#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cc=${PILOT_CC:-aarch64-linux-gnu-gcc-11}
readelf_bin=${PILOT_READELF:-$(command -v aarch64-linux-gnu-readelf || command -v readelf)}
out=${PILOT_FLASH_OUT:-$root/patch/pilot_ap_flash.so}

# Fall back to the unpinned cross compiler when gcc-11 is not installed on this host.
if ! command -v "$cc" >/dev/null 2>&1 && [ -z "${PILOT_CC:-}" ]; then
    if command -v aarch64-linux-gnu-gcc >/dev/null 2>&1; then
        echo "NOTE: aarch64-linux-gnu-gcc-11 not found; building with aarch64-linux-gnu-gcc" >&2
        cc=aarch64-linux-gnu-gcc
    fi
fi
"$cc" --version | head -1 >&2

# The camera loads this module through the Jiritsu dlopen path on a 4 KiB-page rootfs, and the
# stock loader performs no text relocations, so pin the page size explicitly (some cross gcc
# builds default to 64 KiB segments) and reject TEXTREL, NEEDED and oversized modules here
# instead of after a brick. max-page-size=4096 keeps the ELF loadable on the camera.
"$cc" -std=c11 -Os -march=armv8-a -fPIC -fvisibility=hidden \
    -fno-stack-protector -fno-builtin -Wall -Wextra -Werror \
    -shared -nostdlib -Wl,-z,max-page-size=4096 -Wl,-z,common-page-size=4096 \
    -Wl,-init,_init -Wl,--no-undefined -Wl,--build-id=none \
    -o "$out" "$root/patch/pilot_ap_flash.c"
"$readelf_bin" -h "$out" | grep -q AArch64
"$readelf_bin" -d "$out" | grep -q '(INIT)'
if "$readelf_bin" -d "$out" | grep -q '(NEEDED)'; then
    echo "unexpected dynamic dependency: $out" >&2
    exit 1
fi
if "$readelf_bin" -d "$out" | grep -q 'TEXTREL'; then
    echo "unexpected TEXTREL in $out (the camera loader cannot relocate text)" >&2
    exit 1
fi
size_bytes=$(stat -c '%s' "$out")
if [ "$size_bytes" -gt 65536 ]; then
    echo "ERROR: $out exceeds the 64 KiB Jiritsu transfer limit ($size_bytes bytes)" >&2
    exit 1
fi
echo "built $out ($size_bytes bytes)"
sha256sum "$out"
