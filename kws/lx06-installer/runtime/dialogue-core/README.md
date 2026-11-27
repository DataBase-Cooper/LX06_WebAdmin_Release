# Unified dialogue core / 统一语音核心

这是新增功能的唯一会话控制程序 `lx06-dialogued`。当前为 rc24 开发候选，
**未通过整套真机验收，也未合入完整刷机包**。不可据离线测试宣称可发布。
小米闭源采音、降噪、认证和官方对话保留；上传脚本仍负责平台登录和业务协议。

## 文件职责

| 文件 | 职责与边界 |
| --- | --- |
| `main.c` | 唯一非阻塞调度循环、采音订阅、ASR、统一播放、会话资源回收 |
| `types.h` | session epoch、回复 generation、用户 utterance 和公共状态 |
| `control.c/.h` | 纯控制状态机；结束优先、重复打断幂等、完整 FINAL 才提交问题 |
| `session.c/.h` | 唯一副作用顺序：本地停播→后台取消→后续问题→灯光；所有平台复用 |
| `protocol.c/.h` | 有界 Unix SEQPACKET 适配器消息，拒绝坏长度、未知标记及意外 FD |
| `audio_route.c/.h` | 音频问答上传水位、取消屏障、迟到 BEGIN 墓碑、原声保留与背压 |
| `bootstrap.c/.h` | 会话私有TTS broker身份、启动截止、精确后代回收与socket清理 |
| `mode_gate.c/.h` | 私有手机/可信动作结果请求、独立metadata gate、网络子进程FD白名单；当前手机媒体仍拒绝 |
| `media.c/.h` | 有界本地回铃，复用唯一player，保留CONNECTING且不消耗云端回复generation |
| `account.c/.h` | 上传脚本 `--model-request`、增量文字、分句和三对已确认历史 |
| `tts.c/.h` | 直接接热 TTS broker、两槽 URL 预取、单个解码器、固定 PCM 环 |
| `voice_guard.c/.h` | 原厂处理后音频的 VAD、实际播放参考的相关性防回声、手动 dB 增益 |
| `io/` | 一个固定 ALSA 播放线程、一个灯光线程、被动采音接口 |
| `asr/` | 当前音箱自己认证的小米 ASR；完整问题端点、生产时间戳和语句身份 |
| `adapters/` | 小智、豆包纯网络/编解码适配器；不读取麦克风、不播放、不控制灯 |
| `vendor/libfvad/` | 保留许可的开源 VAD，来源见 `asr/origin.json` |
| `tests/` | 状态机、真实调度循环和受控音频 I/O 的离线反例测试 |
| `build.py` | host/原厂 ARM glibc 2.25 构建、符号上限、源码摘要和栈使用 |

`account_fixture.c` 是离线测试入口，**不能通配加入生产链接**。生产源码表在
`build.py`；ARM 构建禁止测试专用的 ownership bypass。`adapters/support-origin.json`
记录迁入的小智/豆包依赖来源，不应重新依赖 `scratch` 下的开发入口。

## 不可破坏的约束

1. 必须由 `session-run` 的有效 lease 启动；真实官方“小爱同学”始终可最高优先抢占。
2. 控制词确认后，第一步撤销本地 PCM 队列并通知 ALSA drop；禁止先等云端 ACK。
3. partial 只用于控制词和端点观察，不能作为完整用户问题发送模型。
4. 每条迟到结果保留原 epoch/generation/utterance；不能替旧音重新贴新回复身份。
5. 活动会话所有状态都有灯效。片段完成只回倾听或思考；只有资源释放后 CLOSED
   可产生 END。CLOSING 仍亮；灯光故障不阻塞收音/停播。
6. 输出音量参考仅来自 ALSA 实际接受的 PCM。防回声单元测试不代表任意房间声学通过。
7. 网络、PCM、文字和取消回收均有固定上限；不清 cache、不放宽原内存/安全保护。
8. 用户凭据和原文不得记录到诊断日志。DATA 只保存账号、配置和用户唤醒模型等必要内容。

## 构建与验证

在项目根目录使用 WSL：

```sh
python3 runtime/dialogue-core/build.py --output build/dialogue-core-host-new
python3 runtime/dialogue-core/build.py --output build/dialogue-core-arm-new --arm
```

每次输出目录必须是 `build` 下新的直接子目录，旧候选不可覆盖。
ARM 链接原厂 2.25 库，`LX06_LEGACY_LIB` 和 `OPUS_LIBRARY` 可显式指定库位置。
完整源码/依赖摘要和最大单函数栈占用见输出 `manifest.json` 和 `stack-usage.json`。
构建成功仅证明编译与符号检查；实际 SDK/云端、扬声器和长期稳定性另行验收。

离线测试入口：`test_account.py`、`test_tts.py`、`tests/test_control.c`、
`tests/test_session.c`、`tests/test_voice_guard.c`、`tests/test_reactor.py`、
`io/tests/test_io.py`、`adapters/tests/`。这些默认不使用真实账号或音箱。
各报告区分 host、sanitizer、原厂 ARM/QEMU 和实际音箱；不得互相替代。

`tests/test_audio_reactor.py` 使用真实主循环与真实网络适配器，模拟ASR、loopback
云协议和ALSA。当前XZ/DB各两种模式的4项host sanitizer组合通过，包括250ms云端
取消屏障前的本地drop和完整续问。模式1用例仍用控制词触发，不能当作人声VAD或
房间防回声验收。脚本入口兼容缺口见
`../../docs/unified-script-migration-20261101.md`；broker生命周期与ARM未验证边界见
`../../docs/unified-bootstrap-lifecycle-20261101.md`。这些候选不能替代当前正式发布。

手机/动作内部接线见`../../docs/unified-phone-action-wiring-20261101.md`；
`tests/test_mode_gate.py`和`tests/test_mode_reactor.py`只用本地synthetic端点。
当前手机`call_ready=false`，main不会把探针附着或metadata READY当作可用通话；
动作结果入口仅播报已有结果，不执行实体操作。真实路由发送者和媒体仍待迁移。

## English overview

`lx06-dialogued` is the sole controller for added microphone, playback, lighting,
interruption and conversation features. Uploaded scripts retain authentication,
model discovery and provider requests. Network-only adapters cannot operate the
speaker hardware or decide control words. The original Xiaomi audio/authentication
services remain in place.

The reactor uses bounded queues and explicit session/reply/utterance identities.
Cancellation invalidates local PCM before remote work. Only genuine ASR finals
become model questions. Lighting stays active throughout connecting, listening,
thinking, speaking and teardown; ending is emitted only after owned resources
have been reclaimed. This is an unreleased candidate. Offline test results do
not establish acoustic accuracy, real provider compatibility or device stability.
