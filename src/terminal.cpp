// terminal.cpp — 超简易终端模拟器（fbcon 实现逻辑的用户态复现，基于 LovyanGFX）
//
// fbcon（drivers/video/fbdev/core/fbcon.c）→ 本实现的对应关系：
//   vc_data.screenbuf（u16 字符+属性）   → Cell{ ch, fg, bg, bg_def } 屏缓冲
//   fbcon_putcs() 按属性取 fg/bg 画一行 → draw_cell() 画单格（背景 + 字形）
//   fbcon_clear() 区域擦除               → clear_region()（按当前属性填空格）
//   fbcon_scroll() 软滚动（行块搬移）    → scroll_up()（入回滚缓冲 + memmove）
//   fbcon_cursor() 光标                  → present() 最上层画 cursor_paint()
//   属性字（fg/bg nibble）+ SGR          → ANSI 解析器写 cur_fg/cur_bg（RGB565）
//
// 布局：默认 53 列 × 17 行（6×10 单元）；小字体 80 列 × 24 行（4×7 单元）。
//       网格 = 屏宽/高 ÷ 单元尺寸，单元尺寸/字体表随窗口字号（4x7 / 6x10 / 8x14）。
// 底图画布 320×170@16bit：背景色/背景图缩放填充；单元格背景为"默认"时透出
// 画布（内存拷贝上屏），ANSI 显式设置背景色时优先显示该色（图片不透出）。
// 刷新为纯内存写（Panel_fb display() 不等屏幕传输），上屏由内核 deferred-io 完成。
//
// 上屏模型 = 脏行合帧（fbterm/现代终端的 damage 思路）：
//   解析层只写屏缓冲 T->s_buf 并标脏行，绝不逐字符画像素；
//   present() 把攒下的脏行一次性画出（每行先贴底图带，再背景色块，再字形，最后光标）。
//   节流：批量输出时至多每 kPresentMs(16ms) 上屏一次（输出暂停/缓冲空时立即上屏）。
//   这样"海量字符"输出的解析只花内存写的时间，绘制次数与帧率挂钩而与字符数无关。
#include "terminal.h"

#include "ui.h"          // extern LGFX tft
#include "term_font.h"        // 6x10：glcdfont ASCII + BoutiqueBitmap9x9（生成物）
#include "term_font_small.h"  // 4x7 ：手绘 ASCII + ChillBitmap 8px（生成物）
#include "term_font_8x14.h"   // 8x14：手绘西文 + 文泉驿点阵宋体 14px（生成物）

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>
#include <signal.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <malloc.h>

namespace term {

// 前置声明：定义在下方公共区（REP 重放/插入模式等解析器内部需要）
void put_raw(char c);
void put_tofu(int w);
void put_glyph(uint16_t cp, int w);
static unsigned char acs_ascii(unsigned char c);
static int cp_width(uint32_t cp);      // 前置：toast 排版判字体表外码点宽度（定义在下方）


// 底图画布 = 整个屏幕 320×170（网格由字体包围框计算，右侧/底部余量由画布覆盖）
constexpr int kCanvasW = 320;
constexpr int kCanvasH = 170;

// 运行时网格/单元尺寸（当前激活窗口；见 apply_grid / activate）
int kCols = 53, kRows = 17, kCellW = 6, kCellH = 10;

// 当前字体表（由激活窗口的字号决定）
static const termfont::FontTable* s_font = &termfont::font_6x10;

// font_size → 单元尺寸 + 字体表：
//   4 → 4x7（小字体表）
//   6 → 6x10（默认表）
//   8 → 8x14（文泉驿点阵宋体 14px + 手绘西文表）
static void font_dims(int size, int& cw, int& ch, const termfont::FontTable** ft)
{
  switch (size) {
    case 4:  cw = 4;  ch = 7;  *ft = &termfont::font_4x7;  break;
    case 8:  cw = 8;  ch = 14; *ft = &termfont::font_8x14; break;
    default: cw = 6;  ch = 10; *ft = &termfont::font_6x10; break;
  }
}

static uint32_t dirty_all_mask()
{
  return kRows >= 32 ? 0xFFFFFFFFu : ((1u << kRows) - 1);
}

// 非 ASCII 占位"豆腐块"伪字符（不会来自普通输入）；
// kChTofuR 同时是"双宽字符右半"的延续标记（真实宽字形的右格也用它）
enum { kChTofu = 1, kChTofuL = 2, kChTofuR = 3 };

// ── 行存储（0.5.0 重构）───────────────────────────────────────────────
// 颜色统一 4 字节条目：(RGB888 3B；第 4 字节 = 透明标志(bit7) | 重复次数(bit6-0))。
// 每行每通道独立取策略（元数据 fg_mode/bg_mode）：
//   uniform —— 行内单色，union 内联 4 字节颜色；
//   array   —— 混色行，union 存指针 → RLE 数组（4B/条，只做行程编码，不识别
//               重复序列；退化膨胀可接受；行宽 ≤80 < 128，单条即够）。
// 行按窗口宽度分三种数据类型（40/53/80 字符），除字符数组长度外完全一致；
// 字符为 Unicode BMP 码点（uint16，含伪字符 kChTofu*）。
// 行释放时颜色数组自动释放（析构）。

// 4B 颜色条目（uniform 内联值与 RLE run 同格式）：
//   低 3 字节 = RGB888；高 1 字节 = 透明标志(bit7) | 重复次数(bit6-0)
// 取色用掩码（& 0x80FFFFFF）避免移位；数量读取用 >>24。
typedef uint32_t ColorRun;
static_assert(sizeof(ColorRun) == 4, "ColorRun must be 4 bytes");

enum : uint8_t { COL_UNIFORM = 0, COL_ARRAY = 1 };

// 颜色 4 字节格式（行首/RLE 统一）：Axxxxxxx RRRRRRRR GGGGGGGG BBBBBBBB
//   A=0：直接 RGB888；
//   A=1：fg → B = OSC4 调色板索引（R/G 无意义）；
//        bg → R=G=0 时 B = 调色板索引；R/G 有非 0 视为透明（写 0x80FFFFFF）。
//   RLE 条目中 xxxxxxx = 重复次数（1..127）；行首颜色字段该 7 位为 0。
static inline uint32_t mk_col(int r, int g, int b, bool trans)
{
  if (trans) return 0x80FFFFFFu;                       // 透明背景规范值
  return (uint32_t)(((r & 0xff) << 16) | ((g & 0xff) << 8) | (b & 0xff));
}
static inline uint32_t mk_pal(int idx)   { return 0x80000000u | (uint32_t)(idx & 0xff); }
static inline bool     is_trans_bg(uint32_t w)
{
  return (w & 0x80000000u) && (((w >> 16) & 0xff) | ((w >> 8) & 0xff));
}
static inline bool     is_pal_ref(uint32_t w) { return (w & 0x80000000u) && !is_trans_bg(w); }
static inline int      pal_idx(uint32_t w)   { return (int)(w & 0xff); }
static inline uint32_t run_color(ColorRun w) { return w & 0x80FFFFFFu; }   // 颜色+透明标志
static inline int      run_cnt (ColorRun w) { return (int)((w >> 24) & 0x7f); }
static inline bool     run_trans(ColorRun w){ return is_trans_bg(w); }
static inline ColorRun mk_run(uint32_t col, int cnt)
{
  return (col & 0x80FFFFFFu) | ((uint32_t)(cnt & 0x7f) << 24);
}
static inline ColorRun run_with_cnt(ColorRun w, int cnt)
{
  return (w & 0x80FFFFFFu) | ((uint32_t)(cnt & 0x7f) << 24);
}

// 单通道颜色流的编辑/解码（fg/bg 共用同一 4B 条目格式）
struct ChanRef {
  uint8_t&   mode;
  uint32_t&  uni;
  ColorRun*& arr;
  uint16_t&  nruns;
  uint16_t&  hint;
};

static void chan_delete(ChanRef c, int i)
{
  if (i + 1 < c.nruns)
    memmove(&c.arr[i], &c.arr[i + 1], (size_t)(c.nruns - i - 1) * sizeof(ColorRun));
  c.nruns--;
}

static void chan_set(ChanRef c, int x, int cols, uint32_t col)
{
  col &= 0x80FFFFFFu;
  if (c.mode == COL_UNIFORM) {
    if (c.uni == col) return;
    ColorRun* a = (ColorRun*)malloc(sizeof(ColorRun) * 3);
    if (a == nullptr) return;                          // OOM：保持 uniform，不写空指针
    int n = 0;
    if (x > 0)               a[n++] = mk_run(c.uni, x);
    a[n++] = mk_run(col, 1);
    if (x < cols - 1)        a[n++] = mk_run(c.uni, cols - 1 - x);
    c.arr = a; c.nruns = (uint16_t)n; c.hint = 0; c.mode = COL_ARRAY;
    return;
  }
  int acc = 0, i = 0, start = 0;
  int h = (c.hint < c.nruns) ? c.hint : 0;
  for (int j = 0; j < h; j++) acc += run_cnt(c.arr[j]);   // hint 前的列偏移
  for (int k = 0; k < c.nruns; k++) {
    int idx = (h + k) % c.nruns;
    int n = run_cnt(c.arr[idx]);
    if (x < acc + n) { i = idx; start = acc; break; }
    acc += n;
    if (idx == c.nruns - 1) acc = 0;                      // 回绕归零
  }
  ColorRun run = c.arr[i];
  if (run_color(run) == col) { c.hint = (uint16_t)i; return; }
  const int off = x - start;
  const uint32_t oc = run_color(run);
  // 邻 run 同色：直接并入（O(1) 顺序写快路径）
  if (off == 0 && i > 0 && run_color(c.arr[i - 1]) == col && run_cnt(c.arr[i - 1]) < 127) {
    c.arr[i - 1] = run_with_cnt(c.arr[i - 1], run_cnt(c.arr[i - 1]) + 1);
    c.arr[i] = run_with_cnt(run, run_cnt(run) - 1);
    if (run_cnt(c.arr[i]) == 0) chan_delete(c, i);
    c.hint = (uint16_t)(i > 0 ? i - 1 : 0);
    return;
  }
  if (off == run_cnt(run) - 1 && i + 1 < c.nruns && run_color(c.arr[i + 1]) == col &&
      run_cnt(c.arr[i + 1]) < 127) {
    c.arr[i + 1] = run_with_cnt(c.arr[i + 1], run_cnt(c.arr[i + 1]) + 1);
    c.arr[i] = run_with_cnt(run, run_cnt(run) - 1);
    if (run_cnt(c.arr[i]) == 0) chan_delete(c, i);
    c.hint = (uint16_t)(i + 1 < c.nruns ? i + 1 : 0);
    return;
  }
  // 拆分 run i → [left(oc)][1(col)][right(oc)]，最多 +2 条
  const int left = off, right = run_cnt(run) - off - 1;
  ColorRun pieces[3];
  int np = 0;
  if (left > 0)  pieces[np++] = mk_run(oc, left);
  pieces[np++] = mk_run(col, 1);
  if (right > 0) pieces[np++] = mk_run(oc, right);
  ColorRun* a = (ColorRun*)realloc(c.arr, sizeof(ColorRun) * (c.nruns + 2));
  if (a == nullptr) return;
  const_cast<ColorRun*&>(c.arr) = a;
  // run i 被 pieces 替换（np 条替换 1 条，delta = np-1 ∈ [0,2]）
  if (np != 1)
    memmove(&a[i + np], &a[i + 1], (size_t)(c.nruns - i - 1) * sizeof(ColorRun));
  for (int k = 0; k < np; k++) a[i + k] = pieces[k];
  c.nruns = (uint16_t)(c.nruns + (np - 1));
  // 合并左右邻同色碎片
  int last = i + np - 1;
  if (last + 1 < c.nruns && run_color(a[last]) == run_color(a[last + 1]) &&
      run_cnt(a[last]) + run_cnt(a[last + 1]) <= 127) {
    a[last] = run_with_cnt(a[last], run_cnt(a[last]) + run_cnt(a[last + 1]));
    chan_delete(c, last + 1);
  }
  if (i > 0 && run_color(a[i - 1]) == run_color(a[i]) &&
      run_cnt(a[i - 1]) + run_cnt(a[i]) <= 127) {
    a[i - 1] = run_with_cnt(a[i - 1], run_cnt(a[i - 1]) + run_cnt(a[i]));
    chan_delete(c, i);
  }
  c.hint = (uint16_t)(i < c.nruns ? i : 0);
}

static void chan_decode(ChanRef c, uint32_t* out, int cols)
{
  if (c.mode == COL_UNIFORM) { for (int x = 0; x < cols; x++) out[x] = c.uni; return; }
  int k = 0;
  for (int i = 0; i < c.nruns && k < cols; i++) {
    uint32_t col = run_color(c.arr[i]);
    int n = run_cnt(c.arr[i]);
    while (n-- > 0 && k < cols) out[k++] = col;
  }
  while (k < cols) out[k++] = c.uni;
}

static void chan_build(ChanRef c, const uint32_t* in, int cols)
{
  // 从逐格颜色重建 RLE（归档 hist_push_live / rewrap 都走此路径，须 O(cols) 且正确）
  if (c.mode == COL_ARRAY) { free(c.arr); c.arr = nullptr; }
  c.arr = nullptr; c.nruns = 0; c.hint = 0;
  if (cols <= 0) { c.mode = COL_UNIFORM; c.uni = 0; return; }
  // 第一遍：数 run 数（每 run ≤127，超长自动拆分成多条同色 run）
  int nr = 0;
  for (int i = 0; i < cols; ) {
    int j = i + 1;
    while (j < cols && in[j] == in[i] && j - i < 127) j++;
    nr++;
    i = j;
  }
  if (nr == 1) {                                   // 整行同色 → UNIFORM
    c.mode = COL_UNIFORM; c.uni = in[0] & 0x80FFFFFFu;
    return;
  }
  ColorRun* a = (ColorRun*)malloc(sizeof(ColorRun) * (size_t)nr);
  if (a == nullptr) { c.mode = COL_UNIFORM; c.uni = in[0] & 0x80FFFFFFu; return; }
  int k = 0;
  for (int i = 0; i < cols; ) {
    int j = i + 1;
    while (j < cols && in[j] == in[i] && j - i < 127) j++;
    a[k++] = mk_run(in[i], j - i);                 // 每 run 覆盖 [i,j)，颜色/透明位保留
    i = j;
  }
  c.arr = a; c.nruns = (uint16_t)k; c.hint = 0; c.mode = COL_ARRAY;
}

static void chan_copy(ChanRef d, ChanRef s)
{
  if (d.mode == COL_ARRAY) { free(d.arr); const_cast<ColorRun*&>(d.arr) = nullptr; }
  const_cast<uint8_t&>(d.mode) = s.mode;
  if (s.mode == COL_ARRAY) {
    ColorRun* a = (ColorRun*)malloc(sizeof(ColorRun) * (s.nruns ? s.nruns : 1));
    if (a == nullptr) {                             // OOM：退化为 uniform，避免 memcpy(null) 崩溃
      const_cast<uint8_t&>(d.mode) = COL_UNIFORM;
      const_cast<uint32_t&>(d.uni) = s.nruns ? run_color(s.arr[0]) : 0u;
      const_cast<uint16_t&>(d.nruns) = 0;
      return;
    }
    memcpy(a, s.arr, (size_t)s.nruns * sizeof(ColorRun));
    const_cast<ColorRun*&>(d.arr) = a;
    const_cast<uint16_t&>(d.nruns) = s.nruns;
  } else {
    const_cast<uint32_t&>(d.uni) = s.uni;
    const_cast<uint16_t&>(d.nruns) = 0;
  }
  const_cast<uint16_t&>(d.hint) = 0;
}

struct RowB {                       // 行公共部分（宽度无关；方法经 ch/cols 工作）
  uint16_t* ch;                     // → 本行内联字符数组
  uint16_t  cols;
  union { uint32_t uni; ColorRun* arr; } fg;   // 前景: 4B 颜色 / RLE 数组
  union { uint32_t uni; ColorRun* arr; } bg;   // 背景: 4B 颜色 / RLE 数组
  uint8_t  fg_mode, bg_mode;        // 颜色策略（COL_UNIFORM/COL_ARRAY）
  uint8_t  flags;                   // bit0: wrapped（软换行，rewrap 用）
  uint8_t  pad_;
  uint16_t fg_runs, bg_runs;        // 数组模式 run 条数
  uint16_t hint_fg, hint_bg;        // run 查找提示（顺序写加速）

  ChanRef fgc() { return ChanRef{fg_mode, fg.uni, fg.arr, fg_runs, hint_fg}; }
  ChanRef bgc() { return ChanRef{bg_mode, bg.uni, bg.arr, bg_runs, hint_bg}; }
  ChanRef fgc() const { return ChanRef{const_cast<uint8_t&>(fg_mode), const_cast<uint32_t&>(fg.uni), const_cast<ColorRun*&>(fg.arr), const_cast<uint16_t&>(fg_runs), const_cast<uint16_t&>(hint_fg)}; }
  ChanRef bgc() const { return ChanRef{const_cast<uint8_t&>(bg_mode), const_cast<uint32_t&>(bg.uni), const_cast<ColorRun*&>(bg.arr), const_cast<uint16_t&>(bg_runs), const_cast<uint16_t&>(hint_bg)}; }

  void release()
  {
    if (fg_mode == COL_ARRAY) free(fg.arr);
    if (bg_mode == COL_ARRAY) free(bg.arr);
    fg_mode = bg_mode = COL_UNIFORM;
    fg.uni = mk_col(255, 255, 255, false);
    bg.uni = mk_col(0, 0, 0, true);
    fg_runs = bg_runs = hint_fg = hint_bg = 0;
  }
  void copy_from(const RowB& o)     // 同宽深拷贝
  {
    memcpy(ch, o.ch, (size_t)cols * sizeof(uint16_t));
    flags = o.flags;
    chan_copy(fgc(), o.fgc());
    chan_copy(bgc(), o.bgc());
  }
  void move_from(RowB& o)           // 同宽移动（窃取数组）
  {
    release();
    memcpy(ch, o.ch, (size_t)cols * sizeof(uint16_t));
    flags = o.flags;
    fg_mode = o.fg_mode; fg = o.fg; fg_runs = o.fg_runs;
    bg_mode = o.bg_mode; bg = o.bg; bg_runs = o.bg_runs;
    hint_fg = hint_bg = 0;
    o.fg_mode = o.bg_mode = COL_UNIFORM;
    o.fg.uni = mk_col(255, 255, 255, false); o.bg.uni = mk_col(0, 0, 0, true);
    o.fg_runs = o.bg_runs = 0;
  }
  void put(int x, uint16_t c, uint32_t fg4, uint32_t bg4)  // 写字符 + 颜色
  {
    ch[x] = c;
    set_colors(x, fg4, bg4);
  }
  void set_colors(int x, uint32_t fg4, uint32_t bg4)   // 单格写色（热路径）
  {
    chan_set(fgc(), x, cols, fg4);
    chan_set(bgc(), x, cols, bg4);
  }
  void fill_span(int x0, int n, uint16_t c, uint32_t fg4, uint32_t bg4)
  {
    for (int x = x0; x < x0 + n && x < cols; x++) {
      ch[x] = c;
      chan_set(fgc(), x, cols, fg4);
      chan_set(bgc(), x, cols, bg4);
    }
  }
  void decode(uint32_t* fg_out, uint32_t* bg_out) const   // 上屏展开
  {
    chan_decode(fgc(), fg_out, cols);
    chan_decode(bgc(), bg_out, cols);
  }
};

template<int COLS>
struct Row : RowB {
  uint16_t ch_[COLS];               // 字符数组，静态分配（每行 80/106/160 B）
  void init() {
    ch = ch_; cols = COLS; flags = 0;
    fg_mode = bg_mode = COL_UNIFORM;
    fg.uni = mk_col(255, 255, 255, false);
    bg.uni = mk_col(0, 0, 0, true);
    fg_runs = bg_runs = hint_fg = hint_bg = 0;
    for (int i = 0; i < COLS; i++) ch_[i] = ' ';
  }
  Row() { init(); }
  Row(const Row& o) { init(); copy_from(o); }
  Row(Row&& o) noexcept { init(); move_from(o); }
  Row& operator=(const Row& o) { copy_from(o); return *this; }
  Row& operator=(Row&& o) noexcept { move_from(o); return *this; }
  ~Row() { release(); }
};

// 三种宽度的窗口存储（除字符数组长度外完全一致）
// 活动区两块：数据区（字符 Row）+ 颜色区（fg/bg 各自独立数组，逐格完整
// 颜色、解压态）。TUI 写格子零压缩；满屏滚动用环头（head）推进代替搬移。
template<int COLS>
struct Grid {
  Row<COLS> s_buf[kMaxRows];          // 数据区：屏幕字符（环形，head 寻址）
  Row<COLS> s_saved_buf[kMaxRows];    // 备用屏字符（?1049）
  std::deque<Row<COLS>> s_hist;
  uint32_t cf[kMaxRows][COLS];        // 颜色区 fg（解压态，环形）
  uint32_t cb[kMaxRows][COLS];        // 颜色区 bg（解压态，环形）
  uint32_t sf[kMaxRows][COLS];        // 备用屏保存 fg
  uint32_t sb[kMaxRows][COLS];        // 备用屏保存 bg
  int head = 0;                       // 环头：屏幕行 y → 槽位 (head+y) % rows
  Grid()
  {
    const uint32_t fg0 = mk_col(255, 255, 255, false), bg0 = mk_col(0, 0, 0, true);
    for (int y = 0; y < kMaxRows; y++)
      for (int x = 0; x < COLS; x++) {
        cf[y][x] = fg0; cb[y][x] = bg0;
        sf[y][x] = fg0; sb[y][x] = bg0;          // A1：备用屏保存色必须初始化（否则 restore 读到未初始化内存）
      }
  }
};

// 活动行视图：数据区(字符/flags) + 颜色区(fg/bg 逐格) 统一读写
struct LiveRowRef {
  RowB*     row;
  uint16_t* ch;
  uint8_t&  flags;
  uint32_t* fg;
  uint32_t* bg;
  LiveRowRef(RowB* r, uint32_t* f, uint32_t* b)
    : row(r), ch(r->ch), flags(r->flags), fg(f), bg(b) {}
  void put(int x, uint16_t c, uint32_t f, uint32_t b)   // TUI 写格子：零压缩
  { ch[x] = c; fg[x] = f; bg[x] = b; }
  void fill_span(int x0, int n, uint16_t c, uint32_t f, uint32_t b)
  {
    for (int x = x0; x < x0 + n && x < row->cols; x++) { ch[x] = c; fg[x] = f; bg[x] = b; }
  }
  void move_from(LiveRowRef o)                           // 区域滚动用（同宽）
  {
    memcpy(ch, o.ch, (size_t)row->cols * sizeof(uint16_t));
    memcpy(fg, o.fg, (size_t)row->cols * sizeof(uint32_t));
    memcpy(bg, o.bg, (size_t)row->cols * sizeof(uint32_t));
    flags = o.flags;
  }
};

// ── 每窗口终端状态（0.3.0 多窗口）────────────────────────────────
// 除背景画布（母版 s_canvas_master 全局共享；s_canvas 为每窗独立拷贝，非共享）
// 与子进程登记（s_child_pid）外，全部可变终端状态都在此结构内，一窗口一份；
// T = 当前激活状态。

enum class St { Normal, Esc, EscMid, Csi, Osc, OscEsc, Skip, SkipEsc };

struct State {
  int      s_kind = 1;                     // 行类型: 0=40列(8x14) 1=53列(6x10) 2=80列(4x7)
  std::shared_ptr<void> s_grid;         // Grid<40|53|80>*（三宽度之一，自定义删除器 RAII）
  int      s_hist_max = 10000;
  int      s_view = 0;                 // 回滚视图偏移（0=跟随底部）
  int      s_view_anchor = -1;         // 翻页锚点：视图首行绝对索引（-1=未锚定）
  int      s_view_hold_ms = 2000;      // 手动翻页后不跳底的保持时长（config）
  uint64_t s_view_hold_until = 0;      // 保持截止时刻（距上次翻页键 + hold_ms）
  bool     s_view_pending = false;     // 保持期内发生了真实滚屏（到点跳底）
  int      s_col = 0, s_row = 0;
  uint32_t s_fg = 0xFFFFFFu;
  int      s_fg_idx = 7;
  bool     s_bold = false;
  uint32_t s_bg = 0x000000u;
  bool     s_bg_def = true;
  bool     s_rev = false;            // SGR 7 反显开关（写格子时交换 fg/bg）
  uint8_t  s_g0 = 0, s_g1 = 0;       // 字符集：0=ASCII, 1=DEC 特殊图形（ACS 框线）
  bool     s_so = false;             // SO/SI：GL 当前用 G1 还是 G0
  uint8_t  s_esc_delim = 0;          // ESC ( / ESC ) 之后待处理的设计符
  uint32_t s_def_fg = 0x00FFFFFFu;    // 默认前景 RGB888（= regular7；OSC 10 可改）
  uint32_t s_def_bg = 0x000000u;      // 默认背景 RGB888（= background_color；OSC 11 可改）
  uint32_t s_def_fg0 = 0x00FFFFFFu;   // 配置默认前景快照（OSC 110 复位用；OSC 10 只改 s_def_fg）
  uint32_t s_def_bg0 = 0x000000u;     // 配置默认背景快照（OSC 111 复位用；OSC 11 只改 s_def_bg）
  uint32_t s_cursor_col = 0x00FFFFFFu;// 光标颜色（config cursor；OSC 12 可改）
  uint32_t s_cursor_col_def = 0x00FFFFFFu; // 配置默认光标色快照（OSC 112/RIS/DECSTR/退出复位用）
  std::unique_ptr<uint32_t[]> s_pal4;   // OSC4 动态调色板 256×RGB888（懒分配/每窗口独立/RAII）
  int      s_cols = kMaxCols, s_rows = kMaxRows;   // 本窗口网格（按字号）
  int      s_cell_w = 6, s_cell_h = 10;
  int      s_font_size = 6;
  const termfont::FontTable* s_font = nullptr;
  bool     s_save_rev = false;
  uint32_t s_pal[16] = {};
  uint32_t s_dirty = 0;
  uint64_t s_last_present = 0;
  int      s_cur_drawn_row = -1;   // 屏上实际画着光标块的格（-1=没画）——不变量见 cursor_paint
  int      s_cur_drawn_col = -1;
  int      s_bl_r = -1, s_bl_c = -1;   // 闪烁锚点：上次复位时光标位置（变化即复位闪烁）
  bool     s_bl_vis = false;           // 闪烁锚点：上次复位时显隐（变化即复位闪烁）
  int      s_shown_view = 0;
  bool     s_cursor_on = true;
  bool     s_cursor_vis = true;
  int      s_cur_style = 1;
  bool     s_cursor_blink = true;
  uint64_t s_cursor_t0 = 0;
  int      s_save_row = 0, s_save_col = 0;
  uint32_t s_save_fg = 0, s_save_bg = 0;
  bool     s_save_bgdef = true;
  int      s_save_fgidx = 7;
  bool     s_save_bold = false;
  bool     s_save_wrp = false;         // DECSC 保存延迟环绕挂起态（STD-070）
  int      s_top = 0, s_bot = kRows - 1;
  bool     s_autowrap = true;
  bool     s_wrap_pending = false;
  bool     s_app_cursor = false;
  bool     s_insert_mode = false;
  bool     s_tabs[kMaxCols] = {};
  uint32_t s_last_print = 0;
  int      s_last_print_w = 1;

  bool     s_alt_active = false;
  int      s_saved_row = 0, s_saved_col = 0;
  Writer   s_writer = nullptr;         // 终端回复通道
  St       s_st = St::Normal;          // ANSI 解析状态机
  char     s_csi[64] = {};
  int      s_csi_len = 0;
  uint32_t s_u8_cp = 0;
  int      s_u8_need = 0;
  std::unique_ptr<uint16_t[]> s_canvas; // 每窗口底图画布（320×170 RGB565，非共享/RAII）
  char      s_osc[8192] = {};          // OSC 载荷缓冲（OSC52 剪贴板 base64 需要较大空间）
  int       s_osc_len = 0;
  uint32_t s_kkp_flags = 0;            // kitty 键盘协议增强标志
  uint32_t s_kkp_stack[8] = {};
  int      s_kkp_sp = 0;
};

// 三宽度 Grid 访问器（Row<COLS> 除字符数组长度外完全一致，统一走 RowB 接口）
template<int COLS> using G = Grid<COLS>;
static inline LiveRowRef buf_row(State* s, int y)   // 环形寻址: (head+y) % rows
{
  const int rows = s->s_rows;
  if (y < 0) y = 0;                                  // 防御性钳位（CSI 越界参数兜底）
  if (y >= rows) y = rows - 1;
  switch (s->s_kind) {
    case 0: { G<40>* g = (G<40>*)s->s_grid.get(); int k = (g->head + y) % rows;
              return LiveRowRef(&g->s_buf[k], g->cf[k], g->cb[k]); }
    case 2: { G<80>* g = (G<80>*)s->s_grid.get(); int k = (g->head + y) % rows;
              return LiveRowRef(&g->s_buf[k], g->cf[k], g->cb[k]); }
    default:{ G<53>* g = (G<53>*)s->s_grid.get(); int k = (g->head + y) % rows;
              return LiveRowRef(&g->s_buf[k], g->cf[k], g->cb[k]); }
  }
}
static inline void grid_head(State* s, int delta)   // 满屏滚动 = 环头推进（零搬移）
{
  const int rows = s->s_rows;
  switch (s->s_kind) {
    case 0: { G<40>* g = (G<40>*)s->s_grid.get(); g->head = (g->head + delta % rows + rows) % rows; break; }
    case 2: { G<80>* g = (G<80>*)s->s_grid.get(); g->head = (g->head + delta % rows + rows) % rows; break; }
    default:{ G<53>* g = (G<53>*)s->s_grid.get(); g->head = (g->head + delta % rows + rows) % rows; break; }
  }
}
static inline uint32_t* saved_fg(State* s, int y)
{
  switch (s->s_kind) {
    case 0:  return ((G<40>*)s->s_grid.get())->sf[y];
    case 2:  return ((G<80>*)s->s_grid.get())->sf[y];
    default: return ((G<53>*)s->s_grid.get())->sf[y];
  }
}
static inline uint32_t* saved_bg(State* s, int y)
{
  switch (s->s_kind) {
    case 0:  return ((G<40>*)s->s_grid.get())->sb[y];
    case 2:  return ((G<80>*)s->s_grid.get())->sb[y];
    default: return ((G<53>*)s->s_grid.get())->sb[y];
  }
}
static inline RowB& saved_row(State* s, int y)
{
  switch (s->s_kind) {
    case 0:  return ((G<40>*)s->s_grid.get())->s_saved_buf[y];
    case 2:  return ((G<80>*)s->s_grid.get())->s_saved_buf[y];
    default: return ((G<53>*)s->s_grid.get())->s_saved_buf[y];
  }
}
static inline RowB& hist_row(State* s, int j)
{
  switch (s->s_kind) {
    case 0:  return ((G<40>*)s->s_grid.get())->s_hist[j];
    case 2:  return ((G<80>*)s->s_grid.get())->s_hist[j];
    default: return ((G<53>*)s->s_grid.get())->s_hist[j];
  }
}
static inline int hist_size(State* s)
{
  switch (s->s_kind) {
    case 0:  return (int)((G<40>*)s->s_grid.get())->s_hist.size();
    case 2:  return (int)((G<80>*)s->s_grid.get())->s_hist.size();
    default: return (int)((G<53>*)s->s_grid.get())->s_hist.size();
  }
}
// 归档：活动行（解压色）→ 历史行（字符 + RLE 压缩颜色，仅此一次压缩）
static void hist_push_live(State* s, LiveRowRef r)
{
  RowB* h = nullptr;
  switch (s->s_kind) {
    case 0: { G<40>* g = (G<40>*)s->s_grid.get(); g->s_hist.emplace_back(); h = &g->s_hist.back(); break; }
    case 2: { G<80>* g = (G<80>*)s->s_grid.get(); g->s_hist.emplace_back(); h = &g->s_hist.back(); break; }
    default:{ G<53>* g = (G<53>*)s->s_grid.get(); g->s_hist.emplace_back(); h = &g->s_hist.back(); break; }
  }
  memcpy(h->ch, r.ch, (size_t)h->cols * sizeof(uint16_t));
  h->flags = r.flags;
  ChanRef fc{h->fg_mode, h->fg.uni, h->fg.arr, h->fg_runs, h->hint_fg};
  ChanRef bc{h->bg_mode, h->bg.uni, h->bg.arr, h->bg_runs, h->hint_bg};
  chan_build(fc, r.fg, h->cols);
  chan_build(bc, r.bg, h->cols);
}
static inline void hist_pop_front(State* s)
{
  switch (s->s_kind) {
    case 0:  ((G<40>*)s->s_grid.get())->s_hist.pop_front(); break;
    case 2:  ((G<80>*)s->s_grid.get())->s_hist.pop_front(); break;
    default: ((G<53>*)s->s_grid.get())->s_hist.pop_front(); break;
  }
}
static inline void hist_push_blank(State* s)
{
  switch (s->s_kind) {
    case 0:  ((G<40>*)s->s_grid.get())->s_hist.emplace_back(); break;
    case 2:  ((G<80>*)s->s_grid.get())->s_hist.emplace_back(); break;
    default: ((G<53>*)s->s_grid.get())->s_hist.emplace_back(); break;
  }
}
static inline void hist_clear(State* s)
{
  switch (s->s_kind) {
    case 0:  ((G<40>*)s->s_grid.get())->s_hist.clear(); break;
    case 2:  ((G<80>*)s->s_grid.get())->s_hist.clear(); break;
    default: ((G<53>*)s->s_grid.get())->s_hist.clear(); break;
  }
}

State* T = nullptr;                    // 当前激活的窗口状态
void break_wide(int x, int y);          // 前置：清除/擦除路径拆宽字对（定义在下方 term 作用域）

// 按字号设置某窗口的网格/字体；激活窗口同步更新全局运行时值。
static void apply_grid(State* s, int font_size)
{
  int cw, ch;
  const termfont::FontTable* ft = nullptr;
  font_dims(font_size, cw, ch, &ft);
  s->s_kind = (font_size == 4) ? 2 : (font_size == 8 ? 0 : 1);   // 80/40/53 列
  int cols = kCanvasW / cw;
  int rows = kCanvasH / ch;
  if (cols < 1) cols = 1;
  if (rows < 1) rows = 1;
  if (cols > kMaxCols) cols = kMaxCols;
  if (rows > kMaxRows) rows = kMaxRows;
  s->s_font_size = font_size;
  s->s_cell_w = cw; s->s_cell_h = ch;
  s->s_cols = cols; s->s_rows = rows;
  s->s_top = 0; s->s_bot = rows - 1;   // 新网格=全屏滚动域（State 构造默认取旧全局 kRows，必须在此校正）
  s->s_font = ft;
  if (T == s) {
    kCellW = cw; kCellH = ch; kCols = cols; kRows = rows; s_font = ft;
  }
}

static bool s_quiet = false;           // 静默解析（后台窗口批量解析时置位）
static bool s_full_line_nl = false;      // 满宽行+LF 语义（config full_line_newline）
void set_full_line_newline(bool on) { s_full_line_nl = on; }

namespace {

std::unique_ptr<uint16_t[]> s_canvas_master; // 底图画布主副本（只读模板，各窗口独立拷贝，RAII）
pid_t    s_child_pid = 0;

// ── 脏行合帧（damage + frame coalescing）─────────────────────────
constexpr uint64_t kPresentMs = 16;  // 批量输出合帧周期（≈60fps 上限）

void mark_row(int y)  { if (y >= 0 && y < kRows) T->s_dirty |= 1u << y; }
void mark_rows(int y0, int y1) { for (int y = y0; y < y1; y++) mark_row(y); }
void mark_all()       { T->s_dirty = dirty_all_mask(); }

// ── 颜色 ──────────────────────────────────────────────────────────
uint64_t now_ms_()
{
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)   // 仅画布图片解码用
{
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
static inline uint32_t c565_to888(uint16_t v)      // RGB565 → RGB888（配置/调色板导入）
{
  uint32_t r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
  return ((r << 3) | (r >> 2)) << 16 | ((g << 2) | (g >> 4)) << 8 | ((b << 3) | (b >> 2));
}

// ── 底图画布：图片缩放到 320×170（仅支持 PNG / JPG）────────────────
bool probe_dims(const char* path, int* w, int* h)
{
  FILE* f = fopen(path, "rb");
  if (!f) return false;
  static unsigned char b[65536];
  size_t n = fread(b, 1, sizeof b, f);
  fclose(f);
  if (n < 30) return false;
  auto be32 = [&](size_t o) { return (int)((b[o] << 24) | (b[o+1] << 16) | (b[o+2] << 8) | b[o+3]); };
  auto be16 = [&](size_t o) { return (int)((b[o] << 8) | b[o+1]); };

  if (b[0] == 0x89 && b[1] == 'P') {              // PNG: IHDR 宽高
    *w = be32(16); *h = be32(20);
    return *w > 0 && *h > 0;
  }
  if (b[0] == 0xFF && b[1] == 0xD8) {             // JPG: 扫描 SOF
    size_t i = 2;
    while (i + 9 < n) {
      if (b[i] != 0xFF) { i++; continue; }
      unsigned m = b[i + 1];
      if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) { i += 2; continue; }
      if (m == 0xDA) break;
      if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
        *h = be16(i + 5); *w = be16(i + 7);
        return *w > 0 && *h > 0;
      }
      i += 2 + ((b[i + 2] << 8) | b[i + 3]);
    }
  }
  return false;
}

bool load_image_to_canvas(const char* path)
{
  // 仅支持 PNG / JPG（BMP 已移除）
  const char* ext = strrchr(path, '.');
  bool is_jpg = ext && (!strcasecmp(ext, ".jpg") || !strcasecmp(ext, ".jpeg"));
  bool is_png = ext && !strcasecmp(ext, ".png");
  if (!is_jpg && !is_png) {
    fprintf(stderr, "[term] unsupported image (need png/jpg): %s\n", path);
    return false;
  }

  int iw = 0, ih = 0;
  probe_dims(path, &iw, &ih);
  fprintf(stderr, "[term] image %s probe %dx%d\n", path, iw, ih);
  if (iw <= 0 || ih <= 0 || iw * ih > 8000000) return false;   // 8MP 上限

  // ① 【内存顺序·防碎片】长生命周期的画布(s_canvas 320×170×2B)已在
  //    init() 中首先分配；这里紧接着分配文件读取内存，不插任何小块。
  //    解码完成后立即释放，避免堆在"大块-小块-大块"之间来回切。
  FILE* fp = fopen(path, "rb");
  if (!fp) return false;
  fseek(fp, 0, SEEK_END);
  long flen = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  if (flen <= 0 || flen > 32L * 1024 * 1024) { fclose(fp); return false; }
  std::unique_ptr<uint8_t[]> mem(new (std::nothrow) uint8_t[(size_t)flen]);
  if (!mem) { fclose(fp); return false; }
  size_t rd = fread(mem.get(), 1, (size_t)flen, fp);
  fclose(fp);

  // ② 原生尺寸解码目标（紧随文件读取内存分配）。
  //    四边各留 1px 背景色边：AA 抗锯齿在图像边缘会向外采样约 1px，
  //    不留边会读到缓冲区外（实测直接段错误）。
  lgfx::LGFX_Sprite img(&tft);
  img.setColorDepth(16);
  if (!img.createSprite(iw + 2, ih + 2)) { return false; }
  img.fillScreen(T->s_bg);

  // ③ 解码：LovyanGFX 自带内存解码变体（tjpgd=JPG / pngle=PNG）。
  // 不走 drawXxxFile（其文件系统注册在本平台缺失，会解码失败）。
  bool ok = is_jpg
      ? img.drawJpg(mem.get(), (uint32_t)rd, 1, 1, 0, 0, 0, 0, 1.0f, 1.0f)
      : img.drawPng(mem.get(), (uint32_t)rd, 1, 1, 0, 0, 0, 0, 1.0f, 1.0f);
  mem.reset();   // 解码完成 → 立即释放文件读取内存（全过程唯一常驻的是画布）
  fprintf(stderr, "[term] decode %s\n", ok ? "ok" : "FAILED");
  if (!ok) { img.deleteSprite(); return false; }

  // ④ 拉伸缩放到 320×170（不考虑长宽比），开抗锯齿
  //    pushImageRotateZoomWithAA = LovyanGFX 的抗锯齿缩放绘制。
  //    文件缓冲若是堆内小块(<128KB)则正好复用其空洞；大文件由 malloc
  //    走 mmap 并随 free 归还系统，同样不留碎片。
  lgfx::LGFX_Sprite dst(&tft);
  dst.setColorDepth(16);
  if (!dst.createSprite(kCanvasW, kCanvasH)) { img.deleteSprite(); return false; }
  dst.fillScreen(T->s_bg);
  //    语义：dst 锚点 = 源锚点映射到的目标坐标（dst = 锚点 + (src-源锚点)*zoom）。
  //    源锚点取带边精灵里的图片原点 (1,1) → 目标 (0,0)，整幅图从画布左上角
  //    铺满 320×170（不保长宽比）；源尺寸用带边精灵尺寸，行跨距才一致。
  //    源指针必须显式转成 uint16_t*：传 void* 会命中 LGFX 的 bgr888_t 重载
  //    （3 字节/像素），按 16bpp 数据读会越界（实测段错误）。
  dst.pushImageRotateZoomWithAA(
      0.0f, 0.0f,                                                 // 目标锚点 = 画布(0,0)
      1.0f, 1.0f,                                                 // 源锚点 = 图片(1,1)
      0.0f,
      (float)kCanvasW / iw, (float)kCanvasH / ih,                 // 拉伸（不保比例）
      iw + 2, ih + 2, (const uint16_t*)img.getBuffer());
  {
    // LGFX 内部解码/AA 管线为大端字节序（0xFC60 存为 0x60FC），
    // 而 tft.pushImage/画布按主机小端读取——在缓冲边界统一字节交换
    uint16_t* bp = (uint16_t*)dst.getBuffer();
    size_t n = (size_t)kCanvasW * kCanvasH;
    for (size_t i = 0; i < n; i++) bp[i] = (uint16_t)((bp[i] >> 8) | (bp[i] << 8));
    fprintf(stderr, "[term] AA out swapped, px(0,0)=%04x\n", bp[0]);
  }
  memcpy(s_canvas_master.get(), dst.getBuffer(), (size_t)kCanvasW * kCanvasH * 2);
  dst.deleteSprite();
  img.deleteSprite();
  return true;
}

// 有效前景色：粗体且取自 16 色的 0-7 时改用对应的亮色（fbterm adjustCharAttr 语义）
uint32_t cur_fg()
{
  if (T->s_bold && T->s_fg_idx >= 0 && T->s_fg_idx < 8) return mk_pal(T->s_fg_idx + 8);
  return T->s_fg;
}
uint32_t cell_fg()     { return T->s_rev ? (T->s_bg_def ? T->s_def_bg : T->s_bg) : cur_fg(); }
uint32_t cell_bg()     { return T->s_rev ? cur_fg() : (T->s_bg_def ? mk_col(0, 0, 0, true) : T->s_bg); }

// 单元属性取值：反显（SGR 7）在写格子时交换 fg/bg，逻辑色不被污染，
// SGR 27 / SGR 0 即可干净还原（此前预交换会把 bg 滞留成白色 → 整行色块）


// ── 渲染（fbcon putcs/clear 的对应物）──────────────────────────────
// 按行绘制：一行先整条贴回底图画布（320×10 一次内存拷贝），
// 再叠显式背景色块，最后画字形/豆腐块。光标由 present() 画在最上层。
//
// 豆腐块（无字形可画的字符占位）：
//   单宽 = 5×9 空心框；双宽 = 11×9 空心框（横跨两个字符格，右侧/下方各留 1px）
static void ensure_pal(State* s);                       // 前置（SGR 调色板色写入用）
static uint32_t resolve_col(State* s, uint32_t w, bool fg_channel);
static inline uint16_t to565(uint32_t col)          // RGB888(0x00RRGGBB) → RGB565
{
  return (uint16_t)((((col >> 16) & 0xF8) << 8) | (((col >> 8) & 0xFC) << 3) | ((col & 0xff) >> 3));
}
static inline std::string rgb_query(uint32_t col)  // OSC 应答 rgb:RRRR/GGGG/BBBB
{
  char b[32];
  snprintf(b, sizeof b, "rgb:%02x%02x/%02x%02x/%02x%02x",
           (col >> 16) & 0xff, (col >> 16) & 0xff, (col >> 8) & 0xff,
           (col >> 8) & 0xff, col & 0xff, col & 0xff);
  return b;
}

// 字形写入 CPU 行缓冲（dst=缓冲基址，stride=缓冲行宽；px0/py0=缓冲内偏移）
static void glyph_to_buf(uint16_t* dst, int stride, int px0, int py0, uint16_t ch, uint16_t fgc)
{
  if (ch == kChTofuR) return;                      // 双宽右半/延续格：框/字形已由左半画出
  const int wpx = (ch == kChTofuL) ? 2 * kCellW : kCellW;
  if (ch == kChTofuL || ch == kChTofu) {           // 空心框：仅画边
    const int x1 = (px0 + wpx - 1 < stride) ? px0 + wpx - 1 : stride - 1;
    for (int c = 0; c < wpx && px0 + c < stride; c++) { dst[py0 * stride + px0 + c] = fgc; dst[(py0 + kCellH - 1) * stride + px0 + c] = fgc; }
    for (int r = 0; r < kCellH; r++) { dst[(py0 + r) * stride + px0] = fgc; dst[(py0 + r) * stride + x1] = fgc; }
    return;
  }
  const termfont::TermGlyph* g = termfont::find(*s_font, ch);
  if (g == nullptr) {                              // 表外码点（兜底）：单宽空心框
    const int x1 = (px0 + kCellW - 1 < stride) ? px0 + kCellW - 1 : stride - 1;
    for (int c = 0; c < kCellW && px0 + c < stride; c++) { dst[py0 * stride + px0 + c] = fgc; dst[(py0 + kCellH - 1) * stride + px0 + c] = fgc; }
    for (int r = 0; r < kCellH; r++) { dst[(py0 + r) * stride + px0] = fgc; dst[(py0 + r) * stride + x1] = fgc; }
    return;
  }
  const uint16_t* bits = &s_font->bits[g->bits];
  for (int r = 0; r < s_font->rows; r++) {
    uint16_t row = bits[r];
    if (row == 0) continue;
    uint16_t* out = dst + (py0 + r) * stride + px0;
    for (int cc = 0; cc < g->w && px0 + cc < stride; cc++)   // 墨迹可宽于单元：按行宽裁剪
      if (row & (1u << cc)) out[cc] = fgc;
  }
}

// 视图首行绝对索引：翻页锚定期间用锚点（内容不随输出漂移），否则距底偏移
static int view_start()
{
  const int total = hist_size(T) + kRows;
  int start = (T->s_view != 0 && T->s_view_anchor >= 0)
                ? T->s_view_anchor : (total - T->s_view - kRows);
  if (start < 0) start = 0;
  if (start > total - kRows) start = total - kRows;
  return start;
}

void paint_row(int y)                               // 画一整行（含底图/背景/字形）
{
  const int start = view_start();
  const int j = start + y;
  const int py = y * kCellH;
  uint32_t fq[kMaxCols], bq[kMaxCols];
  const uint16_t* chp;
  if (j < hist_size(T)) {                          // 历史行：RLE 解码（仅显示时）
    RowB& r = hist_row(T, j);
    r.decode(fq, bq);
    chp = r.ch;
  } else {                                         // 活动行：颜色区直接读（零解压）
    LiveRowRef r = buf_row(T, j - hist_size(T));
    memcpy(fq, r.fg, (size_t)kCols * sizeof(uint32_t));
    memcpy(bq, r.bg, (size_t)kCols * sizeof(uint32_t));
    chp = r.ch;
  }

  // 行级离屏合成：① 底图带 → ② 不透明背景 → ③ 字形，单次上屏
  static uint16_t rowbuf[kCanvasW * 16];              // kCellH ≤ 14
  memcpy(rowbuf, &T->s_canvas[(size_t)py * kCanvasW],
         (size_t)kCanvasW * kCellH * sizeof(uint16_t));
  for (int x = 0; x < kCols; x++) {                   // ② 不透明背景色单元
    uint32_t b = resolve_col(T, bq[x], false);
    if (!is_trans_bg(b)) {
      const uint16_t bc = to565(b);
      for (int r = 0; r < kCellH; r++) {
        uint16_t* out = rowbuf + r * kCanvasW + x * kCellW;
        for (int c = 0; c < kCellW; c++) out[c] = bc;
      }
    }
  }
  for (int x = 0; x < kCols; x++)                     // ③ 字形 / 豆腐块
    glyph_to_buf(rowbuf, kCanvasW, x * kCellW, 0, chp[x], to565(resolve_col(T, fq[x], true)));
  tft.pushImage(0, py, kCanvasW, kCellH, rowbuf);
}

void redraw_view()
{
  for (int i = 0; i < kRows; i++) paint_row(i);
}

// ── 光标（fbcon_cursor 的对应物）：屏幕上任何时刻最多一个方块 ─────────
// 不变量：s_cur_drawn_row/col 记录"像素层上真实画着光标块的格"，-1 = 屏上没有。
// 光标块只能由 cursor_paint() 画；擦除一律走整行 paint_row()（与正常渲染同一条
// "画布底 + 背景色 + 字形"合成路径），绝不做"只覆盖一部分像素"的局部擦除，
// 因此擦除后的格子与整行重画逐像素一致。
// 任何画/擦动作都必须同步更新记录；忘记更新 = 旧方块残留 = 屏幕上两个光标。
static bool cursor_should_draw()
{
  return T != nullptr && T->s_view == 0 && T->s_cursor_vis && T->s_cursor_on &&
         T->s_row >= 0 && T->s_row < kRows && T->s_col >= 0 && T->s_col < kCols;
}
static void cursor_forget()          // 屏上已无光标（调用方负责让旧方块被重画覆盖后清记录）
{
  if (T == nullptr) return;
  T->s_cur_drawn_row = -1;
  T->s_cur_drawn_col = -1;
}
// 记录里的旧光标格所在行标脏（下帧整行重画→顺带擦掉旧方块），并清记录。
static void cursor_dirty_for_repaint()
{
  if (T == nullptr) return;
  if (T->s_cur_drawn_row >= 0 && T->s_cur_drawn_row < kRows)
    mark_row(T->s_cur_drawn_row);
  cursor_forget();
}
// 光标颜色/形状/闪烁/显隐恢复配置默认（RIS / DECSTR / OSC112 / 程序退出路径共用）。
// 只改状态不重画：调用方随后 mark_row()/cursor_dirty_for_repaint() 让新光标生效。
static void cursor_style_default(State* s)
{
  s->s_cursor_col = s->s_cursor_col_def;   // OSC 12 定制色作废，回配置默认
  s->s_cur_style = 1;                      // DECSCUSR 默认：闪烁方块
  s->s_cursor_blink = true;
  s->s_cursor_vis = true;                  // DECTCEM 默认可见
  s->s_cursor_on = true;
  s->s_cursor_t0 = now_ms_();
}
// DECSC 保存态清成默认值（RIS / DECSTR 用；之后 ESC 8 恢复出来的是干净状态）。
static void save_state_default(State* s)
{
  s->s_save_row = 0; s->s_save_col = 0;
  s->s_save_fg = s->s_def_fg; s->s_save_bg = s->s_def_bg;
  s->s_save_bgdef = true; s->s_save_fgidx = 7;
  s->s_save_bold = false; s->s_save_rev = false; s->s_save_wrp = false;
}

void clear_region(int y0, int x0, int y1, int x1)   // 半开区间（屏缓冲坐标）
{
  for (int y = y0; y < y1; y++) {
    if (x1 > x0) {                     // 边界上的宽字对先整对拆掉，避免留下孤儿半字
      break_wide(x0, y);
      break_wide(x1 - 1, y);
    }
    buf_row(T, y).fill_span(x0, x1 - x0, ' ', cell_fg(), cell_bg());
    if (x0 == 0 && x1 >= kCols) buf_row(T, y).flags = 0;   // 整行擦除 → 硬换行边界
    mark_row(y);
  }
  // 擦除区域覆盖了旧光标所在行 → 该行会被整行重画（旧方块随之消失），记录作废。
  if (x1 > x0 && T->s_cur_drawn_row >= y0 && T->s_cur_drawn_row < y1) cursor_forget();
}

// ── 光标闪烁节拍常量（画/擦实现见 cursor_paint / present_now）────────
constexpr uint64_t kBlinkMs = 400;   // 光标闪烁：亮 400ms / 灭 400ms（写死）
                                                  // 写满最后一列后光标停在列尾，
                                                  // 直到下一个可打印字符才换行

// ── 制表位（HT / HTS / TBC / CBT / CHT）───────────────────────────
void tabs_reset()
{
  for (int x = 0; x < kCols; x++) T->s_tabs[x] = (x > 0 && x % 8 == 0);
}

int next_tab(int n)                               // 前进 n 个制表位（到右边界止）
{
  int c = T->s_col;
  for (int k = 0; k < n; k++) {
    int x = c + 1;
    while (x < kCols && !T->s_tabs[x]) x++;
    c = (x < kCols) ? x : kCols - 1;
  }
  return c;
}

int prev_tab(int n)                               // 后退 n 个制表位（到 0 列止）
{
  int c = T->s_col;
  for (int k = 0; k < n; k++) {
    int x = c - 1;
    while (x > 0 && !T->s_tabs[x]) x--;
    c = x < 0 ? 0 : x;
  }
  return c;
}

// 插入模式（IRM）：光标起整行右移 n 格，空出的位置填空格
// 位移/插入/删除后修复断开的宽字符对（孤儿右半标记会显示为空缺格）
void repair_pairs(LiveRowRef r)
{
  for (int x = 0; x < kCols; x++) {
    if (r.ch[x] == kChTofuR) {
      bool ok = (x > 0 && r.ch[x - 1] != kChTofuR);
      if (ok) {
        const termfont::TermGlyph* g = termfont::find(*s_font, r.ch[x - 1]);
        ok = (r.ch[x - 1] == kChTofuL) || (g != nullptr && g->cells == 2);
      }
      if (!ok) r.ch[x] = ' ';
    } else if (r.ch[x] != ' ') {
      const termfont::TermGlyph* g = (r.ch[x] == kChTofuL) ? nullptr : termfont::find(*s_font, r.ch[x]);
      const bool wide = (r.ch[x] == kChTofuL) || (g != nullptr && g->cells == 2);
      if (wide) {
        if (x + 1 >= kCols) r.ch[x] = ' ';               // 宽字形落行尾：退化为空格
        else if (r.ch[x + 1] != kChTofuR) r.ch[x + 1] = kChTofuR;  // 补回右半标记
      }
    }
  }
}

void insert_cells(int n)
{
  LiveRowRef r = buf_row(T, T->s_row);
  for (int x = kCols - 1; x >= T->s_col + n; x--) {
    r.ch[x] = r.ch[x - n]; r.fg[x] = r.fg[x - n]; r.bg[x] = r.bg[x - n];
  }
  r.fill_span(T->s_col, n, ' ', cell_fg(), cell_bg());
  mark_row(T->s_row);
}

// 终端向应用回复数据的通道（PTY 会话中由 session 注入；无参演示模式为空）
void reply(const char* s, size_t n) { if (T->s_writer != nullptr) T->s_writer(s, n); }

// 画光标块（fbcon_cursor 的最上层绘制）。
// 只在 cursor_should_draw() 成立时调用，且调用前该格必须已按最新屏缓冲整行合成过
// （present_now 的脏行绘制 / tick 走 present_now），所以这里只叠光标块不改字形。
// 宽字符对按"整对"画：光标落在宽字右半格时回溯到左半，方块覆盖整对 2*kCellW 像素。
// （此前光标只画 kCellW 宽、而闪烁擦除又按"整对"重画，二者落点/宽度不一致，
//   宽字附近会留下半个方块/半截字形 —— 表现为"乱像素"。现在两者统一到整行合成。）
void cursor_paint()
{
  if (!cursor_should_draw()) return;
  if (T->s_row < 0 || T->s_row >= kRows || T->s_col < 0 || T->s_col >= kCols) return;
  int x0 = T->s_col, cells = 1;
  {                                        // view==0（cursor_should_draw 已保证）→ 光标格必在活动行
    LiveRowRef r = buf_row(T, T->s_row);
    if (r.ch[T->s_col] == kChTofuR && T->s_col > 0) x0 = T->s_col - 1;   // 右半 → 回溯到整对起点
    if (x0 + 1 < kCols && r.ch[x0 + 1] == kChTofuR) cells = 2;           // 宽字对整对处理
  }
  const int x = x0 * kCellW, y = T->s_row * kCellH, w = cells * kCellW;
  const uint16_t c = to565(T->s_cursor_col);           // 光标颜色（config cursor / OSC12）
  switch (T->s_cur_style) {
    case 3: case 4: tft.fillRect(x, y + kCellH - 2, w, 2, c); break;   // 下划线（宽字整对）
    case 5: case 6: tft.fillRect(x, y, 2, kCellH, c); break;           // 竖条（取宽字对左边界）
    default: {                                                           // 方块（宽字整对）
      // 光标处有字符：字符照常显示——底色=光标色，字形用对应格的字符色，
      // 覆盖到光标层之上（与 paint_row 同一条 resolve_col/glyph_to_buf 合成路径）。
      static uint16_t buf[2 * 16 * 16];            // ≤2 单元宽 × 16 高（kCellH≤14）
      for (int i = 0; i < w * kCellH; i++) buf[i] = c;
      LiveRowRef r = buf_row(T, T->s_row);
      for (int i = 0; i < cells; i++) {
        const int cx = x0 + i;
        glyph_to_buf(buf, w, i * kCellW, 0, r.ch[cx], to565(resolve_col(T, r.fg[cx], true)));
      }
      tft.pushImage(x, y, w, kCellH, buf);
      break;
    }
  }
  // 画过的地方一定记得：记录同步更新，后续移动/擦除才不会漏掉这一块。
  T->s_cur_drawn_row = T->s_row;
  T->s_cur_drawn_col = T->s_col;
}

static void view_follow_now();                     // 前置（present_now 到点跳底用）

// OSC4 动态调色板：首次使用调色板色时分配（默认值=配置 0-15 + xterm-256 立方 16-231
// + 灰度 232-255）；一旦分配直到窗口销毁不释放；rewrap 按指针传递。
// 注：16-231 是 xterm-256 的 6×6×6 立方（亮度档 0/95/135/175/215/255），非 web-safe。
static void ensure_pal(State* s)
{
  if (s->s_pal4) return;
  std::unique_ptr<uint32_t[]> p(new (std::nothrow) uint32_t[256]);
  if (!p) return;                        // 分配失败：渲染时回退默认色
  for (int i = 0; i < 16; i++) p[i] = s->s_pal[i];
  static const int lv[6] = {0, 95, 135, 175, 215, 255};
  for (int i = 16; i < 232; i++) {
    int v = i - 16;
    p[i] = mk_col(lv[v / 36], lv[(v / 6) % 6], lv[v % 6], false);
  }
  for (int i = 232; i < 256; i++) {
    int g = 8 + 10 * (i - 232);
    p[i] = mk_col(g, g, g, false);
  }
  s->s_pal4 = std::move(p);
}

// 解析颜色词（fg/bg 通道）→ RGB888（透明原样返回 0x80FFFFFF）
static uint32_t resolve_col(State* s, uint32_t w, bool fg_channel)
{
  if (!(w & 0x80000000u)) return w;               // 显式 RGB（SGR 38;2/48;2）：不透明，即使同默认值
  if (!fg_channel && is_trans_bg(w)) return 0x80FFFFFFu;
  if (!fg_channel) {                              // 背景：解析值==默认背景色(OSC 11) → 渲染透明
    if (s->s_pal4 != nullptr) {
      const uint32_t c = s->s_pal4[pal_idx(w)];
      return ((c & 0x80FFFFFFu) == (s->s_def_bg & 0xFFFFFFu)) ? 0x80FFFFFFu : c;
    }
    return 0x80FFFFFFu;                           // 未分配/分配失败 → 按默认背景 → 透明
  }
  if (s->s_pal4 != nullptr) return s->s_pal4[pal_idx(w)];
  return s->s_def_fg;                             // 前景兜底（未分配/分配失败）
}

// ── toast 叠加层（LovyanGFX 层自定义合成）────────────────────────────
// 设计要点：toast 不是终端网格的一部分（不占字符格、不随字号变化），而是画在
// 行/光标像素之上的一层独立图层。因 present 的脏行重画会整行覆盖 toast 区域，
// 所以合帧入口统一为 present_now() = present_core()（原渲染主路径）+ toast_paint()：
// 任何一次上屏（脏行/光标/翻页视图）结束后都无条件补画 toast，保证它在最上层。
// 恢复：toast_clear_region() 把 toast 覆盖到的屏幕行标脏后立即合帧重画（同一
// paint_row 合成路径 + 光标），不做像素级局部擦除。
static void toast_paint();              // 前置（定义见 present_now 之后）

// 上屏：把攒下的脏行一次画出，光标画在最上层。
// 逐字符只标脏不画像素；绘制次数由帧率决定，与输出字符数无关。
// 光标唯一性：本函数是"擦旧光标 + 画新光标"的唯一合帧点——
//   ① 光标挪动/不该显示 → 先把旧光标格所在行标脏（整行重画 = 与 paint_row 同合成）；
//   ② 画完脏行后，仅在 cursor_should_draw() 时画一次光标并更新记录。
// 任何相位下屏上最多一个方块；记录失真（tick 旧实现只画不记）曾导致旧方块残留。
static void present_core()
{
  if (T->s_view != 0 && T->s_view_pending && now_ms_() >= T->s_view_hold_until)
    view_follow_now();                           // 保持期结束：跳回底部
  T->s_last_present = now_ms_();
  if (T->s_view != T->s_shown_view) mark_all();            // 视图切换：行→内容映射变了，整屏重画
  T->s_shown_view = T->s_view;
  if (T->s_view != 0) {                                // 回滚视图：有变化整屏重画，不画光标
    if (T->s_dirty) { redraw_view(); T->s_dirty = 0; }
    cursor_forget();                                   // 视图里没有光标（回到底部时再画）
    return;
  }
  // ① 旧光标处理：光标挪动过 / 已不该显示 → 旧格所在行整行重画（擦掉旧方块）
  if (T->s_cur_drawn_row >= 0) {
    const bool same = (T->s_cur_drawn_row == T->s_row && T->s_cur_drawn_col == T->s_col);
    if (!cursor_should_draw() || !same) {
      mark_row(T->s_cur_drawn_row);                    // 同一行内左右移动也覆盖：整行重画
      cursor_forget();
    }
  }
  const bool need_cursor = cursor_should_draw();
  if (T->s_dirty == 0) {
    // 没有待画内容：屏上光标已正确 → 整帧无变化；只有"该有却没有"才补画该行
    if (!need_cursor || T->s_cur_drawn_row >= 0) return;
    paint_row(T->s_row);                               // 该格按最新屏缓冲重新合成
  } else {
    for (int y = 0; y < kRows; y++)
      if (T->s_dirty & (1u << y)) paint_row(y);
  }
  T->s_dirty = 0;
  // ② 画新光标（唯一绘制点；cursor_paint 内部更新 s_cur_drawn_*）
  if (need_cursor) cursor_paint();
  else cursor_forget();
}

// 上屏统一入口：先走原渲染主路径（脏行/光标/翻页视图），再在最上层补画 toast。
// toast_paint 自身判空/判开关/判静默/判过期，未显示时开销为零；
// 合帧语义（16ms）完全保留：这里只是同一帧末尾多几步像素写。
// 光标闪烁复位策略（0.7.6）：显示/隐藏/位置任一变化、以及按键输入（session 在
// send_key 调 cursor_reblink），都立即回到"亮点相"并重启 400ms 计时。
// 本函数在 tick() 与 present_now() 顶部各调一次：任何输出路径造成的移动都会在
// 下一帧被发现并复位（与按键复位不冲突，都是同一 reset 语义）。
static void blink_track()
{
  if (T->s_bl_r == T->s_row && T->s_bl_c == T->s_col && T->s_bl_vis == T->s_cursor_vis) return;
  T->s_bl_r = T->s_row; T->s_bl_c = T->s_col; T->s_bl_vis = T->s_cursor_vis;
  cursor_reblink();
}

void present_now()
{
  blink_track();
  present_core();
  toast_paint();
}

// ── toast 叠加层实现（状态/几何/排版/绘制/恢复）──────────────────────
// 状态独立于窗口 State：activate/state_resize 换 State* 不影响它，
// 下次 present_now 会按（新的）当前窗口调色板重画；到期由 tick() 恢复。
// 几何（严格按规格）：屏幕最上方、水平居中、上边距 10px、总高 32px、
// 边框 2px（不透明、无抗锯齿）、内部可视区 28px、圆角半径 6px；
// 宽度 = 文本宽 + 左右内边距 24 + 边框 4，上限屏宽（超宽截断文本）。
constexpr int kToastTop    = 14;    // 上边距（下移 4px）
constexpr int kToastH      = 24;    // 总高（含边框）
constexpr int kToastR      = 9;     // 外圆角半径
constexpr int kToastBorder = 2;     // 边框宽度（不透明像素）
constexpr int kToastPadX   = 7;     // 左右内边距（各缩短 5px）
constexpr int kToastTextH  = 14;    // 固定 8x14 字体行高
constexpr int kToastGlyphW = 8;     // 8x14 单宽单元宽
constexpr int kToastMaxCp  = 40;    // 最多字形数（可用文本宽 292px / 8 = 36，留裕量）

struct ToastState {
  bool     enabled = true;          // set_toast_enabled 总开关
  bool     visible = false;         // 屏上当前是否画着 toast
  int      x = 0, y = 0, w = 0;     // 绘制矩形（含边框），恢复时据此定位屏幕行
  uint64_t until = 0;               // 到期时刻（CLOCK_MONOTONIC ms）
  int      textw = 0;               // 文本总像素宽
  int      ncp = 0;                 // 排版后的字形数
  uint32_t cp[kToastMaxCp] = {};    // 码点（BMP 之外也保留，绘制时退化为豆腐块）
  uint8_t  cw[kToastMaxCp] = {};    // 该字形占的像素宽（8 = 单宽 / 16 = 双宽）
};
static ToastState s_toast;          // 单实例（同一时刻只有一条 toast）
static int s_toast_default_ms = 2000;   // toast 默认停留时长（set_toast_default_ms 配置；进程级）

// 取当前窗口 16 色调色板颜色（RGB888）：OSC4 动态调色板优先（与行渲染一致），
// 否则用配置的 16 色；与 resolve_col 同源，只是这里直接按索引取色。
static uint32_t toast_pal_color(State* s, int idx)
{
  if (idx < 0) idx = 0;
  if (s->s_pal4 != nullptr && idx < 256) return s->s_pal4[idx] & 0x00FFFFFFu;
  if (idx > 15) idx = 15;
  return s->s_pal[idx] & 0x00FFFFFFu;
}

// 无抗锯齿填充圆角矩形：中段 + 左右直边用 fillRect，四角用 fillCircle 拼。
// LovyanGFX 的 fillCircle/fillRect 都是硬边（整数像素），不会产生抗锯齿半透明边。
static void fill_round_rect(int x, int y, int w, int h, int r, uint16_t col)
{
  if (w <= 0 || h <= 0) return;
  if (r > w / 2) r = w / 2;
  if (r > h / 2) r = h / 2;
  if (r <= 0) { tft.fillRect(x, y, w, h, col); return; }
  tft.fillRect(x + r, y, w - 2 * r, h, col);              // 中段（贯穿全高）
  tft.fillRect(x, y + r, r, h - 2 * r, col);              // 左直边
  tft.fillRect(x + w - r, y + r, r, h - 2 * r, col);      // 右直边
  tft.fillCircle(x + r, y + r, r, col);                   // 四角（无 AA）
  tft.fillCircle(x + w - r - 1, y + r, r, col);
  tft.fillCircle(x + r, y + h - r - 1, r, col);
  tft.fillCircle(x + w - r - 1, y + h - r - 1, r, col);
}

// 宽容 UTF-8 解码：返回码点并让 p 前进；非法/截断序列按 U+FFFD 处理（不越界读）。
static uint32_t toast_utf8_next(const char*& p)
{
  const unsigned char* q = (const unsigned char*)p;
  const unsigned char b = *q;
  if (b < 0x80) { p++; return b; }
  int n; uint32_t cp;
  if ((b & 0xE0) == 0xC0)      { n = 1; cp = b & 0x1Fu; }
  else if ((b & 0xF0) == 0xE0) { n = 2; cp = b & 0x0Fu; }
  else if ((b & 0xF8) == 0xF0) { n = 3; cp = b & 0x07u; }
  else { p++; return 0xFFFDu; }
  for (int i = 0; i < n; i++) {
    const unsigned char c = q[1 + i];
    if ((c & 0xC0) != 0x80) { p++; return 0xFFFDu; }     // 截断：只丢弃首字节，后续字节留给下一轮
    cp = (cp << 6) | (c & 0x3Fu);
  }
  p += n + 1;
  return cp;
}

// 文本排版：解码 UTF-8 → 每字形码点/像素宽（按 8x14 字体表判全半角）；
// 超过可用文本宽（屏宽 - 边框 - 左右内边距 = 292px）即截断，绝不在字形中间切。
static void toast_layout(const char* text)
{
  s_toast.ncp = 0;
  s_toast.textw = 0;
  if (text == nullptr) return;
  const int maxw = kCanvasW - 2 * kToastBorder - 2 * kToastPadX;   // 可用文本宽
  const char* p = text;
  while (*p != '\0' && s_toast.ncp < kToastMaxCp) {
    const uint32_t cp = toast_utf8_next(p);
    if (cp < 0x20 || (cp >= 0x7f && cp < 0xa0)) continue;          // 控制字符：跳过
    const termfont::TermGlyph* g = (cp <= 0xFFFF)
        ? termfont::find(termfont::font_8x14, (uint16_t)cp) : nullptr;
    int cells;
    if (g != nullptr) cells = g->cells;
    else {
      const int w = cp_width(cp);                                  // 字体表外：近似 wcwidth
      if (w <= 0) continue;                                        // 零宽/组合符：不占位
      cells = w >= 2 ? 2 : 1;
    }
    if (cells < 1) cells = 1;
    if (cells > 2) cells = 2;
    const int px = cells * kToastGlyphW;
    if (s_toast.textw + px > maxw) break;                          // 超宽：截断
    s_toast.cp[s_toast.ncp] = cp;
    s_toast.cw[s_toast.ncp] = (uint8_t)px;
    s_toast.textw += px;
    s_toast.ncp++;
  }
}

// 文本条带渲染：固定 8x14 字体表，先按背景色铺满条带再点字形像素，
// 合成到局部缓冲后一次 pushImage 上屏（与行渲染同一条"内存合成"思路）。
// 字形墨迹可宽于单元（斜体/宽字溢出）：按单元宽裁剪，避免压到相邻字形。
static void toast_draw_text(int x, int y, uint16_t fg, uint16_t bg)
{
  const int tw = s_toast.textw;
  if (tw <= 0) return;
  static uint16_t band[kCanvasW * kToastTextH];   // ≤292×14，8.2KB
  for (int i = 0; i < tw * kToastTextH; i++) band[i] = bg;
  const int rows = termfont::font_8x14.rows;      // = 14
  int gx = 0;
  for (int i = 0; i < s_toast.ncp; i++) {
    const uint32_t cp = s_toast.cp[i];
    const int cellpx = s_toast.cw[i];
    const termfont::TermGlyph* g = (cp <= 0xFFFF)
        ? termfont::find(termfont::font_8x14, (uint16_t)cp) : nullptr;
    if (g != nullptr) {
      const uint16_t* bits = &termfont::font_8x14.bits[g->bits];
      const int maxc = g->w < cellpx ? g->w : cellpx;   // 墨迹宽于单元时按单元裁剪
      for (int r = 0; r < rows && r < kToastTextH; r++) {
        const uint16_t row = bits[r];
        if (row == 0) continue;
        uint16_t* out = band + r * tw + gx;
        for (int c = 0; c < maxc; c++)
          if (row & (1u << c)) out[c] = fg;
      }
    } else {
      // 字体表外码点：与终端行渲染一致的 1px 空心框（无抗锯齿）
      for (int c = 0; c < cellpx; c++) {
        band[gx + c] = fg;
        band[(kToastTextH - 1) * tw + gx + c] = fg;
      }
      for (int r = 0; r < kToastTextH; r++) {
        band[r * tw + gx] = fg;
        band[r * tw + gx + cellpx - 1] = fg;
      }
    }
    gx += cellpx;
  }
  tft.pushImage(x, y, tw, kToastTextH, band);
}

// 把当前 toast 画到屏幕最上层（必须在行/光标重画之后调用）。
// 颜色全部取当前窗口调色板：背景 bright0(pal[8])、边框 bright4(pal[12])、
// 文字 bright7(pal[15])，经 to565 转 RGB565；形状无 AA。
static void toast_paint()
{
  if (!s_toast.visible || T == nullptr || s_quiet) return;
  if (now_ms_() >= s_toast.until) return;      // 已到期：不再重画，等 tick 统一恢复
  const uint16_t bg = to565(toast_pal_color(T, 8));    // bright0 → 背景
  const uint16_t bd = to565(toast_pal_color(T, 12));   // bright4 → 边框
  const uint16_t fg = to565(toast_pal_color(T, 15));   // bright7 → 文字
  const int x = s_toast.x, y = s_toast.y, w = s_toast.w;
  // ① 外圈（边框色）→ ② 内缩 2px（背景色，留出 2px 边框）→ ③ 文本条带
  fill_round_rect(x, y, w, kToastH, kToastR, bd);
  const int iw = w - 2 * kToastBorder;
  const int ih = kToastH - 2 * kToastBorder;                 // 20
  fill_round_rect(x + kToastBorder, y + kToastBorder, iw, ih, kToastR - kToastBorder, bg);
  const int ty = y + kToastBorder + (ih - kToastTextH) / 2 + 1;  // 文字在 20px 内部区垂直居中，再下移 1px
  toast_draw_text(x + kToastBorder + kToastPadX, ty, fg, bg);
}

// 恢复 toast 覆盖的区域：把被覆盖到的屏幕行整行标脏后立即合帧重画
// （与正常渲染同一条 paint_row 合成路径，并顺带重画唯一光标），
// 不做像素级局部擦除，避免与背景图/反显等合成状态不一致。
// 先置 visible=false：重画路径里的 toast_paint 不会再叠画。
static void toast_clear_region()
{
  if (!s_toast.visible) return;
  s_toast.visible = false;
  if (T == nullptr) return;          // 无活动窗口：记录作废，下次整屏重画会覆盖
  const int y0 = s_toast.y / kCellH;
  const int y1 = (s_toast.y + kToastH - 1) / kCellH;
  for (int y = y0; y <= y1; y++) mark_row(y);
  if (!s_quiet) present_now();       // 立即恢复（含光标），保留 16ms 合帧语义
}

// 显示一条 toast（内部实现）：已有旧 toast 时先按旧区域恢复，再排版/绘制新的。
// 立即绘制（交互反馈不等下一帧）；之后每次 present_now 末尾都会重画以压在最上层。
static void toast_show(const char* text, int ms)
{
  if (!s_toast.enabled || text == nullptr || T == nullptr) return;
  if (s_quiet) return;               // 后台静默解析期间不绘制（由 session 决定调用时机）
  toast_clear_region();              // 旧 toast：先恢复它盖住的终端内容
  toast_layout(text);
  s_toast.w = s_toast.textw + 2 * kToastPadX + 2 * kToastBorder;
  if (s_toast.w > kCanvasW) s_toast.w = kCanvasW;    // 兜底：绝不越界（排版已按上限截断）
  s_toast.x = (kCanvasW - s_toast.w) / 2;            // 水平居中
  s_toast.y = kToastTop;
  s_toast.until = now_ms_() + (uint64_t)(ms > 0 ? ms : s_toast_default_ms);  // ms<=0 → 配置的默认时长
  s_toast.visible = true;
  toast_paint();                     // 立即上屏
}

// tick() 的过期检查：停留时间到 → 恢复被覆盖区域（行重画 + 光标）。
static void toast_expire_check()
{
  if (s_toast.visible && now_ms_() >= s_toast.until) toast_clear_region();
}

// 合帧节流：批量输出时攒着，最多每 kPresentMs 上屏一次。
// 输出暂停（缓冲空）或交互输入时调用方会走 present() 立即上屏。
void maybe_present()
{
  if (s_quiet) return;               // 静默解析：不画（防后台窗口上屏）
  if (T->s_dirty == 0) return;
  if (now_ms_() - T->s_last_present >= kPresentMs) present_now();
}

// ── 视图跟随（0.5.1 语义）────────────────────────────────────────────
// 手动翻页（Shift+PgUp/PgDn、Ctrl+Shift+↑/↓）后 hold_ms 内不跳底；
// 期间真实滚屏只记 pending，到点跳底；hold 外真实滚屏立即跳底；
// 翻页以外的任何按键立即跳底（view_follow，由 session 调用）。
// 程序纯写屏/重绘不触发跳底（仅真实滚屏算自动滚动）。
static void view_follow_now()
{
  if (T == nullptr || T->s_view == 0) return;
  T->s_view = 0;
  T->s_view_pending = false;
  T->s_view_anchor = -1;                   // 回到底部 = 跟随模式
  mark_all();
}
static void note_view_scroll()                      // 真实滚屏事件
{
  if (T == nullptr || T->s_view == 0) return;
  if (now_ms_() < T->s_view_hold_until) { T->s_view_pending = true; return; }
  view_follow_now();
}

void follow_output()                                // 程序写入不再强制跳底（保留调用点）
{
}

// ── 滚屏（fbcon_scroll 软滚动 + 回滚缓冲）─────────────────────────
// 只搬屏缓冲并标脏行——像素层留给 present() 合帧绘制，
// 不再每滚一行就全屏重画（这是海量输出慢的最大元凶）。
void scroll_up()
{
  note_view_scroll();                             // 真实滚屏事件（视图跳底判定）
  const bool full = (T->s_top == 0 && T->s_bot == kRows - 1);
  // 满屏滚动：顶行归档（RLE 压缩一次）后环头推进，零内存搬移；
  // 备用屏(?1049)不占回滚历史（xterm/fbterm 语义）。
  if (full && !T->s_alt_active) {
    hist_push_live(T, buf_row(T, T->s_top));
    { const int hb = hist_size(T);
      while (hist_size(T) > T->s_hist_max) hist_pop_front(T);
      const int popped = hb - hist_size(T);                // 丢头行 → 绝对索引整体前移
      if (popped > 0 && T->s_view_anchor >= 0) {
        T->s_view_anchor -= popped;
        if (T->s_view_anchor < 0) T->s_view_anchor = 0;
      }
    }
    grid_head(T, +1);
    buf_row(T, T->s_bot).fill_span(0, kCols, ' ', cell_fg(), cell_bg());
    buf_row(T, T->s_bot).flags = 0;               // 新空行不是软换行
  } else {
    for (int y = T->s_top; y < T->s_bot; y++) buf_row(T, y).move_from(buf_row(T, y + 1));
    buf_row(T, T->s_bot).fill_span(0, kCols, ' ', cell_fg(), cell_bg());
    buf_row(T, T->s_bot).flags = 0;
  }
  mark_rows(T->s_top, T->s_bot + 1);
  cursor_dirty_for_repaint();                     // 旧光标行（可能滚出脏区/被搬移）一并重画
}

void scroll_down()                                  // 反向滚屏（区域底部丢弃、顶部插入空行）
{
  note_view_scroll();                             // 真实滚屏事件（视图跳底判定）
  if (T->s_top == 0 && T->s_bot == kRows - 1) {
    grid_head(T, -1);                             // 内容整体下移：环头-1（零搬移）
    buf_row(T, T->s_top).fill_span(0, kCols, ' ', cell_fg(), cell_bg());
    buf_row(T, T->s_top).flags = 0;
  } else {
    for (int y = T->s_bot; y > T->s_top; y--) buf_row(T, y).move_from(buf_row(T, y - 1));
    buf_row(T, T->s_top).fill_span(0, kCols, ' ', cell_fg(), cell_bg());
    buf_row(T, T->s_top).flags = 0;
  }
  mark_rows(T->s_top, T->s_bot + 1);
  cursor_dirty_for_repaint();                     // 旧光标行（可能滚出脏区/被搬移）一并重画
}

void line_down()                                    // 下移一行、列不变（LF / IND 语义）
{
  if (T->s_row >= T->s_bot) { T->s_row = T->s_bot; scroll_up(); }
  else if (T->s_row < kRows - 1) T->s_row++;
}

void newline()                                      // 回行首并下移（NEL / 自动环绕语义）
{
  T->s_col = 0;
  line_down();
}

// ── ANSI 解析状态机 ───────────────────────────────────────────────

void sgr(const int* p, const char* sep, int n)
{
  auto val = [](int t) { return t < 0 ? 0 : t; };   // 空参数按 0 处理（扩展色的空 colorspace 例外）
  for (int i = 0; i < n; i++) {
    int v = val(p[i]);
    if (v == 0)      { T->s_fg_idx = 7; T->s_fg = T->s_def_fg; T->s_bold = false; T->s_bg_def = true; T->s_rev = false; }  // 复位
    else if (v == 1) T->s_bold = true;                           // 粗体 → 亮色（fbterm 语义）
    else if (v == 22) T->s_bold = false;
    else if (v == 3 || v == 23 || v == 4 || v == 24) {
      /* 斜体/下划线：不支持，按默认字体渲染 */                 }
    else if (v == 7)  T->s_rev = true;                       // 反显开（幂等）
    else if (v == 27) T->s_rev = false;                      // 反显关
    else if (v >= 30 && v <= 37) { ensure_pal(T); T->s_fg_idx = v - 30; T->s_fg = mk_pal(T->s_fg_idx); }
    else if (v == 39) { T->s_fg_idx = 7; T->s_fg = T->s_def_fg; }      // 默认前景 = regular7（OSC10 可改）
    else if (v >= 40 && v <= 47) { ensure_pal(T); T->s_bg = mk_pal(v - 40); T->s_bg_def = false; }
    else if (v == 49) T->s_bg_def = true;
    else if (v >= 90 && v <= 97) { ensure_pal(T); T->s_fg_idx = v - 90 + 8; T->s_fg = mk_pal(T->s_fg_idx); }
    else if (v >= 100 && v <= 107) { ensure_pal(T); T->s_bg = mk_pal(v - 100 + 8); T->s_bg_def = false; }
    else if (v == 38 || v == 48) {                  // 扩展色：256 色(5;n) / RGB(2;r;g;b)
      bool fg = (v == 38);
      if (i + 1 >= n) continue;
      int mode = val(p[i + 1]);
      if (mode == 5) {                              // 38;5;n 与 38:5:n
        int j = i + 2;
        while (j < n && p[j] < 0) j++;              // 跳过空参数
        if (j < n) {
          ensure_pal(T);
          uint32_t c = mk_pal(p[j]);
          if (fg) { T->s_fg = c; T->s_fg_idx = (p[j] < 16) ? p[j] : -1; }
          else { T->s_bg = c; T->s_bg_def = false; }
          i = j;
        } else i = n;
      } else if (mode == 2) {                       // 38;2;r;g;b / 38:2::r:g:b / 38:2:0:r:g:b
        int r = -1, g = -1, b = -1;
        if (sep[i] == ':') {                        // 冒号组：统计连续 ':' 链长度
          int L = 1;
          while (i + L < n && sep[i + L - 1] == ':') L++;
          if (L - 2 >= 4) { r = p[i + 3]; g = p[i + 4]; b = p[i + 5]; i += 5; }   // [cs,r,g,b]
          else if (L - 2 == 3) { r = p[i + 2]; g = p[i + 3]; b = p[i + 4]; i += 4; } // [r,g,b]
          else { i += L - 1; continue; }
        } else if (i + 4 < n) {                     // 分号形式：紧跟 r;g;b
          r = p[i + 2]; g = p[i + 3]; b = p[i + 4]; i += 4;
        } else { i = n; continue; }
        uint32_t c = mk_col(val(r), val(g), val(b), false);
        if (fg) { T->s_fg = c; T->s_fg_idx = -1; } else { T->s_bg = c; T->s_bg_def = false; }
      }
    }
  }
}

// DA2（CSI > c）的 Pv：软件版本号。按 CARDTERM_VERSION 的 major.minor.patch 编码为
//   major*100 + minor*10 + patch        （0.7.2 → 72，与本仓库版本号段均为个位数相符）
// 若将来出现 >=10 的段，应改成 xterm 式的 major*10000+minor*100+patch（并同步注释）。
static int da2_pv()
{
#ifdef CARDTERM_VERSION
  int a = 0, b = 0, c = 0;
  if (std::sscanf(CARDTERM_VERSION, "%d.%d.%d", &a, &b, &c) < 2) return 0;   // 解析失败：0=未知
  if (a < 0) a = 0;
  if (b < 0) b = 0;
  if (c < 0) c = 0;
  return (a % 10) * 100 + (b % 10) * 10 + (c % 10);
#else
  return 71;      // 非常规构建（未定义 CARDTERM_VERSION）：固定报 0.7.1
#endif
}

// DECSTR（CSI ! p，'!' 是 intermediate 字节）：软复位到"新终端"的默认渲染/模式状态。
// 与 RIS 的区别：不清屏、不清回滚、不移动光标、不动备用屏/调色板。
// 显式复位项（对齐 DEC STD 070 与用户需求）：
//   光标颜色/形状/闪烁/显隐回配置默认；SGR 复位（fg=default、fg_idx=7、bg_def、rev/bold 清）；
//   滚动区=全屏；插入模式关；DECAWM/DECCKM 回默认；G0/G1=ASCII、SO 关；延迟环绕清除；
//   DECSC 保存态清成默认值。不改 OSC 10/11 设定的默认 fg/bg（那是"配置"而非序列状态）。
static void term_decstr()
{
  cursor_style_default(T);            // 光标颜色/形状/闪烁/显隐
  save_state_default(T);              // DECSC 保存态
  T->s_fg_idx = 7;
  T->s_fg = T->s_def_fg;
  T->s_bold = false;
  T->s_rev = false;
  T->s_bg_def = true;
  T->s_top = 0;
  T->s_bot = kRows - 1;
  T->s_insert_mode = false;
  T->s_autowrap = true;
  T->s_app_cursor = false;
  T->s_g0 = 0; T->s_g1 = 0; T->s_so = false;
  T->s_wrap_pending = false;
  T->s_esc_delim = 0;
  mark_row(T->s_row);                 // 光标形状/颜色变了：该行整行重合成（内容本身不变）
}

void csi_exec(const char* body, int len, char fin)
{
  // 移动类序列清除"延迟环绕"挂起态；SGR/ED/EL/擦除/查询/模式类不清。
  // 'p' 也归"查询类"（DECRQM 查询不该清挂起态）；DECSTR（CSI ! p）在分支里自行清除。
  switch (fin) {
    case 'm': case 'n': case 'c': case 'q': case 'h': case 'l':
    case 'J': case 'K': case 'X': case '@': case 'P': case 'L': case 'M':
    case 'u': case 's': case 'p':
      break;
    default:
      T->s_wrap_pending = false;
      break;
  }
  int  p[32]; char sep[32]; int n = 0, cur = 0;
  bool has = false;
  for (int i = 0; i < len; i++) {
    char ch = body[i];
    if (ch == ';' || ch == ':') {
      if (n < 32) { p[n] = has ? cur : -1; sep[n] = ch; n++; }
      cur = 0; has = false;
    } else if (ch >= '0' && ch <= '9') { if (cur < 1000000) cur = cur * 10 + (ch - '0'); has = true; }  // 钳位防溢出
  }
  if (len > 0 && (has || body[len - 1] == ';' || body[len - 1] == ':')) {
    if (n < 32) { p[n] = has ? cur : -1; sep[n] = 0; n++; }
  }

  switch (fin) {
    case 'm':
      if (n == 0) { int z = 0; char zs = 0; sgr(&z, &zs, 1); }   // ESC[m = 复位
      else sgr(p, sep, n);
      break;
    case 'H': case 'f': {                            // CUP 行;列（1 基）
      follow_output();
      int r = (n > 0 && p[0] > 0) ? p[0] - 1 : 0;
      int c = (n > 1 && p[1] > 0) ? p[1] - 1 : 0;
      T->s_row = r < kRows ? r : kRows - 1;
      T->s_col = c < kCols ? c : kCols - 1;
      break;
    }
    case 'A': follow_output(); T->s_row -= ((n > 0 && p[0] > 0) ? p[0] : 1); if (T->s_row < 0) T->s_row = 0; break;
    case 'B': follow_output(); T->s_row += ((n > 0 && p[0] > 0) ? p[0] : 1); if (T->s_row >= kRows) T->s_row = kRows - 1; break;
    case 'C': follow_output(); T->s_col += ((n > 0 && p[0] > 0) ? p[0] : 1); if (T->s_col >= kCols) T->s_col = kCols - 1; break;
    case 'D': follow_output(); T->s_col -= ((n > 0 && p[0] > 0) ? p[0] : 1); if (T->s_col < 0) T->s_col = 0; break;
    case 'J':                                        // ED：0=光标后 1=光标以上 2=整屏 3=清回滚
      follow_output();
      T->s_wrap_pending = false;                       // STD-070：擦除类取消延迟环绕
      if (n == 0 || p[0] == 0)           clear_region(T->s_row, T->s_col, kRows, kCols);
      else if (p[0] == 1) {
        clear_region(0, 0, T->s_row, kCols);
        clear_region(T->s_row, 0, T->s_row + 1, T->s_col + 1);
      } else if (p[0] == 2) { clear_region(0, 0, kRows, kCols); }   // ED2 不动光标（xterm/fbterm 语义）
      else if (p[0] == 3) {                              // 清回滚缓冲
        hist_clear(T);
        T->s_view = 0; T->s_view_anchor = -1; T->s_view_pending = false;   // 视图复位（否则翻页假死）
        mark_all();                                                  // 整屏重画会覆盖旧光标
        cursor_forget();
        if (!s_quiet) present();
      }
      break;
    case 'K':                                        // EL：0=光标后 1=光标前 2=整行
      follow_output();
      T->s_wrap_pending = false;
      if (n == 0 || p[0] == 0)      clear_region(T->s_row, T->s_col, T->s_row + 1, kCols);
      else if (p[0] == 1)           clear_region(T->s_row, 0, T->s_row + 1, T->s_col + 1);
      else if (p[0] == 2)           clear_region(T->s_row, 0, T->s_row + 1, kCols);
      break;
    case 'G':                                        // CHA：列（1 基）
      follow_output();
      T->s_col = (n && p[0] > 0) ? p[0] - 1 : 0;
      if (T->s_col >= kCols) T->s_col = kCols - 1;
      break;
    case 'd':                                        // VPA：行（1 基）
      follow_output();
      T->s_row = (n && p[0] > 0) ? p[0] - 1 : 0;
      if (T->s_row >= kRows) T->s_row = kRows - 1;
      break;
    case 's': T->s_save_row = T->s_row; T->s_save_col = T->s_col;
              T->s_save_fg = T->s_fg; T->s_save_bg = T->s_bg; T->s_save_bgdef = T->s_bg_def;
              T->s_save_fgidx = T->s_fg_idx; T->s_save_bold = T->s_bold; T->s_save_rev = T->s_rev;
              T->s_save_wrp = T->s_wrap_pending; break;
    case 'u':
      if (len > 0 && (body[0] == '=' || body[0] == '>' || body[0] == '<' || body[0] == '?')) {
        // kitty 键盘协议控制序列（foot 语义）：
        //   CSI = flags ; mode u  mode1=全设 2=置位 3=清位
        //   CSI > flags u        压栈并设为 flags
        //   CSI < [n] u          出栈 n 层（默认 1；空栈得 0）
        //   CSI ? u              查询 → CSI ? flags u
        const uint32_t f = (n > 0 && p[0] > 0) ? ((uint32_t)p[0] & 0x1Fu) : 0;
        if (body[0] == '=') {
          const int mode = (n > 1 && p[1] > 0) ? p[1] : 1;
          if      (mode == 1) T->s_kkp_flags = f;
          else if (mode == 2) T->s_kkp_flags |= f;
          else if (mode == 3) T->s_kkp_flags &= ~f;
        } else if (body[0] == '>') {
          if (T->s_kkp_sp < (int)(sizeof T->s_kkp_stack / sizeof T->s_kkp_stack[0]))
            T->s_kkp_stack[T->s_kkp_sp++] = T->s_kkp_flags;
          T->s_kkp_flags = f;
        } else if (body[0] == '<') {
          int cnt = (n > 0 && p[0] > 0) ? p[0] : 1;
          while (cnt-- > 0)
            T->s_kkp_flags = (T->s_kkp_sp > 0) ? T->s_kkp_stack[--T->s_kkp_sp] : 0;
        } else {                                        // '?'：查询
          char b[24];
          int m2 = snprintf(b, sizeof b, "\033[?%uu", (unsigned)T->s_kkp_flags);
          if (m2 > 0) reply(b, (size_t)m2);
        }
        break;
      }
      follow_output();
      T->s_wrap_pending = false;
      T->s_row = T->s_save_row; T->s_col = T->s_save_col;
      T->s_fg = T->s_save_fg; T->s_bg = T->s_save_bg; T->s_bg_def = T->s_save_bgdef;
      T->s_fg_idx = T->s_save_fgidx; T->s_bold = T->s_save_bold; T->s_rev = T->s_save_rev;
      T->s_wrap_pending = T->s_save_wrp;
      T->s_row = T->s_row < 0 ? 0 : (T->s_row >= kRows ? kRows - 1 : T->s_row);
      T->s_col = T->s_col < 0 ? 0 : (T->s_col >= kCols ? kCols - 1 : T->s_col);
      break;
    case 'X': {                                      // ECH：擦除 n 个字符
      follow_output();
      T->s_wrap_pending = false;
      int k = (n && p[0] > 0) ? p[0] : 1;
      if (k > 0) { break_wide(T->s_col, T->s_row); break_wide(T->s_col + k - 1, T->s_row); }
      for (int i = 0; i < k && T->s_col + i < kCols; i++)
        buf_row(T, T->s_row).put(T->s_col + i, ' ', cell_fg(), cell_bg());
      mark_row(T->s_row);
      break;
    }
    case '@': case 'P': {                            // ICH/DCH：插入/删除 n 个字符
      follow_output();
      T->s_wrap_pending = false;
      int k = (n && p[0] > 0) ? p[0] : 1;
      {
        LiveRowRef r = buf_row(T, T->s_row);
        if (fin == '@') {
          for (int x = kCols - 1; x >= T->s_col + k; x--) {
            r.ch[x] = r.ch[x - k]; r.fg[x] = r.fg[x - k]; r.bg[x] = r.bg[x - k];
          }
          r.fill_span(T->s_col, k, ' ', cell_fg(), cell_bg());
        } else {
          for (int x = T->s_col; x < kCols; x++) {
            int src = x + k;
            r.ch[x] = (src < kCols) ? r.ch[src] : (uint16_t)' ';
            r.fg[x] = (src < kCols) ? r.fg[src] : cell_fg();
            r.bg[x] = (src < kCols) ? r.bg[src] : cell_bg();
          }
        }
        repair_pairs(buf_row(T, T->s_row));                // 位移断开的宽字符对清理
      }
      mark_row(T->s_row);
      break;
    }
    case 'L': case 'M': {                            // IL/DL：插入/删除 n 行（到滚动区域底）
      follow_output();
      if (T->s_row < T->s_top || T->s_row > T->s_bot) break;   // 区域外：忽略
      int k = (n && p[0] > 0) ? p[0] : 1;
      const int bot = T->s_bot;
      if (k > bot - T->s_row + 1) k = bot - T->s_row + 1;
      if (fin == 'L') {
        for (int y = bot; y >= T->s_row + k; y--) buf_row(T, y).move_from(buf_row(T, y - k));
      } else {
        for (int y = T->s_row; y + k <= T->s_bot; y++) buf_row(T, y).move_from(buf_row(T, y + k));
      }
      int y0 = (fin == 'L') ? T->s_row : bot - k + 1;
      int y1 = (fin == 'L') ? T->s_row + k : bot + 1;
      for (int y = y0; y < y1; y++) {
        buf_row(T, y).fill_span(0, kCols, ' ', cell_fg(), cell_bg());
        buf_row(T, y).flags = 0;
      }
      mark_rows(T->s_row, bot + 1);
      break;
    }
    case 'r': {                                      // DECSTBM：滚动区域
      int t = (n > 0 && p[0] > 0) ? p[0] - 1 : 0;
      int b = (n > 1 && p[1] > 0) ? p[1] - 1 : kRows - 1;
      if (t < 0) t = 0;
      if (b >= kRows) b = kRows - 1;
      if (t < b) { T->s_top = t; T->s_bot = b; }
      else { T->s_top = 0; T->s_bot = kRows - 1; }
      follow_output();
      T->s_row = 0; T->s_col = 0;          // DECSTBM：光标回屏首（1,1），不是区域首
      break;
    }
    case 'n': {                                      // DSR / DECXCPR
      char b[32];
      const bool priv = (len > 0 && body[0] == '?');
      if (n > 0 && p[0] == 6) {
        const int len2 = priv
            // DECXCPR（CSI ? 6 n）：应答必须带 '?' 前缀和页号字段 → CSI ? row ; col ; 1 R，
            // 不能复用普通 CPR（CSI row;col R），否则应用会按错误格式解析。
            ? snprintf(b, sizeof b, "\033[?%d;%d;1R", T->s_row + 1, T->s_col + 1)
            : snprintf(b, sizeof b, "\033[%d;%dR", T->s_row + 1, T->s_col + 1);
        if (len2 > 0) reply(b, (size_t)len2);
      } else if (!priv && n > 0 && p[0] == 5) {      // DSR 5：设备状态正常
        reply("\033[0n", 4);
      }
      break;
    }
    case 'c': {                                      // DA：主/次/三级设备属性
      if (len > 0 && body[0] == '>') {
        // DA2（CSI > Pp Pv Pc c）：
        //   Pp = 0 —— 不冒充任何 DEC 型号（xterm 报 41=VT420，foot 报 1=VT220）；
        //        0 在 DEC 语义里会落到"未知/VT100"，读端据此走保守路径，正好表达
        //        "非 xterm 但 ANSI 兼容"的定位（值 41 会诱使应用走 xterm 私有路径）。
        //   Pv = da2_pv()（CARDTERM_VERSION 编码，0.7.1 → 71；见 da2_pv 注释）
        //   Pc = 0（无 ROM 卡号）
        if (n > 1 || (n == 1 && p[0] != 0)) break;  // 只答查询形式 CSI > c / CSI > 0 c
        char b[32];
        const int len2 = snprintf(b, sizeof b, "\033[>0;%d;0c", da2_pv());
        if (len2 > 0) reply(b, (size_t)len2);
        break;
      }
      if (len > 0 && (body[0] == '?' || body[0] == '=')) break;  // 私有 DA / DA3 不是 DA1 查询
                                                                 // （DA3 的 CSI = c 本终端无 DCS !| 标识，宁可不答）
      if (len > 0 && (body[0] < '0' || body[0] > '9')) break;    // 未知前缀：不误答
      if (n > 1 || (n == 1 && p[0] != 0)) break;    // DA1 只答查询形式 CSI c / CSI 0 c
      // DA1（CSI ? Ps... c）"诚实版"——如实报告本终端能力，而不是只报一个 VT102 位：
      //   62  VT220 级基准（必须最先给；表示支持 ANSI 参数化/扩展序列。本终端实现了
      //       VT220 级绝大多数控制序列，与 foot 报 62 同理；未实现 VT420 矩形编辑，
      //       故不报 64/65）
      //   6   选择性擦除（DECSCA/DECSED/DECSEL）：本终端没有"受保护单元"，
      //       所有单元都可擦 → CSI ? J / CSI ? K 与 ED/EL 等价、语义正确，如实报此位
      //   22  ANSI 颜色（SGR 16/256/真彩）：完整支持（见 sgr / s_pal4 / 38;2）。
      //       DA1 没有 256 色/真彩的独立位，细粒度由 COLORTERM 环境与 terminfo 表达
      //   52  剪贴板访问（OSC 52）：已实现 set/query（clip_store / osc52）
      // 不报：4(Sixel)/8(UDK)/18(窗口操作)/28(矩形编辑) 等未实现能力；kitty 键盘协议
      // 也没有 DA1 特性位（走 CSI ? u 查询），不在此杜撰。
      static const char kDa1[] = "\033[?62;6;22;52c";
      reply(kDa1, sizeof kDa1 - 1);
      break;
    }
    case 'q': {                                      // DECSCUSR：光标形状
      if (len > 0 && body[0] == '>') break;            // CSI > Ps q = XTVERSION 探测：不改光标形状
      int st = (n > 0 && p[0] >= 0) ? p[0] : 1;
      if (st > 6) st = 1;
      T->s_cur_style = st;
      T->s_cursor_blink = (st == 0 || st == 1 || st == 3 || st == 5);
      if (!T->s_cursor_blink) T->s_cursor_on = true;
      mark_row(T->s_row);                               // 形状变了 → 重画该行（含光标）
      break;
    }
    case 'Z': {                                      // CBT：后退 n 个制表位
      follow_output(); T->s_wrap_pending = false;
      int kk = (n && p[0] > 0) ? p[0] : 1;
      if (kk > kCols) kk = kCols;                      // 钳位：否则 10^7 次循环会冻结主线程
      T->s_col = prev_tab(kk);
      break;
    }
      break;
    case 'I': {                                      // CHT：前进 n 个制表位
      follow_output(); T->s_wrap_pending = false;
      int kk = (n && p[0] > 0) ? p[0] : 1;
      if (kk > kCols) kk = kCols;
      T->s_col = next_tab(kk);
      break;
    }
      break;
    case 'S': {                                      // SU：滚动区域上滚 n 行
      follow_output();
      int k = (n && p[0] > 0) ? p[0] : 1;
      if (k > kRows) k = kRows;
      while (k-- > 0) scroll_up();
      break;
    }
    case 'T': {                                      // SD：滚动区域下滚 n 行
      follow_output();
      int k = (n && p[0] > 0) ? p[0] : 1;
      if (k > kRows) k = kRows;
      while (k-- > 0) scroll_down();
      break;
    }
    case 'b': {                                      // REP：重放最近打印字符 n 次
      follow_output();
      int k = (n && p[0] > 0) ? p[0] : 1;
      if (k > kCols * kRows) k = kCols * kRows;
      while (k-- > 0) {
        if (T->s_last_print >= 0x20 && T->s_last_print <= 0xFFFF) {
          const termfont::TermGlyph* g = termfont::find(*s_font, (uint16_t)T->s_last_print);
          if (g != nullptr) put_glyph((uint16_t)T->s_last_print, g->cells);
          else put_tofu(T->s_last_print_w >= 2 ? 2 : 1);
        } else {
          put_tofu(T->s_last_print_w >= 2 ? 2 : 1);  // 重放豆腐块占位
        }
      }
      break;
    }
    case 'g':                                        // TBC：清除制表位（0=当前列 3=全部）
      if (n == 0 || p[0] == 0) T->s_tabs[T->s_col] = false;
      else if (p[0] == 3) memset(T->s_tabs, 0, sizeof T->s_tabs);
      break;
    case 'E':                                        // CNL：下移 n 行并回行首
      follow_output(); T->s_wrap_pending = false;
      T->s_row += (n && p[0] > 0) ? p[0] : 1;
      if (T->s_row >= kRows) T->s_row = kRows - 1;
      T->s_col = 0;
      break;
    case 'F':                                        // CPL：上移 n 行并回行首
      follow_output(); T->s_wrap_pending = false;
      T->s_row -= (n && p[0] > 0) ? p[0] : 1;
      if (T->s_row < 0) T->s_row = 0;
      T->s_col = 0;
      break;
    case 'p':                                        // DECSTR（CSI ! p）/ DECRQM（CSI ? Ps $ p）
      // '!' 是 intermediate 字节（0x21），解析器把它原样收进 body，最终字节才是 'p'。
      if (len > 0 && body[0] == '!') { term_decstr(); break; }
      if (len <= 0) break;                           // 空 body：不是 DECRQM
      // 注意：body 不是以 len 结尾的 C 字符串（缓冲里可能残留上一条更长的序列），
      // 必须按 len 用 memchr 找 '$'，否则可能把上一条的 '$' 当成本条 DECRQM。
      if (memchr(body, '$', (size_t)len) != nullptr) {
        const bool priv = (len > 0 && body[0] == '?');
        const int ps = (n > 0 && p[0] >= 0) ? p[0] : 0;
        int state = 0;                               // 0=未识别 1=置位 2=复位
        if (priv) {
          if      (ps == 1)   state = T->s_app_cursor ? 1 : 2;
          else if (ps == 7)   state = T->s_autowrap ? 1 : 2;
          else if (ps == 25)  state = T->s_cursor_vis ? 1 : 2;
          else if (ps == 47 || ps == 1047 || ps == 1049) state = T->s_alt_active ? 1 : 2;
          else if (ps == 2004 || ps == 2026 || ps == 1000 || ps == 1002 ||
                   ps == 1003 || ps == 1006) state = 2;
        } else if (ps == 4) state = T->s_insert_mode ? 1 : 2;
        char b[32];
        int m2 = snprintf(b, sizeof b, priv ? "\033[?%d;%d$y" : "\033[%d;%d$y", ps, state);
        if (m2 > 0) reply(b, (size_t)m2);
      }
      break;
    case 'h': case 'l': {                            // DEC 私有模式 / ANSI 模式
      const bool on = (fin == 'h');
      if (len > 0 && body[0] == '?') {
        for (int i = 0; i < n; i++) {
          if (p[i] == 25) {                          // DECTCEM：光标可见
            T->s_cursor_vis = on;
            mark_row(T->s_row);                         // 显隐变化 → 重画该行
          } else if (p[i] == 7) {                    // DECAWM：自动换行
            T->s_autowrap = on;
          } else if (p[i] == 1) {                    // DECCKM：应用光标键模式
            T->s_app_cursor = on;                       // （应用置位后箭头发 SS3 序列）
          } else if (p[i] == 1049 || p[i] == 1047 || p[i] == 47) {  // 备用屏
            if (on && !T->s_alt_active) {
              for (int y = 0; y < kRows; y++) {
                LiveRowRef r = buf_row(T, y);
                saved_row(T, y).flags = r.flags;
                memcpy(saved_row(T, y).ch, r.ch, (size_t)kCols * sizeof(uint16_t));
                memcpy(saved_fg(T, y), r.fg, (size_t)kCols * sizeof(uint32_t));
                memcpy(saved_bg(T, y), r.bg, (size_t)kCols * sizeof(uint32_t));
              }
              T->s_saved_row = T->s_row; T->s_saved_col = T->s_col;
              T->s_alt_active = true;
              for (int y = 0; y < kRows; y++) {
                buf_row(T, y).fill_span(0, kCols, ' ', cell_fg(), cell_bg());
                buf_row(T, y).flags = 0;
              }
              T->s_row = 0; T->s_col = 0; mark_all();       // 整屏重画覆盖旧光标 → 记录作废
              cursor_forget();
            } else if (!on && T->s_alt_active) {
              for (int y = 0; y < kRows; y++) {
                LiveRowRef r = buf_row(T, y);
                r.flags = saved_row(T, y).flags;
                memcpy(r.ch, saved_row(T, y).ch, (size_t)kCols * sizeof(uint16_t));
                memcpy(r.fg, saved_fg(T, y), (size_t)kCols * sizeof(uint32_t));
                memcpy(r.bg, saved_bg(T, y), (size_t)kCols * sizeof(uint32_t));
              }
              T->s_row = T->s_saved_row; T->s_col = T->s_saved_col;
              T->s_alt_active = false;
              mark_all();                                    // 整屏重画覆盖旧光标 → 记录作废
              cursor_forget();
            }
          }
        }
      } else {                                       // ANSI 模式：IRM 插入模式等
        for (int i = 0; i < n; i++)
          if (p[i] == 4) T->s_insert_mode = on;
      }
      break;
    }
    default: break;
  }
}

}  // namespace

// 立即上屏（公开 API，见 terminal.h）：输出暂停/交互操作时调用
void present() { present_now(); }

// ── toast 叠加层公开 API（实现见上：toast_show / toast_paint / toast_clear_region）──
// 显示一条 toast：立即上屏并开始计时，到期由 tick() 恢复下方内容。
// text 为空/T 为空/总开关关闭/后台静默解析时直接返回，不产生任何绘制。
void toast(const char* text, int ms) { toast_show(text, ms); }

// 设置 toast 默认停留时长（进程级，见 terminal.h）：toast(text) 或显式
// ms<=0 时取用；ms<=0 回退 2000（与旧的固定默认一致）。已在屏上的 toast
// 不受影响（until 已定时长，只影响此后新建的 toast）。
void set_toast_default_ms(int ms)
{
  s_toast_default_ms = (ms > 0) ? ms : 2000;
}

// toast 总开关：关闭后 toast() 直接返回；若屏上已有 toast，立即恢复被覆盖区域。
void set_toast_enabled(bool on)
{
  s_toast.enabled = on;
  if (!on) toast_clear_region();
}

// 屏上当前是否有 toast（session 可用它避免重复提示/决定是否强行 present）。
bool toast_active() { return s_toast.visible; }

// ── 全屏倒计时退出提示（req 2/3 共用：剥离/关闭最后一个窗口）────────
// 独立绘制（不依赖终端状态 T，窗口已关闭也可能调用）：整屏铺深底色 →
// 14px 边距、2px 边框矩形（toast 同款配色：bright0 底 / bright4 边框 /
// bright7 文字）→ 三行 8x14 文本水平垂直居中。第三行 "cardterm will exit
// in Ns." 逐秒重绘 3→2→1，各显示满 1 秒后返回（调用方随即退出程序）。
void exit_prompt(const char* line1)
{
  const uint16_t bd = to565(0x5AC7FF);              // bright4：边框
  const uint16_t bg = to565(0x636163);              // bright0：框内背景
  const uint16_t fg = to565(0xFFFFFF);              // bright7：文字
  const int M = 14, B = 2;
  const int x = M, y = M, w = kCanvasW - 2 * M, h = kCanvasH - 2 * M;
  tft.fillRect(0, 0, kCanvasW, kCanvasH, to565(0x242424));   // 盒外深底
  fill_round_rect(x, y, w, h, kToastR, bd);          // 圆角边框矩形（同 toast 圆角）
  fill_round_rect(x + B, y + B, w - 2 * B, h - 2 * B, kToastR - B, bg);

  const char* l2 = "No terminal running.";
  char l3[64];
  const int gap = 10;                                // 行间距
  const int total = 3 * kToastTextH + 2 * gap;       // 三行总高
  const int ih = h - 2 * B;
  for (int n = 3; n >= 1; n--) {
    snprintf(l3, sizeof l3, "cardterm will exit in %ds.", n);
    const char* texts[3] = { line1, l2, l3 };
    int ty = y + B + (ih - total) / 2;
    for (int i = 0; i < 3; i++) {
      toast_layout(texts[i]);                        // 复用 toast 的 8x14 排版
      const int tx = x + B + (w - 2 * B - s_toast.textw) / 2;   // 水平居中
      if (s_toast.textw > 0 && tx >= x + B) toast_draw_text(tx, ty, fg, bg);
      ty += kToastTextH + gap;
    }
    usleep(1000000);                                 // 每档显示满 1 秒
  }
}

// 视图立即跳回底部（公开 API）：翻页以外按键/切窗目标时由 session 调用
void view_follow() { view_follow_now(); }

// 光标颜色/形状/闪烁/显隐恢复配置默认（公开 API，见 terminal.h）。
// 供"子程序结束 / 窗口回到启动器"等 session 侧出口复用；与本文件内 RIS/DECSTR/OSC 112
// 共用 cursor_style_default()。只重画光标所在行：不改屏内容、不动滚动位置与光标位置。
// 实现要点：先让记录里的旧光标行标脏（下帧整行重合成 → 旧方块被彻底擦掉、不会残留），
// 再立即上屏一次让新颜色生效（只刷一行，开销可忽略）。
void reset_cursor_style()
{
  if (T == nullptr) return;
  cursor_style_default(T);
  cursor_dirty_for_repaint();
  if (!s_quiet) present_now();
}

// 每窗口状态生命周期（0.3.0 多窗口）：
//   state_new 创建并复位一份窗口状态（背景画布全局共享，仅首次加载）；
//   activate 设为当前解析/绘制目标；state_free 释放。
State* state_new(const Options& opt)
{
  State* s = new State();
  State* prev = T;
  const int g_cols = kCols, g_rows = kRows, g_cw = kCellW, g_ch = kCellH;
  const termfont::FontTable* g_font = s_font;
  T = s;                                       // 初始化期间以此状态为当前
  apply_grid(s, opt.font_size);                // 网格/字体表 = 字号
  static const uint32_t kDefaultPal[16] = {      // foot 终端默认配色（RGB888）
    0x212421, 0xF7285A, 0x42B610, 0xE7C700, 0x21AED6, 0xF7AEFF, 0x10C39C, 0xE7E7E7,
    0x636163, 0xFF4D52, 0x31D752, 0xEFEB31, 0x5AC7FF, 0xFFAAF7, 0x21DFC6, 0xFFFFFF,
  };
  memcpy(s->s_pal, opt.palette16 ? opt.palette16 : kDefaultPal, sizeof(s->s_pal));
  s->s_def_fg = s->s_pal[7];                         // 默认前景 = regular7
  s->s_cursor_col = opt.cursor_color ? opt.cursor_color : s->s_pal[15];  // 光标 = config cursor（默认 bright7）
  // 配置默认色快照：OSC 10/11/12 只改运行时值，OSC 110/111/112 与各种复位按此恢复
  s->s_def_fg0 = s->s_def_fg;
  s->s_cursor_col_def = s->s_cursor_col;
  int hb = opt.scrollback > 0 ? opt.scrollback : 2000;
  if (hb < 100) hb = 100;               // 配置少于 100 行也至少缓存 100 行
  s->s_hist_max = hb;
  s->s_view_hold_ms = opt.view_hold_ms > 0 ? opt.view_hold_ms : 2000;
  s->s_bg = opt.bg_color;
  s->s_def_bg = opt.bg_color;                  // 反显时充当前景的默认底色
  s->s_def_bg0 = s->s_def_bg;                   // 配置默认背景快照（OSC 111 复位用）
  s->s_fg_idx = 7;
  s->s_fg = s->s_def_fg;                     // 默认字符色 = regular7
  s->s_bold = false;
  s->s_bg_def = true;

  // 底图画布：主副本仅首次构建（默认配置派生），每窗口独立拷贝（不共享）
  if (!s_canvas_master) {
    s_canvas_master.reset(new (std::nothrow) uint16_t[(size_t)kCanvasW * kCanvasH]);
    if (!s_canvas_master) { T = prev; kCols = g_cols; kRows = g_rows; kCellW = g_cw; kCellH = g_ch; s_font = g_font; delete s; return nullptr; }
    bool image_ok = false;
    if (opt.bg_image != nullptr && *opt.bg_image != '\0')
      image_ok = load_image_to_canvas(opt.bg_image);
    if (!image_ok) {
      const uint16_t bg565 = to565(s->s_bg);
      for (int i = 0; i < kCanvasW * kCanvasH; i++) s_canvas_master[i] = bg565;
    }
  }
  s->s_canvas.reset(new (std::nothrow) uint16_t[(size_t)kCanvasW * kCanvasH]);
  if (!s->s_canvas) { T = prev; kCols = g_cols; kRows = g_rows; kCellW = g_cw; kCellH = g_ch; s_font = g_font; delete s; return nullptr; }
  memcpy(s->s_canvas.get(), s_canvas_master.get(), (size_t)kCanvasW * kCanvasH * 2);

  // 三宽度 Grid 分配（Row 构造自带空格+默认颜色）；shared_ptr 自定义删除器按类型释放
  switch (s->s_kind) {
    case 0:  s->s_grid = std::shared_ptr<void>(new G<40>(), [](void* q){ delete (G<40>*)q; }); break;
    case 2:  s->s_grid = std::shared_ptr<void>(new G<80>(), [](void* q){ delete (G<80>*)q; }); break;
    default: s->s_grid = std::shared_ptr<void>(new G<53>(), [](void* q){ delete (G<53>*)q; }); break;
  }
  hist_clear(s);
  tabs_reset();
  s->s_dirty = dirty_all_mask();              // 首次上屏整屏重画
  s->s_last_present = 0;
  s->s_cur_drawn_row = -1; s->s_cur_drawn_col = -1;
  T = prev;
  return s;
}

void state_free(State* s)
{
  if (s == T) T = nullptr;
  delete s;                  // RAII：s_grid(shared_ptr)/s_pal4/s_canvas(unique_ptr) 自动释放
  malloc_trim(0);        // 归还空闲堆页（否则 glibc 保留, RSS 不回落）
}

void activate(State* s)
{
  // 注意：这里**不**清光标记录。session::pump_all 解析后台窗口时会用
  // activate 临时把 T 切到别的窗口再切回来（不重画、不标脏），若在此清记录，
  // 切回后 dirty==0 又"该有光标却无记录"，present 只补画当前行 →
  // 屏上仍留着光标原来那一行/那一格的方块 → 双光标。
  // "切窗"的真正失效点放在 fill_bg()/invalidate()（可见画面被整屏重画时）里。
  T = s;
  if (s == nullptr) return;
  kCols = s->s_cols; kRows = s->s_rows;
  kCellW = s->s_cell_w; kCellH = s->s_cell_h;
  s_font = s->s_font;
}

// ── 字号切换 = rewrap：新建目标宽度窗口 → 复制数据流 → 删旧窗 ─────────
// 数据流 = 历史行 + 屏幕行，按 wrapped 标志合并逻辑行（软换行），
// 宽字符对折叠成一个 2 列单元；逻辑行尾部空白裁剪；在新宽度重新折行
// （放不下的单元触发软换行并打 wrapped 标记）。回滚历史随流保留。
// 备用屏内容不 rewrap（foot/VTE 惯例），丢弃。返回新 State*（调用方替换）。
State* state_resize(State* s, int font_size)
{
  if (s == nullptr || font_size == s->s_font_size) return s;

  struct SC { uint16_t ch; uint32_t fg, bg; uint8_t w; };   // w: 占列数 1/2
  // C1：备用屏活动时，主屏历史（s_hist）与备用屏活动区（s_buf）是两条互不相干的
  // 数据流，绝不能并成一条流重折行——否则 alt 内容会永久混入主屏回滚历史。
  const bool alt = s->s_alt_active;

  // ① 收集逻辑行 + 光标 (逻辑行, 列偏移)
  // 扁平存储：所有单元连续放在 cpool，逻辑行 = (start,len) 区间
  // （替代每行一个小 vector——3 万行即 3 万小块堆分配，缩放后 arena 驻留 ~40MB）
  std::vector<SC> cpool;
  cpool.reserve((size_t)(hist_size(s) + s->s_rows) * (size_t)s->s_cols + 64);  // 免倍增
  std::vector<std::pair<int,int>> lspans;                 // 每逻辑行 (start,len)
  int cur_line = -1, cur_col = 0, cur_cell = 0;
  const int cursor_gi = alt ? s->s_row : hist_size(s) + s->s_row;
  // 回看视图锚定迁移：记录旧视图首行所在逻辑行，rewrap 后映射到新输出行（防缩放后回看跳行）
  const int old_total = hist_size(s) + s->s_rows;
  int old_top_gi = -1, old_top_li = -1;
  if (!alt && s->s_view > 0) {                    // C1：备用屏无回滚视图语义，不迁移锚点
    old_top_gi = (s->s_view_anchor >= 0) ? s->s_view_anchor : (old_total - s->s_view - s->s_rows);
    if (old_top_gi < 0) old_top_gi = 0;
    if (old_top_gi > old_total - 1) old_top_gi = old_total - 1;
  }
  int wsum = 0;                                           // 当前逻辑行已收单元的宽度和（折行 lx 同单位）
  bool cont = false;
  int gi = 0;
  auto add_row = [&](const uint16_t* chp, uint8_t fl, const uint32_t* fgp,
                     const uint32_t* bgp, int ncols) {
    if (!cont) { lspans.emplace_back((int)cpool.size(), 0); wsum = 0; }
    cont = (fl & 1) != 0;
    auto& sp = lspans.back();
    const int row_base = sp.second;                       // 本逻辑行内、本行之前的单元数
    const int wbase = wsum;                               // 本逻辑行内、本行之前的宽度和
    if (gi == old_top_gi) old_top_li = (int)lspans.size() - 1;
    for (int x = 0; x < ncols; x++) {
      uint16_t ch = chp[x];
      if (ch == kChTofuR) continue;                       // 右半并入前一单元
      bool wide = (x + 1 < ncols && chp[x + 1] == kChTofuR);
      cpool.push_back({ch, fgp[x], bgp[x], (uint8_t)(wide ? 2 : 1)});
      sp.second++;
      wsum += wide ? 2 : 1;
    }
    if (gi == cursor_gi) {
      cur_line = (int)lspans.size() - 1;
      int off = 0, ncell = 0;
      for (int x = 0; x < s->s_col && x < ncols; x++) {
        if (chp[x] == kChTofuR) continue;
        off += (x + 1 < ncols && chp[x + 1] == kChTofuR) ? 2 : 1;
        ncell++;
      }
      cur_col = wbase + off;                              // 逻辑行内累计【宽度】偏移（与折行 lx 同单位）
      cur_cell = row_base + ncell;                        // 逻辑行内累计【单元】位置（尾部裁剪下限用）
    }
    gi++;
  };
  if (!alt)                    // C1：备用屏活动 → 主屏历史不并入活动流（下方④逐行迁移）
  for (int j = 0; j < hist_size(s); j++) {                 // 历史行：RLE 解码一次
    RowB& h = hist_row(s, j);
    uint32_t fq[kMaxCols], bq[kMaxCols];
    h.decode(fq, bq);
    add_row(h.ch, h.flags, fq, bq, h.cols);
  }
  for (int y = 0; y < s->s_rows; y++) {                    // 活动行：颜色区直读
    LiveRowRef r = buf_row(s, y);
    add_row(r.ch, r.flags, r.fg, r.bg, r.row->cols);
  }
  for (int li = 0; li < (int)lspans.size(); li++) {        // 裁逻辑行尾空白（透明底）
    auto& sp = lspans[li];
    const int floor = (li == cur_line) ? cur_cell : 0;     // 光标所在行：裁剪不得越过光标单元（否则光标悬空→匹配失败→落行 0）
    while (sp.second > floor) {
      const SC& b = cpool[sp.first + sp.second - 1];
      if (b.ch == ' ' && is_trans_bg(b.bg)) sp.second--; else break;
    }
  }

  // ② 新窗口（新宽度）+ 复制属性
  const int old_view = s->s_view;
  State* ns = new State();
  State* prevT = T;
  T = ns;
  apply_grid(ns, font_size);
  switch (ns->s_kind) {
    case 0:  ns->s_grid = std::shared_ptr<void>(new G<40>(), [](void* q){ delete (G<40>*)q; }); break;
    case 2:  ns->s_grid = std::shared_ptr<void>(new G<80>(), [](void* q){ delete (G<80>*)q; }); break;
    default: ns->s_grid = std::shared_ptr<void>(new G<53>(), [](void* q){ delete (G<53>*)q; }); break;
  }
  memcpy(ns->s_pal, s->s_pal, sizeof ns->s_pal);
  // 新建终端语义：所有资源与内存重新分配（内容拷贝，旧窗随后整体释放）
  if (s->s_pal4) {
    ns->s_pal4.reset(new (std::nothrow) uint32_t[256]);
    if (ns->s_pal4) memcpy(ns->s_pal4.get(), s->s_pal4.get(), 256 * sizeof(uint32_t));
  }
  if (s->s_canvas) {
    ns->s_canvas.reset(new (std::nothrow) uint16_t[(size_t)kCanvasW * kCanvasH]);
    if (!ns->s_canvas) {                       // OOM：放弃本次缩放，回滚旧状态（此前会 memcpy(nullptr) 崩溃）
      fprintf(stderr, "[term] resize OOM (canvas), keeping current font\n");
      T = prevT;
      { // A3：回滚 apply_grid 已改写的全局网格（与 state_new 失败路径一致），
        // 否则调用方 resize_pty 会按错误尺寸发 TIOCSWINSZ
        State* keep = T ? T : s;
        kCols = keep->s_cols; kRows = keep->s_rows;
        kCellW = keep->s_cell_w; kCellH = keep->s_cell_h; s_font = keep->s_font;
      }
      delete ns; return s;
    }
    memcpy(ns->s_canvas.get(), s->s_canvas.get(), (size_t)kCanvasW * kCanvasH * sizeof(uint16_t));
  }
  { // ②b 备用屏(?1049)主屏存档迁移：按列宽截断/补空（否则缩放后退不出备用屏、主屏丢失）
    const int oc = s->s_cols, nc = ns->s_cols;
    const int orr = s->s_rows, nr = ns->s_rows;
    const int m = oc < nc ? oc : nc;
    for (int y = 0; y < nr; y++) {
      RowB& d = saved_row(ns, y);
      uint32_t* df = saved_fg(ns, y); uint32_t* db = saved_bg(ns, y);
      if (y < orr) {
        RowB& src = saved_row(s, y);
        memcpy(d.ch, src.ch, (size_t)m * sizeof(uint16_t));
        memcpy(df, saved_fg(s, y), (size_t)m * sizeof(uint32_t));
        memcpy(db, saved_bg(s, y), (size_t)m * sizeof(uint32_t));
        d.flags = src.flags;
        if (m > 0 && m < oc && src.ch[m] == kChTofuR) {   // 截断处恰是宽字对右半：左半降为空格
          d.ch[m - 1] = ' '; df[m - 1] = 0xFFFFFFu; db[m - 1] = 0x80FFFFFFu;
        }
      }
      for (int x = m; x < nc; x++) { d.ch[x] = ' '; df[x] = 0xFFFFFFu; db[x] = 0x80FFFFFFu; }
    }
    ns->s_saved_row = s->s_saved_row < nr ? s->s_saved_row : nr - 1;
    ns->s_saved_col = s->s_saved_col < nc ? s->s_saved_col : nc - 1;
  }
  ns->s_def_fg = s->s_def_fg;
  ns->s_def_fg0 = s->s_def_fg0;                     // 配置默认色快照随窗口保留
  ns->s_def_bg0 = s->s_def_bg0;
  ns->s_cursor_col = s->s_cursor_col;
  ns->s_cursor_col_def = s->s_cursor_col_def;
  ns->s_bg = s->s_bg; ns->s_def_bg = s->s_def_bg;
  ns->s_fg = s->s_fg; ns->s_fg_idx = s->s_fg_idx;
  ns->s_bold = s->s_bold; ns->s_rev = s->s_rev;
  ns->s_bg_def = s->s_bg_def;
  ns->s_hist_max = s->s_hist_max;
  ns->s_view_hold_ms = s->s_view_hold_ms;  // 翻页保持时长（config）随新终端保留
  ns->s_view_hold_until = 0;               // 保持截止清零（新终端语义）
  ns->s_view_pending = false;
  ns->s_view_anchor = -1;                  // rewrap 后行映射全变，锚点失效（回退距底偏移语义）
  ns->s_font_size = font_size;
  ns->s_autowrap = s->s_autowrap;
  ns->s_app_cursor = s->s_app_cursor;
  ns->s_kkp_flags = s->s_kkp_flags;
  ns->s_writer = s->s_writer;
  ns->s_cursor_vis = s->s_cursor_vis;
  ns->s_cursor_on = s->s_cursor_on;
  ns->s_cur_style = s->s_cur_style;
  ns->s_cursor_blink = s->s_cursor_blink;
  ns->s_cursor_t0 = now_ms_();
  // 状态迁移补全（同类历史 bug 家族：漏字段 → 缩放后行为退化）
  ns->s_g0 = s->s_g0; ns->s_g1 = s->s_g1; ns->s_so = s->s_so;      // 字符集(ACS)
  ns->s_insert_mode = s->s_insert_mode;                            // IRM
  ns->s_last_print = s->s_last_print; ns->s_last_print_w = s->s_last_print_w;  // REP
  ns->s_save_row = s->s_save_row; ns->s_save_col = s->s_save_col;  // DECSC 存档
  ns->s_save_fg = s->s_save_fg; ns->s_save_bg = s->s_save_bg;
  ns->s_save_bgdef = s->s_save_bgdef; ns->s_save_fgidx = s->s_save_fgidx;
  ns->s_save_bold = s->s_save_bold; ns->s_save_rev = s->s_save_rev;
  ns->s_save_wrp = s->s_save_wrp;
  memcpy(ns->s_kkp_stack, s->s_kkp_stack, sizeof s->s_kkp_stack);  // kitty 协议栈
  ns->s_kkp_sp = s->s_kkp_sp;
  ns->s_st = s->s_st;                                              // 跨 chunk 解析器状态
  memcpy(ns->s_csi, s->s_csi, sizeof s->s_csi);
  ns->s_csi_len = s->s_csi_len;
  memcpy(ns->s_osc, s->s_osc, sizeof s->s_osc);
  ns->s_osc_len = s->s_osc_len;
  ns->s_u8_cp = s->s_u8_cp; ns->s_u8_need = s->s_u8_need;
  ns->s_esc_delim = s->s_esc_delim;                                // ESC ( / ESC ) 中间态
  ns->s_top = 0; ns->s_bot = ns->s_rows - 1;
  memset(ns->s_tabs, 0, sizeof ns->s_tabs);
  for (int x = 0; x < ns->s_cols; x++) ns->s_tabs[x] = (x > 0 && x % 8 == 0);

  // ③ 重新折行 → 输出行序列（折行只重排边界，单元序列不变 → espans 直接指向 cpool）
  std::vector<std::pair<int,int>> espans;                  // 每输出行 (cpool起,长)
  std::vector<uint8_t> ewrap;                              // 软换行标记
  std::vector<int> lstart(lspans.size(), -1);              // 每逻辑行的首个输出行（回看视图迁移用）
  int x = 0;
  int crow = 0, ccol = 0;                                // ⑤ 非 alt 分支使用 → 声明在块外
  bool cdone = (cur_line < 0);
  int cur_end_row = -1, cur_end_x = 0;                     // 光标逻辑行的末尾落点（兜底用）
  if (!alt) {   // C1：备用屏逐行迁移、不折行（espans/ewrap 留空）
  auto erow_new = [&](int off) { espans.emplace_back(off, 0); ewrap.push_back(0); };
  erow_new(lspans.empty() ? 0 : lspans[0].first);
  for (int li = 0; li < (int)lspans.size(); li++) {
    lstart[li] = (int)espans.size() - 1;                   // 本逻辑行从当前输出行开始
    int lx = 0;                                            // 逻辑行内累计偏移（光标匹配用）
    for (int k = 0; k < lspans[li].second; k++) {
      const int off = lspans[li].first + k;
      const SC& c = cpool[off];
      if (x + c.w > ns->s_cols) {                          // 软换行
        ewrap.back() = 1;
        erow_new(off);
        x = 0;
      }
      if (!cdone && li == cur_line && lx == cur_col) {     // 光标在本单元之前
        cdone = true; crow = (int)espans.size() - 1; ccol = x;
      }
      espans.back().second++;
      x += c.w;
      lx += c.w;
    }
    if (!cdone && li == cur_line && lx == cur_col) {       // 光标在行尾之后
      cdone = true; crow = (int)espans.size() - 1; ccol = x;
    }
    if (li == cur_line) { cur_end_row = (int)espans.size() - 1; cur_end_x = x; }
    if (li + 1 < (int)lspans.size()) {                     // 硬换行
      erow_new(lspans[li + 1].first);
      x = 0;
    }
  }
  // 兜底：光标列在裁剪/折行后越界（典型：提示符尾随空格被逻辑行裁剪，
  // 光标列 > 裁剪后行长）→ 落在光标逻辑行的末尾行并夹列（0.6.x 语义）。
  // 缺了这层，cdone 保持 false → crow/ccol 停在 0 → 光标被夹到屏幕顶行（0.7.0 回归）。
  if (!cdone && cur_line >= 0 && cur_end_row >= 0) {
    cdone = true;
    crow = cur_end_row;
    ccol = cur_end_x < ns->s_cols ? cur_end_x : ns->s_cols - 1;
  }
  }   // if (!alt)

  // ④ 写入新窗口：历史 = 尾部 kRows 之外的输出行，屏幕 = 尾部 kRows 行
  const int total = (int)espans.size();
  const int new_hist = total > ns->s_rows ? total - ns->s_rows : 0;
  auto fill_hist_row = [](RowB& r, const SC* cs, int len, uint8_t wrapped) {  // 历史行：RLE 压缩
    int cx = 0;
    uint32_t fq[kMaxCols], bq[kMaxCols];
    for (int i = 0; i < len; i++) {
      const SC& c = cs[i];
      r.ch[cx] = c.ch; fq[cx] = c.fg; bq[cx] = c.bg;
      if (c.w == 2) { r.ch[cx + 1] = kChTofuR; fq[cx + 1] = c.fg; bq[cx + 1] = c.bg; }
      cx += c.w;
    }
    for (int x = cx; x < r.cols; x++) { r.ch[x] = ' '; fq[x] = 0xFFFFFFu; bq[x] = 0x80FFFFFFu; }
    ChanRef fc{r.fg_mode, r.fg.uni, r.fg.arr, r.fg_runs, r.hint_fg};
    ChanRef bc{r.bg_mode, r.bg.uni, r.bg.arr, r.bg_runs, r.hint_bg};
    chan_build(fc, fq, r.cols);
    chan_build(bc, bq, r.cols);
    if (wrapped) r.flags |= 1;
  };
  auto fill_live_row = [](LiveRowRef r, const SC* cs, int len, uint8_t wrapped) {  // 活动行：逐格直写
    int cx = 0;
    for (int i = 0; i < len; i++) {
      const SC& c = cs[i];
      r.put(cx, c.ch, c.fg, c.bg);
      if (c.w == 2) r.put(cx + 1, kChTofuR, c.fg, c.bg);
      cx += c.w;
    }
    r.fill_span(cx, r.row->cols - cx, ' ', 0xFFFFFFu, 0x80FFFFFFu);
    if (wrapped) r.flags |= 1;
  };
  if (!alt) {
    while (hist_size(ns) < new_hist) {
      hist_push_blank(ns);
      const int hi = hist_size(ns) - 1;
      fill_hist_row(hist_row(ns, hi), cpool.data() + espans[hi].first, espans[hi].second, ewrap[hi]);
    }
    for (int i = new_hist; i < total; i++)
      fill_live_row(buf_row(ns, i - new_hist), cpool.data() + espans[i].first, espans[i].second, ewrap[i]);
  } else {
    // C1：备用屏活动 → 两条数据流严格分流（都不折行、不跨行合并）：
    //   主屏历史逐行迁移（截断/补空、保留 wrapped 标志），绝不与 alt 内容混流；
    //   备用屏活动区逐行迁移（TUI 收到 SIGWINCH 后自行重绘，迁移只为过渡不闪空）。
    for (int j = 0; j < hist_size(s); j++) {
      RowB& h = hist_row(s, j);
      uint32_t fq[kMaxCols], bq[kMaxCols];
      h.decode(fq, bq);
      SC cells[kMaxCols];
      int n = 0, wsum2 = 0;
      for (int x = 0; x < h.cols; x++) {
        if (h.ch[x] == kChTofuR) continue;               // 右半并入前一单元
        const bool wide = (x + 1 < h.cols && h.ch[x + 1] == kChTofuR);
        const int w = wide ? 2 : 1;
        if (wsum2 + w > ns->s_cols) break;               // 新列宽装不下：截断（同②b语义）
        cells[n++] = {h.ch[x], fq[x], bq[x], (uint8_t)w};
        wsum2 += w;
      }
      hist_push_blank(ns);
      fill_hist_row(hist_row(ns, hist_size(ns) - 1), cells, n, (uint8_t)(h.flags & 1));
    }
    { // 备用屏活动区逐行迁移（截断/补空；宽字对边界不切半）
      const int oc = s->s_cols, nc = ns->s_cols, orr = s->s_rows, nr = ns->s_rows;
      const int m = oc < nc ? oc : nc;
      for (int y = 0; y < nr; y++) {
        LiveRowRef d = buf_row(ns, y);
        if (y < orr) {
          LiveRowRef src = buf_row(s, y);
          memcpy(d.ch, src.ch, (size_t)m * sizeof(uint16_t));
          memcpy(d.fg, src.fg, (size_t)m * sizeof(uint32_t));
          memcpy(d.bg, src.bg, (size_t)m * sizeof(uint32_t));
          d.flags = src.flags;
          if (m > 0 && m < oc && src.ch[m] == kChTofuR) {   // 截断处恰是宽字对右半：左半降为空格
            d.ch[m - 1] = ' '; d.fg[m - 1] = 0xFFFFFFu; d.bg[m - 1] = 0x80FFFFFFu;
          }
        } else {
          d.flags = 0;
        }
        d.fill_span(m, nc - m, ' ', 0xFFFFFFu, 0x80FFFFFFu);
      }
    }
  }
  int popped = 0;                                 // A2：裁掉的最旧历史行数（视图索引须随之平移）
  while (hist_size(ns) > ns->s_hist_max) {        // 窄化时输出行倍增：按上限裁掉最旧历史
    hist_pop_front(ns);
    popped++;
  }

  // ⑤ 光标/视图
  if (alt) {
    // C1：备用屏逐行迁移 → 光标直接夹取到新网格；备用屏无回滚视图语义 → 视图回底
    ns->s_row = s->s_row < ns->s_rows ? s->s_row : ns->s_rows - 1;
    ns->s_col = s->s_col < ns->s_cols ? s->s_col : ns->s_cols - 1;
    ns->s_view = 0; ns->s_view_anchor = -1;
  } else {
  int cr = crow - new_hist;
  if (cr < 0) cr = 0;
  if (cr >= ns->s_rows) cr = ns->s_rows - 1;
  ns->s_row = cr;
  ns->s_col = ccol < ns->s_cols ? ccol : ns->s_cols - 1;
  const int hist_final = new_hist - popped;       // A2：裁剪后的实际历史行数
  ns->s_view = old_view < hist_final ? old_view : hist_final;
  if (old_view > 0 && old_top_li >= 0 && old_top_li < (int)lstart.size() &&
      lstart[old_top_li] >= 0) {                           // 回看视图按逻辑行迁移：缩放后视图首行仍是同一逻辑行
    int top = lstart[old_top_li] - popped;                 // A2：hist_pop_front 后绝对行号整体前移
    if (top < 0) top = 0;
    int vs = hist_final - top;
    if (vs < 0) vs = 0;
    if (vs > hist_final) vs = hist_final;
    ns->s_view = vs;
    ns->s_view_anchor = (vs > 0) ? top : -1;
  }
  }
  ns->s_wrap_pending = s->s_wrap_pending && (ns->s_col >= ns->s_cols - 1);   // 满列挂起态随光标保留
  ns->s_alt_active = s->s_alt_active;              // 备用屏状态随存档迁移保留
  ns->s_dirty = ns->s_rows >= 32 ? 0xFFFFFFFFu : ((1u << ns->s_rows) - 1);
  ns->s_last_present = 0;
  ns->s_cur_drawn_row = -1; ns->s_cur_drawn_col = -1;
  T = prevT;

  // ⑥ 删旧窗 + 释放折行池 + 归还 arena 给内核（防缩放后 RSS 驻留高位）
  if (T == s) T = ns;
  delete s;   // RAII 释放（s_grid shared_ptr / s_pal4 / s_canvas 自动释放）
  { std::vector<SC>().swap(cpool); std::vector<std::pair<int,int>>().swap(lspans);
    std::vector<std::pair<int,int>>().swap(espans); std::vector<uint8_t>().swap(ewrap); }
  malloc_trim(0);
  return ns;
}

int state_font_size(const State* s) { return s ? s->s_font_size : 6; }
void set_quiet(bool on) { s_quiet = on; }
// 标记整屏脏（切窗/缩放后调用）：所有行都会重画 → 屏上任何光标方块都会被覆盖，
// 记录随之作废；下一次 present 在画完全部行后按新位置重画唯一光标。
void invalidate()
{
  if (T == nullptr) return;
  T->s_dirty = dirty_all_mask();
  cursor_forget();
}

// 整屏填背景（画布=背景图或背景色）：缩放 rewrap 前调用，先铺满底，
// 再重排上屏——避免新字符区不覆盖全屏时右侧/下方残留旧缩放的脏像素。
void fill_bg()
{
  if (T == nullptr) return;
  if (T->s_canvas != nullptr) tft.pushImage(0, 0, kCanvasW, kCanvasH, T->s_canvas.get());
  else tft.fillRect(0, 0, kCanvasW, kCanvasH, to565(T->s_bg));
  cursor_forget();                       // 整屏重铺画布 → 屏上已无光标（旧方块被覆盖）
  toast_paint();                         // 整屏重铺同样会覆盖 toast → 补画到最上层（无 toast 时零开销）
}

// 兼容入口：初始化当前状态并铺底图（单窗口旧流程/演示用）
void init(const Options& opt)
{
  State* s = state_new(opt);
  if (s == nullptr) return;
  if (T != nullptr && T != s) delete T;
  T = s;
  tft.pushImage(0, 0, kCanvasW, kCanvasH, T->s_canvas.get());   // 画布作底图（整屏）
  T->s_dirty = 0;
  T->s_last_present = now_ms_();
  cursor_forget();                       // 画布重铺 → 屏上无光标
  if (cursor_should_draw()) cursor_paint();   // 画唯一光标并同步记录
}

// 双宽字符占两格：写入/退格碰到半边时先拆掉整对，避免残留半框
void break_wide(int x, int y)
{
  if (x < 0 || x >= kCols) return;
  LiveRowRef r = buf_row(T, y);
  if (r.ch[x] == kChTofuR && x > 0) {                 // 右半 → 拆掉左半
    r.put(x - 1, ' ', cell_fg(), cell_bg());
    mark_row(y);
  } else if (x + 1 < kCols && r.ch[x + 1] == kChTofuR) {  // 左半 → 拆掉右半
    r.put(x + 1, ' ', cell_fg(), cell_bg());
    mark_row(y);
  }
}

// ── OSC 分发：4=动态调色板；10/11/12=默认前景/背景/光标颜色 ─────────
static bool parse_osc_color(const char* q, uint32_t* out)
{
  // rgb:RRRR/GGGG/BBBB（每段 1-4 位 hex，按最高位扩展）或 #RRGGBB
  if (strncmp(q, "rgb:", 4) == 0) {
    unsigned long v[3] = {0, 0, 0};
    const char* p = q + 4;
    for (int i = 0; i < 3; i++) {
      char* end = nullptr;
      v[i] = strtoul(p, &end, 16);
      int digits = (int)(end - p);
      if (digits <= 0 || digits > 4) return false;
      for (int k = digits; k < 4; k++) v[i] = (v[i] << 4) | ((v[i] >> (4 * (k - 1))) & 0xf);
      if (i < 2) { if (*end != '/') return false; p = end + 1; }
      else p = end;
    }
    *out = mk_col((int)(v[0] >> 8), (int)(v[1] >> 8), (int)(v[2] >> 8), false);
    return true;
  }
  if (q[0] == '#' && strlen(q) >= 7) {
    unsigned v = 0;
    if (sscanf(q + 1, "%6x", &v) == 1) { *out = v & 0x00FFFFFFu; return true; }
  }
  return false;
}

// ── base64（OSC 52 剪贴板编解码）────────────────────────────────────
static void osc_reply(State* s, int num, const std::string& val);   // 前置（osc52 回读用）
static const char kB64T[65] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string b64_encode(const uint8_t* d, size_t n)
{
  std::string out;
  out.reserve((n + 2) / 3 * 4);
  for (size_t i = 0; i < n; i += 3) {
    uint32_t v = (uint32_t)d[i] << 16;
    if (i + 1 < n) v |= (uint32_t)d[i + 1] << 8;
    if (i + 2 < n) v |= (uint32_t)d[i + 2];
    out.push_back(kB64T[(v >> 18) & 63]);
    out.push_back(kB64T[(v >> 12) & 63]);
    out.push_back(i + 1 < n ? kB64T[(v >> 6) & 63] : '=');
    out.push_back(i + 2 < n ? kB64T[v & 63] : '=');
  }
  return out;
}

static int b64_val(char c)
{
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;                                        // '=' 与空白 → 由调用方跳过
}

static std::vector<uint8_t> b64_decode(const char* in, size_t len)
{
  std::vector<uint8_t> out;
  uint32_t v = 0; int bits = 0;
  for (size_t i = 0; i < len; i++) {
    int x = b64_val(in[i]);
    if (x < 0) continue;                            // 跳过 '='、换行等
    v = (v << 6) | (uint32_t)x; bits += 6;
    if (bits >= 8) { bits -= 8; out.push_back((uint8_t)((v >> bits) & 0xFF)); }
  }
  return out;
}

// ── OSC 52 系统剪贴板（跨窗口共享 = OS 级单例，如 /dev/fb0；非每窗口终端状态）──
//   set：解码 base64 → 内存剪贴板 + 落盘（桥接其它程序）；query：回读并应答。
static std::vector<uint8_t> s_clip;                 // 剪贴板内容（原始字节）
static std::string        s_clip_path;             // 持久化路径（懒计算）
static bool               s_clip_loaded = false;

static std::string clip_file()
{
  if (!s_clip_path.empty()) return s_clip_path;
  const char* h = getenv("HOME");
  std::string base = h ? h : "/home/me";
  s_clip_path = base + "/.config/cardterm/clipboard";
  return s_clip_path;
}

static void clip_load_once()                        // 启动后回读落盘剪贴板（跨会话保留）
{
  if (s_clip_loaded) return;
  s_clip_loaded = true;
  FILE* f = fopen(clip_file().c_str(), "rb");
  if (f == nullptr) return;
  uint8_t buf[4096]; size_t n;
  s_clip.clear();
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) s_clip.insert(s_clip.end(), buf, buf + n);
  fclose(f);
}

static void clip_store(const std::vector<uint8_t>& data)
{
  s_clip = data;
  s_clip_loaded = true;                             // 已有内存值，勿再从盘回读覆盖
  const std::string p = clip_file();                // 仅取路径，不回读（避免覆盖刚写入值）
  // 目录可能不存在（首次运行）：尽力创建
  std::string dir = p.substr(0, p.rfind('/'));
  mkdir(dir.c_str(), 0755);
  FILE* f = fopen(p.c_str(), "wb");
  if (f != nullptr) {
    if (!s_clip.empty()) fwrite(s_clip.data(), 1, s_clip.size(), f);
    fclose(f);
  }
}

// OSC 52 ; Pc ; Pd ST   —— Pd=base64 设置；Pd=? 查询。Pc=选择区（c/p/q/s/0-7，任意均映射剪贴板）
static void osc52(State* s, const char* spec)
{
  const char* semi = strchr(spec, ';');
  std::string pc = semi ? std::string(spec, (size_t)(semi - spec)) : std::string(spec);
  const char* pd = semi ? semi + 1 : "";
  if (pc.empty()) pc = "c";
  if (strcmp(pd, "?") == 0) {
    clip_load_once();
    std::string b64 = b64_encode(s_clip.empty() ? nullptr : s_clip.data(), s_clip.size());
    osc_reply(s, 52, pc + ";" + b64);
  } else {
    clip_store(b64_decode(pd, strlen(pd)));
  }
}

static void osc_reply(State* s, int num, const std::string& val)
{
  std::string b = "\033]" + std::to_string(num) + ";" + val + "\033\\";
  if (s->s_writer != nullptr) s->s_writer(b.data(), b.size());
}

static void osc_dispatch(State* s)
{
  s->s_osc[s->s_osc_len] = '\0';
  const char* p = s->s_osc;
  if (p[0] == '4' && (p[1] == ';' || p[1] == '\0')) {   // OSC 4：动态调色板 i;spec[;i;spec…]
    ensure_pal(s);
    const char* q = p + 1;
    if (*q == ';') q++;
    while (*q) {
      char* end = nullptr;
      long idx = strtol(q, &end, 10);
      if (end == q || *end != ';') break;
      const char* spec = end + 1;
      const char* semi = strchr(spec, ';');
      std::string sv(spec, semi ? (size_t)(semi - spec) : strlen(spec));
      if (sv == "?") {
        uint32_t c = (s->s_pal4 && idx >= 0 && idx < 256) ? s->s_pal4[idx] : 0;
        osc_reply(s, 4, std::to_string(idx) + ";" + rgb_query(c));
      } else {
        uint32_t c = 0;
        bool ok = parse_osc_color(sv.c_str(), &c);
        if (ok && s->s_pal4 && idx >= 0 && idx < 256) s->s_pal4[idx] = c;
      }
      if (!semi) break;
      q = semi + 1;
    }
    mark_all();
    if (!s_quiet) present();                       // OSC4 改色立即追溯已写内容（后台静默不抢屏）
    return;
  }
  int num = atoi(p);
  // OSC 110/111/112：复位默认前景/背景/光标色（无参数形式 "ESC ] 110 ST"，没有分号，
  // 必须在下面"必须带分号"的判定之前处理）。TUI（crush 等）退出时常发 112 把
  // OSC 12 定制的光标色清掉；此前这里直接 return，于是退出后光标仍保持 TUI 的颜色。
  if (num == 110 || num == 111 || num == 112) {
    if (num == 110) {
      s->s_def_fg = s->s_def_fg0;                    // 回配置默认前景（OSC 10 定制作废）
      if (s->s_fg_idx == 7) s->s_fg = s->s_def_fg;
    } else if (num == 111) {
      s->s_def_bg = s->s_def_bg0;                    // 回配置默认背景（OSC 11 定制作废）
    } else {
      s->s_cursor_col = s->s_cursor_col_def;         // 光标色回配置默认（OSC 12 定制作废）
    }
    mark_all();                                      // 颜色参与整屏合成 → 全部重画（光标随之为新色）
    if (!s_quiet) present();
    return;
  }
  const char* semi = strchr(p, ';');
  if (semi == nullptr) return;
  const char* spec = semi + 1;
  if (num == 52) { osc52(s, spec); return; }         // OSC 52：剪贴板 set/query
  if (num == 10 || num == 11 || num == 12) {         // OSC 10/11/12：默认 fg/bg/光标
    if (strcmp(spec, "?") == 0) {
      uint32_t c = (num == 10) ? s->s_def_fg : (num == 11) ? s->s_def_bg : s->s_cursor_col;
      osc_reply(s, num, rgb_query(c));
    } else {
      uint32_t c;
      if (!parse_osc_color(spec, &c)) return;
      if (num == 10) {
        s->s_def_fg = c;
        if (s->s_fg_idx == 7) { s->s_fg = c; }
      } else if (num == 11) {
        s->s_def_bg = c;                              // 仅更新默认背景基准色；背景图保留
        // 默认背景色的格子渲染为透明（透出背景图）；显式 RGB 背景即使同值也不透明
      } else {
        s->s_cursor_col = c;
      }
      mark_all();
      if (!s_quiet) present();                       // OSC10/11/12 立即生效（后台静默不抢屏）
    }
  }
  // OSC 0/2（标题）等：忽略
}

// 解析热路径：只改屏缓冲 + 标脏行，不画任何像素
// 换行语义 = xterm"延迟环绕"：打印到最后一列时光标停在列尾（挂起态），
// 直到下一个可打印字符到达才真正换行；CR/LF/BS/HT 等只移动光标并清除挂起态。
void put_raw(char c)
{
  if (c == 0x0e) { T->s_so = true; T->s_st = St::Normal; return; }   // SO：GL = G1
  if (c == 0x0f) { T->s_so = false; T->s_st = St::Normal; return; }  // SI：GL = G0
  follow_output();
  T->s_cursor_on = true; T->s_cursor_t0 = now_ms_();       // 输出后光标立即可见
  switch (c) {
    case '\n': case '\v': case '\f':
      if (T->s_wrap_pending && T->s_autowrap) {
        if (s_full_line_nl) {
          // 可配置语义（full_line_newline=yes）：满列后先提交「自动换行」（软换行，落点空行），
          // 再执行「换行符换行」→ 显示「满列行 / 空行 / 后续文本」；rewrap 时该空行并入上一逻辑行。
          T->s_wrap_pending = false;
          buf_row(T, T->s_row).flags |= 1;             // 满列行 = 软换行（rewrap 并入空行）
          newline();                                    // 自动换行：回行首并下移（落到空行，列=0）
          buf_row(T, T->s_row).flags &= (uint8_t)~1;    // 虚空行 = 硬换行（逻辑行在此结束）
        } else {
          // 默认 xterm/vt.c 语义：LF 取消延迟环绕，只下移一行（列不变），不产生空行、不动 flags。
          T->s_wrap_pending = false;
        }
      } else {
        // 换行符换行：不动 flags —— 走行换行（光标定位排版）会经过已有软换行行，
        // 清 fl 会毁掉 rewrap 合并依据（核心 bug）；行尾 fl 由行内容本身决定。
        T->s_wrap_pending = false;
      }
      line_down();                                    // 换行符换行：下移一行、列不变
      break;
    case '\r': T->s_col = 0; break;   // CR 仅归零列，不清满列挂起（CR+LF 的自动换行需保留到 LF）
    case '\b': T->s_wrap_pending = false; if (T->s_col > 0) T->s_col--; break;
    case '\t':                                       // HT：停在下一制表位/右边界，不环绕
      T->s_wrap_pending = false;
      T->s_col = next_tab(1);
      break;
    default:
      if ((unsigned char)c >= 0x20 && (unsigned char)c != 0x7f) {
        if ((T->s_so ? T->s_g1 : T->s_g0) == 1)          // DEC 特殊图形：映射成 ASCII 框线
          c = (char)acs_ascii((unsigned char)c);
        if (T->s_wrap_pending && T->s_autowrap && T->s_col >= kCols - 1) {   // 末列挂起 + 打印 → 延迟环绕
          T->s_wrap_pending = false;
          buf_row(T, T->s_row).flags |= 1;             // 软换行标记（rewrap 用）
          newline();
        }
        T->s_wrap_pending = false;                     // 写入本行：清除满列挂起（写满末列会在下方重置）
        if (T->s_col == 0) {                           // 从列0重写 = 新逻辑行起点：陈旧软换行标记作废
          buf_row(T, T->s_row).flags &= (uint8_t)~1; }
        break_wide(T->s_col, T->s_row);
        if (T->s_insert_mode) { insert_cells(1); repair_pairs(buf_row(T, T->s_row)); }          // IRM：整行右移一格
        buf_row(T, T->s_row).put(T->s_col, (uint16_t)(unsigned char)c, cell_fg(), cell_bg());
        mark_row(T->s_row);
        T->s_last_print = (unsigned char)c; T->s_last_print_w = 1;
        if (++T->s_col >= kCols) {
          T->s_col = kCols - 1;                         // 光标停在最后一列
          if (T->s_autowrap) T->s_wrap_pending = true;     // 真正的换行延迟到下一个字符
        }
      }
  }
}

void putc(char c)                                    // 交互入口：单字符 + 上屏
{
  put_raw(c);
  maybe_present();
}

void backspace()
{
  follow_output();
  T->s_cursor_on = true; T->s_cursor_t0 = now_ms_();
  T->s_wrap_pending = false;
  if (T->s_col > 0) T->s_col--;
  break_wide(T->s_col, T->s_row);                    // 落在宽字任一半 → 整对拆掉（避免孤儿右半）
  buf_row(T, T->s_row).put(T->s_col, ' ', cell_fg(), cell_bg());
  mark_row(T->s_row);
  maybe_present();
}

void cursor_reblink()                               // 光标回到亮块相并重启闪烁计时
{
  if (T == nullptr) return;
  T->s_cursor_on = true;
  T->s_cursor_t0 = now_ms_();
  mark_row(T->s_row);
}

void tick()                                         // 光标闪烁节拍（UI 循环调用）
{
  if (T == nullptr) return;                       // 防御：状态已释放（无窗口）时无节拍可走
  toast_expire_check();                           // toast 到期：恢复被覆盖区域（行重画 + 光标）
  blink_track();                                  // 位置/显隐变化 → 立即复位闪烁（防相位残留）
  if (!T->s_cursor_blink) {                        // 常亮形状：保证显示
    if (!T->s_cursor_on) { T->s_cursor_on = true; mark_row(T->s_row); }
  } else {
    uint64_t now = now_ms_();
    if (now - T->s_cursor_t0 >= kBlinkMs) {
      T->s_cursor_t0 = now;
      T->s_cursor_on = !T->s_cursor_on;
      if (T->s_view != 0) {                        // 翻页视图：光标不画
        cursor_forget();
      } else if (!T->s_cursor_vis) {               // 应用隐藏光标：擦掉可能残留的方块
        cursor_dirty_for_repaint();
      } else {
        // 闪烁相变：光标格整行重合成，再走 present_now 的统一路径——
        // 它会先擦掉记录里的旧光标（可能已随光标移动在别的格/同一行另一列），
        // 再按当前相位决定是否画新方块。旧实现只重画"当前"格且不更新记录，
        // 光标移动后旧方块残留 → 屏幕上同时两个光标。
        mark_row(T->s_row);
        present_now();
      }
    }
  }
  maybe_present();                               // 兼作合帧节拍
}

void scroll_view(int delta)
{
  int total = hist_size(T);
  if (total == 0) return;
  const int old = T->s_view;
  int v = T->s_view + delta;
  if (v < 0) v = 0;
  if (v > total) v = total;
  const int applied = v - old;                   // 实际生效的位移（含钳位）
  T->s_view = v;
  if (T->s_view != 0) {                          // 翻页键：保持 hold_ms 不跳底
    if (T->s_view_anchor < 0)                     // 自底部首次进入翻页：锚定移动前的视图首行
      T->s_view_anchor = hist_size(T) - old;
    T->s_view_anchor -= applied;                 // 相对移动：输出使历史增长也不漂移
    if (T->s_view_anchor < 0) T->s_view_anchor = 0;
    const int mx = hist_size(T);
    if (T->s_view_anchor > mx) T->s_view_anchor = mx;
    T->s_view_hold_until = now_ms_() + (uint64_t)T->s_view_hold_ms;
    T->s_view_pending = false;
  } else {
    T->s_view_anchor = -1;                       // 回到底部 = 跟随模式
  }
  present();                                     // 翻页是交互操作：立即上屏
}

void attach_process(long pid)
{
  s_child_pid = (pid_t)pid;
}

void close_window()
{
  // 与普通电脑关闭终端窗口一致：SIGHUP 会话 → 宽限 → SIGKILL → 收尾
  if (s_child_pid > 0) {
    pid_t pg = getpgid(s_child_pid);
    if (pg <= 0) pg = s_child_pid;
    kill(-pg, SIGHUP);
    for (int i = 0; i < 30; i++) {
      if (waitpid(s_child_pid, nullptr, WNOHANG) != 0) break;
      usleep(50000);
    }
    if (waitpid(s_child_pid, nullptr, WNOHANG) == 0) {
      kill(-pg, SIGKILL);
      waitpid(s_child_pid, nullptr, 0);
    }
    s_child_pid = 0;
  }
  // 子进程已收尸：TUI 用 OSC 12 定制的光标颜色/形状不该留在返回后的界面上
  // （reset_cursor_style 只重画光标行，不影响屏内容）。
  reset_cursor_style();
}

void set_writer(Writer w) { if (T != nullptr) T->s_writer = w; }

// DECCKM 应用光标键模式（供键盘序列生成选择 SS3/CSI 形式）
bool app_cursor_keys() { return T->s_app_cursor; }

// kitty 键盘协议当前增强标志（0=未启用；键盘编码据此切换 CSI u 形态）
uint32_t kkp_flags() { return T->s_kkp_flags; }

// DEC 特殊图形字符集（VT100 ACS）→ ASCII 近似：ncurses 在非 UTF-8 locale 下
// 用 ESC(0 … ESC(B 输出框线，按 1 格宽画，保证 TUI 布局不错位
static unsigned char acs_ascii(unsigned char c)
{
  switch (c) {
    case '`': return '*';                              // ◆ 菱形
    case 'a': return '#';                              // ▒ 棋盘
    case 'f': return 'o';                              // °
    case 'g': return '+';                              // ±
    case 'j': case 'k': case 'l': case 'm':            // ┘┐┌└ 四角
    case 'n': case 't': case 'u': case 'v': case 'w':  // ┼├┤┴┬ 交叉/三通
      return '+';
    case 'o': case 'p': case 'r': case 's': return '_';// 扫描线
    case 'q': return '-';                              // ─ 横线
    case 'x': return '|';                              // │ 竖线
    case 'y': return '<';                              // ≤
    case 'z': return '>';                              // ≥
    case '{': return 'n';                              // π
    case '|': return '!';                              // ≠
    case '}': return 'f';                              // £
    case '~': return '.';                              // ·
    default:  return c;
  }
}

// 近似 wcwidth：0=组合/控制，2=宽字符（CJK/emoji 等），1=其它
// （只用于字体表外的码点做 tofu 宽度判定；表内字形按字形自带的全半角）
static int cp_width(uint32_t cp)
{
  if (cp < 0x20 || (cp >= 0x7f && cp < 0xa0)) return 0;
  // 零宽字符：组合符 / 变体选择符 / 双向控制 / 不可见操作符 / 软连字符。
  // crush 等 TUI 用 NBSP+VS15（U+00A0 U+FE0E）包内联代码，VS15 不应占一格。
  if ((cp >= 0x0300 && cp <= 0x036F) || (cp >= 0x0483 && cp <= 0x0489) ||
      (cp >= 0x0591 && cp <= 0x05BD) || (cp >= 0x0610 && cp <= 0x061A) ||
      (cp >= 0x064B && cp <= 0x065F) || cp == 0x0670 || (cp >= 0x06D6 && cp <= 0x06ED) ||
      cp == 0x0711 || (cp >= 0x0730 && cp <= 0x074A) || (cp >= 0x07A6 && cp <= 0x07B0) ||
      (cp >= 0x0900 && cp <= 0x0903) || (cp >= 0x093A && cp <= 0x094F) ||
      (cp >= 0x0951 && cp <= 0x0957) || (cp >= 0x0962 && cp <= 0x0963) ||
      (cp >= 0x0E31 && cp <= 0x0E3A) || (cp >= 0x0E47 && cp <= 0x0E4E) ||
      (cp >= 0x0EB1 && cp <= 0x0EBC) || (cp >= 0x0EC8 && cp <= 0x0ECD) ||
      (cp >= 0x0F18 && cp <= 0x0F19) || (cp >= 0x0F35 && cp <= 0x0F39) ||
      (cp >= 0x0F71 && cp <= 0x0F84) || (cp >= 0x0F86 && cp <= 0x0F87) ||
      (cp >= 0x0F8D && cp <= 0x0FBC) || (cp >= 0x135D && cp <= 0x135F) ||
      (cp >= 0x180B && cp <= 0x180E) ||
      (cp >= 0x1AB0 && cp <= 0x1AFF) || (cp >= 0x1DC0 && cp <= 0x1DFF) ||
      (cp >= 0x200B && cp <= 0x200F) || (cp >= 0x202A && cp <= 0x202E) ||
      (cp >= 0x2060 && cp <= 0x2064) || (cp >= 0x2066 && cp <= 0x206F) ||
      (cp >= 0x20D0 && cp <= 0x20F0) ||
      (cp >= 0xFE00 && cp <= 0xFE0F) || (cp >= 0xFE20 && cp <= 0xFE2F) ||
      cp == 0xFEFF || cp == 0x00AD || cp == 0x061C ||
      (cp >= 0x1F3FB && cp <= 0x1F3FF) ||
      (cp >= 0xE0020 && cp <= 0xE007F) || (cp >= 0xE0100 && cp <= 0xE01EF))
    return 0;
  // 圆点族 ●○•∙⋅·◉◎：真实终端（tmux 光标实测）与 glibc wcwidth
  // （en_US.UTF-8 / C.UTF-8）均为 1 列；◉ EAW=N、◎ EAW=A，A 按窄处理。
  // 0x25C9/0x25CE 不在下方宽区间（0x2E80 起）内 → 落到 return 1。
  if ((cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF) ||
      (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
      (cp >= 0xFE30 && cp <= 0xFE4F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
      (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x1F300 && cp <= 0x1F64F) ||
      (cp >= 0x1F900 && cp <= 0x1F9FF) || (cp >= 0x20000 && cp <= 0x3FFFD))
    return 2;
  return 1;
}

// 豆腐块：单宽 5×9 / 双宽 11×9 空心框（几何见 paint_glyph）
void put_tofu(int w)
{
  follow_output();
  T->s_cursor_on = true; T->s_cursor_t0 = now_ms_();
  if (T->s_wrap_pending && T->s_autowrap && T->s_col >= kCols - 1) {   // 挂起态 + 打印 → 此刻才换行
    T->s_wrap_pending = false;
    buf_row(T, T->s_row).flags |= 1;                 // 软换行标记（rewrap 用）
    newline();
  }
  if (w >= 2 && T->s_col >= kCols - 1) {              // 双宽放不下最后一格
    if (!T->s_autowrap) return;                       // 禁止环绕：不显示（xterm 语义）
    buf_row(T, T->s_row).flags |= 1;                 // 软换行标记（rewrap 用）
    newline();
  }
  const uint16_t fg = cell_fg();
  break_wide(T->s_col, T->s_row);
  if (T->s_insert_mode) { insert_cells(w); repair_pairs(buf_row(T, T->s_row)); }   // IRM：整行右移 w 格 + 修对
  T->s_last_print_w = w; T->s_last_print = 0;            // REP 重放占位符
  buf_row(T, T->s_row).put(T->s_col, (uint16_t)(w >= 2 ? kChTofuL : kChTofu), fg, cell_bg());
  if (w >= 2) {
    break_wide(T->s_col + 1, T->s_row);
    buf_row(T, T->s_row).put(T->s_col + 1, kChTofuR, fg, cell_bg());
    mark_row(T->s_row);
  }
  mark_row(T->s_row);
  T->s_col += w;
  if (T->s_col >= kCols) {
    T->s_col = kCols - 1;                             // 光标停在最后一列
    if (T->s_autowrap) T->s_wrap_pending = true;
  }
}

// 打印一个真实 Unicode 字形（字体表内）：w = 单元数（1 或 2，取自字体表的全半角判定）
void put_glyph(uint16_t cp, int w)
{
  follow_output();
  T->s_cursor_on = true; T->s_cursor_t0 = now_ms_();
  if (T->s_wrap_pending && T->s_autowrap && T->s_col >= kCols - 1) {   // 挂起态 + 打印 → 此刻才换行
    T->s_wrap_pending = false;
    buf_row(T, T->s_row).flags |= 1;                 // 软换行标记（rewrap 用）
    newline();
  }
  if (w >= 2 && T->s_col >= kCols - 1) {              // 双宽放不下最后一格
    if (!T->s_autowrap) return;                       // 禁止环绕：不显示（xterm 语义）
    buf_row(T, T->s_row).flags |= 1;                 // 软换行标记（rewrap 用）
    newline();
  }
  break_wide(T->s_col, T->s_row);
  if (T->s_insert_mode) { insert_cells(w); repair_pairs(buf_row(T, T->s_row)); }
  T->s_last_print = cp; T->s_last_print_w = w;
  buf_row(T, T->s_row).put(T->s_col, cp, cell_fg(), cell_bg());
  if (w >= 2) {
    break_wide(T->s_col + 1, T->s_row);
    buf_row(T, T->s_row).put(T->s_col + 1, kChTofuR, cell_fg(), cell_bg());
    mark_row(T->s_row);
  }
  mark_row(T->s_row);
  T->s_col += w;
  if (T->s_col >= kCols) {
    T->s_col = kCols - 1;                             // 光标停在最后一列
    if (T->s_autowrap) T->s_wrap_pending = true;
  }
}

static void emit_cp(uint32_t cp)
{
  if (cp < 0x20 || (cp >= 0x7f && cp < 0xa0)) return;    // 控制字符：忽略
  const int w = cp_width(cp);
  if (w <= 0) return;                                    // 零宽/组合符：直接丢弃（不存储）
  if (cp < 0x80) { put_raw((char)cp); return; }          // ASCII → glcdfont（font0 同源）
  const termfont::TermGlyph* g = (cp <= 0xFFFF) ? termfont::find(*s_font, (uint16_t)cp) : nullptr;
  if (g == nullptr) { put_tofu(w >= 2 ? 2 : 1); return; }// 字体表外（含非 BMP）→ tofu
  put_glyph((uint16_t)cp, g->cells);                     // 全半角由字体表决定
}

// RIS（ESC c）：复位到"新终端"状态（清屏/清回滚/默认 SGR/制表位/滚动域/字符集/延环绕）
static void term_ris()
{
  hist_clear(T);
  T->s_alt_active = false;
  for (int y = 0; y < kRows; y++) {
    buf_row(T, y).fill_span(0, kCols, ' ', cell_fg(), cell_bg());
    buf_row(T, y).flags = 0;
  }
  T->s_row = 0; T->s_col = 0;
  T->s_top = 0; T->s_bot = kRows - 1;
  T->s_autowrap = true; T->s_insert_mode = false; T->s_app_cursor = false;
  // RIS 是硬复位：OSC 10/11 改过的默认色也一并回配置默认（DECSTR 不这么做）
  T->s_def_fg = T->s_def_fg0;
  T->s_def_bg = T->s_def_bg0;
  cursor_style_default(T);             // 光标颜色/形状/闪烁/显隐回配置默认（TUI 定制不残留）
  save_state_default(T);               // DECSC 保存态清理
  T->s_fg_idx = 7; T->s_fg = T->s_def_fg;
  T->s_bg = T->s_def_bg; T->s_bg_def = true;
  T->s_rev = false; T->s_bold = false;
  T->s_g0 = 0; T->s_g1 = 0; T->s_so = false; T->s_wrap_pending = false;
  T->s_esc_delim = 0;
  T->s_saved_row = 0; T->s_saved_col = 0;   // 备用屏存档位置
  tabs_reset();
  mark_all();                          // 整屏重画：旧光标被覆盖，present 按新颜色重画
}

// Esc 状态的单字符分发（St::Esc）：从 write() 主循环抽出的公共入口，供
// 「OSC 内 ESC 中止后字符重放」复用（见 write() 的 St::OscEsc 分支）。
// 语义与 xterm 一致：ESC 之后的字符决定新转义序列走向（[ → CSI、] → 新 OSC、
// P/^/_ → DCS/PM/APC 忽略、( ) * + # % - → 带设计符、其余为单字符终结序列）；
// ch 本身又是 ESC（ESC ESC）时为自转移——中止待处理序列并重新开始一条转义
// 序列，状态保持 St::Esc 等待下一个字符（xterm/DEC 解析器语义）。
static void esc_char(char ch)
{
  if (ch == 0x1b) { T->s_st = St::Esc; return; }             // ESC ESC：自转移，重新起一条转义序列
  if (ch == '[') T->s_st = St::Csi;
  else if (ch == ']') { T->s_st = St::Osc; T->s_osc_len = 0; }  // OSC：收集载荷
  else if (ch == 'P' || ch == '^' || ch == '_') T->s_st = St::Skip;   // DCS/PM/APC：忽略
  else if (ch == '7') { T->s_save_row = T->s_row; T->s_save_col = T->s_col;
                        T->s_save_fg = T->s_fg; T->s_save_bg = T->s_bg; T->s_save_bgdef = T->s_bg_def;
                        T->s_save_fgidx = T->s_fg_idx; T->s_save_bold = T->s_bold;
                        T->s_save_rev = T->s_rev; T->s_save_wrp = T->s_wrap_pending;
                        T->s_st = St::Normal; }
  else if (ch == '8') { follow_output();
                        T->s_row = T->s_save_row; T->s_col = T->s_save_col;
                        T->s_fg = T->s_save_fg; T->s_bg = T->s_save_bg; T->s_bg_def = T->s_save_bgdef;
                        T->s_fg_idx = T->s_save_fgidx; T->s_bold = T->s_save_bold;
                        T->s_rev = T->s_save_rev; T->s_wrap_pending = T->s_save_wrp;  // STD-070: 随 DECSC 恢复
                        T->s_row = T->s_row < 0 ? 0 : (T->s_row >= kRows ? kRows - 1 : T->s_row);
                        T->s_col = T->s_col < 0 ? 0 : (T->s_col >= kCols ? kCols - 1 : T->s_col);
                        T->s_st = St::Normal; }
  else if (ch == 'D') { follow_output(); T->s_wrap_pending = false; line_down(); T->s_st = St::Normal; }
  else if (ch == 'M') { follow_output(); T->s_wrap_pending = false;
                        if (T->s_row == T->s_top) { T->s_row = T->s_top; scroll_down(); }
                        else if (T->s_row > 0) T->s_row--;
                        T->s_st = St::Normal; }
  else if (ch == 'E') { follow_output(); T->s_wrap_pending = false; newline(); T->s_st = St::Normal; }
  else if (ch == 'H') { T->s_tabs[T->s_col] = true; T->s_st = St::Normal; }   // HTS：设置制表位
  else if (ch == 'c') { term_ris(); T->s_st = St::Normal; }                   // RIS：硬复位（reset/tput reset）
  else if (ch == '(' || ch == ')') {
    T->s_esc_delim = (ch == '(') ? 1 : 2;    // G0 / G1 字符集设计
    T->s_st = St::EscMid;
  }
  else if (ch == '*' || ch == '+' || ch == '#' || ch == '%' || ch == '-')
    T->s_st = St::EscMid;                    // 其它模式指定：吞掉后续 1 字节
  else T->s_st = St::Normal;
}

void write(const char* s, size_t len)
{
  for (size_t i = 0; i < len; i++) {
    char ch = s[i];
    switch (T->s_st) {
      case St::Normal: {
        if (ch == 0x1b) { T->s_st = St::Esc; T->s_csi_len = 0; }
        else {
          const unsigned char b = (unsigned char)ch;
          if (b < 0x80) { T->s_u8_need = 0; put_raw(ch); }
          else if (T->s_u8_need == 0) {
            if      ((b & 0xE0) == 0xC0) { T->s_u8_cp = b & 0x1F; T->s_u8_need = 1; }
            else if ((b & 0xF0) == 0xE0) { T->s_u8_cp = b & 0x0F; T->s_u8_need = 2; }
            else if ((b & 0xF8) == 0xF0) { T->s_u8_cp = b & 0x07; T->s_u8_need = 3; }
            else emit_cp(0xFFFD);
          } else if ((b & 0xC0) == 0x80) {
            T->s_u8_cp = (T->s_u8_cp << 6) | (b & 0x3F);
            if (--T->s_u8_need == 0) emit_cp(T->s_u8_cp);
          } else {
            T->s_u8_need = 0;
            emit_cp(0xFFFD);
          }
        }
        break;
      }
      case St::Esc:
        esc_char(ch);                        // 公共 Esc 分发（见 esc_char：含 ESC ESC 自转移）
        break;
      case St::EscMid:
        if (T->s_esc_delim == 1) T->s_g0 = (ch == '0') ? 1 : 0;   // ESC ( 0 / ESC ( B
        else if (T->s_esc_delim == 2) T->s_g1 = (ch == '0') ? 1 : 0;
        T->s_esc_delim = 0;
        T->s_st = St::Normal;
        break;
      case St::Csi:
        if (ch >= 0x40 && ch <= 0x7e) {                       // 终结符
          if (T->s_csi_len >= 0) csi_exec(T->s_csi, T->s_csi_len, ch);
          T->s_st = St::Normal;                               // 溢出序列(-1)：丢弃但正确终结
        } else if (T->s_csi_len >= 0 && T->s_csi_len < (int)sizeof(T->s_csi) - 1) {
          T->s_csi[T->s_csi_len++] = ch;
        } else {
          T->s_csi_len = -1;                                  // 超长 → 吞到终结符
        }
        break;
      case St::Osc: {                                // ESC ] … BEL 或 ST 结束（收集载荷）
        if (ch == 0x07) { if (T->s_osc_len >= 0) osc_dispatch(T); T->s_st = St::Normal; }
        else if (ch == 0x1b) T->s_st = St::OscEsc;
        else if (T->s_osc_len >= 0 && T->s_osc_len < (int)sizeof(T->s_osc) - 1) T->s_osc[T->s_osc_len++] = (char)ch;
        else T->s_osc_len = -1;                        // 超长 → 丢弃载荷直至终结符（ST/BEL 收尾；
                                                       //   遇 ESC 立即中止，见 OscEsc 分支）
        break;
      }
      case St::OscEsc:
        if (ch == '\\') { if (T->s_osc_len >= 0) osc_dispatch(T); T->s_st = St::Normal; }
        else {
          // xterm 语义（OSC 重入吞流修复）：OSC 中的 ESC = 中止当前 OSC（不 dispatch、
          // 不再把后续字节吞进载荷），ESC 之后的字符当作新转义序列的开始，重放进
          // Esc 状态机。例："ESC]0;ti" 被 "ESC[2J" 打断 → 丢弃标题 OSC，随后正常清屏。
          T->s_osc_len = 0;                        // 丢弃未完成/超长(-1)的 OSC 载荷
          T->s_csi_len = 0;                        // 同 Normal→Esc：新序列的 CSI 体重新计数
          T->s_st = St::Esc;
          esc_char(ch);                            // 重放：[ ] ( ) P _ 等 introducer 与 ESC ESC 均正确
        }
        break;
      case St::Skip:                                 // ESC P/^/ _ … ST 结束
        if (ch == 0x1b) T->s_st = St::SkipEsc;
        break;
      case St::SkipEsc:
        T->s_st = (ch == '\\') ? St::Normal : St::Skip;
        break;
    }
  }
  maybe_present();        // 合帧上屏：批量输出攒到这里，至多每 kPresentMs 画一次
}

void write(const char* s)
{
  if (s != nullptr) write(s, strlen(s));
}

}  // namespace term
