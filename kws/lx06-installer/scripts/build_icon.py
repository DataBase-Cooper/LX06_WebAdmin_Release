from __future__ import annotations

import io
import struct
from pathlib import Path
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "assets" / "icon"
PNG_DIR = OUT / "png"
ICO_SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]

IVORY = (246, 239, 228, 255)
IVORY_DARK = (216, 201, 183, 255)
GRAPHITE = (37, 41, 38, 255)
ORANGE = (199, 93, 44, 255)
TRANSPARENT = (0, 0, 0, 0)


def scaled(v: float, factor: int) -> int:
    return max(1, round(v * factor))


def render_icon(size: int) -> Image.Image:
    # Render oversized, then downsample for clean fractional edges.
    aa = 8 if size <= 48 else 4
    canvas_size = size * aa
    image = Image.new("RGBA", (canvas_size, canvas_size), TRANSPARENT)
    draw = ImageDraw.Draw(image)
    s = canvas_size

    def box(x1: float, y1: float, x2: float, y2: float) -> tuple[int, int, int, int]:
        return (round(s * x1), round(s * y1), round(s * x2), round(s * y2))

    outer_margin = 0.055
    outer_radius = round(s * 0.225)
    border_width = max(aa, round(s * 0.018))
    draw.rounded_rectangle(
        box(outer_margin, outer_margin, 1 - outer_margin, 1 - outer_margin),
        radius=outer_radius,
        fill=IVORY,
        outline=IVORY_DARK,
        width=border_width,
    )

    # Strong speaker silhouette survives at 16 px.
    body = box(0.28, 0.18, 0.72, 0.82)
    draw.rounded_rectangle(body, radius=round(s * 0.14), fill=GRAPHITE)

    if size <= 20:
        # Tiny-size glyph: one high-contrast screen and two grille pixels.
        draw.rounded_rectangle(box(0.37, 0.29, 0.63, 0.47), radius=scaled(0.035 * size, aa), fill=IVORY)
        draw.ellipse(box(0.465, 0.34, 0.535, 0.41), fill=ORANGE)
        dot = max(aa, round(s * 0.035))
        for cx in (0.44, 0.56):
            x, y = round(s * cx), round(s * 0.65)
            draw.ellipse((x - dot, y - dot, x + dot, y + dot), fill=IVORY)
    else:
        # Touchscreen inset.
        draw.rounded_rectangle(box(0.35, 0.27, 0.65, 0.49), radius=round(s * 0.06), fill=IVORY)
        center = (round(s * 0.50), round(s * 0.385))
        r = max(aa, round(s * 0.026))
        draw.ellipse((center[0] - r, center[1] - r, center[0] + r, center[1] + r), fill=ORANGE)

        # At 24–40 px use one pair of arcs; larger sizes get a second pair.
        width = max(aa, round(s * (0.032 if size < 48 else 0.025)))
        inner = box(0.405, 0.305, 0.595, 0.47)
        draw.arc(inner, 135, 225, fill=ORANGE, width=width)
        draw.arc(inner, -45, 45, fill=ORANGE, width=width)
        if size >= 48:
            outer = box(0.365, 0.275, 0.635, 0.50)
            outer_width = max(aa, round(s * 0.018))
            draw.arc(outer, 135, 225, fill=ORANGE, width=outer_width)
            draw.arc(outer, -45, 45, fill=ORANGE, width=outer_width)

        grille_radius = max(aa, round(s * (0.022 if size < 48 else 0.024)))
        positions = [(0.43, 0.63), (0.50, 0.63), (0.57, 0.63)]
        if size >= 40:
            positions += [(0.43, 0.70), (0.50, 0.70), (0.57, 0.70)]
        for cx, cy in positions:
            x, y = round(s * cx), round(s * cy)
            color = ORANGE if size >= 40 and cx == 0.50 and cy == 0.70 else IVORY
            draw.ellipse((x - grille_radius, y - grille_radius, x + grille_radius, y + grille_radius), fill=color)

    return image.resize((size, size), Image.Resampling.LANCZOS)


def png_bytes(image: Image.Image) -> bytes:
    buffer = io.BytesIO()
    image.save(buffer, format="PNG", optimize=True)
    return buffer.getvalue()


def write_ico(frames: list[tuple[int, bytes]], path: Path) -> None:
    header = struct.pack("<HHH", 0, 1, len(frames))
    offset = 6 + 16 * len(frames)
    entries: list[bytes] = []
    payload = bytearray()
    for size, data in frames:
        dimension = 0 if size == 256 else size
        entries.append(struct.pack("<BBBBHHII", dimension, dimension, 0, 0, 1, 32, len(data), offset))
        payload.extend(data)
        offset += len(data)
    path.write_bytes(header + b"".join(entries) + payload)


def make_preview(frames: dict[int, Image.Image]) -> Image.Image:
    width, height = 1120, 620
    preview = Image.new("RGB", (width, height), (246, 239, 228))
    draw = ImageDraw.Draw(preview)
    draw.rounded_rectangle((28, 28, width - 28, height - 28), radius=30, fill=(255, 252, 247), outline=(216, 201, 183), width=3)
    draw.text((64, 58), "LX06 APP ICON · MULTI-SIZE", fill=(37, 41, 38))
    x = 66
    for size in ICO_SIZES:
        display = min(128, max(48, size))
        icon = frames[size].resize((display, display), Image.Resampling.NEAREST if size <= 24 else Image.Resampling.LANCZOS)
        y = 150 + (128 - display) // 2
        preview.paste(icon, (x, y), icon)
        draw.text((x, 300), f"{size}px", fill=(37, 41, 38))
        x += max(84, display + 24)
        if x > width - 140:
            x = 66
            y2 = 400
    # Show the master on graphite to prove transparent corners.
    master = frames[256].resize((180, 180), Image.Resampling.LANCZOS)
    draw.rounded_rectangle((830, 370, 1065, 575), radius=28, fill=(37, 41, 38))
    preview.paste(master, (858, 382), master)
    draw.text((64, 405), "Warm ivory / graphite / terracotta\nTransparent corners · no text · tiny-size simplification", fill=(92, 96, 92), spacing=8)
    return preview


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    PNG_DIR.mkdir(parents=True, exist_ok=True)
    images: dict[int, Image.Image] = {}
    frames: list[tuple[int, bytes]] = []
    for size in ICO_SIZES:
        image = render_icon(size)
        images[size] = image
        data = png_bytes(image)
        (PNG_DIR / f"lx06-app-{size}.png").write_bytes(data)
        frames.append((size, data))
    master = render_icon(512)
    master.save(OUT / "lx06-app-512.png", format="PNG", optimize=True)
    write_ico(frames, OUT / "lx06-app.ico")
    make_preview(images).save(OUT / "lx04-icon-preview.png", format="PNG", optimize=True)

    with Image.open(OUT / "lx06-app.ico") as icon:
        if icon.format != "ICO":
            raise RuntimeError("generated icon is not a valid ICO")
    print("ICON_OK", OUT / "lx06-app.ico", "layers=", ",".join(map(str, ICO_SIZES)))


if __name__ == "__main__":
    main()
