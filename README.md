# rm_udp_streamer

基于 Godot 4.7 Mono 的 RoboMaster UDP HEVC 视频接收工程，目标为 Windows 与 Ubuntu、1080p60。

M0–M6 已逐阶段通过 gpt-6-astra 独立审查，D3 完成本轮 Windows/WSL 发布与稳定性验收。两平台提供软件/硬件 HEVC 解码、原生 YUV/RGBA 画面与 C# 控制界面；Windows NVIDIA 已提供零视频像素回读的 CUDA/OpenGL 路径和明确的自动回退。性能与支持范围按实际设备报告，不能把窗口刷新率当视频帧率。

- [架构设计](docs/architecture.md)：GPU 帧传递为目标，CPU 中转为兼容路径；模块、帧接口、同步与恢复。
- [实施计划](docs/implementation-plan.md)：分阶段交付、双平台环境、设备矩阵和验收标准。
- [M0 能力与审查](docs/m0-capabilities.md)：固定依赖、真实硬解、图形互通证据及未验证范围。
- [M1 协议与审查](docs/m1-transport.md)：严格重组、预算、双平台全新构建与收包证据。
- [M2 解码与呈现](docs/m2-media.md)：真实视频、颜色/比例、原生上传、性能短测与明确限制。
- [M3 恢复与 D1](docs/m3-recovery.md)、[M4 GPU 与 D2](docs/m4-gpu.md)、[M5 设备与回退](docs/m5-backends.md)。
- [D3 使用与排错](docs/m6-release.md)、[M6 独立审查](docs/evidence/m6/review.md)、[固定依赖构建](docs/building.md)。

D3 两平台均完成31分钟实际收流、自然序号回绕、60轮真实生命周期及故障恢复。Windows GPU长测平均55.34次新画面提交/秒，WSL CPU呈现59.30次/秒；不声明所有路径稳定1080p60。原生Ubuntu、AMD及VAAPI仍未验收。

技术方向为 Godot C# 界面、C++/FFmpeg 视频核心和 GDExtension 呈现适配。Ubuntu 验证使用本机 WSL，Linux 检出位于 `/home/lqx/code/rm_udp_streamer`。

任务过程、验证脚本和样本保存在外层 `processing/<对应任务>/`，不作为正式应用内容。

## MQTT 连接器

`scripts/network/MqttConnector.cs` 提供独立的 C# 连接接口，Windows / Ubuntu 共用。
按照通信协议第 2 章（印刷页 46）和附录三，默认连接 `192.168.12.1:3333`，
Client ID 原样使用机器人 ID 的十进制字符串，例如红方步兵 `"3"`、蓝方步兵 `"103"`（附录二）。
比赛网络中终端网卡地址按协议配置为 `192.168.12.2`。

```csharp
await using var mqtt = new MqttConnector("3");
await mqtt.ConnectAsync(); // 可传 CancellationToken；失败或服务端拒绝时抛出异常。
bool connected = mqtt.IsConnected;
// 在需要维持连接的整个期间保留 mqtt，离开作用域时自动断开。
```

也可通过 `new MqttConnector("3", "127.0.0.1", 3333)` 指定联调服务端。
使用 MQTTnet 4.3.7.1207、MQTT 3.1.1（官方示例所用库的默认版本），清洁会话，
无需用户名/密码或 TLS；每 15 秒保活，连接超时 10 秒。
只处理连接生命周期，不订阅、不发布、不解析 Protobuf、不自动重连。
`DisconnectAsync()` 主动断开；Godot 节点退出时可调用 `Dispose()` 立即释放连接。
连接器不会随视频界面自动启动，由调用方指定 ID 后调用 `ConnectAsync()`。
