# D3 发布说明与验收范围

M0–M5 已通过 gpt-6-astra 独立审查。本文件随 M6 候选包提供，长时间稳定性与最终独立审查结论由 `docs/evidence/m6` 的最终报告补充；未完成前不把候选当作已通过全部验收。

## 启动与设备

解压后保留全部文件，运行 Windows 的 `RmVideoReceiver.exe` 或 Linux 的 `RmVideoReceiver.x86_64`。Godot 和 .NET 8.0.34 运行库已随包提供，无需编辑器或 SDK。监听地址默认 `0.0.0.0`，UDP 端口默认 `3334`；发送端使用 RoboMaster 的8字节头/1392字节分片 HEVC 协议。

默认自动解码 + CPU YUV。界面显示实际解码器、设备身份、传输路径、新视频提交速率、丢包和恢复次数。Windows 可选择 Intel/NVIDIA D3D11 或 CUDA；GPU传输目前验证 Windows RTX5070 Laptop 同设备 CUDA + Compatibility/OpenGL + NV12 8位。启动示例：

```text
RmVideoReceiver.exe --rendering-method gl_compatibility -- --start --decoder cuda --transfer gpu --bind 127.0.0.1 --port 3334
RmVideoReceiver.exe -- --start --decoder auto --transfer auto --port 3334
./RmVideoReceiver.x86_64 -- --start --decoder auto --transfer yuv --port 3334
```

`--transfer auto` 每会话只尝试一次 GPU bridge，不可用时保留健康 CUDA 硬解、改 CPU 并显示原因。指定 `--transfer gpu` 则明确失败，不做静默回读。后端格式失败可在自动解码时依次回退，等待下一匹配随机访问点；手动开始新会话才重试失败候选。

Linux 基线为 Ubuntu 24.04 x86_64/glibc2.39、X11/Wayland桌面及可用Vulkan/OpenGL驱动，系统仍需标准 .NET 原生依赖（libicu、libssl、zlib、fontconfig 等）。本轮实际机器是 WSL Ubuntu-24.04 + WSLg/llvmpipe；CUDA硬解可用、CUDA/GL互通不可用。原生Ubuntu、AMD与VAAPI未验证，固定FFmpeg未启用VAAPI；没有把WSL结果推广为全厂商支持。

## 诊断与常见问题

- 无画面时先检查监听IP、端口、防火墙与发送端HEVC封装，界面应先从等待参数/关键帧进入播放。没有新IDR/CRA时无法凭空恢复。
- GPU模式提示渲染器不支持：用Compatibility/OpenGL重新启动，或选择CPU/自动传输。解码GPU与渲染GPU不一致时用CPU中转；D3D11设备序号以当前进程实际名称/LUID为准。
- Main10默认拒绝；HDR、宽色域原色或未支持的transfer函数会明确失败。当前交付以8位SDR为常规输入。
- 画面超过500ms未更新会提示信号过期，2s清屏。停止会等待具体在途资源退役，不能只看解码线程结束。GPU退役失败会保留资源并阻止继续建立bridge，需要关闭应用重启。
- 诊断例：在用户参数后加 `--duration 60 --metrics new.ndjson --snapshot new.png`，输出路径必须不存在。duration从界面打开计时；末尾 `stopped` 记录用于确认退出。截图只产生一次整窗读回，与视频路径回读计数分开。

窗口刷新率不等于视频新画面率。M4同条件1080p60短测GPU约57.81次新画面/秒，CPU中转约59.81；不宣称GPU一定更快或本机所有路径持续1080p60。协议不含可靠跨组媒体PTS，当前也不伪造摄像头到屏幕延迟。

## 版本、构建和许可

Godot4.7.2 Mono、FFmpeg n8.1.3、.NET SDK8.0.428/runtime8.0.34、godot-cpp固定commit；详见 `dependencies.lock.json` 和 `build-manifest.json`。构建脚本验证实际编译器、CMake、SDK、源码提交和FFmpeg静态库SHA256，Windows链接不依赖额外MinGW运行时DLL。

包内 `licenses` 包含Godot实际引擎内置第三方清单与全文、godot-cpp/.NET/FFmpeg/nv-codec-headers/编译器运行库许可。`relink` 提供FFmpeg对应完整源码、配置、原生应用对象和静态库以及重新构建/链接脚本；可用修改后的兼容FFmpeg生成替换库，步骤见 `relink/README.txt`。应用作者原有权利不变。

发布文件 `manifest.json` 记录产品提交、引擎/模板/原生库与每个包文件摘要。诊断样本、故障注入和压力测试脚本仅位于外层processing，不混入运行资源。
