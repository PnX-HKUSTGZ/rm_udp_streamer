# rm_udp_streamer

基于 Godot 4.7 Mono 的 RoboMaster UDP HEVC 视频接收工程，目标为 Windows 与 Ubuntu、1080p60。

当前分支正在实施 R0–R6 审查改进，运行代码候选为 `ae15111`。历史候选的 R0、R1、R2、R3、R5 已通过指定的独立审查；R4、R6 尚未完成。当前候选修复了同源断流的参数缓存及 GPU 初始化时丢失首组参数的问题，已完成双平台构建和相关功能验证，完整性能与实机自动接入仍在验收，见[当前候选记录](docs/evidence/r6/candidate-ae15111.md)。默认从本次接收的码流获取参数，当前实机中途直接启动尚未通过。

Windows NVIDIA 已实现零视频像素回读的 CUDA/OpenGL 路径；原生 Ubuntu 已实现 VAAPI 与设备身份核验。`88bf17a` 的 Windows CUDA、D3D11VA 与 AMD VAAPI 指定组合曾完成 30 分钟有效收流及完整诊断开销对照，见[R3 独立审查](docs/evidence/r3/report.md)。这些结果及此前 M0–M6 的 D3 发布记录保留为历史基线，各版本证据和支持范围分别记录。

- [当前实现架构](docs/current-architecture.md)：线程、最新帧准备、缓冲复用、代际和资源所有权。
- [当前能力与验收边界](docs/support-matrix.md)：逐设备的实测范围、性能证据与待验证项目。
- [交互使用与渲染切换](docs/usage.md)、[设备身份](docs/devices.md)、[固定依赖构建](docs/building.md)、[外部验证与CI](docs/validation.md)。

- [架构设计](docs/architecture.md)：GPU 帧传递为目标，CPU 中转为兼容路径；模块、帧接口、同步与恢复。
- [实施计划](docs/implementation-plan.md)：分阶段交付、双平台环境、设备矩阵和验收标准。
- [M0 能力与审查](docs/m0-capabilities.md)：固定依赖、真实硬解、图形互通证据及未验证范围。
- [M1 协议与审查](docs/m1-transport.md)：严格重组、预算、双平台全新构建与收包证据。
- [M2 解码与呈现](docs/m2-media.md)：真实视频、颜色/比例、原生上传、性能短测与明确限制。
- [M3 恢复与 D1](docs/m3-recovery.md)、[M4 GPU 与 D2](docs/m4-gpu.md)、[M5 设备与回退](docs/m5-backends.md)。
- [D3 使用与排错](docs/m6-release.md)、[M6 独立审查](docs/evidence/m6/review.md)、[固定依赖构建](docs/building.md)。

D3 历史包的 Windows/WSL 验收包含31分钟收流、自然序号回绕、60轮生命周期及故障恢复。其 Windows GPU 均值55.34次新画面提交/秒，WSL CPU呈现59.30次/秒；这些数字不代表当前改进候选。当前目标使用180秒预热、1800秒有效输入及严格的吞吐/延迟门槛，不能用短测或单项核心通过宣称稳定1080p60。

实现保留 Godot C# 界面、C++/FFmpeg 视频核心和 GDExtension 呈现适配的分层。验证环境包括 Windows、WSL，以及新增的原生 Ubuntu Intel N250、AMD Radeon 780M；支持声明以对应设备的完整验收为准。

任务过程、验证脚本和样本保存在外层 `processing/<对应任务>/`，不作为正式应用内容。
