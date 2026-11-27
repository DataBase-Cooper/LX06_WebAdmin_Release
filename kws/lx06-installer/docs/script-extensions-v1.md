# 脚本扩展窗口 / Script extension dialog v1

本页记录 Web88 扩展声明协议。rc42 的小智、豆包和各平台账户脚本均使用此通用入口。编写第一份脚本时，可先从示例开始。[LX06 脚本开发指南](../../../docs/SCRIPT_AUTHORING.md)

## 用户界面

声明有效扩展的已部署脚本，在“日志”前出现“扩展”。点击打开与修改密码
同样的小窗口。原有脚本不声明则不出现按钮，不改变其运行方式。
窗口支持多个按钮、文本、密码、数字、复选、下拉、多行输入。参数名、类型、
说明、默认值、按钮文字和每个按钮提交哪些参数，全部定义在上传的脚本中。

```sh
#!/bin/sh
# LX06-EXTENSION-BEGIN
# {"version":1,"title":"模型账户","description":"使用自己的账户登录",
#  "fields":[{"name":"model","label":"模型","type":"select","options":["auto","fast"],"default":"auto"},
#            {"name":"question","label":"测试问题","type":"textarea","placeholder":"请输入问题"}],
#  "actions":[{"id":"login","label":"登录账户","fields":[]},
#             {"id":"ask","label":"测试连接","fields":["model","question"]},
#             {"id":"logout","label":"退出账户","fields":[]}]}
# LX06-EXTENSION-END

if [ "${1:-}" = --web-action ]; then
    # stdin 是完整 JSON：name、action、values。务必使用 JSON 解析，禁止 eval。
    # jsonfilter 可从 stdin 读取，下面只展示提取一个参数的方法。
    case "$2" in
      ask)
        question=$(jsonfilter -e '@.values.question') || exit 1
        # 将 question 作为数据传给自己的模型适配器；不可拼进 sh -c。
        printf '%s\n' '{"message":"已收到测试问题"}'
        ;;
      *) printf '%s\n' '{"message":"此示例尚未实现登录"}'; exit 2 ;;
    esac
    exit
fi
# 原有部署 / 常驻 / 唤醒入口继续写在这里。
```

## 声明和参数

声明必须在脚本开头的连续注释区域，扫描上限 16 KiB，JSON 上限 8 KiB。
最多 24 个字段、12 个按钮；静态下拉框最多 16 个选项。名称用英文字母开头，
之后允许字母、数字、`_`、`-`，最多 32 字节。重复或未知参数、未声明的动作、
错误类型均拒绝执行。窗口通过 DOM textContent 创建，不能插入 HTML/JS。

字段类型：`text`、`password`、`number`、`checkbox`、`select`、`textarea`。
`default` 和提交的 values **统一是字符串**，便于 BusyBox shell/jsonfilter
读取；checkbox 是 `"true"` / `"false"`，number 是有限数字的文本（允许空值）。
静态 select 必须命中 options。动态 select 的服务器目录规则见下节。每项值最多 2048 UTF-8 字节，整次请求最多 8 KiB。
密码不能声明非空默认值，提交后和关窗后从页面清除，不在页面保存账户密码。
按钮 `fields` 必须列出该动作使用的全部字段；登录按钮可用空列表。
前端提交仅包含这些字段。可选字段元数据：`group` 将字段放入折叠区；
`env` 声明通用设置工具传给子进程的环境变量；`min`/`max` 为整数数值边界；
`maxBytes` 限制 UTF-8 字节长度（1–2048）；`format` 支持 `integer`、`words`、
`aliases`、`wss`。`words` 禁止空词表，`aliases` 允许空字符串禁用通道。
单行类型禁止控制字符；多行内容必须由消费者 JSON 转义。业务约束仍在脚本声明。

顶层可选 `load` 指定一个无输入动作，例如 `"load":"load"`。打开窗口时，
状态空闲则自动执行它；首次读取失败不能保存默认值覆盖既有配置。正在执行其他
操作时不并发启动读取，操作结束后可点“读取当前配置”。所有扩展操作按钮与授权
链接采用现有浅色 secondary 样式，不用蓝色 primary 按钮。

## 根据设置即时显示字段

在脚本扩展声明中，用 `conditions` 对 `settings.<字段名>` 声明 `equals` 或 `notEquals`，再用 `visibility` 指定受控字段。普通控件、分段按钮和开关变更后，Web88 会立即用当前输入值重新计算显示状态。

```json
{
  "conditions": [{"id":"voice-enabled","source":"settings.reply_mode","equals":"voice"}],
  "visibility": [{"target":"fields.reply_volume","conditions":["voice-enabled"]}]
}
```

`reply_mode` 和 `reply_volume` 都在 `fields` 中声明。`equals` 与 `notEquals` 的值使用选项值字符串。相反的分支可声明另一条条件。脚本仍负责决定当前模式下实际使用哪些参数。

## 动态模型下拉目录（2026-10-31）

平台模型目录可以由上传脚本获取后交给通用扩展窗口，不需要用户手填模型 ID。只有
显式声明 `dynamic:true` 的 select 接受运行时选项，静态 select 的验证和行为保持原样：

```json
{"name":"model","label":"模型","type":"select","dynamic":true,
 "options":["default-model"],"default":"default-model","maxBytes":128,"env":"MODEL_ID"}
```

schema 的 `options` 仍是字符串数组，作为尚未拉取目录时的种子，可为空；`dynamic`
只允许 select 使用，必须为 JSON 布尔值。脚本成功获取服务器目录后输出一行：

```json
{"message":"已获取模型，请选择后保存","options":{"model":[
  {"value":"model-a","label":"模型 A"},{"value":"model-b","label":"模型 B"}]}}
```

每项只能有 `value` 和 `label` 两个字符串键，均非空且不含控制字符；value 最多
128 UTF-8 字节并同时受字段 `maxBytes`、`format` 约束，label 最多 160 字节。
每个字段最多 64 项，整个成功目录快照合计最多 64 项，NDJSON 单行仍不超过
8192 字节。重复 value、重复字段、未知字段、非 dynamic 字段、额外键、错误类型和
任何超限都拒绝整批，不静默截断模型。标签只通过 DOM `textContent` 展示，不能注入 HTML。

同一动作可分多行输出 `values` 和 `options`。目录按声明字段合并，未提及的字段保持
原目录；空数组表示清除此字段可新增选择的目录，仍保留默认/种子与已保存当前值。
合并后的总项数和字节边界再次验证。只有整个动作正常 exit 0，执行器才用一次原子
rename 提交目录和本动作已确认的当前值。非零退出、取消、超时、坏 JSON、超量输出
或暂存失败均保留前一份完整目录，平台明确执行下述 `forget-options` 除外。目录输出本身不更改 DATA 中的已保存模型。

页面将目录显示为下拉框，显示 label，提交 value 字符串。当前保存的 ID 即使不在
新目录中，也继续显示并保留，不会自动切到第一项。获取目录失败不清空配置和草稿。
浏览器 POST、受管执行器和 `script-settings save` 均重新检查：新增赋值必须命中
已成功提交的目录、schema 默认或种子；已确认/已保存的当前 ID 可以原样保留。
动态 select 不能退化为任意字符串输入，单改前端或伪造 POST 不会放开目录校验。

目录保存在该脚本扩展工作区的私有 `dynamic.json` TMP 缓存中，约 24 KiB 上限。
文件包含长度帧、精确 schema 字节和两份有界公开 JSON 快照，是内部格式，不能当作
普通 JSON 手工修改。声明变化后旧缓存不再提供允许列表；重启后重新获取，不新增 DATA
目录。所有文件检查所有者、0600、单硬链接及符号链接，工作区为受管 UID 的 0700；
卸载同时删除本脚本的缓存，不影响其他脚本。

扩展动作结束后 keeper 会回收槽位，下一次动作可能分配不同 UID。Web 在仍控制
0700 工作区时，仅用已经重新验证的快照重建缓存，并在发布输入文件前一并交给新 UID；
不会只改目录所有者而让缓存失效，也不会对旧动作留下的任意路径直接 chown。

执行器向子脚本导出 `LX06_EXTENSION_DIR`。`script-settings show` 可以附带已缓存的
`options`；若目录与全部配置合在一行会超过 8192 字节，仍成功返回配置，目录经状态 API
单独返回。`script-settings has-options 脚本路径` 可检查该声明的缓存：退出 0 表示
有有效目录，1 表示缺失或声明已变化，2 表示缓存或目录不可信/损坏。平台脚本可据此
仅在没有目录时拉取，保留独立“获取模型”按钮供用户刷新。它不查询平台，也不调用脚本。

平台脚本在发起新账号登录前或退出本地账号成功后，应在受管扩展动作内调用
`script-settings forget-options 脚本路径`。此通用原语只删除该动作工作区的动态 TMP
目录缓存，不改 DATA 设置、账户文件或公开状态；有效工作区与文件权限仍重新校验，
缓存缺失时幂等成功。没有受管工作区环境时按无缓存处理，不猜测或遍历其他脚本目录。
内部私有 `dynamic.reset` 标记阻止当前 runner 在 exit 0 时把启动前读到的旧目录重新
提交；动作结束后清理标记，下一次 load 可以重新拉取。已有模型 ID 继续保存，平台
目录与新账号权限是否兼容由重新获取的结果决定。这样 C 无需识别 ChatGPT、Grok 等平台。

状态 API 在原有 `event` 外另返回 `options` 对象和 `values` 对象或 null。前者仅来自
最后一次成功提交的目录；后者只有本次动作成功且实际输出了合法 values 才返回，
否则为 null，避免目录刷新把旧值回填覆盖草稿。内部状态的 `values_updated` 布尔值
由执行器设置，不能从浏览器请求传入。页面只在本次期待读取且成功后使用
`event.values || values` 回填，并保留等待中用户的新输入。状态响应总计有界为约
24 KiB 加固定封装；这不放宽每行 NDJSON、schema 或浏览器请求的 8 KiB 上限。

## 操作执行和返回

后台在 keeper 中启动独立的 `ext-<脚本名摘要>` 任务，不调用会停止同名会话的
run_app_profile。沿用原脚本 `# LX06-RUN: root` 权限声明；未声明则为独立
受限 UID。扩展任务的 HOME **不同于语音主任务 HOME**，同一扩展后续调用
复用自己的 HOME；不要假定两个任务共享工作目录。需要共享状态的 root
脚本应自行定义独立私有目录，权限 0700、账户凭据 0600，原子写入并加锁。

执行为 `/bin/sh 原脚本 --web-action 动作名`。JSON 经受保护临时文件转 stdin，
打开后立即 unlink，不出现在 argv 和 keeper 日志。脚本必须在任何部署、
启动或开门等副作用之前分派 `--web-action`。不支持 ELF 自定义扩展入口。

stdout 是逐行 JSON（NDJSON），每行最多 8192 字节、总计最多 64 KiB，仅允许：

```json
{"message":"请登录自己的账户","link":"https://官方认证站点/验证地址","code":"短期验证码"}
```

脚本必须在开头 8 KiB 内单独声明 `# LX06-COPY-CODES: on` 才启用复制按钮；缺少声明或 off 均不启用。这同时控制扩展、单独日志和日志跟踪，不根据脚本名称猜测。

`code` 必须是字符串：已声明的脚本在该值后显示中性样式的“复制”按钮，复制精确值，不包含“验证码：”标签。比如 `"001234"` 的前导零不会丢失，`"ABCD-EFGH"` 的连字符也保留。授权等待期间仍可点击复制，不受扩展操作按钮的忙碌状态影响。不要在 `code` 中输出 Cookie、access_token、refresh_token 或完整鉴权响应；只输出用户应当填写的短期代码。完成/失败后的事件应省略 `code`，以清除旧代码。

普通脚本也可输出 `绑定码 001234` 或 `验证码：ABCD-EFGH`。单独日志与“日志跟踪”会为明确标注的代码附加复制按钮，不要求脚本实现网页或剪贴板逻辑。复制仅在用户点击时发生；HTTP 局域网访问使用兼容的选择复制方式，HTTPS 优先使用 Clipboard API。脚本日志与远端账号状态不会因复制而改变。

上述三项均可省略；另外支持 `"values":{"已声明字段":"当前值"}` 和上节的动态 `options`。
执行器和状态 API 都重新验证 values，禁止 password、未知键、重复键、错误类型
或范围。页面只在本次动作成功结束时回填，不用旧状态覆盖表单，也保留等待期间
用户新输入的内容。只保留最近一条状态，不累积日志。链接仅支持 HTTPS。
不要输出 access_token、refresh_token、Cookie、设备身份或原始 HTTP 响应；
stderr 不展示，脚本须自行将错误转成不含秘密的 message。exit 0 表示该动作
完成，非零显示失败。关窗停止前端轮询，**不取消正在进行的授权**；再次打开
可继续查看。最长 300 秒，超时由执行器停止自身进程组，不触发 keeper 的
job-timeout 安全模式。按钮操作不得后台启动常驻服务。

操作占用一个 keeper 槽位（总上限仍为 8），遵守现有内存/进程预算。同一脚本
扩展不能重复启动；操作中拒绝覆盖上传该脚本。卸载会先清理其扩展任务。
普通脚本安装数量接近槽位上限时，需释放不再使用的任务后再操作。

API（均要求 Web 登录）：GET `/api/apps/extension?name=...` 读取 schema；
POST `/api/apps/extension/action` 提交 `{name,action,values}`；GET
`/api/apps/extension/state?name=...` 读取最新公开结果。POST 要求同源 JSON，
不能由一个外站表单触发。已有每 5 秒设备信息请求只增加 extension 布尔标志，
不携带完整 schema。扩展仅在窗口打开且操作运行时使用现有共享调度器轮询。

## 文件索引 / Developer entry points

- `hooks/webd/app-extension.h`：只读声明、字段和动作边界验证。
- `hooks/webd/app-extension-api.h`：鉴权后 API、独立 keeper 任务、临时输入和清理。
- `hooks/webd/app-extension-runner.h`：同一 webd 可执行文件的受管操作模式；
  有界 NDJSON、截止时间、退出清理。没有模型平台逻辑。
- `hooks/webd/app-extension-options.h`：按字段合并、精确声明绑定的私有 TMP 目录缓存，
  供 API、执行器与设置工具共同校验。只提交成功结果，不含平台地址或账户逻辑。
- `hooks/webd/index.html`：扩展按钮、小窗口、类型控件和共享轮询。
- `tests/test_app_extension.py`、`hooks/webd/test_web_extensions.cjs`：输入边界和 UI 回归。
- `tests/test_app_extension_dynamic.py`：真实 runner、设置保存和状态 API 函数的动态目录回归，
  包括伪造 ID、失败保留、声明变化、权限/链接、旧当前值、多行合并、大目录配置回填、
  显式失效以及非 root 槽位更换 UID 后的二次验证。
- `runtime/model-adapter/voice_settings.py`：小智、豆包字段清单及生成器，嵌入上传脚本。
- `runtime/model-adapter/script-settings.c`：`show/save/run/forget` 通用原语，不含平台协议。
- `runtime/common/script-settings-store.h`：私有 DATA 存储的路径、权限、大小边界。
- `scripts/build_script_settings.sh`：面向 stock glibc 2.25 的固件构建。
- `docs/voice-script-settings.md`：当前字段、生效规则、数据生命周期及验证边界。

平台 URL、OAuth client_id、PKCE/refresh 编排、模型请求和映射应在脚本或
可更新适配器中；固件仅提供通用能力。不能用该 UI 的完成冒充真实平台登录
成功，也不能以电脑后台代理替代音箱独立推理。

## English summary

Uploaded shell scripts declare a bounded JSON comment block. A small modal opens
from the Extension button immediately before Logs. Supported fields are text,
password, number, checkbox, select and textarea; each action names its fields.
All submitted values are strings. The script receives JSON on stdin and runs
as a separate keeper job with its existing declared privilege. Existing voice
jobs are never restarted to execute UI actions. Public NDJSON status is bounded,
rendered as inert text, and must contain no credentials. Provider-specific login
and inference stay in updateable scripts; this UI alone is not a tested login.

## 脚本角标（2026-10-30）

文件头可声明 `# LX06-RESIDENT: on`，表示脚本会常驻等待事件；此标记仅
描述脚本，不启动进程、不提升权限、不自动登记开机自启。小智、豆包及账户
脚本模板已加入。`/api/status` 在现有 apps 列表中同时返回 resident
（实际开机名单）、resident_declared（文件头声明）、root（运行身份）。
常驻信息保留为后台元数据，卡片不再显示“常驻”。已登记的开机名单仍受
用户脚本开机自启动总开关控制。

root 角标使用紫色；正在运行时优先取 keeper 的实际 UID，
否则只依据 RUN 标记或 isroot=1，INSTALL-only 不表示日常 root 运行。
显示角标不会改变原有运行、账户、音频和自启设置，也不增加轮询请求。

脚本卡片的 root 和“运行中/未运行”角标放在右下角，状态来自实际 keeper job；
未运行不代表没有部署，常驻声明也不代表进程正在运行。脚本中心、扩展弹窗和运行日志弹窗
移除手动“刷新连接”按钮，继续使用原有自动刷新与运行期间轮询。

扩展窗口空闲时不显示“请选择操作”。仅扩展弹窗使用独立布局：标题固定、
内容区滚动、高级设置分组、统一尺寸的输入控件和底部操作区；保存按钮以
淡绿色区分，其他操作沿用中性按钮。390px 窄屏已检查普通与展开后的长表单，
关闭按钮、末尾输入框和操作按钮可正常使用。样式作用域限定在 extensionModal，
不改变密码窗口及脚本配置协议。现有扩展测试 12 项通过。

脚本列表采用独立 script-row 样式：名称与部署时间/大小分层，信息区与操作区
整体垂直居中，root/运行状态在操作区右下方自然排布。部署时间与大小可独立换行，
不再用分隔点挤在同一个文本节点。脚本大小展示下限为 0.01 MB，实际字节数、
空间检查及 TMP/DATA 显示不变。390px 实机页面检查五个卡片的两区中心差均为 0。

## 临时操作的槽位回收

每个扩展动作使用独立 ext-<脚本名MD5> keeper job。动作结束后，Web 每 5 秒共享
一次清理，并在新动作前清理：仅对注册表中已无运行进程的合法 ext-32位小写十六进制
名字发 LK_FORGET。keeper 二次核验活动进程和后代，拒绝清理仍在使用的工作区。
公开结果保留在 /tmp/lx06-extensions，账户和脚本配置不属于临时工作区，不被删除。
即使用户关掉弹窗或 Web 重启，也不会永久占用扩展槽位。

2026-10-30 实机发现四个已结束扩展动作占满旧 8 槽注册表，导致官方转发的
LK_START 返回 EUSERS（Too many users）；该失败不是 API Key 或网络问题。
另外，私有角标部署辅助程序曾将 shebang 脚本权限降为 0600，已修为 0700，
避免直接 exec 失败。小智/豆包已恢复运行和开机登记，官方音频进程未重启。

持久身份槽位 LK_MAX_SLOTS 扩为 32，并与同时运行上限 LK_MAX_JOBS=8 分离。
固定通信报文仍保留八个运行任务的容量，旧管理客户端无需同步更换协议。
UID 保留范围扩为 61000–61031；旧八槽表保持原 UID 映射，使用高位槽后写完整
32 槽表；高位未用时仍写八槽格式。扩展目录所有权校验使用相同常量。
完整 32 槽表不能交给旧 keeper；固件更新/回退时须成套检查注册表格式，
不要只复制 keeper 二进制或手工重排 UID。账户登录材料和用户配置不应公开。
# 2026-10-30 扩展弹窗简化

弹窗顶部显示脚本文件名和扩展标题。操作状态独立为一行短状态：加载中、处理中、
等待授权、已就绪、未完成或连接失败；状态色使用低饱和绿、琥珀和红。
配置读取/保存成功只显示第一句确认，脚本任意输出、错误原因、授权链接和验证码
保留完整内容，长结果独立滚动。操作状态不表示主脚本运行状态。

打断方式选项显示“打断词 / 人声”，提交值仍为 0 / 1。下拉和数字字段可并排，
词表保留整行，高级项折叠；多操作的账户脚本采用普通三列按钮区域，手机两列，
避免固定操作区遮挡表单。保持浅色按钮，无蓝色主按钮或新增刷新连接入口。

voice_settings.py 与 build_accounts.py 的标签/介绍同步精简；设备更新只替换
扩展声明中的 label 和 description，没有覆盖字段默认、参数、payload、账户或设置。
页面单独挂载部署，四个主脚本和官方音频 PID 保持不变。前端 12 项与设置 5 项
通过，桌面/390px 手机宽度实机检查；未生成新的固件/刷机包。
