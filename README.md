# DS5DongleRE

基于 **Bouffalo Lab BL616 / BL618** 的 DualSense 无线接收器开源固件。它通过蓝牙连接 DualSense 或 DualSense Edge 手柄，并在电脑上枚举为 USB DualSense，同时提供按键映射、宏、体感、音频和诊断等功能。

## 主要功能

- **蓝牙转 USB**：接收手柄蓝牙输入，向电脑输出标准 DualSense USB 报告。
- **按键映射**：支持单击、长按、双击、三击、按下和松开，提供主层与最多 4 个覆盖层。
- **多种输出**：映射手柄键、键盘、鼠标、媒体键、宏和《绝地潜兵 2》战备序列，并支持连发与开关模式。
- **手柄扩展**：支持 DualSense Edge 额外按键、触摸板分区、虚拟组合键和两段式扳机。
- **体感映射**：陀螺仪可映射到鼠标或左右摇杆，支持轴向、灵敏度、死区和静置校准。
- **设备端宏**：宏保存在接收器 Flash 中，由固件独立回放，不依赖电脑常驻软件。
- **音频与触觉**：包含 USB 音频、Opus 编解码、耳机/麦克风及音频触觉处理。
- **维护功能**：配置持久化、配对与状态灯、省电、诊断日志和性能统计。

## 支持板型

- LCTech BL616（默认）
- Sipeed M0S Dock（BL616）
- Ai-M61-32S-Kit（BL618）

## 目录结构

```text
firmware/             固件源码、构建脚本和 Opus 库
  src/                蓝牙、USB、映射、宏、音频及诊断实现
  lib/opus/           Opus 音频编解码库
  build_windows.bat   Windows 构建与烧录入口
tools/                开发、诊断、协议测试和验证脚本
```

## 构建

本仓库不包含 Bouffalo SDK 和 RISC-V 工具链，需要先准备：

1. DS5Dongle BL618 对应的 `bouffalo_sdk`；
2. T-Head RISC-V 工具链；
3. SDK 附带的 Make、CMake 和 Ninja。

在 `cmd.exe` 中设置路径并构建：

```bat
cd firmware
set BL_SDK_BASE=D:\path\to\bouffalo_sdk
set TOOLCHAIN_PATH=D:\path\to\toolchain_gcc_t-head_windows
build_windows.bat rebuild
```

默认构建 `lctech616`。可通过 `BOARD_TYPE=aim61` 或 `BOARD_TYPE=m0sdock` 选择其他板型，通过 `USB_SPEED=fs` 或 `USB_SPEED=hs` 选择 USB 速度。

```bat
build_windows.bat both
build_windows.bat flash COMx
```

构建产物路径会打印在脚本末尾。烧录前请确认板型、串口号和启动模式，避免因选错参数导致刷写失败。

## 说明

- 当前固件版本见 `firmware/src/version.h`。
- Web 配置页面不包含在本次源码目录中。
- `tools/` 中的脚本主要用于开发和诊断，使用前请先阅读脚本说明及参数。

## 开源项目与致谢

本项目在开发过程中参考、移植或使用了以下开源项目。感谢各项目作者及贡献者：

| 项目 | 用途或参考关系 | 许可证 |
| --- | --- | --- |
| [awalol/DS5Dongle](https://github.com/awalol/DS5Dongle) | 上游基础工程，部分实现由其移植或改编 | MIT |
| [xiph/opus](https://github.com/xiph/opus) | Opus 音频编解码 | BSD-3-Clause |
| [CherryUSB](https://github.com/cherry-embedded/CherryUSB) | USB 设备协议栈，由外部 SDK 提供 | Apache-2.0 |
| [bouffalolab/bouffalo_sdk](https://github.com/bouffalolab/bouffalo_sdk) | BL616／BL618 构建与运行依赖 | Apache-2.0 |
| [Linux hid-playstation](https://github.com/torvalds/linux/blob/master/drivers/hid/hid-playstation.c) | DualSense 协议、报文偏移及传感器刻度参考；未并入内核源码 | GPL-2.0 |
| [FreeRTOS](https://github.com/FreeRTOS/FreeRTOS-Kernel) | 实时操作系统及配置参考 | MIT |
| [Cockos WDL](https://github.com/justinfrankel/WDL) | 音频 sinc 重采样实现的行为与算法参考；未捆绑 WDL 源文件 | WDL License（zlib 类） |
| [JibbSmart/GamepadMotionHelpers](https://github.com/JibbSmart/GamepadMotionHelpers) | 早期陀螺仪自动校准参考；相关代码已于 R1.3 移除 | MIT |

更完整的第三方说明见 [NOTICE](firmware/NOTICE)。各组件的许可证归各自项目所有，使用时请同时遵守对应条款。

## 许可证

本项目自身源码采用 **GPL-3.0** 开源，详见 [LICENSE](firmware/LICENSE)。
