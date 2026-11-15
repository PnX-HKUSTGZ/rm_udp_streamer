# 当前实现架构（R0–R6 改进分支）

本文对应已实现的代码职责。R0、R1、R2 已独立审查通过；R3–R6 的性能、平台、发布与完整集成验收仍在进行。[早期设计](architecture.md)保留研究方向，不作为当前接口或支持声明。

```mermaid
flowchart LR
    UDP[UDP 接收线程] --> Protocol[ProtocolCore 校验、重组、排序]
    Protocol --> Queue[有界压缩队列]
    Queue --> Decoder[解码线程：parser、Decoder、恢复]
    Decoder --> Lease[FrameLease 与三种代际]
    Lease --> Latest[最新解码帧槽]
    Latest --> Prepare[独立准备线程：CpuPreparer]
    Prepare --> Mailbox[最新已准备帧槽]
    Lease --> GPU[硬件帧引用]
    GPU --> Mailbox
    Mailbox --> Render[Godot 渲染线程：上传或 CUDA/GL 桥接]
    Render --> Publication[提交结果与纹理引用]
    Publication --> Main[Godot 主线程：材质、布局、状态]
```

## 分层与所有权

| 代码区域 | 实际职责 |
| --- | --- |
| `native/src/transport` | IPv4 Socket、协议校验、来源过滤、重组/排序和压缩队列；独立于 Godot |
| `native/src/media/device.*` | 后端枚举、序号/稳定身份解析、设备创建、实际身份核对 |
| `native/src/media/decoder.*` | 顺序处理压缩码流、解析参数、HEVC 解码、硬件格式协商、恢复与后端回退 |
| `native/src/media/cpu_preparer.*` | 硬件回读、平面布局、RGBA 转色及有界缓冲池；由独立准备线程拥有 |
| `native/src/media/session.*` | 最新帧选择、准备线程、代际失效、失败状态、计数守恒、停止协调 |
| `native/src/godot` | 会话绑定、渲染线程调度、上传、CUDA/GL 同步、纹理发布和 GPU 退役 |
| `scripts/interop` | 少量配置/诊断字典；不传输托管视频像素数组 |
| `scripts/ui` | 连接和设备控件、设置、节奏、状态与有截止时间的退出/重启 |

CPU 路径仍顺序解码压缩流，只在解码完成后替换未准备的旧帧。准备线程同时最多保留一张正在处理的源帧，最新槽最多一张；不会把压缩 HEVC 帧直接当成可随意跳过的独立图片。输出缓冲最多三份，仅消费者释放后复用。回读或转色不在 UI/渲染线程执行。

`FrameLease` 持有 FFmpeg 帧引用；CUDA、D3D11VA 和 VAAPI 的硬件帧可使设备/帧池活到 Decoder 销毁之后。VAAPI 通过 FFmpeg 的 DRM→VAAPI 派生引用链管理显示和 fd，不改写 FFmpeg 私有结构。

CPU 上传保留最多三组 `PackedByteArray`，存在 Image/渲染器引用时写入会分离存储。逐帧像素复制和 Image 创建仍存在，缓存地址变化次数不等于完整分配次数。

GPU 路径使用私有互操作纹理和 Godot 采样纹理，继续保留 GPU 内复制。CUDA 完成事件、GL 消费者 fence 与退役计数保证在途对象不被提前复用。可捕获退役失败保留隔离对象并禁止继续分配新的桥接资源；不能为了让计数归零强行释放。

## 代际与停止

`stream_generation` 隔离换源/重启和压缩流恢复，`resource_generation` 隔离尺寸或格式变化，`presentation_generation` 隔离 GPU→CPU 回退。准备结果、上传工作和纹理发布均须匹配当前接受的代际，旧结果在明确计数后丢弃。

解码线程完成、渲染资源退役、退役失败分别报告。UI 请求停止后轮询终态，定时退出和渲染器重启使用 5 秒实时时限。截图等待绘制最多 2 秒；窗口不绘制不能无限阻止停止。真正不返回的驱动调用仍是进程级故障边界。

## 预算与观测

默认单组压缩数据最多 8 MiB；重组最多 8 组/32 MiB，压缩队列最多 4 组/16 MiB。组帧与排队共享从首包开始的 50 ms 年龄预算。GPU 桥接最多三个在途槽；FFmpeg 自己的参考图像/解码表面池另行管理。

诊断采用有界直方图、完整秒窗口和分类丢弃计数。首包到提交的组合延迟只统计能证明关联的帧；未知关联保持未知。已解码、准备完成、纹理提交、主线程接受分别计数，均不声称等于物理屏幕扫描。

Windows 硬件渲染的自动节奏可使用 120 次/秒处理上限，Vulkan 使用 Mailbox，OpenGL 关闭显示同步。Linux 自动模式保留启动策略；显式低延迟模式可选。非默认引擎策略保留，正常停止恢复原值及本进程定时器请求。

## 可迁移性与当前边界

产品构建锁定 Godot、godot-cpp、FFmpeg、SDK 和工具版本，并检查完整 FFmpeg 头文件、静态库及配置输入。重新构建依赖需要独立 developer 锁，不能继承旧产物的验证结论。静态 FFmpeg 的源码、许可和可重链接对象随发布包提供；VAAPI 另依赖系统 libva/libdrm 和 GPU 驱动。

当前原生 Ubuntu Intel/AMD 的核心硬解、回读、身份与帧引用寿命已有实机证据；AMD 已补齐桌面/回退矩阵及指定组合的最终包长测，Intel 桌面仍待连接显示器。Windows 最终包性能和完整集成审查继续进行。Windows CUDA/OpenGL 为已实现 GPU 桥接；D3D11/Vulkan、VAAPI/DMA-BUF/Vulkan 直传仍不能列为已实现。WSL 不能替代原生 Ubuntu，模拟发送样本不能替代真实 RoboMaster 设备。
