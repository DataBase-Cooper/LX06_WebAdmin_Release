"""Build-time declarations; provider configuration policy stays in scripts.

No credentials here. Environment mappings are consumed by script-settings;
UI, validation and defaults are generated from this one inventory. Private
script credentials remain defaults in the shell and are never schema defaults.
"""
import json
import re
from pathlib import Path
from extension_flow import step

WORDS = '等一下,等等,停一下,你说的不对,你说得不对'
XIAOZHI_URL = 'wss://api.tenclass.net/xiaozhi/v1/'

def field(name, label, default=None, type='text', advanced=False, **kw):
    f = dict(name=name, label=label, type=type, **kw)
    if name != 'official_wake':
        f['env'] = name
    if default is not None:
        f['default'] = str(default)
    if advanced:
        f['group'] = '高级设置'
    return f

def number(name, label, default, low, high):
    return field(name, label, default, 'number', True, min=low, max=high, format='integer')

def switch(name, label, default=1, advanced=True):
    return field(name, label, default, 'select', advanced, options=['0', '1'],widget='switch')

def xiaozhi_pause_field():
    return field('XZ_LISTEN_PAUSE_MS', '预留换气时间', 0, 'select',
                 options=['0', '1000', '1500', '2000', '3000', '4000', '5000', '8000'],
                 optionLabels=['关闭（自动断句）', '1 秒', '1.5 秒', '2 秒',
                               '3 秒', '4 秒', '5 秒', '8 秒'])

def xiaozhi_end_word_field():
    return field('XZ_LISTEN_END_WORD', '换气结束词', '发送', placeholder='留空关闭', maxBytes=64)

def xiaozhi_keepalive_field():
    return number('XZ_KEEPALIVE_MINUTES', '长任务连接保留（分钟，0 关闭）', 0, 0, 120)

def xiaozhi_mcp_query_field():
    return field('XZ_MCP_TASK_QUERY', '备用查询指令（可留空）', '', advanced=True,
                 placeholder='任务可自行提供查询指令', maxBytes=160)


def xiaozhi_mcp_auto_field():
    return switch('XZ_MCP_TASK_AUTO', 'MCP 自动查询', 1)

def xiaozhi_home_mcp_fields():
    return [
        switch('XZ_HOME_MCP', '智能家居控制', 0, advanced=False),
        field('XZ_HOME_MCP_SCRIPT', '智能家居 MCP 脚本', 'none', 'select',
              options=['none'], dynamic=True, maxBytes=80),
    ]

def xiaozhi_home_mcp_visibility(schema):
    condition = dict(id='home-mcp-on', source='settings.XZ_HOME_MCP', equals='1')
    visibility = dict(target='fields.XZ_HOME_MCP_SCRIPT', conditions=['home-mcp-on'])
    for key, prop, item in (('conditions', 'id', condition), ('visibility', 'target', visibility)):
        schema[key] = [old for old in schema.get(key, []) if old[prop] != item[prop]] + [item]

def xiaozhi_home_mcp_catalog(text):
    helper = (Path(__file__).resolve().parents[2] / 'scripts/xz_home_mcp_options.sh').read_text('utf8')
    action = '    load) binding_web_load; exit $?;;'
    replacement = '    load) home_mcp_options; binding_web_load; exit $?;;'
    if helper.rstrip() in text and replacement in text:
        return text
    if text.count(action) != 1:
        raise ValueError('Expected one Xiaozhi Web load action')
    entry = 'if [ "${1:-}" = --web-action ]; then'
    if text.count(entry) != 1:
        raise ValueError('Expected one Xiaozhi Web action dispatcher')
    return text.replace(entry, helper.rstrip() + '\n\n' + entry, 1).replace(action, replacement, 1)

def xiaozhi_pause_visibility(schema):
    condition = dict(id='pause-enabled', source='settings.XZ_LISTEN_PAUSE_MS', notEquals='0')
    visibility = dict(target='fields.XZ_LISTEN_END_WORD', conditions=['pause-enabled'])
    for key, item in (('conditions', condition), ('visibility', visibility)):
        prop, = set(item) & {'id', 'target'}
        schema[key] = [old for old in schema.get(key, []) if old[prop] != item[prop]] + [item]

def schema(provider):
    db = provider == 'doubao'
    prefix, title = ('DB', '豆包') if db else ('XZ', '小智')
    fields = ([field(prefix+'_BARGE_ANY','打断方式',0,'select',options=['0','1'],
                     optionLabels=['打断词','人声'],widget='segments')] if db else []) + [
        field('DB_INTERRUPT' if db else 'XZ_INTERRUPT_TEXT', '打断词', WORDS, format='aliases' if db else 'words'),
        field('DB_GOODBYE' if db else 'XZ_GOODBYE_TEXT', '结束词', title+'再见,再见'+title+',拜拜了哦', format='words'),
        field('official_wake', '小爱通道词（留空关闭）', title+'出来,召唤'+title, format='aliases'),
    ]
    if db:
        fields[1:1] = [
            field('DB_BARGE_RMS', '打断音量门槛', 760, 'number',
                  widget='slider', min=400, max=8000, step=20, precision=0, center=760),
            field('DB_ECHO_PROTECTION', '回声保护', 0, 'select',
                  widget='segments', options=['0','1'], optionLabels=['标准','增强']),
        ]
        fields += [
            field('DB_GREETING', '唤醒后问候语', '我在呢，请说。', maxBytes=450),
            field('DB_API_KEY', '豆包 API Key（留空保留原值）', type='password', maxBytes=512),
            field('DB_ENDPOINT', '豆包连接地址', 'wss://openspeech.bytedance.com/api/v3/duplex/realtime/dialogue', advanced=True, format='wss', maxBytes=768),
            field('DB_VOICE', '豆包音色 ID', 'zh_female_xiaohe_jupiter_bigtts', advanced=True, maxBytes=100),
            field('DB_INSTRUCTIONS', '角色与回复要求', '你是豆包，用简洁自然的中文连续对话。', 'textarea', True, maxBytes=600),
            number('DB_IDLE_SECONDS', '无交互退出时间（秒）', 120, 1, 3600),
            number('DB_MAX_SECONDS', '最长会话时间（秒）', 900, 1, 86400),
            switch('DB_PLAYBACK', '播放回复'),
            field('DB_AUDIO_FORMAT', '回复音频格式', 'ogg_opus', 'select', True, options=['ogg_opus', 'pcm_s16le'],
                  widget='segments',optionLabels=['Opus（推荐）','PCM']),
            field('DB_PLAYBACK_BUFFER_MS', '起播缓冲', 500, 'number', True,
                  widget='slider', min=100, max=1000, step=50, precision=0, center=500, unit='ms'),
            switch('DB_TRACE_ASR', '识别调试日志（含说话内容）', 0),
        ]
    else:
        fields += [
            xiaozhi_pause_field(),
            xiaozhi_end_word_field(),
            *xiaozhi_home_mcp_fields(),
            field('XZ_INPUT_GAIN_DB','声音增益（0.00 dB 无增益）','0.00','number',
                  widget='slider',min=-12,max=12,step=.01,precision=2,center=0,unit='dB'),
            field('XZ_ACCESS_TOKEN', '连接令牌（留空保留原值）', type='password', maxBytes=512),
            number('XZ_IDLE_TIMEOUT_MS', '无交互退出时间（毫秒，0 禁用）', 120000, 0, 86400000),
            xiaozhi_keepalive_field(),
            xiaozhi_mcp_auto_field(),
            xiaozhi_mcp_query_field(),
            number('XZ_MAX_SESSION_SECONDS', '最长会话时间（秒）', 900, 1, 86400),
            switch('XZ_PLAYBACK', '播放回复'),
            number('XZ_CONNECT_TIMEOUT_MS', '连接超时（毫秒）', 10000, 1000, 60000),
            number('XZ_MIC_WAIT_MS', '等待麦克风（毫秒）', 15000, 0, 120000),
            number('XZ_PLAYBACK_TAIL_MUTE_MS', '尾音保护（毫秒，0 关闭）', 500, 0, 5000),
            number('XZ_GOODBYE_WAIT_MS', '结束词后等待回复（毫秒）', 5000, 0, 30000),
            number('XZ_GOODBYE_CLOSE_MS', '结束回复关闭期限（毫秒）', 5000, 500, 30000),
            number('XZ_PLAYBACK_DRAIN_MS', '退出时播放收尾期限（毫秒）', 5000, 100, 30000),
        ]
    result = dict(version=1, title=title+' · 对话设置',
                description='多个词用逗号分隔。保存后下次对话生效。' + ('人声模式可能受环境声音影响。' if db else ''),
                load='load', fields=fields, actions=[
                    dict(id='load', label='读取当前配置', fields=[]),
                    dict(id='save', label='保存设置' if db else '保存配置', fields=[f['name'] for f in fields])]
                    + ([dict(id='verify-key', label='验证连接', fields=[f['name'] for f in fields])]
                       if db else [dict(id='bind', label='检查绑定', fields=[])]))
    if db:
        result['conditions'] = [dict(id='keyword-mode', source='settings.DB_BARGE_ANY', equals='0'),
                                dict(id='human-mode', source='settings.DB_BARGE_ANY', equals='1')]
        result['visibility'] = [dict(target='fields.DB_INTERRUPT', conditions=['keyword-mode']),
                                dict(target='fields.DB_BARGE_RMS', conditions=['human-mode']),
                                dict(target='fields.DB_ECHO_PROTECTION', conditions=['human-mode'])]
        result['flow'] = dict(initial='connect',steps=[
            step('connect','连接豆包',['DB_API_KEY'],['verify-key'],
                 description='输入 API Key，验证通过后自动保存。'),
            step('configure','对话设置',[f['name'] for f in fields],['save'],
                 description='调低门槛可提高人声灵敏度。增强回声保护需要更持续的人声。')])
    else:
        xiaozhi_pause_visibility(result)
        xiaozhi_home_mcp_visibility(result)
        result['flow'] = dict(initial='pair',steps=[
            step('pair','绑定小智',['XZ_ACCESS_TOKEN'],['save','bind'],watch='bind',interval=8,
                 description='在小智控制台添加设备，填写配对码。'),
            step('configure','对话设置',[f['name'] for f in fields],['save','bind'],
                 description='使用换气结束词时，说完正文短停一下，再说结束词。')])
    return result

def upgrade_xiaozhi(text):
    """Make the existing identity query callable without running deployment.

    Keep startup binding behavior; extension explicitly requests a fresh code,
    so rotated logs and expired historical codes cannot mislead users.
    """
    start = text.find('load_config(){\n')
    if start >= 0:
        end = text.index('\n}', start)
        loader = text[start:end]
        export = '        export "$_k=$_v" ;;'
        if export in loader and 'XZ_WS_URL|XZ_ACCESS_TOKEN)' not in loader:
            # Old deployments wrote shell-style outer quotes to the data-only
            # xz.env. Recover just those delimiters without sourcing the file.
            loader = loader.replace(export, r'''        case "$_k" in XZ_WS_URL|XZ_ACCESS_TOKEN)
          case "$_v" in
            \'*\') _v=${_v#\'}; _v=${_v%\'} ;;
            \"*\") _v=${_v#\"}; _v=${_v%\"} ;;
          esac ;;
        esac
        export "$_k=$_v" ;;''', 1)
            text = text[:start] + loader + text[end:]
    ui = Path(__file__).with_name('xiaozhi-binding-ui.sh').read_text('utf8')
    text = re.sub(r"(?m)^XZ_WS_URL='[^'\n]*'$", f"XZ_WS_URL='{XIAOZHI_URL}'", text)
    text = text.replace('      XZ_WS_URL|XZ_ACCESS_TOKEN|', '      XZ_ACCESS_TOKEN|')
    text = text.replace("curl -sS --max-time 20 -X POST 'https://api.tenclass.net/xiaozhi/ota/'",
                        "curl -fsS --max-time 6 -X POST 'https://api.tenclass.net/xiaozhi/ota/'")
    if 'binding_web_event() (' in text:
        event_start = text.index('binding_web_event() (')
        start = text.rfind('# Check the device binding without starting audio.', 0, event_start)
        if start < 0:
            start = event_start
        end = text.index('\n)',event_start)+2
        text=text[:start]+ui.rstrip()+text[end:]
        return re.sub(r'(?m)^    load\) (?:binding_web_(?:event|load); exit \$\?;|exec "\$SETTINGS" show "\$0";);$',
                      '    load) binding_web_load; exit $?;;', text, count=1)
    if 'publish_bind_code() {' not in text:
        return text
    start = text.index('publish_bind_code() {')
    end = text.index('\n}', start) + 2
    publisher = text[start:end]
    text = text[:start] + text[end:]
    pos = text.index('if [ "${1:-}" = --web-action ]; then')
    text = text[:pos] + publisher + '\n' + ui + '\n' + text[pos:]
    text = text.replace('    load) exec "$SETTINGS" show "$0";;',
                        '    bind) binding_web_event; exit $?;;\n    load) binding_web_load; exit $?;;', 1)
    text = text.replace('--name "xz-$$"', '--name "xz-dialog"')
    # Keeper has stopped the complete tree before this declared uninstall hook.
    text = text.replace('  printf \'XZ-UNINSTALL-DONE\\n\'',
        '  rm -f /tmp/lx06-xz-timeline.log /tmp/lx06-xz-timeline.log.previous /tmp/lx06-xz-resident.lock\n'
        '  for lock in /tmp/lx06-mic/xz-*.lock; do\n'
        '    [ -f "$lock" ] && [ ! -L "$lock" ] || continue\n'
        '    ( flock -n 9 && rm -f "$lock" ) 9<"$lock"\n'
        '  done\n  printf \'XZ-UNINSTALL-DONE\\n\'')
    return text

def integrate(text, provider, candidate=False):
    # Replacing an extension is idempotent. Never append two declarations or
    # re-run/alter the installer payload while upgrading a private upload.
    text=re.sub(r'^# LX06-EXTENSION-BEGIN\n(?:#.*\n)*?# LX06-EXTENSION-END\n','',text,flags=re.M)
    text=re.sub(r'^# LX06-(?:SETTINGS: official_wake|SESSION-MANAGED: on)\n','',text,flags=re.M)
    if provider in ('xiaozhi', 'xz'):
        text = xiaozhi_home_mcp_catalog(upgrade_xiaozhi(text))
        fixed = f"export XZ_WS_URL='{XIAOZHI_URL}' XZ_BARGE_ANY=0"
        text = re.sub(r'(?m)^export XZ_(?:WS_URL=.* XZ_BARGE_ANY|BARGE_ANY)=0$', fixed, text, count=1)
        if fixed not in text:
            text = text.replace('#!/bin/sh\n', '#!/bin/sh\n'+fixed+'\n', 1)
        # Parent configuration and legacy stores must not override fixed settings.
        text = re.sub(r'(?m)^  export XZ_FULL_DUPLEX=0 XZ_BARGE_IN=0 XZ_LOCAL_AEC=1(?: XZ_BARGE_ANY=0)?(?: XZ_WS_URL=\x27[^\x27]*\x27)?$',
                      f'  export XZ_FULL_DUPLEX=0 XZ_BARGE_IN=0 XZ_LOCAL_AEC=1 XZ_BARGE_ANY=0 XZ_WS_URL=\'{XIAOZHI_URL}\'', text)
    else:
        pos=text.index('if [ "${1:-}" = --web-action ]; then')
        ui=Path(__file__).with_name('doubao-setup-ui.sh').read_text('utf8').rstrip()
        start=text.rfind('# UI-only probe;',0,pos) if 'doubao_setup_event() {' in text[:pos] else pos
        if start<0:
            raise ValueError('Unexpected Doubao setup helper')
        text=text[:start]+ui+'\n\n'+text[pos:]
        text=re.sub(r'(?m)^    load\) (?:exec "\$SETTINGS" show "\$0"|doubao_setup_load);;$',
                    '    load) doubao_setup_load; exit $?;;',text,count=1)
        text=re.sub(r'(?m)^    (?:verify-key|check)\) doubao_setup_.*\n','',text)
        text=text.replace('    save) exec "$SETTINGS" save "$0";;',
                          '    save) doubao_setup_save; exit $?;;',1)
        text=re.sub(r'(?m)^    (?:verify-key\|)?save\) doubao_setup_save; exit \$\?;;$',
                    '    verify-key|save) doubao_setup_save "$2"; exit $?;;', text, count=1)
        if 'worker() {\n  SESSION=' in text and 'worker() {\n  ensure_install || return 2\n' not in text:
            text=text.replace('worker() {\n  SESSION=', 'worker() {\n  ensure_install || return 2\n  SESSION=', 1)
    s = schema(provider)
    # Preserve the script's selected defaults, including the two interruption
    # editions, without publishing secret shell assignments in the UI schema.
    for f in s['fields']:
        if f['type'] == 'password':
            continue
        if f['name'] == 'official_wake':
            m = re.search(r'^# 小爱通道唤醒词=(.*)$', text, re.M)
            if m:
                f['default'] = m[1]
        else:
            m = re.search(r'^export '+re.escape(f['name'])+r"=(?:'([^'\n]*)'|([0-9]+))$", text, re.M)
            if m:
                f['default'] = next(v for v in m.groups() if v is not None)
    block = '# LX06-SETTINGS: official_wake\n# LX06-SESSION-MANAGED: on\n# LX06-EXTENSION-BEGIN\n# '+json.dumps(s, ensure_ascii=False, separators=(',', ':'))+'\n# LX06-EXTENSION-END\n'
    if provider in ('xiaozhi', 'xz') and '# LX06-COPY-CODES: on' not in text:
        block = '# LX06-COPY-CODES: on\n' + block
    # Put UI after leading routing markers, before any executable assignment.
    pos = re.search(r'^(?!#|\s*$)', text, re.M).start()
    text = text[:pos]+block+text[pos:]
    helper = '/tmp/lx06-model-candidate/script-settings' if candidate else '/usr/lib/lx06/script-settings'
    text = text.replace('@@SETTINGS_HELPER@@', helper)
    assert len(json.dumps(s, ensure_ascii=False, separators=(',', ':')).encode()) < 8192
    return text
