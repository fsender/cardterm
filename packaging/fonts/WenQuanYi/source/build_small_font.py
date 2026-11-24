#!/usr/bin/env python3
"""build_small_font.py — 生成 4x8 小号终端字体（cardterm 0.4.2）。

字体组成:
  ASCII 0x00-0x7F : cardterm/extra/cardterm_font.png 里手工绘制的 4x6 点阵
                    （逐字 4 列、行 1..6；4px 顶格，不做任何缩放/居中）
  Unicode BMP     : ChillBitmap 7px（真实设计 8px；墨迹 7x7，基线 row 6）
                    缺字 → extra_assets/fonts/fallback8px/*.bdf（若存在，按文件名顺序）
  单元            : 窄 4x8 px（ASCII 与 EAW 非 W/F），宽 8x8 px（EAW W/F）

流程:
  PNG + BDF → 合并 BDF → bdfconv → u8g2 C → 解码校验 → src/term_font_small.h
"""
import json
import os
import re
import subprocess
import sys
import unicodedata

import boxdraw

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.dirname(HERE)
ROOT = os.path.dirname(PROJ)

sys.path.insert(0, HERE)
from gen_term_font import FontData, load_u8g2_c  # noqa: E402

PNG       = os.path.join(ROOT, "cardterm", "extra", "cardterm_font.png")
CHILL     = os.path.join(ROOT, "extra_assets", "fonts", "chillbitmap", "ChillBitmap_7px_8px.bdf")
FALLBACK_DIR = os.path.join(ROOT, "extra_assets", "fonts", "fallback8px")
OUT_BDF   = os.path.join(ROOT, "extra_assets", "fonts", "cardterm_small_4x7.bdf")
OUT_C     = os.path.join(ROOT, "extra_assets", "fonts", "cardterm_small_4x7_u8g2.c")
OUT_JSON  = os.path.join(ROOT, "extra_assets", "fonts", "cardterm_ascii_4x6.json")
OUT_H     = os.path.join(PROJ, "src", "term_font_small.h")
BDFCONV   = os.path.join(ROOT, ".tmp", "u8g2", "tools", "font", "bdfconv", "bdfconv")

CELL_W, CELL_H, ROWS = 4, 7, 7
BASE_ROW = 5          # 单元 row 0 的 BDF 顶行 y = 5（ascent 6：y=5..0 / 下行 y=-1,-2）

# ── 手绘图：4 列 × 6 行，画在 5×7 的网格里（网格线 x=1,6,11…；y=7,14,…）────────
def read_ascii_png(path):
    from PIL import Image
    im = Image.open(path).convert("RGBA")
    px = im.load()
    if im.size != (192, 80):
        sys.exit(f"{path}: 期望 192x80，实际 {im.size}")

    def ink(x, y):
        r, g, b, a = px[x, y]
        return (r, g, b) == (0, 0, 0)

    out = {}
    for r in range(4):
        for c in range(32):
            cp = r * 32 + c
            x0 = 2 + c * 5 + (c // 8) * 2
            y0 = 8 + r * 7
            rows = []
            for dy in range(6):
                bits = 0
                for dx in range(4):
                    if ink(x0 + dx, y0 + dy):
                        bits |= 1 << (3 - dx)
                rows.append(bits)
            out[cp] = rows
    # 抽样校验：图上的 'A' 必须是我们看到的形状
    a = out[0x41]
    assert a[1] == 0b1010 and a[2] == 0b1110, f"ASCII 图解析异常: A={a}"
    return out


# ── BDF 读取：返回 {cp: (w, h, xoff, yoff, rows[w 位, 最高位=最左列])} ──────────
def parse_bdf(path):
    out = {}
    cp = None
    bbx = None
    rows = []
    in_bm = False
    for line in open(path, encoding="utf-8", errors="replace"):
        line = line.rstrip("\n")
        if line.startswith("ENCODING "):
            cp = int(line.split()[1])
        elif line.startswith("BBX "):
            bbx = tuple(int(v) for v in line.split()[1:])
        elif line == "BITMAP":
            in_bm = True
            rows = []
        elif in_bm and re.fullmatch(r"[0-9A-Fa-f]+", line):
            rows.append(line)
        elif line == "ENDCHAR":
            if cp is not None and bbx is not None:
                w, h, xo, yo = bbx
                pix = []
                for hexs in rows:
                    v = int(hexs, 16)
                    total = 4 * len(hexs)
                    pix.append([(v >> (total - 1 - i)) & 1 for i in range(w)])
                if len(pix) != h:
                    pix = (pix + [[0] * w for _ in range(h)])[:h]
                out[cp] = (w, h, xo, yo, pix)
            in_bm = False
            cp = None
            bbx = None
    return out


def ink_bbox(rows, w, h):
    xs = [x for y in range(h) for x in range(w) if rows[y][x]]
    ys = [y for y in range(h) if any(rows[y])]
    if not xs:
        return None
    return min(xs), max(xs), min(ys), max(ys)


def crop_rows(rows, w, x0, x1, y0, y1):
    return [[rows[y][x] for x in range(x0, x1 + 1)] for y in range(y0, y1 + 1)]


# 合成一个 BDF 字形（宽度 w、高度 h、XOFF、YOFF），行按 4 位对齐输出
def bdf_glyph_lines(cp, w, h, xo, yo, pix, dwidth, name):
    digits = (w + 3) // 4
    lines = [f"STARTCHAR U+{cp:04X}", f"ENCODING {cp}", "SWIDTH 500 0",
             f"DWIDTH {dwidth} 0", f"BBX {w} {h} {xo} {yo}", "BITMAP"]
    for row in pix:
        v = 0
        for b in row:
            v = (v << 1) | b
        lines.append("%0*X" % (digits, v << (4 * digits - w)))
    lines.append("ENDCHAR")
    return lines


def ascii_glyph(cp, rows4):
    """手绘 4x6（行 0..5 → 单元行 1..6）→ BDF 字形（保持原 x 位置/顶格）。"""
    nz = [(x, y) for y in range(6) for x in range(4) if rows4[y] & (0x8 >> x)]
    if not nz:
        return bdf_glyph_lines(cp, 4, 6, 0, -1, [[0] * 4 for _ in range(6)], 4, "ASCII")
    x0 = min(p[0] for p in nz)
    x1 = max(p[0] for p in nz)
    y0 = min(p[1] for p in nz)
    y1 = max(p[1] for p in nz)
    pix = [[1 if rows4[y] & (0x8 >> x) else 0 for x in range(x0, x1 + 1)]
           for y in range(y0, y1 + 1)]
    # 单元行 = 1+y → cell_row_bottom = 1+y1；yoff = BASE_ROW - cell_row + 1 - h = 5-(1+y1)
    return bdf_glyph_lines(cp, x1 - x0 + 1, y1 - y0 + 1, x0, 4 - y1, pix, 4, "ASCII")


def compress_x(rows, w=8, out_w=4):
    """把 8 列设计横压到 4 列（按列 OR）——用于符号/制表符/方块，保证 4px 单元内对齐。"""
    out = []
    for row in rows:
        r = []
        for c in range(out_w):
            x0 = c * w // out_w
            x1 = max(x0 + 1, (c + 1) * w // out_w)
            r.append(1 if any(row[x] for x in range(x0, min(x1, w))) else 0)
        out.append(r)
    return out


def to_7rows(rows, w, merge_row7):
    """宽字形：合并第 6/7 行（8 行设计塞进 7 行单元，保留底部墨迹）；
    单宽字形：保持原版位图，直接取前 7 行，不做纵向压缩/拉伸。"""
    if not merge_row7:
        out = [[rows[y][x] for x in range(w)] for y in range(min(7, len(rows)))]
        while len(out) < 7:
            out.append([0] * w)
        return out
    out = [[rows[y][x] for x in range(w)] for y in range(min(6, len(rows)))]
    while len(out) < 6:
        out.append([0] * w)
    merged = [0] * w
    for y in range(6, len(rows)):
        for x in range(w):
            merged[x] |= rows[y][x]
    out.append(merged)
    return out


def unicode_glyph(cp, src, cells):
    """主/兜底 BDF 字形（8 行，行 0..7 直接对到单元行 0..7）→ 裁剪后 BDF 字形。

    窄单元（4px）里墨迹超过 4 列的字形先横压到 4 列（按列 OR），
    避免跨格重叠；框线/方块/箭头/数学符号靠这个才能 4px 对齐。
    """
    w, h, xo, yo, rows = src
    rows = to_7rows(rows, w, cells == 2)
    h = 7
    # 不再横向压缩：保留原版字形墨迹（可溢出到右邻格），与 6x10 字体一致。
    # 这样希腊/西里尔/◇/™/↑ 等符号不会被压扁；框线/方块由 boxdraw 程序化生成。
    bb = ink_bbox(rows, w, h)
    if bb is None:
        # 空白（空格/全角空格等）：保留一个 1x1 空字形，保证码点存在
        return bdf_glyph_lines(cp, 1, 1, 0, 0, [[0]], 8 if cells == 2 else 4, "blank")
    x0, x1, y0, y1 = bb
    pix = crop_rows(rows, w, x0, x1, y0, y1)
    dst_y = 5 - y1                     # 源 BDF 顶行 y=5 ↔ 单元 row 0
    return bdf_glyph_lines(cp, x1 - x0 + 1, y1 - y0 + 1, x0, dst_y, pix,
                           8 if cells == 2 else 4, "Unicode")


# 圆点族强制单宽（tmux/glibc wcwidth en_US.UTF-8 实测均前进 1 列；
# ◉ EAW=N、◎ EAW=A，A 在本终端按窄处理）。与 gen_term_font.py 的 WIDTH1 保持一致。
WIDTH1 = {0x25CF, 0x25CB, 0x2022, 0x2219, 0x22C5, 0x00B7, 0x25C9, 0x25CE}


def eaw_cells(cp):
    if cp in WIDTH1:
        return 1
    return 2 if unicodedata.east_asian_width(chr(cp)) in ("W", "F") else 1


def build_merged(ascii_g, chill, fallbacks):
    glyphs = {}
    # ASCII：0x00-0x7F 全部来自手绘图
    for cp in range(0x80):
        glyphs[cp] = ascii_glyph(cp, ascii_g[cp])
    stats = {"ascii": 128, "primary": 0, "fallback": 0, "wide": 0, "narrow": 0}
    # 非 ASCII：主字体优先，兜底补缺
    srcs = [("primary", chill)] + [("fallback", fb) for fb in fallbacks]
    for kind, table in srcs:
        for cp in sorted(table):
            if cp < 0x80 or cp in glyphs:
                continue
            cells = eaw_cells(cp)
            glyphs[cp] = unicode_glyph(cp, table[cp], cells)
            stats[kind] += 1
            stats["wide" if cells == 2 else "narrow"] += 1

    # 制表符 / 方块元素：按 4x7 单元用 boxdraw 重绘（单线/粗线程序化，
    # 双线/虚线/圆角/斜线重采样；方块元素填满单元），覆盖上游字形。
    box_src = {}
    for cp in range(0x2500, 0x25A0):
        for table in (chill, *fallbacks):
            e = table.get(cp)
            if e:
                w, h, xo, yo, rows = e
                box_src[cp] = (w, h, rows)
                break
    boxed = boxdraw.box_glyphs(CELL_W, CELL_H, box_src)
    for cp, pts in boxed.items():
        rows = [[0] * CELL_W for _ in range(CELL_H)]
        for x, y in pts:
            if 0 <= x < CELL_W and 0 <= y < CELL_H:
                rows[y][x] = 1
        glyphs[cp] = bdf_glyph_lines(cp, CELL_W, CELL_H, 0, -1, rows, CELL_W, "box")

    # 4x7 的圆点/圆环手工设计：原版 7x7 圆点放进 4px 单元会变成又大又椭的块。
    # 圆点在 row 1..4（4 行），空心圆同尺寸；小圆点 2x2。
    DOTS = {
        0x25CF: ["....", ".##.", "####", "####", ".##.", "....", "...."],  # ●
        0x25CB: ["....", ".##.", "#..#", "#..#", ".##.", "....", "...."],  # ○
        0x2022: ["....", "....", ".##.", ".##.", "....", "....", "...."],  # • 保持
        0x2219: ["....", "....", "....", ".#..", "....", "....", "...."],  # ∙
        0x22C5: ["....", "....", "....", ".#..", "....", "....", "...."],  # ⋅
        0x00B7: ["....", "....", "....", ".#..", "....", "....", "...."],  # ·
        0x2027: ["....", "....", "....", ".#..", "....", "....", "...."],  # ‧
        # ◉/◎：8px 宽墨迹，但按单字符宽度绘制（cells=1，向右溢出 4px）
        0x25C9: ["..###...", ".#...#..", "#.###.#.", "#.###.#.",
                 "#.###.#.", ".#...#..", "..###..."],                       # ◉
        0x25CE: ["..###...", ".#...#..", "#..#..#.", "#.#.#.#.",
                 "#..#..#.", ".#...#..", "..###..."],                       # ◎
    }
    for cp, pat in DOTS.items():
        w = len(pat[0])
        glyphs[cp] = bdf_glyph_lines(
            cp, w, CELL_H, 0, -1,
            [[1 if ch == '#' else 0 for ch in row] for row in pat], CELL_W, "dot")
    return glyphs, stats


def run_bdfconv(bdf_path, c_path):
    cmd = [BDFCONV, "-f", "1", "-b", "0", "-m", "0-65535",
           "-o", c_path, "-n", "cardterm_small", bdf_path]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("bdfconv 失败:\n" + r.stdout + r.stderr)
    if r.stderr.strip():
        print("bdfconv stderr:", r.stderr.strip()[:500])


def decoded_glyphs(c_path):
    fd = FontData(load_u8g2_c(c_path))
    table = {}
    table.update(fd.ascii_glyphs())
    table.update(fd.unicode_glyphs())
    out = {}
    for cp, off in table.items():
        out[cp] = fd.glyph(off)      # (w, h, x, y, d, rows)
    return out, fd


def build_table(decoded):
    glyphs = []
    bits = []
    bad = []
    for cp in sorted(decoded):
        w, h, x, y, d, rows = decoded[cp]
        cells = 2 if cp >= 0x80 and eaw_cells(cp) == 2 else 1
        first_row = BASE_ROW - (y + h - 1)
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
        if ink:
            box_w = max(sx for sx, _ in ink) + 1
        else:
            box_w = CELL_W * cells
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
    L.append("// term_font_small.h — 终端专用等宽字体 4x7（小字体，自动生成, 勿手改）")
    L.append("// 生成器: helloworld_customfont/tools/build_small_font.py")
    L.append("// ASCII : cardterm/extra/cardterm_font.png 手绘 4x6（4px 顶格）")
    L.append("// Unicode: ChillBitmap 7px（OFL-1.1, 8px 点阵）+ 兜底字体")
    L.append("// 单元 4x7 px; 全角=8px 单元, 半角=4px; 基线 row 6（ASCII 占 row 1..6）")
    L.append("#pragma once")
    L.append('#include "termfont_core.h"')
    L.append("")
    L.append("namespace termfont {")
    L.append("")
    L.append("// 每字形 7 行, 每行 uint16: 位 c = 列 c（LSB 为最左列）")
    L.append("static const uint16_t bits_4x7[] = {")
    per = 16
    for i in range(0, len(bits), per):
        L.append("  " + ", ".join("0x%04X" % v for v in bits[i:i + per]) + ",")
    L.append("};")
    L.append("")
    L.append("static const TermGlyph glyphs_4x7[] = {")
    for idx, (cp, cells, w, rb) in enumerate(glyphs):
        L.append("  {0x%04X, %d, %d, %d}," % (cp, cells, w, idx * ROWS))
    L.append("};")
    L.append("")
    L.append("static const FontTable font_4x7 = {")
    L.append("  glyphs_4x7, %d, bits_4x7, %d, %d, %d" % (len(glyphs), ROWS, CELL_W, CELL_H))
    L.append("};")
    L.append("")
    L.append("}  // namespace termfont")
    open(path, "w", encoding="utf-8").write("\n".join(L) + "\n")
    print("wrote", path, "-", len(glyphs), "glyphs,", len(bits), "u16 bits")


def validate_ascii(decoded, ascii_g):
    bad = []
    for cp, rows4 in ascii_g.items():
        if cp not in decoded:
            bad.append((cp, "missing"))
            continue
        w, h, x, y, d, rows = decoded[cp]
        if not h:
            # 空字形（空格）允许
            if any(rows4):
                bad.append((cp, "empty"))
            continue
        first_row = BASE_ROW - (y + h - 1)
        canvas = [[0] * 4 for _ in range(6)]
        for ry, row in enumerate(rows):
            for rx, v in enumerate(row):
                if not v:
                    continue
                cr = first_row + ry - 1          # 单元行 1..6 → 画布行 0..5
                cc = x + rx
                if 0 <= cr < 6 and 0 <= cc < 4:
                    canvas[cr][cc] = 1
        want = [[1 if rows4[ry] & (0x8 >> cc) else 0 for cc in range(4)] for ry in range(6)]
        if canvas != want:
            bad.append((cp, "mismatch", canvas, want))
    return bad


def main():
    ascii_g = read_ascii_png(PNG)
    json.dump({str(k): v for k, v in ascii_g.items()}, open(OUT_JSON, "w"), indent=0)

    chill = parse_bdf(CHILL)
    fallbacks = []
    if os.path.isdir(FALLBACK_DIR):
        for name in sorted(os.listdir(FALLBACK_DIR)):
            if name.endswith(".bdf"):
                fallbacks.append(parse_bdf(os.path.join(FALLBACK_DIR, name)))
    print(f"sources: primary={len(chill)} fallbacks={[len(fb) for fb in fallbacks]}")

    glyphs, stats = build_merged(ascii_g, chill, fallbacks)

    lines = [
        "STARTFONT 2.1",
        "FONT -CardTerm-CardTermSmall-Medium-R-Normal--8-80-72-72-C-80-ISO10646-1",
        "SIZE 8 72 72",
        "FONTBOUNDINGBOX 8 7 0 -1",
        "STARTPROPERTIES 8",
        'FAMILY_NAME "CardTerm Small 4x8"',
        'COPYRIGHT "ASCII: hand-drawn by cardterm author. Unicode: ChillBitmap (c) 2020-2022 warren2060 / ChillType, OFL-1.1. Fallback: see extra_assets/fonts/fallback8px/."',
        "FONT_ASCENT 6",
        "FONT_DESCENT 1",
        "DEFAULT_CHAR 32",
        'CHARSET_REGISTRY "ISO10646"',
        "ENDPROPERTIES",
        f"CHARS {len(glyphs)}",
    ]
    for cp in sorted(glyphs):
        lines.extend(glyphs[cp])
    lines.append("ENDFONT")
    open(OUT_BDF, "w", encoding="utf-8").write("\n".join(lines) + "\n")
    print("wrote", OUT_BDF, "-", len(glyphs), "glyphs,", os.path.getsize(OUT_BDF), "bytes")
    print("stats:", stats)

    run_bdfconv(OUT_BDF, OUT_C)
    decoded, fd = decoded_glyphs(OUT_C)
    print("u8g2 decoded:", len(decoded), "glyphs; header start_a=", fd.start_a,
          "start_unicode=", hex(fd.start_unicode))

    bad = validate_ascii(decoded, ascii_g)
    if bad:
        print("ASCII 校验失败:", len(bad))
        for b in bad[:10]:
            print(" ", b)
        sys.exit(1)
    print("ASCII 校验通过（128 个码点位置/形状与 PNG 一致）")

    table, bits, overflow = build_table(decoded)
    if overflow:
        print("越界字形:", overflow[:10])
        sys.exit(1)
    emit_header(table, bits, OUT_H)


if __name__ == "__main__":
    main()
