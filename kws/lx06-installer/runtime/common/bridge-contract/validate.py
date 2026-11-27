#!/usr/bin/env python3
"""K0/K3 offline contract consumer. Not a script/settings parser or daemon."""
import json
from pathlib import Path
import re
import struct
from copy import deepcopy

ROOT = Path(__file__).resolve().parent
VERSIONS = json.loads((ROOT / 'versions.json').read_text())
ERRORS = json.loads((ROOT / 'errors.json').read_text())


class Refused(ValueError):
    def __init__(self, code, field):
        self.code, self.field = code, field
        super().__init__(code + ':' + field)


def require(ok, field, code='unsupported'):
    if not ok:
        raise Refused(code, field)


def loads(data, limit=8192):
    def pairs(items):
        out = {}
        for key, value in items:
            require(key not in out, 'duplicate-key')
            out[key] = value
        return out
    require(isinstance(data, bytes) and len(data) <= limit, 'payload')
    try:
        value = json.loads(data.decode('utf-8'), object_pairs_hook=pairs,
                           parse_constant=lambda _: require(False, 'nonfinite'))
        encoded(value, limit)
        return value
    except (UnicodeError, ValueError, RecursionError):
        raise Refused('unsupported', 'json') from None


def encoded(value, limit=8192):
    try:
        data = json.dumps(value, ensure_ascii=False, allow_nan=False,
                          separators=(',', ':')).encode('utf-8')
    except (ValueError, TypeError, UnicodeError, RecursionError):
        raise Refused('unsupported', 'json') from None
    require(len(data) <= limit, 'payload')
    return data


def string(value, field):
    require(isinstance(value, str) and bool(value), field)


def strings(value, field):
    require(isinstance(value, list), field)
    for item in value:
        string(item, field)
    require(len(set(value)) == len(value), field + '.duplicate')


def no_credentials(value):
    """Reject credential fields at any depth, including diagnostic extensions."""
    if isinstance(value, dict):
        for key, item in value.items():
            require(isinstance(key, str), 'json.key')
            require(key.lower() not in ('token', 'authorization', 'phone_token'), 'credential')
            no_credentials(item)
    elif isinstance(value, list):
        for item in value:
            no_credentials(item)


def runtime_audio(value):
    require(isinstance(value, dict), 'audio')
    for key, state in value.items():
        require(key in ('mic', 'speaker', 'ring') and state in ('open', 'close'), 'audio')
    return {key: value.get(key, 'close') for key in ('mic', 'speaker', 'ring')}


def frame(data, direction, epoch):
    require(isinstance(data, bytes) and len(data) >= 48, 'header')
    require(type(epoch) is int and 0 < epoch < 2**64, 'epoch')
    magic, version, kind, size, flags, ep, gen, seq, rate, ch, reserved = struct.unpack('<IHHIIQQQIHH', data[:48])
    require(magic == 0x31474344 and version == 1, 'wire-version', 'bridge_version_mismatch')
    require(direction in VERSIONS['directions'], 'direction')
    require(kind in VERSIONS['directions'][direction], 'frame-type')
    require(size <= 8192 and len(data) == 48 + size and reserved == 0 and flags & ~7 == 0, 'frame-size/flags')
    require(ep == epoch and ep > 0, 'epoch')
    if kind in (2, 37):
        require(flags & 2 and 8000 <= rate <= 48000 and ch in (1, 2) and size > 0 and size % (2 * ch) == 0 and seq > 0, 'pcm')
    else:
        require(rate == ch == 0, 'control-format')
    if kind == 6:
        ack = loads(data[48:])
        require(isinstance(ack, dict) and bool(ack), 'grant-empty')
        if ack.get('state') == 'started':
            require(all(k in ack for k in ('command_id','task_id')), 'grant-ack')
    return kind


def open_envelope(value, epoch):
    require(isinstance(value, dict), 'open')
    encoded(value)
    for key in ('action', 'epoch', 'command_id', 'script_id', 'wake_source'):
        require(key in value, key)
    require(type(value['epoch']) is int and type(epoch) is int and value['epoch'] == epoch and 0 < epoch < 2**64, 'epoch')
    require(isinstance(value['command_id'], str) and re.fullmatch(r'[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-7[0-9a-fA-F]{3}-[89abAB][0-9a-fA-F]{3}-[0-9a-fA-F]{12}', value['command_id']), 'command_id')
    require(isinstance(value['script_id'], str) and bool(value['script_id']), 'script_id')
    require(isinstance(value['action'], str) and bool(value['action']) and not value['action'].startswith('flow:'), 'action')
    if 'target' in value:
        string(value['target'], 'target')
    require(value['wake_source'] in ('official','local','scheduled'), 'wake_source')
    text = value.get('wake_text', '')
    require(isinstance(text, str) and len(text) <= 128, 'wake_text')
    require(value['wake_source'] != 'scheduled' or text == '', 'scheduled.wake_text')
    for key in ('params','set','audio'):
        require(isinstance(value.get(key, {}), dict), key)
    runtime_audio(value.get('audio', {}))
    return value


def caps(value):
    require(isinstance(value, dict), 'caps')
    encoded(value)
    no_credentials(value)
    require(value.get('bridge_version') == '2.0', 'bridge_version', 'bridge_version_mismatch')
    require(value.get('kind') == 'caps' and value.get('bridge') is True, 'caps')
    require('token' not in value and isinstance(value.get('apps'), list), 'caps.apps/token')
    for key in ('features', 'parameter_keys', 'audio_keys'):
        strings(value.get(key, []), key)
    require(set(value.get('audio_keys', [])) <= {'mic', 'speaker', 'ring'}, 'audio_keys')
    names = set()
    for app in value['apps']:
        require(isinstance(app, dict) and isinstance(app.get('app'), str) and app['app'] and 'app_id' not in app and 'operations' not in app, 'apps.app')
        require(app['app'] not in names, 'duplicate-app')
        names.add(app['app'])
        require(app.get('hook_protocol') == {'min':'1.0','max':'1.0'}, 'hook_protocol', 'hook_version_mismatch')
        require(app.get('compat', 'ok') in ('ok', 'degraded', 'absent', 'out_of_range'), 'compat')
        require(isinstance(app.get('audio'), dict), 'audio')
        for key, choices in [('mic', ('input','none')),('speaker',('output','none')),('ring',('ringtone','none'))]:
            require(app['audio'].get(key) in choices, 'caps.audio.' + key)
        require(isinstance(app.get('ops'), list), 'ops')
        ops = set()
        for op in app['ops']:
            require(isinstance(op, dict) and isinstance(op.get('name'), str) and op['name'] and op['name'] not in ops, 'op.name')
            ops.add(op['name'])
            require(op.get('session', 'keep') in ('keep','end'), 'op.session')
            require(type(op.get('timeout_ms')) is int and op['timeout_ms'] > 0, 'op.timeout_ms')
            for key in ('oneway', 'async', 'silent'):
                if key in op:
                    require(type(op[key]) is bool, 'op.' + key)
            if 'fallback' in op:
                strings(op['fallback'], 'fallback')
            if 'returns' in op:
                require(isinstance(op['returns'], dict), 'returns')
                strings(op['returns'].get('json_fields', []), 'returns.json_fields')
            declarations(op.get('params'))
        if 'session' in app:
            session = app['session']
            require(isinstance(session, dict), 'session')
            require(session.get('cancel') in ('pause_hold', 'freeze_only'), 'session.cancel')
            if 'end_op' in session:
                require(isinstance(session['end_op'], str) and session['end_op'] in ops, 'session.end_op')
        if 'connection' in app:
            conn = app['connection']
            require(isinstance(conn, dict), 'connection')
            require(isinstance(conn.get('op'), str) and conn['op'] in ops, 'connection.op')
            string(conn.get('ready_signal'), 'connection.ready_signal')
            require(type(conn.get('timeout_ms')) is int and conn['timeout_ms'] > 0, 'connection.timeout_ms')
        if 'parameter_keys' in app:
            strings(app['parameter_keys'], 'app.parameter_keys')
    require(isinstance(value.get('peer_ops', []), list), 'peer_ops')
    peers = set()
    for peer in value.get('peer_ops', []):
        require(isinstance(peer, dict), 'peer_op')
        string(peer.get('op'), 'peer_op.op')
        require(peer['op'] not in peers, 'peer_op.duplicate')
        peers.add(peer['op'])
        declarations(peer.get('params'))
    return value


def declarations(params, lists=True):
    require(isinstance(params, dict), 'params-object')
    for key, decl in params.items():
        string(key, 'param.key')
        require(isinstance(decl, dict) and decl.get('type') in
                (('string','int','enum','bool','array') if lists else ('string','int','enum','bool')), 'param.type')
        if 'optional' in decl:
            require(type(decl['optional']) is bool, 'param.optional')
        if decl['type'] == 'enum':
            require(isinstance(decl.get('values'), list) and bool(decl['values']) and all(type(v) in (str, int) for v in decl['values']), 'param.values')
            require(len({(type(v), v) for v in decl['values']}) == len(decl['values']), 'param.values.duplicate')
        for bound in ('min','max'):
            if bound in decl:
                require(type(decl[bound]) is int, 'param.' + bound)
        if 'min' in decl and 'max' in decl:
            require(decl['min'] <= decl['max'], 'param.range')
        if decl['type'] == 'string' and 'max' in decl:
            require(decl['max'] >= 1, 'param.max')
        if decl['type'] == 'array':
            lo, hi, item = decl.get('minItems'), decl.get('maxItems'), decl.get('items')
            require(type(lo) is int and type(hi) is int and 0 < lo <= hi <= 2**64-1, 'param.items.bounds')
            require(isinstance(item, dict) and item.get('type') == 'object', 'param.items')
            require('additionalProperties' not in item or item['additionalProperties'] is False, 'param.items.extra')
            declarations(item.get('properties'), lists=False)


def params(decls, values):
    declarations(decls)
    require(isinstance(values, dict) and values.keys() <= decls.keys(), 'params.keys')
    encoded(values)
    for key, decl in decls.items():
        require(key in values or decl.get('optional', False), 'missing.' + key)
        if key not in values:
            continue
        value, kind = values[key], decl['type']
        if kind == 'array':
            require(type(value) is list and decl['minItems'] <= len(value) <= decl['maxItems'], key)
            for row in value:
                params(decl['items']['properties'], row)
        elif kind == 'enum':
            require(any(type(value) is type(v) and value == v for v in decl['values']), key)
        else:
            require(type(value) is {'string':str,'int':int,'bool':bool}[kind], key)
        if kind == 'string':
            require(max(1, decl.get('min', 1)) <= len(value) <= decl.get('max', 8192), key)
        if kind == 'int':
            require(decl.get('min', value) <= value <= decl.get('max', value), key)


def _schema_subset(target, source):
    require(source.keys() <= target.keys(), 'schema.keys')
    for key, to in target.items():
        require(key in source or to.get('optional', False), 'schema.required')
    for key, fr in source.items():
        to = target[key]
        kind = fr['type']
        require(kind == to['type'], 'schema.type')
        require(not fr.get('optional', False) or to.get('optional', False), 'schema.optional')
        if kind == 'array':
            require(to['minItems'] <= fr['minItems'] <= fr['maxItems'] <= to['maxItems'], 'schema.items')
            _schema_subset(to['items']['properties'], fr['items']['properties'])
        elif kind == 'enum':
            require(all(any(type(v) is type(t) and v == t for t in to['values'])
                        for v in fr['values']), 'schema.enum')
        elif kind in ('string', 'int'):
            if kind == 'string':
                require(max(1, fr.get('min', 1)) >= max(1, to.get('min', 1)), 'schema.min')
            elif 'min' in to:
                require('min' in fr and fr['min'] >= to['min'], 'schema.min')
            if 'max' in to:
                require('max' in fr and fr['max'] <= to['max'], 'schema.max')


def save_schema(snapshot, request):
    """Save-only same-type schema inclusion; A supplies mapped declarations.

    Reuses caps descriptors, not the script/input DSL. No runtime authority,
    settings parser, mapping, defaults or persistence. Conservative: a missing
    upper bound is unbounded; no cross-type coercions or vacuous empty domains.
    """
    caps(snapshot)
    encoded(request)
    no_credentials(request)
    require(isinstance(request, dict) and request.keys() <= {'app', 'op', 'params'}, 'schema.request')
    string(request.get('app'), 'app')
    string(request.get('op'), 'op')
    source = request.get('params', {})
    declarations(source)
    app = next((a for a in snapshot['apps'] if a['app'] == request['app']), None)
    require(app is not None, 'app', 'app_not_installed')
    require(not (app.get('compat') == 'absent' and app.get('reason') == 'protocol'),
            'compat.protocol', 'hook_version_mismatch')
    require(app.get('compat') != 'out_of_range', 'compat', 'app_version_unsupported')
    op = next((o for o in app['ops'] if o['name'] == request['op']), None)
    require(op is not None, 'op')
    _schema_subset(op['params'], source)
    return True


def _save_peer_request(snapshot, request, schema):
    caps(snapshot)
    encoded(request)
    no_credentials(request)
    require(isinstance(request, dict) and request.keys() <= {'op', 'params'}, 'schema.request')
    string(request.get('op'), 'op')
    source = request.get('params', {})
    if schema:
        declarations(source)
    op = next((o for o in snapshot.get('peer_ops', []) if o['op'] == request['op']), None)
    require(op is not None, 'peer_op')
    if schema:
        _schema_subset(op['params'], source)
    else:
        params(op['params'], source)
    return True


def save_peer_schema(snapshot, request):
    """Save-only mapped peer schema; no local execution/path authority."""
    return _save_peer_request(snapshot, request, True)


def save_peer_operation(snapshot, request):
    """Save-only concrete peer constants, never a mapped schema or run plan."""
    return _save_peer_request(snapshot, request, False)


def _save_bindings(snapshot, request, peer):
    """PROVISIONAL(P-20261112-1055-B): A supplies a whole-name partition."""
    caps(snapshot)
    encoded(request)
    no_credentials(request)
    allowed = {'op', 'params', 'schema'} | (set() if peer else {'app'})
    require(isinstance(request, dict) and request.keys() <= allowed, 'bindings.request')
    string(request.get('op'), 'op')
    values, mapped = request.get('params', {}), request.get('schema', {})
    require(isinstance(values, dict), 'bindings.params')
    declarations(mapped)
    require(not values.keys() & mapped.keys(), 'bindings.overlap')
    if peer:
        op = next((o for o in snapshot.get('peer_ops', []) if o['op'] == request['op']), None)
    else:
        string(request.get('app'), 'app')
        app = next((a for a in snapshot['apps'] if a['app'] == request['app']), None)
        require(app is not None, 'app', 'app_not_installed')
        require(not (app.get('compat') == 'absent' and app.get('reason') == 'protocol'),
                'compat.protocol', 'hook_version_mismatch')
        require(app.get('compat') != 'out_of_range', 'compat', 'app_version_unsupported')
        op = next((o for o in app['ops'] if o['name'] == request['op']), None)
    require(op is not None, 'op')
    target = op['params']
    require(values.keys() | mapped.keys() <= target.keys(), 'bindings.keys')
    for key, decl in target.items():
        require(key in values or key in mapped or decl.get('optional', False), 'bindings.required')
    # Validate the complete constant subset and complete mapped subset with the
    # same original algorithms. Never convert constants into enum declarations.
    params({k: target[k] for k in values}, values)
    _schema_subset({k: target[k] for k in mapped}, mapped)
    return True


def save_bindings(snapshot, request):
    """Save-only app constants plus mapped declarations; no plan or IO."""
    return _save_bindings(snapshot, request, False)


def save_peer_bindings(snapshot, request):
    """Save-only peer partition; original local rule/path admission remains."""
    return _save_bindings(snapshot, request, True)


def live_caps(value):
    """Official rev13 WFP-1-A: explicit bounded row declarations, no flag.

    This pure schema check never authenticates a connection or grants runtime
    authority to a saved snapshot. Callers must obtain current READY caps.
    """
    return caps(value)


def operation(current_caps, app_name, op_name, values=None, settings=None, audio=None):
    """K3 shared host consumer. Pure validation, no dial, settings parse or execution.

    Caller must supply caps from THIS connection. A snapshot is save-time only.
    App-specific semantic preconditions/fallback execution remain with the hook.
    """
    live_caps(current_caps)
    app = next((a for a in current_caps['apps'] if a['app'] == app_name), None)
    require(app is not None, 'app', 'app_not_installed')
    require(not (app.get('compat') == 'absent' and app.get('reason') == 'protocol'),
            'compat.protocol', 'hook_version_mismatch')
    require(app.get('compat') != 'out_of_range', 'compat', 'app_version_unsupported')
    op = next((o for o in app['ops'] if o['name'] == op_name), None)
    require(op is not None, 'op')
    params(op['params'], {} if values is None else values)
    settings = {} if settings is None else settings
    require(isinstance(settings, dict) and settings.keys() <= set(current_caps.get('parameter_keys', [])), 'set.keys')
    encoded(settings)
    requested = runtime_audio({} if audio is None else audio)
    for key, state in requested.items():
        require(state != 'open' or app['audio'][key] != 'none', 'audio.' + key)
    require(not op.get('silent', False) or all(state == 'close' for state in requested.values()), 'audio.silent')
    # No confirmation is invented: requested audio is not a READY confirmation.
    return dict(app=app_name, op=op_name, timeout_ms=op['timeout_ms'],
                session=op.get('session', 'keep'), oneway=op.get('oneway', False),
                requested_audio=requested, compat=app.get('compat', 'ok'),
                end_op=app.get('session', {}).get('end_op'))


def peer_operation(current_caps, op_name, values):
    """Validate a resolved peer op. Wire system.op envelope is a separate contract."""
    live_caps(current_caps)
    op = next((o for o in current_caps.get('peer_ops', []) if o['op'] == op_name), None)
    require(op is not None, 'peer_op')
    params(op['params'], values)
    return deepcopy(op)


def describe_request(current_caps, app=None):
    """Action fields only; caller supplies current OPEN identity from A."""
    require('hook.describe' in current_caps.get('features', []), 'features.hook.describe')
    if app is not None:
        require(any(a['app'] == app for a in current_caps['apps']), 'describe.app')
    return dict(action='hook.describe', target='system', params={} if app is None else {'app':app})


def describe_response(value):
    """Documentation is display-only; this does not create runtime caps."""
    require(isinstance(value, dict) and value.get('kind') == 'state', 'describe.kind')
    encoded(value)
    no_credentials(value)
    body = value.get('json')
    require(isinstance(body, dict) and type(body.get('doc_version')) is int and body['doc_version'] == 1 and isinstance(body.get('apps'), list), 'describe.json')
    seen = set()
    for app in body['apps']:
        require(isinstance(app, dict) and isinstance(app.get('app'), str) and app['app'] and app['app'] not in seen, 'describe.app')
        seen.add(app['app'])
        require(len(json.dumps(app,ensure_ascii=False,separators=(',',':')).encode()) <= 4096, 'describe.app_size')
        string(app.get('hook_version'), 'describe.hook_version')
        require(isinstance(app.get('doc_sha256'), str) and re.fullmatch('[0-9a-f]{64}', app['doc_sha256']) is not None, 'describe.hash')
        require(isinstance(app.get('summary'), str) and isinstance(app.get('ops'), list), 'describe.summary/ops')
        op_names = set()
        if 'notes' in app:
            require(isinstance(app['notes'], str), 'describe.notes')
        for op in app['ops']:
            require(isinstance(op, dict), 'describe.op')
            string(op.get('name'), 'describe.op.name')
            require(op['name'] not in op_names, 'describe.op.duplicate')
            op_names.add(op['name'])
            require(isinstance(op.get('desc'), str) and isinstance(op.get('returns_text'), str), 'describe.op.text')
            require(isinstance(op.get('params'), dict), 'describe.op.params')
            for key, param in op['params'].items():
                string(key, 'describe.param.key')
                require(isinstance(param, dict) and isinstance(param.get('desc'), str) and 'example' in param, 'describe.param')
            require(isinstance(op.get('examples'), list) and all(isinstance(s, str) for s in op['examples']), 'describe.examples')
    require(len(json.dumps(value,ensure_ascii=False,separators=(',',':')).encode()) <= 8192, 'describe.wire_size')
    return body


def _shape(value, schema, field):
    """The small, explicit keyword subset used by hook-ipc.schema.json only.

    This is not a general JSON Schema engine. Unknown validation keywords fail
    the consumer instead of silently weakening a future schema revision.
    """
    known = {'title', 'x-directions', 'type', 'const', 'enum', 'minimum', 'maximum',
             'minLength', 'pattern', 'properties', 'required', 'items', 'additionalProperties'}
    if schema.keys() - known:
        raise RuntimeError('unsupported schema keyword')
    if 'type' in schema:
        types = {'object':dict, 'array':list, 'string':str, 'integer':int, 'boolean':bool}
        require(type(value) is types[schema['type']], field)
    if 'const' in schema:
        require(type(value) is type(schema['const']) and value == schema['const'], field)
    if 'enum' in schema:
        require(any(type(value) is type(v) and value == v for v in schema['enum']), field)
    if 'minimum' in schema:
        require(value >= schema['minimum'], field)
    if 'maximum' in schema:
        require(value <= schema['maximum'], field)
    if 'minLength' in schema:
        require(len(value) >= schema['minLength'], field)
    if 'pattern' in schema:
        require(re.fullmatch(schema['pattern'], value) is not None, field)
    if isinstance(value, dict):
        require(set(schema.get('required', [])) <= value.keys(), field + '.required')
        properties = schema.get('properties', {})
        if schema.get('additionalProperties') is False:
            require(value.keys() <= properties.keys(), field + '.keys')
        for key, child in properties.items():
            if key in value:
                _shape(value[key], child, field + '.' + key)
    if isinstance(value, list) and 'items' in schema:
        for item in value:
            _shape(item, schema['items'], field + '[]')


def hook_message(line, direction):
    """One complete hook/1.0 JSONL record, including its terminating LF.

    The 64KiB hook line limit never changes the 8192B Wire limit. The caller
    performs SO_PEERCRED and task/app ownership checks before consuming this.
    No network listener or phone Ledger is implemented here.
    LF counting and heartbeat ts representation are PROVISIONAL(P-20261111-0722-B).
    """
    require(isinstance(line, bytes) and len(line) <= 65536 and line.endswith(b'\n')
            and b'\n' not in line[:-1] and b'\r' not in line, 'hook.line')
    value = loads(line, limit=65536)
    require(isinstance(value, dict), 'hook.message')
    schema = json.loads((ROOT / 'hook-ipc.schema.json').read_text())
    variant = next((v for v in schema['oneOf'] if v['title'] == value.get('type')), None)
    require(variant is not None and direction in variant['x-directions'], 'hook.type/direction')
    if value['type'] == 'REGISTER':
        require(value.get('protocol') == VERSIONS['hook_ipc'], 'protocol', 'hook_version_mismatch')
    _shape(value, variant, 'hook')
    no_credentials(value)
    return value


def hook_route(value):
    """Return routing obligations, never claim that ACK establishes readiness."""
    kind = value['type']
    if kind in ('REGISTER_ACK', 'REGISTER_REJECT', 'INVOKE_ACK', 'REGISTER',
                'INVOKE', 'HOOK_PING', 'HOOK_PONG', 'HOOK_KICK'):
        return None
    if kind == 'INVOKE_PROGRESS':
        return dict(frame=VERSIONS['frames']['STATE'], terminal=False, ledger=False)
    if kind == 'INVOKE_ERROR':
        return dict(frame=VERSIONS['frames']['ERROR'], terminal=True, ledger=True)
    if kind == 'REPORT_RESULT' or (kind == 'INVOKE_RESULT' and value['kind'] == 'result'):
        return dict(frame=VERSIONS['frames']['TASK'], kind='result', terminal=True, ledger=True)
    require(kind == 'INVOKE_RESULT', 'hook.route')
    return dict(frame=VERSIONS['frames']['READY'],
                kind='result' if value['kind'] == 'ready' else 'state', terminal=True, ledger=True)


def main():
    import argparse
    p = argparse.ArgumentParser(description='I0 host contract validation; no device capability evidence')
    p.add_argument('caps_file', type=Path)
    p.add_argument('--app')
    p.add_argument('--op')
    p.add_argument('--params', type=Path)
    p.add_argument('--set', dest='settings', type=Path)
    a = p.parse_args()
    try:
        current = caps(loads(a.caps_file.read_bytes()))
        if a.app is not None or a.op is not None:
            require(a.app is not None and a.op is not None, 'app/op')
            plan = operation(current, a.app, a.op,
                             loads(a.params.read_bytes()) if a.params else {},
                             loads(a.settings.read_bytes()) if a.settings else {})
            print(json.dumps(plan, ensure_ascii=False))
        else:
            print('caps contract passed (offline; not runtime capability evidence)')
    except (Refused, OSError) as error:
        # Never echo untrusted values, result text, paths or tokens.
        print(json.dumps({'ok':False, 'code':error.code if isinstance(error, Refused) else 'unsupported'}))
        return 2
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
