// terminal.h — 超简易终端模拟器（fbcon 实现逻辑的用户态复现，基于 LovyanGFX）
//
// 规格：
//   - 网格由屏幕尺寸与字号单元计算：4x7 → 80×24，6x10 → 53×17，8x14 → 40×12
//     （默认 6×10 → 53×17；解析器/绘制全部使用运行时维度）
//   - 屏幕 = 字符屏缓冲（fbcon vc_data.screenbuf 思想）：每格 {字符, 前景, 背景}
//   - 底图画布 320×170@16bit：默认背景色或背景图（PNG/JPG 缩放到 320×170）。
//     单元格未设置背景色时透出画布；终端（ANSI）显式设置背景色时优先显示该色
//   - 回滚缓冲（[scrollback] lines），PgUp/PgDn 翻看历史
//   - ANSI 转义序列：SGR 16 色（调色板可自定义）/ 256 色 / 38;2;r;g;b 与
//     48;2;r;g;b 真彩 RGB、CUP/ED/EL/光标移动。粗体/斜体等不支持，
//     按默认字体渲染
//   - 输出 API 即"伪终端上屏口"：write() 供键盘本地回显与应用输出共用
#pragma once

#include <cstddef>
#include <cstdint>

#include "config.h"      // cfg::UiColors（toast/提示框/输入法框配色覆盖）

namespace term {

// 缓冲区上限（供更小字体形式使用；实际网格由 state_new 按字体参数计算）
constexpr int kMaxCols = 80;        // 列上限（最小字格 4px：320/4 = 80）
constexpr int kMaxRows = 32;        // 行上限（s_dirty 为 32 位）

// 运行时网格/单元尺寸（由激活窗口的字号决定，见 apply_grid/activate）
extern int kCols;                   // 列数
extern int kRows;                   // 行数
extern int kCellW;                  // 字符包围框宽（字号单元）
extern int kCellH;                  // 单元高（字号 4/6/8 → 7/10/14）

struct Options {
  uint32_t     bg_color   = 0x000000; // 默认背景色（RGB888）
  const char*  bg_image   = nullptr;// 背景图路径（空 = 纯色背景）
  int          bg_image_transparency = 0; // 背景图透明度 0–90（0=直接显示背景图，
                                    //   越高背景图越黯淡、背景色越明显）
  const uint32_t* palette16 = nullptr; // ANSI 16 色 RGB888（空 = 内置）
  uint32_t     cursor_color = 0; // 光标颜色 RGB888（0 = 同 bright7；config cursor）
  int          view_hold_ms = 2000; // 手动翻页后保持不跳底的时长（ms）
  int          scrollback = 10000;  // 最大回滚行数
  bool         unicode    = true;   // 向子进程宣称 Unicode（UTF-8 locale）；false → LANG=C
  int          font_size   = 6;     // 初始字号：4 = 4x7，6 = 6x10（默认），
                                    //   8 = 8x14（暂用 6x10 字形）；每个窗口可单独调整
  cfg::UiColors ui;                 // toast/提示框/输入法框配色覆盖（0 = 默认 bright 色系）
};

void init(const Options& opt);      // 画布+清屏；默认光标 (0,0)

// 满宽行 + LF/CRLF 的语义开关（config [general] full_line_newline，进程级）：
//   false（默认）= xterm/vt.c：LF 只下移一行并取消延迟环绕
//   true  = 先提交自动换行再下移（显示「满列行/空行/后续文本」，缩放时会并回）
void set_full_line_newline(bool on);
// ── 多窗口（0.3.0）：每窗口一份终端状态 ──────────────────────────
// state_new 创建并复位窗口状态（背景画布全局共享）；activate 设为当前
// 解析/绘制目标（write/present/putc 等只作用于当前状态）；state_free 释放。
struct State;
State* state_new(const Options& opt);
void   state_free(State* s);
void   activate(State* s);
// 改变某窗口的字号（4/6/8）：rewrap —— 新建目标宽度窗口、复制数据流
// （软换行合并/拆分）、删除旧窗口；回滚历史随之保留。可能返回新的 State*
// 指针（永远与入参不同宽时重建），调用方须用返回值替换旧指针，随后
// present() 并给 PTY 发 TIOCSWINSZ。备用屏内容不 rewrap（foot/VTE 惯例）。
State* state_resize(State* s, int font_size);
int    state_font_size(const State* s);
// 静默解析：开启期间 write/tick 不自动上屏（批量解析后台窗口用），
// 关闭后由调用方 tick/present 补画。
void   set_quiet(bool on);
// 标记当前状态整屏脏（切窗后必须整屏重画，防旧窗口残留）
void   invalidate();
// 视图立即跳回底部（翻页以外按键时由 session 调用；切窗时对目标窗口也调用）
void   view_follow();
// 终端回复通道（DSR/DA 等查询需要回写应用；PTY 会话注入）
using Writer = void (*)(const char*, size_t);
void set_writer(Writer w);
// DECCKM 应用光标键模式状态（键盘序列据此选择 SS3/CSI 形式）
bool app_cursor_keys();
// kitty 键盘协议当前增强标志（0=未启用；见 CHANGELOG 0.2.3）
uint32_t kkp_flags();

// 输出 API 即"伪终端上屏口"。批量输出按"脏行合帧"上屏：
// 解析只改屏缓冲，真正绘制被合并——至多每 kFrameMs 一次，
// 或输出暂停（缓冲空）时立即上屏，绝不逐字符刷屏。
void write(const char* s);          // 输出文本（解析 ANSI 转义序列）
void write(const char* s, size_t n);// 输出 n 字节（PTY 数据可能含 \0）
void putc(char c);                  // 输出单字符（\n \r \b \t 均处理）
void backspace();                   // 退格并擦除（键盘 Backspace）
void present();                     // 立即上屏（把当前脏行画出来 + 光标）
void fill_bg();                     // 整屏填背景色/背景图（缩放 rewrap 前清残影用）
void tick();                    // 光标闪烁节拍（UI 循环每 ≤100ms 调用；兼作合帧节拍）
void cursor_reblink();          // 光标回到亮块相并重启闪烁计时（非翻页键按下时调用）
// 光标颜色/形状/闪烁/显隐恢复配置默认（0.7.2）：OSC 12 改过光标色的 TUI 退出后不残留。
// RIS(ESC c) / DECSTR(CSI ! p) / OSC 112 / 本函数共用同一实现；只重画光标所在行，
// 不改屏内容/滚动位置/光标位置。session 在子进程结束、窗口回到启动器时调用兜底。
void reset_cursor_style();
void scroll_view(int delta);        // 翻看回滚：>0 更旧，<0 更新（回到底部）

// ── toast 叠加层（LovyanGFX 层自定义合成）────────────────────────────
// 屏幕最上方水平居中的圆角提示条，与终端网格/字号无关：
//   - 几何：上边距 14px、总高 24px（含 2px 无 AA 边框）、外圆角 8px / 内圆角 6px
//           （r8 样式）、内部可视区 20px；
//           宽度 = 文本宽 + 左右内边距各 7px + 边框 4px，上限屏宽（超宽截断文本）；
//   - 字体：固定 8×14（termfont::font_8x14），文字在内部区垂直/水平居中，中文双宽；
//   - 颜色：取当前窗口 16 色调色板 —— 背景 bright0(pal[8])、边框 bright4(pal[12])、
//           文字 bright3(pal[11])，经 to565 转 RGB565；
//   - 合成：画在所有终端行/光标之上，每次 present（合帧末）自动补画；由 tick() 到时清除。
//   text 为 UTF-8，可为 nullptr（忽略）；ms 为停留时长：>0 = 显式毫秒数
//   （既有 toast(text, 2000) 调用行为不变）；<=0（含默认参数 0）= 用
//   set_toast_default_ms() 配置的默认时长（未配置时 2000ms）。
void toast(const char* text, int ms = 0);
// 全屏倒计时退出提示（剥离/关闭最后一个窗口、末窗程序自然退出时用）：
// 42px 上下 / 32px 左右边距的圆角提示框，toast 同款渲染，直接叠在终端内容上
// （边缘透出之前的输出）；显示前清除屏上 toast，同屏不再出现 toast。
// 三行文本居中；第三行 "cardterm will exit in Ns." 逐秒重绘 3→2→1 后返回。
// line1 为场景文案（如 "PTY 03 exited with 0." / "Detached PTY 03."），第二行固定
// "No terminal running."。叠加渲染不依赖终端状态（窗口已关闭、T 可能为空）。
void exit_prompt(const char* line1);
// 设置 toast 默认停留时长（进程级）：toast(text)/toast(text, 0) 由此取值；
// ms<=0 时按 2000 兜底。只影响此后新建的 toast。
void set_toast_default_ms(int ms);
void set_toast_enabled(bool on);    // 总开关：关闭后 toast() 直接返回，并撤销屏上现存 toast
bool toast_active();                // 屏上当前是否有 toast（供 session/tick 判断，可选）

// ── 会话与窗口关闭语义（与普通电脑关闭终端窗口一致）──────────────
void attach_process(long pid);      // 登记前台子进程（shell/PTY 挂接点）
void close_window();                // SIGHUP → 宽限 → SIGKILL → 返回

// ── 叠加层扩展（1.3.0-rc003 拼音候选框；只增不改，旧路径不受影响）──
// 光标所在格（0-based 列/行）：拼音编辑行/候选框定位用
void cursor_cell(int* col, int* row);
// 配置调色板第 idx 项（0..15，RGB888）。不含 OSC4 动态色：拼音叠加层
// 不受终端程序控制（色彩取配置项，同 toast/全屏横幅的色彩配置来源）
uint32_t pal_config(int idx);
// toast/全屏横幅共用的三色（RGB888）：toast_* 覆盖优先，否则当前窗口
// 调色板 bright0/4/7（含 OSC4 动态色，与 toast 渲染同源）。拼音框三色
// 角色（白=文字/高亮底、暗=框底/高亮字、蓝=边框/Tab 底）取自这里，
// 保证与 toast/全屏横幅永远一致。
void ui_chrome_colors(uint32_t* bg, uint32_t* border, uint32_t* fg);
// 合帧补画钩子：present_now() 在终端行/光标合成之后、toast 之前调用。
// 图层规范（低→高）：终端内容 → toast → 拼音输入框 → 全屏提示框；
// 拼音候选框/编辑行注册后每帧压在 toast 之上、全屏提示框之下
void set_overlay_hook(void (*fn)());
// 按像素行范围标脏（换算成字符行）：叠加层消失/移动时擦除残影用。
// 标脏后由 tick()/maybe_present 的正常合帧路径重画这些行
void mark_pixel_rows(int py0, int py1);
// toast/提示框/输入法框 配色覆盖（0 = 默认 bright 色系；config [colors]）
const cfg::UiColors& ui_colors();
// 文本原语（字体表只在 terminal.cpp 一份，叠加层共用；行内全半角逐码点判格数）：
//   text_cells814  — 8x14 表测宽（格数；cap_cells>0 时按格截断，不切半格）
//   draw_text814   — 8x14 文本；opaque=true 铺 bg 后点字形一次上屏，false 只画墨迹
//   draw_text_ink_win — 窗口字号（4x7/6x10/8x14）只画墨迹，超出屏幕右缘不画
//   text_px610 / draw_text610 — 6x10 小字（拼音状态行 N/M Tab 用；测宽为像素）
//   toast_box_size / toast_box_at — toast 外观提示框（24px 高 r8 + 2px 边框 +
//     8x14 文本，排版渲染与 toast 同口径）画在指定左上角；三色由调用方给
int  text_cells814(const char* utf8, int cap_cells);
void draw_text814(int x, int y, const char* utf8, uint32_t fg, uint32_t bg,
                  bool opaque, int cap_cells);
void draw_text_ink_win(int x, int y, const char* utf8, uint32_t fg, int cap_cells);
int  text_px610(const char* utf8);
void draw_text610(int x, int y, const char* utf8, uint32_t fg, uint32_t bg, bool opaque);
int  text_px4x7(const char* utf8);
void draw_text4x7(int x, int y, const char* utf8, uint32_t fg, uint32_t bg, bool opaque);
void toast_box_size(const char* utf8, int* w, int* h);
void toast_box_at(int x, int y, const char* utf8, uint32_t bg, uint32_t border, uint32_t fg);

// ── 半透明窗口合成（魔数蒙版模型；toast/全屏提示/输入候选框共用口径）────
// 画布 = 一只双半幅 LGFX_Sprite（w×2h 的 16 位 RGB565）：上半 = 画布1（内容
// 层：魔数填充 = 透明区标记，UI 经 ov_dst() 以 (0,0) 原点画在其上）；下半 =
// 画布2（合成暂存）。内容不变期间常驻复用，不随屏幕更新重画。
//   ov_begin(x, y, w, h, transparency, content_sig, magic)：
//     true = 内容需重画（调用方把 UI 画进画布1）；false = 本轮跳过绘制。
//     仅当"屏幕更新与叠加层行相交"或"内容指纹变化"才进入合成。
//   ov_end()：被盖行先重合成干净底，画布2 读屏 → 画布1 非魔数像素按透明度
//     混合进画布2（魔数像素不拷、原样透出）→ 画布2 整块回屏。
//   ov_magic(base, avoid, n)：魔数色 = 默认底色 565 值 +1（溢出回绕），与
//     叠加层用色（avoid）撞车则继续 +1。
// transparency 0 = 不透明快速路径：不建画布，ov_dst() 直指屏幕每帧直绘
// （与 rc200 一致），ov_begin 恒返回 true、ov_end 为空操作。
uint16_t ov_magic(uint16_t base, const uint16_t* avoid, int n);
bool ov_begin(int x, int y, int w, int h, int transparency, uint32_t content_sig, uint16_t magic);
void ov_end();

}  // namespace term
