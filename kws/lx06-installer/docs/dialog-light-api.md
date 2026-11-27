# LX06 对话灯光接口 v1

固件复用 `/bin/show_led`、`/bin/shut_led` 使用的原厂 `ubus led show/shut` 接口及 `ledserver` 的 LX06 灯效，不直接写 RGB、GPIO 或 I²C。相同的静默开关由管理器检查，夜间效果由原厂服务控制；省去每次 shell/cat/logger 的进程开销，不新增常驻灯光进程，不写 DATA。

| 状态 | 调用 | 行为 |
| --- | --- | --- |
| 唤醒 | `lx06-dialog-light wake` | 原厂效果 1；会话管理器启动时自动发出 |
| 等待用户说话 | `lx06-dialog-light listening` | 原厂效果 1 |
| 等待云端回复 | `lx06-dialog-light thinking` | 原厂效果 2 |
| 实际播报 | `lx06-dialog-light speaking` | 原厂效果 3 |
| 打断后继续听 | `lx06-dialog-light interrupted` | 清播报/思考灯，回到效果 1 |
| 结束 | `lx06-dialog-light ending`（或 `idle`） | 复用 ready 的效果 11，再关闭对话效果 |

原厂没有独立的“打断颜色”；打断体现为从播报灯回到聆听灯。没有从本地 MFCC 取得声源角度，效果 1 使用原厂保留/默认位置，不能声称有真人方位跟随。贾维斯路由到官方时继续由官方控制灯光，无须再运行本接口。

## Shell 脚本接入

在真正的单次会话进程启动处启用。常驻监听器不用占有灯光；录音预览等无对话任务保持默认关闭。

```sh
XA=/usr/lib/lx06/xiaoai
LX06_DIALOG_LIGHTS=1 LX06_SESSION_MAX_SECONDS=900 \
  "$XA/session-run" run my_voice.sh sh "$0" --session-worker
```

在 `--session-worker` 或其子进程中随真实事件通知：

```sh
lx06-dialog-light listening || :
# 收到有效用户语句，等待云端答复：
lx06-dialog-light thinking || :
# 播放器开始送出音频：
lx06-dialog-light speaking || :
# 停止并回收旧播放器后：
lx06-dialog-light interrupted || :
# 等本地队列和播放器全部结束后：
lx06-dialog-light listening || :
# 关闭会话时；即使遗漏，session-run 也会清理：
lx06-dialog-light ending || :
```

这是状态提示接口，不会替脚本取消云端响应、停止音频或关闭网络。脚本仍须完成这些操作。`ending` 后如果还有告别语，应在实际播放时上报 `speaking`，播完退出；不能收到云端发送完成就当作本地播放完成。

## C 客户端

构建时使用固件配套 `dialog-light.h`（固件路径 `/usr/include/lx06/dialog-light.h`）：

```c
#include <lx06/dialog-light.h>
(void)lx06_dialog_light(LX06_LIGHT_THINKING);
```

通知用继承的 `LX06_DIALOG_FD` 和 `LX06_DIALOG_CHANNEL`。不要自行保存/复用这些环境变量，不要清除该 FD；Python 的 `subprocess` 若启动会话子程序，应使用 `pass_fds=(int(os.environ['LX06_DIALOG_FD']),)`。脚本没有启用或不在受管理会话内时，CLI 返回 1，不影响音频业务；非法状态返回 2。

## 生命周期与资源

每场会话独享一个非阻塞 Unix socketpair。通知去重并合并，音频线程不等待 LED 后端。会话稳定时约每秒检查一次原厂灯效状态，当前效果仍在时保持原动画。原厂服务清掉当前效果时，只重新点亮当前会话效果。静默模式暂停恢复。单次后端最多等待 1.8 秒。正常退出、脚本崩溃、超时、内存保护、官方接管、本地切换都在释放会话锁前清理灯光。旧通道无法操作新会话。只触碰对话效果 1、2、3、9、11，保留静音、联网等其他系统提示。

脚本会话显示灯效期间独占 `/tmp/lx06-dialog-led/lease`。原厂 `shut_led` 通过同一锁串行处理效果 1、2、3 的关闭请求。后台晚到的关闭请求在脚本拥有灯效期间被忽略。会话清理后立即归还灯效，随后原厂清理照常执行。

日志可见 `dialog-light: state=...`。后端超时/失败会明确记录，仍及时释放会话资源；如果原厂灯光服务本身损坏，不能保证物理灯已经熄灭。本接口不会重启原厂引擎。播放器首个成功 PCM 写入是播音灯触发点，不等同于用测量设备确认的扬声器物理发声时刻。

会话管理器保留 MemAvailable 的启动 20 MiB / 运行 12 MiB 下限。另在 MemAvailable 低于 24 MiB 时，检查实际 MemFree：启动至少 8 MiB，运行至少 6 MiB；低于保护线则结束用户会话，保留官方引擎。可回收缓存充足时不能单凭 MemFree 偏低就杀掉正常会话。此组合覆盖本次观测的 OOM 和误拒绝状态，不保证任意碎片化状态零 OOM。
