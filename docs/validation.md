# 外部验证入口与复跑

验证脚本、样本和故障注入不属于产品运行资源。当前独立归档为 `rm-validation-r5-v5.zip`，SHA256 为 `ed9d5da44dac1d51e08365870f0e2f234e11394d72251aef55a18fdf8c16447b`。应从同一交付记录取得归档，先核对外部摘要，再解压；内部 `bundle-manifest.json` 覆盖26个成员，但不是数字签名。

需要 Python 3.12+。输出必须放在验证包外的全新目录。入口先核对脚本与样本摘要；源码模式需要按[构建说明](building.md)准备固定工具和依赖，发行包模式直接运行包中可执行文件。

```text
python validate.py verify
python validate.py core --repo PATH/product --cache PATH/cache --output PATH/results/core
python validate.py desktop --package PATH/package --decoder cuda --transfer gpu --renderer gl_compatibility --screen 0 --output PATH/results/short
python validate.py formal --package PATH/package --decoder cuda --transfer gpu --renderer gl_compatibility --screen 0 --output PATH/results/formal
python validate.py formal --package PATH/package --decoder vaapi --device vaapi:pci:0000:c6:00.0 --transfer yuv --renderer mobile --display-driver wayland --cadence responsive --output PATH/results/formal-vaapi
```

设备标识和屏幕序号应换成目标机实际值。`core` 支持显式 `--dependency-lock`、`--cmake`、`--cxx`、`--make`、`--dotnet`；新依赖保持自身身份，不继承旧依赖验收。源码桌面模式使用 `--repo`、`--engine` 和 Linux 所需的 `--dotnet-root`；它不能与 `--package` 同用。

`desktop` 默认5秒预热、30秒工程测量，退出零只说明计数、路径和停止检查通过。`formal` 固定180秒预热、1800秒有效窗口并开启诊断，检查均值≥59.4fps、完整秒窗P10≥58、可关联延迟P95≤50ms/P99≤100ms、至少60Hz、持续可绘制和资源预算。仍须审阅输入质量、完整环境记录和资源趋势；结果不等于物理扫描或相机到屏幕延迟。

2026-11-17 用户将当前接受目标改为完整正式窗口平均≥55fps。已发布验证包及原60fps等级规则保持原样；本轮从已有正式原件重新计算新用户目标，详见[接受范围](evidence/r6/acceptance-55fps.md)。记录新目标是否通过与原等级是否通过，保留所有低帧率/零提交窗口、延迟、退出和输入质量，不改变历史日志，不用部分窗口或重复择优得出新结果。

同一物理机器一次只运行一个测量，不同时构建或运行其他测试，也不读取活动 metrics。笔记本接电，保持窗口可绘制；执行器必须能存活超过34分钟。Windows 入口会检查窗口状态和进程优先级。Linux 输出写到本机原生文件系统，结束后才传输完整证据。

发行包模式逐成员核对包清单、大小与摘要，隔离用户配置并使用不存在的 SDK 根和最小系统 PATH，记录真实运行提交、原生库及发行程序集。重链接变体必须有自己的来源记录和清单。实际 profile/硬件帧仍需收到码流后证明，不能用枚举结果代替。

CI 可以执行 `verify` 和 `core` 并保存完整日志。桌面与正式测量需要真实目标 GPU 和图形会话的专用执行机，按顺序运行。普通云 CI、WSL 或软件渲染的结果不能代替原生 Ubuntu GPU 验收。

三种入口不覆盖全部独立验收：完整颜色/profile矩阵、GPU故障注入、60轮生命周期、真实图传、依赖重建和实际重链接有各自阶段记录。v1–v4 的失败或不同适用范围保留，v5 没有改变冻结发送、闭合秒窗、关联延迟和分位数算法。
