# M0：构建与 GPU 互通验证

日期：2026-11-15。状态：实现与验证完成，等待 6-astra 独立审查；审查通过前不进入 M1。

## 结论

Windows 和本机 WSL Ubuntu 均已实际加载 C# + C++ GDExtension 并正常退出。固定 FFmpeg 在 Windows Intel/NVIDIA D3D11VA、Windows NVIDIA CUDA 和 WSL CUDA 上输出硬件帧；每种模式的 120 帧结果全部匹配软件参考。

首条 GPU bridge 选择 **Windows NVIDIA + CUDA + Godot Compatibility/OpenGL**。已验证原生 RGBA8 纹理导入、CUDA 设备匹配/注册/映射/显存写入/设备到纹理复制/解除映射、Godot 采样，以及生产者和消费者完成后的释放。**这只是 M0 可行性结论，真实 HEVC 解码表面、YUV 平面、连续呈现与恢复属于 M4，尚未完成。**

本机 WSL 的 Vulkan 和 OpenGL 实际均使用 llvmpipe 软件渲染。CUDA 解码可用，但 CUDA 无法映射该 GL 上下文；WSL 交付采用 CPU 中转，并如实记录软件渲染的性能。此结论不代表原生 Ubuntu/NVIDIA 不支持互通。

## 固定依赖与构建

完整版本、源码提交、ABI、下载校验和、编译参数与静态归档哈希见 [依赖锁](../build/dependencies.lock.json)，构建说明见 [build/README.md](../build/README.md)。

| 组件 | 固定版本或验证结果 |
| --- | --- |
| Godot Mono | 4.7.2 stable，实际引擎 commit `ed1daf0bf001b61586d9930840f2f1394092c079` |
| .NET | SDK 8.0.428 / runtime 8.0.34，Windows 和 WSL 均成功构建并执行 |
| godot-cpp | 4.5 stable / `e83fd0904c13356ed1d4c3d09f8bb9132bdc6b77`；4.7.2 上实际验证兼容本项目所用 API |
| FFmpeg | n8.1.3 / `1041abdc962f4cc4f394aa8de9dc5236c0c3b9e7`；HEVC/parser/avcodec/avutil/swscale；CUDA，Windows 另有 D3D11VA |
| NVIDIA 头文件 | `f2053071ea93137255b6dcf42e9b4c9e0965a44e`；使用驱动 API，无需 CUDA Toolkit 或 nvcc |
| Windows 扩展 | MinGW GCC/G++ 15.2.0 win32-seh，C++17，CMake 4.1.3，MinGW Makefiles |
| FFmpeg Windows 库 | WSL 下 MinGW GCC 13.2 win32 交叉编译；Windows 真实加载/解码验证，未用模拟器代替 |
| Ubuntu | Ubuntu 24.04.5 / WSL2，GCC/G++ 13.3.0、CMake 3.28.3、NASM 2.16.01 |

采用已验证的静态 FFmpeg/godot-cpp 归档链接进单个扩展 DLL/SO，减少附带依赖；这是当前构建选择，用户已撤回“所有组件必须静态”的限制，不重编译 Godot。Windows 扩展的直接导入为系统 `KERNEL32.dll`、`msvcrt.dll`；Linux 为 `libm.so.6`、`libc.so.6` 和系统加载器。GPU 驱动与 .NET/引擎仍由各自运行环境提供。扩展内调用范围较小，最终视频扩展的完整依赖表必须在 M3/M6 重审。

导出模板与引擎版本同为 4.7.2 Mono，官方 URL/大小/SHA512 已锁定；大型模板包正在缓存，独立导出与包运行是 M3/M6 的门槛，此处不声称已通过。最初共享 FFmpeg SDK 的探针仅是前期设备能力基线，最终报告使用 n8.1.3 源码构建。

## 实际设备与解码证据

| 环境 | 实际渲染设备 | 实际解码设备与结果 |
| --- | --- | --- |
| Windows Vulkan index 0 | NVIDIA RTX 5070 Laptop，Vulkan 1.4.325 | D3D11VA index 1 为同型号独显；CUDA index 0 实际解码通过 |
| Windows Vulkan index 1 | Intel Arc 140T，Vulkan 1.4.303 | D3D11VA index 0，PCI vendor/device `8086:7d51`，通过 |
| Windows Compatibility | NVIDIA RTX 5070 Laptop，OpenGL 3.3 / driver 591.74 | `cuGLGetDevices` 在当前 GL 上下文返回 NVIDIA 设备；CUDA 注册与复制通过 |
| WSL Vulkan | llvmpipe / LLVM 20.1.2，Vulkan 1.4.318 | CUDA 输出硬件帧，NVIDIA 设备经 WSL `/dev/dxg` 映射；与渲染设备不相同 |
| WSL Compatibility | Mesa 25.2.8，llvmpipe，OpenGL 4.5 | CUDA/GL 设备匹配返回 304；该上下文不可用此互通路径 |

各 API 的设备索引不是同一编号空间。后续选择必须验证设备身份，不能把 `index 0` 直接互换。本机没有 AMD 测试设备，也没有 `/dev/dri`，所以没有 VAAPI/DMA-BUF 实测证据。当前最小 FFmpeg 构建不启用 VAAPI；M5 如要增加该后端，须补齐其依赖并重新验收，不能仅添加一个配置选项就声称支持。

测试样本：1920×1080、60 fps、HEVC Main、8 bit、120 帧、无 B 帧；使用发送器侧 FFmpeg 生成，SHA256 为 `17d8f0835ee58c8f1948f01a4f91dc901018418eb5a6d4f13f3e5d1acee16ba6`。静态库独立探针强制选择硬件格式，收到软件帧即失败；测试性回读后统一为 NV12 并逐帧比较 MD5。每个硬解案例均得到 120 个硬件帧，Windows 与 Linux 软件参考也一致。正确性回读和哈希耗时不属于端到端性能测量。

机器可读结果：

- [Windows 解码](evidence/m0/windows-static-decode.json)、[Linux 解码](evidence/m0/linux-static-decode.json)。
- [Windows NVIDIA Vulkan](evidence/m0/windows-vulkan-static-gpu0.json)、[Windows Intel Vulkan](evidence/m0/windows-vulkan-static-gpu1.json)、[WSL Vulkan](evidence/m0/linux-godot-vulkan-static.json)。
- [Windows GPU 资源与 CUDA 复制](evidence/m0/windows-gl-static.json)、[WSL GL 资源](evidence/m0/linux-godot-gl-static.json)。

## GPU bridge 候选判定

`feasible` 仅表示本阶段互通原语实测成立；完整视频路径仍须 M4 验收。

| 候选 | M0 状态 | 依据与下一步 |
| --- | --- | --- |
| Windows NVIDIA CUDA → GL | **feasible** | 同上下文设备匹配、RGBA8 注册/映射、CUDA 显存 memset 和设备到 array 复制、GL 正确采样均通过；M4 验证 NV12 分平面或 GPU 转 RGBA、AVFrame 生命周期和持续更新 |
| Windows D3D11 → Vulkan | **unverified** | 当前 Godot 4.7.2 Vulkan 设备创建代码未请求 Win32 外部内存扩展；公开导入接口不能单独解决跨 API 资源与完成同步。保留 D3D11 硬解 + CPU 路径 |
| Windows CUDA → Vulkan | **unverified** | 同上；没有证明 Godot 现有设备上的导出/导入标志、外部同步及布局协作。不因拿到设备句柄认定可用 |
| Windows Intel/AMD D3D11 → GL | **unverified** | 未验证该设备的 WGL 互通扩展、NV12/转色和共享资源条件；Intel CPU 中转已有硬解基础，AMD 待实机 |
| 本机 WSL CUDA → GL/Vulkan | **unsupported（当前环境）** | Vulkan/GL 均为 llvmpipe；GL CUDA 设备匹配实际失败。CPU 中转可继续，不能自动重启切换渲染器 |
| 原生 Ubuntu Intel/AMD VAAPI → DMA-BUF/Vulkan | **unverified** | 无测试设备/DRM 节点；待验证导出格式、plane/stride/modifier、同步和驱动版本 |
| 原生 Ubuntu NVIDIA CUDA → GL/Vulkan | **unverified** | 与 WSL 分开；GL 可复用接口设计，但仍须原生实测。Vulkan 外部资源条件单独验证 |

因此默认 Mobile/Vulkan 保留 CPU 呈现；首条 GPU 模式显式选择 Compatibility 并重启生效。不能在一次会话中静默更换引擎渲染器。UI、FrameLease、协议和解码核心继续共用。

## 同步与释放验证边界

探针创建 128×128 `GL_RGBA8` 原生纹理，以 GPU 清屏建立四象限基线。Windows 使用 CUDA 显存 memset 和设备到纹理复制将颜色顺序偏移一格，所以无操作的复制无法通过像素断言。纹理由 `RenderingServer.texture_create_from_native_handle` 导入，C++ Texture2D 包装只持有引擎 RID，原生 GL 所有权由探针管理；已核对 Godot 4.7.2 的 `is_from_native_handle` 释放逻辑。

生产者使用真实 GL fence 完成检查；CUDA map/unmap、context synchronize 与返回码检查覆盖跨 API 的交接。显示后先移除唯一 Canvas 消费者，再在相同渲染上下文中插入 consumer fence，只有 `ALREADY_SIGNALED` 或 `CONDITION_SATISFIED` 才释放 RID 和 GL 纹理。单独回调到渲染线程或等待固定帧数均不作为 GPU 完成依据，也不调用全局 `RenderingDevice.sync()`。

每个平台执行 12 次创建/显示/释放，其中 6 次检查四角像素、6 次在第一帧之后立即停止；无 GL 错误，均实际等到 fence。截图是独立诊断读回，报告单列 6 次，不能把包含截图的探针当成绝对无读回的性能路径。生产者复制路径自身未调用像素 CPU 回读。M4 仍须验证真实解码帧和在途槽，M6 仍须完成 50 次生命周期与 30 分钟稳定性。

## 证据位置与复现

临时源码、构建目录、样本、PNG、原始日志在工作区外层 `processing/2026-11-15-m0-environment/`，Linux 对应 `/home/lqx/code/processing/2026-11-15-m0-environment/`。产品仓库只保存锁文件、构建入口和可审查报告。探针源码与样本摘要在 [源码校验清单](evidence/m0/probe-source-sha256.json)。

任务内 `run_static_decode.py` 重跑两平台强制硬解/软件 MD5 比较，`run_windows_probe.ps1` 和 `run_linux_probe.py` 重跑实际引擎验证。源码中给出所有 GPU API、返回码与等待条件，可独立审查。图形探针不支持 headless 代替真实图形路径。
