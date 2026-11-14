# M4：CUDA / OpenGL GPU 视频路径

2026-11-15。核心实现 `bfd2334`；本阶段尚待独立审查。D1 的 CPU YUV/RGBA 路径继续可用。

## 支持范围与使用

本机已验证 Windows + NVIDIA RTX 5070 Laptop、驱动 591.74、Godot 4.7.2 Compatibility/OpenGL 3.3、GL_ARB_copy_image、FFmpeg n8.1.3 CUDA/NVDEC。要求解码与 OpenGL 为同一物理 GPU，实际帧为 NV12 8 bit、裁剪起点为偶数，颜色规则沿用 [M2](m2-media.md)。显式 GPU 模式不做静默 CPU 回退；自动策略在 M5 完成。

```text
RmVideoReceiver.exe --rendering-method gl_compatibility -- --start --decoder cuda --transfer gpu --bind 127.0.0.1 --port 3334
```

界面显示的实际传输必须是 `gpu_cuda_gl_nv12`，仅选中 GPU 选项不代表成功。Mobile/Vulkan 会明确拒绝。Main10 即使显式允许 8 位转换，GPU bridge 仍明确拒绝其 P010 表面；可选择 CPU RGBA。Windows 其他厂商、原生 Ubuntu 的 GPU 组合未验证。本机 WSLg/llvmpipe 的 CUDA/GL 设备查询返回 `CUDA_ERROR_OPERATING_SYSTEM`，按不支持处理；WSL CUDA CPU 中转回归为 599 解码/597 提交。

## GPU 资源与同步

1. 解码线程仅传递持有 AVFrame 的原生租约，不调用 CPU 回读或 swscale。渲染线程验证真正的 CUDA 帧、上下文和当前 OpenGL 设备。
2. 固定最多 3 个显示槽，每槽有私有 CUDA 注册纹理和独立 Godot 显示纹理，分别是 R8/RG8 两平面。CUDA 在 FFmpeg 的同一个流上将可见区域复制到私有纹理，因流内顺序位于解码异步输出之后。
3. CUDA unmap 保证后续 OpenGL 工作在 CUDA 写入后执行。记录 CUDA event，通过非阻塞查询确定源 AVFrame 可以释放；不以 CPU 回调返回或经过若干帧判断完成。
4. `glCopyImageSubData` 将私有平面复制到显示平面；同一 GL 命令队列保证 Godot shader 在复制后采样，完成 NV12 → SDR RGB。两次都是 GPU 内复制，允许非零 GPU 复制字节数，视频像素 CPU 回读仍为 0。
5. 替换/清除画面时先显式清空 RenderingServer 的 canvas 命令和 shader 平面引用，再释放呈现租约。渲染队列随后插入 GL fence，只有 fence 已完成且 CUDA 源已释放才复用槽。格式变化必须等待槽安全退役才重建尺寸。
6. 停止时有界等待这些具体事件/fence，最多 2 秒。失败则保留资源并禁止本进程继续建立 GPU bridge，提示重启应用；不能强制释放在途资源。正常停止后 render_pending、live slots 和 live sources 均为 0。

不调用 `cuCtxSynchronize`、`glFinish` 或全局 `RenderingDevice.sync()`，也没有正常逐帧 CPU 等待 GPU 完成。槽不足时丢弃尚未提交的显示候选。1080p 三槽的私有+显示平面约 18.7 MB，FFmpeg 参考帧池另计。

私有纹理是必要的互通边界：NVIDIA 禁止 Godot 在仍注册给 CUDA 的纹理上修改部分采样参数；直接共享注册纹理的初版产生 GL_INVALID_OPERATION。逐帧注销可以避免错误但增加开销，因此最终采用上述 GPU 内复制，Godot 只接触未注册给 CUDA 的显示纹理。

## 实测与限制

- 真实 HEVC 色条/灰阶/非方形像素比，对比 FFmpeg 参考：BT.709 最大采样通道误差 3/255，全范围 1/255，SAR=2 为 3/255。最终实现的 Godot GPU 验证日志无 GL ERROR。
- 缺帧、损坏码流、2.5 秒断流、换源、640×360 ↔ 1280×720、全范围、1800 组过载后均能恢复；一次完整链记录 23 次恢复、1 次输入错误，CPU 回读为 0。
- 18 次实际视频启停与 GPU/YUV/RGBA 切换，同时调整窗口尺寸；另 12 次直接销毁视频控件，交替在绘制前/后销毁。每次均检查源、显示槽、在途工作和端口释放，全部通过。
- 强制 Vulkan GPU 模式以及播放 NV12 后切换 Main10 均明确失败，未悄悄回读，仍可安全退役。WSL GPU 不支持错误和 CPU 回归分别验证。
- 固定 M0 1080p60 样本、600 组、同一 Compatibility 渲染器，CUDA CPU YUV 为 599 解码/595 提交；GPU 为 599/560。所有 GPU 视频回读计数/字节为 0；560 次源释放、560 次消费者 fence 与退役匹配，停止后资源计数归零。GPU 路径在该短测中没有获得更高的新画面提交速率，不宣称持续 1080p60 或普遍快于 CPU 路径。

截图保存会产生一次整窗读回，用于 QA；该行为与视频传输的逐帧像素回读分开。30 分钟长期运行、50 次完整 GPU 生命周期、最终设备回退矩阵与发布材料仍分别在 M5/M6 执行。

原始材料位于外层 `processing/2026-11-15-m4-gpu`，正式 D2 包与后续审查结果在本阶段完成时追加到本报告。
