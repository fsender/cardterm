#!/usr/bin/env python3
"""gen_term_font.py — 生成 cardterm 0.4.0 草案用的终端字体 term_font.h。

输入:
  1. extra_assets/fonts/boutique9x9_u8g2.c  — bdfconv 生成的 u8g2 格式字体
     （仅含非 ASCII 字形; ASCII 由下面的 glcdfont 提供）
  2. LGFX 的 glcdfont.h                     — 经典 5×7 点阵（Font0）ASCII 字形

输出:
  src/term_font.h — 单一等宽终端字体:
    单元 6×10 px:  ASCII → 6px 单元, 5×7 字形置于 5×9 包围盒第 1..7 行
                   非 ASCII → BoutiqueBitmap9x9 字形; 宽度按 wcwidth（EAW W/F）=2 格、其余=1 格;
                   包围盒 11×9（居中）; 单宽=6px 单元, 包围盒 6×9（超宽墨迹水平压缩撑满）
    字形按 Unicode 码点升序排列, 运行时二分查找。

u8g2 字体二进制格式（见 .tmp/u8g2/csrc/u8g2_font.c）:
  头部 23B: glyph_cnt, bbx_mode, bits_per_0, bits_per_1, w/h/x/y/dx 位宽,
            max_w/h/x/y, ascent_A, descent_g, ascent_para, descent_para,
            start_A(2), start_a(2), start_unicode(2)
  ASCII 段:  [enc][size][glyph...]… 以 size=0 结束
  Unicode 段: 4B 查找表 (offset16, last_enc16) + [enc16][size8][glyph...] 块, enc=0 结束
  字形: w=ubits, h=ubits, x=sbits, y=sbits, d=sbits, 然后 RLE:
        循环 { a=ubits(bits_per_0); b=ubits(bits_per_1);
               重复 { 画 a 个 0 像素; 画 b 个 1 像素 } 直到续位=0;
               填满 w*h 结束 } —— 像素流按行展开、自动折行。
"""
import os
import re
import sys
import unicodedata

import boxdraw

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.dirname(HERE)
ROOT = os.path.dirname(PROJ)
PRIMARY_C  = os.path.join(ROOT, "extra_assets", "fonts", "boutique9x9_u8g2.c")
FALLBACK_C = os.path.join(ROOT, "extra_assets", "fonts", "fusion-pixel-10px-u8g2.c")
U8G2_C = PRIMARY_C   # 兼容旧脚本/校验工具
GLCD_H = os.path.join(ROOT, "helloworld_customfont", "lib", "LovyanGFX-1.3.0",
                      "src", "lgfx", "Fonts", "glcdfont.h")
OUT = os.path.join(PROJ, "src", "term_font.h")

CELL_W = 6
CELL_H = 10
# 圆点族强制单宽：真实终端（tmux 3.x）与 glibc wcwidth（en_US.UTF-8 / C.UTF-8）
# 实测 ●○•∙⋅·◉◎ 都只前进 1 列（◉ EAW=N，◎ EAW=A；A 按本终端策略当窄）。
# 显式锁定，避免今后 EAW/locale 变化让字体表与 cp_width 不一致。
WIDTH1 = {0x25CF, 0x25CB, 0x2022, 0x2219, 0x22C5, 0x00B7, 0x25C9, 0x25CE}

BOX_H = 10         # 字形包围盒高度（5×9 / 11×9；制表符用满 10 行）
WIDE_BOX = 11      # 双宽包围盒（CJK 9px 墨迹居中）
SINGLE_BOX = 6     # 单宽包围盒 = 整个单元宽（块/框线撑满，避免缝隙）


# ── u8g2 C 文件 → 裸字节（C 字符串可能被拆成多段 "…" "…"）────────────
def load_u8g2_c(path):
    src = open(path, encoding="utf-8").read()
    # 数据串在 "= " 之后（U8G2_FONT_SECTION("名称") 里也有引号, 需跳过）
    i = src.index("=")
    while src[i] != '"':
        i += 1
    out = bytearray()
    while True:
        assert src[i] == '"'
        i += 1
        while True:
            ch = src[i]
            if ch == "\\":
                i += 1
                if src[i] in "01234567":
                    digits = ""
                    while len(digits) < 3 and src[i] in "01234567":
                        digits += src[i]
                        i += 1
                    out.append(int(digits, 8))
                else:
                    esc = {"n": 10, "t": 9, "r": 13, '"': 34, "\\": 92, "?": 63}
                    out.append(esc.get(src[i], ord(src[i])))
                    i += 1
                continue
            if ch == '"':
                i += 1
                break
            out.append(ord(ch))
            i += 1
        # 段结束: ';' 表示整个字符串结束, 否则跳过空白进入下一段
        if src[i] == ";":
            break
        while src[i] in " \t\r\n":
            i += 1
    return bytes(out)


class BitReader:
    def __init__(self, data, bitpos=0):
        self.data = data
        self.pos = bitpos

    def u(self, n):
        # u8g2/bdfconv 位流为 LSB 优先（字节内低位先出）
        v = 0
        for k in range(n):
            byte = self.data[self.pos >> 3]
            v |= ((byte >> (self.pos & 7)) & 1) << k
            self.pos += 1
        return v

    def s(self, n):
        u = self.u(n)
        return u - (1 << (n - 1))


class FontData:
    def __init__(self, data):
        self.data = data
        h = data
        assert h[0] > 0, "empty u8g2 font"
        (self.glyph_cnt, self.bbx_mode, self.b0, self.b1,
         self.bw, self.bh, self.bx, self.by, self.bdx,
         self.max_w, self.max_h, self.max_x, self.max_y,
         self.ascent_a, self.descent_g, self.ascent_para, self.descent_para) = h[:17]
        self.start_a = (h[17] << 8) | h[18]
        self.start_lower_a = (h[19] << 8) | h[20]
        self.start_unicode = (h[21] << 8) | h[22]
        self.base_row = 6          # 包围盒行 = base_row - 字形行；各来源字体按自身网格设置

    # 逐字形解码 —— 返回 (w, h, x, y, d, rows[list[list[int]]])
    def glyph(self, data_off):
        r = BitReader(self.data, data_off * 8)
        w = r.u(self.bw)
        h = r.u(self.bh)
        x = r.s(self.bx)
        y = r.s(self.by)
        d = r.s(self.bdx)
        if w == 0 or h == 0:
            return w, h, x, y, d, []
        pixels = bytearray(w * h)
        pos = 0
        total = w * h
        while pos < total:
            a = r.u(self.b0)
            b = r.u(self.b1)
            while True:
                pos = min(pos + a, total)
                end = min(pos + b, total)
                for k in range(pos, end):
                    pixels[k] = 1
                pos = end
                if not r.u(1):
                    break
        rows = [pixels[i * w:(i + 1) * w] for i in range(h)]
        return w, h, x, y, d, rows

    def ascii_glyphs(self):
        """返回 {cp: data_offset}（ASCII 段, 仅 0x80-0xFF 有用; 条目 [enc][size][data]）。"""
        out = {}
        off = 23
        while off + 3 <= len(self.data):
            enc = self.data[off]
            size = self.data[off + 1]
            if size == 0:
                break
            out[enc] = off + 2
            off += size
        return out

    def unicode_glyphs(self):
        """返回 {cp: data_offset}（BMP 非 ASCII 段）。"""
        out = {}
        base = 23 + self.start_unicode
        lut = []
        pos = base
        while True:
            off = (self.data[pos] << 8) | self.data[pos + 1]
            enc = (self.data[pos + 2] << 8) | self.data[pos + 3]
            lut.append((off, enc))
            pos += 4
            if enc == 0xFFFF:
                break
        # 字形块在查找表之后连续排列: 从表尾开始线性扫描直到 enc=0 结束标记
        off = base + 4 * len(lut)
        while True:
            if off + 3 > len(self.data):
                break
            enc = (self.data[off] << 8) | self.data[off + 1]
            size = self.data[off + 2]
            if size == 0:
                break
            if enc:
                out[enc] = off + 3
            off += size
        return out


# ── glcdfont（Font0）────────────────────────────────────────────────
def load_glcdfont(path):
    src = open(path, encoding="utf-8", errors="ignore").read()
    m = re.search(r"font\[\][^=]*=\s*\{(.*?)\};", src, re.S)
    if not m:
        sys.exit("glcdfont[] not found")
    vals = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(1))]
    if len(vals) < 256 * 5:
        sys.exit(f"glcdfont: expected 1280 bytes, got {len(vals)}")
    return vals


def build_sources():
    """主字体 + 兜底字体（按优先级）；各来源带自己的基线行。"""
    primary = FontData(load_u8g2_c(PRIMARY_C))
    primary.base_row = 6                 # BoutiqueBitmap9x9: CJK 墨迹行 -2..6
    sources = [primary]
    if os.path.exists(FALLBACK_C):
        fb = FontData(load_u8g2_c(FALLBACK_C))
        fb.base_row = 7                  # Fusion Pixel 10px: CJK 墨迹行 -1..7
        sources.append(fb)
    return sources


def build_glyphs(fonts, glcd):
    """返回 ([(cp, cells, box_w, ink[(col,row)…])…], stats)（按码点升序）。

    fonts: FontData 列表（优先级从高到低）；同一码点先到先得。
    """
    if isinstance(fonts, FontData):      # 兼容旧调用
        fonts = [fonts]
    merged = {}
    for fd in fonts:
        for cp, off in fd.ascii_glyphs().items():
            merged.setdefault(cp, (fd, off))
        for cp, off in fd.unicode_glyphs().items():
            merged.setdefault(cp, (fd, off))
    # 生成循环按 Codepoint 取源字体
    uni = {cp: None for cp in merged}

    glyphs = []          # (cp, cells, width, ink[(col,row)…])
    stats = {"ascii": 0, "wide": 0, "narrow": 0, "empty": 0}

    def put(cp, cells, w, ink):
        glyphs.append((cp, cells, w, ink))

    # ASCII：glcdfont 5×7 字形实际为 8 行（bit0=最上行, bit7=降部行,
    # 如 y/g/p/q/j 的尾巴）。整 8 行放在 5×9 包围盒第 1..8 行（第 0 行留白）。
    for cp in range(0x20, 0x7F):
        cols = glcd[cp * 5:cp * 5 + 5]
        ink = []
        for cx in range(5):
            byte = cols[cx]
            for ry in range(8):
                if byte & (1 << ry):
                    ink.append((cx, ry + 1))
        put(cp, 1, 5, ink)
        stats["ascii"] += 1

    # 非 ASCII：BoutiqueBitmap9x9（设计网格：行 6..-2 ↔ 包围盒 0..8, 基线在行 7 上缘）
    # 宽度按终端语义（foot/xterm 等）：East Asian Width W/F → 2 格，其余 1 格
    # （歧义 A 按窄，与 foot 默认/本终端宽度策略一致）。
    for cp in sorted(uni):
        src_font, src_off = merged[cp]
        w, h, x, y, d, rows = src_font.glyph(src_off)
        if d <= 0:
            d = 1
        cells = 1 if cp in WIDTH1 else (2 if unicodedata.east_asian_width(chr(cp)) in ("W", "F") else 1)
        box_w = WIDE_BOX if cells == 2 else SINGLE_BOX
        stats["wide" if cells == 2 else "narrow"] += 1
        if w == 0 or h == 0:
            put(cp, cells, box_w, [])
            stats["empty"] += 1
            continue
        ink = []
        # 垂直：按来源字体的基线行映射（包围盒行 = base_row - 字形行）
        first_row = src_font.base_row - (y + h - 1)
        if cells == 2:
            # 设计跨度拉进 11 宽包围盒并居中（CJK 9px 墨迹左右各留 1px）
            cx0 = x
            if cx0 < 0:
                cx0 = 0
            if w > WIDE_BOX:                     # 超宽墨迹：左右压缩到 11px
                for ry in range(h):
                    sry = first_row + ry
                    if sry < 0 or sry >= BOX_H:
                        continue
                    for nx in range(WIDE_BOX):
                        x0 = nx * w // WIDE_BOX
                        x1 = max(x0 + 1, (nx + 1) * w // WIDE_BOX)
                        if any(rows[ry][rx] for rx in range(x0, min(x1, w))):
                            ink.append((cx0 + nx, sry))
            else:
                rem = WIDE_BOX - (cx0 + w)
                if rem > 0:
                    cx0 += rem // 2
                for ry in range(h):
                    sry = first_row + ry
                    if sry < 0 or sry >= BOX_H:
                        continue
                    brow = rows[ry]
                    for rx in range(w):
                        if brow[rx]:
                            sx = cx0 + rx
                            if 0 <= sx < box_w:
                                ink.append((sx, sry))
        else:
            # 单宽：字形不做任何改动（不压缩、不缩放），按设计位置原样绘制；
            # 墨迹可超过 6px（溢出到右侧相邻格），光标仍只前进 6px（cells=1）。
            cx0 = x if x >= 0 else 0
            box_w = cx0 + w                      # 实际墨迹跨度（u16 行每行 16 列上限）
            if box_w > 16:                       # 极少数 PUA/超宽组合字形：截到 16 列
                w = 16 - cx0
                box_w = cx0 + w
            for ry in range(h):
                sry = first_row + ry
                if sry < 0 or sry >= BOX_H:
                    continue
                brow = rows[ry]
                for rx in range(w):
                    if brow[rx]:
                        ink.append((cx0 + rx, sry))
        put(cp, cells, box_w, ink)

    # 手工圆点覆盖：◎ U+25CE 为 6x10 单宽、右侧顶格（墨迹占满 0..5 列，不预留空列）。
    CUSTOM = {
        # 6x10 手工绘制（单字符宽度）；◉/◎ 墨迹 8px，按单宽绘制（向右溢出 2px）
        0x25CB: [  # ○
            "......", "......", "..##..", ".#..#.", "#....#",
            "#....#", ".#..#.", "..##..", "......", "......",
        ],
        0x25CF: [  # ●
            "......", "......", "..##..", ".####.", "######",
            "######", ".####.", "..##..", "......", "......",
        ],
        0x25C9: [  # ◉
            "........", "..####..", ".#....#.", "#..##..#", "#.####.#",
            "#.####.#", "#..##..#", ".#....#.", "..####..", "........",
        ],
        0x25CE: [  # ◎
            "........", "..####..", ".#....#.", "#..##..#", "#.#..#.#",
            "#.#..#.#", "#..##..#", ".#....#.", "..####..", "........",
        ],
    }
    have = {g[0] for g in glyphs}
    if any(cp in have for cp in CUSTOM):
        glyphs = [g for g in glyphs if g[0] not in CUSTOM]
    for cp, rows in CUSTOM.items():
        ink = [(x, y) for y, row in enumerate(rows) for x, ch in enumerate(row) if ch == '#']
        put(cp, 1, len(rows[0]), ink)

    # 锁定的单宽圆点若源字体缺字，用 ·（U+00B7）的字形兜底（如 U+22C5 ⋅）。
    have = {g[0] for g in glyphs}
    fallback = []
    for cp in sorted(WIDTH1):
        if cp in have:
            continue
        ref = 0x00B7 if cp != 0x00B7 else 0x2219
        for (rcp, _rcells, rw, rink) in glyphs:
            if rcp == ref:
                fallback.append((cp, 1, rw, list(rink)))
                break
    for item in fallback:
        put(*item)

    # 去重 + 按码点排序
    seen = set()
    final = []
    for cp, cells, w, ink in glyphs:
        if cp in seen:
            continue
        seen.add(cp)
        final.append((cp, cells, w, ink))
    final.sort(key=lambda g: g[0])
    return final, stats


def apply_boxdraw(glyphs):
    """用 boxdraw 重新生成 U+2500–U+259F：单线/粗线按 6x10 重绘、双线等重采样，
    方块元素程序化生成。保证制表符在 6px 单元内对齐、竖线贯穿 10 行。"""
    src = {}
    for cp, cells, w, ink in glyphs:
        if not (0x2500 <= cp <= 0x259F) or not ink:
            continue
        h = max(y for _, y in ink) + 1
        ww = max(x for x, _ in ink) + 1
        rows = [[0] * ww for _ in range(h)]
        for x, y in ink:
            rows[y][x] = 1
        src[cp] = (ww, h, rows)
    boxed = boxdraw.box_glyphs(CELL_W, BOX_H, src)
    out = []
    for cp, cells, w, ink in glyphs:
        if cp in boxed:
            out.append((cp, 1, CELL_W, boxed[cp]))
        else:
            out.append((cp, cells, w, ink))
    return out


def main():
    sources = build_sources()
    glcd = load_glcdfont(GLCD_H)
    final, stats = build_glyphs(sources, glcd)
    final = apply_boxdraw(final)

    # ── 输出 C 头文件 ──
    L = []
    L.append("// term_font.h — 终端专用等宽字体 6x10（自动生成, 勿手改）")
    L.append("// 生成器: helloworld_customfont/tools/gen_term_font.py")
    L.append("// 来源: glcdfont (ASCII) + BoutiqueBitmap9x9 1.93 (OFL-1.1, 其他字形)")
    L.append("// 单元 6x10 px; 单宽包围盒 6x9, 双宽包围盒 11x9; 全角=12px 单元, 半角=6px")
    L.append("#pragma once")
    L.append('#include "termfont_core.h"')
    L.append("")
    L.append("namespace termfont {")
    L.append("")
    L.append("// 每字形 %d 行, 每行 uint16: 位 c = 列 c（LSB 为最左列）" % BOX_H)
    bits = []
    for (cp, cells, w, ink) in final:
        rows = [0] * BOX_H
        for (sx, sry) in ink:
            rows[sry] |= 1 << sx
        bits.extend(rows)
    L.append("static const uint16_t bits_6x10[] = {")
    per = 16
    for i in range(0, len(bits), per):
        chunk = bits[i:i + per]
        L.append("  " + ", ".join("0x%04X" % v for v in chunk) + ",")
    L.append("};")
    L.append("")
    L.append("static const TermGlyph glyphs_6x10[] = {")
    for idx, (cp, cells, w, ink) in enumerate(final):
        L.append("  {0x%04X, %d, %d, %d}," % (cp, cells, w, idx * BOX_H))
    L.append("};")
    L.append("")
    L.append("static const FontTable font_6x10 = {")
    L.append("  glyphs_6x10, %d, bits_6x10, %d, %d, %d" % (len(final), BOX_H, CELL_W, CELL_H))
    L.append("};")
    L.append("")
    L.append("}  // namespace termfont")

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    open(OUT, "w", encoding="utf-8").write("\n".join(L) + "\n")
    print("wrote", OUT, "-", len(final), "glyphs", stats)
    print("bits:", len(bits), "uint16 =", len(bits) * 2, "bytes")


if __name__ == "__main__":
    main()
