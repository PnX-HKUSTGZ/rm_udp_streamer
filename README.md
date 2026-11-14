# rm_udp_streamer

基于 Godot 4.7 Mono 的 RoboMaster UDP HEVC 视频接收工程，目标为 Windows 与 Ubuntu、1080p60。

M0 环境、M1 协议核心与 M2 解码/CPU 呈现已通过 6-astra 独立审查，当前进入 M3 恢复与独立交付。两平台已提供软件/硬件 HEVC 解码、原生 YUV/RGBA 画面与 C# 控制界面；性能按实际组合测量，尚未完成发布验收。

- [架构设计](docs/architecture.md)：GPU 帧传递为目标，CPU 中转为兼容路径；模块、帧接口、同步与恢复。
- [实施计划](docs/implementation-plan.md)：分阶段交付、双平台环境、设备矩阵和验收标准。
- [M0 能力与审查](docs/m0-capabilities.md)：固定依赖、真实硬解、图形互通证据及未验证范围。
- [M1 协议与审查](docs/m1-transport.md)：严格重组、预算、双平台全新构建与收包证据。
- [M2 解码与呈现](docs/m2-media.md)：真实视频、颜色/比例、原生上传、性能短测与明确限制。

技术方向为 Godot C# 界面、C++/FFmpeg 视频核心和 GDExtension 呈现适配。Ubuntu 验证使用本机 WSL，Linux 检出位于 `/home/lqx/code/rm_udp_streamer`。

任务过程、验证脚本和样本保存在外层 `processing/<对应任务>/`，不作为正式应用内容。
