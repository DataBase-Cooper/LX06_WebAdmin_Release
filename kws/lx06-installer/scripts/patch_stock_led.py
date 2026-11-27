"""Preserve the stock LED dispatcher and interlock dialogue shutdown."""
from pathlib import Path
import sys


def patch_stock_led(stock: bytes, guard: bytes) -> bytes:
    if guard.rstrip() in stock:
        return stock
    if not stock.startswith(b"#!/bin/sh\n") or not all(token in stock for token in (
        b"*shut_led)", b'ubus -t 1 call led shut "{\\"L\\":$OP}"'
    )):
        raise ValueError("unsupported LX06 stock LED dispatcher")
    first, rest = stock.split(b"\n", 1)
    return first + b"\n" + guard.rstrip() + b"\n\n" + rest


if __name__ == "__main__":
    path = Path(sys.argv[1])
    guard = Path(__file__).resolve().parents[1] / "hooks/xiaoai/stock-led-guard.inc"
    path.write_bytes(patch_stock_led(path.read_bytes(), guard.read_bytes()))
