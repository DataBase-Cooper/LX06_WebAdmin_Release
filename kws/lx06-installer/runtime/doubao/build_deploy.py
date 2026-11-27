"""Build an uploadable script; private key comes from a private input script."""
import argparse
from pathlib import Path
import re
import shlex
import sys

ROOT=Path(__file__).resolve().parent
p=argparse.ArgumentParser()
p.add_argument('--key-script', type=Path)
p.add_argument('--settings-script', type=Path, help='Preserve script-top user settings when updating an installed script')
p.add_argument('--barge-mode', choices=['0', '1'], help='Explicitly select the new interruption mode')
p.add_argument('--interrupt-words', help='Explicit user-requested replacement of the script interruption list')
p.add_argument('--output', type=Path, required=True)
p.add_argument('--candidate', action='store_true', help='Use reversible TMP settings helper')
a=p.parse_args()
key='填写你的APIKey'
if a.key_script:
    # An empty key is a valid public template: the user fills it in the
    # script-owned connect step. Read literal assignments only, never eval.
    match=re.search(r"^export DB_API_KEY='([^'\r\n]*)'$",a.key_script.read_text(encoding='utf8'),re.M)
    if not match: raise SystemExit('input script lacks a literal DB_API_KEY assignment')
    key=match[1]
text=(ROOT/'doubao_watch.sh.tpl').read_text(encoding='utf8').replace('@@API_KEY@@',key)
if a.settings_script:
    old=a.settings_script.read_text(encoding='utf8').split('\nBASE=',1)[0]
    for name in ('DB_INTERRUPT','DB_GOODBYE','DB_IDLE_SECONDS','DB_MAX_SECONDS',
                 'DB_BARGE_ANY','DB_GREETING','DB_VOICE','DB_INSTRUCTIONS'):
        settings=re.findall(r'^export '+name+r'=[^\r\n]*$',old,re.M)
        if len(settings)!=1:raise SystemExit('settings script must contain exactly one '+name)
        text=re.sub(r'^export '+name+r'=[^\r\n]*$',lambda _:settings[0],text,flags=re.M)
    # Preserve any user-selected official aliases as well as speech settings.
    aliases=re.findall(r'^# 小爱通道唤醒词=[^\r\n]*$',old,re.M)
    if len(aliases)==1:
        text=re.sub(r'^# 小爱通道唤醒词=[^\r\n]*$',lambda _:aliases[0],text,flags=re.M)
if a.barge_mode is not None:
    text=re.sub(r'^export DB_BARGE_ANY=[^\r\n]*$',f'export DB_BARGE_ANY={a.barge_mode}',text,flags=re.M)
if a.interrupt_words is not None:
    if '\n' in a.interrupt_words or '\r' in a.interrupt_words:raise SystemExit('invalid interruption list')
    text=re.sub(r'^export DB_INTERRUPT=[^\r\n]*$',lambda _: 'export DB_INTERRUPT='+shlex.quote(a.interrupt_words),text,flags=re.M)
sys.path.insert(0,str(ROOT.parent/'model-adapter'))
from voice_settings import integrate
text=integrate(text,'doubao',a.candidate)
assert '\r' not in text
a.output.parent.mkdir(parents=True,exist_ok=True)
a.output.write_text(text,encoding='utf8',newline='\n')
print('upload script built:',a.output.name,'bytes=',len(text.encode()),'audio=firmware-unified-core')
