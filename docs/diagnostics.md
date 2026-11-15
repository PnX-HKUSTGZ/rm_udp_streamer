# 运行诊断

`--duration seconds --metrics new-file.ndjson` 使用当前界面运行时间触发停止。新画面提交计数表示纹理提交，不表示屏幕实际扫描。停止前会写入最终运行快照，再写 `stop_requested` 和终态记录。

## 停止与资源所有权

会话快照保留 `worker_done` 的原兼容含义：解码线程完成且没有 GPU holder。退役失败会隔离资源，因而这个字段继续为 false。自动调用方不要仅等待它判断失败结束。

| 字段 | 含义 |
| --- | --- |
| `decoder_done` | 解码 worker 已结束，可以回收线程；不等于 GPU 资源已退役 |
| `render_pending` | 尚未释放的 GPU holder 数量，包含故障隔离资源 |
| `render_retired` | GPU holder 为零且没有退役错误 |
| `render_retirement_failed` | 渲染关闭捕获了退役错误，资源继续隔离 |
| `render_retirement_error` | 退役失败原因 |
| `stop_terminal` | 解码 worker 完成，且 GPU 已退役或已明确退役失败 |
| `gpu_retirement` | 最后一次退役的资源计数；失败时也保留实际数量 |

定时停止使用单调时钟的 5 秒截止。正常退役写 `stopped`；退役失败或超过停止截止写 `stop_failed`，并以非零码退出。终态含 `stop_elapsed_ms`、`stop_error` 和 `exit_code`。启动/运行错误但资源正常退役时，终态仍为 `stopped`，`exit_code` 为非零，`core.error` 保留错误。

可捕获的 CUDA event / OpenGL fence 失败及 bridge 的两秒退役超时不会通过强制释放在途纹理来恢复。隔离对象保留至进程结束，同一会话不能重启，其他会话也不能再创建 GPU bridge。正常停止要求 GPU 资源计数归零。

以上截止依赖界面主循环能够继续执行，且驱动调用能返回。驱动调用永久不返回需要外部进程监督；本机制不承诺进程内部能够中断驱动。
