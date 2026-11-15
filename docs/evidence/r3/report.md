# R3呈现性能改进：独立审查PASS

2026-11-16，用户指定的gpt-6-astra完成[独立审查](independent-review.md)，结论为R3 PASS。运行源码冻结`88bf17a9e046db6d75d3301e09d893fb1e8f4ed5`，审查时文档提交`2adf8b9599ab78a474d945a2d5ccdaa4004563cd`。审查没有修改产品、降低原门槛或新增性能测量。

审查覆盖准备线程、最新帧选择、三种代际与epoch、硬件引用、CPU输出池、上传写时复制、GPU同步及失败终态，并从原始闭合秒窗、累计直方图和进程采样独立重算三条正式路径及全部诊断对照，逐轮核对双平台各两种60轮生命周期。代码、原始结果、故障副本来源及发布收据未发现阻止R3通过的确定缺陷。

| 已测组合 | 平均 / P10 fps | 首片→纹理提交P95 / P99 |
| --- | ---: | ---: |
| Windows RTX5070 Laptop / CUDA / GPU / OpenGL | 59.997222 / 60 | 30 / 32ms |
| Windows Intel Arc140T / D3D11VA / YUV / Mobile | 59.998333 / 60 | 32 / 34ms |
| Ubuntu24.04 Radeon780M / VAAPI / YUV / Wayland / Mobile | 60 / 60 | 33 / 42ms |

每条正式路径180秒预热加1800秒有效输入。诊断CPU增量点估计分别为-1.319611%、-2.621155%、+0.737258%，完整轮次、配对波动与中断组均保留；负值不是诊断加速证明，每状态两轮不构成2%置信上界。

不可变原件见[性能与生命周期收据](independent-receipt.json)、[来源与故障收据](supplement-receipt.json)和[摘要索引](evidence-index.json)。复算脚本与全部原始输入保留在外层`processing/2026-11-16-rm-r3-review`及各阶段证据目录，未纳入产品运行资源。

本结论只覆盖这三条1080p60、8位SDR固定HEVC样本组合及R3相关实现和回归。没有物理扫描或相机到屏幕延迟结论，其他RGBA、CUDA/CPU、WSL及未测组合没有同等性能等级。原生Ubuntu NVIDIA、Intel物理接屏、Windows AMD与真实RoboMaster仍属R4/R6未完成门槛，整个目标尚未完成。详见[支持矩阵](../../support-matrix.md)和[当前集成证据](../r6/report.md)。
