import os
import hashlib
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> None:
    client = ROOT / "build" / "armhf" / "xz_client"
    assert client.is_file(), "build the armhf client before packaging"
    with tempfile.TemporaryDirectory(prefix="xz-package-test-") as temporary:
        output = Path(temporary) / "xiaozhi-lx06"
        command = [
            "bash", str(ROOT / "scripts" / "package-lx06.sh"),
            "--output", str(output),
            "--client", str(client),
            "--ssl", str(Path(os.environ.get("LEGACY_LIB_DIR", "/opt/lx06/legacy")) / "sysarm-libssl.so.1.0.0"),
            "--crypto", str(Path(os.environ.get("LEGACY_LIB_DIR", "/opt/lx06/legacy")) / "sysarm-libcrypto.so.1.0.0"),
            "--opus", os.environ.get("OPUS_LIBRARY", "/opt/lx06/legacy/libopus.so.0.6.1"),
        ]
        subprocess.run(command, check=True, cwd=ROOT)

        assert (output / "xz_client").stat().st_mode & 0o111
        assert sha256(output / "xz_client") == sha256(client)
        assert (output / "lib" / "libssl.so.1.0.0").is_file()
        assert (output / "lib" / "libcrypto.so.1.0.0").is_file()
        assert (output / "lib" / "libopus.so.0").is_file()

        env_file = (output / "xz.env").read_text(encoding="utf-8")
        assert "XZ_PLAYBACK=1" in env_file
        assert "XZ_WAKE_TEXT=小爱同学召唤小智" in env_file
        assert "XZ_FULL_DUPLEX=0" in env_file
        assert "XZ_BARGE_IN=0" in env_file
        assert "XZ_MAX_SESSION_SECONDS=900" in env_file

        watcher = output / "xz_watch.sh"
        subprocess.run(["sh", "-n", str(watcher)], check=True)
        watcher_text = watcher.read_text(encoding="utf-8")
        assert 'export XZ_FULL_DUPLEX=0' in watcher_text
        assert 'export XZ_BARGE_IN=0' in watcher_text
        assert 'LD_LIBRARY_PATH="$BASE/lib' in watcher_text
        assert "XZ_WAKE_TEXT=\"$WAKE_TEXT\"" in watcher_text
        assert 'FIFO=${XZ_MIC_FIFO:-/tmp/mico-voip-audio.fifo}' in watcher_text
        assert 'INSTRUCTIONS=${XZ_INSTRUCTIONS_FILE:-/tmp/mico_aivs_lab/instruction.log}' in watcher_text
        assert 'notify_pns 5' in watcher_text
        # Allow the OEM NLP dialog to fully relinquish PNS before its VOIP
        # microphone stream is requested.  Without this drain, the hand-off
        # is timing-sensitive and the client can remain connected with zero
        # microphone frames after the greeting.
        handoff = watcher_text.split("quiet_official_reply() {", 1)[1].split("\n}\n", 1)[0]
        assert "notify_pns 8" in handoff
        assert "sleep 2" in handoff
        assert handoff.index("notify_pns 8") < handoff.index("sleep 2")
        assert '"is_final":true' in watcher_text
        assert 'last_final_record=' in watcher_text
        assert 'duplicate_final_ignored' in watcher_text
        assert 'XZ_MAX_SESSION_SECONDS' in watcher_text
        assert 'MAX_SESSION_SECONDS=${XZ_MAX_SESSION_SECONDS:-900}' in watcher_text

        manifest = output / "SHA256SUMS"
        rows = [line.split() for line in manifest.read_text(encoding="ascii").splitlines()]
        expected = {
            "xz_client", "lib/libssl.so.1.0.0", "lib/libcrypto.so.1.0.0",
            "lib/libopus.so.0", "xz.env", "xz_watch.sh",
        }
        assert {row[1] for row in rows} == expected
        for digest, relative in rows:
            assert digest == sha256(output / relative)

    print("lx06 package test: PASS")


if __name__ == "__main__":
    main()
