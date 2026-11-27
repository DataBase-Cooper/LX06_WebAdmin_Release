"""Embed optional account audio/voice helpers in an independently usable upload.

No firmware writes or DATA helper copies: a verified, root-only TMP directory
is shared by uploads carrying the same binaries. Every failure keeps
the caller's original AUDIO/VOICE/HTTP paths, so optimization cannot quarantine an
otherwise working user script. Other version directories are never removed.
"""
import gzip
import hashlib
from pathlib import Path


def build_runtime(audio_path, voice_path, http_path=None):
    """Return ASCII POSIX/BusyBox shell defining account_speed_runtime().

    Caller initializes AUDIO, VOICE and optional HTTP first, then invokes it before
    resident/worker entry. It is safe on model-request entry too: matching ROM
    or an already extracted pair only incurs bounded metadata/hash checks.
    """
    binaries = []
    helpers = [('model-audio', 'AUDIO', audio_path), ('model-voice', 'VOICE', voice_path)]
    if http_path is not None:
        helpers.append(('model-http', 'HTTP', http_path))
    for name, variable, path in helpers:
        raw = Path(path).read_bytes()
        if not raw or len(raw) > 2 * 1024 * 1024:
            raise ValueError(name + ' must be a nonempty helper at most 2 MiB')
        binaries.append((name, variable, raw, hashlib.sha256(raw).hexdigest()))
    version = hashlib.sha256(':'.join(item[3] for item in binaries).encode()).hexdigest()[:16]
    directory = '/tmp/lx06-account-speed-' + version

    # The hashes and bounded filenames are the only interpolated shell values.
    # Payload is printf octal; arbitrary binary bytes never become shell code.
    source = r'''
# Optional account latency runtime; no cloud/authentication policy here.
account_speed_meta() {
    # This firmware's BusyBox has ls -n but no stat applet. Parse only the
    # fixed numeric prefix, keeping exact modes/owner/link checks and refusing
    # symlinks. read avoids field splitting or glob expansion of a pathname.
    [ ! -L "$1" ] || return 1
    local lx06_speed_listing lx06_speed_mode lx06_speed_links
    local lx06_speed_uid lx06_speed_gid lx06_speed_rest
    lx06_speed_listing=$(LC_ALL=C ls -nd "$1" 2>/dev/null) || return 1
    IFS=' ' read -r lx06_speed_mode lx06_speed_links lx06_speed_uid lx06_speed_gid lx06_speed_rest <<LX06_SPEED_META
$lx06_speed_listing
LX06_SPEED_META
    [ "$lx06_speed_mode" = "$2" ] && [ "$lx06_speed_uid" = 0 ] || return 1
    [ -z "${3:-}" ] || [ "$lx06_speed_links" = "$3" ]
}
account_speed_hash() {
    [ -f "$1" ] && [ -x "$1" ] || return 1
    local lx06_speed_sum
    lx06_speed_sum=$(sha256sum "$1" 2>/dev/null) || return 1
    [ "${lx06_speed_sum%% *}" = "$2" ]
}
account_speed_files() {
@FILE_CHECKS@
}
account_speed_targets() {
    local lx06_speed_target
    for lx06_speed_target in @TARGETS@; do
        if [ -e "$lx06_speed_target" ] || [ -L "$lx06_speed_target" ]; then
            [ -f "$lx06_speed_target" ] &&
                account_speed_meta "$lx06_speed_target" -rwx------ 1 || return 1
        fi
    done
    return 0
}
account_speed_runtime() {
    # Preserve the working paths until all replacements have been validated.
    if @ORIGINAL_CHECKS@; then return 0; fi
    [ "$(id -u 2>/dev/null)" = 0 ] || return 0
    local lx06_speed_dir=@DIRECTORY@
    if [ ! -d "$lx06_speed_dir" ]; then
        ( umask 077; mkdir -m 700 "$lx06_speed_dir" ) 2>/dev/null || :
    fi
    [ -d "$lx06_speed_dir" ] &&
        account_speed_meta "$lx06_speed_dir" drwx------ || return 0
    if account_speed_files "$lx06_speed_dir"; then
        @ASSIGNMENTS@
        return 0
    fi
    account_speed_targets "$lx06_speed_dir" || return 0
    # Nonblocking coordination: another upload's extraction never stalls a
    # wake. Only the old ROM helpers are used for this invocation if busy.
    if [ -e "$lx06_speed_dir/.lock" ] || [ -L "$lx06_speed_dir/.lock" ]; then
        [ -f "$lx06_speed_dir/.lock" ] &&
            account_speed_meta "$lx06_speed_dir/.lock" -rw------- 1 || return 0
    fi
    local lx06_speed_umask
    lx06_speed_umask=$(umask)
    umask 077
    if (
        umask 077
        flock -n 7 || exit 1
        account_speed_meta "$lx06_speed_dir/.lock" -rw------- 1 || exit 1
        account_speed_files "$lx06_speed_dir" && exit 0
        lx06_speed_stage=$(mktemp -d "$lx06_speed_dir/.install.XXXXXX") || exit 1
        trap 'rm -f @STAGE_FILES@; rmdir "$lx06_speed_stage" 2>/dev/null || :' EXIT
        trap 'exit 1' HUP INT TERM
@PAYLOAD@
        chmod 700 @STAGE_FILES@ || exit 1
        account_speed_files "$lx06_speed_stage" || exit 1
        account_speed_targets "$lx06_speed_dir" || exit 1
        @MOVES@ || exit 1
        account_speed_files "$lx06_speed_dir" || exit 1
    ) 2>/dev/null 7>"$lx06_speed_dir/.lock"; then
        @ASSIGNMENTS@
    fi
    umask "$lx06_speed_umask"
    return 0
}
'''
    payload = ''
    for name, _, raw, _ in binaries:
        compressed = gzip.compress(raw, compresslevel=9, mtime=0)
        payload += '        {\n'
        for i in range(0, len(compressed), 128):
            octal = ''.join('\\%03o' % byte for byte in compressed[i:i + 128])
            payload += "            printf '" + octal + "'\n"
        payload += '        } | gzip -dc > "$lx06_speed_stage/' + name + '" || exit 1\n'
    checks = []
    for name, _, _, sha in binaries:
        checks.extend(['account_speed_meta "$1/'+name+'" -rwx------ 1',
                       'account_speed_hash "$1/'+name+'" '+sha])
    source = source.replace('@FILE_CHECKS@', '    '+' &&\n        '.join(checks))
    source = source.replace('@TARGETS@', ' '.join('"$1/'+item[0]+'"' for item in binaries))
    source = source.replace('@ORIGINAL_CHECKS@', ' &&\n        '.join('account_speed_hash "${'+variable+':-}" '+sha for _,variable,_,sha in binaries))
    source = source.replace('@ASSIGNMENTS@', '; '.join(variable+'=$lx06_speed_dir/'+name for name,variable,_,_ in binaries))
    source = source.replace('@STAGE_FILES@', ' '.join('"$lx06_speed_stage/'+item[0]+'"' for item in binaries))
    source = source.replace('@MOVES@', ' &&\n            '.join('mv -f "$lx06_speed_stage/'+name+'" "$lx06_speed_dir/'+name+'"' for name,_,_,_ in binaries))
    source = source.replace('@DIRECTORY@', directory).replace('@PAYLOAD@', payload.rstrip())
    source.encode('ascii')
    return source
