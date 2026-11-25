# 第三方资产与字体归属（ATTRIBUTION）

> 复核日期：2026-11-24 · 对应版本：cardterm 1.0.0（GPL 对应源码已随包分发）
> 本文档是 cardterm **开源再分发时的权威归属说明**。它不改变任何代码/字体的许可，
> 也不构成法律意见；有疑义时以各上游许可原文为准。

**English summary** — cardterm's own code is MIT (see [`../LICENSE`](../LICENSE));
it vendors LovyanGFX 1.3.0 (FreeBSD), embeds three baked bitmap font tables built from
Adafruit glcdfont (BSD-2-Clause), BoutiqueBitmap9x9 1.93 (SIL OFL-1.1),
ChillBitmap 7px (SIL OFL-1.1 + reserved font name), Fusion Pixel 10px/8px Monospaced
(SIL OFL-1.1), and WenQuanYi Bitmap Song 14px (**GPL-2.0 with font embedding exception**),
plus hand-drawn ASCII/Western glyphs by the cardterm author (MIT). The per-item license,
source URL, usage and redistribution obligations (including the GPL font obligations that
apply to the 8×14 table) are listed below. **Before publishing a binary/deb you must ship
the license texts listed in §4/§5** — the current `.deb` still lacks the WenQuanYi GPL text,
the Adafruit BSD notice and the LovyanGFX license.

---

## 1. 总览

| 资产 | 版本 / 日期 | 版权 / 作者 | 许可证 | 在 cardterm 中的用途 |
|---|---|---|---|---|
| cardterm 代码 | 0.7.2 | © 2026 fsender | **MIT**（[`LICENSE`](../LICENSE)） | 程序本体 |
| LovyanGFX（vendored） | 1.3.0 | © 2020 lovyan03 及贡献者 | **FreeBSD**（2-clause BSD） | `/dev/fb0` 面板驱动、绘图（`Panel_fb`） |
| Adafruit glcdfont（经 LovyanGFX Font0） | 5×7，2012 | © 2012 Adafruit Industries | **BSD 2-Clause** | 6×10 的 ASCII 0x20–0x7E |
| 手绘 4×6 ASCII | — | cardterm 作者 | **MIT**（随项目） | 4×7 的 ASCII 0x00–0x7F |
| 手绘西文 / CP437 符号 | — | cardterm 作者 | **MIT**（随项目） | 8×14 的西文与 CP437 符号位 |
| BoutiqueBitmap9x9（精品點陣體9×9） | 1.93 | © 2020–2022《字言字語》Cen-cyun / Luke Liu；基于 M+ BITMAP FONTS（© COZ）与 BestTen-DOT | **SIL OFL-1.1**（保留字体名） | 6×10 的非 ASCII 主体；8×14 兜底 |
| ChillBitmap（寒蝉点阵体）7px | 7px（2020–2022 warren2060 / 2024 ChillType） | © 2020–2022 warren2060；© 2024 ChillType；基于 美咲フォント（© 2002–2021 Num Kadoma）等 | **SIL OFL-1.1**（保留字体名）+ 7px 自定义许可条款 | 4×7 的 Unicode 主体 |
| Fusion Pixel 10px Monospaced（缝合像素字体） | 本地 BDF 生成于 2026-11-20；对应上游 release 2026.11.10（素材未记录版本号） | © 2022 TakWolf 及贡献者 | **SIL OFL-1.1** | 6×10 的兜底（Hangul/盲文/更广 BMP）；8×14 的最终兜底 |
| Fusion Pixel 8px Monospaced zh_Hans | 2026.11.10 | © 2022 TakWolf 及贡献者；含 Ark Pixel / Misaki / MisekiBitmap / BoutiqueBitmap7x7 / Galmuri 字形 | **SIL OFL-1.1**（另附上游各字体通知） | 4×7 的兜底（Hangul/盲文/制表符/几何图形等） |
| 文泉驿点阵宋体（WenQuanYi Bitmap Song） | 0.9.9.8（Release 8；发行包 1.0.0-RC1） | © 2004–2010 WenQuanYi Project Board of Trustees 与 Qianqian Fang | **GPL-2.0 + 字体嵌入例外**（见 §3.5 的表述差异） | 8×14 的 Unicode/CJK 主体；另用 10pt/9pt 版本兜底 |
| 参数化制表符/方块/盲文 | — | cardterm 作者（`tools/boxdraw.py` 及生成器） | **MIT**（随项目） | 三套字体表统一重绘 U+2500–259F / U+2800–28FF |

字体表字形数（`src/term_font*.h` 中 `FontTable::count` 的实测值，供核对）：
6×10 = **26,163**，4×7 = **29,081**，8×14 = **43,429**。

> ⚠️ 仓库内另有两份**已过时**的许可说明：`docs/fonts.md` 的“数据来源与许可”表、
> `packaging/fonts/NOTICE.md`。它们把 glcdfont 写成“公版”、把 BoutiqueBitmap9x9
> 写成“自定义宽松条款”、只描述两套字体且完全没提文泉驿。具体差异见 §6。
> **以本文档为准**（为控制改动范围，本次未直接修改这两份既有文件，建议项目所有者随后同步修正）。

> 📦 **素材可见性**：cardterm 仓库只含**烘焙后的** `src/term_font*.h` 与手绘源图 `extra/*.png`；
> 字体源文件（TTF/BDF）、`extra_assets/fonts/` 下的中间产物、生成脚本
> （在工作区同级 `helloworld_customfont/tools/`）**不随 cardterm 仓库分发**（README“构建”节亦如此说明）。
> 若发布二进制，为满足 OFL 的溯源与 GPL 的“对应源码”义务，应把这些源文件/脚本
> （至少 8×14 的 BDF 与 `build_wqy8x14.py`）作为 release 资产或独立源码包一并提供。

---

## 2. cardterm 自己的代码与素材

- **代码**：MIT，Copyright (c) 2026 fsender，全文见根目录 [`LICENSE`](../LICENSE)。
- **手绘字形**：`extra/cardterm_font.png`（4×6 ASCII，用于 4×7）、
  `extra/cardterm_1.png`（8×14 西文/CP437 符号）由 cardterm 作者手工绘制，
  随项目按 MIT 发布。生成脚本（`gen_term_font.py`、`build_small_font.py`、
  `build_wqy8x14.py`、`boxdraw.py`）在工作区同级 `helloworld_customfont/tools/`，
  **不随 cardterm 仓库分发**（见上方“素材可见性”）。
- `extra/logo.svg` 目前未入库（`git status` 显示未跟踪），若要随开源发布，
  需要项目所有者确认其来源/许可。

---

## 3. 逐项归属明细

### 3.1 LovyanGFX 1.3.0（vendored 第三方库）

- 位置：`lib/LovyanGFX-1.3.0/`（随源码分发，不改名）。
- 上游：<https://github.com/lovyan03/LovyanGFX>，版本 **1.3.0**（2026-11-05 发布）。
- 许可证：**FreeBSD License（2-clause BSD）**，© 2020 lovyan03。
  原文 `lib/LovyanGFX-1.3.0/license.txt`。
- **vendored 副本是裁剪版**：cardterm 移除了未使用的 GFXFF / IPA / efont / Custom 等字体资源
  （见 `CHANGELOG.md` 的 LovyanGFX 精简条目），因此这些内嵌字体不再随 cardterm 源码/二进制分发；
  `license.txt` 中的完整第三方清单仍保留，用于**溯源**（而不是“仍在使用”的声明）。
  BSD 2-clause 允许修改与再分发，前提是保留版权声明与许可文本——本副本已保留。
- 该 `license.txt` 同时收录 LovyanGFX 内嵌第三方代码的许可原文，其中与 cardterm 直接相关的：
  - **Adafruit GFX / GLCD font：BSD 2-Clause，© 2012 Adafruit Industries**
    （即 6×10 使用的 glcdfont 的法律来源）；
  - TFT_eSPI：BSD，© 2020 Bodmer（其字体资源已被裁剪，未使用）；
  - TJpgDec（ChaN，自有许可）、Pngle（MIT）、QRCode（MIT）、`result`（MIT）等——
    cardterm 未使用这些功能，但 `license.txt` 随源码原样保留。
- 再分发义务：FreeBSD 2-clause 要求在**二进制分发的文档/材料**中复现版权声明、
  条件与免责声明；因此分发 deb 时应把 `license.txt` 一并装入
  （例如 `/usr/share/doc/cardterm/`），这样也同时满足 glcdfont 的 BSD 声明义务。

### 3.2 Adafruit glcdfont（6×10 的 ASCII）

- 来源：LovyanGFX `src/lgfx/Fonts/glcdfont.h`（即 Adafruit_GFX 的经典 5×7 点阵，
  LovyanGFX 的 `Font0`）；由 `gen_term_font.py` 取用，占 6×10 表 ASCII 0x20–0x7E。
- 版权/许可证：文件头携带 **BSD 2-Clause** 全文，`Copyright (c) 2012 Adafruit Industries`；
  上游 <https://github.com/adafruit/Adafruit-GFX-Library/blob/master/license.txt> 同文；
  LovyanGFX README 的“License”一节也写明 “GFX font and GLCD font: 2-clause BSD, Adafruit Industries”。
- 再分发义务：二进制分发需在文档中复现上述版权声明与 BSD 条件/免责声明
  （最省事的做法是随包提供 LovyanGFX `license.txt`，其中已包含该段原文）。
- **更正**：它不是“公版/公有领域”。`packaging/fonts/NOTICE.md` 与 `docs/fonts.md`
  的“public domain / 公版”表述不准确，按实际文件头应为 BSD-2-Clause。

### 3.3 BoutiqueBitmap9x9 1.93（6×10 的非 ASCII 主体；8×14 兜底）

- 上游：<https://github.com/scott0107000/BoutiqueBitmap9x9>（tag `1.93`，2026-03-29）。
- 作者/版权：© 2020–2022《字言字語》Cen-cyun（Luke Liu）；
  字型基于 **M+ BITMAP FONTS**（© 2002–2004 COZ）与 **ベストテン BestTen-DOT** 改作，
  并以 Fusion Pixel 补字。
- 许可证：**SIL Open Font License 1.1**（保留字体名 `BoutiqueBitmap`、精品點陣體、精品点阵体）。
  上游 `OFL.txt` 的正文即完整 SIL OFL 1.1，其开头保留了 M+ / BestTen-DOT 的原始许可通知；
  仓库内 `packaging/fonts/BoutiqueBitmap9x9/OFL.txt`（118 行）是这份文件的完整副本
  （含 OFL 正文，可直接随包分发）。
- 用途：`term_font.h`（6×10）的非 ASCII 字形主体；`term_font_8x14.h` 在文泉驿与
  其 10pt/9pt 兜底都缺字时也会取用。
- 再分发义务：OFL-1.1 §2 —— 每份副本须附版权声明与本许可（已随 deb 分发）；
  §3 —— 修改版不得使用保留字体名（cardterm 的烘焙表命名为 `CardTerm 6x10` /
  `CardTerm WQY 8x14`，未使用上游名）；§5 —— 修改后的字体数据仍须以 OFL 分发，
  不能改标为 MIT。OFL 允许把字体嵌入/捆绑进软件而软件本身不受 OFL 传染。
- **更正**：`docs/fonts.md` 写的“自定义宽松条款”是早期印象；1.93 上游 README 明确声明
  以 SIL OFL 1.1 发布（OFL.txt 里那段“衍生品授权必须相同”的文字是 M+ 系旧通知，
  与 OFL §5 并不冲突）。按 OFL-1.1 处理最稳妥。

### 3.4 ChillBitmap 7px（4×7 的 Unicode 主体）

- 上游：<https://github.com/Warren2060/ChillBitmap>（release `v2.502`）。
- 版权：© 2020–2022 warren2060；© 2024 ChillType。7px 由
  美咲フォント（© 2002–2021 Num Kadoma）、BoutiqueBitmap7x7、Guanzhi 8px 等改作/融合。
- 许可证：
  - **SIL OFL-1.1**，并声明保留字体名 `'ChillBitmap' '7px & 16px'`
    （上游 `License` / 仓库内 `packaging/fonts/ChillBitmap/OFL-1.1.txt`）；
  - 另有 7px 专用的 `7px-License.txt`（© 2020–2022 warren2060，免费软件、
    衍生品须同许可）+ 美咲フォント的版权/许可通知；仓库内 `ChillBitmap/7px-License.txt`
    与 `LICENSE.md` 已一并打包。`LICENSE.md` 中的 GPLv2 段落针对 16px 变体的
    构建程序/混合来源（16px 基于 Unifont），cardterm 未使用 16px。
- 用途：`term_font_small.h`（4×7）的 Unicode 宽字形与部分窄字形。
- 再分发义务：随包附 OFL-1.1 + `7px-License.txt`（+ `LICENSE.md` 更保险）——
  当前 `packaging/fonts/ChillBitmap/` 已齐；修改版不得使用保留字体名（cardterm 用
  `CardTerm Small 4x7`）；烘焙表仍属 OFL 修改版。

### 3.5 文泉驿点阵宋体（WenQuanYi Bitmap Song）14px（8×14 的 Unicode/CJK 主体）

- 上游项目：<http://wenq.org/>（WenQuanYi，Spring of Letters）；
  发布文件：<https://sourceforge.net/projects/wqy/files/wqy-bitmapfont/> 的
  `1.0.0-RC1/wqy-bitmapsong-bdf-1.0.0-RC1_GPLv2+.tar.gz`。
  本地生成用的 BDF（`wenquanyi_13px.bdf` 等）来自该 1.0.0-RC1 的 BDF 包
  （BDF 头：Version 0.9.9.8 / Release 8；`helloworld_customfont/tools/build_wqy8x14.py`
  也从 `.tmp/wqy/wqy-bitmapsong/` 读取）。
- 作者/版权：© 2004–2010 WenQuanYi Project Board of Trustees 与 Qianqian Fang。
- 许可证：**GNU GPL v2 + 字体嵌入例外（font embedding exception）**。
  - BDF 文件头 `COMMENT License : GPL v2 (with font embedding exception)`；
  - 上游 `README` 第 149 行起给出完整例外条款；
  - Debian 打包（`xfonts-wqy`，trixie 版 1.0.0~rc1-8）把它的许可机器可读地标为
    `GPL-2-with-font-embedding-exception`：
    <https://sources.debian.org/data/main/x/xfonts-wqy/1.0.0~rc1-8/debian/copyright>。
  - ⚠️ **表述差异（如实记录，不猜）**：SourceForge 上的发布包文件名写作 `GPLv2+`，
    而 README/COPYING/Debian 版权文件写的都是 **GPL v2（第 2 版）**。
    最保守的处理是当作 **GPL-2.0（不带 or-later）**；无论取哪种，再分发的核心义务相同
    （附许可全文、保留版权声明、提供对应源码）。若项目所有者要写死 SPDX 标识，
    建议先向 WenQuanYi 存档/打包者确认，或统一用 `GPL-2.0-with-font-embedding-exception`。
- 用途：`term_font_8x14.h`（8×14）的 Unicode/CJK 主体（CJK 墨迹 13×13、占双格）；
  缺字依次取 `wenquanyi_10pt.bdf`、`wenquanyi_9pt.bdf`（同许可），
  再取 BoutiqueBitmap9x9 与 Fusion Pixel 10px 兜底。
- 再分发义务（重点，见 §4）：GPL 要求分发字体数据时提供**对应源码**与 **GPL-2 全文**。
  “字体嵌入例外”按字面只覆盖“把该字体或**未经修改的部分**嵌入文档”的情形；
  cardterm 的 8×14 表是**子集化/重排/重绘后的修改版**（Modified Version），
  不应依赖该例外来回避 GPL 义务。因此：把 8×14 表视为 GPL-2.0 字体数据，
  随分发提供 GPL-2 文本 + 该字体的“源码”（`extra_assets/fonts/cardterm_wqy_8x14.bdf`
  与 `helloworld_customfont/tools/build_wqy8x14.py`）或书面提供承诺。
  注意这两项**目前都不在 cardterm 仓库内**（在工作区同级目录），发布前必须另行提供。
  MIT 的 cardterm 代码与 GPL 字体数据是“聚合”（mere aggregation）：代码可继续 MIT，
  但必须明确声明二进制里包含 GPL-2.0 字体数据，且该部分不可按 MIT 单独再许可。

### 3.6 Fusion Pixel（缝合像素字体）10px / 8px

- 上游：<https://github.com/TakWolf/fusion-pixel-font>（字体部分 **SIL OFL-1.1**，
  © 2022 TakWolf；构建程序 MIT，未嵌入）。
- 10px Monospaced：`term_font.h`（6×10）与 `term_font_8x14.h`（8×14）的兜底。
- 8px Monospaced **zh_Hans 2026.11.10**：`term_font_small.h`（4×7）的兜底；
  由 Ark Pixel（OFL-1.1）、Misaki（无类型许可，声明兼容 OFL-1.1）、
  MisekiBitmap（OFL-1.1）、BoutiqueBitmap7x7（OFL-1.1）、Galmuri（OFL-1.1）拼合。
  上游各字体的许可通知在 `packaging/fonts/FusionPixel/` 中随包提供：
  `OFL.txt`、`misaki.txt`、`miseki-bitmap-LICENSE.txt`、`galmuri-LICENSE.txt`、
  `boutique-bitmap-7x7-OFL.txt`。
- 再分发义务：附 OFL 文本与版权行（已随 deb）；OFL §5 要求修改版仍以 OFL 分发。
- **更正**：上游 `LICENSE-OFL` 里**没有**声明 Reserved Font Name（复核
  <https://github.com/TakWolf/fusion-pixel-font/blob/master/LICENSE-OFL>），
  所以 `packaging/fonts/NOTICE.md` 写的“保留名 Fusion Pixel”缺乏许可依据；
  cardterm 仍主动改用 `CardTerm *` 命名（好习惯，但不是强制的）。
- **素材元数据小问题（P2，不影响归属结论）**：`extra_assets/fonts/fusion-pixel-10px.bdf`
  的 BDF 头 `FONT`/`COMMENT` 被误写成 `boutique-boutique9x9…` / “BoutiqueBitmap9x9 1.93”，
  但字表内容经核对是 **Fusion Pixel 10px Monospaced**：25,203 个 BMP 字形、
  含 U+AC00（谚文）与 U+2800（盲文），而上游 BoutiqueBitmap9x9 的 BDF 只有 13,039 字形且
  不含这两类。建议下次重新生成时修正 BDF/`u8g2` 元数据（同时把 `Copyright: (null)` 补成
  上游版权行，便于溯源与 OFL 合规检查）。

### 3.7 参数化制表符 / 方块 / 盲文

- `tools/boxdraw.py` 与生成器对 U+2500–U+259F、U+2800–28FF 按目标单元程序化重绘
  （比上游字形更适合 4/6/8px 终端单元）。这部分代码/数据由 cardterm 作者创作，
  按项目 MIT 发布；它替换了上游字体在这些码位的字形，因此不额外产生上游义务。

---

## 4. 再分发义务清单（二进制 / deb / 源码）

| 义务 | 许可依据 | 现状 | 需要做什么（建议优先级 P0/P1/P2） |
|---|---|---|---|
| cardterm 代码许可 | MIT | 源码有 `LICENSE` | deb 增加 `/usr/share/doc/cardterm/copyright`（含 MIT 全文与下方第三方清单），P1 |
| LovyanGFX 许可 | FreeBSD 2-clause（二进制形式须在文档/材料中复现版权+条件+免责） | 源码有 `lib/LovyanGFX-1.3.0/license.txt`；**deb 未包含** | 把 `license.txt` 装进 deb 文档目录，P0 |
| Adafruit glcdfont 许可 | BSD-2（同上，二进制形式义务） | **deb 未包含** | 随 LovyanGFX `license.txt` 一并装入即可（其中含 Adafruit_GFX 段），P0 |
| BoutiqueBitmap9x9 | OFL-1.1 §2/§3/§5 | `packaging/fonts/BoutiqueBitmap9x9/OFL.txt` 完整，deb 已装 | 保持现状；确认打包时该目录整体复制，P2 |
| ChillBitmap 7px | OFL-1.1 + 7px 自定义条款 | `OFL-1.1.txt` + `7px-License.txt` + `LICENSE.md` 已随 deb | 保持；勿删 `7px-License.txt`，P2 |
| Fusion Pixel 10px/8px | OFL-1.1 + 上游字体通知 | `FusionPixel/` 下 5 份文本已随 deb | 保持；该目录同时服务 10px/8px 两代素材，无需改名，P2 |
| **文泉驿点阵宋体 14px** | **GPL-2.0（+字体嵌入例外）** | **deb 与 cardterm 仓库都没有 GPL-2 文本、也没有字体源码随包** | **P0**：① 装入 GPL-2 全文（如 `packaging/fonts/WenQuanYi/COPYING` + `README`（含例外条款））；② 提供对应源码：把 `cardterm_wqy_8x14.bdf` + `build_wqy8x14.py`（现在工作区 `extra_assets/`、`helloworld_customfont/tools/`，不在 cardterm 仓库）作为 release 资产/源码包发布，或写书面提供承诺；③ 在 deb copyright 里注明“二进制包含 GPL-2.0 字体数据（8×14 表）” |
| 许可证文本随二进制分发 | 各许可共同要求 | 见上 | 建议统一放 `/usr/share/doc/cardterm/`：`copyright`（DEP-5 机器可读）+ `fonts/`（现有）+ `LovyanGFX-license.txt`，P1 |
| 上游名称使用 | OFL 保留字体名条款 | 烘焙表命名为 `CardTerm 6x10`/`CardTerm Small 4x7`/`CardTerm WQY 8x14` | 保持；文档/宣传中不要暗示是上游官方版本，P2 |

**一句话结论**：cardterm 仓库内的**许可文本**基本齐备（三份 OFL 原文 + LovyanGFX `license.txt`），
但**分发侧（deb）缺三样**：文泉驿的 GPL-2 文本与字体源码提供方式、LovyanGFX/Adafruit 的 BSD 通知、
cardterm 自己的 `copyright` 文件；另外字体源文件/生成脚本本身不在 cardterm 仓库里，
发布二进制时需另行提供（GPL 的“对应源码”义务）。其中文泉驿与 BSD 通知是硬性义务，开源发布前必须补齐。

---

## 5. 项目许可证状态（任务要求的 LICENSE 检查）

- 根目录**已有** `LICENSE`：**MIT**，`Copyright (c) 2026 fsender`。
  因此本任务不涉及“缺 LICENSE 需要自行选择许可证”的情形，也**不需要**替项目所有者选许可证。
- 已使用的第三方许可证清单（分发时需随附的文本）：
  1. **MIT**（cardterm 代码）；
  2. **FreeBSD / BSD-2-Clause**（LovyanGFX；Adafruit glcdfont 的 BSD 段包含在 LovyanGFX `license.txt` 中）；
  3. **SIL OFL-1.1**（BoutiqueBitmap9x9、ChillBitmap 7px、Fusion Pixel 10px/8px，
     其中 ChillBitmap 另附 7px 自定义条款与美咲フォント通知）；
  4. **GPL-2.0 + 字体嵌入例外**（文泉驿点阵宋体 14px/10pt/9pt——8×14 表）。
- 建议项目所有者在发布说明中明确：**“cardterm 代码 MIT；内置 8×14 字体表含 GPL-2.0
  数据，随源码/字体 BDF 一起分发”**，避免把整个二进制简单标成 MIT-only。

---

## 6. 已知文档不一致（建议后续同步，本次未改）

本次评估为控制改动范围，未直接修改下面几份既有文件（它们仍在并行改动范围内），因此仍保留旧说法：

| 文件 | 过时/错误内容 | 正确说法 |
|---|---|---|
| `docs/fonts.md` | 只写“两套字体”（6×10 / 4×7），未收录 8×14；许可表写 glcdfont“公版”、Boutique“自定义宽松条款”；字形数 26,162 / 29,080 | 三套（含 8×14 文泉驿）；glcdfont = BSD-2-Clause；BoutiqueBitmap9x9 1.93 = SIL OFL-1.1；实测 26,163 / 29,081 / 43,429 |
| `packaging/fonts/NOTICE.md` | 同上（“公版”“自定义宽松条款”），且**完全没有文泉驿一行**；称 Fusion Pixel 有“保留名” | 按 §1/§3 修正；补 WenQuanYi GPL 行；Fusion Pixel 未声明保留名 |
| `packaging/build-deb.sh`（注释 L42） | “两套点阵字体均为 OFL/公版，随包分发需附原文” | 三套；8×14 为 GPL-2.0 字体数据，另需 GPL 文本与源码提供方式 |

> 这些是**发布前必须修**的文档级问题（P1），建议由项目所有者或打包/文档维护者后续处理。
> 另：`packaging/control.in` 的 `Maintainer` 已修正为 `fsender <f_ender@163.com>`；
> `Homepage: https://www.m5stack.com` 仍是占位值，开源发布前应替换为真实项目主页。

---

## 7. 核实所引用的 URL

| URL | 用于核实 |
|---|---|
| <https://github.com/lovyan03/LovyanGFX> | LovyanGFX 版本 1.3.0、README“License”节（main: FreeBSD；GFX/GLCD font: 2-clause BSD Adafruit） |
| <https://raw.githubusercontent.com/adafruit/Adafruit-GFX-Library/master/license.txt> | glcdfont/Adafruit GFX 的 BSD 2-Clause 原文与 © 2012 Adafruit |
| <https://github.com/scott0107000/BoutiqueBitmap9x9> | BoutiqueBitmap9x9 版本 1.93、作者、README 声明的 SIL OFL-1.1 与保留字体名 |
| <https://raw.githubusercontent.com/scott0107000/BoutiqueBitmap9x9/main/OFL.txt> | BoutiqueBitmap9x9 的 OFL-1.1 全文（含 M+ / BestTen-DOT 旧通知），与仓库内副本一致 |
| <https://github.com/Warren2060/ChillBitmap> | ChillBitmap 7px/16px 版本与来源、README 声明的 SIL OFL |
| <https://raw.githubusercontent.com/Warren2060/ChillBitmap/main/License> | ChillBitmap 的 OFL-1.1 全文与保留字体名 `'ChillBitmap' '7px & 16px'` |
| <https://github.com/TakWolf/fusion-pixel-font> | Fusion Pixel 版本、字体 OFL-1.1 / 构建程序 MIT、8px 组成字体清单 |
| <https://raw.githubusercontent.com/TakWolf/fusion-pixel-font/master/LICENSE-OFL> | Fusion Pixel 的 OFL-1.1 全文（确认**未**声明保留字体名） |
| <https://sources.debian.org/src/xfonts-wqy/> | Debian 打包名 `xfonts-wqy`、版本 1.0.0~rc1（trixie/forky/sid） |
| <https://sources.debian.org/data/main/x/xfonts-wqy/1.0.0~rc1-8/debian/copyright> | 文泉驿点阵宋体 = `GPL-2-with-font-embedding-exception`，含例外条款全文与上游 SourceForge 地址 |
| <https://sourceforge.net/projects/wqy/> | 上游项目主页；发布物 `wqy-bitmapfont/1.0.0-RC1/wqy-bitmapsong-bdf-1.0.0-RC1_GPLv2+.tar.gz`（文件名中的 GPLv2+ 与文本中的 GPL v2 的差异见 §3.5） |
| <https://sourceforge.net/projects/wqy/files/wqy-bitmapfont/1.0.0-RC1/> | 文泉驿点阵宋体 BDF 发布目录 |

本地佐证（未改动）：
`lib/LovyanGFX-1.3.0/license.txt`、`lib/LovyanGFX-1.3.0/src/lgfx/Fonts/glcdfont.h`、
`src/term_font.h` / `src/term_font_small.h` / `src/term_font_8x14.h` 头注释、
`packaging/fonts/**`、`CHANGELOG.md`（0.4.5 的 8×14 条目与 0.4.2 字体条目）、
`helloworld_customfont/tools/{gen_term_font,build_small_font,build_wqy8x14}.py`、
`extra_assets/fonts/README.md`、`extra_assets/fonts/fallback8px/README.md`。
