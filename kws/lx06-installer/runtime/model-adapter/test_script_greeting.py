"""Exercise greeting publication through the same settings commands as Web."""

import io
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
import wave


@unittest.skipUnless(os.geteuid() == 0, "requires root-owned fixture files")
class ScriptGreetingTests(unittest.TestCase):
    def test_upload_save_regenerate_and_disable(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            apps = root / "apps"
            apps.mkdir()
            source = apps / "voice.sh"
            schema = dict(version=1, title="Voice", fields=[
                dict(name="greeting", env="MODEL_GREETING", type="text", label="Greeting",
                     default="Original", maxBytes=240)],
                actions=[dict(id="save", label="Save", fields=["greeting"])])
            source.write_text("#!/bin/sh\n# LX06-EXTENSION-BEGIN\n# " + json.dumps(schema) +
                              "\n# LX06-EXTENSION-END\n")
            synth = root / "synth"
            synth.write_text(
                "#!/usr/bin/python3\n"
                "import pathlib, sys, wave\n"
                "assert sys.argv[1:] == ['--synthesize-fresh']\n"
                "text = sys.stdin.buffer.read()\n"
                "if text == b'fail': raise SystemExit(3)\n"
                "with wave.open(sys.stdout.buffer, 'wb') as f:\n"
                "    f.setparams((1, 2, 16000, 0, 'NONE', ''))\n"
                "    f.writeframes(text[:1] * 320)\n"
            )
            synth.chmod(0o700)
            tool = root / "settings"
            here = Path(__file__).resolve().parent
            definitions = dict(LX06_GREETING_DIR=root / "greetings", LX06_GREETING_APPS=apps,
                               LX06_SETTINGS_DIR=root / "settings-store", LX06_GREETING_SYNTH=synth)
            subprocess.run(["cc", "-O2", *(f'-D{k}="{v}"' for k, v in definitions.items()),
                            here / "script-settings.c", "-o", tool], check=True)

            def call(command, text=None):
                body = "" if text is None else json.dumps(dict(action="save", values={"greeting": text}))
                result = subprocess.run([tool, command, source], input=body, text=True,
                                        capture_output=True, check=True, timeout=10)
                return json.loads(result.stdout)

            asset = root / "greetings" / source.name / "greeting.cache"

            def cached(text):
                data = asset.read_bytes()
                self.assertEqual(data[:241].split(b"\0")[0], text.encode())
                with wave.open(io.BytesIO(data[241:])) as wav:
                    self.assertEqual(wav.readframes(160), text.encode()[:1] * 320)
                self.assertEqual(asset.stat().st_mode & 0o777, 0o600)

            call("greeting-upload")
            cached("Original")
            call("save", "Replacement")
            cached("Replacement")
            with asset.open("rb") as old:
                call("greeting")
                self.assertNotEqual(os.fstat(old.fileno()).st_ino, asset.stat().st_ino)
            cached("Replacement")
            self.assertIn("生成失败", call("save", "fail")["message"])
            cached("Replacement")
            call("save", "")
            self.assertFalse(asset.exists())


if __name__ == "__main__":
    unittest.main()
