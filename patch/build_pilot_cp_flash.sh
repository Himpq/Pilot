#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "$0")" && pwd)"
cc="${PILOT_CC:-aarch64-linux-gnu-gcc}"
readelf_bin="${PILOT_READELF:-$(command -v aarch64-linux-gnu-readelf || command -v readelf)}"
out="${PILOT_CP_FLASH_OUT:-$script_dir/pilot_cp_flash.so}"

mkdir -p "$(dirname "$out")"

"$cc" -std=c11 -Os -march=armv8-a -fPIC -fvisibility=hidden \
    -fno-stack-protector -fno-builtin -Wall -Wextra -Werror \
    -shared -nostdlib -Wl,-z,max-page-size=4096 -Wl,-z,common-page-size=4096 \
    -Wl,-init,_init -Wl,--no-undefined -Wl,--build-id=none \
    -o "$out" "$script_dir/pilot_cp_flash_hardened.c"

"$readelf_bin" -h "$out" | grep -q 'AArch64'
"$readelf_bin" -d "$out" | grep -q '(INIT)'
if "$readelf_bin" -d "$out" | grep -q '(NEEDED)'; then
    echo "ERROR: unexpected dynamic dependency in $out" >&2
    exit 1
fi
if "$readelf_bin" -d "$out" | grep -q 'TEXTREL'; then
    echo "ERROR: unexpected TEXTREL in $out" >&2
    exit 1
fi
if ! grep -aq "pilot_cp_hardened_v2" "$out"; then
    echo "ERROR: missing pilot_cp_hardened_v2 marker in $out" >&2
    exit 1
fi
size_bytes=$(stat -c '%s' "$out")
if [ "$size_bytes" -gt 65536 ]; then
    echo "ERROR: $out exceeds 64 KiB Jiritsu transfer limit ($size_bytes bytes)" >&2
    exit 1
fi
echo "Built $out ($size_bytes bytes)"
sha256sum "$out"
