// ime_ui.cpp — 中文输入叠加层渲染（候选框 + 编辑行）
// 布局/配色契约见 ime_ui.h。形状用 tft 硬边图元（fillRect/fillCircle/drawRoundRect），
// 文本走 terminal 的文本原语（字体表单份）。本文件不碰引擎与会话状态。
#include "ime_ui.h"

#include <cstdio>
#include <cstring>

#include "ime.h"
#include "terminal.h"
#include "ui.h"          // extern LGFX tft

namespace ime_ui {
namespace {

// ── 几何常量（用户规格）────────────────────────────────────────
constexpr int kBoxH     = 79;    // 固定总高（2 + 5*15 + 2）
constexpr int kRowH     = 15;    // 每选项行高（文本 14px + 上方 1px）
constexpr int kBorder   = 2;     // 边框宽
constexpr int kEdge     = 8;     // 左右圆角区（边框 2 + 圆角缓冲 6）
constexpr int kGap      = 8;     // 两列间空格（凸条圆角展示区）
constexpr int kR        = 10;    // 外圆角 r10
constexpr int kEntryCap = 16;    // 条目最大格宽（序号+句点+7 汉字）
constexpr int kTextTop  = 1;     // 文本在行内上留 1px

// 颜色配置项（toast/全屏横幅同款）：black=底(bright0)、blue=边框/Tab(bright4)、
// white=白(bright7)；编辑行用 regular7(pal[7])。RGB888 直到 tft 调用才转 565。
// config [colors] ime_* 覆盖优先，0 = 默认 bright 色系。
struct Colors {
  uint32_t base, border, text, sel_bg, sel_fg, status_bg, status_fg, reg7;
  int      transparency = 0;           // [colors] ime_transparency（0–90；0=不透明快速路径）
};

Colors colors()
{
  const cfg::UiColors& u = term::ui_colors();
  // 默认三色与 toast/全屏横幅同源（ui_chrome_colors）：白=文字/高亮底、
  // 暗=框底/未选中底/高亮字/Tab 字、蓝=边框/Tab 底；ime_* 覆盖优先。
  uint32_t cbg = 0, cbd = 0, cfg = 0;
  term::ui_chrome_colors(&cbg, &cbd, &cfg);
  Colors c;
  c.base      = u.ime_bg ? u.ime_bg : cbg;
  c.border    = u.ime_border ? u.ime_border : cbd;
  c.text      = u.ime_text ? u.ime_text : cfg;
  c.sel_bg    = u.ime_sel_bg ? u.ime_sel_bg : c.text;
  c.sel_fg    = u.ime_sel_text ? u.ime_sel_text : c.base;
  c.status_bg = u.ime_status_bg ? u.ime_status_bg : c.border;
  c.status_fg = u.ime_status_text ? u.ime_status_text : c.base;
  c.reg7      = u.ime_compose ? u.ime_compose : term::pal_config(7);
  c.transparency = u.ime_transparency;   // 半透明（画布合成口径，见 term::ov_begin）
  return c;
}

uint16_t c565(uint32_t rgb)
{
  // 与 terminal 的 to565 同式（此前取位错拿蓝字节，图元颜色全乱，务必保持一致）
  return (uint16_t)((((rgb >> 16) & 0xF8) << 8) | (((rgb >> 8) & 0xFC) << 3) | ((rgb & 0xff) >> 3));
}

// 无抗锯齿圆角矩形填充（同 toast：中段/直边 fillRect，四角 fillCircle 拼）
void fill_round_rect(int x, int y, int w, int h, int r, uint16_t col)
{
  if (w <= 0 || h <= 0) return;
  if (r > w / 2) r = w / 2;
  if (r > h / 2) r = h / 2;
  if (r <= 0) { term::ov_dst().fillRect(x, y, w, h, col); return; }
  term::ov_dst().fillRect(x + r, y, w - 2 * r, h, col);
  term::ov_dst().fillRect(x, y + r, r, h - 2 * r, col);
  term::ov_dst().fillRect(x + w - r, y + r, r, h - 2 * r, col);
  term::ov_dst().fillCircle(x + r, y + r, r, col);
  term::ov_dst().fillCircle(x + w - r - 1, y + r, r, col);
  term::ov_dst().fillCircle(x + r, y + h - r - 1, r, col);
  term::ov_dst().fillCircle(x + w - r - 1, y + h - r - 1, r, col);
}

// r10 线宽 2 圆角描边（helloworld 实测无空洞法：外 r + 内 r-1 + 内 r-2）
void stroke_round_rect_w2(int x, int y, int w, int h, int r, uint16_t col)
{
  term::ov_dst().drawRoundRect(x, y, w, h, r, col);
  term::ov_dst().drawRoundRect(x + 1, y + 1, w - 2, h - 2, r - 1, col);
  term::ov_dst().drawRoundRect(x + 1, y + 1, w - 2, h - 2, r - 2, col);
}

// 像素是否在框的外圆角矩形内（r10 之外不画：那里透出终端内容）
bool inside_box(int px, int py, int bx, int by, int W, int H)
{
  if (px < bx || px >= bx + W || py < by || py >= by + H) return false;
  const int r = kR;
  auto corner_ok = [&](int cx, int cy) {
    const int dx = px - cx, dy = py - cy;
    return dx * dx + dy * dy <= r * r;
  };
  if (px < bx + r && py < by + r)               return corner_ok(bx + r, by + r);
  if (px >= bx + W - r && py < by + r)          return corner_ok(bx + W - 1 - r, by + r);
  if (px < bx + r && py >= by + H - r)          return corner_ok(bx + r, by + H - 1 - r);
  if (px >= bx + W - r && py >= by + H - r)     return corner_ok(bx + W - 1 - r, by + H - 1 - r);
  return true;
}

// ── 条目文本（"N." 前缀 + 候选/符号；截断 16 格）──────────────────
struct Ent {
  char s[80];
  int  cells;
};

Ent make_entry(int num, const std::string& text)
{
  Ent e;
  const int tcells = term::text_cells814(text.c_str(), kEntryCap - 2);
  snprintf(e.s, sizeof e.s, "%d.%s", num, text.c_str());
  e.cells = 2 + tcells;
  return e;
}

// 状态行（右列第 5 行）：翻页标签页码 N/M（6x10 小字）；宽度换算成 8px 格
// 计入选区宽度。（空候选不再占状态位：由 toast 外观提示框承担，见 paint_box）
Ent make_status(int page, int pages)
{
  Ent e;
  snprintf(e.s, sizeof e.s, "%d/%d", page, pages);
  e.cells = (term::text_px610(e.s) + 7) / 8;
  return e;
}

// ── 高亮块（间隙侧 4px 凸条雕圆角；角格按外圆角裁剪）────────────────
// side: 0 = 左列（凸条向右伸入间隙），1 = 右列（凸条向左）
// corner: 0 无 / 1 TL / 2 BL / 3 TR / 4 BR（第 1/5/6 个与状态行靠盒角时裁剪）
void paint_block(int x0, int x1, int y, int bx, int by, int W, int H,
                 int side, int corner, bool is_tab, uint16_t hl, uint16_t base)
{
  // 主体（逐行填充；角格行按 r10 外圆角裁掉盒外像素 —— 两笔填色的合成结果）
  for (int r = 0; r < kRowH; r++) {
    int xa = x0, xb = x1;
    const int py = y + r;
    if (corner == 1 || corner == 2) {          // 外角在左（TL/BL）
      while (xa < xb && !inside_box(xa, py, bx, by, W, H)) xa++;
    } else if (corner == 3 || corner == 4) {   // 外角在右（TR/BR）
      while (xb > xa && !inside_box(xb - 1, py, bx, by, W, H)) xb--;
    }
    if (xb > xa) term::ov_dst().fillRect(xa, py, xb - xa, 1, hl);
  }
  // 间隙侧凸条（4×13 居中 / Tab 4×14）+ 雕角（round_3 口径：4 像素阶梯，
  // 端头 2px + 紧邻两行各 1px → 2/3/3/4 齿形；Tab 只雕上端）
  const int tx = (side == 0) ? x1 : x0 - 4;
  const int sh = is_tab ? 14 : 13;
  term::ov_dst().fillRect(tx, y + kTextTop, 4, sh, hl);
  const int ox = (side == 0) ? tx + 2 : tx;      // 凸条外端（雕角所在侧）的起点
  term::ov_dst().fillRect(ox, y + kTextTop, 2, 1, base);                 // 端首行：雕 2px
  term::ov_dst().fillRect(ox + ((side == 0) ? 1 : 0), y + kTextTop + 1, 1, 2, base);  // 次两行：各雕 1px
  if (!is_tab) {
    term::ov_dst().fillRect(ox + ((side == 0) ? 1 : 0), y + kTextTop + sh - 3, 1, 2, base);
    term::ov_dst().fillRect(ox, y + kTextTop + sh - 1, 2, 1, base);
  }
}

// ── 定位（光标跟随；候选框与无候选 toast 提示框共用同一套对齐规则）────
// 纵向：上半屏框在下方 / 下半屏在上方；53×17 中央行（第 9 行）在下方。
// 横向：左半屏左缘对齐光标 / 右半屏右缘对齐；53×17 中央列（第 27 列）在
// 光标右边；越界贴屏边。
void place_box(int W, int H, int* out_bx, int* out_by)
{
  int col = 0, row = 0;
  term::cursor_cell(&col, &row);
  const int cw = term::kCellW, ch = term::kCellH;
  const int cx = col * cw, cy = row * ch;
  const int scrW = tft.width(), scrH = tft.height();

  bool below = (cy + ch / 2) <= scrH / 2;
  if (term::kCols == 53 && term::kRows == 17 && row == 8) below = true;
  int by = below ? (cy + ch - 1) + 2 : (cy - 2) - H + 1;
  if (by < 0) by = 0;
  if (by + H > scrH) by = scrH - H;

  int bx;
  if (term::kCols == 53 && term::kRows == 17 && col == 26) {
    bx = (cx + cw - 1) + 2;
  } else if (cx + cw / 2 < scrW / 2) {
    bx = cx;
  } else {
    bx = (cx + cw - 1) - W + 1;
  }
  if (bx + W > scrW) bx = scrW - W;
  if (bx < 0) bx = 0;

  *out_bx = bx;
  *out_by = by;
}

// ── 候选框 ─────────────────────────────────────────────────────
// out_rect：返回本帧覆盖区域（残影擦除跟踪用）
void paint_box(const ime::UiState& st, const Colors& C, int out_rect[4])
{
  // 空候选：不画候选框，按候选框对齐规则放 toast 外观提示框（同三色）
  if (st.ncand <= 0) {
    static const char kNoCand[] = "无候选词";
    int W = 0, H = 0;
    term::toast_box_size(kNoCand, &W, &H);
    int bx = 0, by = 0;
    place_box(W, H, &bx, &by);
    // 半透明：内容层走合成画布（魔数蒙版模型；0 = 不透明快速路径）
    uint32_t sig = 2166136261u;
    auto fnv = [&sig](uint32_t v) { sig = (sig ^ v) * 16777619u; };
    fnv(1u); fnv((uint32_t)bx); fnv((uint32_t)by); fnv((uint32_t)W);
    fnv(c565(C.base)); fnv(c565(C.border)); fnv(c565(C.text));
    const uint16_t avoid[3] = { c565(C.base), c565(C.border), c565(C.text) };
    const uint16_t magic = term::ov_magic(c565(C.base), avoid, 3);
    if (term::ov_begin(bx, by, W, H, C.transparency, sig, magic))
      term::toast_box_at(bx, by, kNoCand, C.base, C.border, C.text);
    term::ov_end();
    out_rect[0] = bx; out_rect[1] = by; out_rect[2] = bx + W; out_rect[3] = by + H;
    return;
  }

  // 条目（页内编号 1~9；左列 1~5，右列 6~9 + 状态行）
  Ent le[5] = {}, re[5] = {};
  int ln = 0;
  for (int i = 0; i < 5 && i < st.ncand; i++) { le[i] = make_entry(i + 1, st.cand[i]); ln = i + 1; }
  for (int i = 0; i < 4 && 5 + i < st.ncand; i++) re[i] = make_entry(6 + i, st.cand[5 + i]);
  re[4] = make_status(st.page + 1, st.pages);   // 状态行（右列第 5 行，6x10 小字）

  int L = 0, R = 0;
  for (int i = 0; i < ln; i++) if (le[i].cells > L) L = le[i].cells;
  for (int i = 0; i < 4; i++) if (re[i].cells > R) R = re[i].cells;
  // 翻页标签占右下角区（含 4px 凸条+角缓冲），其宽度判定少算 8px（1 格）：
  // 不让页码把右列撑宽，否则条目右侧多出一格空白
  if (re[4].cells - 1 > R) R = re[4].cells - 1;
  if (L > 16) L = 16;
  if (L < 4) L = 4;   // 单条项目最小 32px（4 格×8px）：宽形符号不再显示不全（rc214）
  if (R < 4) R = 4;   // 右侧边栏最小 32px（4 格×8px）
  if (R > 16) R = 16;

  const int W = 2 * kEdge + kGap + 8 * (L + R);
  const int H = kBoxH;

  // ── 定位（光标跟随，规则见 place_box）─────────────────────────
  int bx = 0, by = 0;
  place_box(W, H, &bx, &by);

  out_rect[0] = bx; out_rect[1] = by; out_rect[2] = bx + W; out_rect[3] = by + H;

  // 半透明：内容层走合成画布（魔数蒙版模型；0 = 不透明快速路径）
  // 内容指纹 = UiState + 全部用色（变化才重画内容层）
  uint32_t sig = 2166136261u;
  auto fnv = [&sig](uint32_t v) { sig = (sig ^ v) * 16777619u; };
  fnv((uint32_t)bx); fnv((uint32_t)by); fnv((uint32_t)W);
  fnv((uint32_t)st.ncand); fnv((uint32_t)st.page); fnv((uint32_t)st.pages);
  fnv((uint32_t)st.sel); fnv(st.symbol ? 1u : 0u);
  for (const char* q = st.preedit.c_str(); *q; q++) fnv((uint32_t)(unsigned char)*q);
  for (int i = 0; i < st.ncand && i < 9; i++)
    for (const char* q = st.cand[i].c_str(); *q; q++) fnv((uint32_t)(unsigned char)*q);
  const uint16_t col8[8] = { c565(C.base), c565(C.border), c565(C.text),
                             c565(C.sel_bg), c565(C.sel_fg), c565(C.status_bg),
                             c565(C.status_fg), c565(C.reg7) };
  for (int i = 0; i < 8; i++) fnv(col8[i]);
  const uint16_t magic = term::ov_magic(col8[0], col8, 8);
  const bool redraw = term::ov_begin(bx, by, W, H, C.transparency, sig, magic);
  if (redraw) {

  // ① 底色（inset1/r9 铺底防圆角缝；外边框最后压上）
  fill_round_rect(bx + 1, by + 1, W - 2, H - 2, kR - 1, c565(C.base));

  // ② 选项 1~9：[高亮块] + 文本
  for (int i = 0; i < 9; i++) {
    if (i >= st.ncand) continue;
    const bool left = i < 5;
    const int r = left ? i : i - 5;
    const int ex = left ? bx + kEdge : bx + kEdge + kGap + 8 * L;
    const int ey = by + kBorder + kRowH * r;
    const Ent& e = left ? le[i] : re[i - 5];
    if (i == st.sel) {
      int corner = 0;
      if (i == 0) corner = 1;          // 第 1 个 → 盒左上角
      else if (i == 4) corner = 2;     // 第 5 个 → 盒左下角
      else if (i == 5) corner = 3;     // 第 6 个 → 盒右上角
      const int x0 = left ? bx + kBorder : ex;
      const int x1 = left ? ex + 8 * L : bx + W - kBorder;
      paint_block(x0, x1, ey, bx, by, W, H, left ? 0 : 1, corner, false,
                  c565(C.sel_bg), c565(C.base));
      term::draw_text814(ex, ey + kTextTop, e.s, C.sel_fg, C.sel_bg, true, kEntryCap);
    } else {
      term::draw_text814(ex, ey + kTextTop, e.s, C.text, C.base, true, kEntryCap);
    }
  }

  // ③ 翻页标签（右列第 5 行）：蓝色圆角标签（左缘含 4px 凸条矩形，盒右下角
  //    裁剪）+ 页码 N/M（6x10，距标签左缘 1px）+ 白色 TAB 小片（4x7 字，片内
  //    文字距片左缘 4px；仅多页显示，单页 1/1 不显示白片与 TAB 字样）。
  //    颜色仅取框三色角色：暗=页码/TAB 字、蓝=标签底、白=白片。
  {
    const int ex = bx + kEdge + kGap + 8 * L - 4;    // 标签左缘（含 4px 凸条矩形）
    const int ey = by + kBorder + kRowH * 4;
    const Ent& e = re[4];
    paint_block(ex + 4, bx + W - kBorder, ey, bx, by, W, H, 1, 4, true,
                c565(C.status_bg), c565(C.base));
    // 页码（6x10）：距标签左缘 4px（rc107 左移 1px 后按用户要求再右移 3px），
    // 偏行下部（ink 落 ey+8..ey+14）
    term::draw_text610(ex + 4, ey + 7, e.s, C.status_fg, C.status_bg, true);
    if (st.pages > 1) {
      // 白色 TAB 小片：左上/右下阶梯雕角（4/2/1/1 与镜像）
      static const int kTL[7] = {4, 2, 1, 1, 0, 0, 0};
      static const int kBR[7] = {0, 0, 0, 1, 1, 2, 4};
      for (int r = 0; r < 7; r++) {
        const int xa = ex + kTL[r], xb = ex + 24 - kBR[r];
        if (xb > xa) term::ov_dst().fillRect(xa, ey + r, xb - xa, 1, c565(C.text));
      }
      term::draw_text4x7(ex + 6, ey, "TAB", C.base, C.text, false);
    }
  }

  // ④ 外边框（r10：外 r + 内 r-1 + 内 r-2）
  stroke_round_rect_w2(bx, by, W, H, kR, c565(C.border));
  }
  term::ov_end();                     // 画布按透明度比较混合上屏（快速路径为 no-op）
}

}  // namespace

// 覆盖区域跟踪：盒区与编辑行分别记账（编辑行矩形恒到屏幕右缘，混在一起会掩盖
// 框宽变化）；变化/消失或编辑行文本/位置变化时标脏旧行，下一拍合帧擦残影。
static int s_last_box[4] = {0, 0, 0, 0};
static bool s_last_box_valid = false;
static std::string s_last_pre;
static int s_last_pre_y = -1;

void paint()
{
  ime::UiState st;
  ime::ui_state(&st);

  int col = 0, row = 0;
  term::cursor_cell(&col, &row);
  const int pre_y = row * term::kCellH;

  int br[4] = {0, 0, 0, 0};
  const bool box_now = st.on && st.box_visible;

  // 残影判定（绘制前）
  const bool pre_visible = st.on && st.text_visible;
  const bool pre_changed =
      (pre_visible && (st.preedit != s_last_pre || pre_y != s_last_pre_y)) ||
      (!pre_visible && !s_last_pre.empty());

  // 绘制（box 区域由 paint_box 回填 br）
  const Colors C = colors();
  if (pre_visible) {
    term::draw_text_ink_win(col * term::kCellW, pre_y,
                            st.preedit.c_str(), C.reg7, 0);
  }
  if (box_now) paint_box(st, C, br);

  // 标脏旧区（下一拍合帧恢复终端内容）；br 已回填后再比
  const bool box_changed = s_last_box_valid &&
      (!box_now || br[0] != s_last_box[0] || br[1] != s_last_box[1] ||
       br[2] != s_last_box[2] || br[3] != s_last_box[3]);
  if (pre_changed && s_last_pre_y >= 0)
    term::mark_pixel_rows(s_last_pre_y, s_last_pre_y + term::kCellH);
  if (pre_changed && pre_visible)
    term::mark_pixel_rows(pre_y, pre_y + term::kCellH);
  if (box_changed) {
    term::mark_pixel_rows(s_last_box[1], s_last_box[3]);
    if (box_now) term::mark_pixel_rows(br[1], br[3]);
  }

  for (int i = 0; i < 4; i++) s_last_box[i] = br[i];
  s_last_box_valid = box_now;
  s_last_pre = pre_visible ? st.preedit : std::string();
  s_last_pre_y = pre_visible ? pre_y : -1;
}

void attach() { term::set_overlay_hook(paint); }
void detach() { term::set_overlay_hook(nullptr); }

}  // namespace ime_ui
