// config.cpp — cardterm 配置文件（INI）解析
#include "config.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace cfg {
namespace {

// 家目录（HOME 缺失时回退 passwd，不写死任何用户名）：
std::string home_dir()
{
  const char* h = getenv("HOME");
  if (h && *h) return h;
  struct passwd* pw = getpwuid(getuid());
  if (pw && pw->pw_dir && *pw->pw_dir) return pw->pw_dir;
  return "/tmp";
}

// 用户默认可交互 shell（默认配置 launch= 的初值）：passwd 登录 shell → $SHELL → /bin/sh。
// 通常 bash，也可能是 zsh/fish 等，不写死。
std::string default_shell()
{
  struct passwd* pw = getpwuid(getuid());
  if (pw && pw->pw_shell && *pw->pw_shell) return pw->pw_shell;
  const char* sh = getenv("SHELL");
  if (sh && *sh) return sh;
  return "/bin/sh";
}

// 默认配置模板：每一项上方都有 1-2 行英文说明（取值/默认值/作用），面向全球用户。
// 保持与 load() 的解析逻辑兼容；新增键（toast / splash）也在模板中给出示例。
const char* kDefaultIni =
    "# cardterm configuration file (INI).\n"
    "# Lines starting with '#' or ';' are comments; unknown keys are ignored.\n"
    "# The file is read again at every start, so edits apply after a restart.\n"
    "\n"
    "[general]\n"
    "# Program started automatically when cardterm is launched (space-separated argv).\n"
    "# Default: your default interactive shell (login shell from /etc/passwd).\n"
    "# Leave empty to show the launcher instead (type a command there, e.g. bash/ssh).\n"
    "launch=bash\n"
    "# Working directory for that program:\n"
    "#   home / ~ / $HOME = home directory; ~/foo = home/foo; anything else = literal\n"
    "#   path; empty = inherit cardterm's working directory.\n"
    "cwd=home\n"
    "# Number of terminal window slots (2-32, default 12).\n"
    "# Shortcut keys targeting a slot above this count only show a toast\n"
    "# (\"Max PTY is NN. Edit config.ini.\"); windows above 12 are reached with Fn+W / Fn+E.\n"
    "windows=12\n"
    "# Tell programs that this terminal supports Unicode (UTF-8 locale):\n"
    "#   yes = force LANG/LC_ALL=C.UTF-8 when the inherited locale is not UTF-8 (default)\n"
    "#   no  = LANG=C, so TUI programs draw ACS lines instead of wide glyphs\n"
    "unicode=yes\n"
    "# A full-width line followed by a newline (LF):\n"
    "#   no  = xterm/vt.c semantics: LF only moves down one line (default)\n"
    "#   yes = commit the pending auto-wrap first, showing a blank line after a full row\n"
    "full_line_newline=no\n"
    "# Show transient toast notifications (window switch / font size / window exit):\n"
    "#   yes = enabled (default), no = disabled\n"
    "toast=yes\n"
    "# How long each toast stays on screen, in milliseconds (200-60000, default 2000).\n"
    "# Values outside the range are clamped to the nearest bound.\n"
    "toast_ms=2000\n"
    "\n"
    "[ime]\n"
    "# Chinese (pinyin, RIME) input subsystem.\n"
    "# Requires the RIME engine and the luna_pinyin dictionary. On Debian/Ubuntu:\n"
    "#   sudo apt install librime1t64 rime-data-luna-pinyin rime-prelude\n"
    "# Enable the Chinese input feature (Fn+Space switches EN<->CN):\n"
    "#   enable_zh_cn_input = true (default): enabled;\n"
    "#   false: the feature is disabled (Fn+Space shows a notice).\n"
    "enable_zh_cn_input=true\n"
    "# Candidate script conversion:\n"
    "#   simplified_traditional = all (default): no conversion; both simplified and\n"
    "#     traditional forms can appear as candidates;\n"
    "#     simplified: convert all candidates to simplified Chinese;\n"
    "#     traditional: convert all candidates to traditional Chinese.\n"
    "# This seeds the generated rime user config (luna_pinyin.custom.yaml) on first\n"
    "# run; afterwards edit that rime config file directly to change the conversion.\n"
    "simplified_traditional=all\n"
    "# Custom symbol table for the 'i' symbol picker in CN mode: one string, each\n"
    "# character is one selectable symbol, at most 891 symbols (99 pages max).\n"
    "# Empty (default): built-in 70-symbol set (，。？！、：；—…“”（）《》「」‘’\n"
    "#   【】〈〉『』￥℃·①②③④⑤⑥⑦⑧⑨⑩≤≥±≠≈∞√°↑↓←→■□●○◉◎△▷▽◁▲▼◆◇＠＃％＆※～).\n"
    "# Example: custom_symbols=，。？！、：；“”「」【】（）《》\n"
    "custom_symbols=\n"
    "\n"
    "[font]\n"
    "# Initial font size for every new window (runtime: Fn+U / Fn+I per window):\n"
    "#   4 = 4x7 small (80x24 cells), 6 = 6x10 normal (53x17, default),\n"
    "#   8 = 8x14 large (40x12; WenQuanYi Bitmap Song 14px + hand-drawn Latin)\n"
    "font_size=6\n"
    "\n"
    "[colors]\n"
    "# Terminal background color, RRGGBB hex (foot terminal default: 242424)\n"
    "background_color=242424\n"
    "# Optional background image (PNG/JPG), scaled to the 320x170 screen.\n"
    "# Uncomment and set a readable path; cells without an explicit ANSI background\n"
    "# show this image instead of background_color.\n"
    "#background_image=/path/to/image.png\n"
    "# Background image transparency (0-90, default 0 = show the image as-is):\n"
    "#   higher = the image grows dimmer and background_color shows through more.\n"
    "background_image_transparency=0\n"
    "# Cursor color, RRGGBB hex. Leave this line commented out to follow bright7.\n"
    "#cursor=ffffff\n"
    "\n"
    "# 16-color ANSI palette, RRGGBB hex (foot terminal defaults).\n"
    "# regular0-7 = normal colors (0 = black ... 7 = white); bright0-7 = bright variants.\n"
    "regular0=242424\n"
    "regular1=f62b5a\n"
    "regular2=47b413\n"
    "regular3=e3c401\n"
    "regular4=24acd4\n"
    "regular5=f2affd\n"
    "regular6=13c299\n"
    "regular7=e6e6e6\n"
    "\n"
    "bright0=616161\n"
    "bright1=ff4d51\n"
    "bright2=35d450\n"
    "bright3=e9e836\n"
    "bright4=5dc5f8\n"
    "bright5=feabf2\n"
    "bright6=24dfc4\n"
    "bright7=ffffff\n"
    "\n"
    "# Popup color scheme shared by toast, pinyin input box and the exit prompt.\n"
    "# Each value is a palette slot name (regular0-7 / bright0-7) or RRGGBB hex.\n"
    "# Leave empty to use the defaults (follow the palette):\n"
    "#   popup_bg_color = bright0, popup_border_color = bright4, popup_text_color = bright7.\n"
    "popup_bg_color=\n"
    "popup_border_color=\n"
    "popup_text_color=\n"
    "\n"
    "# Per-popup fine tuning (RRGGBB hex), overriding the popup_* scheme above.\n"
    "# toast / fullscreen prompt / input-method box colors.\n"
    "# Leave them commented out = default bright palette (same look as before).\n"
    "# 尽量使用较亮较鲜艳的彩色替代边框颜色 (prefer bright, vivid colors for borders).\n"
    "#toast_bg_color=636163\n"
    "#toast_border_color=5ac7ff\n"
    "#toast_text_color=ffffff\n"
    "#prompt_bg_color=636163\n"
    "#prompt_border_color=5ac7ff\n"
    "#prompt_text_color=ffffff\n"
    "#ime_bg_color=636163\n"
    "#ime_border_color=5ac7ff\n"
    "#ime_text_color=ffffff\n"
    "#ime_selected_bg_color=ffffff\n"
    "#ime_selected_text_color=636163\n"
    "#ime_status_bg_color=5ac7ff\n"
    "#ime_status_text_color=636163\n"
    "#ime_compose_color=e6e6e6\n"
    "# Overlay transparency (0-90, default 0 = opaque; 0 skips blending for speed).\n"
    "#   Higher = more see-through, capped at 90 so the overlay stays barely visible.\n"
    "#   toast_prompt_transparency: toast + fullscreen prompt; ime_transparency: IME box.\n"
    "toast_prompt_transparency=0\n"
    "ime_transparency=0\n"
    "\n"
    "[scrollback]\n"
    "# Max scrollback lines kept in memory for every window (100-30000, default 2000):\n"
    "#   values below 100 are treated as 100; values above 30000 are clamped to 30000\n"
    "#   (a note is printed to stderr when clamping).\n"
    "lines=2000\n"
    "# After manual scrolling (Shift+PgUp etc.) keep the view for this long (ms,\n"
    "# max 60000; values above that are clamped) before jumping back to the bottom;\n"
    "# any non-paging key jumps immediately.\n"
    "hold_ms=2000\n"
    "\n"
    "[background]\n"
    "# Background (detached) sessions: max bytes of the session's plain-text output\n"
    "# that is buffered while detached and pasted back into the scrollback on reattach\n"
    "# (16-4096 KB, default 256 = 256KB; at most 4MB history; colors are not kept).\n"
    "buffer_kb=256\n"
    "\n"
    "[text]\n"
    "# Splash hint text shown on every new-window / launcher screen, before the\n"
    "# command line. Empty (or key missing) = show NO hint text at all.\n"
    "# The default value below is the standard cardterm hint text - edit or clear it.\n"
    "#\n"
    "# Tags (plain text, not XML): <br> = line break; <fgcolor=RRGGBB>...</fgcolor>\n"
    "# and <bgcolor=RRGGBB>...</bgcolor> = true-color span. An unclosed color tag\n"
    "# lasts to the end of the text; colors may nest. Malformed tags such as\n"
    "# <fgcolor=zzz>, a lone </fgcolor> or an incomplete <br are printed literally -\n"
    "# that is also how to display a literal '<' or '>'.\n"
    "#\n"
    "# Placeholders (replaced before tag parsing):\n"
    "#   <pty_id>              = window identifier, e.g. \"PTY 01\" (1-based, two digits)\n"
    "#   <version>             = program version, e.g. \"1.0.0\" (no 'v' prefix)\n"
    "#   <term_size_font_size> = grid + cell size as one string, e.g. \"53x17 cells, 6x10px\"\n"
    "splash=<fgcolor=80c8ff>cardterm</fgcolor> <fgcolor=a0a0a0>v<version> terminal emulator</fgcolor> <fgcolor=ffffff><pty_id></fgcolor><br>  <fgcolor=FFFFFF><term_size_font_size></fgcolor> - Pinyin: <fgcolor=5AC7FF><pinyin_status></fgcolor><br>  fg16 <fgcolor=212421>##</fgcolor><fgcolor=F7285A>##</fgcolor><fgcolor=42B610>##</fgcolor><fgcolor=E7C700>##</fgcolor><fgcolor=21AED6>##</fgcolor><fgcolor=F7AEFF>##</fgcolor><fgcolor=10C39C>##</fgcolor><fgcolor=E7E7E7>##</fgcolor> <fgcolor=636163>##</fgcolor><fgcolor=FF4D52>##</fgcolor><fgcolor=31D752>##</fgcolor><fgcolor=EFEB31>##</fgcolor><fgcolor=5AC7FF>##</fgcolor><fgcolor=FFAAF7>##</fgcolor><fgcolor=21DFC6>##</fgcolor><fgcolor=FFFFFF>##</fgcolor><br>  bg16 <bgcolor=212421>  </bgcolor><bgcolor=F7285A>  </bgcolor><bgcolor=42B610>  </bgcolor><bgcolor=E7C700>  </bgcolor><bgcolor=21AED6>  </bgcolor><bgcolor=F7AEFF>  </bgcolor><bgcolor=10C39C>  </bgcolor><bgcolor=E7E7E7>  </bgcolor> <bgcolor=636163>  </bgcolor><bgcolor=FF4D52>  </bgcolor><bgcolor=31D752>  </bgcolor><bgcolor=EFEB31>  </bgcolor><bgcolor=5AC7FF>  </bgcolor><bgcolor=FFAAF7>  </bgcolor><bgcolor=21DFC6>  </bgcolor><bgcolor=FFFFFF>  </bgcolor><br>  RGB  <bgcolor=DC2828>  </bgcolor><bgcolor=28B450>  </bgcolor><bgcolor=325ADC>  </bgcolor><bgcolor=F0C828>  </bgcolor> <fgcolor=FF0000>TUI</fgcolor> <fgcolor=00FF00>CLI</fgcolor> <fgcolor=0000FF>colors</fgcolor><br>  Enter command (bash/ssh/codex...)<br>  to run it in this terminal.<br>  Fn+W / Fn+E: switch terminal window.<br>  Hold ESC for 2s: kill all and close.<br>  Press <fgcolor=F0C828>Fn+H</fgcolor> for more information.<br>\n";

void trim(char* s)
{
  size_t n = strlen(s);
  while (n && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
  size_t b = 0;
  while (s[b] && isspace((unsigned char)s[b])) b++;
  if (b) memmove(s, s + b, n - b + 1);
}

// 进程级状态：最近一次解析结果 + 是否新建了默认配置文件（首次运行）。
Config g_last;
bool   g_created = false;

uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// 按 UTF-8 字符数截断（custom_symbols 上限 891 个符号）；不完整尾字符丢弃
std::string utf8_clamp(const char* s, int max_chars)
{
  std::string out;
  const unsigned char* p = (const unsigned char*)s;
  int n = 0;
  while (*p != '\0' && n < max_chars) {
    int len = 1;
    if ((*p & 0xE0) == 0xC0) len = 2;
    else if ((*p & 0xF0) == 0xE0) len = 3;
    else if ((*p & 0xF8) == 0xF0) len = 4;
    else if ((*p & 0xC0) == 0x80) { p++; continue; }   // 孤立续字节：跳过
    bool ok = true;
    for (int i = 1; i < len; i++)
      if ((p[i] & 0xC0) != 0x80) { ok = false; break; }
    if (!ok) break;
    out.append((const char*)p, len);
    p += len;
    n++;
  }
  return out;
}

std::string config_path()
{
  const std::string home = home_dir();        // HOME 缺失回退 passwd，不写死用户名
  const std::string dir = home + "/.config/cardterm";
  mkdir((home + "/.config").c_str(), 0755);
  mkdir(dir.c_str(), 0755);
  return dir + "/config.ini";
}

}  // namespace

// 最近一次 load() 得到的配置（供 session 读取 splash/launch/toast）。
const Config& current() { return g_last; }

// 最近一次 load() 是否新建了默认配置文件（= 本次运行是首次运行）。
bool config_created() { return g_created; }

bool parse_hex_color(const char* s, uint32_t& out)
{
  if (s == nullptr) return false;
  if (*s == '#') s++;
  if (strlen(s) != 6) return false;
  char* end = nullptr;
  unsigned long v = strtoul(s, &end, 16);
  if (end == nullptr || *end != '\0') return false;
  out = (uint32_t)(v & 0x00FFFFFFu);              // RGB888
  return true;
}

// popup_* 配色取值：调色板槽位名 regular0-7 / bright0-7（编码 kUiPalFlag|idx，
// 绘制时按当前调色板动态解析，与"沿用 bright4"语义一致），或 RRGGBB 十六进制。
// 空串 = 未配置（留空沿用默认值）。
bool parse_color_value(const char* s, uint32_t& out)
{
  if (s == nullptr || *s == '\0') return false;
  if (!strncasecmp(s, "bright", 6) && s[6] >= '0' && s[6] <= '7' && s[7] == '\0') {
    out = kUiPalFlag | (8u + (uint32_t)(s[6] - '0'));
    return true;
  }
  if (!strncasecmp(s, "regular", 7) && s[7] >= '0' && s[7] <= '7' && s[8] == '\0') {
    out = kUiPalFlag | (uint32_t)(s[7] - '0');
    return true;
  }
  return parse_hex_color(s, out);
}

bool load(Config& out)
{
  const std::string path = config_path();
  std::ifstream f(path);
  if (!f.is_open()) {                                  // 缺省：写入默认样例后回读生效
    FILE* w = fopen(path.c_str(), "w");
    if (w) {
      // launch= 初值 = 用户默认可交互 shell（模板里的 launch=bash 是占位）
      std::string text = kDefaultIni;
      const std::string ph = "launch=bash\n";
      const size_t pos = text.find(ph);
      if (pos != std::string::npos)
        text.replace(pos, ph.size(), "launch=" + default_shell() + "\n");
      fputs(text.c_str(), w);
      fclose(w);
      g_created = true;                  // 本次运行 = 首次运行（已新建默认模板）
      return load(out);                  // 回读生效
    }
    return false;
  }

  // 跳过 UTF-8 BOM（EF BB BF）：BOM 会污染首行（如 "[general]" 认不出节名 →
  // 首节/首键整体失效）。config.ini 的逐行解析都建立在同一循环上，跳过文件
  // 开头这 3 字节即对全部解析生效。
  {
    char bom[3] = {0, 0, 0};
    f.read(bom, 3);
    const bool has_bom = (f.gcount() == 3 &&
                          (unsigned char)bom[0] == 0xEF &&
                          (unsigned char)bom[1] == 0xBB &&
                          (unsigned char)bom[2] == 0xBF);
    f.clear();                             // 不足 3 字节时清掉 eof/fail 状态
    if (!has_bom) f.seekg(0);              // 无 BOM：回到文件开头
  }

  char line[4096];                       // [text] splash 可能较长，放宽行/值缓冲
  char section[64] = "";
  for (;;) {
    if (!f.getline(line, sizeof(line))) {
      if (f.fail() && !f.eof()) { f.clear(); f.ignore(1 << 24, '\n'); continue; }   // L8：超长行丢余量继续，不再吞掉后续键
      break;
    }
    trim(line);
    if (line[0] == '\0' || line[0] == ';' || line[0] == '#') continue;
    if (line[0] == '[') {                              // 节
      snprintf(section, sizeof(section), "%s", line + 1);
      char* e = strchr(section, ']');
      if (e) *e = '\0';
      continue;
    }
    char* eq = strchr(line, '=');
    if (!eq) continue;
    *eq = '\0';
    char key[128], val[4096];
    snprintf(key, sizeof(key), "%.*s", (int)sizeof(key) - 1, line);
    snprintf(val, sizeof(val), "%.*s", (int)sizeof(val) - 1, eq + 1);
    trim(key); trim(val);

    if (!strcmp(section, "general")) {
      if (!strcmp(key, "launch")) out.launch = val;
      else if (!strcmp(key, "cwd")) out.cwd = val;
      else if (!strcmp(key, "unicode")) {
        const char* v = val;
        out.unicode = !(strcasecmp(v, "no") == 0 || strcasecmp(v, "false") == 0 ||
                        strcasecmp(v, "off") == 0 || strcmp(v, "0") == 0);
      }
      else if (!strcmp(key, "full_line_newline")) {   // 满宽行+LF 语义（默认 no = xterm）
        const char* v = val;
        out.full_line_newline = (strcasecmp(v, "yes") == 0 || strcasecmp(v, "true") == 0 ||
                                 strcasecmp(v, "on") == 0 || !strcmp(v, "1"));
      }
      else if (!strcmp(key, "toast")) {               // 事件通知（toast 叠加层）开关
        const char* v = val;
        out.toast = !(strcasecmp(v, "no") == 0 || strcasecmp(v, "false") == 0 ||
                      strcasecmp(v, "off") == 0 || strcmp(v, "0") == 0);
      }
      else if (!strcmp(key, "toast_ms")) {            // toast 显示时长（ms）
        int n = atoi(val);
        if (n > 0) {
          if (n < 200) n = 200;                       // 范围 200–60000：超限钳位
          if (n > 60000) {
            n = 60000;
          }
          out.toast_ms = n;
        }
      }
      else if (!strcmp(key, "windows")) {             // 窗口槽位数（2–32）
        int n = atoi(val);
        if (n > 0) {
          if (n < 2) n = 2;                           // 超出范围钳位到最近边界
          if (n > 32) {
            n = 32;
          }
          out.windows = n;
        }
      }
    } else if (!strcmp(section, "font")) {
      if (!strcmp(key, "font_size")) { int n = atoi(val); if (n > 0) out.font_size = n; }

    } else if (!strcmp(section, "ime")) {               // 中文输入（拼音）子系统
      const bool falsy = (strcasecmp(val, "no") == 0 || strcasecmp(val, "false") == 0 ||
                          strcasecmp(val, "off") == 0 || strcmp(val, "0") == 0);
      if (!strcmp(key, "enable_zh_cn_input")) {         // 功能开关（默认启用）
        out.ime_enabled = !falsy;
      } else if (!strcmp(key, "simplified_traditional")) {  // all / simplified / traditional
        if (strcasecmp(val, "simplified") == 0) out.ime_script = ime::Script::Simplified;
        else if (strcasecmp(val, "traditional") == 0) out.ime_script = ime::Script::Traditional;
        else out.ime_script = ime::Script::All;         // all（默认）或未识别值 = 不转换
      } else if (!strcmp(key, "custom_symbols")) {      // i 符号框自定义符号串（上限 891 个）
        out.ime_custom_symbols = utf8_clamp(val, 891);
      // ── 旧键（rc108 及更早）兼容读取（同段同名后写覆盖前写）────────
      } else if (!strcmp(key, "enabled")) {
        out.ime_enabled = !falsy;
      } else if (!strcmp(key, "simplified")) {          // yes=仅简体 / no=仅繁体
        out.ime_script = falsy ? ime::Script::Traditional : ime::Script::Simplified;
      } else if (!strcmp(key, "symbols")) {             // half=半角预设串
        if (strcasecmp(val, "half") == 0 || strcasecmp(val, "halfwidth") == 0 ||
            strcasecmp(val, "english") == 0 || strcasecmp(val, "halfwidth_english") == 0)
          out.ime_custom_symbols = ",.?!;:'\"()[]{}<>/\\|~@#$%^&*-_+=`";
      }

    } else if (!strcmp(section, "background")) {
      if (!strcmp(key, "buffer_kb")) {                // 后台会话恢复贴回文本上限（KB）
        int n = atoi(val);
        if (n > 0) {
          if (n < 16) n = 16;
          if (n > 4096) {                 // 上限 4MB：与守护进程捕获截断一致（rc212/M4）
            n = 4096;
          }
          out.bg_buffer_kb = n;
        }
      }
    } else if (!strcmp(section, "colors")) {
      if (!strcmp(key, "background_color")) parse_hex_color(val, out.bg_color);
      else if (!strcmp(key, "cursor")) parse_hex_color(val, out.cursor_color);
      else if (!strcmp(key, "background_image")) out.bg_image = val;
      else if (!strcmp(key, "background_image_transparency")) {
        int v = atoi(val); if (v < 0) v = 0; if (v > 90) v = 90;
        out.bg_image_transparency = v;
      } else if (!strcmp(key, "toast_prompt_transparency")) {
        int v = atoi(val); if (v < 0) v = 0; if (v > 90) v = 90;
        out.ui.toast_prompt_transparency = v;
      } else if (!strcmp(key, "ime_transparency")) {
        int v = atoi(val); if (v < 0) v = 0; if (v > 90) v = 90;
        out.ui.ime_transparency = v;
      } else if (!strcmp(key, "popup_bg_color")) parse_color_value(val, out.ui.popup_bg);
      else if (!strcmp(key, "popup_border_color")) parse_color_value(val, out.ui.popup_border);
      else if (!strcmp(key, "popup_text_color")) parse_color_value(val, out.ui.popup_text);
      else if (!strcmp(key, "toast_bg_color")) parse_hex_color(val, out.ui.toast_bg);
      else if (!strcmp(key, "toast_border_color")) parse_hex_color(val, out.ui.toast_border);
      else if (!strcmp(key, "toast_text_color")) parse_hex_color(val, out.ui.toast_text);
      else if (!strcmp(key, "prompt_bg_color")) parse_hex_color(val, out.ui.prompt_bg);
      else if (!strcmp(key, "prompt_border_color")) parse_hex_color(val, out.ui.prompt_border);
      else if (!strcmp(key, "prompt_text_color")) parse_hex_color(val, out.ui.prompt_text);
      else if (!strcmp(key, "ime_bg_color")) parse_hex_color(val, out.ui.ime_bg);
      else if (!strcmp(key, "ime_border_color")) parse_hex_color(val, out.ui.ime_border);
      else if (!strcmp(key, "ime_text_color")) parse_hex_color(val, out.ui.ime_text);
      else if (!strcmp(key, "ime_selected_bg_color")) parse_hex_color(val, out.ui.ime_sel_bg);
      else if (!strcmp(key, "ime_selected_text_color")) parse_hex_color(val, out.ui.ime_sel_text);
      else if (!strcmp(key, "ime_status_bg_color")) parse_hex_color(val, out.ui.ime_status_bg);
      else if (!strcmp(key, "ime_status_text_color")) parse_hex_color(val, out.ui.ime_status_text);
      else if (!strcmp(key, "ime_compose_color")) parse_hex_color(val, out.ui.ime_compose);
      else if (!strncmp(key, "regular", 7)) {
        int i = atoi(key + 7);
        if (i >= 0 && i < 8) parse_hex_color(val, out.palette[i]);
      } else if (!strncmp(key, "bright", 6)) {
        int i = atoi(key + 6);
        if (i >= 0 && i < 8) parse_hex_color(val, out.palette[8 + i]);
      }
    } else if (!strcmp(section, "scrollback")) {
      if (!strcmp(key, "lines")) {
        int n = atoi(val);
        if (n > 0) {
          if (n > 30000) {                            // 上限 30000：超限钳位并记一行
            n = 30000;
          }
          if (n < 100) n = 100;                       // 下限 100：低于 100 仍按 100
          out.scrollback = n;
        }
      } else if (!strcmp(key, "hold_ms")) {
        int n = atoi(val);
        if (n > 0) {
          if (n > 60000) {                            // 上限 60000（60s）：超限钳位并记一行
            n = 60000;
          }
          out.view_hold_ms = n;
        }
      }
    } else if (!strcmp(section, "text")) {             // 开屏页自定义文本
      if (!strcmp(key, "splash")) out.splash = val;
    }
  }
  g_last = out;                          // 供 session::run 读取（splash/launch/toast）
  return true;
}

}  // namespace cfg
