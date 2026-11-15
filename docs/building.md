# 固定依赖构建与打包

产品源码在独立Git仓库`rm_udp_streamer`。临时验证放外层processing；正式工程仅含运行代码、构建工具和文档。

## 官方构建基线

Windows x86_64：MinGW GCC/G++15.2.0 win32-seh、GNU Make4.4.1、CMake4.1.3、.NET SDK8.0.428；Linux x86_64：Ubuntu24.04、GCC/G++13.3.0、GNU Make4.3、CMake3.28.3、.NET SDK8.0.428。引擎Godot4.7.2 Mono，固定godot-cpp与FFmpeg提交在`build/dependencies.lock.json`。

默认缓存为仓库上层`.tools/rm-video`，可用`--cache`指定。缓存布局：

```text
godot-cpp/                 固定commit的干净Git检出
ffmpeg-static-windows/     发布包relink/ffmpeg中的原始include/lib与build-manifest
ffmpeg-static-linux/       同上，按平台
python-tools/cmake/data/bin/cmake.exe   Windows CMake4.1.3
dotnet-8.0.428/dotnet       Linux SDK
release-sources/           prepare_release_sources.py生成的对应源码归档
```

可从D3包的`relink/ffmpeg`恢复该平台已锁定的FFmpeg依赖。构建器同时核对三个静态库和完整的`include/`、`lib/`、`build-manifest.json`文件清单、大小与SHA256；头文件或配置被替换、缺失或混入未知文件都会失败。清单采用前缀相对路径，原样迁移缓存不会改变校验结果。历史构建配置中的路径仅用于追溯，CMake使用本次`--cache`指定的实际位置。

修改FFmpeg时使用包内重链接流程，不能把变化的库假装为已验证的原始库。源代码重建可能因编译器或绝对配置路径而有不同字节摘要，须单独记录变更及验收。`build_project.py --dependency-lock PATH`可显式选择独立记录的开发者锁文件；构建证据将记录其摘要并标为`developer`，不覆盖官方锁文件或继承官方验收结论。

## 准备新依赖缓存

先准备Python 3.12+、Git，以及上述固定版本的编译器、Make、CMake和.NET SDK。以下入口恢复FFmpeg的完整固定输入，并获取固定提交的godot-cpp：

```text
python build/prepare_dependencies.py --cache PATH/receiver-deps --ffmpeg PATH/receiver-release.zip
```

`--ffmpeg`也接受发布包解压目录、tar归档或完整FFmpeg前缀。归档成员按锁文件逐项读取并核验，不按归档路径直接解包。已验证的缓存可重复使用；已有目录不匹配时明确失败，不覆盖。失败的暂存目录会保留以便定位。输出中的`prepare-*.json`记录本次依赖来源及锁文件摘要。

godot-cpp默认从锁文件指定的Git仓库获取。离线时增加`--godot-cpp-source PATH`，复用固定提交的干净检出；仅恢复一个组件可使用`--components ffmpeg`或`--components godot-cpp`。此入口不安装GPU驱动或系统构建工具。

新目录构建可显式指定工具位置，避免依赖历史CMake缓存或PATH：

```text
python build/build_project.py --cache PATH/receiver-deps --cmake PATH/cmake --cxx PATH/g++ --make PATH/make --dotnet PATH/dotnet
```

Windows将路径换为对应`.exe`；带空格的路径使用引号。构建器为所选SDK设置`DOTNET_ROOT`和子进程PATH，并记录实际工具版本、路径和摘要。`export_project.py`接受相同四个工具参数。

开发者重新构建固定源码后，保留原官方锁文件并记录新依赖：

```text
python build/record_dependency_lock.py --ffmpeg PATH/rebuilt-prefix --platform linux --output PATH/developer.lock.json
python build/build_project.py --cache PATH/developer-cache --dependency-lock PATH/developer.lock.json
```

新前缀必须包含构建器产生的`build-manifest.json`。记录器核对源码版本、目标平台和既有LGPL构建约束；新锁明确标记`developer`及`validated: false`。它只记录产物身份，不证明重新构建、功能、颜色或性能已验收。构建及导出均需显式传入该锁，导出包保留实际锁文件与类别。改变源码版本需先建立和审查新的源码锁定信息。

## 构建与导出

```text
python build/build_project.py --verify-only
python build/build_project.py
```

Windows优先使用缓存内CMake，不受PATH中其他版本影响。已有构建缓存保留同一编译器路径拼写，避免Windows大小写别名触发CMake隐式重置。新构建不依赖历史缓存里的链接参数，静态编译器运行库及无未定义符号要求已在CMake明确配置。`global.json`禁止SDK自动升级，运行库固定8.0.34。

要从原始 Git 源码重建 FFmpeg，使用 `build/build_ffmpeg.py --cache PATH --target linux --work NEW_DIRECTORY`；Windows FFmpeg 通过 WSL/Linux 加 `--target windows --cross-tools PATH` 构建。对应源码必须固定提交且工作区干净。重建程序从归档创建独立源码快照，输出到新目录的 `ffmpeg-install`，保留实际工具/源码/配置/日志和输出校验收据，不修改官方缓存。新输出用 `record_dependency_lock.py` 记录为尚未验收的 developer 锁，再恢复到独立缓存、构建并执行受影响验收。原始配置和交叉工具包摘要仍保存在官方锁文件；不同工具或路径的重建不自动等于字节复现。

Linux 的依赖开发可额外传 `--vaapi`，明确编入 HEVC VAAPI 与 DRM。Ubuntu 24.04 需先安装 `libva-dev libdrm-dev pkg-config`；构建记录实际系统包的版本、链接选项和 pkg-config 文件摘要，继续禁用自动发现其他外部库及 GPL/nonfree 组件。VAAPI 使用发行版的 libva/libva-drm/libdrm 动态运行库和 GPU 厂商媒体驱动，新增产物仍须记录独立锁并重新验收。此选项仅准备依赖，不能据此宣称接收器已支持某个设备或达到性能目标。

## 最终包与许可材料

先准备FFmpeg与nv-codec-headers对应源码：

```text
python3 build/prepare_release_sources.py --cache PATH --output PATH/release-sources
python build/export_project.py --cache PATH --templates PATH/Godot_v4.7.2-stable_mono_export_templates.tpz --output dist/R-candidate-windows --label R-candidate
```

Linux的输出目录改为`dist/R-candidate-linux`，其余参数按实际路径。导出要求产品Git工作区干净，新输出目录为空，模板SHA512与引擎SHA256匹配。它执行原生/.NET构建和Godot导出，检查错误日志、程序集、自包含运行库及实际版本；仅退出码0不足以通过。

最终包包含完整许可清单、对应源码、可重链接对象和操作脚本。`relink/relink_native.py`可选择新的FFmpeg前缀；为避免覆盖正在运行的库，输出必须是新路径。先关闭应用并备份，再替换包内DLL/SO。重链接后的库需重新验证实际解码与生命周期。

VAAPI 的发布包与重链接工具会保留 `libva`、`libva-drm`、`libdrm` 的系统链接依赖；更换 FFmpeg 前缀时按所选前缀重新选择这些库。Linux 链接要求所有符号已解析，避免生成缺少外部库、直到启动才报错的 SO。运行机器需要发行版的 `libva2 libva-drm2 libdrm2` 及 Intel/AMD 媒体驱动；开发/重链接机器另需相应 `-dev` 包。编入 VAAPI 的原生库在启动时就需要这些运行库，即使本次选择软件解码也一样。

`manifest.json`覆盖包中每个交付文件；`build-manifest.json`记录真正使用的工具版本/路径/摘要与库摘要。导出产物不提交到Git，审查与性能证据按阶段提交到docs/evidence。

当前构建、无SDK运行、重链接与交互检查见[R5报告](evidence/r5/report.md)。可复跑的独立归档、命令和CI范围见[外部验证](validation.md)。构建导出完成不自动构成性能或真实图传验收。

D3在独立审查后仅更新随包文档，运行文件、依赖及重链接对象/库均保持已测摘要。`manifest.json`保留导出`commit`并另记审查文档`evidence_commit`。最终ZIP逐成员核对大小与SHA256，Linux压缩包在Linux生成并保留执行权限；包旁`D3-delivery-*.json`和`SHA256SUMS.txt`记录归档验证及摘要。
