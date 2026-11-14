# M1 独立完成度审查

日期：2026-11-15；审查对象：9136f52（协议实现基线 9a185ba，新增首次导入构建兼容处理）。

结论：**PASS（M1 范围）**。可进入 M2。未发现需阻塞 M2 的协议/生命周期问题；首次 Linux 导入崩溃已有可复现、文档化的正式构建规避，并经独立干净检出复验。

## 独立执行的检查

- 阅读 `docs/implementation-plan.md` 的 M1 门槛及协议异常矩阵；审查 ProtocolCore、UdpReceiver、GDExtension 注册/配置/诊断、C# 调用与释放、CMake、正式构建入口。
- Windows 从生产源码重新编译既有协议检查程序至本审查目录，38/38 PASS。
- WSL 从 Linux 独立检出的生产源码重新编译，同样 38/38 PASS；启用 ASan + UBSan，未见内存/未定义行为报告。
- 两端真实 UDP 重组和核心检查输出摘要相同：`9b2514f310b89c1d`。测试包含乱序、重复、冲突、超长/短包、缺片、来源隔离、无新包超时、回绕、重启、预算、端口占用/重绑、20 次启停及持续收包时停止。
- 另写 extra.cpp，两平台独立通过 131100 连续帧（两次自然回绕）、8 MiB 最大帧逐字节一致、100000 包预算压力；每步检查 pending 字节/帧与 queue 字节上限，最终定时清空。Linux 同样启用 sanitizer。
- WSL `/home/lqx/code/processing/2026-11-15-m1-review/fresh-9136f52` 为全新 Git clone，脚本断言初始不存在 `.godot`。运行正式 `build/build_project.py --cache /home/lqx/code/.tools/rm-video`：exit 0；首个 editor import：exit 0，无 ERROR；真实图形 C#/GDExtension UI：exit 0、errors=[]、接收完整传输组 1 可见、停止后端口重绑成功。截图已人工查看。

## 判断与边界

- 长度/索引在分配前校验；重组数据与分片标记计入字节预算，map 节点和对象由帧数上限约束；输出队列具备字节/帧数/年龄上限。因此是硬有界缓存，但该预算不是进程总 RSS 限额。
- 单调时钟驱动独立 5 ms select 清理，持续数据使用有界批次，停止先 join 后关 socket，避免关闭中复用描述符。真实无新包清理与持续输入停止已复跑。
- 丢失整组时未来完整组也可能达到统一年龄上限被丢弃；这是低延迟约束而非无损等待承诺。超时/缺失与 discontinuity 均可观测。
- 自动来源学习允许空闲后重学来源与 generation；指定来源仍先过滤。未将其描述为网络认证。
- 9136f52 在正式构建后合并 `.godot/extension_list.cfg`，在启动阶段加载扩展，避免固定引擎首次扫描期间晚发现原生类的退出崩溃。已读取其保留已有清单项的实现。必须先正式构建再打开，删除 `.godot` 后需重跑构建；这是已文档化构建前提，不是宣称修复引擎。仅复制 SO 后首次导入的旧失败不得改写为通过。
- WSL 图形复验使用 llvmpipe/WSLg；不构成 GPU 解码、原生 Ubuntu 显卡、视频颜色/帧率、30 分钟稳定性或发布包验收。上述属于 M2 及后续。
- C# 只传配置与诊断；无大像素数组路径。M1 仅接收协议，尚无解码器和实际视频帧。
- 真实网络测试为各平台本地 UDP，未证明 Windows↔WSL 路由或外部设备网络；不影响本阶段跨平台相同重组实现验收。

## 可复查证据

本目录：windows-tests.log、linux-tests.log、windows-extra.log、linux-extra.log、extra.cpp、fresh_review.py、fresh-results.json、build.log、first-import.log、ui.log、ui-review.json、ui-review.json.png。

无生产代码修改。报告基于独立复跑，未以此前 PASS 摘要代替检查。
`n补充核查：主执行者提供的 Windows 9136f52 全新检出正式构建、首次导入、UI 报告已读取（../2026-11-15-m1-transport/fresh-windows-results.json 与 fresh-windows-ui.json），三步 exit 0、无引擎错误、UI errors=[]、port_rebound=true。这项由主执行者执行，本审查独立复跑的是 Windows 原生/UDP 与 WSL 全栈干净构建。
