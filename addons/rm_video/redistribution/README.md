# 工程内原生库的许可和重链接材料

本目录与 `../bin/rm_video.dll`、`../bin/librm_video.so` 一起交付。对应摘要和构建身份见 `../prebuilt.json`。

- `native-windows-x86_64.zip`：Windows 已测 DLL 的材料。
- `native-ubuntu2404-intel-x86_64.zip`：Ubuntu 24.04 Intel 已测 SO 的材料；它不是 AMD 独立包中的 SO。

每个归档保留原发行包中的 `licenses/`、`relink/`、实际依赖锁和构建记录，包含 FFmpeg/nv-codec-headers 对应源码、原生适配源码、可重链接对象/库及操作脚本。`source-package-manifest.json` 用于追溯原完整发行包，不表示本材料归档包含那个发行包的全部运行文件。

FFmpeg n8.1.3 使用 LGPL-2.1-or-later，构建未启用 GPL 或 nonfree 组件。完整许可证和第三方声明在归档的 `licenses/`；Godot/godot-cpp、.NET、nv-codec-headers 与编译器运行库的声明也随原材料保留。应用源码权利保持原样。此交付不限制为调试 LGPL 库修改而进行的修改或逆向工程。

在目标系统将相应归档解压到独立目录，按照 `relink/README.txt` 重建或替换 FFmpeg，再运行：

```text
python relink/relink_native.py --output /new/path/rm_video.dll
python3 relink/relink_native.py --output /new/path/librm_video.so
```

分别使用 Windows 或 Ubuntu 对应命令。输出必须为新路径；关闭接收器并保留旧库后，再替换工程中对应平台的文件。新库需要重新验证，不继承现有性能结论。系统 GPU 驱动和 Ubuntu 的 libva/libdrm 等运行依赖需由目标机器提供，未包含在归档内。

此目录不参与 Godot 资源导入或视频运行；完整可运行发行包也各自包含许可及重链接材料。
