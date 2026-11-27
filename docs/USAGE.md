# 使用方式

框架发布下载：[Releases](https://github.com/Himpq/Pilot/releases)。适用 Sony ILCE-6700 固件 2.00，模块 ABI 为 2。电脑端工具需要 Python 3.10 或更新版本。

## 1. 准备文件

从 Releases 下载所需版本的 `pilot.so`。
下载源码包或克隆仓库，在仓库根目录运行后续命令：

```sh
git clone https://github.com/Himpq/Pilot.git
cd Pilot
python tools/pilot.py --help
```

### 为相机生成 AP 安装配置

安装 `requirements-camera.txt` 的依赖。将相机连接到电脑后，先读取当前 AP 并生成配套文件：

```powershell
python tools/pilot.py flash prepare-ap --map patch.a6700.json --pilot ./pilot.so --output-dir ./ap-plan
```

预检不写入相机。它会输出各补丁位置的当前值与配置差异，并在 `ap-plan/` 保存完整的 `preflight-report.json`、安装前备份、匹配当前相机的 `patch.json`、补丁数据和通用刷写器。检查报告，决定是否覆盖不同的位置后再执行：

```powershell
python tools/pilot.py flash install --map ./ap-plan/patch.json --writer ./ap-plan/pilot_ap_flash.so --pilot ./ap-plan/pilot.so --yes
```

生成的配置记录本次读取的安装前字节；恢复时使用相同的配置和刷写器。安装前如 AP 文件发生变化，需要重新预检。

首次安装或更新常驻框架还需要以下本机材料：

| 材料 | 内容 |
|---|---|
| `patch.json` | 预检时根据当前相机生成的完整配置 |
| `pilot_ap_flash.so` | 预编译的通用刷写器，读取本次补丁数据和 `pilot.so` |
| USB 环境 | 安装 `requirements-camera.txt` 中的依赖 |
| 本机状态目录 | 存放原文件备份和设备操作记录 |

上述材料保留在本机，通过命令参数指定。`patch.a6700.json` 描述机型补丁；预检生成的配置记录当前相机、所选 `pilot.so` 和刷写器的校验值。

## 2. 检查与升级常驻框架

相机连接电脑并设置为大容量存储 USB 模式。下面是 Windows PowerShell 示例，将路径替换为自己的本机材料位置：

```powershell
$pilotArgs = @(
  "--map", "F:/PilotLocal/patch.json",
  "--writer", "F:/PilotLocal/pilot_ap_flash.so",
  "--pilot", "F:/PilotLocal/pilot.so",
  "--transport-root", "./patch",
  "--state-dir", "F:/PilotLocal/device-state"
)

# 只读演练：让相机侧刷写器自报环境（不写 /usr 和 /dev/nflasha5）
python tools/pilot.py flash writer-check @pilotArgs

# 读取相机 AP 和常驻 Pilot 的哈希
python tools/pilot.py flash check @pilotArgs

# 已安装 Pilot：更新 /usr/lib/pilot.so
python tools/pilot.py flash update-pilot @pilotArgs --yes
```

首次安装启动钩子时，使用同一组参数运行：

```powershell
python tools/pilot.py flash install @pilotArgs --yes
```

写入顺序与安全保证（`install` 与 `restore` 共用同一条通路）：

1. 读取并整包校验 `appFw.so`：配置声明 `binary_sha256` / `patched_binary_sha256` 时，除四个补丁窗口外还比对整个 89 MB 文件的 SHA-256，避免在预期外的固件上盲写。
2. 先备份将被修改的文件，再按配置校验刷写器和 `pilot.so` 的大小与 SHA-256、ELF 架构和刷写器标记。
3. 刷写器先备份并清零 `/dev/nflasha5` 头 512 字节的 `WBI2` 休眠快照头，**之后**才修改 `appFw.so`（两阶段 `fsync` 屏障：先写窗口 1/2/3 落盘，再写窗口 0，各自 `fsync`；`restore` 反向）。若清零失败则 `appFw.so` 一字未改直接返回，不会留下"补丁已写入但冷启动仍被旧快照遮蔽"的中间态（[#10](https://github.com/Himpq/Pilot/issues/10) 的同类故障）。
4. `/usr/lib/pilot.so` 通过临时文件 + `fsync` + 回读 + `renameat` + 目录 `fsync` 原子替换，写入中断或掉电不会留下被截断的 `pilot.so`（截断的 ELF 头会在开机加载时触发 `SIGBUS`）。
5. 写入后回读校验；若首次回读与预期不符而刷写器报告成功，会在同一会话内再读一次以排除 USB 抖动。

`update-pilot` 只按第 4、5 条原子替换 `/usr/lib/pilot.so`（刷写器回报 `mode=pilot`、`atomic=1`、`readback=OK`），不修改 `appFw.so`，也不清零 `WBI2` 快照头，但同样需要完整断电重启才会加载新版框架；替换成功后如仍读到旧哈希，说明仍在旧系统的 page cache 中，此时不要重复写入，应先断电重启。

成功后按提示**完整断电重启**：关机 → 拔出电池 ≥ 5 秒 → 装回 → 开机，仅按电源键待机不会刷新休眠快照与挂载状态。

`check` 按配置中的目标版本比对；升级前仍是旧版时，会报告 `persistent bootstrap: mismatch`。
`writer-check` 的 `check` 模式全程只读，只输出：`state`（`baseline` / `patched` / `mixed`）、`window_mask`（已打补丁窗口的位图）、`usr_statfs_stage`、`usr_magic` 与 `usr_flags`（`/usr` 的实际挂载参数，原厂应为只读）、`usr_free`（剩余空间）、`wbi_status` 与 `wbi_hdr4`（`/dev/nflasha5` 快照头状态，`wbi2` / `0x57424932` 表示仍存在，清零后为 `zero`）。首次安装前建议先执行一次，确认 `/usr` 只读、`/dev/nflasha5` 可读后再写入；此模式同样会如实报告混合状态，便于判断上次写入是否完整落盘。

若上次写入在屏障中途被打断，`check` 会显示 `state: mixed` 并逐窗口列出当前是原厂值还是补丁值；此时直接执行 `install` 或 `restore` 即可自动收敛到完整状态，无需 `--force`。
`--force` 只放宽"非窗口字节与配置不一致"这一项，四个窗口内的字节仍必须是已知的原厂值或补丁值；出现无法识别的代码字节时即使命令行了 `--force` 也会拒绝写入，应先重新预检或人工核对。

操作记录位于指定状态目录的 `device-state.json`，每次操作写入独立的 `attempt_id` 回执（含 `before`/`after` SHA-256、刷写器返回原文和备份位置），不沿用上一次运行的成功字段；原文件备份位于其中的 `backups/`。

## 3. 刷写 CP 固件补丁（可选，高风险）

> **警告：CP 刷写直接修改相机协处理器固件 `cpapp-b.bin`，风险高于 AP 补丁与 SD 卡模块。**
> - **适用范围**：仅限 Sony ILCE-6700 固件 2.00。刷写前必须先执行 `cp-check`，确认输出为 `CP image: original` 且完整 `sha256` 与配置一致；若状态为 `unknown` 或哈希不匹配，**切勿继续刷写**。
> - **防变砖要求**：确保相机电池电量充足、USB 连接稳固；刷写及回读校验期间（最长约 2 分钟）**严禁断电、拔线或操作相机**。
> - **备份与恢复**：`cp-install` 会在 `--state-dir` 下的 `cp-backups/` 保存完整的原始 `cpapp-b.bin` 备份，请妥善保留。拔出 SD 卡**不会**绕过已刷入的 CP 补丁，如需还原必须通过 USB 执行 `cp-restore`。
> - **按需使用**：仅在需要使用 UIPC v4 批量写入等依赖 CP 代码补丁的能力时刷写；仅使用基础 Pilot 功能时可跳过本节。

使用仓库内置的 `patch.a6700.json` 与 `patch/cpdelta.so`（命令中的状态目录可按本机路径调整）：

```powershell
$cpArgs = @(
  "--map", "./patch.a6700.json",
  "--transport-root", "./patch",
  "--state-dir", "F:/PilotLocal/device-state"
)

# 1. 先检查当前 CP 镜像状态与 SHA-256（只读）
python tools/pilot.py flash cp-check @cpArgs

# 2. 确认状态为 original 且哈希匹配后再刷写
python tools/pilot.py flash cp-install @cpArgs --yes
```

刷写完成后会执行整包回读校验并清理临时暂存文件，随后完整断电重启相机。如需卸载 CP 补丁并恢复原厂 `cpapp-b.bin`：

```powershell
python tools/pilot.py flash cp-restore @cpArgs --yes
```

## 4. 安装扩展模块

将模块文件放入 SD 卡 `/pilot/`，并将文件名加入现有 `/pilot/modules.json` 的 `modules` 数组，保留其他条目及加载顺序。
模块的兼容性、下载和具体使用方式以各自的发布说明为准。

## 5. 日志与停用

SD 卡主日志为 `/pilot/log.txt`，模块日志位于 `/pilot/modules/<模块名去掉 .so>/module.log`。
通过读卡器导出到本机：

```powershell
python tools/pilot.py logs --card D:/ --out F:/PilotLocal/logs/session-1
```

在 SD 卡 `/pilot/` 创建名为 `disable` 的空文件，重启后暂停扩展模块加载；移除该标记后重启可恢复。
恢复原启动钩子使用对应安装配置：

```powershell
python tools/pilot.py flash restore @pilotArgs --yes
```

`restore` 恢复该配置记录的 AP 修改窗口，并把 `WBI2` 休眠快照头清零以免冷启动仍被旧快照遮蔽，完成后完整断电重启；SD 卡模块和常驻库文件保留。

## 6. 彻底卸载

`restore` 只恢复 AP 启动钩子，`/usr/lib/pilot.so`、SD 卡 `/pilot/` 与机内 `WBI2` 备份都会保留，便于再次安装。需要回到完全原厂状态时，先 `restore`，确认 `check` 已显示 `state: baseline`，再删除常驻库与机内备份：

```powershell
# 分两步：先确认 AP 已回原厂，再清理常驻文件
python tools/pilot.py flash check @pilotArgs          # 应显示 state=baseline
python tools/pilot.py flash purge @pilotArgs --yes    # 删除 /usr/lib/pilot.so 与 /usr/lib/pilot_wbi_hdr.bak
```

`purge` 只在 `appFw.so` 已回到原厂基线、且 `/dev/nflasha5` 快照头确认为全零时才删除，并回读确认两个文件都不存在（回报 `verify=enoent`）；否则直接拒绝（`reason=appfw_not_baseline` 或 `reason=wbi_not_clear`），不会在补丁仍生效时删掉框架。也可一次完成 restore、回读校验与清理：

```powershell
python tools/pilot.py flash full-uninstall @pilotArgs --yes
```

中途失败时会保留已有备份并按刷写器提示处理，不会自动重试破坏性步骤。
SD 卡上的 `/pilot/` 目录和电脑端状态目录（备份与回执）由使用者自行删除。
`flash clean` 只清理相机 `/tmp/jiritsu/` 下 Pilot 自身的暂存文件；`flash clean-foreign --yes` 会连同其他项目的探针文件一并删除。
电脑端请保留状态目录里的 `backups/` 和 `wbi_hdr.bak`：`WBI2` 快照头一旦被清零，原厂休眠快照就只能从这份 512 字节备份恢复（`flash restore-wbi --yes` 仅在设备端头部为全零时写回）。

更多镜像补丁、AP/CP RAM 模块构建与工具参数见 [通用工具说明](../tools/README.md)。
实机刷写与验证记录见 [A6700 2.00 刷写记录](FLASH-RECORD-A6700-2.00.md)。

[返回首页](../README.md)
