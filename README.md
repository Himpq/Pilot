# Pilot

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/Platform-AArch64%20%2F%20BIONZ%20XR-blue)](README.md)
[![Status](https://img.shields.io/badge/Status-Experimental-orange)](README.md)

<img width="400" alt="05083979-51cb-4126-b926-339e21e61a45" src="https://github.com/user-attachments/assets/01ff953c-8194-448d-98f2-8f38e3fe164e" />


Pilot 是一个面向索尼相机固件的扩展框架，为模块提供非 Jiritsu 依赖与运行时内存访问与便捷日志功能。

## 使用方式

框架发布下载：[Releases](https://github.com/Himpq/Pilot/releases)。适用 Sony ILCE-6700 固件 2.00，模块 ABI 为 2。电脑端工具需要 Python 3.10 或更新版本。

### 1. 准备文件

从 Releases 下载所需版本的 `pilot.so`。
下载源码包或克隆仓库，在仓库根目录运行后续命令：

```sh
git clone https://github.com/Himpq/Pilot.git
cd Pilot
python tools/pilot.py --help
```

首次安装或更新常驻框架还需要以下本机材料：

| 材料 | 内容 |
|---|---|
| `patch.json` | 匹配当前相机 AP 基线的完整 `pilot.patch-map.v1` 配置 |
| `pilot_ap_flash.so` | 与配置、目标 `pilot.so` 配套的刷写器，内部嵌入待安装的框架 |
| USB transport 目录 | `cpload_run.py` 及其所需的本机 USB 环境 |
| 本机状态目录 | 存放原文件备份和设备操作记录 |

上述材料保留在本机，通过命令参数指定。`patch.a6700.json` 是结构说明；安装使用包含目标哈希、框架及刷写器哈希的完整配置。
更新框架版本时，配置和刷写器也要与新的 `pilot.so` 一起匹配。

### 2. 检查与升级常驻框架

相机连接电脑并设置为大容量存储 USB 模式。下面是 Windows PowerShell 示例，将路径替换为自己的本机材料位置：

```powershell
$pilotArgs = @(
  "--map", "F:/PilotLocal/patch.json",
  "--writer", "F:/PilotLocal/pilot_ap_flash.so",
  "--pilot", "F:/PilotLocal/pilot.so",
  "--transport-root", "F:/PilotLocal/transport",
  "--state-dir", "F:/PilotLocal/device-state"
)

# 读取相机 AP 和常驻 Pilot 的哈希
python tools/pilot.py flash check @pilotArgs

# 已安装 Pilot：更新 /usr/lib/pilot.so
python tools/pilot.py flash update-pilot @pilotArgs --yes
```

首次安装启动钩子时，使用同一组参数运行：

```powershell
python tools/pilot.py flash install @pilotArgs --yes
```

工具在真实写入前备份原文件，写入后回读校验。成功后完整断电重启，加载新版框架。
`check` 按配置中的目标版本比对；升级前仍是旧版时，会报告 `persistent bootstrap: mismatch`。
操作记录位于指定状态目录的 `device-state.json`，原文件备份位于其中的 `backups/`。

### 3. 安装扩展模块

将模块文件放入 SD 卡 `/pilot/`，并将文件名加入现有 `/pilot/modules.json` 的 `modules` 数组，保留其他条目及加载顺序。
模块的兼容性、下载和具体使用方式以各自的发布说明为准。

### 4. 日志与停用

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

`restore` 恢复该配置记录的 AP 修改窗口，完成后完整断电重启；SD 卡模块和常驻库文件保留。
更多镜像补丁、AP/CP RAM 模块构建与工具参数见 [通用工具说明](tools/README.md)。

## 特性

- 引导模块精简，常驻部分只负责加载调度
- 挂载成功后按清单加载扩展模块
- 模块先复制到 tmpfs 再加载，不占用可移动介质挂载
- 卡上 `disable` 标记可关闭扩展加载
- `PilotApi` 提供 `file`、`log` 和 `mlog` 命名空间，以及时间和内存接口

## 启动日志

`/pilot/log.txt` 在每次启动记录前写入一个空行，便于区分不同启动。
模块加载记录包含清单文件名、加载顺序和初始化状态；加载失败时记录具体模块及错误。

```text

2026-11-22T00:00:00Z Pilot startup: ...
2026-11-22T00:00:00Z Pilot modules.json loaded count=1
2026-11-22T00:00:00Z name=pilot_menu_ui.so Pilot module loading index=1/1 path=/tmp_wbshd/pilot/modules/pilot_menu_ui.so
2026-11-22T00:00:00Z name=pilot_menu_ui.so Pilot module initialized index=1/1 status=0
```

## 目录结构

| 路径 | 内容 |
|---|---|
| `patch/` | 框架核心、API 和相机刷写入口 |
| `modules/` | 扩展模块源码及其构建脚本 |
| `tools/` | 通用补丁、模块部署和日志工具 |
| `build/` | 构建产物 |
| `patch/tests/` | 测试脚本 |

## 模块

模块导出 `pilot_module_init`，接口定义在 `patch/pilot.h`。

```c
#include "pilot.h"

PilotStatus pilot_module_init(const PilotApi *api)
{
    if (!api || api->abi_version != PILOT_ABI_VERSION)
        return PILOT_STATUS_UNSUPPORTED;

    api->file->write(api->file, "settings.cfg", "enabled=1\n");
    api->log->print("pilot module loaded");
    api->mlog->print(api->mlog, "module initialized");
    return PILOT_STATUS_OK;
}
```

## 构建

```sh
PILOT_SYSROOT=/path/to/target/sysroot bash patch/build_pilot_dso.sh
PILOT_SYSROOT=/path/to/target/sysroot bash patch/tests/run_pilot_runtime_smoke.sh
```

通过 `PILOT_SYSROOT`、`PILOT_CC` 等环境变量指定 sysroot 与交叉编译器。

## 许可

仅限在合法授权的设备上使用，本仓库仅供个人研究学习，避免复杂的Jiritsu注入。

## 通用补丁工具

[`tools/README.md`](tools/README.md) 提供补丁描述校验、离线镜像修改、AP/CP RAM 模块构建、SD 模块部署与日志导出。相机刷写通过外部本地配置调用 `on_camera.py`；研究资料与设备记录保留在本机。
