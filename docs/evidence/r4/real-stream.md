# 真实UDP输入的解码检查

2026-11-16更新。**真实流启动、硬件解码和实际画面已验证成功。** 接收器保持运行后，用户重新开流；网卡抓包确认新流第1组包含VPS/SPS/PPS和IDR，同一未修改发行包随后开始播放。此前缺少参数的失败记录全部保留，不能将持续流采样结论扩大为“发送端从不发送参数”。

## 先前未解码的原因与证据

本机以太网4绑定`192.168.12.2:3334`，实际来源`192.168.12.1:50935`。10秒被动捕获收到5,931包、7,971,259字节UDP负载，有效到包区间约7.91秒。[抓包摘要](real-stream-capture.json)和[重组分析](real-stream-analysis.json)确认8字节大端头、1392字节分片步长；475组中474完整，最后一组因结束捕获不完整，没有包头、分片长度或内容冲突。

474个Annex-B NAL中，类型1共466个，类型20（IDR）共8个，类型32/33/34为0。因此阻塞是缺少初始化参数，不是缺少关键帧。分析JSON保留两种NAL头解释以便识别，不表示同时存在H.264和HEVC。

使用未修改的`88bf17a`自包含Windows包直接接收，Mobile渲染、自动解码器、CPU YUV传输：

| 检查 | 完整重组 | 解码 / 准备 / 提交 / 接受 |
| --- | ---: | --- |
| [20秒](real-stream-20s.json) | 1,017 | 全部0 |
| [180秒](real-stream-180s.json) | 10,800（终态） | 全部0 |

状态持续为`WaitingConfig`，missing/invalid/decode_errors均为0。自动选择D3D11VA / RTX 5070 Laptop（LUID `0:95085`），但`hardware=false`且没有硬件帧，不能称为硬解成功。两次进程均正常退出0、释放端口；验证失败源于没有实际解码和显示帧，退出0不等于解码通过。

将重组流交给独立FFmpeg 7.1仍报`PPS id out of range: 0`并非零退出，见[独立检查](real-stream-ffmpeg.json)。运行包manifest SHA256为`c303f834fc47a45aa89733fce1baff14aeaf801865e918d24011bd191136e514`，发行DLL和程序集与既有候选一致。

原始记录保留在外层`processing/2026-11-16-rm-real-stream`：抓包`capture-v1/datagrams.bin` SHA256为`aa1a10d56b4f86b06cd76760c9d4695af529e7c2dd6f37fdf064ea4a809fb359`；重组流`capture-analysis-v1/reassembled.h26x`为7,922,419字节，SHA256为`be8468b7cf185c5b1845dbce77c03d4f51ccd3e9646b6f05cbb58eec4cad7591`。这里仅发布检查摘要，原始视频不复制入产品仓库。

第一次重启回复后另用Wireshark dumpcap从网卡独立采集120秒，[89,900包检查](real-stream-wire-before.json)没有捕获丢包，直接扫描UDP负载与重组后扫描均没有VPS/SPS/PPS，但有120个IDR；[独立FFmpeg](real-stream-wire-before-ffmpeg.json)仍失败。首组frame_id为57143，不能据此认定已覆盖编码器启动第一瞬间。程序的WaitingConfig/Recovering状态也不足以单独证明网络未发送参数。

## 完整开流与成功解码

提前启动第二次网卡抓包，再由用户重新开流。该次共捕获39,242包、报告丢包0，包含旧来源的41包、断流间隔以及新来源`192.168.12.1:57108`的数据。第42包时间为17:14:41.937834（UTC+08:00），frame_id=1、fragment=0、整组85,268字节，在UDP负载偏移8/35/91/102分别出现VPS/SPS/PPS/IDR。后续约53秒仍有IDR，但本次捕获中参数集各仅出现一次，见[启动抓包](real-stream-wire-startup.json)。

从真实抓包中新来源的初始化组开始提取码流，[独立FFmpeg](real-stream-startup-ffmpeg.json)成功解码60帧、退出0、错误日志0字节；[提取记录](real-stream-startup-selection.json)证明没有生成或借用参数。此前将旧来源尾部也拼入的检查出现前缀缺参数警告，原记录仍保留。FFmpeg输入检查报告Main、8位yuv420p、有限范围BT.709、1920×1080；裸流探测显示25 fps/60 tbr的混合估计，不将它当成正式帧率测量，见[原始元数据](real-stream-metadata.json)。

未重启接收器、未修改解码代码，原PID40220随后进入Running。固定的[实时成功快照](real-stream-live-success.json)覆盖首次解码至后续约424秒：累计25,310个实际硬件解码帧、25,072次准备及提交、25,071次主线程接受；1920×1080、NV12 8位，D3D11VA / RTX 5070 Laptop，CPU NV12回读上传，Vulkan Mobile呈现。截图已视觉确认是真实现场画面，原图保留在外层`processing/2026-11-16-rm-real-stream/live-listener-v1/live-picture-v1.jpg`，SHA256 `40f647ad19696cbe83ffe122c9de6b01c164b81b8dce20b3d48f17f5e79416b6`。

该快照decode_errors/invalid均0；累计missing=2、recoveries=7、restarts=5，包含整个持续接收期间的重启/不连续，不能宣称整段网络零丢失或零恢复。接收器保持播放，本项不宣称已完成停止/端口重绑或正式1080p60性能等级。原始前后窗口、完整抓包和截图只存于外层processing，产品仅发布摘要。

当前已验证的开流顺序是先监听、再启动发送。中途接入时若未获得参数，仅有关键帧仍无法解码；需要继续验证发送端参数重发或获取机制，不能猜造配置。具体重启的物理组件、设备型号和固件尚未确认；完整设备矩阵、真实恢复/性能门槛以及R4/R6阶段审查仍未完成。
