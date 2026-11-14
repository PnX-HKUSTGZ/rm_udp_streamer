# M4 独立审查

结论：**PASS**，允许进入 M5。仅针对 M4 门槛，不代表 M5/M6 或全设备发布验收。

审查日期：2026-11-15。产品 HEAD 8c754ce9ef084163c13a72477e4cfcf816a59980；D2 实现 aa2753e，GPU 核心 bfd2334。未修改产品代码或提交。

## 审阅结果

已审阅 implementation-plan.md、m4-gpu.md、evidence/m4/validation.json，以及 cuda_gl_bridge.cpp/.hpp、gpu_api.hpp、video_surface.cpp/.hpp、video_session.cpp/.hpp、media/session.cpp、decoder.cpp 的回读入口和 C# Main/RuntimeOptions。

GPU 分支保留真实 CUDA AVFrame 并绕开 prepare_cpu；CUDA copy 的源/目标分别为 device pointer/CUDA array，没有 CPU 视频像素路径。最多三个槽，私有注册纹理与 Godot 显示纹理分离，避免 Godot 改变 CUDA 注册纹理采样状态。沿用 FFmpeg context/stream，unmap 保证后续 GL copy 的顺序；CUDA event 查询完成才释放源帧。显式清空 canvas 和 shader 引用后释放 publication，GL fence 完成且源释放后才复用/销毁槽。失败上传的 mapped/未记录事件状态在 close 中处理；无法安全退役时保留 bridge/session，设置进程级禁止重建标志。未发现 M4 阻塞性同步或所有权缺陷。

原始源帧租约、队列回调持有的 state/session 和停止计数形成连续所有权；停止不会仅因 decoder worker 结束就报告 GPU 已退出。正常逐帧查询为非阻塞，不使用全局 GPU 同步。停止的轮询等待有两秒界限（不意味着能中断底层驱动自身挂死）。

## 本次独立执行

1. 隔离复制 run_package.py 到本审查目录，在空工作目录、仅系统 PATH、无效 DOTNET_ROOT 下运行正式 Windows D2 主 UI，CUDA + Compatibility + GPU，真实发送色条 HEVC。退出码 0，errors=[]；665 硬件解码，660 GPU 复制/提交；视频像素回读次数/字节均 0。停止后源释放、消费者 fence、退役均 660，live sources/slots 与 render_pending 均 0，worker_done=true，端口可重新绑定。独立截图已目视检查，色条、灰阶、比例和实际 GPU 路径正确。
2. 隔离复制 run_destroy.py/destroy_probe.gd，独立执行12次收到真实 GPU 帧后直接销毁控件，交替在 frame_post_draw 前/后销毁。退出码0，errors=[]；资源和渲染工作归零，端口重新绑定通过；GPU validation 日志无 GL 错误。

证据位于本目录：windows-gpu-package.json、windows-gpu-package.json.ndjson、windows-gpu-package.json.png、windows-gpu-package-dotnet.log、destroy-result.json、destroy.log。原实现方证据未被覆盖。

## 验收边界

实现方的最终双纹理证据覆盖色彩/全范围/SAR、实际故障恢复与GPU/CPU切换、Vulkan/Main10明确拒绝，以及WSLg不支持和Linux CPU独立包。本次独立重新执行Windows包与直接销毁验证，未重新执行全部原矩阵，也未独立复跑Linux包。结合代码与已有证据，M4的首条可用GPU路径、零回读、同步、重复生命周期、图像正确与D2门槛满足。

性能报告诚实记录1080p短测 GPU 57.81新帧/秒，低于同条件CPU路径59.81；PASS不表示持续1080p60或GPU必然更快。原生Ubuntu/AMD未验证，WSL结果不代替原生Ubuntu；自动回退/设备矩阵属于M5，30分钟、50次生命周期及最终发布材料属于M6，必须继续完成。
