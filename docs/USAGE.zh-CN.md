# 使用方式

中文 · [English](USAGE.md)

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

### Windows USB 驱动：请使用 WinUSB，而非 libusb-win32

Pilot 通过 libusb-1.0 与相机通信，因此 Windows 下相机的 service mode USB 设备（`054c:0336`，刷写时由工具自动切换）必须绑定 **WinUSB** 驱动。

如果之前为其他 A6700 工具（例如 OpenGate RAM patch）安装过 **libusb-win32**，刷写会在开始后立刻失败，报 `USBError: [Errno 32] Pipe error`。解决方法是更换驱动：

1. 先让相机进入 service mode，例如在相机仍以大容量存储模式连接且 libusb-win32 驱动生效时，使用 [Sony-PMCA-RE](https://github.com/ma1co/Sony-PMCA-RE) 的 `serviceshell`。
2. 打开 [Zadig](https://zadig.akeo.ie/)，在设备列表中选择 service mode 下的相机（`054c:0336`）。
3. 安装 **WinUSB**（替换 libusb-win32）。

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

# 只读自检：让相机侧刷写器输出当前环境信息（不写 /usr 和 /dev/nflasha5）
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

写入前会备份原文件并校验配置中的 SHA-256；刷写器先备份并清零 `/dev/nflasha5` 的 `WBI2` 快照头，再分两步写入 `appFw.so`（各自 `fsync`），并以临时文件 `renameat` 替换 `/usr/lib/pilot.so`，最后回读校验。`update-pilot` 仅替换 `/usr/lib/pilot.so`，不修改 `appFw.so` 与 `WBI2`。

成功后**完整断电重启**：关机 → 拔出电池 ≥ 5 秒 → 装回 → 开机（仅按电源键待机不会刷新休眠快照与挂载状态）。

- 若写入中途中断，`check` 会显示 `state: mixed`，直接重跑 `install` 或 `restore` 即可补齐，无需 `--force`（`--force` 仅放宽非窗口字节差异）。
- 操作记录与备份保存在 `--state-dir` 下的 `device-state.json` 与 `backups/`。

## 3. 刷写 CP 固件补丁

> **警告：CP 刷写直接修改相机协处理器固件 `cpapp-b.bin`，风险高于 AP 补丁与 SD 卡模块。**
> - **适用范围**：仅限 Sony ILCE-6700 固件 2.00。首次安装前执行 `cp-check`，确认 `CP state: baseline` 且全量 SHA-256 与配置一致。重新上电后执行 `cp-writer-check`，确认 `RESULT=OK`、`cp_magic=0x4d44`、`cp_ro=0` 和 `wbi_status=clear`。
> - **防变砖要求**：确保相机电池电量充足、USB 连接稳固；刷写及回读校验期间（最长约 2 分钟）**严禁断电、拔线或操作相机**。
> - **备份与恢复**：`cp-install` 会在 `--state-dir` 下的 `cp-backups/` 保存完整的原始 `cpapp-b.bin` 备份，请妥善保留。拔出 SD 卡**不会**绕过已刷入的 CP 补丁，如需还原必须通过 USB 执行 `cp-restore`。
> - **按需使用**：仅在需要使用 UIPC v4 批量写入等依赖 CP 代码补丁的能力时刷写；仅使用基础 Pilot 功能时可跳过本节。

使用仓库内置的 `patch.a6700.json` 与 `patch/pilot_cp_flash.so`。将命令中的状态目录改为本机路径。每条检查命令使用独立的 USB 服务会话，必要时重新上电。

```powershell
$cpArgs = @(
  "--map", "./patch.a6700.json",
  "--transport-root", "./patch",
  "--state-dir", "F:/PilotLocal/device-state"
)

# 1. 先检查当前 CP 镜像状态与 SHA-256（只读）
python tools/pilot.py flash cp-check @cpArgs

# 2. 重新上电并检查刷写器、CP 文件系统和 WBI2 状态
python tools/pilot.py flash cp-writer-check @cpArgs

# 3. 检查通过后，在新的服务会话写入
python tools/pilot.py flash cp-install @cpArgs --yes
```

看到 `CP install VERIFIED OK` 后，拔 USB、关机、取出电池至少 10 秒，再无卡开机。重新执行 `cp-check`，确认 `patched`、掩码 `0x3` 和目标全量哈希。若被 WBI2 门禁拒绝，按终端提示在独立的服务会话中用 AP 工具处理。

使用 `cp-restore` 正常恢复原厂固件。

```powershell
python tools/pilot.py flash cp-restore @cpArgs --yes
```

收到 `mixed_danger_w0_only` 或 `corrupt_windows` 时，保持供电与 USB 连接。写入失败后，若窗口之外的内容与原厂一致，工具可在同一服务会话中尝试自动恢复。有可用服务会话时，也可用 `cp-repair-restore --yes` 显式执行这一受限恢复。`unknown` 状态会拒绝写入。请保留 `cp-device-state.json` 和备份。实机记录及恢复验证范围见 [α6700 2.00 CP 刷写记录](CP-FLASH-RECORD-A6700-2.00.md)。

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

`restore` 只恢复 AP 启动钩子，会保留 `/usr/lib/pilot.so` 与机内 `WBI2` 备份。需要彻底清理时：

```powershell
# 分两步：先确认 AP 已回原厂，再清理常驻文件
python tools/pilot.py flash check @pilotArgs          # 应显示 state=baseline
python tools/pilot.py flash purge @pilotArgs --yes    # 删除 /usr/lib/pilot.so 与 /usr/lib/pilot_wbi_hdr.bak
```

或一步执行 restore + purge：

```powershell
python tools/pilot.py flash full-uninstall @pilotArgs --yes
```

`purge` 仅在 `appFw.so` 处于原厂基线且 `WBI2` 头为全零时执行。SD 卡 `/pilot/` 与电脑端 `--state-dir` 中的 `backups/`、`wbi_hdr.bak` 请按需自行保留（如需恢复原厂休眠快照 `restore-wbi`，必须依赖 `wbi_hdr.bak`）。
`flash clean` 清理 `/tmp/jiritsu/` 下 Pilot 暂存文件；`flash clean-foreign --yes` 会一并清理其他项目的暂存文件。

更多镜像补丁、AP/CP RAM 模块构建与工具参数见 [通用工具说明](../tools/README.md)。

[返回首页](../README.md)
