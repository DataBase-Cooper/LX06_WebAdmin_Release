"""Generate a disabled standalone example; never runs inference while building."""
import json,subprocess,sys
from pathlib import Path
from account_structured import account_structured

def build(destination):
    root=Path(__file__).resolve().parent
    schema=dict(version=1,title='Existing text adapter',fields=[],actions=[])
    account_structured(schema,request='payload',prompt='guidance',response='result',
        invocation='lease',status='availability',status_action='ready',logged_in='usable')
    source='#!/bin/sh\n# LX06-EXEC: shell-fd-v1\n# LX06-EXTENSION-BEGIN\n# '+json.dumps(schema,separators=(',',':'))+'\n# LX06-EXTENSION-END\n'
    source+=r'''exec 9<&-
# Named identity works for both explicit FD launch and unchanged legacy Web.
# A new self-call may explicitly use script-settings run-script with this path.
SCRIPT_PATH=${LX06_SCRIPT_PATH:-$0}
SCRIPT_ID=${LX06_SCRIPT_ID:-${SCRIPT_PATH##*/}}
set -eu
umask 077
TOOL=/usr/bin/lx06-model-tool
WORK=
cleanup() { [ -z "$WORK" ] || rm -rf "$WORK"; }
trap cleanup EXIT
trap 'exit 130' INT TERM HUP
get() { "$TOOL" get "$1" < "$2"; }
put() { printf '%s' "$2" > "$WORK/$1"; }
field() { get "$1" "$WORK/values" 2>/dev/null || :; }
fail() { printf '%s\n' '{"message":"Text adapter unavailable"}';exit 1; }
prepare() {
    WORK=$(mktemp -d /tmp/lx06-structured-example.XXXXXX)
    cat > "$WORK/request"
    get values "$WORK/request" > "$WORK/values" || fail
}
account_status() { fail; }
STRUCTURED_REQUEST_FIELD=payload
STRUCTURED_PROMPT_FIELD=guidance
STRUCTURED_RESPONSE_FIELD=result
STRUCTURED_INVOCATION_FIELD=lease
STRUCTURED_STATUS_FIELD=availability
STRUCTURED_LOGGED_IN_VALUE=usable
# Replace only these callbacks with this upload's existing status/text paths.
# There is deliberately no service URL, account, login, or executable fallback.
structured_account_ready() { fail; }
structured_text_infer() { fail; }
'''
    source+=(root/'account-structured.sh').read_text()
    source+=r'''
case "${1:-}:${2:-}" in
    --web-action:ready) structured_status;;
    --web-action:extract|--web-action:understand) structured_request "$2";;
    *) exit 2;;
esac
'''
    destination=Path(destination);destination.write_text(source,encoding='utf-8',newline='\n')
    subprocess.run(['sh','-n',str(destination)],check=True)
    return destination

if __name__=='__main__':build(sys.argv[1])
