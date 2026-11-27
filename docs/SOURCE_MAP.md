# 源码地图

除小智客户端外，下表路径均相对于 kws/lx06-installer。

| 路径 | 用途 |
| --- | --- |
| main.go、app.go | Windows 程序入口、嵌入界面、固件列表和本地 API。 |
| web/index.html | Windows 安装器页面。驱动提示、选固件、显示进度和日志。 |
| flash*.go | USB 流程、镜像与 SHA-256 检查、刷写和 DATA 选项。 |
| driver_compat_windows.go、elevhelper/ | 驱动识别、签名错误诊断和 UAC 提权。 |
| hooks/webd/webd.c | 音箱 Web88 HTTP 服务、登录、日志、脚本上传和固件升级。 |
| hooks/webd/index.html | 音箱管理网页。包含本地唤醒、脚本扩展、降噪和在线升级。 |
| hooks/webd/app-extension*.h、runtime/model-adapter/script-settings.c | 解析脚本提供的字段和流程。统一验证、保存和读取设置。 |
| hooks/upgrade/ota-verify.c | 流式检查 LX06OTA1 的格式、目标型号、容量和 SHA-256。 |
| scripts/online_upgrade_package.py | 用原厂 Android boot 和增强 SquashFS 生成或验证 OTA。 |
| hooks/wakeword/wakeword_template.c、wakeword_joint.inc | 本地 MFCC 匹配、比较及同一段语音的检测状态。 |
| hooks/wakeword/wakeword_calibrate.c、ww_store.c、ww-*.sh | 录音、样本存储、校准、训练、删词和云端复核。 |
| hooks/xiaoai/processed-mic-tap.c、processed-mic-read.c | 读取原厂降噪后的麦克风数据，供唤醒和会话使用。 |
| hooks/xiaoai/session-run.c、official-router.c | 会话所有权、原厂小爱仲裁及脚本调度。 |
| runtime/common/control_asr.c | 通过原厂语音接口识别打断词。 |
| runtime/dialogue-core/ | 通用会话、音频输入输出和平台协议接口。 |
| runtime/model-adapter/ | 账号登录、模型请求、配置声明和问候音频。 |
| runtime/doubao/src/ | 豆包 C 客户端。协议、播放队列、回声过滤和打断。 |
| kws/scratch/xz-client-rewrite/src/main.c、protocol.c | 小智 C 客户端、Tenclass WebSocket、上传和预留换气时间。 |
| kws/scratch/xz-client-rewrite/src/player.c | 小智 ALSA 播放队列、播完和主动取消。 |
| scripts/examples/user-scripts/V20261127.5-rc42/ | 七个发布脚本及它们的配置界面声明、嵌入载荷。 |
| hooks/unified/、hooks/init.d/、hooks/robust/ | SSH89、设备启动和目标固件 ABI 兼容。 |
| scripts/build_*.sh、scripts/build_release.py | 交叉编译、生成固件和 Windows 安装包。 |
| tests/、各模块 tests/ | 校验协议、配置、OTA 和播放器行为。 |
| `kws/scratch/xz-client-rewrite/src/task_poll.c` | 按小智会话隔离 MCP 任务、管理轮询顺序。 |
| `scripts/xz_mcp_task_state.sh` | 接收 MCP 网关提交的任务编号、状态和自定义查询指令。 |
| `runtime/model-adapter/mcp_task_watch.sh` | 用小爱通道词启动独立 MCP 工具，并在当前音箱播报结果。 |

rc39 增强镜像基于已检验的 rc37 镜像，新增 rc39 版本记录。改小智和豆包时，可单独重编对应 C 客户端后重新打包脚本。机器可读的逐文件用途和符号见 [source-manifest.json](../source-manifest.json)。
