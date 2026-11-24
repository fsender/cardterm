#!/usr/bin/env python3
"""boxdraw.py — 制表符 / 方块元素 的等宽点阵生成（cardterm 6x10 与 4x7 字体共用）。

背景：终端里的 U+2500–U+257F 必须**在单元内对齐**才能拼出连续的 TUI 边框。
上游字体（BoutiqueBitmap9x9 / ChillBitmap / Fusion Pixel）的制表符是按自己的
9x9 / 8x8 网格设计的，直接塞进 6px / 4px 单元会溢出或每行断 1px。这里改为：

  - 单线 / 粗线（含 light/heavy 混排）：按 Unicode 名称解析出各方向线段，
    用目标单元的几何参数**重新绘制**（单线 1px，粗线 2px）；
  - 双线 / 虚线 / 圆角 / 斜线：把上游字形重采样到目标单元（宽度按列 OR，
    高度按目标行数映射），保证不越界、竖线贯穿整个单元；
  - 方块元素 U+2580–259F：按单元尺寸程序化生成（█ 必须填满单元才能无缝拼接）。

入口：
    glyphs = box_glyphs(W, H, source)   # source: {cp: (src_w, src_h, rows)}
    #   → {cp: [(x, y), ...]}，坐标在目标单元内（0..W-1, 0..H-1）
"""
import unicodedata

BLOCK_FIRST, BLOCK_LAST = 0x2580, 0x259F

# 分数块的每格列/行数（1/8..7/8 七个档位）。按目标单元手工调过：
# 宽度保证 7/8 不占满整格；高度在 10 行时让步进尽量均匀（1,2,4,5,6,8,9）。
_FRAC_W = {4: (1, 1, 2, 2, 3, 3, 3), 6: (1, 2, 2, 3, 4, 5, 5)}
_FRAC_H = {7: (1, 2, 3, 4, 4, 5, 6), 10: (1, 2, 4, 5, 6, 8, 9)}

_BASIC = {
    "LIGHT": "light", "HEAVY": "heavy", "DOUBLE": "double", "SINGLE": "light",
}
_DIRS = {
    "HORIZONTAL": ("left", "right"),
    "VERTICAL": ("up", "down"),
    "LEFT": ("left",), "RIGHT": ("right",), "UP": ("up",), "DOWN": ("down",),
}


def parse_box_name(cp):
    """解析 U+2500–U+257F 的名称 → (segments, flags)。

    segments: {'left'|'right'|'up'|'down': 'light'|'heavy'|'double'}
    flags: {'dash','arc','diag'}
    """
    name = unicodedata.name(chr(cp))
    if not name.startswith("BOX DRAWINGS "):
        return None
    rest = name[len("BOX DRAWINGS "):]
    seg = {}
    flags = set()
    if "DIAGONAL" in rest:
        flags.add("diag")
        return seg, flags
    if "ARC" in rest:
        flags.add("arc")
    if "DASH" in rest:
        flags.add("dash")
    prev = None
    for part in rest.split(" AND "):
        toks = part.split()
        style = None
        for t in toks:
            if t in _BASIC:
                style = _BASIC[t]
                break
        dirs = []
        for t in toks:
            if t in _DIRS:
                dirs.extend(_DIRS[t])
        if style is None:
            style = prev or "light"
        prev = style
        for d in dirs:
            seg[d] = style
    return seg, flags


# ── 单线 / 粗线 / 双线：按目标单元重绘 ─────────────────────────────────────
def _geom(W, H):
    vx = W // 2 - 1          # 单线竖线列（4→1, 6→2）
    hy = (H - 1) // 2        # 单线横线行（7→3, 10→4）
    return vx, hy


def _line(pts, cx, cy, d, W, H):
    if d == "up":    pts.update((cx, y) for y in range(0, cy + 1))
    if d == "down":  pts.update((cx, y) for y in range(cy, H))
    if d == "left":  pts.update((x, cy) for x in range(0, cx + 1))
    if d == "right": pts.update((x, cy) for x in range(cx, W))


def _draw_segments(seg, W, H, dashed=False):
    vx, hy = _geom(W, H)
    h_double = any(seg.get(d) == "double" for d in ("left", "right"))
    v_double = any(seg.get(d) == "double" for d in ("up", "down"))
    pts = set()

    def col(cx, y0, y1):
        for y in range(max(0, y0), min(H, y1 + 1)):
            pts.add((cx, y))

    def row(cy, x0, x1):
        for x in range(max(0, x0), min(W, x1 + 1)):
            pts.add((x, cy))

    for d, st in seg.items():
        if st == "light":
            if d == "up":    col(vx, 0, hy + 1 if h_double else hy)
            if d == "down":  col(vx, hy - 1 if h_double else hy, H - 1)
            if d == "left":  row(hy, 0, vx + 1 if v_double else vx)
            if d == "right": row(hy, vx - 1 if v_double else vx, W - 1)
        elif st == "heavy":
            vx1, hy1 = min(vx + 1, W - 1), min(hy + 1, H - 1)
            if d == "up":    col(vx, 0, hy1); col(vx1, 0, hy1)
            if d == "down":  col(vx, hy, H - 1); col(vx1, hy, H - 1)
            if d == "left":  row(hy, 0, vx1); row(hy1, 0, vx1)
            if d == "right": row(hy, vx, W - 1); row(hy1, vx, W - 1)
        elif st == "double":
            if d == "up":    col(vx - 1, 0, hy + 1); col(vx + 1, 0, hy + 1)
            if d == "down":  col(vx - 1, hy - 1, H - 1); col(vx + 1, hy - 1, H - 1)
            if d == "left":  row(hy - 1, 0, vx + 1); row(hy + 1, 0, vx + 1)
            if d == "right": row(hy - 1, vx - 1, W - 1); row(hy + 1, vx - 1, W - 1)
    if dashed:
        # 简化虚线：横线每 3 列空 1 列，竖线每 3 行空 1 行（双线/四线共用）
        pts = {(x, y) for (x, y) in pts if not (x % 3 == 2 and not _has_vert(seg, x))}
        pts = {(x, y) for (x, y) in pts if not (y % 3 == 2)}
    return pts


def _has_vert(seg, x):
    return False


def _draw_diagonal(cp, W, H):
    pts = set()

    def diag(main):
        for i in range(max(W, H)):
            x = i * (W - 1) // max(1, H - 1) if False else None
        # 逐行取列：\ 从左上到右下，/ 从右上到左下
        for y in range(H):
            x = round(y * (W - 1) / max(1, H - 1))
            if main:
                pts.add((x, y))
            else:
                pts.add((W - 1 - x, y))

    if cp == 0x2571:   # ╱
        diag(False)
    elif cp == 0x2572: # ╲
        diag(True)
    else:              # ╳
        diag(True); diag(False)
    return pts


# ── 方块元素 U+2580–259F ───────────────────────────────────────────────────
def _blocks(cp, W, H):
    """U+2580–259F。象限/半块严格按 W//2 / H//2 切分（奇数高度时上 3 下 4），
    保证 ▀+▄、四个象限、16 种 2×2 组合都无缝拼合——qrencode -t UTF8 依赖这一点。
    分数块（1/8..7/8）用 half-up 取整，给进度条稳定的单调递增。"""
    pts = set()

    def rect(x0, y0, x1, y1):
        for y in range(max(0, y0), min(H, y1 + 1)):
            for x in range(max(0, x0), min(W, x1 + 1)):
                pts.add((x, y))

    hw, hh = W // 2, H // 2
    LX, RX = (0, hw - 1), (hw, W - 1)      # 左/右半
    TY, BY = (0, hh - 1), (hh, H - 1)      # 上/下半（奇数高度：上 3 下 4）
    FX, FY = (0, W - 1), (0, H - 1)

    def at(xr, yr):
        rect(xr[0], yr[0], xr[1], yr[1])

    def q(v):
        return int(v + 0.5)                # half-up

    def fw(f):                             # 左侧分数宽度（列数；7/8 不占满整格）
        return max(1, min(W - 1, q(W * f)))

    def fh(f):                             # 底部分数高度（行数）
        return max(1, min(H, q(H * f)))

    if cp == 0x2580:                       # ▀
        at(FX, TY)
    elif cp == 0x2584:                     # ▄
        at(FX, BY)
    elif cp == 0x258C:                     # ▌
        at(LX, FY)
    elif cp == 0x2590:                     # ▐
        at(RX, FY)
    elif cp == 0x2596:                     # ▖
        at(LX, BY)
    elif cp == 0x2597:                     # ▗
        at(RX, BY)
    elif cp == 0x2598:                     # ▘
        at(LX, TY)
    elif cp == 0x259D:                     # ▝
        at(RX, TY)
    elif cp == 0x2599:                     # ▙
        at(LX, FY); at(RX, BY)
    elif cp == 0x259A:                     # ▚
        at(LX, TY); at(RX, BY)
    elif cp == 0x259B:                     # ▛
        at(FX, TY); at(LX, BY)
    elif cp == 0x259C:                     # ▜
        at(FX, TY); at(RX, BY)
    elif cp == 0x259E:                     # ▞
        at(RX, TY); at(LX, BY)
    elif cp == 0x259F:                     # ▟
        at(RX, FY); at(LX, BY)
    elif cp == 0x2588:                     # █
        at(FX, FY)
    elif 0x2589 <= cp <= 0x258F:
        # 0x2589=7/8 ... 0x258F=1/8（0x258C 1/2 走上面的 ▌）
        idx = cp - 0x2589
        tbl = _FRAC_W.get(W)
        n = tbl[6 - idx] if tbl else fw((7 - idx) / 8)
        rect(0, 0, n - 1, H - 1)
    elif 0x2581 <= cp <= 0x2587:
        # 0x2581=1/8 ... 0x2587=7/8
        idx = cp - 0x2581
        tbl = _FRAC_H.get(H)
        n = tbl[idx] if tbl else fh((idx + 1) / 8)
        rect(0, H - n, W - 1, H - 1)
    elif cp == 0x2591:                     # ░ 25%
        for y in range(H):
            for x in range(W):
                if (x % 2 == 0) and (y % 2 == 0):
                    pts.add((x, y))
    elif cp == 0x2592:                     # ▒ 50%
        for y in range(H):
            for x in range(W):
                if (x + y) % 2 == 0:
                    pts.add((x, y))
    elif cp == 0x2593:                     # ▓ 75%
        for y in range(H):
            for x in range(W):
                if not ((x % 2 == 0) and (y % 2 == 0)):
                    pts.add((x, y))
    elif cp == 0x2594:                     # ▔ 上 1/8
        rect(0, 0, W - 1, fh(1 / 8) - 1)
    elif cp == 0x2595:                     # ▕ 右 1/8
        n = fw(1 / 8)
        rect(W - n, 0, W - 1, H - 1)
    return pts


def box_glyphs(W, H, source=None):
    """返回 {cp: [(x,y)]}，覆盖 U+2500–U+259F（坐标为单元内 0..W-1 / 0..H-1）。"""
    out = {}
    for cp in range(0x2500, 0x25A0):
        if BLOCK_FIRST <= cp <= BLOCK_LAST:
            out[cp] = sorted(_blocks(cp, W, H))
            continue
        parsed = parse_box_name(cp)
        if parsed is None:
            continue
        seg, flags = parsed
        if "diag" in flags:
            out[cp] = sorted(_draw_diagonal(cp, W, H))
        elif "dash" in flags:
            out[cp] = sorted(_draw_segments(seg, W, H, dashed=True))
        else:
            # 弧线（╭╮╯╰）按普通单线折角绘制
            out[cp] = sorted(_draw_segments(seg, W, H))
    return out


def render_ink(pts, W, H):
    rows = [[0] * W for _ in range(H)]
    for (x, y) in pts:
        if 0 <= x < W and 0 <= y < H:
            rows[y][x] = 1
    return rows
