# Pilot

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/Platform-AArch64%20%2F%20BIONZ%20XR-blue)](README.md)
[![Status](https://img.shields.io/badge/Status-Experimental-orange)](README.md)

<img width="400" alt="05083979-51cb-4126-b926-339e21e61a45" src="https://github.com/user-attachments/assets/01ff953c-8194-448d-98f2-8f38e3fe164e" />


Pilot 是一个面向索尼相机固件的扩展框架，由常驻引导模块和按需加载的扩展模块组成。模块通过版本化 API 使用框架提供的能力。

## 特性

- 引导模块精简，常驻部分只负责加载调度
- 挂载成功后按清单加载扩展模块
- 模块先复制到 tmpfs 再加载，不占用可移动介质挂载
- 卡上 `disable` 标记可关闭扩展加载
- `PilotApi` 提供 `file`、`log` 和 `mlog` 命名空间，以及时间和内存接口

## 目录结构

| 路径 | 内容 |
|---|---|
| `patch/` | 核心源码、跳板代码、构建与部署工具 |
| `modules/` | 示例模块及其构建脚本 |
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
bash patch/build_pilot_dso.sh
bash patch/build_pilot.sh
bash modules/menu_ui_probe/build.sh
```

通过 `PILOT_SYSROOT`、`PILOT_CC` 等环境变量指定 sysroot 与交叉编译器。

## 许可

仅限在合法授权的设备上使用，本仓库仅供个人研究学习，避免复杂的Jiritsu注入。
