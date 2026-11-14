# rm_udp_streamer

基于 Godot 4.7 Mono 的 RoboMaster UDP HEVC 视频接收工程，目标为 Windows 与 Ubuntu、1080p60。

当前处于架构设计阶段，视频接收与显示功能尚未实现。

- [架构设计](docs/architecture.md)：GPU 帧传递为目标，CPU 中转为兼容路径；模块、帧接口、同步与恢复。
- [实施计划](docs/implementation-plan.md)：分阶段交付、双平台环境、设备矩阵和验收标准。

技术方向为 Godot C# 界面、C++/FFmpeg 视频核心和 GDExtension 呈现适配。Ubuntu 验证使用本机 WSL，Linux 检出位于 `/home/lqx/code/rm_udp_streamer`。

任务过程、验证脚本和样本保存在外层 `processing/<对应任务>/`，不作为正式应用内容。
