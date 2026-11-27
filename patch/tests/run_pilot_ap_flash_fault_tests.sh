#!/usr/bin/env bash
# Host fault-injection suite for the on-camera AP writer (Linux/WSL, native arch).
#
#   bash patch/tests/run_pilot_ap_flash_fault_tests.sh
#
# The writer is built for AArch64, but pilot_ap_flash.c compiles natively with
# -DPILOT_FLASH_HOST_TEST, which swaps the raw AArch64 syscalls for libc wrappers and
# exposes pilot_flash_run_once() so the suite can drive one mode file per test.  Every
# path the writer opens is redirected into a fresh mktemp fixture through generated
# -D overrides, so this never touches /usr/lib, /dev/nflasha5 or /tmp/jiritsu, and the
# suite injects pwrite()/fsync()/open() failures to check the two-stage barrier order,
# the atomic pilot.so replacement, WBI2 rollback and the bounded 512-byte result file.
#
# AddressSanitizer + UndefinedBehaviorSanitizer are used when the toolchain provides
# them; set PILOT_CC to choose a compiler.
set -euo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)
cc=${PILOT_CC:-cc}
base=$(mktemp -d /tmp/pilot-ap-flash-fault.XXXXXX)
trap 'rm -rf "$base"' EXIT
mkdir "$base/fixture"

flags=(-std=c11 -g -O1 -Wall -Wextra -Werror -fno-omit-frame-pointer
       -DPILOT_FLASH_HOST_TEST -DPILOT_FLASH_TEST)
probe="$base/probe.c"
printf 'int main(void){return 0;}\n' > "$probe"
if "$cc" "${flags[@]}" -fsanitize=address,undefined -o "$base/probe" "$probe" 2>/dev/null; then
    flags+=(-fsanitize=address,undefined)
    echo "sanitizers: address,undefined"
else
    echo "NOTE: building without sanitizers ($cc lacks -fsanitize=address,undefined)" >&2
fi

# PILOT_USR_LIB_DIR is the prefix for /usr/lib paths; the fixture stands in for both
# /usr/lib and /dev/nflasha5, and the staging paths replace /tmp/jiritsu.
"$cc" "${flags[@]}" \
    -DPILOT_USR_LIB_DIR="\"$base/fixture\"" \
    -DPILOT_BOOTSTRAP_PATH="\"$base/fixture/pilot.so\"" \
    -DPILOT_BOOTSTRAP_TMP_PATH="\"$base/fixture/.pilot.so.tmp\"" \
    -DPILOT_WBI_DEV_PATH="\"$base/fixture/nflasha5\"" \
    -DPILOT_WBI_BACKUP_PATH="\"$base/fixture/pilot_wbi_hdr.bak\"" \
    -DPILOT_WBI_BACKUP_TMP_PATH="\"$base/fixture/.pilot_wbi_hdr.bak.tmp\"" \
    -DPILOT_WBI_TMP_BACKUP_PATH="\"$base/fixture/pilotflash.wbi.bak\"" \
    -DPILOT_WBI_TMP_BACKUP_TMP_PATH="\"$base/fixture/.pilotflash.wbi.bak.tmp\"" \
    -DPILOT_FLASH_TEST_APPFW_PATH="\"$base/fixture/appFw.so\"" \
    -DPILOT_FLASH_MODE_PATH="\"$base/fixture/pilotflash.mode\"" \
    -DPILOT_FLASH_RESULT_PATH="\"$base/fixture/pilotflash.result\"" \
    -DPILOT_FLASH_RESULT_TMP_PATH="\"$base/fixture/.pilotflash.result.tmp\"" \
    -DPILOT_FLASH_PLAN_PATH="\"$base/fixture/pilotflash-plan.bin\"" \
    -DPILOT_FLASH_PILOT_PATH="\"$base/fixture/pilotflash-pilot.so\"" \
    "$root/patch/tests/test_pilot_ap_flash_fault_injection.c" \
    "$root/patch/pilot_ap_flash.c" \
    -o "$base/suite"
"$base/suite"
