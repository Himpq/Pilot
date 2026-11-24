# Pilot

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/Platform-AArch64%20%2F%20BIONZ%20XR-blue)](docs/USAGE.md)

Pilot 是面向索尼相机的模块化扩展框架。它为相机加载独立模块，让开发者可以围绕相机功能构建、更新和组合自己的扩展。

<table>
  <tr>
    <td width="50%"><img width="400" alt="Pilot 界面" src="https://github.com/user-attachments/assets/01ff953c-8194-448d-98f2-8f38e3fe164e" /></td>
    <td width="50%"><img width="400" alt="Pilot 菜单" src="modules/menu_ui/assets/menu-preview.png" /></td>
  </tr>
</table>

## 功能与特性

- **模块化扩展**：通过 SD 卡清单选择和组合模块，框架负责加载与初始化。
- **相机菜单**：[Menu UI](modules/menu_ui/README.md) 在相机中展示 Pilot 版本和模块列表。
- **视频扩展**：[6K 模块](modules/6k/README.md) 将 4K HS 24p 档位扩展为 5760×3240、23.976p 录制。
- **运行时内存访问**：通过统一接口读取、写入和修改 AP / CP 内存，CP 接口支持异步操作和 UIPC v4 批量写入。
- **文件与日志**：模块可以使用独立数据目录和日志，查看启动、加载及运行状态。

## 方便开发

模块只需实现 `pilot_module_init`，即可通过 `PilotApi` 使用文件、日志、时间和内存等能力。`sdk/pilot_mod_util.h` 提供常用辅助函数；通用工具支持补丁描述、模块构建、部署和日志导出。

- [安装与使用](docs/USAGE.md)
- [模块开发与构建](docs/DEVELOPMENT.md)
- [通用补丁工具](tools/README.md)
- [下载模块和框架](https://github.com/Himpq/Pilot/releases)
