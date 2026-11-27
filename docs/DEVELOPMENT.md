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

### AP 刷写器

`patch/pilot_ap_flash.c` 是相机侧真正修改 `appFw.so`、`/usr/lib/pilot.so` 和 `/dev/nflasha5` 的程序，构建脚本会检查 AArch64 架构、`INIT` 段、无 `NEEDED`、无 `TEXTREL`，并限制在 Jiritsu 单次传输上限 64 KiB 以内：

```sh
PILOT_CC=aarch64-linux-gnu-gcc PILOT_FLASH_OUT=./pilot_ap_flash.so bash patch/build_pilot_ap_flash.sh
```

页大小固定为 4096（部分交叉工具链默认 64 KiB 段对齐，相机最小加载器不接受），模块必须无文本重定位，因此这两项在构建期直接失败，而不是等实机变砖后才发现。仓库内的 `patch/pilot_ap_flash.so` 由该脚本构建，换用其他编译器时产物字节可能不同（`.text` 等加载段一致），可用 `--writer` 指定自行构建的文件。

刷写器的关键路径行为用宿主机故障注入测试覆盖，不需要相机、QEMU 或交叉工具链：源码在 `-DPILOT_FLASH_HOST_TEST` 下改用 libc 调用并暴露 `pilot_flash_run_once()`，测试通过注入 `open()`/`pwrite()`/`fsync()` 失败检查两阶段 `fsync` 屏障顺序、`pilot.so` 原子替换、`WBI2` 清零与回滚、混合窗口逐窗口回滚以及 512 字节结果文件的边界：

```sh
bash patch/tests/run_pilot_ap_flash_fault_tests.sh
```

### 离线测试

```sh
python -m unittest discover -s patch/tests -p 'test_*.py'
python -m unittest discover -s tools/tests -p 'test_*.py'
```

`patch/tests/test_on_camera_offline.py` 校验既有安装、恢复与预检流程，`patch/tests/test_on_camera_hardening.py` 覆盖加固后的宿主机行为（写入前 `WBI2` 校验、整包 SHA-256 闸门、`--force` 收紧、混合状态收敛、`--purge`/`--full-uninstall` 零残留、512 字节结果严格解析与每会话回读重试）。两者都不访问相机，也不需要 `requirements-camera.txt` 的依赖。

## 目录

| 路径 | 内容 |
|---|---|
| `patch/` | 框架核心、API 和 AP 相机刷写入口 |
| `patch/cp/uipc_v4/` | CP UIPC v4 处理器源码 |
| `patch/tests/` | 刷写入口离线测试与 AP 刷写器故障注入测试 |
| `sdk/` | 模块开发辅助函数 |
| `modules/` | 扩展模块源码及构建脚本 |
| `tools/` | 补丁、模块部署和日志工具 |
| `docs/` | 安装、使用和开发文档 |

## 启动日志

`/pilot/log.txt` 用空行分隔每次启动。模块加载记录包含文件名、加载顺序和初始化状态，失败时记录具体模块及错误。

## CP 补丁

> [!WARNING]
> 请尽量维持原厂 CP 状态。刷写 CP 补丁前，请先确认相机**从未刷过与 Pilot 冲突的其他项目**，或**未验证 CP 补丁区是否冲突的第三方项目**（例如占用同一代码洞或修改过 `cpapp-b.bin` 的其他补丁）。

根目录 `patch.a6700.json` 中的 `cp.patch` 保存 CP 补丁描述。相机刷写统一使用 `patch/on_camera.py`：

```sh
python -m pip install -r requirements-camera.txt
python tools/pilot.py flash cp-check --map patch.a6700.json
python tools/pilot.py flash cp-install --map patch.a6700.json --yes
python tools/pilot.py flash cp-restore --map patch.a6700.json --yes
```

写入前会备份 CP 文件，完成后检查完整文件并提示重启。`cp-install` 与 `cp-restore` 会在抽点前门控之后强制校验整份 `cpapp-b.bin` 的全量 SHA-256（仅当严格匹配原厂基线时才允许安装，严格匹配已安装基线时才允许还原）；其他状态会显示为 `unknown` 并拒绝写入。

[返回首页](../README.md)
