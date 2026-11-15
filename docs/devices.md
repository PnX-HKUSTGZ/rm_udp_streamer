# 解码后端与设备身份

`native/src/media/device.*` 负责设备枚举、选择、创建与实际身份核验；`Decoder` 负责码流解析、实际硬件格式协商和帧输出。这两个模块均不依赖 Godot。`RmVideoSession.capabilities()` 返回枚举结果，不能把设备可见当作实际码流可解或 GPU 直传可用。

| 入口 | 含义与稳定范围 |
| --- | --- |
| `0` 至 `999` | 当前后端的枚举序号；不保证跨后端或重启后指向同一显卡 |
| `cuda:GPU-xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx` | CUDA UUID；先解析当前序号，再核验实际 CUDA context 的 UUID |
| `d3d11va:LUID=high:low` | DXGI LUID，两个部分均为十进制；仅在当前系统启动期间有效 |
| `vaapi:pci:0000:c6:00.0` | 当前 PCI 拓扑中的设备位置；插槽或拓扑改变后应重新选择，不是显卡序列号 |
| `/dev/dri/renderD128` | 显式 render node，节点编号可能变化 |
| `vaapi:/dev/dri/by-path/pci-0000:c6:00.0-render` | 系统提供的 render-node 链接；解析后核验实际打开的字符设备与 PCI 身份 |

CUDA UUID 也可省略 `cuda:` 前缀。显式后端与身份所属后端不一致时拒绝配置；身份不存在时不静默选择设备 0。`decoder=auto` 搭配带后端的身份时只尝试该后端，失败后允许软件回退，并保留原因。

没有限定身份时，Windows 的自动候选仍为 CPU 传输下 D3D11VA → CUDA → 软件、自动传输下 CUDA → D3D11VA → 软件。Linux 为 CUDA → VAAPI → 软件。每个候选每会话最多尝试一次；依赖中没有编入的后端也会明确记录失败原因。

Linux VAAPI 使用明确的 DRM render node 创建 FFmpeg DRM 设备，再派生 VAAPI 设备。实际打开的 fd 必须匹配选择时的设备号和 PCI 地址。VAAPI 保留源 DRM 设备引用；帧池和 `FrameLease` 延长整条引用链的寿命，回读结束后才释放 VA 显示和 DRM fd。

VAAPI 首个实现使用硬件解码加 CPU 回读、YUV/RGBA 上传。`transfer=gpu` 仍要求显式 CUDA 和可用的 CUDA/OpenGL 桥接。Main10、颜色、裁剪和错误恢复沿用既有约束，不能通过切换后端绕过。

`capabilities()` 的 `compiled` 仅说明固定 FFmpeg 中存在 HEVC 后端；设备条目的 `error` 表示枚举/访问检查结果。`gpu_transfer_candidate` 仅用于筛选候选路径。实际 profile、硬件帧、回读和渲染互操作必须在接收真实输入后验证。

VAAPI 需要通过 `build/build_ffmpeg.py --vaapi` 生成并记录独立依赖锁，随后用该锁恢复缓存和构建；旧官方 Linux 依赖锁仍只编入 CUDA。Ubuntu 开发依赖为 `libva-dev libdrm-dev pkg-config`，运行需要 libva/libva-drm/libdrm 和对应 GPU 媒体驱动。使用新依赖不代表整机或性能已通过验收。

R4 实机及性能矩阵仍在验收。已取得原生 Ubuntu Intel N250/AMD Radeon 780M 的产品核心像素一致性、回读和帧引用寿命证据，以及 Windows CUDA/D3D11 设备选择证据；完整桌面、故障、性能和最终包证据以阶段报告为准。
