# 小智 WebSocket 客户端需求核对

## 状态与范围

- 目标：LX06 `.166` 上的新 C 客户端；`.100` 作为实时语音源/验收端。
- 本文件只记录协议与源码核对结果，不包含设备凭据。
- 通过 Feishu 当前公开预览可见的正文仅覆盖页面前段：确认协议版本 1、WebSocket、OPUS、客户端音频参数 16 kHz/单声道/60 ms、握手头字段，以及客户端 `hello` 示例；服务端 `hello` 预览显示 `sample_rate: 24000`，但代码块在对象结束前被截断。[5]
- Feishu 页面之后要求登录/注册；状态图及后续 JSON 未能从当前预览取得。此前草稿中的“服务端不返回 `session_id`”和“Auto/Realtime 的具体 VAD/AEC 语义”目前不能作为已核实事实。[unverified]

## Feishu 可见要求

1. **连接与身份**：WebSocket 协议版本 1；握手包含 `Authorization`、`Protocol-Version`、`Device-Id`、`Client-Id`。[5]
2. **上行音频**：OPUS、16,000 Hz、单声道、60 ms；客户端 `hello` 示例声明 `version: 1`、`transport: websocket`、`features.mcp: true` 和上述音频参数。[5]
3. **下行参数**：可见的服务端 `hello` 示例开始部分为 `transport: websocket`、OPUS、24,000 Hz、单声道、60 ms；因页面预览截断，不推断缺失字段。[5]
4. **不可见内容**：当前 Feishu 预览未暴露 Manual/Auto 状态图及剩余协议代码块。模式名与消息格式以下面的锁定上游文档/源码核对；Feishu 对各模式的服务端 VAD/AEC 语义仍待完整页面或导出件确认。[1][3][unverified]

## 上游协议与实现交叉核对

- 锁定的 `xiaozhi-esp32` commit 为 `e076ac496e2d93dc3a2308a0491d745afe9489ab`。官方文档与实现定义 v1 为裸 Opus；v2 使用 16 字节时间戳/长度头；v3 使用轻量头。**16 kHz 是客户端上行参数；服务端 `hello` 可以给出 24 kHz 下行参数**，客户端需按服务端参数解码/播放，不可假设双向同采样率。[1][2]
- 客户端先发送 `hello`，并等待 `type=hello`、`transport=websocket` 的服务端确认；服务端 `session_id` 可选，服务端 `audio_params` 可提供实际下行采样率/帧长。连接未确认前不得开始音频会话。[1][2]
- 上游 `protocol.cc` 定义 `listen/start|stop|detect`、`auto|manual|realtime`、`abort` 和 MCP 消息。仅声明 `features.mcp=true` 会使服务器尝试初始化 MCP，因此客户端未完整实现 MCP 前不得宣称支持；同理 `features.aec` 是能力声明，不是音频链路具备 AEC 的证据。[1][2][3]
- 上游 `audio_service.cc` 为启用 server-AEC 的构建维护播放时间戳队列，并将对应时间戳附到上行音频；该路径依赖正确的协议版本/音频头，不能用裸 v1 Opus 代替。[4]

## 本地服务端与现有客户端的关键差异

- 本地服务端 checkout：`scratch/xzsrv`，HEAD `4576e391bb1f45be514a3bdf358d7dbfac37643e`。`helloHandle.py:57-59` 会记录客户端是否声明 `features.aec`；但 `connection.py:374-415` 只有 `conn_from_mqtt_gateway` 且包长至少 16 字节时才解析时间戳头并调用 `_apply_aec`。普通直连 WebSocket 音频走裸 Opus 解码路径；因此在当前服务端代码下，直连客户端仅在 hello 里声明 `aec:true` 并不会自动获得 server-AEC。
- 现有 `scratch/xz_client.c:313-342` 从 `pnshelper` 提供的 `/tmp/mico-voip-audio.fifo` 读取 **16 kHz 单声道 S16LE**；这只证明客户端可见的是单声道处理后流，不能据此断言 LX06 麦克风阵列数量、波束形成、噪声抑制或 AEC 的具体实现。
- 现有客户端已有一版本地 barge-in 启发式：TTS 开始 250 ms 内忽略，比较麦克风 RMS 与固定/播放电平阈值，连续 3 帧超阈后停止播放、发 abort 并恢复 `auto`（`xz_client.c:647-669`）。这是幅度门限，不是 AEC；电视对白/噪声也可能越过门限，不能当作已解决的自打断/电视过滤能力。
- 现有状态机在 TTS 播放期间不向服务器上传麦克风流（`xz_client.c:570-572`），因此不是服务器侧 full-duplex。局部 RMS 检测与持续上行不是同一能力。

## 实施约束与验收判据

1. 首个兼容实现使用协议 v1：客户端上行裸 Opus、16 kHz/单声道/60 ms；解析服务端 `hello.audio_params` 并按下行协商参数解码，不能把 16 kHz 假设用于双向，也不得附加 v2/v3 头。[1][2][5]
2. 基线先使用 `auto`。上游支持 `auto`、`manual`、`realtime`，但 Feishu 对这些模式的服务端 VAD/AEC 语义仍待完整页面确认；在 `.166` 的真实 AEC 路径和回声验收完成前，不发送 `features.aec:true`，不把“realtime”宣称为已具备全双工/AEC。[1][2][3][unverified]
3. 自打断需与自声回声和电视声区分：播放参考、麦克风输入、AEC 残差及检测决策要分开记录；不得只靠提高 RMS 阈值宣称通过。验收同时包含“仅播放 TTS 不打断”“电视连续播放不误打断”“真人在播放中插话能及时停止并继续收音”。
4. 时间线日志至少记录单调时钟与事件序号：连接/hello、监听开始、麦克风帧与 Opus 发送、STT、TTS 首帧/播放开始/播放结束、打断候选/判决、abort、状态迁移；记录端到端及打断延迟、丢帧/队列统计。禁止记录 Bearer/token、验证码或原始音频。
5. `.100`↔`.166` 真机验收必须保留每轮源端播放完成证据与目标端日志增量；正例、电视负例、TTS 回声负例、播放中插话分别计数，不能用一次成功或仅看退出码判定通过。

## 当前未闭环项

- Feishu 公开预览后续内容需登录；Manual/Auto 状态图、剩余 JSON，以及各模式的 VAD/AEC 说明仍不可见。完整文档导出可消除此项。[unverified]
- `.166` 当前 SSH 未监听；恢复到可验证的 SSH 后，先读取实际 ABI、播放器和 PCM 输入路径，再进行部署。不得使用为早期 `armhf` 假设编译的产物。
- `.166` 的 `pns_ubus_helper` 与原生 `mipns-xiaomi` 是否仍在运行、以及实际 DSP/AEC 路径，须在恢复访问后重新读取；直连 WebSocket v1 保持裸 Opus，当前本地服务端的时间戳 AEC 分支只适用于 MQTT-gateway 音频。

## Sources

[1] https://raw.githubusercontent.com/78/xiaozhi-esp32/e076ac496e2d93dc3a2308a0491d745afe9489ab/docs/websocket.md
[2] https://raw.githubusercontent.com/78/xiaozhi-esp32/e076ac496e2d93dc3a2308a0491d745afe9489ab/main/protocols/websocket_protocol.cc
[3] https://raw.githubusercontent.com/78/xiaozhi-esp32/e076ac496e2d93dc3a2308a0491d745afe9489ab/main/protocols/protocol.cc
[4] https://raw.githubusercontent.com/78/xiaozhi-esp32/e076ac496e2d93dc3a2308a0491d745afe9489ab/main/audio/audio_service.cc
[5] https://my.feishu.cn/wiki/M0XiwldO9iJwHikpXD5cEx71nKh — 通信协议：Websocket 连接 (Feishu shared page)
