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

构建 `patch/pilot_ap_flash.c`（固定 4096 页对齐，检查 AArch64、`INIT`、无 `NEEDED`/`TEXTREL` 且不超过 64 KiB）：

```sh
PILOT_CC=aarch64-linux-gnu-gcc PILOT_FLASH_OUT=./pilot_ap_flash.so bash patch/build_pilot_ap_flash.sh
```

运行宿主机故障注入与离线测试：

```sh
bash patch/tests/run_pilot_ap_flash_fault_tests.sh
python -m unittest discover -s patch/tests -p 'test_*.py'
python -m unittest discover -s tools/tests -p 'test_*.py'
```

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
python tools/pilot.py flash cp-writer-check --map patch.a6700.json
python tools/pilot.py flash cp-install --map patch.a6700.json --yes
python tools/pilot.py flash cp-restore --map patch.a6700.json --yes
```

加固刷写器先确认 CP 文件系统和 WBI2 状态，安装时先持久化处理函数，再写入口跳转，恢复时顺序反过来。每一阶段都执行 `fsync` 和回读。主机校验完整镜像哈希，写前持久化备份，写后核对完整目标镜像。确认仅补丁窗口异常后，可以尝试在同一服务会话恢复原厂。

在 Linux 或 WSL 下运行 `bash patch/build_pilot_cp_flash.sh` 可重建刷写器。主机端固定校验随仓库提供的刷写器 SHA-256，换工具链构建时需核对生成物并同步更新钉值。测试命令和实机验收记录见 [α6700 2.00 CP 刷写记录](CP-FLASH-RECORD-A6700-2.00.md)。

[返回首页](../README.md)
