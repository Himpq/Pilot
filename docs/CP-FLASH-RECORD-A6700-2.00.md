# ILCE-6700 2.00 CP UIPC v4 flash record

On 2026-11-28 (UTC+08:00), one Sony ILCE-6700 running firmware 2.00 completed a stock-to-patched CP install over USB using the hardened writer from the Antigravity work. After disconnecting USB, switching off, and removing the battery for at least 10 seconds, the camera booted without an SD card. Live view, focus, and menus responded normally. A fresh USB service session then read back the same complete patched image.

## Device evidence

The bundled `patch/pilot_cp_flash.so` is the exact 15,408-byte artifact used for the device run. Its SHA-256 is `65802b98ec1efdbffdc60aea27eebb1da9e37704e77d5a667ae590cd955193cc`. The device run used the original standalone host controller. This PR routes that flow through Pilot's existing CLI.

| Step | Recorded result |
| --- | --- |
| Initial full USB read | 17,892,024 bytes, mask `0x0`, stock SHA-256 `61b7fe5c0ce227ea865d799b81fe77ae359b7c5dcd39d13c9c2241d9cad23559` |
| On-device writer check | `RESULT=OK`, `mode=check`, `state=baseline`, `cp_statfs_rc=0`, `cp_magic=0x4d44`, `cp_ro=0`, `wbi_status=clear` |
| Backup | Full 17,892,024-byte original image written locally and reread against the stock SHA-256 before the CP write |
| Install | `RESULT=OK`, `mode=apply`, `before_mask=0x0`, `window_mask=0x3`, `barrier=2stage`, `written=572`, `readback=OK`, `dir_sync=0`, `wbi_status=clear` |
| Full USB read after install | Patched SHA-256 `a103a5dc256a7bdba44c3e61f42efefc3865c8729d7cd649758993618fab14e9` |
| Full USB read after battery-out cold boot | Same 17,892,024-byte patched image and SHA-256, mask `0x3`, both windows patched, staging files clean |

The device evidence covers installation and persistence on this camera and firmware build. Restore and same-session emergency repair were exercised offline. Recovery when the camera cannot boot into USB service mode remains unverified. The record covers the stock Pilot UIPC v4 patch.

## Offline checks for this port

Run from the repository root:

```sh
python -m unittest discover -s patch/tests -p 'test_*.py'
python -m unittest discover -s tools/tests -p 'test_*.py'
```

The writer was rebuilt with AArch64 GCC 13.3.0 on Ubuntu 24.04 and compared byte-for-byte with the bundled device-tested artifact. To rebuild and run the C fault-injection tests on Linux or WSL:

```sh
bash patch/build_pilot_cp_flash.sh
mkdir -p build
cc -std=c11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -fno-omit-frame-pointer -o build/test_cp_flash_host patch/tests/test_cp_flash_host.c
TMPDIR="$PWD/build" build/test_cp_flash_host
```

The earlier installer validation also ran the writer against a private copy of the original FAT12 CP partition under QEMU with Linux 4.9.337-scel+. The original firmware and partition images remain local.
