# 小智 / 豆包传输适配器

状态：统一核心的离线候选。没有部署音箱、请求真实云端、替换旧客户端或写入固件。
它只实现平台连接、消息、编解码和取消握手。麦克风、播放器、控制词匹配、增益、
灯光、会话切换和“何时发送用户原声”全部由 `../main.c` 的唯一核心决定。

## 文件

| 文件 | 职责 |
| --- | --- |
| `worker.c` | FD3 上的有界本地消息循环、epoch、reply sequence 到核心 generation 的映射 |
| `transport.c` / `.h` | 小智 WebSocket/60ms Opus；豆包实时消息/20ms PCM上行及PCM/Ogg Opus下行 |
| `prepare_support.py` | 从已有项目复制协议/WS/JSON/编解码组件，仅改WS符号命名空间 |
| `support-origin.json` | 原文件和复制件的逐文件 SHA256；测试同时检查两者 |
| `support-local-deltas.json` | 原来源不变；私有WS的精确PONG token改动另列基准/当前hash，不反向覆盖原客户端 |
| `build.sh` | host 与原厂 glibc 2.25 armhf 构建、ABI门槛及逐文件构建来源 |
| `tests/test_transports.py` | 网络隔离内真实WS/编解码/取消/本地ABI的合成fixture |

不修改 `runtime/doubao` 和 `kws/scratch/xz-client-rewrite` 的现有运行实现。
`support/` 是候选私有命名空间，不能反向覆盖原件。

## 启动

核心以 `SOCK_SEQPACKET` socketpair 的一端作为可读写、非阻塞 FD3，随后执行：

```sh
lx06-transport-adapter --provider xiaozhi
lx06-transport-adapter --provider doubao
```

`--protocol` 输出 `LX06DCTRANSPORT/1` 并立即退出。适配器自身不取得音箱会话锁；
启动、许可验证、PDEATHSIG、进程组和有界收尾必须由核心负责。不要从Web脚本直接
将它当作第二个音频客户端运行。

`--check --provider doubao` 是扩展配置专用的无音频探测：只发送认证
`session.create`，等待 `session.created/updated` 后关闭。不发送MIC、文字或问题，
不启动麦克风/播放器/灯光，不取对话lease。成功只代表当时认证握手通过，不能据此
声称余额可查或未来调用一定成功。脚本入口迁移的缺口见
`../../../docs/unified-script-migration-20261101.md`。

| 环境变量 | 说明 |
| --- | --- |
| `DC_ENDPOINT` | 优先端点；否则小智用 `XZ_WS_URL`，豆包用 `DB_ENDPOINT` |
| `DC_CREDENTIAL` | 优先凭据；否则小智用 `XZ_ACCESS_TOKEN`，豆包用 `DB_API_KEY` |
| `DC_DEVICE_ID` / `DC_CLIENT_ID` | 小智标识；否则 `XZ_DEVICE_ID` / `XZ_CLIENT_ID` |
| `DC_UPLINK_MODE` | `guarded` 默认；或 `native`。只控制协议协商，不作本地打断判定 |
| `DB_VOICE` | 豆包音色，默认 `zh_female_xiaohe_jupiter_bigtts` |
| `DB_INSTRUCTIONS` | 豆包提示词 |
| `DB_AUDIO_FORMAT` | `ogg_opus` 默认；或 `pcm_s16le`，下行均交给核心24kHz单声道PCM |
| `XZ_CA_FILE` / `DB_CA_FILE` | 可选CA文件；仍校验证书及主机名 |
| `XZ_CONNECT_TIMEOUT_MS` / `DB_CONNECT_TIMEOUT_MS` | 严格1000..60000ms，默认10000/8000；整个worker连接至平台READY共用一个截止 |
| `XZ_PING_INTERVAL_MS` | 默认15000ms，100..300000；0禁用心跳 |
| `XZ_PONG_TIMEOUT_MS` | 默认10000ms，100..60000；仅精确8字节当前token能确认本次ping |

豆包默认端点与原客户端相同。没有预置个人身份、账户令牌或API Key。生产端点必须
为 `wss://`；只有 `ws://127.0.0.1:<port>` 且凭据恰为合成 `fixture-key` 时允许明文
离线fixture。连接/收发在平台worker内，不能因此阻塞核心的本地停播通道。

## 消息与提交契约

消息结构见 `../protocol.h`，48字节little-endian header，每包body最多8192字节。
所有消息带核心分配的非零epoch；跨epoch、未知消息、超长包、错误PCM格式和附带FD
均拒绝。适配器不打印body、转录、原始服务端错误、鉴权头或URL。

1. 核心发空 `DC_CMD_OPEN`。适配器连接并完成平台握手后发 `DC_EVT_READY`。
   连接前启用单个相对计时器，覆盖DNS/TCP/TLS/HTTP upgrade及平台session-ready；
   平台有效READY后关闭。超时信号只在网络worker执行异步安全`_exit(124)`，不能
   阻塞核心本地停播；不通过放宽TLS或自动重连延长本轮。
   可选的音频专用 `DC_CMD_GREET=8`（7保留）带核心非零generation、sequence0和
   1..240字节问候文本；仅一次，且须在首云端BEGIN之前。小智用`listen.detect`，
   豆包用`speech_text_buffer.commit`。这是显式问候请求，不是QUESTION重复提交。
2. 核心发无FINAL的空 `DC_CMD_RESUME` 开倾听。小智guarded使用`manual`，native
   使用`realtime`；guarded不能用服务端auto截句抢先提交半句。豆包保持原实时协议。
3. 核心选择上行原声，用 `DC_CMD_MIC` 发送16kHz单声道S16LE，包间保持连续。
   小智每960样本编码60ms Opus，豆包每320样本发送20ms base64 PCM。
4. 核心的唯一小米ASR产生完整QUESTION FINAL后，发 `DC_CMD_RESUME|DC_WIRE_FINAL`。
   末尾不足一帧的真实语音补零提交，不跨到下一轮。小智发`listen.stop`，豆包发
   `input_audio_buffer.commit`。可选body为不足2048字节、无NUL的完整QUESTION，
   只用于核心观测；绝不同时把text再发成第二个用户问题。
5. 云端回复开始时适配器只分配单调 `sequence` 并发 `DC_EVT_REPLY_BEGIN`，不自行
   分配generation。核心以 `DC_CMD_GRANT` 为该sequence绑定generation后，才允许
   AUDIO/TEXT/REPLY_END回传。grant前保存最多32KiB/64条，满后明确失败关闭。
6. 核心先停止本地音频，再后台发 `DC_CMD_CANCEL`。同sequence重复取消不重复发云端。
   `DC_EVT_USER_PARTIAL/FINAL` 仅是平台观测，不是第二个控制词/提问识别入口。
7. 结束时核心发 `DC_CMD_CLOSE`，并负责worker退出超时的精确进程组回收。

每个会话至多32个活跃sequence→generation映射槽、16个输出包槽和32KiB grant前区。
满队列不会静默截断问题。Ogg使用复用的有上限TMP压缩环，用户数据不写DATA。

预BEGIN取消墓碑不发GRANT。CANCEL会校验并登记核心已给定的sequence/generation，
丢弃该sequence的grant前缓存，保证随后RESUMED仍回原generation；不能回0而卡住
下一轮，也不能为放行旧音频伪造一次播放GRANT。核心的独立问候槽负责与首问分离。

XZ心跳同时最多一个待确认ping，8字节递增token必须逐字节匹配当前PONG。旧/错误/
任意其他流量不能延长PONG截止；服务端PING仍按RFC回PONG。心跳或网络失败结束本轮，
适配器不另建会话、不自行重连。所有协议状态都留在网络worker，控制/停播仍由核心。

## 豆包词语模式和迟到消息

豆包全双工服务器仍可能根据收到的原声自行停止旧回复。`guarded` **不表示已经关闭
服务端打断**：核心必须在受保护播报/思考期间保留用户原声、继续唯一的本地控制识别，
而不要把原声上传到豆包。人声模式是否上传、何时取消同样由核心决定。

取消之后 `response.canceled` 只发ACK，不能解开上行保护；只有对应 `response.done`
才发带被取消sequence的 `DC_EVT_RESUMED`。等待期间后续原声由核心按有界策略保留，
RESUMED后再按时间顺序发送。无response_id的done只在确有待取消回复时作为协议屏障。

收到的豆包response_id和本地sequence不可混淆。最近8个已取消ID保留用于过滤迟到
started/audio/text/done；第二次取消不应使第一次的迟到包复活。核心仍须拒绝旧代次
的包。小智abort/原始音频没有可验证的请求ID，不能将晚到的abort标为新取消的ACK；
适配器在成功发送对应abort及listen.start后只回原sequence的RESUMED。正式组合必须
继续验证平台取消屏障和核心expected-generation处理；本fixture不证明任意迟到包
在真实服务端都能按新旧请求分辨。

## 离线验证与剩余验收

在WSL root及已安装交叉编译器、OpenSSL/Opus开发库、QEMU、SquashFS、ffmpeg的环境：

```sh
cd runtime/dialogue-core/adapters
DC_ADAPTER_BUILD_DIR=/absolute/new-output/host bash build.sh host
DC_ADAPTER_BUILD_DIR=/absolute/new-output/sanitize DC_ADAPTER_SANITIZE=1 bash build.sh host
DC_ADAPTER_BUILD_DIR=/absolute/new-output/arm bash build.sh arm
unshare --net python3 tests/test_transports.py --output /absolute/new-output
```

网络namespace只启用loopback，无外网路由。测试用合成账号/PCM/文字，真实完成WS
握手、mask、Opus编码解码、PCM/Ogg播放数据格式、控制词只作转录、重复取消、两次
取消后旧包、ACK不恢复、同连接后续回复、末帧补零、可选QUESTION不重复提交、越界和
旧epoch拒绝。另有4项认证探测验证：created/updated接受，错误或未授权的音频开始
拒绝，全程只create/close且不提交音频或问题。host与原厂rc23 rootfs内QEMU各18项通过；报告位于
`build/fixture-report.json`，逐构建来源在`build/<mode>/source-reference.json`。

ARM离线chroot用host OS生成的随机字节常规文件作为fixture熵源（宿主不允许临时
字符设备），以及固定数值`/proc/meminfo`；这不改产品二进制、不改真实设备熵源，
也不构成真机内存验收。测试记录编译来源并核对原有客户端文件未改变。

未完成：与统一核心的完整组合fixture、真实平台账号协议/长回复、实际音箱停播与
续问、官方接管/本地切换、OOM/资源长期测试。不得据此覆盖旧刷机包或标为已发布。

以上18项与未完成组合的说明属于首版历史记录。当前兼容版本分别验证三类传输31项、
route12项、真实main组合30项；精确hash、禁播/问候边界及额外native问候记录见
`../../../docs/unified-audio-compatibility-20261101.md`。仍没有真实音箱验收。
`build.sh`拒绝覆盖已有ELF；`prepare_support.py`遇到已审阅local-deltas时拒绝直接
覆盖support，必须明确保留并rebase这些改动。原来源manifest保持历史字节不变。
