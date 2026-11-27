"""Build standalone DeepSeek login or streaming voice uploads; no ROM change."""
import argparse
import base64
import gzip
import hashlib
import json
from pathlib import Path
import subprocess
import shlex
from account_runtime import build_runtime
from extension_flow import account_flow
from account_structured import account_structured

ROOT=Path(__file__).resolve().parent


def build(destination, sealer=None, public_key=None, voice=False, streamer=None, speed_runtime=False):
    sealer=Path(sealer) if sealer else ROOT/'build/armhf/auth-seal'
    raw=sealer.read_bytes()
    public_key=public_key or (ROOT/'deepseek-auth-public-key.txt').read_text('utf8').strip()
    der=base64.b64decode(public_key,validate=True)
    compressed=gzip.compress(raw,9,mtime=0)
    def octal(data):return ''.join('\\%03o'%b for b in data)
    payload='ds_sealer() {\n  [ ! -x "$WORK/auth-seal" ] || return 0\n'
    payload+='  : > "$WORK/auth-seal.gz"\n'
    for i in range(0,len(compressed),128):
        payload+="  printf '"+octal(compressed[i:i+128])+"' >> \"$WORK/auth-seal.gz\"\n"
    payload+='  gzip -dc "$WORK/auth-seal.gz" > "$WORK/auth-seal" || fail \'无法展开登录加密组件。\'\n'
    payload+='  [ "$(sha256sum "$WORK/auth-seal" | cut -d " " -f 1)" = "'+hashlib.sha256(raw).hexdigest()+'" ] || fail \'登录加密组件校验失败。\'\n'
    payload+='  chmod 700 "$WORK/auth-seal"\n'
    payload+="  printf '"+octal(der)+"' > \"$WORK/auth-public.der\"\n}\n"
    schema=dict(version=1,title='DeepSeek · 登录测试',
        description='支持已注册账号的密码或手机验证码登录。密码、验证码不保存；官网可能要求滑块验证。此版暂不接入语音。',load='load',
        fields=[dict(name='account',label='邮箱或手机号',type='text',maxBytes=254),
                dict(name='sms_code',label='短信验证码（6 位）',type='password',maxBytes=6),
                dict(name='password',label='DeepSeek 登录密码',type='password',maxBytes=256),
                dict(name='area_code',label='手机国际区号（邮箱登录忽略）',type='text',default='+86',maxBytes=4,group='更多选项')],
        actions=[dict(id='send_sms',label='发送验证码',fields=['account','area_code']),
                 dict(id='sms_login',label='验证码登录',fields=['account','sms_code','area_code']),
                 dict(id='login',label='密码登录',fields=['account','password','area_code']),
                 dict(id='status',label='检查登录',fields=[]),
                 dict(id='website',label='打开官网',fields=[]),
                 dict(id='logout',label='退出本地账户',fields=[]),
                 dict(id='load',label='读取状态',fields=[])])
    defaults={}
    markers='# LX06-RUN: root\n'
    if voice:
        markers+='# LX06-AUDIO-MODE: account-text\n'
        # Saved experimental scheduling remains readable/overridable, but a
        # new upload must preserve continuous ASR rather than yield it to TTS.
        # MODEL_TTS_STREAM stays absent, retaining the core's disabled default.
        defaults=dict(MODEL_ID='default',DS_THINKING='0',MODEL_BARGE_ANY='0',
            MODEL_INTERRUPT='等一下,等等,停一下,你说的不对,你说得不对',
            MODEL_GOODBYE='结束对话,深度思考再见,拜拜了哦',MODEL_GREETING='我在呢，请说。',
            MODEL_IDLE_SECONDS='120',MODEL_MAX_SECONDS='900',MODEL_TTS_YIELD='0',MODEL_TRACE_ASR='0')
        markers+='# LX06-RESIDENT: on\n# LX06-LOCALWAKEUP: on\n# LX06-SESSION-MANAGED: on\n# LX06-SETTINGS: official_wake\n# 小爱通道唤醒词=深度思考\n'
        schema.update(title='DeepSeek · 账户与语音',description='说“小爱同学，深度思考”开始对话。保存后下次对话生效。')
        # Keep the official website action in both login-only and voice builds:
        # SMS verification may require the website before any API error occurs.
        schema['fields'] += [dict(name='question',label='测试问题',type='textarea',default='请用一句中文介绍自己。',maxBytes=1800),
            dict(name='official_wake',label='小爱通道词（留空关闭）',type='text',format='aliases',default='深度思考')]
        labels={'MODEL_ID':'模型（DeepSeek 官网自动匹配）','DS_THINKING':'深度推理（等待更久）','MODEL_BARGE_ANY':'打断方式',
            'MODEL_INTERRUPT':'打断词','MODEL_GOODBYE':'结束词','MODEL_GREETING':'唤醒后问候语',
            'MODEL_IDLE_SECONDS':'无交互退出时间（秒）','MODEL_MAX_SECONDS':'最长会话时间（秒）',
            'MODEL_TTS_YIELD':'语音响应优化','MODEL_TRACE_ASR':'识别调试日志（含说话内容）'}
        for name,value in defaults.items():
            f=dict(name=name,env=name,label=labels[name],type='text',default=value,maxBytes=128 if name=='MODEL_ID' else 450)
            if name in ('DS_THINKING','MODEL_BARGE_ANY','MODEL_TTS_YIELD','MODEL_TRACE_ASR'):
                f.update(type='select',options=['0','1'],widget='switch')
                if name=='MODEL_BARGE_ANY':f.update(widget='segments',optionLabels=['打断词','人声'])
            # The website voice adapter exposes default + thinking mode, not
            # an account model catalog. Do not invent server model IDs or ask
            # a user to type a website-internal identifier.
            if name=='MODEL_ID':f.update(type='select',options=['default'])
            if name=='MODEL_GREETING':f.update(maxBytes=240)
            if name in ('MODEL_INTERRUPT','MODEL_GOODBYE'):f.update(format='words')
            if name in ('MODEL_IDLE_SECONDS','MODEL_MAX_SECONDS'):f.update(type='number',format='integer',min=1,max=3600 if name=='MODEL_IDLE_SECONDS' else 86400)
            if name in ('MODEL_TRACE_ASR','MODEL_ID','MODEL_TTS_YIELD'):f.update(group='高级设置')
            schema['fields'].append(f)
        schema['actions'].insert(4,dict(id='test',label='文字测试',fields=['question']))
        schema['actions'].append(dict(id='save',label='保存对话设置',fields=['official_wake',*(name for name in defaults if name!='MODEL_BARGE_ANY')]))
    account_flow(schema,'deepseek')
    if voice:account_structured(schema)
    script='#!/bin/sh\n'+markers+'# LX06-EXTENSION-BEGIN\n# '+json.dumps(schema,ensure_ascii=False,separators=(',',':'))+'\n# LX06-EXTENSION-END\n'
    script+='SETTINGS=/usr/lib/lx06/script-settings\n'
    if voice:
        script+='if [ "${1:-}" != --configured ]; then\n'+''.join('export '+k+'='+shlex.quote(v)+'\n' for k,v in defaults.items())+'fi\n'
        script+='XA=/usr/lib/lx06/xiaoai\nVOICE=/usr/bin/lx06-model-voice\nAUDIO=/usr/bin/lx06-model-audio\nHTTP=/usr/bin/lx06-model-http\n'
        script+='if [ "${1:-}" = --configured ]; then shift; else\n  if [ "${1:-}" = --session-worker ]; then exec "$SETTINGS" run "$0" sh "$0" --configured "$@"; fi\n  if [ "${1:-}" = --web-action ]; then case "${2:-}" in extract|understand) exec "$SETTINGS" run "$0" sh "$0" --configured "$@";; esac; fi\nfi\n'
    source=(ROOT/'deepseek-login.sh').read_text('utf8')
    if voice:
        oldentry=source[source.index('case "${1:-}" in'):]
        uninstall=oldentry.split('    --uninstall)\n',1)[1].split("    '')",1)[0].rstrip()
        assert uninstall.endswith(';;');uninstall=uninstall[:-2]
        source=source[:source.index('case "${1:-}" in')]
        source=source.replace('此测试版尚未接入语音对话。','可以说“小爱同学，深度思考”开始对话。')
        source=source.replace('尚未验证模型对话。','可以使用文字测试或唤醒对话。')
        source+='\ndeepseek_uninstall() {\n'+uninstall+'\n}\n'
        binary=Path(streamer) if streamer else ROOT/'build/armhf/deepseek-stream'
        streamraw=binary.read_bytes();packed=gzip.compress(streamraw,9,mtime=0)
        payload+='ds_streamer() {\n  [ ! -x "$WORK/deepseek-stream" ] || return 0\n  : > "$WORK/streamer.gz"\n'
        for i in range(0,len(packed),128):payload+="  printf '"+octal(packed[i:i+128])+"' >> \"$WORK/streamer.gz\"\n"
        payload+='  gzip -dc "$WORK/streamer.gz" > "$WORK/deepseek-stream" || fail \'无法展开流式组件。\'\n'
        payload+='  [ "$(sha256sum "$WORK/deepseek-stream" | cut -d " " -f 1)" = "'+hashlib.sha256(streamraw).hexdigest()+'" ] || fail \'流式组件校验失败。\'\n  chmod 700 "$WORK/deepseek-stream"\n}\n'
    script+=(ROOT/'account-common.sh').read_text('utf8')+'\n'+payload+'\n'+source
    if voice:
        if speed_runtime:script+='\n'+build_runtime(ROOT/'build/armhf/model-audio-speed',ROOT/'build/armhf/model-voice-speed',ROOT/'build/armhf/model-http-speed')+'\n'
        script+='\n'+(ROOT/'account-voice.sh').read_text('utf8').rsplit('case "${1:-}" in',1)[0]
        script+='\n'+(ROOT/'account-structured.sh').read_text('utf8')
        script+='\n'+(ROOT/'deepseek-voice.sh').read_text('utf8')
    destination.mkdir(parents=True,exist_ok=True)
    output=destination/'deepseek_account.sh'
    output.write_text(script,encoding='utf-8',newline='\n')
    subprocess.run(['sh','-n',str(output)],check=True)
    print('DeepSeek '+('streaming voice' if voice else 'login-only')+' upload:',output)
    return output


if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('destination',type=Path);parser.add_argument('--voice',action='store_true');parser.add_argument('--speed-runtime',action='store_true')
    parser.add_argument('--sealer',type=Path,help='Explicit ARM auth helper; otherwise use the normal build output')
    args=parser.parse_args();build(args.destination,sealer=args.sealer,voice=args.voice,speed_runtime=args.speed_runtime)
