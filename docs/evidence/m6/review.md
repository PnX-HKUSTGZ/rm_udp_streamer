# M6 独立审查

结论：**PASS（M6 技术验收，本轮 Windows/WSL 矩阵）**。审查日期：2026-11-15；审查员：用户指定的 gpt-6-astra。未发现阻塞问题。允许完成下文限定的文档刷新与压缩归档；本报告不声称尚未执行的 ZIP/校验文件交付已经完成。

审查对象：两平台产品 HEAD `669a4a6310f3ea6c83b9497158843de2a9727d07`，均为干净工作区。最终 D3 包运行提交 `c8dcaa00c17cf6008d75b3c55b6ef9e41c6a0a72`，长测候选提交 `84bbd2e1a77b3aac97c3127bb96421f105eaeb22`，运行实现核心 `b8bde42`。未修改产品、未提交、未覆盖实现方证据。审查脚本及新结果位于外层 `processing/2026-11-15-m6-review`；Linux 原生执行位于 `/home/lqx/code/processing/2026-11-15-m6-review`。

## 审查依据和独立工作

阅读 implementation-plan.md 的 M6/稳定性/发布门槛、m6-release.md、building.md、validation.json、M0–M5 审查结果，以及固定构建、导出、许可组装、源码归档和重链接脚本。M0–M5 的 PASS 不被当作 M6 的替代证据。

1. 在两个最终包上重新执行完整文件审计：Windows 466 个、Linux 456 个清单文件的字节数及 SHA256 全部匹配，无额外未登记文件；自包含 .NET 8.0.34、原生库依赖、许可清单、源码包和重链接材料摘要检查通过。独立结果为 `windows-final-audit.json`、`linux-final-audit.json`。
2. 分别重新运行 20 秒真实 C# 主 UI，在空工作目录、PATH 无 SDK、DOTNET_ROOT 指向不存在目录、DOTNET_MULTILEVEL_LOOKUP=0 的条件下使用正式 1080p60 HEVC 发送器。Windows 使用真正 GUI 可执行文件，CUDA/OpenGL GPU 路径：1145 次解码、1033 次新纹理提交；WSL 使用 CUDA/CPU NV12/llvmpipe：1081/1077。两者 exit=0、errors=[]、worker_done=true、render_pending=0，退出后可重绑端口。Windows 视频像素回读为0，所有 GPU publication 完成退役。截图已独立目视检查，实际画面、比例和路径显示正常。结果为 `*-independent-smoke.json`、对应 NDJSON、截图和日志。未重新跑完整31分钟。
3. 独立复算两个原始长测 NDJSON（1756/1835条）和原始 OS 采样（186/187条），逐条核对停止记录/末尾记录与摘要一致；校验 validation.json 登记的82份原始证据摘要，全部一致。重新计算持续速率、内存分窗、线程/句柄范围、预算及GPU计数，输出 `independent-evidence.json`，最终 errors=[]。
4. 独立重复运行工具链 verify-only：Windows GCC15.2.0/CMake4.1.3、Linux GCC13.3.0/CMake3.28.3，两者 .NET SDK8.0.428、固定 godot-cpp commit 和三个 FFmpeg 静态库摘要均匹配。审查最终包的 relink_native.py，并分别实际运行：两平台均成功从随包对象/库生成新的 DLL/SO，文件只写审查目录。该项证明最终材料可链接；修改 FFmpeg 后真实播放的证据另见下文，未将“链接成功”充作“播放成功”。

## 门槛与证据结论

**长时间运行与自然回绕：通过。** Windows持续发送1869.58秒/112175帧；Linux1869.25秒/112155帧。发送器从0开始，每个平台在约1092秒自然经过65535→0；全部记录 restarts=0。180秒后的解码/新纹理速率分别为 Windows 60.00/55.34、WSL 59.96/59.30 帧/秒，与报告一致。这是稳定性结果，不代表持续1080p60性能目标已经达成。

**内存、队列和资源：通过本轮观察窗口。** Windows预热后的连续5分钟私有提交均值约678.01、680.05、677.01、678.88、677.46 MiB，最后不足5分钟约688.41 MiB，但持续周期回落，末尾采样约653.68 MiB；原报告首尾完整窗口差约2.63MiB，未表现随帧数持续增长。Linux匿名驻留分窗均值268.20、283.38、287.17、288.87、289.72 MiB，最后约3分钟精确保持289.742 MiB；前段上升后形成平台。两者线程/句柄稳定。采样中的最大待重组/解码队列分别约59/70KB、33/59KB，远低于预算，稳态无不断增长的队列。Windows停止时103450次GPU复制、源释放、消费者fence、退役相等，live sources/slots均0，视频回读计数与字节均0。此结论限定31分钟及当前设备，不推导无限期无泄漏。

**真实生命周期：通过。** Windows采用 `windows-lifecycle-process-v2.json`，60轮每轮30次真实GPU提交，总1800次；覆盖 clear/free、窗口尺寸变化，逐轮worker退出、render_pending=0、端口重绑及全部GPU计数/零live资源检查通过。Linux `linux-lifecycle.json` 60轮交替CPU YUV/RGBA，每轮29–31次真实提交，总1836次，逐轮退出与端口检查通过。实现方早期Windows采样console启动器的结果不用于进程内存结论；32帧冷启动无GPU提交的试验也不作为通过证据。连续发送至真实提交的新harness消除了先前缺省计数误判，修正充分。

**故障和恢复：通过。** 两平台原始故障采样覆盖缺帧、损坏AU、2.5秒断流清屏、换源、尺寸/范围变化、突发过载与后续播放恢复，随后各18轮模式切换。过载前后 queue_dropped 分别增加1748/1780，恢复后仍有新画面；Linux过载阶段短于100ms采样间隔，没有命中该阶段标签，但实际队列丢弃突增与发送脚本/日志证明注入发生，不能仅凭缺少标签判作未执行。最终格式为1280×720，Windows19/WSL18次恢复、各1次输入解码错误。GUI端口占用分别明确报告OS error10048/98，exit=2、worker_done=true、render_pending=0；不采用不支持的headless尝试。后端失败/格式回退仍依据已独立通过的M5，不冒称物理驱动移除已测。

**许可、对应源码和可重链接：通过本轮发布材料检查。** 包含实际Godot引擎第三方清单（102项/19份许可正文）、godot-cpp、.NET、FFmpeg、nv-codec和编译器运行库声明。FFmpeg配置禁用GPL/nonfree；随包完整源码、配置、应用对象与静态库、重构建和重链接脚本齐全，没有禁止LGPL调试所需逆向/修改的随包EULA。独立将FFmpeg归档10140个文件、nv-codec归档10个文件逐个计算Git blob摘要，与固定提交的完整树比较，零缺失/新增/不一致；Windows与Linux源码清单相同，Linux审计亦核验归档摘要。实现方修改 `av_version_info()` 后的原生重构建日志、重新链接脚本和实际独立主UI记录互相对应，两平台运行时确实报告 `n8.1.3-relink-validation` 并有547/473、549/543解码/提交和正常停止。正式包与独立复跑均仍报告原始n8.1.3，未混入修改版。

**长测与最终包连续性：通过。** 独立重复比较候选/最终运行源码与工程选项，无差异；Linux原生库字节完全相同，Windows原生库在仅归一化COFF时间、export时间及PE checksum后字节完全相同，其余代码/数据相同。C#程序集/PCK重新导出未被误称字节相同，两平台最终实际主UI独立收流补充了这一差异的运行验证。b8bde42之后的构建调整为运行库/SDK固定、静态系统运行库链接和strip，已包含在长测候选中；没有把旧构建结果直接等同新的未经测试核心。

## 审查过程中排除的错误假设

独立检查脚本初版曾把“Windows每轮至少30次提交”错误扩展到Linux，并要求100ms采样必须命中过载标签，产生两条检查告警。核对原始Linux首轮29次提交及过载前后queue_dropped 0→1780后，按真实M6门槛修正检查；初版结果保留为 `independent-evidence-initial.json`。这两项均不是产品失败，也不是删除不利原始证据。

## 必须完成的归档收尾与边界

已阅读尚未执行的 finalize_delivery.py/check_delivery.py。接受以下限定收尾，不需为纯文档/容器变化重跑31分钟：

- 将本审查及独立证据归档，更新阶段状态，保留所有未验证设备及性能限制；两平台同步该文档提交。
- 仅刷新包内docs、CAPABILITIES.md、README.txt及manifest证据元数据。保留运行commit=c8dcaa0；evidence_commit单独指向归档文档提交。先核对原manifest，再逐一证明全部非文档交付文件SHA256未变。
- 分别生成Windows/Linux ZIP，校验压缩文件每个成员的字节数和摘要、无遗漏/额外文件；Linux ZIP保留执行权限。汇总两个ZIP的SHA256并核对复制后的Linux归档一致性。
- 如任何运行文件、依赖、对象/库或源码配置发生变化，停止把本PASS直接应用到变化后的包，重新评估相应测试。

归档脚本目前的非文档摘要保护、文件集合检查和ZIP逐成员校验足以执行上述步骤。实际交付完成状态应由这些步骤的成功记录确认，不由本审查提前宣告。

支持范围限Windows本机Intel/NVIDIA和本机WSL Ubuntu-24.04/WSLg矩阵。原生Ubuntu、AMD、VAAPI、其他GPU互通组合和物理驱动拔除/挂死没有新增验收；不将本PASS推广到这些组合，不声明所有路径持续1080p60。
