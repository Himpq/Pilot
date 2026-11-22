# Pilot Menu UI

Pilot v0.1.1 的菜单模块，文件名 `pilot_menu_ui.so`。
适用 Sony ILCE-6700 固件 2.00，依赖 Pilot ABI 2。

## 功能

- 在现有“自定义设置”页底部增加 Pilot 入口。
- 显示 Pilot 版本，以及 `modules.json` 中的模块名称、清单启用状态和文件路径。
- 从 `/pilot/modules/<模块文件名去掉 .so>/config.json` 的 `title` 字段读取模块显示名。
- 使用原生列表和详情页，列表支持滚动；返回列表时恢复先前选择。

模块状态来自清单配置；运行时加载结果查看 `/pilot/log.txt`。

## 当前实现

列表屏幕为 `0x293`，详情屏幕为 `0x1C3`，Pilot 分组为 `0x535`。
菜单表、页面表和布局使用克隆副本；通过 Pilot 内存 API 发布相应 GOT 修改。
文字钩子将 Pilot 自定义 RID 转成字符串。

`pilot_ui_navigation.inc` 负责列表控制器生命周期、返回选中项恢复和详情焦点表。
列表返回先恢复 `sel_row`，再调用原生光标接口，保持选中项与界面通知一致。
详情显示布局保留原厂按钮控件类型，控制器使用独立焦点表。

源码中的地址对应 ILCE-6700 2.00；安装时检查原始表地址与关键内容。

## 构建

在 Pilot 仓库根目录运行：

```sh
PILOT_A6700_SYSROOT=/path/to/a6700/sysroot bash modules/menu_ui/build.sh
```

输出为 `build/pilot_menu_ui.so`。交叉编译器可通过 `AARCH64_CC` 指定。
脚本检查 AArch64、动态依赖、导出入口和 TEXTREL，并打印 SHA-256。

## 安装

先安装并启动 Pilot。将 `pilot_menu_ui.so` 复制到 SD 卡 `/pilot/`，
在现有 `/pilot/modules.json` 的 `modules` 数组中加入该文件名。
首次创建清单时可使用：

```json
{
  "modules": ["pilot_menu_ui.so"]
}
```

保留现有其他模块条目，断电重启后进入“自定义设置 → Pilot”。
在 SD 卡创建 `/pilot/disable` 可暂停 Pilot 扩展加载。

## 日志

安装成功会记录：

```text
PILOT_UI_READY group=0x535 list=0x293 detail=0x1c3 nav=lifecycle-v1 focus=typed-layout-v2
```

`PILOT_UI_NAV` 记录页面返回与光标恢复，`PILOT_UI_FOCUS` 记录详情焦点，
`PILOT_UI_ERR` 记录安装或导航错误。

## 验证

```sh
cc -std=c11 -O2 -Wall -Wextra -Werror tests/test_navigation.c -ldl -lpthread -o /tmp/pilot-ui-test
/tmp/pilot-ui-test
python tests/verify_native_focus.py /path/to/ILCE-6700-2.00/appFw.so
```

以上两条命令在 `modules/menu_ui/` 中运行。Python 检查需要 `unicorn`、`pyelftools`。

2026-11-22：候选已重新构建，导航生命周期 C 检查通过，原厂焦点筛选及 Version
控件类型检查通过 Unicorn 验证。

## 发布前待办

实机复测：从非第一项进入详情并多次返回、切换不同模块详情、详情滚轮和左右键，
以及退出 Pilot 后打开原厂 Version 页面。完成后再将候选包正式发布。
