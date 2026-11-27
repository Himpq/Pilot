# 更新记录

## 未发布

AP 底座刷写加固（保留现有命令与流程，新增只读演练与彻底卸载入口）：

- 刷写器先备份并清零 `/dev/nflasha5` 头 512 字节 `WBI2` 快照头，之后才修改 `appFw.so`；清零失败时 `appFw.so` 一字未改直接返回，不再出现"补丁已写入但冷启动仍被旧快照遮蔽"的中间态（[#10](https://github.com/Himpq/Pilot/issues/10) 同类故障）。`restore` 同样先清 `WBI2` 再按反向顺序写回原厂窗口。
- `appFw.so` 四个窗口改为两阶段 `fsync` 屏障（先写窗口 1/2/3 并落盘，再写窗口 0）；中途写入失败按窗口回滚到运行前的确切字节。窗口呈混合状态时 `check` 报 `state=mixed` 并逐窗口列出当前值，`install` 与 `restore` 可直接收敛，无需 `--force`。
- `/usr/lib/pilot.so` 改为临时文件 + `fsync` + 回读 + `renameat` + 目录 `fsync` 原子替换，避免原地 `O_TRUNC` 写入被打断后留下残缺 ELF 头，导致开机加载时 `SIGBUS`。
- `patch.a6700.json` 修正与实际原厂镜像不符的字段：`segment.original_size` 及 `pt_load0_p_filesz` / `pt_load0_p_memsz` 的原厂值由 `0x04489500` 改为 2.00 实测的 `0x04488eed`；新增 `target.binary_sha256` 与 `target.patched_binary_sha256`，宿主机在写入前后增加 89 MB 整包 SHA-256 闸门（旧配置缺少这两个字段时仍按窗口逐项校验）。
- 新增 `flash writer-check`（只读演练，回报 `window_mask`、`/usr` 挂载参数与剩余空间、`wbi_status` 与 `wbi_hdr4`）、`flash purge`（仅在基线且 `WBI2` 已清零时删除 `pilot.so` 与机内备份，并回读确认 `-ENOENT`）、`flash full-uninstall`（同会话 restore + 回读校验 + purge）与 `flash clean-foreign`。
- `--force` 语义收紧：只放宽"非窗口字节与配置不一致"，窗口内出现无法识别的字节即使命令行 `--force` 也拒绝写入。
- 刷写器结果按 `A6700PILOTFLASH` 之后 512 字节内的 `key=value` 严格解析，避免把 `remount_ro` 失败误判为可以安全重启；首次回读与预期不符时在同一会话内再读一次以排除 USB 抖动。
- 每次操作写入独立 `attempt_id` 回执，不沿用上一次运行的成功字段；`WBI2` 备份改为原子落盘，`restore-wbi` 仅在设备端头部为全零时写回，避免覆盖更新的快照头。
- `patch/cpload_run.py` 在 Windows 下按已安装的 `libusb_package` 定位 `libusb-1.0.dll`（兼容 venv 与 conda 布局），设备去重改用总线加地址，避免不同总线上的相同地址被误认为同一设备。
- `patch/build_pilot_ap_flash.sh` 固定 4096 字节页大小并拒绝 `TEXTREL`、`NEEDED` 与超过 64 KiB Jiritsu 传输上限的产物；`aarch64-linux-gnu-gcc-11` 缺失时回退到 `aarch64-linux-gnu-gcc` 并提示。
- 新增测试：刷写器宿主机故障注入 16 项（`bash patch/tests/run_pilot_ap_flash_fault_tests.sh`，AddressSanitizer + UndefinedBehaviorSanitizer）、刷写入口加固离线测试 19 项（`patch/tests/test_on_camera_hardening.py`）。
- 实机记录：Sony ILCE-6700 固件 2.00 共 8 次上电循环完成安装、拔电池真冷启动、回读校验、回滚与复装，全部通过，见 [A6700 2.00 实机刷写与验证记录](docs/FLASH-RECORD-A6700-2.00.md)。

## v0.2.1（2026-11-26）

- 修复原厂未清过 WBI 快照的 A6700 刷入 AP 补丁后冷重启被旧 WBI Page Cache 遮蔽的问题（[#10](https://github.com/Himpq/Pilot/issues/10)）。
- `flash install` 写入并校验 `appFw.so` 后，自动将 `/dev/nflasha5` 头 512 字节 `WBI2` 快照头双份备份（机内 `/usr/lib/pilot_wbi_hdr.bak` + 电脑端 `<state-dir>/wbi_hdr.bak`）并清零，保证冷启动直接加载最新补丁。
- 新增 `flash clear-wbi --yes`（用于已刷写但被旧 WBI 遮蔽的设备一键清头生效，不改动 `appFw.so`）与 `flash restore-wbi --yes`（从机内备份原样恢复 `WBI2` 快照头）。

## v0.2.0（2026-11-24）

- 支持运行时修改 CP 代码区。
- 优化 CP 内存读写和框架。

## v0.1.1（2026-11-22）

- 每次启动的 SD 日志首行前增加空行。
- 记录清单模块总数、模块文件名和加载顺序。
- 模块暂存、动态加载、API 绑定、数据目录及初始化失败记录具体模块和状态。
- 初始化成功记录模块文件名和位置。
- 增加 QEMU 启动检查，验证启动分隔和模块日志。
- 增加通用补丁 CLI：描述校验、离线镜像修改、RAM 模块构建、SD 模块部署和日志导出。
- 刷写入口支持外部本机配置，写入前备份，设备状态与补丁描述分开保存。
- 实机更新 pilot.so 并重启后，启动分隔与 7 个模块的具名加载记录正常。
