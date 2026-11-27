"""Add the listening pause control to an existing private Xiaozhi upload."""

import argparse
import json
from pathlib import Path
import re
import subprocess
import sys

from replace_script_payload import replace_payload

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'runtime/model-adapter'))
from voice_settings import (
    xiaozhi_end_word_field, xiaozhi_keepalive_field, xiaozhi_mcp_auto_field, xiaozhi_mcp_query_field,
    xiaozhi_pause_field, xiaozhi_pause_visibility, xiaozhi_home_mcp_catalog,
    xiaozhi_home_mcp_fields, xiaozhi_home_mcp_visibility,
)


def build(source, client, output):
    raw = client.read_bytes()
    if raw[:4] != b'\x7fELF' or int.from_bytes(raw[18:20], 'little') != 40:
        raise ValueError('Expected an LX06 ARM client')

    text = source.read_text(encoding='utf-8')
    pause_fields = [xiaozhi_pause_field(), xiaozhi_end_word_field(), *xiaozhi_home_mcp_fields()]
    pause_names = [field['name'] for field in pause_fields]
    task_fields = [xiaozhi_keepalive_field(), xiaozhi_mcp_auto_field(), xiaozhi_mcp_query_field()]
    task_names = [field['name'] for field in task_fields]

    def update(match):
        schema = json.loads(match[2])
        fields = [item for item in schema['fields'] if item['name'] not in (*pause_names, *task_names)]
        position = next(i for i, item in enumerate(fields) if item['name'] == 'XZ_INPUT_GAIN_DB')
        fields[position:position] = pause_fields
        position = next(i for i, item in enumerate(fields) if item['name'] == 'XZ_MAX_SESSION_SECONDS')
        fields[position:position] = task_fields
        schema['fields'] = fields
        xiaozhi_pause_visibility(schema)
        xiaozhi_home_mcp_visibility(schema)
        save, = [action for action in schema['actions'] if action['id'] == 'save']
        configure, = [step for step in schema['flow']['steps'] if step['id'] == 'configure']
        for container in (save, configure):
            names = [item for item in container['fields'] if item not in (*pause_names, *task_names)]
            position = names.index('XZ_INPUT_GAIN_DB')
            names[position:position] = pause_names
            position = names.index('XZ_MAX_SESSION_SECONDS')
            names[position:position] = task_names
            container['fields'] = names
        configure['description'] = '使用换气结束词时，说完正文短停一下，再说结束词。'
        return match[1] + json.dumps(schema, ensure_ascii=False, separators=(',', ':')) + match[3]

    text, count = re.subn(r'(# LX06-EXTENSION-BEGIN\n# )([^\n]+)(\n# LX06-EXTENSION-END)',
                          update, text)
    if count != 1:
        raise ValueError('Expected one Xiaozhi extension declaration')
    text = xiaozhi_home_mcp_catalog(text)
    original = '    export LX06_DIALOG_LIGHTS=1 LX06_SESSION_MAX_SECONDS="${XZ_MAX_SESSION_SECONDS:-900}"\n'
    replacement = '''    limit=${XZ_MAX_SESSION_SECONDS:-900}
    keepalive=${XZ_KEEPALIVE_MINUTES:-0}
    case "$limit" in ""|*[!0-9]*|0*|??????*) limit=900;; esac
    case "$keepalive" in 0|[1-9]|[1-9][0-9]|1[01][0-9]|120) ;; *) keepalive=0;; esac
    keepalive=$((keepalive * 60))
    [ "$limit" -ge "$keepalive" ] || limit=$keepalive
    [ "$limit" -le 86400 ] || limit=86400
    export LX06_DIALOG_LIGHTS=1 LX06_SESSION_MAX_SECONDS="$limit"
'''
    # The uploaded script embeds these lines inside a single-quoted sh -c
    # argument. The outer script's sh -n cannot detect truncated inner quoting.
    if "'" in replacement:
        raise ValueError('Single quote would break the embedded Xiaozhi session command')
    if text.count(original) != 1 and text.count(replacement) != 1:
        raise ValueError('Expected one Xiaozhi session time limit')
    text = text.replace(original, replacement, 1)
    launch = '  exec "$BIN"\nfi\n'
    defaults = ('  export XZ_MCP_TASK_AUTO="${XZ_MCP_TASK_AUTO:-1}"\n'
                '  export XZ_LISTEN_END_WORD="${XZ_LISTEN_END_WORD-发送}"\n')
    if defaults + launch not in text:
        if text.count(launch) != 1:
            raise ValueError('Expected one Xiaozhi client launch')
        text = text.replace(launch, defaults + launch, 1)
    writer = Path(__file__).with_name('xz_mcp_task_state.sh').read_text(encoding='utf-8')
    root_guard = "[ \"$(id -u)\" = 0 ] || { say '需要 root 运行'; exit 2; }\n"
    if writer not in text:
        if '--mcp-task-state' in text or text.count(root_guard) != 1:
            raise ValueError('Could not locate the Xiaozhi task state writer entry point')
        text = text.replace(root_guard, root_guard + "\n" + writer, 1)
    data = replace_payload(text.encode(), b'$WORK/xz_client.gz', raw)
    subprocess.run(['/bin/sh', '-n'], input=data, check=True)
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open('xb') as script:
        script.write(data)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('arm_client', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    build(args.source, args.arm_client, args.output)
