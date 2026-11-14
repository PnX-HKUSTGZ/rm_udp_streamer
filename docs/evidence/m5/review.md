# M5 独立审查

结论：**PASS**。审查日期：2026-11-15。无阻塞发现；允许进入 M6，但不构成 M6 或 D3 完成。

审查对象：Windows `c2bb0c89d1635775b91c3406afb31e2f026ee06c`，产品实现 `b8bde426ab11d8f734b8d5bc524e0d8d85a6babe`；WSL Ubuntu-24.04 独立检出为后一提交。Windows 产品工作区干净。本审查未改产品代码、未提交。

## 审查范围与判定依据

已阅读 implementation-plan.md、m5-backends.md、validation.json，并审查 decoder.cpp/.hpp、session.cpp/.hpp、video_surface.cpp、video_session.cpp、cuda_gl_bridge.cpp 及 Main.cs/RuntimeOptions.cs 变更。

- 每会话候选由单调 next_candidate_ 限制，恢复/格式变更不重置游标；指定后端只有一个候选，不静默返回软件帧。
- get_format 无法再提供硬件格式时记录 Backend 错误；输入错误走关键帧恢复，显示策略 Unsupported 不参与换后端。回退在 feed 栈退出后执行，不重放已失效 parser 输出。
- CUDA 身份来自实际 FFmpeg context 的设备名/UUID。D3D11 创建前枚举，创建后核对实际名称/LUID，999 不允许被默认设备替换。
- 自动 GPU 失败保留健康 CUDA；gpu_disabled 在会话内单向变化，presentation_generation 隔离旧 GPU 帧。发布前后与主线程显示均校验门禁。
- surface 先清空 canvas/material/shown 引用，再按渲染队列退役 bridge；CPU 上传在退役之后。退役失败保留资源并阻止重复 GPU 分配，不伪装安全成功。
- AVPacket 作用域析构覆盖 receive/send 异常；候选耗尽路径更新诊断再抛出。
- UI 自动传输选项与命令行映射一致，实际后端/回退原因继续显示。

原始 Windows 16 + WSL 11 组证据以及 2 组最终补丁回归共 29 份文件的 SHA256 全部与 validation.json 相符。原始矩阵包含两块 D3D11 设备、CUDA、强制错误、显示策略拒绝及 C# 主界面证据；未把这些均称为本次独立复跑。

## 本次独立复跑

复制测试脚本/样本到本目录，使用真实图形窗口；WSL 在 /home/lqx/code 下独立运行，结果已复制回本目录。没有覆盖原始证据。

| 平台/用例 | 结果 |
| --- | --- |
| Windows combined-fallback | PASS：CUDA → D3D11 → software；候选 3、GPU 尝试 1，退役 114 个 GPU publication，最终 cpu_yuv420p |
| Windows decode-format-fallback | PASS：NV12 → 444 → gray → NV12；D3D11 → CUDA → software，候选总数 3，普通格式恢复后不重试 |
| Windows gpu-format-fallback | PASS：NV12 → 允许 Main10 → NV12，始终 CUDA、候选 1、GPU 尝试 1，退役 118，最终 cpu_nv12 |
| Windows d3d11-invalid-forced | PASS：明确 D3D11 adapter index 999 does not exist，无画面、无默认适配器替换 |
| Windows invalid-auto | PASS：CUDA 和 D3D11 失败后 software；总候选 3，失败原因明确 |
| WSL combined-fallback | PASS：CUDA → software，总候选 2、GPU 尝试 1；WSLg 互通错误明确，最终 CPU |

六组均 exit 0、errors []，停止后 worker_done=true；已分配 GPU 的用例 sources/slots 归零，端口重新绑定检查通过。Windows GPU 格式回退和 WSL 组合回退最终截图目视检查：色条/灰阶及比例正常。逐帧记录中的呈现代际检查通过。

## 结论边界

M5 的已测设备与自动/强制回退门槛成立。未测 Windows AMD、原生 Ubuntu Intel/AMD/NVIDIA、物理驱动拔除/挂死仍为待验证，WSL 不代表原生 Ubuntu。没有新增稳定 1080p60 声明。

M6 仍须完成至少 30 分钟稳定性、50 次有意义的生命周期、发布包/依赖/完整许可证与重链接材料及干净环境启动验证。不得把本次短矩阵或 M4 成果代替这些门槛。
