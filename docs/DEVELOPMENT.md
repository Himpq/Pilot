# 模块开发与构建

模块导出 `pilot_module_init`，接口定义在 `patch/pilot.h`：

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

`sdk/pilot_mod_util.h` 提供编码、等待、日志、内存和补丁辅助函数。使用 CP 异步接口前，请检查 `PilotCpMemApi.struct_size`，并确保回调使用的数据在回调执行前保持有效。

## 构建与测试

```sh
PILOT_SYSROOT=/path/to/target/sysroot bash patch/build_pilot_dso.sh
PILOT_SYSROOT=/path/to/target/sysroot bash patch/tests/run_pilot_runtime_smoke.sh
```

通过 `PILOT_SYSROOT` 和 `PILOT_CC` 指定目标 sysroot 与交叉编译器。

## 目录

| 路径 | 内容 |
|---|---|
| `patch/` | 框架核心、API 和 AP 相机刷写入口 |
| `cp/uipc_v4/` | CP UIPC v4 处理器及其镜像补丁描述 |
| `sdk/` | 模块开发辅助函数 |
| `modules/` | 扩展模块源码及构建脚本 |
| `tools/` | 补丁、模块部署和日志工具 |
| `docs/` | 安装、使用和开发文档 |

## 启动日志

`/pilot/log.txt` 用空行分隔每次启动。模块加载记录包含文件名、加载顺序和初始化状态，失败时记录具体模块及错误。

## CP 补丁

`cp/uipc_v4/patch.json` 使用 `pilot.patch.v1` 格式，可以通过 `python tools/pilot.py image` 对匹配的本地 CP 镜像生成补丁或还原。相机上的 CP 补丁刷写目前使用 FireHydrant 的 `fl2_fl3/cp_uipc_patch.py` 和 `cpdelta.so`；Pilot 的 `patch/on_camera.py` 负责 AP 启动补丁及 Pilot 框架安装。两者都使用外部 `cpload_run.py` USB transport。

[返回首页](../README.md)
