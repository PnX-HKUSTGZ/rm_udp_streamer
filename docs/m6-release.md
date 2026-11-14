# D3 发布说明与验收范围

M0–M5 已通过 gpt-6-astra 独立审查。M6 双平台长测、真实生命周期、故障注入、完整许可与重链接验证已完成，当前等待最终独立审查。机器可读结果见 [M6 验证摘要](evidence/m6/validation.json)。

## M6 实测

测试核心为 `b8bde42`，固定构建/发布材料为 `84bbd2e`。两个 D3-rc1 独立包同时在本机 Windows 与 WSL 运行；输入固定为 M0 的1920×1080/60 HEVC样本，从协议序号0开始，约1092秒自然经过65535→0。运行在空工作目录，PATH中无SDK，DOTNET_ROOT指向不存在目录。

| 指标 | Windows CUDA/GPU/OpenGL | WSL CUDA/CPU/Vulkan llvmpipe |
| --- | --- | --- |
| 实际持续发送 | 1869.58秒，112175帧 | 1869.25秒，112155帧 |
| 自然回绕/误判重启 | 1次/0次 | 1次/0次 |
| 视频像素CPU回读 | 0 | CPU中转按实际帧计数 |
| 180秒后平均解码速率 | 60.00帧/秒 | 59.96帧/秒 |
| 180秒后平均新纹理提交 | 55.34帧/秒 | 59.30帧/秒 |
| 一秒窗口提交速率P10 | 45.94帧/秒 | 57.75帧/秒 |
| 线程/句柄 | 稳态约57/855，没有持续增长 | 72线程/70文件描述符，保持稳定 |
| GPU停止退役 | 103450次复制、源释放、消费者fence、退役完全一致；live源/槽均0 | CPU路径，无GPU bridge |

两平台并行长测用于稳定性验收，不是隔离设备性能对比；编译校验工作只在初始预热期间并行。队列始终受预算约束，没有持续积压，未把窗口刷新率冒充视频提交率。

Windows在180秒预热后，私有提交内存首/尾5分钟均值相差约2.63MiB，周期性回落，没有随帧数持续增加。WSL匿名驻留内存首/尾窗口相差约21.54MiB，主要发生在运行前段，最后约10分钟保持约290MiB平台。原始采样、全程斜率及窗口均值全部保留；两OS的内存指标定义不同，不比较绝对值。

每平台另执行60次收到真实视频后的完整生命周期：Windows每轮至少30次GPU提交，覆盖清空/销毁控件与窗口尺寸变化；WSL交替CPU YUV/RGBA。每次检查worker、端口及GPU引用/纹理退役，均通过。Windows另有早期60轮短流成功记录；最终进程采样采用实际GUI引擎进程，排除console启动器的内存数据。

最终核心上又完成缺帧、损坏访问单元、2.5秒断流、换源、尺寸/范围变化、1800组过载与恢复：Windows19次恢复、WSL18次恢复，各1次明确输入错误；随后各18轮模式切换通过。独立包端口占用明确报错并退出。截图确认真实画面、比例与实际路径。

FFmpeg从随包完整源码分别为Linux原生和Windows交叉重建，修改`av_version_info()`的返回值后重新链接并替换测试包库。实际主UI报告`n8.1.3-relink-validation`，Windows547解码/473提交，Linux549/543，正常停止；正式包仍保留原始n8.1.3库。完整文件清单、第三方许可、实际动态依赖与自包含.NET8.0.34检查通过。

不宣称本机GPU路径持续1080p60。原生Ubuntu、AMD和物理驱动拔除/挂死仍未实测。本轮已完成的范围是Windows本机Intel/NVIDIA及WSL Ubuntu矩阵。

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

这是桌面视频接收程序，验收使用实际窗口。`--headless`不属于受支持运行方式，其渲染完成信号不可用于当前定时退出流程。

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
