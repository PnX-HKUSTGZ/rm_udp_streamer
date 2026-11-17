# 主分支交付说明

本次交付按用户接受的 **180 秒预热后、完整 30 分钟平均新画面提交至少 55 fps** 收口。主分支包含接收器源码、Godot 工程及 UID、Windows/Linux 工程原生库、对应许可/重链接材料、构建工具和验收文档。临时测试脚本、抓包和原始测量继续保存在仓库外的 `processing`，不作为应用资源。

## 打开工程

使用 Godot 4.7.2 Mono 和 .NET SDK 8.0.428，在仓库根目录执行：

```text
dotnet build RmUdpStreamer.csproj
```

然后用 Godot 导入 `project.godot` 并运行。监听地址填写本机地址或 `0.0.0.0`，端口为 `3334`，点击“开始接收”。默认参数来自已核验的固定发送端；新的有效参数会在成功解码后保存。详情见[使用说明](usage.md)。

首次打开新目录前应先执行上述构建。构建会提前登记原生扩展并保留已有扩展条目，规避固定版本编辑器首次扫描时的扩展晚注册退出问题；这与完整原生构建流程中的准备步骤一致。删除 `.godot/` 后也先重新执行构建。

`addons/rm_video/bin/` 中的两个原生库已进入 Git，不需要从外层验证目录复制。身份与 SHA256 见 `addons/rm_video/prebuilt.json`，许可及可重链接材料在 `addons/rm_video/redistribution/`。工程中的 Linux SO 对应已测 Intel N250/Ubuntu 24.04；AMD 780M 的独立发行包使用另一份已测 SO，不将 Intel SO 的验证范围扩展到 AMD。

Ubuntu 运行基线为 24.04 x86_64；需系统显示环境、媒体驱动、`libva2`、`libva-drm2`、`libdrm2` 及 Godot/.NET 的系统运行依赖。编入 VAAPI 的库在启动时即依赖这些组件。完整重建和重新导出见[构建说明](building.md)。

## 可直接交付的归档

本地交付目录为仓库根目录下的 `dist/mean55-20261117/`：

| 文件 | 内容与适用范围 |
| --- | --- |
| `rm-windows-x86_64.zip` | 自包含 Windows 程序；已测 NVIDIA CUDA/GPU/OpenGL 和 Intel D3D11VA/YUV/Mobile 组合 |
| `rm-ubuntu2404-intel-x86_64.zip` | Intel N250 的独立 Ubuntu 24.04 自包含包，VAAPI/YUV/Wayland/Mobile |
| `rm-ubuntu2404-amd-x86_64.zip` | AMD 780M 的独立 Ubuntu 24.04 自包含包，VAAPI/YUV/Wayland/Mobile |
| `rm_udp_streamer-source.zip` | 此次主分支源码快照，含工程原生库及其许可/重链接材料 |
| `delivery-index.json`、`SHA256SUMS.txt` | 包来源、源码/文档提交、归档摘要及完整性/启动检查结果 |

三个运行包使用已经验证的 `136681794a862d9086ec196590fc886491d55c4d` 运行文件，更新随包文档并重新生成文件清单；保留原始运行提交和独立文档提交。没有用重新编译的运行文件冒充已测产物。各包自带引擎、.NET 运行时、许可、对应源码与重链接材料；完整解压后启动 `RmVideoReceiver.exe` 或 `RmVideoReceiver.x86_64`。Linux ZIP 在对应 Linux 主机生成并保留执行权限。

完整自包含运行包作为交付归档放在 `dist`，不进入 Git 历史；Git 中的主工程和工程原生库可独立检出。重新复制、解压或交付前应核对 `SHA256SUMS.txt`，保留整个包目录。Windows 新程序路径需要按实际接收网络允许入站。

## 已验收与保留项

四条已测本地回放长测平均分别为 Windows CUDA **56.17 fps**、Windows Intel D3D11VA **57.21 fps**、Ubuntu Intel **59.93 fps**、Ubuntu AMD **60 fps**。平均通过不承诺每秒最低 55 fps；Windows 短时停顿与 LAN 短测未达标记录保留。

用户明确将 Windows 当前包诊断开销留作未验收项。当前包真实输入已在 NVIDIA D3D11VA/CPU NV12/Mobile 路径完成短时解码、显示和停止；之后重启时无数据输入，实时恢复仍未验收。原生 Ubuntu NVIDIA、Windows AMD 尚无设备，完整 R4/R6 阶段不声明完成。交付边界及指定独立审查见[收尾记录](evidence/r6/current-input-check.md)和[能力矩阵](support-matrix.md)。
