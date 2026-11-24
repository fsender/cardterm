# 字体：两套内置点阵字体表

cardterm 不依赖系统字体，两套等宽点阵表直接编译进二进制（`src/term_font.h`、
`src/term_font_small.h`，均为生成物，勿手改）。运行时由 `term::Options` 选择
`termfont::font_6x10` 或 `termfont::font_4x7`（`terminal.cpp` 的 `apply_font()`）。

## 字体表对照

| 项 | 6×10（默认） | 4×7（小字体） |
|---|---|---|
| 表 | `termfont::font_6x10` | `termfont::font_4x7` |
| 单元 | 6×10 px | 4×7 px |
| 网格（320×170） | 53×17 | 80×24 |
| 字形数 | 26,162 | 29,080 |
| 每字形行数 | 10（文本占 0..8，制表符占 0..9） | 7 |
| ASCII | glcdfont 5×7（8 行含降部） | 手绘 4×6（4px 顶格） |
| Unicode | BoutiqueBitmap9x9 1.93 | ChillBitmap 7px（真实 8px 点阵） |
| 兜底 | Fusion Pixel 10px Monospaced | Fusion Pixel 8px Monospaced |
| 位图池 | 261,620 u16 | 203,560 u16 |
| 单元选择 | `font_size=6`、`font_width=6/font_height=10` | `font_size=4`，或 `font_width<6 && font_height<10` |

## 布局约定

`termfont::TermGlyph { cp, cells, w, bits }`，每字形固定 `rows` 行 u16（位 c = 列 c）。
`cells` 由 East Asian Width 决定（W/F=2，其余含歧义 A=1）。

### 6×10

- 包围盒 10 行；Boutique 基线 `base_row=6`，Fusion 10px `base_row=7`。
- ASCII：glcdfont 5×7 的 8 行放在包围盒第 1..8 行（第 0 行留白）。
- 文本字形占 row 0..8（row 9 为行间留白）；**制表符/方块占满 row 0..9**，
  相邻行竖线因此连续。
- 双宽字形放 11×9 包围盒居中；单宽字形原样绘制（可溢出到右邻格，光标只前进 6px）。

### 4×7

- 包围盒 7 行；单元 row 0..6，基线在 row 5/6 之间（`FONT_ASCENT 6`）。
- ASCII：`cardterm/extra/cardterm_font.png` 的 4×6 手绘点阵，**原样放在 row 1..6、
  col 0..3（4px 顶格）**，不缩放、不居中；光标前进 4px。
- Unicode（寒蝉点阵体）：8 行设计（基线 row 6）→ 7 行：row 0..5 原样，
  row 6 = OR(源 row 6, row 7)；墨迹 7×7（row 0..6），双宽字符单元 8px。
- 窄字形（EAW 非 W/F）墨迹超过 4 列时，先把 8 列**按列 OR 横压到 4 列**再裁剪：
  框线/方块/箭头/数学符号因此在 4px 单元内对齐；拉丁/希腊/西里尔字母同样处理。
- **所有字形墨迹高度 ≤ 7px**（含制表符，单元只有 7 行）。
- 缺字：Fusion Pixel 8px（Hangul/盲文/制表符/几何图形等）；仍缺的码点运行时画
  `单元-1` 的空心 tofu（3×6 / 7×6）。
- 手绘图 0x00–0x1F 也收录（控制字符不会被打印，仅保持图表完整）。

## 制表符 / 方块元素（两套字体统一重绘）

上游字体（BoutiqueBitmap9x9 / ChillBitmap / Fusion Pixel）的制表符按自己的
9×9 / 8×8 网格设计，直接塞进 6px / 4px 单元会溢出，且竖线高度只有 9/8 行，
TUI 边框每行会断 1px。因此 `tools/boxdraw.py` 对 U+2500–U+259F **按目标单元重绘**：

- 单线 / 粗线（含 `LIGHT`/`HEAVY` 混排）：解析 Unicode 名称得到 up/right/down/left
  各线段，按单元几何画 1px / 2px 线；
- 双线（`DOUBLE`，含 single/double 混排）：两条平行 1px 线（列/行取 `vx±1`、`hy±1`）；
- 虚线 / 圆角 / 斜线：按 dash/arc/diagonal 规则生成；
- 方块元素 U+2580–259F：按单元尺寸程序化生成（█ 填满单元才能无缝拼接，
  ░▒▓ 用有序抖动）。
- **象限/半块**严格按 `W//2` / `H//2` 切分（4×7：上半 3 行、下半 4 行；6×10：5/5），
  ▀+▄、四个象限、16 种 2×2 组合都无缝拼合。`qrencode -t UTF8` 的二维码直接把
  2×2 模块映射到这些字符，因此可以正常显示——实测用 OpenCV 在 6×10 与 4×7
  渲染图上都解码出 "hello"（1x/2x/4x 缩放均通过）。
- **分数块**（▏▎▍▌▋▊▉ / ▁▂▃▄▅▆▇）按单元查表，保证单调且 7/8 不占满整格：
  6×10 宽 `1,2,2,3,4,5,5`、高 `1,2,4,5,6,8,9`；4×7 宽 `1,1,2,2,3,3,3`、
  高 `1,2,3,4,4,5,6`。进度条（如 htop/自定义脚本）的步进更均匀。

竖线在 6×10 中贯穿 row 0..9、在 4×7 中贯穿 row 0..6，因此多行框线连续。

## 生成流程（helloworld_customfont/tools/）

```bash
cd helloworld_customfont

# 6×10：glcdfont + BoutiqueBitmap9x9 + Fusion Pixel 10px → src/term_font.h
python3 tools/gen_term_font.py

# 4×7：PNG 手绘 ASCII + ChillBitmap BDF + Fusion Pixel 8px → src/term_font_small.h
python3 tools/build_small_font.py

# 交叉校验（BDF vs u8g2 解码；4×7 生成器内置 ASCII 逐字校验）
python3 tools/validate_font.py
```

`build_small_font.py` 依次完成：

1. 从 `cardterm/extra/cardterm_font.png` 提取 128 个 4×6 字形（网格 5×7，行 8..13）；
2. 读 `extra_assets/fonts/chillbitmap/ChillBitmap_7px_8px.bdf`（8×8，基线 row 6）；
3. 依次读 `extra_assets/fonts/fallback8px/*.bdf` 补缺（主字体优先）；
4. 用 `boxdraw.py` 重绘制表符/方块；
5. 合并成 `extra_assets/fonts/cardterm_small_4x7.bdf`；
6. `bdfconv -f 1 -b 0 -m 0-65535` → `cardterm_small_4x7_u8g2.c`（**u8g2 字体**，
   >32KB 所以包在 `U8G2_USE_LARGE_FONTS` 里）；
7. 解码 u8g2 数据、逐字校验 ASCII 位置，生成 `src/term_font_small.h`。

源 BDF/TTF 与许可原文在 `extra_assets/fonts/`（不在 git 仓库内）；随 deb 分发的
许可文本已复制到 `cardterm/packaging/fonts/`，安装到
`/usr/share/doc/cardterm/fonts/`。

## 数据来源与许可

| 来源 | 用途 | 许可 |
|---|---|---|
| 手绘 ASCII（cardterm 作者） | 4×7 ASCII | 随 cardterm 项目 |
| glcdfont（LovyanGFX Font0） | 6×10 ASCII | 公版 |
| BoutiqueBitmap9x9 1.93 | 6×10 Unicode | 自定义宽松条款 |
| Fusion Pixel 10px / 8px Monospaced（TakWolf） | 两套的兜底 | SIL OFL-1.1 |
| ChillBitmap 7px（Warren2060 / ChillType） | 4×7 Unicode | SIL OFL-1.1（保留字体名） |

烘焙后的字体表命名为 `CardTerm 6x10` / `CardTerm Small 4x7`，不使用上游保留名。
完整版权与许可原文见 [packaging/fonts/NOTICE.md](../packaging/fonts/NOTICE.md)。
