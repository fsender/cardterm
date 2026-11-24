#!/usr/bin/env python3
"""build_wqy8x14.py — 生成 8x14 终端字体（文泉驿点阵宋体 14px + cardterm 西文）。

字体组成:
  西文/符号 0x20-0x7F + CP437 符号位: cardterm/extra/cardterm_1.png 手绘
             （格内墨迹 7x13, 单元 8x14, 墨迹贴左上、右/下各留 1px）
  Unicode BMP : 文泉驿点阵宋体（WenQuanYi Bitmap Song, GPL-2.0+font-exception）
             wenquanyi_13px.bdf (SIZE 14, CJK 14px/墨迹 13x13) 为主,
             缺字依次 wenquanyi_10pt.bdf (13px) / wenquanyi_9pt.bdf (12px),
             再缺 → Fusion Pixel 10px（同 6x10 表的兜底）
  制表符/方块: boxdraw 参数化重绘（U+2500-259F, 满格连续）, 覆盖上游
  盲文 U+2800-28FF: 参数化 2x4 点阵

单元: 窄 8x14（墨迹 7x13, 基线 row 10/11 之间）; 宽 16x14（CJK, 墨迹 13x13, rows 0-12）。
终端至少 8x14 才能用本字体（7x13 墨迹 + 1px 间距）。

流程:
  PNG + BDF → 合并 BDF → bdfconv → u8g2 C → 解码校验 → src/term_font_8x14.h
"""
import json
import os
import subprocess
import sys

import boxdraw

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.dirname(HERE)
ROOT = os.path.dirname(PROJ)

sys.path.insert(0, HERE)
from gen_term_font import FontData, load_u8g2_c          # noqa: E402
from build_small_font import parse_bdf, bdf_glyph_lines  # noqa: E402

PNG      = os.path.join(ROOT, "cardterm", "extra", "cardterm_1.png")
WQY_DIR  = os.path.join(ROOT, ".tmp", "wqy", "wqy-bitmapsong")
WQY_MAIN = os.path.join(WQY_DIR, "wenquanyi_13px.bdf")   # SIZE 14
WQY_FB   = [os.path.join(WQY_DIR, "wenquanyi_10pt.bdf"), # SIZE 13
            os.path.join(WQY_DIR, "wenquanyi_9pt.bdf")]  # SIZE 12
BOUTIQUE = os.path.join(ROOT, "extra_assets", "fonts", "boutique9x9_10px.bdf")
FUSION   = os.path.join(ROOT, "extra_assets", "fonts", "fusion-pixel-10px.bdf")
OUT_BDF  = os.path.join(ROOT, "extra_assets", "fonts", "cardterm_wqy_8x14.bdf")
OUT_C    = os.path.join(ROOT, "extra_assets", "fonts", "cardterm_wqy_8x14_u8g2.c")
OUT_H    = os.path.join(PROJ, "src", "term_font_8x14.h")
BDFCONV  = os.path.join(ROOT, ".tmp", "u8g2", "tools", "font", "bdfconv", "bdfconv")

CELL_W, CELL_H, ROWS = 8, 14, 14
BDF_BASE = 11          # BDF y=0 ↔ 单元 row 11；row r ↔ y = 11 - r
R0_NARROW = 10         # 窄字形基线（y=0 → row 10，与手绘西文一致）
R0_WIDE   = 11         # 宽字形（CJK）基线：13x13 墨迹落 rows 0..12

# 圆点族强制单宽（与前两张表一致）
WIDTH1 = {0x25CF, 0x25CB, 0x2022, 0x2219, 0x22C5, 0x00B7, 0x25C9, 0x25CE}


def cp_cells(cp):
    """与 terminal.cpp cp_width() 的 2 格判定保持一致（advance 由 cp_width 决定）。"""
    if cp in WIDTH1:
        return 1
    return 2 if ((0x1100 <= cp <= 0x115F) or (0x2E80 <= cp <= 0xA4CF) or
                 (0xAC00 <= cp <= 0xD7A3) or (0xF900 <= cp <= 0xFAFF) or
                 (0xFE30 <= cp <= 0xFE4F) or (0xFF00 <= cp <= 0xFF60) or
                 (0xFFE0 <= cp <= 0xFFE6)) else 1


# ── 手绘 PNG：CP437 布局 16 列 x 8 行（r2..r9, c1..c16 → 码点 0x00-0x7F）──────
CP437_UNI = {
    0x01: 0x263A, 0x02: 0x263B, 0x03: 0x2665, 0x04: 0x2666, 0x05: 0x2663,
    0x06: 0x2660, 0x07: 0x2022, 0x08: 0x25D8, 0x09: 0x25CB, 0x0A: 0x25D9,
    0x0B: 0x2642, 0x0C: 0x2640, 0x0D: 0x266A, 0x0E: 0x266B, 0x0F: 0x263C,
    0x10: 0x25BA, 0x11: 0x25C4, 0x12: 0x2195, 0x13: 0x203C, 0x14: 0x00B6,
    0x15: 0x00A7, 0x16: 0x25AC, 0x17: 0x21A8, 0x18: 0x2191, 0x19: 0x2193,
    0x1A: 0x2192, 0x1B: 0x2190, 0x1C: 0x221F, 0x1D: 0x2194, 0x1E: 0x25B2,
    0x1F: 0x25BC,
}


def read_png_glyphs(path):
    from PIL import Image
    import numpy as np
    a = np.array(Image.open(path).convert("RGB"))
    ink = (np.all(a == (0, 0, 0), axis=2))
    vx = [2, 10, 18, 26, 34, 42, 50, 58, 66, 74, 76, 84, 92, 100,
          108, 116, 124, 132, 140]
    hy = [2, 16, 30, 44, 58, 72, 86, 100, 114, 128, 142]
    cols = [(vx[i] + 1, vx[i + 1]) for i in range(len(vx) - 1)
            if vx[i + 1] - (vx[i] + 1) >= 7]
    rows = [(hy[i] + 1, hy[i + 1]) for i in range(len(hy) - 1)]
    assert len(cols) == 17 and len(rows) == 10
    out = {}
    for r in range(2, 10):
        for c in range(1, 17):
            cp = 0x10 * (r - 2) + (c - 1)
            y0, y1 = rows[r]
            x0, x1 = cols[c]
            cell = ink[y0:y1, x0:x1]
            assert cell.shape == (13, 7)
            art = [[1 if v else 0 for v in row] for row in cell]
            if cp == 0x7F:
                uni = 0x0394
            elif cp >= 0x20:
                uni = cp
            elif cp == 0x00:
                continue
            else:
                uni = CP437_UNI[cp]
            out[uni] = art
    out[0x2206] = out[0x0394]      # Δ 同时给 U+2206 INCREMENT
    return out


def png_bdf(cp, art):
    """手绘 7x13（rows 0-12）→ 单元 8x14 内 rows 0-12, cols 0-6（右/下留空）。"""
    nz = [(x, y) for y in range(13) for x in range(7) if art[y][x]]
    if not nz:
        return bdf_glyph_lines(cp, 1, 1, 0, 0, [[0]], CELL_W, "PNG")
    x1 = max(p[0] for p in nz)
    y1 = max(p[1] for p in nz)
    # 墨迹底行 y1（0-12）→ BDF yoff = BDF_BASE - y1
    return bdf_glyph_lines(cp, x1 + 1, y1 + 1, 0, BDF_BASE - y1,
                           [row[:x1 + 1] for row in art[:y1 + 1]], CELL_W, "PNG")


# ── BDF 源字形（w,h,xo,yo,pix）→ 单元 BDF ────────────────────────────────
def src_bdf(cp, src, cells):
    w, h, xo, yo, pix = src
    if not w or not h:
        return bdf_glyph_lines(cp, 1, 1, 0, 0, [[0]], 8 * cells, "blank")
    r0 = R0_WIDE if cells == 2 else R0_NARROW
    # 墨迹在单元中的行范围: row = r0 - y, y ∈ [yo, yo+h-1]
    top = r0 - (yo + h - 1)
    bot = r0 - yo
    if cells == 1:
        # 窄字形：保留源左 bearing，夹到 7 列墨区（cols 0..6, 右留 1px）；
        # 超宽墨迹贴左、向右溢出（与 6x10 表同策略）。
        if w <= 7:
            xoff = min(max(xo, 0), 7 - w)
        else:
            xoff = max(xo, 0)
    else:
        # 宽字形：墨迹在 16 格内居中（13px 源 ink 13 → x=1..2）
        xoff = (16 - w) // 2 if w < 16 else 0
    # 裁剪到行 0..13
    keep = [(r, row) for r, row in zip(range(top, bot + 1), pix) if 0 <= r < ROWS]
    if not keep:
        return bdf_glyph_lines(cp, 1, 1, 0, 0, [[0]], 8 * cells, "empty")
    r_top = keep[0][0]
    r_bot = keep[-1][0]
    rows2 = [row for _, row in keep]
    bbx_w = w
    # BDF yoff = BDF_BASE - r_bot
    return bdf_glyph_lines(cp, bbx_w, r_bot - r_top + 1, xoff, BDF_BASE - r_bot,
                           rows2, 8 * cells, "BDF")


# ── 参数化盲文 U+2800-28FF（2x4 点阵, 2x2 px 点）────────────────────────
def braille_bdf(cp):
    bits = cp - 0x2800
    # 盲文点位: bit0..2 = 左列 1,2,3; bit3..5 = 右列 4,5,6; bit6/7 = 左/右下 7,8
    dot = {0: (1, 1), 1: (1, 4), 2: (1, 7), 3: (5, 1), 4: (5, 4), 5: (5, 7),
           6: (1, 10), 7: (5, 10)}
    rows = [[0] * 7 for _ in range(13)]
    for b in range(8):
        if bits & (1 << b):
            x, y = dot[b]
            for dy in range(2):
                for dx in range(2):
                    if 0 <= y + dy < 13 and 0 <= x + dx < 7:
                        rows[y + dy][x + dx] = 1
    return png_bdf(cp, rows)


# ── 框线/方块：boxdraw 参数化（满格 8x14 / 连续）────────────────────────
def boxed_bdf(boxed):
    out = {}
    for cp, pts in boxed.items():
        rows = [[0] * CELL_W for _ in range(CELL_H)]
        for x, y in pts:
            if 0 <= x < CELL_W and 0 <= y < CELL_H:
                rows[y][x] = 1
        nz = [(x, y) for y in range(CELL_H) for x in range(CELL_W) if rows[y][x]]
        if not nz:
            out[cp] = bdf_glyph_lines(cp, 1, 1, 0, 0, [[0]], CELL_W, "box")
            continue
        x1 = max(p[0] for p in nz)
        y1 = max(p[1] for p in nz)
        out[cp] = bdf_glyph_lines(cp, x1 + 1, y1 + 1, 0, BDF_BASE - y1,
                                  [row[:x1 + 1] for row in rows[:y1 + 1]],
                                  CELL_W, "box")
    return out


def run_bdfconv(bdf_path, c_path):
    cmd = [BDFCONV, "-f", "1", "-b", "0", "-m", "0-65535",
           "-o", c_path, "-n", "cardterm_wqy8x14", bdf_path]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("bdfconv 失败:\n" + r.stdout + r.stderr)
    if r.stderr.strip():
        print("bdfconv stderr:", r.stderr.strip()[:500])


def build_table(decoded):
    glyphs, bits, bad = [], [], []
    for cp in sorted(decoded):
        w, h, x, y, d, rows = decoded[cp]
        cells = 2 if cp >= 0x80 and cp_cells(cp) == 2 else 1
        first_row = BDF_BASE - (y + h - 1)
        ink = []
        if w and h:
            for ry, row in enumerate(rows):
                cr = first_row + ry
                if cr < 0 or cr >= ROWS:
                    continue
                for rx, v in enumerate(row):
                    if v:
                        sx = x + rx
                        if 0 <= sx < 16:
                            ink.append((sx, cr))
        box_w = (max(sx for sx, _ in ink) + 1) if ink else CELL_W * cells
        if box_w > 16:
            box_w = 16
        if first_row < 0 or first_row + h > ROWS:
            bad.append((cp, first_row, h))
        rb = [0] * ROWS
        for sx, cr in ink:
            rb[cr] |= 1 << sx
        glyphs.append((cp, cells, box_w, rb))
        bits.extend(rb)
    return glyphs, bits, bad


def emit_header(glyphs, bits, path):
    L = []
    L.append("// term_font_8x14.h — 终端等宽字体 8x14（文泉驿点阵宋体 14px, 自动生成, 勿手改）")
    L.append("// 生成器: helloworld_customfont/tools/build_wqy8x14.py")
    L.append("// 西文/CP437 符号: cardterm/extra/cardterm_1.png 手绘（墨迹 7x13）")
    L.append("// Unicode: 文泉驿点阵宋体 wenquanyi_13px.bdf（GPL-2.0 + 字体嵌入例外）")
    L.append("//   兜底 wenquanyi_10pt/9pt + Fusion Pixel 10px；框线/方块 boxdraw 参数化；")
    L.append("//   盲文 U+2800-28FF 参数化。宽格 16x14（CJK 墨迹 13x13, rows 0-12）")
    L.append("#pragma once")
    L.append('#include "termfont_core.h"')
    L.append("")
    L.append("namespace termfont {")
    L.append("")
    L.append("// 每字形 14 行, 每行 uint16: 位 c = 列 c（LSB 为最左列）")
    L.append("static const uint16_t bits_8x14[] = {")
    per = 16
    for i in range(0, len(bits), per):
        L.append("  " + ", ".join("0x%04X" % v for v in bits[i:i + per]) + ",")
    L.append("};")
    L.append("")
    L.append("static const TermGlyph glyphs_8x14[] = {")
    for idx, (cp, cells, w, rb) in enumerate(glyphs):
        L.append("  {0x%04X, %d, %d, %d}," % (cp, cells, w, idx * ROWS))
    L.append("};")
    L.append("")
    L.append("static const FontTable font_8x14 = {")
    L.append("  glyphs_8x14, %d, bits_8x14, %d, %d, %d"
             % (len(glyphs), ROWS, CELL_W, CELL_H))
    L.append("};")
    L.append("")
    L.append("}  // namespace termfont")
    open(path, "w", encoding="utf-8").write("\n".join(L) + "\n")
    print("wrote", path, "-", len(glyphs), "glyphs,", len(bits), "u16 bits")


def main():
    png = read_png_glyphs(PNG)
    print("png glyphs:", len(png))

    wqy_main = parse_bdf(WQY_MAIN)
    wqy_fbs = [parse_bdf(p) for p in WQY_FB if os.path.exists(p)]
    boutique = parse_bdf(BOUTIQUE) if os.path.exists(BOUTIQUE) else {}
    fusion = parse_bdf(FUSION) if os.path.exists(FUSION) else {}
    print(f"wqy: main={len(wqy_main)} fallbacks={[len(f) for f in wqy_fbs]} "
          f"boutique={len(boutique)} fusion={len(fusion)}")

    glyphs = {}
    stats = {"png": 0, "wqy": 0, "fb": 0, "boutique": 0, "fusion": 0, "box": 0, "braille": 0}

    def put(cp, lines):
        if cp not in glyphs:
            glyphs[cp] = lines

    # 1) 手绘西文/符号（优先级最高）
    for cp in sorted(png):
        put(cp, png_bdf(cp, png[cp]))
        stats["png"] += 1

    # 2) 文泉驿主/兜底 → Boutique → Fusion
    for kind, table in [("wqy", wqy_main),
                        ("fb", None),
                        ("fb", boutique),
                        ("fusion", fusion)]:
        if kind == "fb" and table is None:
            for fb in wqy_fbs:
                for cp in sorted(fb):
                    if cp < 0x20 or cp in glyphs:
                        continue
                    put(cp, src_bdf(cp, fb[cp], cp_cells(cp)))
                    stats["fb"] += 1
            continue
        for cp in sorted(table):
            if cp < 0x20 or cp in glyphs:
                continue
            put(cp, src_bdf(cp, table[cp], cp_cells(cp)))
            stats["boutique" if table is boutique else kind] += 1

    # 3) 盲文参数化（强制覆盖上游，点阵按 2x4 设计）
    for cp in range(0x2800, 0x2900):
        glyphs[cp] = braille_bdf(cp)
        stats["braille"] += 1

    # 4) 框线/方块参数化（覆盖上游，满格连续）
    box_src = {}
    for cp in range(0x2500, 0x25A0):
        e = wqy_main.get(cp)
        if e:
            w, h, xo, yo, rows = e
            box_src[cp] = (w, h, rows)
    boxed = boxdraw.box_glyphs(CELL_W, CELL_H, box_src)
    for cp, lines in boxed_bdf(boxed).items():
        glyphs[cp] = lines
        stats["box"] += 1

    lines = [
        "STARTFONT 2.1",
        "FONT -CardTerm-CardTermWqy-Medium-R-Normal--14-140-72-72-C-160-ISO10646-1",
        "SIZE 14 72 72",
        "FONTBOUNDINGBOX 16 14 0 -2",
        "STARTPROPERTIES 9",
        'FAMILY_NAME "CardTerm WQY 8x14"',
        'COPYRIGHT "Western: hand-drawn by cardterm author. Unicode: WenQuanYi Bitmap Song, GPL-2.0 with font embedding exception. Fallback: Fusion Pixel (OFL-1.1)."',
        "FONT_ASCENT 12",
        "FONT_DESCENT 2",
        "DEFAULT_CHAR 32",
        'CHARSET_REGISTRY "ISO10646"',
        "ENDPROPERTIES",
        f"CHARS {len(glyphs)}",
    ]
    for cp in sorted(glyphs):
        lines.extend(glyphs[cp])
    lines.append("ENDFONT")
    open(OUT_BDF, "w", encoding="utf-8").write("\n".join(lines) + "\n")
    print("wrote", OUT_BDF, "-", len(glyphs), "glyphs,",
          os.path.getsize(OUT_BDF), "bytes")
    print("stats:", stats)

    run_bdfconv(OUT_BDF, OUT_C)
    fd = FontData(load_u8g2_c(OUT_C))
    table = {}
    table.update(fd.ascii_glyphs())
    table.update(fd.unicode_glyphs())
    decoded = {cp: fd.glyph(off) for cp, off in table.items()}
    print("u8g2 decoded:", len(decoded), "glyphs; start_a=", fd.start_a,
          "start_unicode=", hex(fd.start_unicode))

    # 校验：关键码点在单元里的点阵
    def cell_rows(cp):
        w, h, x, y, d, rows = decoded[cp]
        first = BDF_BASE - (y + h - 1)
        grid = [[0] * 16 for _ in range(ROWS)]
        for ry, row in enumerate(rows):
            for rx, v in enumerate(row):
                if v and 0 <= first + ry < ROWS and 0 <= x + rx < 16:
                    grid[first + ry][x + rx] = 1
        return grid

    checks = {0x41: (1, 10), 0x30: (1, 10), 0x4E2D: (2, 12), 0x2502: (1, 13),
              0x2588: (1, 13), 0x263A: (1, 11)}
    bad = []
    for cp, (cells, lastrow) in checks.items():
        if cp not in decoded:
            bad.append((cp, "missing"))
            continue
        g = cell_rows(cp)
        used = [y for y in range(ROWS) if any(g[y])]
        if not used:
            bad.append((cp, "empty"))
        elif used[-1] != lastrow:
            bad.append((cp, f"bottom row {used[-1]} != {lastrow}"))
    if bad:
        print("校验异常:", bad)
    else:
        print("校验通过")

    glyphs_t, bits, bad2 = build_table(decoded)
    if bad2:
        print("越界裁剪字形数:", len(bad2), bad2[:8])
    emit_header(glyphs_t, bits, OUT_H)

    json.dump({str(k): [list(r) for r in v] for k, v in png.items()},
              open(os.path.join(HERE, "..", "..", ".tmp", "wqy",
                                "png_glyphs.json"), "w"), indent=0)


if __name__ == "__main__":
    main()
