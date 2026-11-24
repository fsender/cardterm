# cardterm 字体来源与许可（随 deb 安装到 /usr/share/doc/cardterm/fonts/）

cardterm 的两套点阵字体都是**位图烘焙进二进制**的，本目录保留各上游字体的版权与
许可原文（OFL 要求随分发附带）。

| 字体表 | 用途 | 来源 | 许可 |
|---|---|---|---|
| `term_font.h`（6×10） | ASCII 0x20–0x7E | glcdfont（LovyanGFX `Font0`，经典 5×7 点阵，公版） | public domain |
| 〃 | 其他 Unicode 字形 | **BoutiqueBitmap9x9 1.93**（字言字語 Cen-cyun / Luke Liu） | 自定义宽松条款（见 `BoutiqueBitmap9x9/OFL.txt`） |
| 〃 | 兜底（Hangul/盲文/更广 BMP） | **Fusion Pixel 10px Monospaced**（TakWolf） | SIL OFL-1.1（`FusionPixel/`） |
| `term_font_small.h`（4×7） | ASCII 0x00–0x7F | 由 cardterm 作者手工绘制（`cardterm/extra/cardterm_font.png`） | 随 cardterm 项目 |
| 〃 | 宽字形（CJK/假名/全角/符号）+ 部分窄字形 | **ChillBitmap 7px**（真实 8px 点阵，2020-2022 warren2060 / 2024 ChillType） | SIL OFL-1.1，保留字体名 "ChillBitmap" "7px & 16px"（`ChillBitmap/`） |
| 〃 | 兜底（Hangul/盲文/制表符/几何图形等） | **Fusion Pixel 8px Monospaced**（TakWolf，2026.11.10） | SIL OFL-1.1（`FusionPixel/`） |

说明：

- 位图化/子集化属于 OFL 的 “Modified Version”；cardterm 内的字体表不叫
  `ChillBitmap` / `Fusion Pixel`，而是 `CardTerm 6x10` / `CardTerm Small 4x7`。
- `ChillBitmap` 7px 的上游另有自定义宽松条款与美咲フォント版权行，原文见
  `ChillBitmap/7px-License.txt` 与 `ChillBitmap/LICENSE.md`。
- 字体源文件（TTF/BDF）不在 deb 内，仅随 cardterm 源码仓库的生成流程保留；
  上游地址：<https://github.com/Warren2060/ChillBitmap>、
  <https://github.com/TakWolf/fusion-pixel-font>、
  <https://github.com/scott0107000/BoutiqueBitmap9x9>。
