# M5：设备矩阵与自动回退

2026-11-15，主体实现 `fb6eea8`，异常路径补充 `b8bde42`。Windows 16 组、WSL Ubuntu 11 组真实 HEVC 图形验证全部通过；最终补丁又在两平台复跑组合回退通过。尚待 gpt-6-astra 独立审查。具体配置、实际设备、各段结果及退役计数见 [validation.json](evidence/m5/validation.json)。

## 选择策略

解码和画面传输是两个独立选择。默认仍是 `decoder=auto, transfer=yuv`，兼容 M3/D1 的已测行为；可在界面选“自动 GPU / CPU”或传入 `--transfer auto`。

| 配置 | 候选与行为 |
| --- | --- |
| Windows 自动解码 + CPU YUV/RGBA | D3D11VA → CUDA → 软件 |
| Windows 自动解码 + 自动传输 | CUDA → D3D11VA → 软件；健康 CUDA 优先尝试 GPU bridge |
| Linux 自动解码 | CUDA → 软件；本构建未启用 VAAPI |
| 指定 software/cuda/d3d11va | 强制该解码器；失败明确报错，不能悄悄改软件 |
| 自动传输 | CUDA 的 bridge 失败后保留健康硬解并改 CPU；其他解码器直接使用 CPU，记录原因 |
| 强制 gpu 传输 | 要求显式 CUDA；不支持的渲染器、设备、格式均报错且不读回像素 |

候选在每个接收会话中最多尝试一次。设备创建失败和实际码流的硬件格式/profile 协商失败都记录原因；运行时换后端在当前 `feed` 调用退出后执行，保留匹配的参数集，丢弃旧 parser/codec 状态并等待下一个随机访问点。不会重新提交可能引用已销毁 parser 内存的访问单元。恢复时间依赖发送端的 IDR/CRA 周期。

格式恢复、发送端重启不会清除失败缓存；手动开始新会话才重试。输入损坏仍走 M3 的关键帧恢复；HDR、未允许的 Main10 等显示策略拒绝会明确失败，不能靠换解码器绕过。

传输失败另有一次性缓存与呈现代际：失败前的 GPU 帧不能在 CPU 路径生效后重新发布。先清空旧画面及 shader 引用，再在渲染队列安全退役 bridge；后续 CPU 上传排在其后。退役失败继续使用 M4 的资源隔离和重启阻止规则，不能把驱动挂死伪装为安全回退。

## 实际设备与结果

CUDA 诊断来自实际 FFmpeg CUDA context，包含设备名和稳定 UUID。D3D11 在创建前枚举指定序号，创建后再次核对名称/LUID；不同进程中序号不应作为永久身份。本轮发现 FFmpeg 在无效 D3D11 序号下可能选择默认设备，现已显式拒绝。

| 组合/场景 | 结果 |
| --- | --- |
| Windows NVIDIA RTX 5070 Laptop / CUDA / OpenGL 自动传输 | 真正 GPU NV12；一次 bridge 尝试、视频回读 0 |
| Windows Intel Arc 140T / D3D11VA / NVIDIA OpenGL 渲染 | CPU NV12 正常显示；实际解码/渲染设备分别报告，覆盖混合显卡中转 |
| Windows NVIDIA / D3D11VA / NVIDIA OpenGL | CPU NV12 正常显示 |
| Windows CUDA / Vulkan 自动传输 | bridge 仅试一次，保留 CUDA、改 CPU NV12并显示原因 |
| Windows 已在 GPU 播放的 NV12 → Main10（显式允许）→ NV12 | bridge 安全退役；CUDA 保持不变，先 CPU RGBA 再 CPU NV12；不会重新探测 GPU |
| Windows 自动 CPU 模式：NV12 → 4:4:4 → 灰度 → NV12 | D3D11VA → CUDA → 软件；后端总尝试 3 次，恢复普通格式后仍沿用软件 |
| Windows 自动 GPU 模式：NV12 → 灰度 → NV12 | 已在用的 GPU bridge 退役，最终软件 CPU；旧呈现代际不能再显示 |
| WSL Ubuntu / CUDA / llvmpipe OpenGL 自动传输 | CUDA/GL 查询报告 CUDA_ERROR_OPERATING_SYSTEM；一次尝试后 CPU NV12，保持 CUDA |
| WSL 自动解码：实际不支持的灰度格式 | CUDA → 软件，总尝试 2 次；后续 NV12不重新尝试硬解 |
| 无效设备 999 | 自动模式遍历候选后软件；强制 CUDA/D3D11 明确报错，无默认适配器替换 |
| 强制 GPU、强制不支持格式、未允许 Main10 | 明确失败；无静默回退；端口和已分配 GPU 资源可回收 |

每组都检查真实新画面、停止后的线程/端口、GPU 源与纹理归零。C# 主界面另用 `--decoder auto --transfer auto` 在 Mobile/Vulkan 中运行，验证选项、实际路径与回退原因。GPU/CPU 切换和后端切换后的色条、灰阶与比例截图已目视检查。

`backend_attempts/backend_failures/backend_switches`、`gpu_attempts/presentation_fallbacks` 与 `fallback` 用于解释实际路径。若构造时所有候选均失败，实例尚未建立，计数为 0，完整失败链保存在 `error`，不把不存在的设备显示为实际设备。

本阶段不新增持续 1080p60 的性能声明，沿用 M4 的实测限制。Windows AMD、原生 Ubuntu Intel/AMD/NVIDIA及物理驱动移除/挂死未实测；WSL不替代原生 Ubuntu。长时间稳定性、最终包和许可证材料在 M6 完成。
