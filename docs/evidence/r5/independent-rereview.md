# R5 独立复审：PASS（仅 R5 范围）

复审日期：2026-11-16。运行源码：`88bf17a9e046db6d75d3301e09d893fb1e8f4ed5`；证据文档：`ee4eab8c8487ec01b3c25ac6897fe5be67735b4a`。首次审查的 `review.md` 保留原 FAIL 结论，不覆盖历史。

**R5-F1 已修复并通过受影响验证；结合首次审查中已核实的其余 R5 项，本阶段复审 PASS。** 这是构建迁移与使用体验范围的结论，不表示 R3/R4/R6 或完整项目目标完成，也不将04737bd旧性能结果推广到88bf17a新程序集/包。

## R5-F1 闭环依据

生产 `scripts/ui/Main.cs` 新增 `_lastPrepared`/`_prepareFps`，从真实快照 `prepared` 增量除以单调时间计算“准备就绪”速率。`decoded` 和 `submitted_total` 分别保留自己的字段和基准，没有重命名解码数或使用UI刷新次数冒充准备数。开始时基准和速率清零，停止后计数静止、后续窗口归零，指标标签启用自动换行。

核对 `MediaSession::complete_preparation` 和GDExtension绑定确认 `prepared` 是通过代际检查并放入呈现槽的就绪结果。`docs/usage.md` 正确解释CPU回读/整理完成与GPU源帧引用可供桥接的差别；不声称GPU复制、消费者使用或物理屏幕扫描已完成。

外部探针仅在生产Main中插入调用入口，沿用真实更新代码、原生库和真实UDP HEVC输入。它暂停呈现消费，而非篡改计数。三个原始 `ui-result.json` 证明：

| 实际路径 | 暂停消费时的原生快照 | 界面对应结果 |
| --- | --- | --- |
| Windows CUDA / GPU / OpenGL | decoded=53，prepared=53，submitted=0 | 准备约51.5fps，提交0fps |
| Windows D3D11VA / CPU YUV / Mobile | decoded=57，prepared=1，submitted=0 | 准备约0.9fps，提交0fps |
| Ubuntu AMD VAAPI / CPU YUV / Wayland / Mobile | decoded=52，prepared=1，submitted=0 | 准备约1.0fps，提交0fps |

CPU只有一张待消费准备结果时解码仍在继续，进一步证明准备显示没有复制解码计数。恢复消费后三条路径均实际出图；停止输入后“画面信号过期”与“视频信号已中断”出现，准备/提交速率归零。正常停止、资源终态、再开始基准清零、第二次真实播放均通过；runner记录退出0、无强制清理、端口重绑成功。

独立查看了三条路径各四张1100×720截图（准备未提交、播放、过期、再开始，共12张），新增速率、设备与状态文本均完整可见。Windows GPU再开始截图的2.8/1.8fps是包含初始化的最近UI统计窗口；原始快照已准备60帧、提交58帧，不能将该截图当成稳定性能结论。

## 新包与证据核对

- 当前索引 **50项全部大小和SHA256匹配**，逐项记录见 `rereview-receipt-integrity.json`。其中包含原始UI快照和截图，不只是完成声明。
- Windows `release-windows-v3` 从干净88bf17a检出导出，完整512成员、隔离SDK启动/停止、实际原生重链接通过。原始/重链接包各D3D11VA和CUDA/GPU四项实际硬解播放通过；本次逐份核对原始result摘要及其最终 `stopped`、exit0、worker_done、render_retired。
- 原生Intel Ubuntu `release-linux-v4` 从相同提交导出，完整476成员、隔离SDK启动/停止、实际重链接通过。AMD新包由严格核验旧包后在新目录应用已验摘要差分，再按完整新清单重新验证全部成员；没有放宽成员校验。原始/重链接包实际VAAPI播放通过。
- Windows新包对应原生DLL的实际Mobile→OpenGL跨进程重启，PID从33880变为16760，保持完整CUDA UUID、连接/来源/传输/节奏、Windows显示后端及screen0，不自动接收。其重启截图亦已独立查看。Linux渲染器重启代码本次未改，首次审查的Wayland/screen0设置保持证据仍限定原测试范围。
- Ubuntu生产/探针编译原始日志均0警告、0错误，Intel新导出日志亦0警告；Windows准备和渲染重启探针编译0警告。Windows新export.log含实际Godot导出成功记录，但未包含完整原生编译输出；本次原生代码、构建逻辑及依赖锁未变，既有原生诊断处理仍由首次审查证据支持，不伪称已从此日志重新核对每条原生编译输出。

| 新包对象 | SHA256 |
| --- | --- |
| Windows manifest.json | `37519b1c9851bbdcb9cee96cb97623b28b70a33d5c244c18c9fcd0de4be9ebec` |
| Windows 原生DLL | `10cff3de5321e2480224713a5eea744cb2593a7b8f77c48c790dac2b60009703` |
| Windows 重链接DLL | `2b2b3061e7c75708d7eb958866b425cd9170e6a2048001ffdd860a7836b50818` |
| Ubuntu manifest.json | `55c7fc12151d779475454a62534d0622e7f4023c7fbba5a018032b638fb729a5` |
| Ubuntu 原生SO | `32af73a97a4dc73c7d9388524406a253a36e81bb52218ec880bd1a2575e56cc5` |
| Ubuntu 重链接SO | `6caee21b719ffc5b07b83d8dbbbae65664c888bdd8dbd05ac41ff7e60ab9dab4` |

## 本次独立执行

仅在空闲Intel原生主机执行轻量复核，目录为 `~/qixuan/processing/2026-11-16-rm-r5-review/rereview-88bf17a`：

1. 核对v5 ZIP SHA256，解压到新的验证目录并验证26个清单成员。
2. 严格验证 `release-linux-v4/pristine-package` 的476个成员及88bf17a提交身份，核对自包含.NET 8.0.34配置。
3. 使用不存在的SDK根、最小系统PATH、隔离用户设置启动3秒，实际进程正常空闲停止，exit0、stop_terminal和worker_done均通过。

本地结果为 `rereview-independent-intel.json`，脚本为 `intel_rereview.py`，完整启动日志留远端本任务目录。此复核不宣称独立重测硬解或性能。

## 失败记录与边界

v1探针误要求CPU在已有单张准备结果未消费时持续准备；v2改为真实prepared=1/submitted=0、decoded继续，符合原生容量设计。v3相对v2仅增加等待真实UI刷新到过期占位文案，没有改变生产阈值或性能统计。原失败保留，修正后的探针没有伪造准备/提交数据。

首次审查确认的依赖恢复、完整输入及路径安全、独立developer锁、实际依赖重建、来源过滤、能力/完整设备身份和可迁移外部入口继续成立；本次产品差异仅Main.cs和文档。Linux新SO摘要与旧包不同，即使代码节比较成立也不等于二进制相同，更不等于新托管UI已完成正式长测。

本次未在Windows/AMD编译或启动测试，未读取活动metrics；只核对已结束R5材料和小收据。未修改产品、未暂存或提交，保留四个外部C#空白变化及UID。

未验证边界沿用首次报告：Ubuntu24.04基线、未执行默认网络godot-cpp获取、Windows符号链接创建权限跳过、screen0以外多显示器未全面覆盖、自包含不消除系统运行依赖。真实图传、其他硬件、正式性能/诊断开销和完整集成由各阶段单独验收。

**最终结论：R5 PASS；R5-F1关闭，无剩余R5确定阻断项。**
