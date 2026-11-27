"""Reproducible unified-core build. No deployment, credentials or cloud access.

ARM links stock glibc 2.25 explicitly; current cross-toolchain libc must never
silently become a firmware dependency. Every input is hashed into the output.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command, **kwargs):
    p = subprocess.run([str(x) for x in command], capture_output=True, text=True, **kwargs)
    if p.returncode:
        raise RuntimeError(p.stderr[-4000:])
    return p.stdout


def sources():
    return ([HERE / name for name in ('main.c', 'session.c', 'control.c', 'protocol.c', 'audio_route.c',
             'account.c', 'tts.c', 'voice_guard.c', 'bootstrap.c', 'mode_gate.c', 'media.c', 'io/player.c', 'io/alsa.c',
             'io/capture.c', 'io/lights.c', 'asr/control_asr.c', 'asr/question-endpoint-vad.c')]
            + sorted((HERE / 'vendor/libfvad/src').rglob('*.c'))
            + [ROOT / 'runtime/doubao/src/json.c', ROOT / 'runtime/doubao/src/text_match.c'])


def phone_source_inputs():
    """Pure raw recipe closure; called before any materialization or compiler."""
    repo = ROOT.parents[1]
    draft = repo / 'docs/plan/parallel-evidence/a/settings-draft'
    wt1 = draft.parent / 'wt1-draft'
    manifest = wt1 / 'inputs.json'
    pins = json.loads(manifest.read_text())['source_refs']
    paths = [manifest, *(repo / name for name in pins)]
    paths += [p for p in [*draft.glob('*.h'), *draft.glob('*.inc')] if p.name != 'recognition-model.inc']
    paths += [draft / n for n in ('ctl-config.c','ctl-store.c','bridge-view.c','extension-objects.c','declaration-reader.c','schedule-hash.c')]
    paths += [wt1 / 'dispatch-draft.h', wt1 / 'dispatch-draft.c', wt1 / 'matcher-listen.patch']
    paths += [draft / n for n in ('rev11-matcher-capacity.patch','rev11-declaration-capacity.patch',
        'rev12-expression.patch','rev12-runtime.patch','rev12-declaration.patch','rev13-typed-script.patch')]
    return sorted(set(paths))

def phone_sources(output, runner=run):
    """Materialize the existing unique parser patches, then compile its owner.

    These are the same frozen parser inputs used by the original source owner;
    no alternative grammar or fixture permission is introduced into the binary.
    """
    repo = ROOT.parents[1]
    draft = repo / 'docs/plan/parallel-evidence/a/settings-draft'
    wt1 = draft.parent / 'wt1-draft'
    stage = output / 'phone-source'
    stage.mkdir()
    pins = json.loads((wt1 / 'inputs.json').read_text())['source_refs']
    for name, expected in pins.items():
        path = repo / name
        if sha(path) != expected:
            raise ValueError('original parser source changed: ' + name)
        shutil.copyfile(path, stage / path.name)
    for path in [*draft.glob('*.h'), *draft.glob('*.inc')]:
        if path.name != 'recognition-model.inc':
            shutil.copyfile(path, stage / path.name)
    for name in ('ctl-config.c', 'ctl-store.c', 'bridge-view.c', 'extension-objects.c', 'declaration-reader.c', 'schedule-hash.c'):
        shutil.copyfile(draft / name, stage / name)
    for name in ('dispatch-draft.h', 'dispatch-draft.c'):
        shutil.copyfile(wt1 / name, stage / name)
    for patch in (wt1 / 'matcher-listen.patch', draft / 'rev11-matcher-capacity.patch',
                  draft / 'rev11-declaration-capacity.patch', draft / 'rev12-expression.patch',
                  draft / 'rev12-runtime.patch', draft / 'rev12-declaration.patch',
                  draft / 'rev13-typed-script.patch'):
        runner(['patch', '--batch', '--fuzz=0', '-p1', '-i', patch], cwd=stage)
    features = ('LX06_SETTINGS_DRAFT_FIXTURE', 'LX06_SELECTED_SCRIPT_PROFILE_FIXTURE',
        'LX06_TEST_FIXTURE_ONLY', 'LX06_EXTENSION_OBJECTS_FIXTURE', 'LX06_INTERACTIVE_FLOW_FIXTURE',
        'LX06_REV11_DECLARATION_FIXTURE', 'LX06_REV11_UPLOAD_FIXTURE', 'LX06_REV11_TEMPLATE_FIXTURE',
        'LX06_REV12_EXPRESSION_FIXTURE', 'LX06_REV12_RUNTIME_FIXTURE', 'LX06_REV12_DECLARATION_FIXTURE')
    # Emit NORMAL product sources from the same pinned original parser bodies.
    # No test permission is defined or read in emitted code. Raw fixture inputs
    # remain unchanged; all actual C/include inputs still pass the original K8.
    # These two shared implementation headers change only their declared first-
    # line provenance. Parser grammar, bounds and runtime callbacks are exact.
    provenance = {
        'template-expression.h': ('/* LX06_TEST_FIXTURE_ONLY: compiler in the existing matcher translation unit.',
            '/* LX06_PHONE_PROFILE_RUNTIME: original expression compiler in the matcher translation unit.'),
        'template-runtime.h': ('/* LX06_TEST_FIXTURE_ONLY: extension of the one original matcher. */',
            '/* LX06_PHONE_PROFILE_RUNTIME: runtime of the one original matcher. */'),
    }
    promoted = []
    for path in stage.iterdir():
        if path.suffix not in ('.h', '.inc', '.c'):
            continue
        original = path.read_text()
        raw = original
        for feature in features:
            conditions = (
                ('#ifdef ' + feature, '#ifdef LX06_PHONE_PROFILE_RUNTIME'),
                ('#ifndef ' + feature, '#ifndef LX06_PHONE_PROFILE_RUNTIME'),
                ('#if defined(' + feature + ') || defined(LX06_PHONE_PROFILE_RUNTIME)', '#ifdef LX06_PHONE_PROFILE_RUNTIME'),
                ('#if !defined(' + feature + ') && !defined(LX06_PHONE_PROFILE_RUNTIME)', '#ifndef LX06_PHONE_PROFILE_RUNTIME'),
            )
            for before, after in conditions:
                raw = re.sub(r'^' + re.escape(before) + r'$', after, raw, flags=re.M)
        if path.name in provenance:
            before, after = provenance[path.name]
            if original.splitlines()[0] != before:
                raise ValueError('normal parser provenance changed: ' + path.name)
            raw = raw.replace(before, after, 1)
        if raw != original:
            promoted.append(dict(name=path.name, original_sha256=hashlib.sha256(original.encode()).hexdigest(),
                normal_sha256=hashlib.sha256(raw.encode()).hexdigest(), scope='normal capability guards and declared provenance only'))
        path.write_text(raw)
    (stage/'source-promotion.json').write_text(json.dumps(dict(
        capability='LX06_PHONE_PROFILE_RUNTIME', original_parser_bodies=True,
        fixture_permissions_defined=[], promotions=promoted), indent=2)+'\n')
    common = ROOT / 'runtime/common/bridge-contract'
    bridge = HERE / 'bridge'
    source = [stage / n for n in ('ctl-config.c', 'bridge-view.c', 'declaration-reader.c',
                                   'dispatch-draft.c', 'extension-objects.c')]
    source += [common / n for n in ('caps.c', 'caps_json.c', 'describe.c')]
    source += [bridge / n for n in ('wire.c', 'discovery.c', 'operation.c', 'session.c',
        'profile.c', 'profile_owner.c', 'inbox_store.c', 'result_inbox.c')]
    source += [ROOT / 'runtime/common/unicode-nfc.c', HERE / 'adapters/phone_worker.c', HERE / 'adapters/support/xiaozhi/ws_frame.c',
        stage / 'schedule-hash.c', ROOT / 'hooks/unified/dropbear-2017.75/libtomcrypt/src/hashes/sha2/sha256.c']
    flags = ['-DLX06_PHONE_PROFILE_RUNTIME', '-DDC_PHONE_WORKER_EMBEDDED', '-DARGTYPE=4',
        '-DLX06_PHONE_SETTINGS_IMPL_PATH="' + str(ROOT / 'runtime/model-adapter/script-settings.c') + '"',
        '-I' + str(stage), '-I' + str(ROOT / 'hooks/webd'), '-I' + str(ROOT / 'runtime/common'),
        '-I' + str(ROOT / 'hooks/unified/dropbear-2017.75'),
        '-I' + str(ROOT / 'hooks/unified/dropbear-2017.75/libtomcrypt/src/headers'),
        '-Wno-misleading-indentation', '-Wno-deprecated-declarations', '-Wno-sign-compare']
    return source, flags


def build(output, arm=False, fixture=False, sanitize=False):
    output = Path(output).resolve()
    if output.parent != (ROOT / 'build').resolve() or output.exists():
        raise ValueError('output must be a new direct child of build/')
    if arm and fixture:
        raise ValueError('test ownership bypass is absent from production ARM')
    output.mkdir(); objects = output / 'objects'; objects.mkdir()
    source = sources()
    flags = ['-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
             '-fstack-usage', '-fPIE', '-pthread',
             '-I' + str(HERE / 'vendor/libfvad/include'), '-I' + str(HERE / 'vendor/libfvad/src')]
    if fixture:
        flags.append('-DDC_CORE_HOST_FIXTURE')
    else:
        phone, phone_flags = phone_sources(output)
        source += phone
        flags += phone_flags
    if sanitize:
        flags += ['-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all']
    if arm:
        flags += ['-D_TIME_BITS=32', '-D_FILE_OFFSET_BITS=32']
        source.append(ROOT / 'hooks/webd/shim225.c')
    report = dict(passed=False, actual_device=False, actual_audio=False,
                  arm=arm, fixture=fixture, sanitized=sanitize, threads=3,
                  note='main reactor + fixed player + optional lights; SDK/transport are isolated')
    inputs = source + sorted(HERE.rglob('*.h')) + [Path(__file__).resolve(),
              ROOT / 'runtime/common/script-greeting.h']
    if not fixture:
        inputs += sorted((output / 'phone-source').glob('*.h')) + sorted((output / 'phone-source').glob('*.inc'))
        report['normal_phone_profile'] = True
        report['test_permission_macros'] = []
    report['sources'] = {os.path.relpath(p, ROOT): sha(p) for p in inputs}
    try:
        cc = 'arm-linux-gnueabihf-gcc' if arm else 'gcc'
        obj = []
        for index, path in enumerate(source):
            target = objects / f'{index:02}-{path.stem}.o'
            # GNU headers redirect strto*l back to this compatibility shim.
            # Defining GNU here turns its bodies into O2 self-branches. POSIX
            # exposes fmemopen without enabling those C23 numeric redirects.
            extra = ['-D_POSIX_C_SOURCE=200809L'] if path.name == 'shim225.c' else []
            run([cc, *flags, *extra, '-c', path, '-o', target]); obj.append(target)
            if arm and path.name=='shim225.c':
                relocations=run(['arm-linux-gnueabihf-readelf','-rW',target])
                symbols=run(['arm-linux-gnueabihf-readelf','-sW',target])
                (output/'shim225-relocations.txt').write_text(relocations)
                (output/'shim225-disassembly.txt').write_text(run(['arm-linux-gnueabihf-objdump','-dr',target]))
                if any(not re.search(r'\bUND\s+'+symbol+r'\b',symbols) for symbol in ('strtol','strtoul')) or re.search(r'R_ARM_\S+\s+\S+\s+__isoc23_strtou?l\b',relocations):
                    raise RuntimeError('shim225 numeric self-recursion or missing stock base-symbol import')
                report['shim225_stock_numeric_imports']=['strtol','strtoul']
        binary = output / 'lx06-dialogued'
        if not arm:
            run([cc, *flags, *obj, '-ldl', '-lopus', '-lm', '-lssl', '-lcrypto', '-o', binary])
            report['protocol'] = run([binary, '--protocol']).strip()
        else:
            legacy = Path(os.environ.get('LX06_LEGACY_LIB', '/opt/lx06/legacy'))
            opus = Path(os.environ.get('OPUS_LIBRARY', '/opt/lx06/legacy/libopus.so.0.6.1'))
            libs = [opus] + [legacy / ('sysarm-' + name) for name in
                ('libpthread.so.0', 'libm-2.25.so', 'libdl.so.2', 'libc-2.25.so', 'libgcc_s.so.1')]
            report['libraries'] = {p.name: sha(p) for p in libs}
            crt = Path('/usr/arm-linux-gnueabihf/lib')
            gcc = Path(run([cc, '-print-file-name=crtbeginS.o']).strip()).parent
            run([cc, '-nostdlib', '-pie', '-Wl,--dynamic-linker,/lib/ld-linux-armhf.so.3',
                 '-Wl,-z,now,-z,relro', '-Wl,--unresolved-symbols=ignore-in-shared-libs',
                 crt / 'Scrt1.o', crt / 'crti.o', gcc / 'crtbeginS.o', *obj, *libs,
                 '-lgcc', gcc / 'crtendS.o', crt / 'crtn.o', '-o', binary])
            symbols = run(['arm-linux-gnueabihf-readelf', '--dyn-syms', '-W', binary])
            (output / 'dynsyms.txt').write_text(symbols)
            if re.search(r'@GLIBC_2\.(?:2[6-9]|[3-9][0-9])\b', symbols):
                raise RuntimeError('unsupported post-2.25 glibc symbol')
        stacks = []
        for path in objects.glob('*.su'):
            for line in path.read_text().splitlines():
                function, amount, kind = line.split('\t')
                stacks.append(dict(function=function, bytes=int(amount), kind=kind))
        (output / 'stack-usage.json').write_text(json.dumps(stacks, indent=2) + '\n')
        report.update(passed=True, bytes=binary.stat().st_size, sha256=sha(binary),
                      max_stack_frame=max(x['bytes'] for x in stacks))
    except Exception as error:
        report['failure'] = str(error)
        raise
    finally:
        report['source_unchanged'] = all(sha(ROOT / name) == digest for name, digest in report['sources'].items())
        report['passed'] &= report['source_unchanged']
        (output / 'manifest.json').write_text(json.dumps(report, indent=2, ensure_ascii=False) + '\n')
    print(json.dumps({k: v for k, v in report.items() if k not in ('sources', 'libraries')}))


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', required=True)
    p.add_argument('--arm', action='store_true')
    p.add_argument('--fixture', action='store_true')
    p.add_argument('--sanitize', action='store_true')
    a = p.parse_args()
    build(a.output, a.arm, a.fixture, a.sanitize)
