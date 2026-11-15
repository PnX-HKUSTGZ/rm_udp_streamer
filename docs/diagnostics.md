# 运行诊断

`--duration seconds --metrics new-file.ndjson` 使用当前界面运行时间触发停止。新画面提交计数表示纹理提交，不表示屏幕实际扫描。停止前会写入最终运行快照，再写 `stop_requested` 和终态记录。

未请求截图时，定时结束直接进入停止流程。`--snapshot` 最多等待两秒的新绘制；窗口最小化或停止绘制导致超时时，报告截图失败、不生成替代图片，仍记录最终快照并退役资源，以非零码退出。截图等待和下面的停止期限分别计时。

## 停止与资源所有权

会话快照保留 `worker_done` 的原兼容含义：解码线程完成且没有 GPU holder。退役失败会隔离资源，因而这个字段继续为 false。自动调用方不要仅等待它判断失败结束。

| 字段 | 含义 |
| --- | --- |
| `decoder_done` | 解码 worker 已结束，可以回收线程；不等于 GPU 资源已退役 |
| `render_pending` | 尚未释放的 GPU holder 数量，包含故障隔离资源 |
| `render_retired` | GPU holder 为零且没有退役错误 |
| `render_retirement_failed` | 渲染关闭捕获了退役错误，资源继续隔离 |
| `render_retirement_error` | 退役失败原因 |
| `stop_terminal` | 解码 worker 完成、已取走的帧均上传或取消，且 GPU 已退役或已明确退役失败 |
| `gpu_retirement` | 最后一次退役的资源计数；失败时也保留实际数量 |

定时停止使用单调时钟的 5 秒截止。正常退役写 `stopped`；退役失败或超过停止截止写 `stop_failed`，并以非零码退出。终态含 `stop_elapsed_ms`、`stop_error` 和 `exit_code`。启动/运行错误但资源正常退役时，终态仍为 `stopped`，`exit_code` 为非零，`core.error` 保留错误。

可捕获的 CUDA event / OpenGL fence 失败及 bridge 的两秒退役超时不会通过强制释放在途纹理来恢复。隔离对象保留至进程结束，同一会话不能重启，其他会话也不能再创建 GPU bridge。正常停止要求 GPU 资源计数归零。

以上截止依赖界面主循环能够继续执行，且驱动调用能返回。驱动调用永久不返回需要外部进程监督；本机制不承诺进程内部能够中断驱动。

## 计时开关与统计口径（timing_schema = 1）

`--metrics` 默认开启阶段计时；`--diagnostics on|off` 显式覆盖，原生配置使用可选布尔 `diagnostics`。关闭计时仍保留累计计数、`snapshot_us` 与精确提交秒窗口，取消逐阶段时钟采样、输入关联及额外 GPU 计时对象。对照诊断开销时，两组均应写 metrics，使用同样的样本、窗口和发送节奏。

全部 CPU 时间来自同一个原生 `steady_clock`，单位为微秒。协议入口原有时间戳精度为毫秒，转换成微秒后不增加精度。`snapshot_us` 在会话计数锁内读取；接收器和 GPU 快照各有独立锁，不保证三者属于同一瞬间。不要用 C# 累加的 `seconds`、发送进程时钟或 GPU 绝对时钟计算原生阶段延迟。

每个耗时直方图固定 201 桶，最多 1664 字节，不保存逐帧数组。`count`、`sum_us`、`min_us`、`max_us` 为累计值，`bins` 为非零 `[桶号,数量]`。桶 0 表示零；1–100 每桶 100 微秒，到 10 毫秒；101–190 每桶 1 毫秒，到 100 毫秒；191–199 每桶 100 毫秒，到 1 秒；200 为溢出。P50/P95/P99 使用 nearest-rank，返回桶上界；空样本或分位点落入溢出桶返回 null，不当作零。`invalid` 记录负时间差，`overflow` 记录超过 1 秒的数量。

C# 调用方负责及时释放读取快照的 `Variant` 及 `Dictionary`。`SnapshotAccess` 用明确作用域管理原生字典、键值和 JSON 序列化引用；不要等待托管 GC 去估计其看不到的原生内存。原生存储有界不代表忘记释放的调用方快照同样有界。

原生 `snapshot(detailed = true)` / `get_metrics(detailed = true)` 默认保留完整调用兼容。界面刷新传 false，只省略帧时序、秒窗口和直方图的字典构造，仍保留控制状态与累计计数；每秒日志及停止终态读取完整快照。这个读取选项不改变计时采样、窗口边界或计数本身。

窗口分析应对 `count`、`sum_us` 和各桶求差后重算分位数，不能相减两个累计分位数。窗口极值无法由累计 min/max 相减获得。嵌套阶段不能直接相加；所有 CPU 指标为调用或等待的墙钟耗时，不表示线程实际消耗的 CPU 时间。

| core.timings 字段 | 统计边界 |
| --- | --- |
| `assembly_time` | 首片收到至完整重组 |
| `order_wait` | 完整重组至按序入队 |
| `compressed_queue_wait` | 压缩组入队至出队 |
| `dequeue_to_packet` | 可证明关联组的出队至 parser 输出，含等待后续输入确认边界 |
| `parser_cpu` / `packet_prepare_cpu` | parser 调用 / 已送入解码器的包准备；后者含必要参数集拼接 |
| `decode_send_cpu` / `decode_receive_cpu` | FFmpeg send/receive 调用，包含返回 EAGAIN 的调用 |
| `packet_to_decoded` | 具有传递元数据的 parser 输出至解码帧回调；含缓冲、重排和调度，不是 GPU 解码耗时 |
| `cpu_readback` / `rgba_conversion` / `plane_pack` | 硬件帧回读 / RGBA 转换与裁剪 / CPU 平面整理 |
| `prepare_total` | CPU 帧准备总耗时，包含上面准备子阶段；GPU 直传不调用 CPU 准备 |
| `prepared_wait` | 准备完成至主线程取走，含等待呈现容量 |
| `render_dispatch_wait` | 主线程取走至渲染回调开始 |
| `upload_cpu` | 完整上传准备的 CPU 调用耗时，首次还含资源创建 |
| `submission_commit_wait` | 上传调用完成至会话接受提交 |
| `ingress_to_submit` | 已证明关联的输入首片至会话接受纹理提交 |
| `submit_to_main_accept` | 纹理提交至 Godot 主线程接受 publication |

首片到提交延迟不等于相机到屏幕延迟。GPU 上传返回只表示异步工作已正确排队；主线程接受画面也不等于实际扫描输出。被替换或丢弃的帧不进入 `ingress_to_submit`，必须同时报告提交覆盖率与丢弃原因。

## 输入关联与帧代际

`last_decoded`、`last_prepared`、`last_submitted`、`last_main_accepted` 分别保存最近到达对应阶段的帧。它们含 `sequence`、`stream`、`resource`、`presentation` 与各阶段时间；尚未到达的阶段为零，不能把不同 sequence 的字段拼成一帧。surface 的 `current_main_accepted` 表示当前仍引用的画面；会话的 last 字段在画面清除后仍保留历史。

输入关联使用固定 128 个字节区间记录。只有 parser 输出范围及长度与一个完整组严格一致、且恰有一个 first slice 时，才标为 `exact_group`。FFmpeg COPY_OPAQUE 将元数据随帧重排传递，协议序号不被伪造为媒体 PTS。EOF 使用实际已消费结尾，避开 parser 的负 sentinel，并继续检查范围和长度。

`correlation` 为 `parser_split_or_join`、`history_budget`、`missing_input_clock`、`not_single_picture`、`missing_decoder_opaque` 或 `disabled` 时，入口延迟未知。元数据淘汰只丢诊断关联，不丢视频。`correlated_frames/unknown_timing_frames` 和 `correlated_submitted/unknown_submitted` 报告覆盖情况；启用诊断时两项分别合计为 decoded 和 submitted_total。

## 计数守恒与提交帧率

`prepared` 保留旧含义：已发布到准备邮箱的累计帧数。`prepared_completed` 还包含准备后因停止或代际竞态丢弃的帧。`submitted_total` 和 `shown_total` 为会话累计计数，在 surface clear 后继续保留；旧 `surface.submitted` 仍为当前 surface 的计数。

同一 core 快照应满足：

```text
decoded = prepared_completed + prepare_failures + decoded_discarded_stop
        + decoded_discarded_generation + decoded_discarded_failure
        + decoded_replaced + decoded_capacity_drops + preparing + decoded_pending
prepared_completed = prepared + prepared_discarded_race
prepared = mailbox_replaced + prepared_discarded_stop + prepared_discarded_generation
         + prepared_discarded_failure + upload_requested + prepared_pending
upload_requested = submitted_total + upload_stale_before + upload_stale_after
                 + gpu_slot_drops + upload_failures + pending_uploads
submitted_total = shown_total + publication_replaced + publication_stale + pending_publications
```

`accounting_schema=2` 增加 CPU 准备前的原始帧邮箱。`decoded_replaced` 表示在回读/转色前被更新帧替换；它与准备后替换的 `mailbox_replaced` 分开。`decoded_pending` 和 `preparing` 各最多一帧，`cpu_sources_peak` 是这两个位置合计的峰值，不包含解码器内部帧池或正在交付的解码回调。CPU 结果未被取走时不会开始下一次准备；GPU 路径仍直接发布。`preparation_queue_wait` 测量解码完成到实际开始 CPU 准备的等待，保持同一 native 时钟。

CPU 准备输出池最多三份。只有外部消费者全部释放后才复用；三份都被持有时，在回读之前返回容量不足并计 `decoded_capacity_drops` / `preparation_pool_misses`。正常单上传消费不应触发此项。`output_buffers_allocated`、`output_buffer_growths`、`readback_allocations`、`scratch_buffer_growths` 统计本会话的输出对象、平面容量增长、硬件回读存储及编码空间 RGBA 临时存储的分配次数。对应 `*_capacity_peak_bytes` 是准备器拥有的容量峰值，既不代表停止后的存活资源，也不包含 FFmpeg 内部/渲染器分配。`plane_copy_bytes` 和 `rgba_crop_copy_bytes` 只统计准备器显式复制的像素字节；CPU 上传到 Godot 及驱动内部复制应分别评估。

CPU 上传按平面缓存最多三份 `PackedByteArray`，格式改变时清空。写入时若旧图像仍被消费者持有，Godot 的写时复制会分离存储；不会覆盖在用像素。`surface.cpu_upload_storage_changes` 统计缓存可写数据地址的变化次数，不能覆盖分配器内部的全部分配；`cpu_upload_copied_bytes` 与 `cpu_upload_images_created` 分别统计复制字节及图像对象创建次数。`cpu_upload_cached_bytes/peak_bytes` 是当前缓存逻辑字节数及峰值，不含引擎/驱动持有的其他副本。这些 surface 计数在 clear 时重新开始，区别于 core 的会话累计计数。

`upload_busy_polls` 统计主循环发现上传繁忙的次数，不是丢帧数；`shown_invalidated` 统计已接受画面失效，不从累计接受数扣除。正常停止应无 decoded_pending/preparing/prepared_pending/pending_uploads/pending_publications，压缩队列资源也归零。接收器停止保留累计输入统计；新会话才清空。解码、准备与渲染统计分别发布锁保护的快照，GPU 完成数可晚于提交数。

`submission_windows` 从第一张被接受的纹理提交时刻 `origin_us` 开始，以原生单调时钟精确分成一秒。`closed` 包含 `[index,count]`，只导出闭合窗口；无提交的窗口明确为零。固定 64 槽包含当前秒，保证最近 63 个闭合窗口；`first_index`、`current_index` 使观察者检测缺失历史。观察间隔超过保存范围时标记测量缺口，不能补零冒充已观测。索引必须去重，P10 对完整一秒窗口的计数做 nearest-rank；均值为这些窗口的提交总数除以秒数。暖机和输入停止后的窗口不得混入有效输入测量。

## CUDA/OpenGL 异步计时

仅在 diagnostics 开启时，每个既有槽额外分配一个 CUDA start event 和两个 GL_TIMESTAMP query，最多 3 个 event / 6 个 query。已有 producer completion event 启用计时，仍负责 source 生命周期；消费者 fence 继续决定槽复用。计时结果不会控制纹理所有权，也不会新增同步等待。

`surface.gpu_timings` 与停止后的 `core.gpu_retirement.gpu_timings` 包含：

| 字段 | 统计含义 |
| --- | --- |
| `map_cpu` / `unmap_cpu` | CUDA 资源映射 / 取消映射调用 |
| `cuda_copy_enqueue_cpu` / `gl_copy_enqueue_cpu` | 两平面拷贝排队的 CPU 调用 |
| `cuda_copy_unmap_gpu` | map 后的 CUDA start event 至 producer event；含两平面复制与 unmap 的流上区间 |
| `gl_display_copy_gpu` | 私有纹理复制到 Godot 采样纹理的两个 GL 时间戳差 |
| `cuda_completion_observed` | CPU 开始记录复制至已有轮询发现完成，包含轮询间隔，不能当 GPU 耗时 |

CUDA elapsed 只在原有 producer query 已成功后读取；GL 先检查两个 query 均可用。槽需要复用而查询仍未就绪时，计为 missing，不等待、不增加对象。GPU 浮点毫秒/纳秒向上取整到微秒，再进入直方图；GPU 只输出相对时间，不与 CPU 时间戳相减。

`cuda_timing_status/gl_timing_status` 区分 disabled、available、unavailable 和 error。`gpu_timing_errors` 计可选计时 API 错误；它们不伪造零耗时或导致本可正常播放的会话失败。每类 `gpu_timing_*_requested = histogram.count + *_missing + *_pending`。正常停止时 pending 与 `gpu_timing_events_live/queries_live` 归零；退役故障时保留实际在途资源数量。

## RGBA 颜色转换

RGBA 转换显式使用帧的色度采样位置；未标注位置按 LEFT 处理，与 YUV 呈现默认值一致。转换器缓存包含编码尺寸、输入像素格式和色度位置，matrix/range 每帧设置。固定 FFmpeg 转换启用完整水平色度插值和精确舍入，避免忽略位置的无缩放快捷路径。

RGBA 在编码坐标内转换后裁取可见区域，保留裁剪边界的色度邻居及奇数裁剪起点。没有裁剪时直接写入输出缓冲。10 位输入仍要求显式允许降到 8 位，并在快照中标注精度降低；HDR 和其他未支持的颜色策略保持原有拒绝行为。
