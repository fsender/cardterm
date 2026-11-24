# WenQuanYi Bitmap Song（文泉驿点阵宋体）14px

- 用途：cardterm 8×14 字体（40×12 网格）的 CJK 字形来源；已做子集化 / 重排 / 与手绘西文合成。
- 上游：https://sourceforge.net/projects/wqy/ （wqy-bitmapfont 1.0.0-RC1）
- 许可：GPL-2.0（含"字体嵌入例外"，但本仓库对其做了修改，故按 GPL-2.0 处理）。
- 再分发义务：
  1. 随分发附 GPL-2.0 全文（本目录 `COPYING-GPL-2.0.txt`）；
  2. 提供被修改字体数据的对应源码。生成脚本与中间 BDF 目前在开发工作区（`wqy` 字体管线），
     **已于 2026-11-24 入库**：`source/cardterm_wqy_8x14.bdf`（合并后的修改版点阵源）与
     `source/build_wqy8x14.py`（及配套 gen_term_font.py / build_small_font.py / boxdraw.py）随包分发到
     `/usr/share/doc/cardterm/fonts/WenQuanYi/source/`，满足 GPL-2.0 对应源码义务。
- 若不愿意承担 GPL 义务：可改用 OFL 字体（例如 Fusion Pixel 14px）替换 8×14 的 CJK 部分，
  该替换只影响字体生成管线，不涉及终端代码。

## 决定（2026-11-24）：保留文泉驿，按 GPL-2.0 合规分发

本项目**决定保留** 8×14 字体中的文泉驿 CJK 字形（视觉密度优于可替代的 OFL 方案），
并按下述方式满足 GPL-2.0 义务：

1. 本目录随 deb 分发 **GPL-2.0 全文**（`COPYING-GPL-2.0.txt`，`/usr/share/doc/cardterm/fonts/WenQuanYi/`）。
2. **release 资产必须附带被修改字体数据的"首选修改形式"**：生成后的
   `cardterm_wqy_8x14.bdf`（14px、子集/重排后的点阵源）与生成脚本
   （`build_wqy8x14.py` 或等价），放在 GitHub Release 附件中，并在 release notes 说明获取方式。
   仓库内对应位置：`packaging/fonts/WenQuanYi/source/`（发布时放入，未放入则 release 附件提供）。
3. 再分发者修改该字体数据后同样需以 GPL-2.0 提供其修改版源码。
4. 若将来不想承担 GPL 义务，可改用 SIL OFL 方案（如 Fusion Pixel 12px），
   替换只影响字体生成管线，不涉及终端代码。
