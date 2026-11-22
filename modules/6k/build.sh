#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
MOD="$ROOT/modules/6k"
SYSROOT=${PILOT_A6700_SYSROOT:?set PILOT_A6700_SYSROOT to a target sysroot}
CC=${AARCH64_CC:-aarch64-linux-gnu-gcc-11}
OUT=${1:-$ROOT/build/pilot_6k.so}
TMP=$(mktemp -d /tmp/pilot-6k-build.XXXXXX)
trap 'rm -rf "$TMP"' EXIT

COMMON=(--sysroot="$SYSROOT" -std=c11 -O2 -march=armv8-a -mno-outline-atomics -fno-stack-protector -fPIC -fvisibility=hidden -Wall -Wextra -Werror)
ENCODER=(-DPILOT_HS6K30_CUSTOM_W=5760 -DPILOT_HS6K30_CUSTOM_H=3240 -DPILOT_HS6K30_KEEP_VQ=1 -DPILOT_HS6K30_USE_DDR_CD=1)
LINK=(--sysroot="$SYSROOT" -shared -nostdlib -Wl,--no-undefined -Wl,--no-as-needed -Wl,--build-id=none -L"$SYSROOT/lib")
LIBS=(-l:libpthread.so.0 -l:libc.so.6 -lgcc)

# First reproduce the two installed modules exactly, before merging their entry points.
"$CC" "${COMMON[@]}" "${ENCODER[@]}" -c "$MOD/pilot_hs6k30_all_v1.c" -o "$TMP/encoder.o"
"$CC" "${LINK[@]}" -Wl,-soname,pilot_hs6k30_all_v1.so -o "$TMP/encoder.so" "$TMP/encoder.o" "${LIBS[@]}"
"$CC" "${COMMON[@]}" -c "$MOD/pilot_cp_6k_og.c" -o "$TMP/buffers.o"
"$CC" "${LINK[@]}" -Wl,-soname,pilot_cp_6k_og.so -o "$TMP/buffers.so" "$TMP/buffers.o" "${LIBS[@]}"
python3 - "$MOD" "$TMP" <<'PY'
import hashlib,json,sys
from pathlib import Path
m,t=map(Path,sys.argv[1:]); b=json.loads((m/'baseline.json').read_text())
for name,digest in b['source_sha256'].items():
    assert hashlib.sha256((m/name).read_bytes()).hexdigest()==digest, name
for name,key in [('encoder.so','encoder_binary_sha256'),('buffers.so','buffers_binary_sha256')]:
    actual=hashlib.sha256((t/name).read_bytes()).hexdigest()
    assert actual==b[key], (name,actual,b[key])
print('BASELINES_REPRODUCED encoder=fe5609fd buffers=cfeac73b')
PY

"$CC" "${COMMON[@]}" "${ENCODER[@]}" -Dpilot_module_init=pilot_6k_encoder_init -c "$MOD/pilot_hs6k30_all_v1.c" -o "$TMP/encoder.o"
"$CC" "${COMMON[@]}" -Dpilot_module_init=pilot_6k_buffers_init -c "$MOD/pilot_cp_6k_og.c" -o "$TMP/buffers.o"
"$CC" "${COMMON[@]}" -c "$MOD/pilot_6k.c" -o "$TMP/entry.o"
mkdir -p "$(dirname "$OUT")"
"$CC" "${LINK[@]}" -Wl,-soname,pilot_6k.so -Wl,--version-script="$MOD/exports.map" -o "$OUT" "$TMP/entry.o" "$TMP/encoder.o" "$TMP/buffers.o" "${LIBS[@]}"
readelf -h "$OUT" | grep -q AArch64
if readelf -d "$OUT" | grep -q TEXTREL; then exit 1; fi
sha256sum "$OUT"
stat -c 'size_bytes=%s' "$OUT"
