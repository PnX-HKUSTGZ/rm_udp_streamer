# 固定依赖构建

版本和产物校验值以 [dependencies.lock.json](dependencies.lock.json) 为准。项目使用 C++17、Godot Mono 4.7.2、.NET SDK 8.0.428；两平台分别构建。Linux 构建与验证输出应放在 Linux 原生文件系统的独立工作目录。

正式工程入口为 `native/CMakeLists.txt`。依赖缓存准备好后，在接收器仓库执行 `python build/build_project.py`（Ubuntu 使用 `python3`），会构建原生扩展与 C# 工程。可用 `--cache`、`--cmake`、`--cxx`、`--make`、`--dotnet` 指定位置，工具版本仍需符合所声明的构建组合。重建的 FFmpeg 使用独立 `--dependency-lock`。构建输出在被忽略的 `out/<平台>` 与 `addons/rm_video/bin`，打开根 `project.godot` 运行。

首次打开前须运行上述构建入口。它在构建成功后合并生成 `.godot/extension_list.cfg`，让引擎在启动阶段发现扩展。固定的 Linux Godot 4.7.2 在第一次文件扫描中才发现扩展类时会在退出时崩溃；仅注册一个空原生类也能复现，启动阶段加载同一扩展则正常。这是已验证的构建兼容处理，并非修改引擎。删除 `.godot` 缓存后请重新运行构建入口；不要跳过它只复制 DLL/SO 后打开全新工程。

## FFmpeg

`build_ffmpeg.py` 在 Linux 构建最小 LGPL FFmpeg：只启用 HEVC 解码/parser、swscale、CUDA，Windows 额外启用 D3D11VA。关闭自动发现外部依赖、GPL 和 nonfree 组件。生成静态 `libavcodec.a`、`libavutil.a`、`libswscale.a` 与完整配置清单。GPU 驱动由系统提供，不随应用打包。

准备独立缓存目录，分别克隆 FFmpeg 和 nv-codec-headers，然后检出锁文件中的完整 commit。目录名必须为 `ffmpeg-source` 和 `nv-codec-headers`；构建程序会核对 commit，不能把其他分支或系统 FFmpeg 冒充固定版本。

```sh
python3 build/build_ffmpeg.py --cache /path/to/cache --target linux --work /new/linux-rebuild --nasm /path/to/nasm
python3 build/build_ffmpeg.py --cache /path/to/cache --target windows --work /new/windows-rebuild --cross-tools /path/to/cross-tools
```

Linux 工具为 GCC/G++、GNU Make、pkg-config、NASM。当前 `cross-tools` 是使用 `apt-get download` 下载 Ubuntu 24.04 的 MinGW win32 编译器、runtime、binutils、mingw-w64-common/x86-64-dev 和 NASM，再用 `dpkg-deb -x` 解包到用户目录，未要求系统级安装。准确包文件版本和 SHA256 列在锁文件的 `toolchains.ubuntu_packages`。为复现同一工具链应使用这些版本；缺包时明确失败，不静默替换。

重建入口只读取固定源码缓存，从 Git 归档创建干净源码快照，构建到全新 `--work/ffmpeg-install`。已有目录拒绝覆盖，失败日志保留。`build-receipt.json` 记录源码归档、实际工具版本/摘要、命令、配置文件和输出摘要；安装前缀中的 `build-manifest.json` 包含同一收据。收据是构建来源证据，不是运行验收，也不声称所有系统头文件与动态链接工具均已封闭复现。可用 `--cc`、`--make`、`--pkg-config`、`--nasm` 选择工具；隐式编译和 Make 参数会被清除。

新输出必须用 `record_dependency_lock.py` 创建独立 developer 锁，随后用 `prepare_dependencies.py` 按新锁恢复到独立缓存，并以 `build_project.py --dependency-lock` 构建与验收。官方锁及已有缓存不被重建流程覆盖。不能将旧锁的校验值改成新值后宣称官方产物复现成功。发布包中的 `rebuild_ffmpeg.py` 使用同一驱动和全新目录；手动修改源码并执行 Make 后，原收据即不再描述修改后的产物，需另行记录与验证。

Windows 静态库在 WSL 交叉编译后，将新前缀和对应 developer 锁交给 Windows 的依赖恢复入口；Windows Godot 扩展仍在 Windows 使用 MinGW GCC 15.2 编译。当前固定 C 归档的链接、硬解、像素结果已实测，新重建的归档须重新验证。MSVC 与 MinGW 产物不可混为一个未验证 ABI 组合。

两平台重建输出都位于指定的 `--work/ffmpeg-install`，依赖恢复入口按锁恢复成缓存中的 `ffmpeg-static-linux` 或 `ffmpeg-static-windows`。扩展链接顺序为 `avcodec` / `swscale` / `avutil`，其余系统链接要求以生成的 pkg-config `.pc` 文件为准。Windows 当前需要系统 `bcrypt` 等库；Linux 需要 `m`、`pthread`、`dl`，`atomic` 使用静态归档。C++ 运行库使用 `-static-libgcc -static-libstdc++`。

## Godot 与 .NET

下载锁定的 Linux Godot Mono 和 .NET SDK，验证 SHA512 后解包到缓存。设置 `DOTNET_ROOT` 和 PATH。离线或 NuGet 获取不稳定时，使用引擎自身 `GodotSharp/Tools/nupkgs` 作为 `Godot.NET.Sdk/4.7.2` 的包源。

godot-cpp 使用锁定的 4.5 stable 提交，已经在两个平台的 Godot 4.7.2 上加载并执行本项目所需调用。后续使用新的引擎 API 时必须重新核对绑定版本，不根据这次有限测试宣称所有 API 均兼容。

导出模板固定为 Godot 4.7.2 Mono，须校验下载并安装到明确的模板目录。编辑器中的扩展加载、独立导出、自包含运行时和无 SDK 启动分别验证；源码构建成功不能代替发布包验收。

## 发布材料

FFmpeg 为 LGPL-2.1-or-later。导出流程组装版本/许可证、对应源码及可重新链接的应用对象与静态库；最终发布候选还须验证实际重链接和包完整性。godot-cpp/引擎及 NVIDIA 头文件分别附其许可证。正式包不包含任务中的测试样本、故障注入脚本或 SDK 调试工具。
