# 当前候选的能力与验收边界

更新时间：2026-11-16。本文用于 R0–R6 改进分支的候选包。R0、R1、R2 已独立审查通过；R3–R6 仍在验收。历史 D3 的结果仅适用于其原始二进制，不构成当前包的最终验收。

## 平台、后端与传输

| 平台与设备 | 已实现路径 | 当前实测范围 | 未完成项目 |
| --- | --- | --- | --- |
| Windows / NVIDIA RTX 5070 Laptop | CUDA 硬解，CPU YUV/RGBA；同设备 CUDA/OpenGL GPU 桥接 | 实际硬解/呈现、身份、颜色；04737bd 最终包 CUDA/GPU 完成有效 180/1800 长测 | 诊断对照与当前源 GPU 故障回归仍在执行 |
| Windows / Intel Arc 140T、NVIDIA | D3D11VA 硬解与 CPU 回读上传 | 实际硬件帧、设备选择和桌面播放 | 各拟声明性能档位及最终包验收 |
| Ubuntu 24.04 / Intel N250 | VAAPI 硬解与 CPU 回读上传 | 核心像素、身份、帧寿命、恢复、格式变化、Main10 策略 | 当前没有连接物理显示器，桌面与性能待验证 |
| Ubuntu 24.04 / AMD Radeon 780M | VAAPI 硬解与 CPU 回读上传 | 核心像素、身份、帧寿命、桌面/回退矩阵；04737bd 发行包的 Wayland/Vulkan Mobile/低延迟组合完成 180/1800 长测、恢复和 60 轮原生库生命周期 | 诊断开销与阶段独立审查；没有物理扫描测量 |
| Windows / Ubuntu 软件解码 | HEVC 软件解码，CPU YUV/RGBA | 功能、恢复、颜色及已有桌面样本检查 | 性能等级须对应具体设备、配置和最终二进制 |
| WSL / WSLg | 已验证的软件及部分 CUDA/CPU 路径 | 开发环境回归 | 不替代原生 Ubuntu GPU 桌面验收 |
| 原生 Ubuntu / NVIDIA；Windows / AMD | 已有相关后端实现 | 当前未取得完整实机证据 | 实际设备、呈现、回退、性能和最终包验收 |

AMD 上述单个长测组合在 `04737bd` 自包含发行包中的均值/P10 为 60/60 fps，首包到纹理提交延迟 P95/P99 为 33/42 ms，输入119040帧、自然回绕一次、没有超过一帧的发送迟到。包使用独立 VAAPI developer 依赖锁，不能自动套用到别的依赖、渲染器、包或硬件。测试时应用持续可绘制、报告刷新率 60 Hz、桌面未锁屏；Mutter 电源属性出现0/3，未验证物理屏幕实际扫描。原显示模式和锁屏已恢复。

Windows 先前因最小化、采样覆盖不足或执行中断而无效的轮次均保留。阶段冻结候选完成有效 180/1800 长测后，`04737bd` 最终包 CUDA/GPU 也独立完成同一门槛：59.9994 fps、P10=60、P95/P99=30/32 ms，持续接电、165 Hz且可绘制。119040帧输入中一次发送迟到约18.6ms，原记录保留；GPU源帧峰值1，正常停止资源归零。目标仍为均值 ≥59.4 fps、完整秒窗 P10 ≥58、关联延迟 P95 ≤50 ms/P99 ≤100 ms、刷新率 ≥60 Hz、测量期间持续可绘制。

## 输入与回退

基线为 1920×1080、60 fps、HEVC 8 位 4:2:0 SDR。矩阵/range、裁剪、比例、色度位置和格式变化按实际输入元数据处理。Main10 的 CPU 路径需显式允许降精度；HDR、未支持的宽色域及 LINEAR/LOG transfer 明确拒绝。GPU 桥接当前限定 NV12 8 位、CUDA 与 OpenGL 同设备，仍包含 GPU 内复制。

`--transfer gpu` 要求明确选择 CUDA，不满足条件时失败。`--transfer auto` 允许 GPU 尝试失败后保留解码并回退 CPU YUV，界面报告实际路径和原因。VAAPI 仅交付 CPU 回读路径；VAAPI/DMA-BUF/Vulkan、D3D11/Vulkan 直传尚未实现。

固定样本与模拟发送器能验证协议处理及恢复逻辑，真实 RoboMaster 图传设备验收仍待完成。没有实机证据时不声明具体固件兼容或相机到屏幕延迟。

## 运行与依赖

解压后运行 `RmVideoReceiver.exe`（Windows）或 `RmVideoReceiver.x86_64`（Ubuntu）；保留包内文件结构。包应自带 Godot 与 .NET 运行时，仍需操作系统的显示环境和 GPU 驱动。Ubuntu 基线为 24.04 x86_64、glibc 2.39、X11/Wayland，以及标准 .NET 原生依赖和 fontconfig；不声明兼容 Ubuntu 22.04。

旧官方 Linux FFmpeg 锁没有 VAAPI。启用 VAAPI 的候选使用独立 developer 锁，包中记录实际依赖类别及摘要。VAAPI 包启动时需系统 `libva2`、`libva-drm2`、`libdrm2` 和对应 Intel/AMD 媒体驱动，即使选择软件解码也不能省略这些动态依赖。

`04737bd` 候选已在 Windows 和原生 Ubuntu 的干净检出完成导出、许可/源码材料清单、实际重链接及无 SDK 启动；原始与重链接包的实际硬解播放也有独立结果。性能、生命周期和故障仍逐项对应包内 `manifest.json` 的提交与摘要，见[构建阶段报告](evidence/r5/report.md)。`validated: false` 的开发者依赖不因部分测试通过而自动升级为通用已验收依赖。

使用方法见[交互使用](usage.md)、[设备身份](devices.md)、[构建发布](building.md)和[外部验证](validation.md)。当前线程、所有权和停止边界见[当前架构](current-architecture.md)。
