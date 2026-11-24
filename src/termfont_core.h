// termfont_core.h — 终端字体表公共类型（6x10 与 4x7 两张表共用）
//
// 由生成的 term_font.h / term_font_small.h 引用；本文件手写，不随字体重新生成。
#pragma once
#include <stdint.h>

namespace termfont {

// 一个字形：码点 + 占几格（1/2）+ 墨迹跨度（可超过单宽 cell_w，溢出到右侧相邻格）
// + 位图起始下标（rows 行，每行 uint16，位 c = 列 c，LSB 为最左列）。
struct TermGlyph {
  uint16_t cp;
  uint8_t  cells;
  uint8_t  w;
  uint32_t bits;
};

// 一张字体表：字形数组 + 位图池 + 行数 + 设计单元尺寸。
struct FontTable {
  const TermGlyph* glyphs;
  int              count;
  const uint16_t*  bits;
  uint8_t          rows;    // 每字形行数（6x10 → 10；4x7 → 7）
  uint8_t          cell_w;  // 单宽单元宽（6 / 4）
  uint8_t          cell_h;  // 单元高（10 / 8）
};

// 二分查找；未收录返回 nullptr。
inline const TermGlyph* find(const FontTable& t, uint16_t cp)
{
  int lo = 0, hi = t.count - 1;
  while (lo <= hi) {
    int mid = (lo + hi) >> 1;
    uint16_t v = t.glyphs[mid].cp;
    if (v == cp) return &t.glyphs[mid];
    if (v < cp) lo = mid + 1; else hi = mid - 1;
  }
  return nullptr;
}

}  // namespace termfont
