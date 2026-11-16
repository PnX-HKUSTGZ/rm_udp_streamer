# 88bf17a样本候选的集成证据

本报告记录已结束的样本和设备验证，不宣称R6阶段完成。运行源码固定为`88bf17a9e046db6d75d3301e09d893fb1e8f4ed5`；后续文档提交不改变被测运行文件。R0、R1、R2、R3、R5已独立PASS，[R3独立审查](../r3/report.md)覆盖三条已测组合；R4/R6仍缺部分硬件和真实RoboMaster图传证据。完整能力边界见[支持矩阵](../../support-matrix.md)，构建与原始/重链接包来源见[R5报告](../r5/report.md)。

## 正式测量和诊断对照

使用固定1080p60 HEVC 8位SDR样本，每条正式路径180秒预热加1800秒有效输入、119040输入帧及一次自然16位回绕。门槛为均值≥59.4fps、完整秒窗P10≥58、首片到纹理提交P95≤50ms/P99≤100ms；应用可绘制且报告刷新率≥60Hz。三条路径均正常退出，验证错误为0。

| 实际路径 | mean / P10 fps | 首片→提交P95 / P99 | 诊断CPU增量点估计 |
| --- | ---: | ---: | ---: |
| Windows RTX 5070 Laptop / CUDA / GPU / OpenGL | 59.997222 / 60 | 30 / 32ms | -1.319611% |
| Windows Intel Arc 140T / D3D11VA / CPU YUV / Mobile | 59.998333 / 60 | 32 / 34ms | -2.621155% |
| Ubuntu 24.04 Radeon 780M / VAAPI / CPU YUV / Wayland / Mobile | 60 / 60 | 33 / 42ms | +0.737258% |

诊断对照均预先规定off/on/on/off，每轮30秒预热加300秒有效输入。相邻配对分别为CUDA +2.581862%/-4.941918%、D3D11VA -4.841970%/-0.250876%、AMD +0.770200%/+0.704508%。均值点估计满足预定2%目标；每状态两轮不能建立2%置信上界，负值也不表示诊断带来加速。全部预定轮次保留。

Windows CUDA正式输入一次迟到27.036734ms，原样保留。Windows原诊断组因状态文件写入失败在两轮后中断，补齐后的点估计-0.886314%及1029.207秒停顿另行保留；第一次接续的长路径复制失败也保留。连续主比较在读取CPU比较结果前另行声明，不按结果挑选轮次。D3D11VA补充四轮同样事前声明，第一轮P10=59、其余60，全部新主组超过一帧发送迟到和验证错误均为0。

六个300秒资源窗各有300个进程样本。Windows CUDA后期RSS约443–444.5MB、线程57/句柄855中位；D3D11VA后五窗RSS中位828.78–828.83MB、后四窗线程80/句柄1135中位；AMD末三窗RSS中位486.67MB、线程50/fd77。路径同时改变解码、传输和渲染，不能把资源差值单独归因于解码器。Windows持续接电、报告165.0013Hz；AMD报告60Hz且桌面未锁，但Mutter电源属性有0/3，物理扫描未测量。上述延迟终点是纹理提交。

收据：[Windows正式及两组CUDA对照](windows-performance.json)、[D3D11VA完整对照](windows-d3d11va-diagnostics.json)、[AMD正式及对照](amd-performance.json)。

## 生命周期、故障与证据范围

Windows发行DLL的原版和逐轮写盘探针各完成60轮，最大停止49/42ms；AMD发行SO的两种探针也各60轮、最大停止17ms。每轮1080p和720p各至少30次新提交及主线程接受，覆盖传输模式、格式代际、窗口变化、清图/销毁、重复停止、端口重绑和视频资源归零。实际GPU呈现必须有非零复制及退役记录，资源字段真实存在且计数守恒。这是专用编辑器场景加载精确发行原生库，不是60轮发行GUI按钮测试。

原探针保留详细字典会增加记录占用，因此另做输入、断言、期限和轮数相同的逐轮写盘复核，完整详细记录仍保留。Windows静态内存后期约37.7–37.9MB、末两资源四分窗RSS中位432.04/432.37MB；AMD静态内存约43.7–43.8MB、后期RSS约495MB。有限窗口不能证明不存在任何泄漏。Windows两次启动最初约16/15ms各一次无窗口观察保留，之后实际播放均有完成绘制。

实际发行包另有乱序、重复、丢片、冲突、损坏HEVC、断流清旧图、换源/尺寸/range、过载及恢复检查。Windows六项GPU故障使用对应源码的外部注入副本，与未注入发行DLL分列。上游关键帧等待和本地恢复时间分开记录，不跨时钟域相减。Intel ASan/UBSan只覆盖逐字节相同的应用核心输入；FFmpeg、驱动、Godot及发行二进制本身未插桩。Windows旧/新DLL归一化后仍有40字节代码差异未解释，因此按新DLL实际摘要和实测判断，不声称二进制等价。

收据：[Windows两种生命周期](windows-lifecycles.json)、[Windows故障与恢复](windows-gpu-faults.json)、[AMD功能与生命周期](amd-functional.json)、[AMD原探针内存说明](amd-original-probe-memory.json)、[AMD写盘复核](amd-streamed-lifecycle.json)、[核心插桩输入映射](native-sanitizer-input-scope.json)。

## 身份与归档

| 对象 | SHA256 |
| --- | --- |
| Windows原包manifest | `37519b1c9851bbdcb9cee96cb97623b28b70a33d5c244c18c9fcd0de4be9ebec` |
| Windows发行DLL | `10cff3de5321e2480224713a5eea744cb2593a7b8f77c48c790dac2b60009703` |
| Ubuntu原包manifest | `55c7fc12151d779475454a62534d0622e7f4023c7fbba5a018032b638fb729a5` |
| Ubuntu发行SO | `32af73a97a4dc73c7d9388524406a253a36e81bb52218ec880bd1a2575e56cc5` |
| 双平台托管程序集 | `1214eb533c807498b78f93a14359e31790a1644ccfbeca38d9542ab9dd1a3d52` |

[不可变收据索引](evidence-index.json)记录逐字节复制的14份摘要、大小、SHA256和工作区原始入口。完整Windows记录归档29620304字节、SHA256 `ff31ef4342720ff80e4ec2ecbcbb858d849007a201b009b5c3d88983b4f5f371`，含489份记录，原始失败、调度清理和脚本均保留，运行二进制与重复构建缓存另存。[Windows归档收据](windows-records-archive.json)记录范围。

AMD正式/诊断、功能、逐轮写盘生命周期和历史04737bd四份完整归档均已取回并校验，详见[取回索引](amd-retrieval-index.json)及[性能](amd-performance-archive.json)、[功能](amd-functional-archive.json)、[生命周期](amd-streamed-lifecycle-archive.json)归档收据。本目录只发布摘要，完整原始归档保存在外层`processing/2026-11-16-rm-r6-integration`，提取记录子集不替代完整原件。

后续[Intel接屏补验](../r4/report.md)已完成实际桌面功能、故障/恢复及60轮精确发行SO生命周期，但该机88bf17a包未达到1080p60门槛。[真实UDP输入](../r4/real-stream.md)已完成实际硬解与画面检查，随后完成自然回绕与三条路径的真实输入回归；原始失败均保留。用户明确发送端固定，验收本侧代码，不将型号/固件或修改发送端列为前提。新回归又复现同来源断流后参数被错误清除的缺陷，修复与最终包验证见[真实输入回归](../r4/real-input-regression.md)。未完成门槛仍包括最新运行身份的功能/性能复验、Intel性能、原生Ubuntu NVIDIA、Windows AMD及完整R4/R6审查。其他RGBA、CUDA/CPU、WSL及未做正式长测的配置没有同等1080p60性能声明。
