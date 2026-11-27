"""Produce independently uploadable scripts. Output is private build material.

Provider-neutral C stays in ROM; all platform URLs, OAuth policies and response
mapping stay in these scripts. No runtime dependency between account uploads.
"""
import argparse,json,subprocess
import shlex
from pathlib import Path
from account_runtime import build_runtime
from extension_flow import account_flow
from account_structured import account_structured

ROOT=Path(__file__).resolve().parent
def build(destination:Path,candidate=False,speed_runtime=False,experimental_account_declaration=None):
    # Header-only experiment: no sealed launcher or session binding is added.
    if experimental_account_declaration not in (None, 'keyword', 'voice'):
        raise ValueError('invalid experimental account declaration')
    destination.mkdir(parents=True,exist_ok=True)
    for provider,label in [('chatgpt','ChatGPT'),('grok','Grok')]:
        alias='召唤奥特曼' if provider=='chatgpt' else '召唤马斯克'
        model='gpt-6.1-sol' if provider=='chatgpt' else 'grok-4.7'
        # Keep this experimental scheduler field for saved configurations,
        # but never sacrifice continuous ASR by enabling it for new installs.
        # MODEL_TTS_STREAM is intentionally not exported: the core defaults off.
        defaults={'MODEL_ID':model,'MODEL_BARGE_ANY':'0',
                  'MODEL_INTERRUPT':'等一下,等等,停一下,你说的不对,你说得不对',
                  'MODEL_GOODBYE':'结束对话,拜拜了哦,'+('奥特曼再见' if provider=='chatgpt' else '马斯克再见'),
                  'MODEL_GREETING':'我在呢，请说。','MODEL_IDLE_SECONDS':'120','MODEL_MAX_SECONDS':'900',
                  'MODEL_TTS_YIELD':'0','MODEL_TRACE_ASR':'0','MODEL_SPEECH_FLOW':'连贯优先'}
        schema={'version':1,'title':label+' · 账户与语音','description':'选择模型后保存。多个词用逗号分隔。','load':'load',
            'fields':[{'name':'model','env':'MODEL_ID','label':'对话模型','type':'select','dynamic':True,'options':[model],'default':model,'maxBytes':128},
                      {'name':'question','label':'测试问题','type':'textarea','default':'请用一句中文介绍你自己。'}],
            'actions':[{'id':'login','label':'登录账户','fields':[]},{'id':'status','label':'账户状态','fields':[]},
                       {'id':'models','label':'更新模型列表','fields':[]},{'id':'test','label':'文字测试','fields':['model','question']},
                       {'id':'logout','label':'退出本地账户','fields':[]}]}
        if provider=='chatgpt':
            # Login completion must be visible without discovering a collapsed
            # optional group. This one-use secret is never a saved setting.
            schema['fields'].insert(0,{'name':'auth_callback','label':'授权回调地址（粘贴官网跳转后的完整地址）','type':'password','maxBytes':2048})
            schema['description']='登录账户 → 官网授权 → 粘贴回调地址 → 完成登录。'
            schema['actions'].insert(1,{'id':'complete-login','label':'完成登录','fields':['auth_callback']})
        schema['fields'] += [
            {'name':'official_wake','label':'小爱通道词（留空关闭）','type':'text','format':'aliases','default':alias},
            {'name':'MODEL_BARGE_ANY','label':'打断方式','type':'select','options':['0','1'],
             'widget':'segments','optionLabels':['打断词','人声']},
            {'name':'MODEL_INTERRUPT','label':'打断词','type':'text','format':'words'},
            {'name':'MODEL_GOODBYE','label':'结束词','type':'text','format':'words'},
            {'name':'MODEL_GREETING','label':'唤醒后问候语','type':'text','maxBytes':240},
            {'name':'MODEL_IDLE_SECONDS','label':'无交互退出时间（秒）','type':'number','format':'integer','min':1,'max':3600},
            {'name':'MODEL_MAX_SECONDS','label':'最长会话时间（秒）','type':'number','format':'integer','min':1,'max':86400},
            {'name':'MODEL_TTS_YIELD','label':'语音响应优化','type':'select','options':['0','1'],'widget':'switch','default':'0','group':'高级设置'}]
        schema['fields'].append({'name':'MODEL_SPEECH_FLOW','label':'播报节奏','type':'select','options':['连贯优先','首声优先'],'widget':'segments','group':'高级设置'})
        schema['fields'].append({'name':'MODEL_TRACE_ASR','label':'识别调试日志（含说话内容）','type':'select','options':['0','1'],'widget':'switch','default':'0','group':'高级设置'})
        for field in schema['fields']:
            if field['name'] in defaults:
                field['env'] = field['name']
                if field['name'] != 'MODEL_BARGE_ANY':
                    field['default'] = defaults[field['name']]
        # Keep the mode field for display and saved-value overlay, but never
        # accept it from a save request; the settings helper retains its stored value.
        schema['actions'] += [{'id':'load','label':'读取当前配置','fields':[]},
                              {'id':'save','label':'保存配置','fields':[f['name'] for f in schema['fields'] if f['name'] not in ['question','auth_callback','MODEL_BARGE_ANY']]}]
        account_flow(schema,provider)
        account_structured(schema)
        common=(ROOT/'account-common.sh').read_text('utf8')
        if candidate:common=common.replace('TOOL=/usr/bin/lx06-model-tool','TOOL=/tmp/lx06-model-candidate/model-tool')
        script='#!/bin/sh\n# LX06-AUDIO-MODE: account-text\n# LX06-COPY-CODES: on\n# LX06-RUN: root\n# LX06-RESIDENT: on\n# LX06-LOCALWAKEUP: on\n# LX06-SESSION-MANAGED: on\n# LX06-SETTINGS: official_wake\n# 小爱通道唤醒词='+alias+'\n# LX06-EXTENSION-BEGIN\n# '+json.dumps(schema,ensure_ascii=False,separators=(',',':'))+'\n# LX06-EXTENSION-END\n'
        if experimental_account_declaration:
            script=script.replace('#!/bin/sh\n', '#!/bin/sh\n# LX06-ACCOUNT-IDENTITY: v1\n# LX06-INTERRUPTION: '+experimental_account_declaration+'\n', 1)
        script+=''.join('export '+k+'='+shlex.quote(v)+'\n' for k,v in defaults.items())
        script+='XA=/usr/lib/lx06/xiaoai\n'
        script+='SETTINGS='+('/tmp/lx06-model-candidate/script-settings' if candidate else '/usr/lib/lx06/script-settings')+'\n'
        script+='VOICE='+('/tmp/lx06-model-candidate/model-voice' if candidate else '/usr/bin/lx06-model-voice')+'\n'
        script+='AUDIO='+('/tmp/lx06-model-candidate/model-audio' if candidate else '/usr/bin/lx06-model-audio')+'\n'
        script+='HTTP='+('/tmp/lx06-model-candidate/model-http' if candidate else '/usr/bin/lx06-model-http')+'\n'
        # Worker re-entry must apply saved settings AFTER shell defaults. The
        # --configured marker is private to this self-exec, not a Web action.
        script+='if [ "${1:-}" = --configured ]; then shift; else\n  if [ "${1:-}" = --session-worker ]; then exec "$SETTINGS" run "$0" sh "$0" --configured "$@"; fi\n  if [ "${1:-}" = --web-action ]; then case "${2:-}" in extract|understand) exec "$SETTINGS" run "$0" sh "$0" --configured "$@";; esac; fi\nfi\n'
        # The self-exec above would otherwise reassign defaults. Preserve the
        # settings overlay in worker entry by skipping only default exports.
        start=script.index('export MODEL_ID=');end=script.index('XA=',start)
        script=script[:start]+'if [ "${1:-}" != --configured ]; then\n'+script[start:end]+'fi\n'+script[end:]
        script+=common+'\n'+(ROOT/(provider+'-account.sh')).read_text('utf8')+'\n'
        if speed_runtime:script+=build_runtime(ROOT/'build/armhf/model-audio-speed',ROOT/'build/armhf/model-voice-speed',ROOT/'build/armhf/model-http-speed')+'\n'
        voice=(ROOT/'account-voice.sh').read_text('utf8')
        # Save must not materialize an inherited shell default as a stored mode.
        # Keep the environment mapping for existing saved values and keep the
        # shell default for actual sessions; do not change the generic helper.
        save_call='    "$SETTINGS" save "$0" || return $?'
        if voice.count(save_call) != 1: raise ValueError('account save hook changed')
        voice=voice.replace(save_call, '    ( unset MODEL_BARGE_ANY; "$SETTINGS" save "$0" ) || return $?')
        script+=(ROOT/'account-structured.sh').read_text('utf8')+'\n'
        script+=voice
        path=destination/(provider+'_account.sh');path.write_text(script,encoding='utf-8',newline='\n')
        subprocess.run(['sh','-n',str(path)],check=True)

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('destination',type=Path);parser.add_argument('--candidate',action='store_true');parser.add_argument('--speed-runtime',action='store_true')
    parser.add_argument('--experimental-account-declaration',choices=('keyword','voice'),
                        help='emit v1 metadata only; not connected to a sealed launcher')
    args=parser.parse_args();build(args.destination,args.candidate,args.speed_runtime,args.experimental_account_declaration)
