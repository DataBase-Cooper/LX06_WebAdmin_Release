# LX06 脚本开发指南

本指南以 `V20261127.5-rc42` 的 Web88 为例。你可以把这里的完整示例复制成 `.sh`，先在自己的 LX06 验证，再让 AI 按同样的入口和规则编写业务代码。一个脚本可以带配置表单、接收唤醒或通道词、调用自己的服务，并记录可用于排查的运行结果。

## 1. 先决定脚本怎么启动

| 想做的功能 | 使用入口 | 实现建议 |
| --- | --- | --- |
| 点网页按钮做一次检查，例如测试 API | `--web-action 动作名` | 声明一个扩展按钮，从标准输入读取参数，用逐行 JSON 返回结果。 |
| 打开网页读配置、点击保存 | `--web-action load`、`--web-action save` | 声明字段和对应动作。需要持久化时可使用设备的 `script-settings`。 |
| 说「小爱同学」后说一句固定指令，例如查询工单 | `--official-command 识别到的通道词` | 声明「小爱通道指令」。入口运行一次业务逻辑，结束后退出。 |
| 说本地唤醒词开启一次对话 | `--wakeup 唤醒词` | 声明 `LX06-LOCALWAKEUP`，上传后在词条中绑定。需要麦克风和播放时复用受管会话。 |
| 通过官方通道词切换到新的语音对话 | `--wakeup 识别到的通道词` | 声明「小爱通道唤醒词」。选择同样的会话入口处理收到的唤醒词。 |
| 任务需要后台完成，再由小爱播报 | 通道词入口交给受管后台任务 | 参考 `mcp_task_watch.sh` 的交接、超时和卸载。把后台工具逻辑和语音会话的生命周期分开。 |

先只做一种入口。例如 API 查询可以先做「扩展 → 测试」，验证服务和返回值后，再加语音入口。需要接手麦克风的脚本可从现有小智或豆包脚本开始，普通 HTTP 动作可以用较短的 Shell 脚本。

## 2. 文件开头和参数分派

- 用 UTF-8、LF 换行保存。文件名建议用英文、数字、下划线和连字符，后缀 `.sh`。第一行是 `#!/bin/sh`。
- LX06 使用 `/bin/sh`。使用 `case`、`[ ]` 和 `printf` 等 POSIX Shell 写法，避免 Bash 数组、`[[ ]]` 等专属语法。
- 把声明写在脚本开头的连续注释里。Web88 在这个区域检测脚本能力和扩展表单。扩展 JSON 可以用一行，也可以每行以 `#` 开头。示例使用单行 JSON，便于复制。
- 先在 `case` 中分派 `--web-action`、`--official-command`、`--wakeup` 和必要的卸载操作。默认无参数分支只做安全初始化或输出说明。上传、打开扩展、打开日志和点击保存都应有明确的副作用边界。
- 使用 `[ "$#" -eq N ]` 核对参数个数，并用 `"$变量"` 保留原始字符串。把语音、表单和远端响应作为数据处理。

### 常用声明

| 声明 | 用途 |
| --- | --- |
| `# LX06-EXTENSION-BEGIN` / `# LX06-EXTENSION-END` | 包住一个扩展 JSON 对象，决定 Web88 显示哪些配置。 |
| `# 小爱通道指令=查询工单,查询状态` | 官方小爱识别到其中一句时，调用 `--official-command`。 |
| `# 小爱通道唤醒词=进入对话,开始对话` | 官方小爱识别到其中一句时，调用 `--wakeup`，供语音会话脚本使用。 |
| `# LX06-SETTINGS: official_command` | 允许脚本通过设置项 `official_command` 配置小爱通道指令。需要在扩展 JSON 里声明该字段。 |
| `# LX06-SETTINGS: official_wake` | 允许脚本通过设置项 `official_wake` 配置小爱通道唤醒词。 |
| `# LX06-LOCALWAKEUP: on` | 让脚本可以作为本地唤醒绑定目标。脚本要实现 `--wakeup`。 |
| `# LX06-RUN: root` | 声明需要系统身份运行。仅用于确实要访问系统组件的脚本，例如调用官方 TTS 的受管脚本。 |
| `# LX06-RESIDENT: on` | 描述脚本有常驻模式。开机运行仍由 Web88 的自启动功能管理。 |
| `# WEBD-UNINSTALL-TIMEOUT: 5` | 请求卸载时执行 `--uninstall`，以秒为限。清理范围应限定在脚本自己创建的资源。 |

固定通道词示例的逗号分隔是声明格式。扩展中的 `format:"aliases"` 也用于通道词字段，可以留空关闭。做语音路由时，先区分「执行一个动作」和「建立一段对话」，再选对应的声明。

## 3. 完整示例一：网页输入与测试按钮

保存为 `hello_extension.sh`。它只接收一个文本字段，在「测试」按钮完成后显示确认消息，适合验证扩展解析和调用链。

```sh
#!/bin/sh
# LX06-EXTENSION-BEGIN
# {"version":1,"title":"脚本测试","description":"输入一句话，点击测试。","fields":[{"name":"text","label":"测试内容","type":"text","default":"你好","maxBytes":160}],"actions":[{"id":"preview","label":"测试","fields":["text"]}]}
# LX06-EXTENSION-END

set -u

case "${1:-}:${2:-}" in
    --web-action:preview)
        text=$(jsonfilter -e '@.values.text' 2>/dev/null) || {
            printf '%s\n' '{"message":"读取测试内容失败。"}'
            exit 2
        }
        if [ -z "$text" ]; then
            printf '%s\n' '{"message":"请输入测试内容。"}'
            exit 2
        fi
        printf '%s\n' '{"message":"已收到测试内容。"}'
        ;;
    --web-action:*)
        printf '%s\n' '{"message":"未知操作。"}'
        exit 2
        ;;
    *)
        printf '%s\n' '脚本已就绪，请在 Web88 扩展中测试。'
        ;;
esac
```

上传后，在「脚本维护」打开这个文件的「扩展」，改一下测试内容，再点击「测试」。在音箱命令行复现时，可使用和 Web88 相同的输入格式。

```sh
printf '%s\n' '{"name":"hello_extension.sh","action":"preview","values":{"text":"你好"}}' |
  /bin/sh /data/lx06/apps/hello_extension.sh --web-action preview
```

期望返回 `{"message":"已收到测试内容。"}`，退出码为 `0`。把 `text` 改成空字符串时，会返回提示且退出码为 `2`。示例不会把用户输入直接拼入 JSON。需要返回输入或远端文本时，使用正规的 JSON 编码工具，把字段作为字符串编码。

## 4. 完整示例二：保存设置和用语音触发

保存为 `lx06_status_demo.sh`。扩展可以改演示文字和通道词。本地词或小爱通道词命中时，脚本读取已保存的值，在自己的日志中记录一次结果。它使用固件的持久设置工具，因此声明系统身份。示例本身只读配置和写运行日志，便于先验证路线。

```sh
#!/bin/sh
# 小爱通道指令=查看脚本状态
# LX06-SETTINGS: official_command
# LX06-LOCALWAKEUP: on
# LX06-RUN: root
# LX06-EXTENSION-BEGIN
# {"version":1,"title":"脚本状态演示","load":"load","fields":[{"name":"message","env":"LX06_DEMO_MESSAGE","label":"状态文字","type":"text","default":"状态正常","maxBytes":160},{"name":"official_command","env":"LX06_DEMO_COMMAND","label":"小爱通道词","type":"text","format":"aliases","default":"查看脚本状态","maxBytes":96}],"actions":[{"id":"load","label":"读取配置","fields":[]},{"id":"save","label":"保存","fields":["message","official_command"]}]}
# LX06-EXTENSION-END

set -u
SETTINGS=/usr/lib/lx06/script-settings
SCRIPT=/data/lx06/apps/lx06_status_demo.sh

case "${1:-}:${2:-}" in
    --web-action:load) exec "$SETTINGS" show "$SCRIPT" ;;
    --web-action:save) exec "$SETTINGS" save "$SCRIPT" ;;
    --web-action:*) printf '%s\n' '{"message":"未知操作。"}'; exit 2 ;;
    --official-command:*|--wakeup:*)
        [ "$#" -eq 2 ] && [ -n "$2" ] || exit 2
        exec "$SETTINGS" run "$SCRIPT" /bin/sh "$SCRIPT" --report
        ;;
    --report:)
        printf '[状态演示] %s\n' "${LX06_DEMO_MESSAGE:-状态正常}"
        ;;
    *) printf '%s\n' '脚本已就绪，请先在扩展中配置。' ;;
esac
```

1. 保持文件名为 `lx06_status_demo.sh` 上传，示例的 `SCRIPT` 路径才能与安装文件匹配。改名时同时改 `SCRIPT`。
2. 打开「扩展」确认可以读取默认值。改「状态文字」，保存，关掉扩展后再打开，确认回读的是新值。
3. **测试小爱通道词。** 让「小爱同学」继续进入官方小爱，再说「查看脚本状态」。在脚本日志里查看保存后的文字。可将通道词清空后再试，确认原口令已关闭。
4. **测试本地唤醒。** 在已训练的词条里绑定本脚本，启用后说出该词。脚本日志应出现同一条状态信息。示例通过日志确认触发，语音播报可以在后续版本按实际需要加入。

### 设置工具的三个入口

| 调用 | 作用 |
| --- | --- |
| `script-settings show 脚本路径` | 读取声明及已保存值。密码值不会回填到网页。 |
| `script-settings save 脚本路径` | 从 stdin 读取动作 JSON、校验并保存。调用 `save` 时按声明同时提交该动作需要的全部字段。 |
| `script-settings run 脚本路径 命令 参数…` | 读取已保存配置，将声明了 `env` 的值传给子进程。给需要的设置提供运行时缺省值。 |

实际脚本应处理空值、旧配置和同名更新。支持接收用户凭据的 `password` 字段没有非空默认值。`script-settings save` 中提交空密码时会保留已有密码。账户退出应通过明确的操作实现。

## 5. 设计 Web88 扩展表单

一个声明通常由以下几部分组成。

| 键 | 如何填写 |
| --- | --- |
| `version` | 当前使用整数 `1`。 |
| `title`、`description` | 说明用户打开的是哪项配置，给出完成这一步所需的信息。 |
| `fields` | 每个字段包含 `name`、`label`、`type`。用 `default` 给新配置提供默认值，使用 `env` 供运行时读取。 |
| `actions` | 每个按钮给出 `id`、`label` 和完整的 `fields`。无参数动作的 `fields` 是 `[]`。 |
| `load` | 可选。填一个无参数动作的 ID，打开扩展时自动读取状态。 |
| `conditions`、`visibility` | 声明何时显示字段、分组或动作。适合分模式配置。 |
| `flow` | 可选。适用于「连接账户 → 对话设置」等明确步骤。 |

字段类型包括 `text`、`password`、`textarea`、`number`、`checkbox` 和 `select`。用 `options` 列出下拉选项，用 `default` 指定选项值。`default` 和 `values` 中的值按字符串处理，复选框使用 `"true"`、`"false"`。可用 `group` 收纳高级设置。`widget:"segments"` 可把选项做成分段按钮，开关和滑块也由声明控制。字段最大字节数可用 `maxBytes` 收紧。常见格式有 `integer`、`words`、`aliases` 和 `wss`。

在 rc42 中，扩展声明最多 24 个字段、12 个动作，JSON 声明和一次提交各最多 8192 字节。字段和动作名以英文开头，后面可用英文、数字、`_` 和 `-`，最多 32 字节。给短词、URL、短代码和长文本分别设置合适的限制。

### 同一表单按模式显示不同字段

例如，某个自定义音箱脚本有打断词、人声两种模式。把 `interrupt_mode`、`interrupt_phrase`、`voice_threshold` 声明在 `fields` 里，其中模式使用 `select`，选项值是 `word` 和 `speech`。然后在**同一个扩展 JSON 对象**添加以下两组规则。

```json
{
  "conditions": [
    {"id":"word-mode","source":"settings.interrupt_mode","equals":"word"},
    {"id":"speech-mode","source":"settings.interrupt_mode","equals":"speech"}
  ],
  "visibility": [
    {"target":"fields.interrupt_phrase","conditions":["word-mode"]},
    {"target":"fields.voice_threshold","conditions":["speech-mode"]}
  ]
}
```

点选模式时会即时切换显示。可用 `notEquals` 表达「只要不是关闭就显示」，例如选项值是 `0`、`1000`、`3000` 时用 `notEquals:"0"`。字段显隐只管界面，业务代码仍按配置的模式使用相应参数。`save` 动作的字段清单保持完整，保留其他分支的设置，方便用户切回来继续使用。

### 登录步骤和动态选项

需要授权的脚本可把配置分成「连接」和「设置」两步。连接成功后，脚本返回 `step` 切到设置页。`load` 可读取当前授权状态，并根据结果回到正确步骤。定时检查 `watch` 用无参数动作。实际的账户判断和授权信息由脚本处理。

用户可更改的模型列表可用 `dynamic:true` 的 `select`。脚本成功获取可用列表后，输出 `options` 对象。`label` 是给用户看的名称，`value` 是保存及调用 API 所用的真实 ID。失败时让动作非零退出，保留此前成功的列表和选项。

```json
{"message":"已更新模型列表。","options":{"model":[{"value":"standard-v1","label":"标准"},{"value":"fast-v1","label":"快速"}]}}
```

完整的 `flow`、动态目录及回复对象规则见扩展协议。[Web88 脚本扩展协议](../kws/lx06-installer/docs/script-extensions-v1.md)

## 6. 输入、输出、错误与账户数据

Web88 执行 `/bin/sh 脚本路径 --web-action 动作名`。stdin 提供完整 JSON，包括 `name`、`action` 和 `values`。用 `jsonfilter` 或你提供的 JSON 解析器读取，始终校验参数。模板里的 `--web-action` 分支应在执行网络调用、启动会话和业务动作之前明确路由。

扩展动作的 stdout 是 **NDJSON**，一行一个 JSON 对象。`message` 提示用户当前发生了什么，`values` 可以回填已声明的普通字段，`options` 用于动态列表，`link` 指向 HTTPS 授权页面。短期用户验证码可放到 `code`，按协议开启复制按钮。正常运行日志使用脚本自身的日志入口。扩展单行最多 8192 字节，整个动作公开输出最多 64 KiB。动作正常完成时用 `exit 0`，失败时给出明确原因并用非零退出码。

有账号或网络请求时，给 AI 明确这些边界。

- 令牌、手机号和密码由用户通过扩展填写。避免写入脚本默认值、输出消息、调试日志和命令参数。
- 用标准 JSON 编码或 `curl --data-urlencode` 处理字符串。把用户数据传给子程序作为参数或 stdin，避免 `eval`、拼接 `sh -c` 或把输入当 Shell 配置文件执行。
- 对服务请求设连接超时、总时间及响应体上限。无响应时明确报告原因，避免把退出失败当成功。可能造成实际操作的工具应规定重试行为，防止重复提交。
- 临时文件用 `mktemp` 创建，退出时清理。确实需要保存的数据放在脚本私有空间，权限收紧，写入使用临时文件后原子替换。脚本日志记录状态和短错误原因，限制大小并清理临时文件。
- 扩展动作和语音主任务可能由不同工作目录和身份运行。使用明确的持久配置接口，避免依赖打开网页时的临时目录或环境。

## 7. 语音和后台任务的写法

### 只做一次动作

匹配小爱通道词后，在 `--official-command` 进入业务方法。方法完成后退出，常见用途是读取状态或发起一个有界请求。Web88 的按钮可以先提供参数测试。测试动作和业务动作使用不同的入口和标签，让用户清楚哪一步会执行真实操作。

### 音箱说出结果

需要官方小爱播报时，可参考发布包的 `mcp_task_watch.sh`。它检查会话和播放器状态，用 `jshn` 编码要说的文本，再调用系统正式的 `mibrain` 文本转语音接口。播报使用发起任务的设备。注重现有会话、音频资源与返回状态，尤其要处理系统正在对话或播报的情况。[独立 MCP 脚本](../kws/lx06-installer/runtime/model-adapter/mcp_task_watch.sh)

### 持续对话、打断和结束

需要占用麦克风和播放的脚本可参考小智、豆包，使用 `/usr/lib/lx06/xiaoai/session-run` 管理会话生命周期，使用现有处理后的麦克风流。处理好播放器、网络连接及终止信号的清理。打断方式属于脚本的配置。设计时明确哪些输入中断当前播报、哪些结束整场对话，以及打断后怎样接着接收用户的新话。[源码与脚本目录](../kws/lx06-installer/scripts/examples/user-scripts/V20261127.5-rc42)

### 长任务与结果路由

- **独立任务。** 通道词入口交给受管后台进程后退出，后台进程管理请求超时和结果。参考独立 MCP 脚本的实现。它使用 MCP Streamable HTTP 调用已配置工具，并等待可播报的最终文本。
- **小智会话内任务。** 网关立即确认任务已提交，并保存发起音箱及本次会话标识。为每个任务提供独立编号、状态和只读查询指令。小智在原会话内排队查询，结束会话后停止自动播报。

两个场景的网关接口和时序见接入说明。[MCP 接入与状态约定](../kws/lx06-installer/docs/xiaozhi-mcp-task-polling.md)

## 8. 从本地自检到真机验收

1. **先检查语法。** 在 Linux 或 WSL 运行 `sh -n 你的脚本.sh`。查看文件是 LF，且扩展声明为有效 JSON。
2. **检查固件识别。** 在自己的测试音箱上运行 `/usr/sbin/lx06webd --check-extension /path/to/你的脚本.sh`。这个入口只检查声明。确认 `ok` 和 `declared` 都为 `true`，字段和动作数量符合预期。
3. **验证输入与返回。** 用一份正常 JSON 和一份错误 JSON 通过 stdin 调 `--web-action`。分别检查退出码和逐行 JSON。需要网络的操作先用测试服务或只读工具验证。
4. **验证 Web88。** 上传后检查按钮、初次读值、保存后重开、模式切换时即时显隐、修改前已有的设置，以及网络错误时的提示。
5. **验证语音入口。** 分开测试本地词绑定和小爱通道词。每次核对识别记录、脚本日志和实际结果。做连续对话时补测打断后还能听到用户说话、结束后释放播放和麦克风。
6. **验证生命周期。** 对长任务检查超时、失败、重复触发、后台任务播报及卸载清理。换同名版本时先停旧脚本，保留对业务有用的已保存配置。

| 遇到的问题 | 先检查什么 |
| --- | --- |
| 脚本上传后没有「扩展」 | `--check-extension`、文件头、JSON 和固件版本。 |
| 能看到按钮，点了显示失败 | 动作 ID、参数列表、stdin 格式、退出码。显示给用户的消息使用单行 JSON。 |
| 切换模式后字段不变 | 输入框真实的选项值、条件引用的字段名、目标 `fields.字段名` 和 Web88 版本。 |
| 能保存，语音触发时仍用旧值 | 唤醒入口有没有通过 `script-settings run` 读取配置。正在进行的会话可结束后再验证。 |
| 官方小爱能回应，但通道词没走到脚本 | 检查是否启用官方唤醒、官方词当前绑定、通道词是否保存，以及用户脚本是否允许运行。 |
| 本地词有候选记录，但没有说话 | 对照实际提交唤醒和目标脚本启动日志，检查是否完成训练、已启用并绑定到支持唤醒的脚本。 |
| 进程完成了，页面还显示旧状态 | 检查 `load` 是否返回本次状态、JSON 动作是否成功退出，以及临时任务是否仍在运行。 |

## 9. 可以直接交给 AI 的提示词

把下方内容复制给 AI，并附上本指南、扩展协议及与你任务最接近的一个示例。描述你的工具调用协议时可使用假数据和占位符。

```text
请为我的小爱音箱 Pro LX06 写一个 Web88 rc42 可上传的 Shell 脚本。
先读本仓库的 docs/SCRIPT_AUTHORING.md 和脚本扩展协议，按现有固件入口实现。

功能：[例如查询我的工单，并在工具完成后由发起音箱的官方小爱播报]
触发方式：[网页按钮 / 小爱通道词 / 本地唤醒绑定]
服务协议：[HTTPS 请求或 MCP Streamable HTTP，提供脱敏示例请求和响应]
配置：[字段名、类型、默认值、范围、是否保密]，模式切换后即时显隐。
输出：[页面应显示的结果、应该播报的文字、失败和超时提示]
生命周期：[一次执行 / 对话会话 / 受管后台任务]

给我完整的 POSIX /bin/sh 脚本。将设置和业务留在脚本里，复用 Web88 的声明和配置工具。
把 --web-action、--official-command 和 --wakeup 中实际需要的入口分别实现，说明默认分支的效果。
所有请求有超时和返回校验。访问令牌用用户输入，无预置凭据。权限按实际需要选择。
给出上传和配置步骤，并提供 sh -n、扩展声明检查、正常和失败输入示例及真机验收清单。
写清楚用的是哪种语音入口，实际测试到了哪一步。需要额外设备程序时，列出名称和用途。
```

## 10. 源码入口

- Web88 解析和执行扩展可从 `hooks/webd/app-extension.h`、`app-extension-api.h`、`app-extension-runner.h` 与 `index.html` 开始读。
- 配置读写在 `runtime/model-adapter/script-settings.c`。小智、豆包表单由 `runtime/model-adapter/voice_settings.py` 生成。
- 官方通道词路由与会话由 `hooks/xiaoai/official-router.c`、`session-run.c` 管理。
- 新业务可以参考七个配套脚本的实际入口和声明。[源码地图](SOURCE_MAP.md) · [编译说明](BUILD.md)
