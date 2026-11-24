// termfont_sizes.h — 两张字体表的设计单元尺寸（仅 ui.cpp 用于配置校验/提示）
//
// 数值必须与生成的 term_font.h / term_font_small.h 中的 FontTable 一致；
// 生成器: gen_term_font.py (6x10) / build_small_font.py (4x7)。
// 单独成头是为了让 ui.cpp 不必包含 ~5MB 的字形数据（避免二进制重复）。
#pragma once

namespace termfont {

constexpr int rows_6x10   = 9;
constexpr int cell_w_6x10 = 6;
constexpr int cell_h_6x10 = 10;

constexpr int rows_4x7    = 7;
constexpr int cell_w_4x7  = 4;
constexpr int cell_h_4x7  = 7;

}  // namespace termfont
