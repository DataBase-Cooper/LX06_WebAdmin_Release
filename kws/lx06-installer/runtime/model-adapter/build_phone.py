"""Build the sanitized phone upload with the original reusable voice caller.

No private values, runtime service or provider credentials are embedded.
"""
import argparse
from pathlib import Path

ROOT = Path(__file__).resolve().parent
TEMPLATE = ROOT.parents[1] / "scripts/examples/user-scripts/phone-bridge-v1/doubao_phone.sh.tpl"


def build(destination: Path):
    voice = (ROOT / "account-voice.sh").read_text(encoding="utf-8")
    marker = 'case "${1:-}" in'
    functions, dispatcher = voice.rsplit(marker, 1)
    if "phone_session()" not in functions or "--phone-wakeup" not in dispatcher:
        raise ValueError("phone caller is missing from the original voice source")
    template = TEMPLATE.read_text(encoding="utf-8")
    placeholder = "@@ACCOUNT_VOICE_FUNCTIONS@@"
    if template.count(placeholder) != 1:
        raise ValueError("phone template must contain one function placeholder")
    script = template.replace(placeholder, functions.rstrip())
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(script, encoding="utf-8", newline="\n")
    return destination


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("destination", type=Path)
    build(parser.parse_args().destination)
