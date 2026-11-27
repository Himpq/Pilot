#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
MODULE_DIR="$ROOT/modules/pilot_canvas"
BUILD_DIR="$ROOT/build"
SYSROOT="${PILOT_A6700_SYSROOT:-/home/cameraemu/.cache/a6700-kernel-20230712/a6700-original-boot/pilot-merged-root-test}"
CC="${AARCH64_CC:-aarch64-linux-gnu-gcc-11}"
OUT="${1:-$BUILD_DIR/pilot_canvas.so}"
TMP="$(mktemp -d /tmp/pilot-canvas.XXXXXX)"
trap 'rm -rf "$TMP"' EXIT

SRCS=(
    pilot_canvas.c
    canvas_core.c
    canvas_text.c
    canvas_panel.c
    canvas_cp_sensors.c
    canvas_scope_widget.c
    canvas_focal_widget.c
    canvas_status_widget.c
)

mkdir -p "$(dirname "$OUT")"
OBJS=()
for src in "${SRCS[@]}"; do
    obj="$TMP/${src%.c}.o"
    "$CC" --sysroot="$SYSROOT" -std=c11 -O2 -march=armv8-a -mno-outline-atomics \
        -fno-stack-protector -fPIC -fvisibility=hidden -Wall -Wextra -Werror \
        -I "$ROOT/patch" -I "$ROOT/modules/menu_ui" -I "$MODULE_DIR" \
        -c "$MODULE_DIR/$src" -o "$obj"
    OBJS+=("$obj")
done

"$CC" --sysroot="$SYSROOT" -shared -nostdlib -Wl,-soname,pilot_canvas.so \
    -Wl,--no-undefined -Wl,--no-as-needed -Wl,--build-id=none \
    -L"$SYSROOT/lib" -o "$OUT" "${OBJS[@]}" \
    -l:libpthread.so.0 -l:libdl.so.2 -l:libc.so.6 -lgcc

grep -q 'pilot_canvas_probe_v1' <<<"$(readelf -sW "$OUT")"
sha256sum "$OUT"
stat -c '%n %s bytes' "$OUT"
