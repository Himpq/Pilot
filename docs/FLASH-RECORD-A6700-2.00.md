# A6700 固件 2.00 实机刷写与验证记录

本记录对应 AP 底座加固改动（`patch/pilot_ap_flash.c`、`patch/on_camera.py`）的实机刷写结果与离线验证汇总：一次完整的"安装 → 真冷启动 → 回读校验 → 回滚 → 真冷启动 → 回读校验 → 复装"闭环，在真机 Sony ILCE-6700 固件 2.00 上共 8 次上电循环全部通过，零报错。

英文摘要见文末 [Summary](#summary)。

## 1. 测试对象与环境

| 项目 | 内容 |
|---|---|
| 相机 | Sony ILCE-6700，固件 Ver.2.00，未刷 CP 闪存补丁 |
| 日期 | 2026-11-27（UTC+08:00） |
| 电脑端 | Windows + Python 3.12.7，`pyusb 1.3.1`、`libusb-package 1.0.30.1`、`pycryptodomex 3.24.2` |
| USB 模式 | 大容量存储（`054C:0E76`），经 Jiritsu `cpload` 与 `pmca-re` 完成两轮服务模式认证 |
| 代码基线 | Pilot `main`（`b9a2df9`，v0.2.1 之后）+ 本 PR 的 AP 刷写器与宿主机加固改动 |
| 刷写器构建 | WSL Ubuntu 24.04 + `aarch64-linux-gnu-gcc (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0` |
| 相机侧暂存 | `/tmp/jiritsu/`（模式文件 `pilotflash.mode`、计划 `pilotflash-plan.bin`、结果 `pilotflash.result`） |

## 2. 材料哈希（SHA-256）

| 材料 | 大小（字节） | SHA-256 |
|---|---|---|
| 原厂 `/usr/lib/appFw.so` | 89,027,992 | `e65fd595052be5d0e98ce8000dfcb94f90a2f8309de70ef140375052009b0ef3` |
| 补丁后 `/usr/lib/appFw.so` | 89,027,992 | `2523b2ad10c9d17f8064be6735718e98d8278e2c85c0e74e1a5a1c4029fbd857` |
| `/usr/lib/pilot.so` | 51,456 | `ac6dcbc94570fbfe3d5fb7139074cfab1ec791b49c4b3069ba5c747bdc34e14c` |
| 实机刷入的刷写器 | 24,632 | `7521d94dc988412c1ae52d3771c78ff03758885f2f9c081979f74bd2eae156ed` |
| 本 PR 提交的 `patch/pilot_ap_flash.so` | 24,624 | `cbac58ec42eb61e5120089ab9956d2f84bde257c57e65b5f3bad0c9609144c1e` |
| `WBI2` 快照头备份（512 字节） | 512 | `4988d80398d594a5f0d62cd13a7a0f83e78c70d6c24e8ab3bc14c3d85b402ff3` |

两个刷写器二进制的差别只有嵌入的源文件名（构建时源码路径不同，位于非加载段）：`aarch64-linux-gnu-objcopy --only-section` 对比显示 `.text`、`.rodata`、`.got`、`.got.plt`、`.dynsym`、`.dynstr` 逐字节相同，`readelf -d` 动态段与 `readelf -l` 程序头也完全一致。提交版由 `bash patch/build_pilot_ap_flash.sh` 构建，产物通过 AArch64 架构、`(INIT)`、无 `NEEDED`、无 `TEXTREL` 和 64 KiB 传输上限检查。

## 3. 实机流程（8 次上电循环）

| # | 操作 | 关键返回 | 结果 |
|---|---|---|---|
| 1 | `flash check` | `appFw.so` 实测 89,027,992 B / `e65fd595…`，`state: baseline`，`persistent bootstrap: absent (clean stock)`，`on-camera WBI backup: absent` | 与离线基准逐字节一致，确认原厂未刷状态 |
| 2 | `flash writer-check` | `RESULT=OK`、`mode=check`、`state=baseline`、`window_mask=0x0`、`usr_statfs_stage=0`、`usr_magic=0xef53`、`usr_flags=0xc31`、`usr_free=0x1baf000`、`wbi_status=wbi2`、`wbi_hdr4=0x57424932` | 只读演练通过：`/usr` 实际只读（`ST_RDONLY\|ST_SYNCHRONOUS\|ST_VALID\|ST_NOATIME\|ST_NODIRATIME`），剩余空间 29,028,352 B（27.68 MiB）与离线 `dumpe2fs` 一致；`/dev/nflasha5` 可读且仍存有效 `WBI2` 快照头 |
| 3 | `flash install --yes` | `RESULT=OK`、`mode=write`、`state=patched`、`windows=4`、`barrier=2stage`、`fsync=0`、`readback=OK`、`wbi=cleared`、`remount_ro=0` | 一次写入成功；`appFw.so` → `2523b2ad…`，`pilot.so` 回读 51,456 B / `ac6dcbc9…`，原始 512 B `WBI2` 头已备份 |
| 4 | 拔出电池 ≥ 5 秒后无 SD 卡开机 | 实时取景、菜单、对焦与快门均正常 | 真冷启动未被旧 `WBI2` 快照遮蔽（[#10](https://github.com/Himpq/Pilot/issues/10) 场景） |
| 5 | 再次重启后 `flash check` | `appFw.so` = `2523b2ad…`、`state: patched`、`persistent bootstrap: present (51456 bytes sha256=ac6dcbc9…)`、`on-camera WBI backup: present (512 bytes)` | 补丁与常驻框架持久化确认 |
| 6 | `flash restore --yes` | `mode=restore`、`state=baseline`、`wbi=already_clear`、`remount_ro=0`，`appFw.so` → `e65fd595…` | 回滚一次成功 |
| 7 | 拔出电池 ≥ 5 秒冷启动 + `flash check` | 开机正常；`state: baseline`、`appFw.so` = `e65fd595…`、`persistent bootstrap: present (51456 bytes)`、`on-camera WBI backup: present (512 bytes)` | 回滚后的原厂状态经冷启动验证；按设计保留 `pilot.so` 与 WBI 备份 |
| 8 | `flash install --yes` 复装 | `RESULT=OK`、`mode=write`、`state=patched`、`windows=4`、`barrier=2stage`、`readback=OK`、`wbi=already_clear`、`remount_ro=0`、`persistent bootstrap: verified` | 阶段 0 闭环完成（装 → 冷启 → 验 → 滚 → 冷启 → 验 → 复装） |

## 4. 刷写器回执示例

最后一次安装的完整回执（`attempt_id = ca263d94-12cc-4de1-83a6-abd6fa420dc2`，`started_at_utc = 2026-11-27T05:44:19Z`）：

```text
A6700PILOTFLASH
RESULT=OK
mode=write
state=patched
windows=4
barrier=2stage
fsync=0
readback=OK
wbi=already_clear
remount_ro=0
```

配套状态记录：`baseline_sha256=e65fd595…`、`patched_sha256=2523b2ad…`、`before_sha256=e65fd595…`、`after_sha256=2523b2ad…`、`camera_installed_sha256=ac6dcbc9…`（51,456 B）、`camera_stage_readback_verified=true`、`activation=pending_full_power_cycle`。

字段含义：`barrier=2stage` 表示按窗口 1/2/3 落盘 + `fsync` 后再写窗口 0 + `fsync`（`restore` 反向）；`fsync=0` 与 `remount_ro=0` 是相机侧返回码 0；`readback=OK` 为写后回读逐字节一致；`wbi=cleared` / `already_clear` 表示 `/dev/nflasha5` 头 512 字节已清零 / 此前已清零。
每次操作写入独立的 `attempt_id` 回执并原子替换 `device-state.json`，因此表中第 3 次的 `wbi=cleared` 等字段来自当次运行记录，落盘文件保留的是最后一次（第 8 次）运行的结果。

## 5. 离线验证汇总

| 验证 | 命令 | 结果 |
|---|---|---|
| 刷写入口离线测试 | `python -m unittest discover -s patch/tests -p 'test_*.py'`（Python 3.12.7） | Ran 37 tests，OK：既有安装/恢复/预检流程 18 项 + 加固行为 19 项 |
| 刷写器故障注入 | `bash patch/tests/run_pilot_ap_flash_fault_tests.sh`（WSL Ubuntu 24.04，ASan + UBSan） | 16/16 通过 |
| 通用工具测试 | `python -m unittest discover -s tools/tests -p 'test_*.py'` | Ran 16 tests，OK（1 项 skip，与改动前基线一致） |
| 刷写器构建检查 | `bash patch/build_pilot_ap_flash.sh` | 24,624 B，架构 / `(INIT)` / 无 `NEEDED` / 无 `TEXTREL` / 小于 64 KiB 全部通过 |
| 补丁映射离线核验 | 用离线提取的 2.00 原厂 `appFw.so` 复算 | 原厂 + 四个窗口目标字节 → `2523b2ad…`，与实机写入后回读的哈希一致 |

故障注入覆盖的行为（无需相机与 QEMU）：`WBI2` 预检失败时 `appFw.so` 零写入；`WBI2` 清零发生在 `appFw.so` 写盘之前（事件序列 `W@256:32,W@512:8,W@1024:8,FSYNC,W@32:4,FSYNC`）；两阶段屏障中途 `pwrite` 失败时逐窗口回滚到运行前的确切字节；混合状态（窗口 0 原厂、窗口 1..3 补丁）可被 `install`/`restore` 收敛；`pilot.so` 写入失败保留原文件并清理临时文件；`purge` 在补丁态与非零 `WBI2` 下拒绝、在干净状态删除后回读确认 `-ENOENT`；非 AArch64 的 `pilot.so` 直接拒绝；结果文件写入严格限制在 512 字节内。

另有隔离 QEMU 端到端与整机菜单 M6 启动复验（工具包侧脚本，原始日志未随本 PR 提交）与本次实机结果一致。

## 6. 未在本轮实机覆盖的项

- `flash purge` / `flash full-uninstall`：仅由刷写器故障注入测试与宿主机离线测试覆盖（`reason=appfw_not_baseline`、`reason=wbi_not_clear`、`verify=enoent` 均为测试实证），实机 8 次循环未执行。
- `flash clean-foreign`：未在实机执行。
- CP 闪存补丁（`cp-install` / `cp-restore`）：本轮相机 CP 保持原厂状态，未刷写。
- 64 KiB 页对齐差异：由构建期检查拦截，未在真机复现（相机根文件系统为 4 KiB 页）。

## 7. 补丁映射数据修正

本 PR 同时修正 `patch.a6700.json` 中两处与实际原厂镜像不符的字段：

| 字段 | 原值 | 修正值 | 依据 |
|---|---|---|---|
| `segment.original_size` | `0x4489500` | `0x4488eed` | 2.00 版 `appFw.so`（`e65fd595…`）ELF 程序头 `p_filesz`/`p_memsz` 实际值 |
| `pt_load0_p_filesz.original_bytes` | `00954804…`（= `0x04489500`） | `ed8e4804…`（= `0x04488eed`） | 该文件偏移 `0x60` 处读回的 8 字节 |
| `pt_load0_p_memsz.original_bytes` | 同上 | 同上 | 该文件偏移 `0x68` 处读回的 8 字节 |

原值 `0x04489500` 是该 PT_LOAD 段的虚拟地址，被误当成段大小写入。影响：直接用公开的 `patch.a6700.json` 在原厂相机上执行 `flash check`，这两个窗口会报告"当前值既非原厂也非补丁值"而落到 `state=unknown`，预检也会把这两处列为差异；`flash prepare-ap` 会用实际读到的字节自愈，因此按 README 走预检流程的用户不受影响。本轮实机第 1 步是在生成相机专属配置后执行，故 `check` 直接给出 `baseline`。
补充：映射中新增 `target.binary_sha256` / `target.patched_binary_sha256` 两个字段，供宿主机在写入前后做整包 SHA-256 闸门（旧版配置缺少这两个字段时仍按窗口逐项校验，行为不变）。

## Summary

Target: stock Sony ILCE-6700 firmware 2.00, CP flash untouched. Host: Windows, Python 3.12.7 (`pyusb 1.3.1`, `libusb-package 1.0.30.1`, `pycryptodomex 3.24.2`); writer built with `aarch64-linux-gnu-gcc 13.3.0`.

Eight power cycles, all green: read-only `check` confirmed stock `/usr/lib/appFw.so` (89,027,992 bytes, `e65fd595…`); read-only `writer-check` confirmed `/usr` mounted read-only (`usr_flags=0xc31`), 27.68 MiB free and a live `WBI2` snapshot header (`wbi_hdr4=0x57424932`); `install` reported `RESULT=OK`, `state=patched`, `windows=4`, `barrier=2stage`, `readback=OK`, `wbi=cleared`, `remount_ro=0`, with `appFw.so` reaching `2523b2ad…` and `/usr/lib/pilot.so` reading back `ac6dcbc9…`; two battery-pull cold boots booted normally and persistence was re-verified; `restore` returned to `e65fd595…` and a third cold boot was normal; a reinstall passed again.

Offline validation: 37 host tests pass (`patch/tests`), 16 tool tests pass, and 16 ASan/UBSan fault-injection tests pass (`patch/tests/run_pilot_ap_flash_fault_tests.sh`), proving WBI2 clearing precedes any `appFw.so` write, the two-stage fsync barrier, exact per-window rollback from a mid-write failure, mixed-state convergence, atomic `pilot.so` replacement, purge guards (`-ENOENT` verified) and the bounded 512-byte result file. The shipped `patch/pilot_ap_flash.so` was rebuilt with the committed script; its `.text`, `.rodata`, `.got`, `.dynsym` and `.dynstr` sections, dynamic section and program headers are byte-identical to the binary flashed on this camera, which differs only by the embedded source file name.

Not exercised on hardware this round: `purge`, `full-uninstall`, `clean-foreign` (covered by fault-injection and host tests only) and the CP flash patch.

[返回首页](../README.md)
