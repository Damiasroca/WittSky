#!/usr/bin/env python3
"""Build the on-device overlay font and images.

From the repository root:

    python tools/gen_overlay_assets.py

Needs Pillow. Reads tools/fonts/DejaVuSans-Bold.ttf, rainy.png, and rosa4.png.
Rewrites firmware/main/overlay_font.c, overlay_font.h, overlay_assets.c, and
overlay_assets.h.

Flash choice: one 512px rose (8-bit alpha, zlib) plus a tiny red-N sprite, one
96px rain icon, and three DejaVu sizes (14, 22, 32). The rose is scaled at draw
time to frame_height * 480 / 1200. Glyphs are blitted at the nearest size, not
scaled, so the text stays sharp. A full bitmap set for every OV2640 resolution
would not fit the remaining app-partition space.
"""

import pathlib
import zlib

from PIL import Image, ImageDraw, ImageFont

ROOT = pathlib.Path(__file__).resolve().parents[1]
FONT_PATH = ROOT / "tools" / "fonts" / "DejaVuSans-Bold.ttf"
RAIN_PATH = ROOT / "rainy.png"
ROSE_PATH = ROOT / "rosa4.png"
OUT_DIR = ROOT / "firmware" / "main"

FONT_PX = (14, 22, 32)
CHARS = list("0123456789.:-/ CEFNSWhkmnps") + ["°", "º"]
ROSE_SIDE = 512
RAIN_SIDE = 96


def c_bytes(name, data, static=True):
    stor = "static " if static else ""
    lines = [f"{stor}const uint8_t {name}[] = {{"]
    for i in range(0, len(data), 16):
        chunk = ", ".join(f"0x{b:02x}" for b in data[i:i + 16])
        lines.append(f"    {chunk},")
    lines.append("};")
    return "\n".join(lines)


def c_u16(name, words):
    lines = [f"static const uint16_t {name}[] = {{"]
    for i in range(0, len(words), 12):
        chunk = ", ".join(f"0x{w:04x}" for w in words[i:i + 12])
        lines.append(f"    {chunk},")
    lines.append("};")
    return "\n".join(lines)


def pack_a4(values):
    out = bytearray((len(values) + 1) // 2)
    for i, a in enumerate(values):
        nib = (a * 15 + 127) // 255
        if i & 1:
            out[i // 2] |= nib
        else:
            out[i // 2] = nib << 4
    return bytes(out)


def rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def build_font():
    fonts = []
    for px in FONT_PX:
        face = ImageFont.truetype(str(FONT_PATH), px)
        ascent, descent = face.getmetrics()
        glyphs = []
        bits = bytearray()
        for ch in CHARS:
            cp = ord(ch)
            adv = max(1, int(round(face.getlength(ch))))
            if ch == " ":
                glyphs.append((cp, 0, 0, 0, 0, adv, 0))
                continue
            box = face.getbbox(ch)
            l, t, r, b = box
            w, h = r - l, b - t
            if w <= 0 or h <= 0:
                glyphs.append((cp, 0, 0, 0, 0, adv, 0))
                continue
            img = Image.new("L", (w, h), 0)
            ImageDraw.Draw(img).text((-l, -t), ch, font=face, fill=255)
            raw = img.tobytes()
            off = len(bits)
            row = bytearray((w + 1) // 2)
            packed = bytearray()
            for y in range(h):
                row[:] = b"\x00" * len(row)
                for x in range(w):
                    nib = (raw[y * w + x] + 8) >> 4
                    if nib > 15:
                        nib = 15
                    if x & 1:
                        row[x // 2] |= nib
                    else:
                        row[x // 2] = nib << 4
                packed += row
            bits += packed
            glyphs.append((cp, w, h, l, t, adv, off))
        fonts.append({
            "px": px,
            "ascent": ascent,
            "descent": descent,
            "glyphs": glyphs,
            "bits": bytes(bits),
        })
    return fonts


def build_rose():
    im = Image.open(ROSE_PATH).convert("RGBA").resize(
        (ROSE_SIDE, ROSE_SIDE), Image.Resampling.LANCZOS)
    px = im.load()
    alpha = bytearray(ROSE_SIDE * ROSE_SIDE)
    reds = []
    for y in range(ROSE_SIDE):
        for x in range(ROSE_SIDE):
            r, g, b, a = px[x, y]
            i = y * ROSE_SIDE + x
            if a > 30 and r > 150 and r > g + 50 and r > b + 50:
                reds.append((x, y, a))
                alpha[i] = 0
            else:
                alpha[i] = a
    z = zlib.compress(bytes(alpha), 9)
    if reds:
        xs = [p[0] for p in reds]
        ys = [p[1] for p in reds]
        x0 = max(0, min(xs) - 2)
        y0 = max(0, min(ys) - 2)
        x1 = min(ROSE_SIDE - 1, max(xs) + 2)
        y1 = min(ROSE_SIDE - 1, max(ys) + 2)
        rw, rh = x1 - x0 + 1, y1 - y0 + 1
        red = bytearray(rw * rh)
        for x, y, a in reds:
            red[(y - y0) * rw + (x - x0)] = a
    else:
        x0 = y0 = rw = rh = 0
        red = b""
    return {
        "alpha_z": z,
        "alpha_len": len(alpha),
        "red": bytes(red),
        "red_x": x0,
        "red_y": y0,
        "red_w": rw,
        "red_h": rh,
    }


def build_rain():
    im = Image.open(RAIN_PATH).convert("RGBA").resize(
        (RAIN_SIDE, RAIN_SIDE), Image.Resampling.LANCZOS)
    px = im.load()
    words = []
    alphas = []
    for y in range(RAIN_SIDE):
        for x in range(RAIN_SIDE):
            r, g, b, a = px[x, y]
            words.append(rgb565(r, g, b))
            alphas.append(a)
    return {"rgb": words, "a4": pack_a4(alphas)}


def emit_font(fonts):
    parts = [
        "/* Generated by tools/gen_overlay_assets.py. Do not edit. */",
        '#include "overlay_font.h"',
        "",
    ]
    decl = []
    for i, f in enumerate(fonts):
        parts.append(c_bytes(f"s_bits_{i}", f["bits"]))
        parts.append("")
        parts.append(f"static const ov_glyph_t s_glyphs_{i}[] = {{")
        for cp, w, h, xo, yo, adv, off in f["glyphs"]:
            parts.append(
                f"    {{ 0x{cp:04x}, {w}, {h}, {adv}, {xo}, {yo}, {off} }},")
        parts.append("};")
        parts.append("")
        decl.append(
            f"    {{ {f['px']}, {f['ascent']}, {f['descent']}, "
            f"{len(f['glyphs'])}, s_glyphs_{i}, s_bits_{i} }},")
    parts.append("const ov_font_t ov_fonts[] = {")
    parts.extend(decl)
    parts.append("};")
    parts.append(f"const int ov_font_n = {len(fonts)};")
    parts.append("")
    (OUT_DIR / "overlay_font.c").write_text("\n".join(parts), encoding="utf-8")

    header = """/* Generated by tools/gen_overlay_assets.py. Do not edit.
 * Regenerate from the repo root: python tools/gen_overlay_assets.py
 */
#pragma once

#include <stdint.h>

typedef struct {
    uint16_t cp;
    uint8_t w, h, adv;
    int8_t xoff, yoff;
    uint32_t off;
} ov_glyph_t;

typedef struct {
    uint8_t px, ascent, descent;
    uint16_t n;
    const ov_glyph_t *glyphs;
    const uint8_t *bits;
} ov_font_t;

extern const ov_font_t ov_fonts[];
extern const int ov_font_n;
"""
    (OUT_DIR / "overlay_font.h").write_text(header, encoding="utf-8")


def emit_assets(rose, rain, font_bytes):
    rose_raw = ROSE_SIDE * ROSE_SIDE
    parts = [
        "/* Generated by tools/gen_overlay_assets.py. Do not edit.",
        " * Regenerate from the repo root: python tools/gen_overlay_assets.py",
        f" * Rose alpha zlib {len(rose['alpha_z'])} bytes "
        f"(raw {rose_raw}), red N {len(rose['red'])} bytes,",
        f" * rain {RAIN_SIDE}px RGB565+A4, fonts {font_bytes} bytes.",
        " */",
        '#include "overlay_assets.h"',
        "",
        c_bytes("s_rose_z", rose["alpha_z"]),
        "",
        c_bytes("s_red", rose["red"] or b"\x00"),
        "",
        c_u16("s_rain_rgb", rain["rgb"]),
        "",
        c_bytes("s_rain_a4", rain["a4"]),
        "",
        "const ov_rose_asset_t ov_rose_asset = {",
        f"    {ROSE_SIDE}, {ROSE_SIDE}, {rose['alpha_len']}, {len(rose['alpha_z'])}, s_rose_z,",
        f"    {rose['red_x']}, {rose['red_y']}, {rose['red_w']}, {rose['red_h']}, s_red",
        "};",
        "",
        "const ov_rain_asset_t ov_rain_asset = {",
        f"    {RAIN_SIDE}, {RAIN_SIDE}, s_rain_rgb, s_rain_a4",
        "};",
        "",
    ]
    (OUT_DIR / "overlay_assets.c").write_text("\n".join(parts), encoding="utf-8")
    header = """/* Generated by tools/gen_overlay_assets.py. Do not edit.
 * Regenerate from the repo root: python tools/gen_overlay_assets.py
 */
#pragma once

#include <stdint.h>

typedef struct {
    uint16_t w, h;
    uint32_t alpha_len;
    uint32_t zlen;
    const uint8_t *z;
    uint16_t red_x, red_y, red_w, red_h;
    const uint8_t *red_a;
} ov_rose_asset_t;

typedef struct {
    uint16_t w, h;
    const uint16_t *rgb565;
    const uint8_t *a4;
} ov_rain_asset_t;

extern const ov_rose_asset_t ov_rose_asset;
extern const ov_rain_asset_t ov_rain_asset;
"""
    (OUT_DIR / "overlay_assets.h").write_text(header, encoding="utf-8")


def main():
    fonts = build_font()
    rose = build_rose()
    got = zlib.decompress(rose["alpha_z"])
    if len(got) != rose["alpha_len"]:
        raise SystemExit("rose zlib round-trip failed")
    rain = build_rain()
    emit_font(fonts)
    font_bytes = sum(len(f["bits"]) for f in fonts)
    emit_assets(rose, rain, font_bytes)
    rain_bytes = RAIN_SIDE * RAIN_SIDE * 2 + len(rain["a4"])
    total = len(rose["alpha_z"]) + len(rose["red"]) + rain_bytes + font_bytes
    print(f"fonts {font_bytes} bytes")
    for f in fonts:
        print(f"  {f['px']}px glyphs {len(f['glyphs'])} bits {len(f['bits'])} "
              f"ascent {f['ascent']} descent {f['descent']}")
    print(f"rose zlib {len(rose['alpha_z'])} raw {rose['alpha_len']} "
          f"red {rose['red_w']}x{rose['red_h']} at {rose['red_x']},{rose['red_y']}")
    print(f"rain {rain_bytes} bytes")
    print(f"flash payload {total} bytes ({total / 1024:.1f} KiB)")


if __name__ == "__main__":
    main()
