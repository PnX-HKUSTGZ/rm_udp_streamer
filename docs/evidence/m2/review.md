# M2 独立完成度审查

结论：**PASS（M2 阶段）**。最终审查实现提交 `be62ebd0b4bef8416d0253d23fa5fc146d11604a`，包含 `48a7ecf` 主实现及 `f4446b4` / `be62ebd` 颜色契约修正。两个阻塞颜色问题已经关闭，可以进入 M3。

审查范围为 implementation-plan.md 的 M2 门槛：Windows/WSL 中由正式 sender 输入并显示，颜色/比例/真实新帧计数正确，原生像素路径。不是 M3 恢复/发布、M4 GPU bridge、M5 设备矩阵或 M6 长时稳定性验收。

## 已完成独立检查

- 审查 decoder/session、FrameLease/PreparedFrame、Godot upload/surface、C# UI/interop。AVFrame 使用 shared_ptr 强引用；CPU prepare 在解码工作线程执行；压缩 parser 保持跨组状态且带零填充；解码器排空全部输出。
- CPU 邮箱只保留最新一帧，上传最多一个回调在途。回调持有自身 Ref、state 和 PreparedFrame；PackedByteArray/Image 在原生侧复制拥有像素。C# 仅传小配置和诊断，不接收像素数组。
- 硬解输出检查真实 hw_frames_ctx/format，CPU 回读可计数。显式硬解不静默给软件帧；auto 在初始化失败时尝试候选并记录原因。GPU bridge 未实现也未标成当前模式。
- UI 分开统计重组、解码、新画面提交；没有用窗口FPS冒充视频吞吐，没有伪造媒体时间戳/端到端延迟。

## 独立复跑证据

1. 最终修正后再次独立复跑 Windows 32/32 解码案例、WSL 16/16，使用实现阶段的原生检查程序但由本审查重新运行，日志/摘要另存本目录；120 帧规范化 YUV 摘要硬软一致；不规则组切分、no-AUD/B帧、SAR、Main10默认拒绝/显式RGBA、PQ/HLG拒绝全部通过。原始测试程序源码亦已审阅。
2. Windows plane_checks：负stride、可见偶数crop、奇数尺寸、NV12双通道以及parser预算均通过。
3. 双平台完整 C# UI + 正式 sender.py 的360组文件循环输入，使用新临时端口，通过无错误/可见画面/实际模式/停止重新绑定检查。Windows: 359 decoded、289 submitted；WSL: 299 decoded、295 submitted（CUDA准备错过首个IDR后等待下一IDR）。截图已亲自检查。WSL 渲染为 llvmpipe/WSLg。
4. 独立 Windows 图形补测：软件RGBA 359/354、采样最大误差0/255；SAR=2 的YUV 359/278、误差2/255、显示比3.556:1正确。颜色检查针对参考色条/灰阶内部采样，不声称全像素色度重采样验证。

## 审查发现及闭环

- BT2020/P3等原色在没有色域映射时被接受会产生静默错色。独立 bt2020-sdr.hevc 元数据契约样本在旧实现输出120帧；f4446b4 增加显式原色拒绝，已检查源码并复测软件路径拒绝。
- 同类显式LINEAR/LOG transfer需要拒绝或转换。独立 linear-sdr.hevc 在f4446b4仍输出120帧，已要求实现阶段修正；be62ebd 已增加明确允许列表，其余传递函数拒绝。最终独立颜色契约复测 Windows 16/16、WSL 8/8 全通过，覆盖 BT2020 原色及 LINEAR transfer × YUV/RGBA × 各平台可测软硬解后端。两个样本为重写 VUI 的契约测试，不作为真实宽色域图像参考。结果见 windows-color-contract-results.json / linux-color-contract-results.json。

## 必须保留的后续边界

- M3：错误恢复、参数变化/解码器重建、stream/resource代际精细门禁、停止/重启压力及独立导出。现有resource_generation仅按coded尺寸/AV像素类型变化，不能宣称完整格式切换验收；现有surface也未按资源代际全面拦截旧帧。
- M5：auto 收到实际不支持profile后的回退、CUDA稳定设备身份（目前cuda:序号）、主流厂商实机矩阵。
- M6：30分钟、50次启停、资源增长和干净环境包验证。独立短测也有提交数低于解码数，CPU中转不得宣称普遍1080p60。
- 未验证GPU无回读、外部表面同步、原生Ubuntu/AMD。未证明首包到屏幕延迟。

复测可复现入口：run_decode_checks.py、verify_color_contract.py、run_ui_sender.py、run_graphics.py。原生检查二进制来自实现任务的独立构建目录；本审查读取检查源码并独立执行，未修改产品源码。完整 UI / 图形复跑发生于两条颜色拒绝补丁前，最终补丁只增加拒绝/默认值标记，补丁后的正常解码回归及拒绝矩阵已独立重跑。

