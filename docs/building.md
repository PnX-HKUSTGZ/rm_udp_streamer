# 固定依赖构建与打包

产品源码在独立Git仓库`rm_udp_streamer`。临时验证放外层processing；正式工程仅含运行代码、构建工具和文档。

## 官方构建基线

Windows x86_64：MinGW GCC/G++15.2.0 win32-seh、CMake4.1.3、.NET SDK8.0.428；Linux x86_64：Ubuntu24.04、GCC/G++13.3.0、CMake3.28.3、.NET SDK8.0.428。引擎Godot4.7.2 Mono，固定godot-cpp与FFmpeg提交在`build/dependencies.lock.json`。

默认缓存为仓库上层`.tools/rm-video`，可用`--cache`指定。缓存布局：

```text
godot-cpp/                 固定commit的干净Git检出
ffmpeg-static-windows/     发布包relink/ffmpeg中的原始include/lib与build-manifest
ffmpeg-static-linux/       同上，按平台
python-tools/cmake/data/bin/cmake.exe   Windows CMake4.1.3
dotnet-8.0.428/dotnet       Linux SDK
release-sources/           prepare_release_sources.py生成的对应源码归档
```

可从D3包的`relink/ffmpeg`恢复该平台已锁定的FFmpeg库；构建器核对三个静态库SHA256。修改FFmpeg时使用包内重链接流程，不能把变化的库假装为已验证的原始库。源代码重建可能因编译器或绝对配置路径而有不同字节摘要，须单独记录变更及验收。

```text
python build/build_project.py --verify-only
python build/build_project.py
```

Windows优先使用缓存内CMake，不受PATH中其他版本影响。已有构建缓存保留同一编译器路径拼写，避免Windows大小写别名触发CMake隐式重置。新构建不依赖历史缓存里的链接参数，静态编译器运行库及无未定义符号要求已在CMake明确配置。`global.json`禁止SDK自动升级，运行库固定8.0.34。

要从原始Git源码构建FFmpeg，使用`build/build_ffmpeg.py --cache PATH --target linux`；WindowsFFmpeg通过WSL/Linux加`--target windows --cross-tools PATH`构建。对应源码必须固定提交且工作区干净；原始配置和交叉工具包摘要保存在锁文件。

## 最终包与许可材料

先准备FFmpeg与nv-codec-headers对应源码：

```text
python3 build/prepare_release_sources.py --cache PATH --output PATH/release-sources
python build/export_project.py --cache PATH --templates PATH/Godot_v4.7.2-stable_mono_export_templates.tpz --output dist/D3-windows --label D3
```

Linux的输出目录改为`dist/D3-linux`，其余参数按实际路径。导出要求产品Git工作区干净，新输出目录为空，模板SHA512与引擎SHA256匹配。它执行原生/.NET构建和Godot导出，检查错误日志、程序集、自包含运行库及实际版本；仅退出码0不足以通过。

最终包包含完整许可清单、对应源码、可重链接对象和操作脚本。`relink/relink_native.py`可选择新的FFmpeg前缀；为避免覆盖正在运行的库，输出必须是新路径。先关闭应用并备份，再替换包内DLL/SO。重链接后的库需重新验证实际解码与生命周期。

`manifest.json`覆盖包中每个交付文件；`build-manifest.json`记录真正使用的工具版本/路径/摘要与库摘要。导出产物不提交到Git，审查与性能证据按阶段提交到docs/evidence。
