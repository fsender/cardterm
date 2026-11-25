// config.cpp — cardterm 配置文件（INI）解析
#include "config.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sys/stat.h>
#include <sys/types.h>

namespace cfg {
namespace {

// 默认配置模板：每一项上方都有 1-2 行英文说明（取值/默认值/作用），面向全球用户。
// 保持与 load() 的解析逻辑兼容；新增键（toast / splash）也在模板中给出示例。
const char* kDefaultIni =
    "# cardterm configuration file (INI).\n"
    "# Lines starting with '#' or ';' are comments; unknown keys are ignored.\n"
    "# The file is read again at every start, so edits apply after a restart.\n"
    "\n"
    "[general]\n"
    "# Program started automatically when cardterm is launched (space-separated argv).\n"
    "# Leave empty to show the launcher instead (type a command there, e.g. bash/ssh).\n"
    "launch=bash\n"
    "# Working directory for that program:\n"
    "#   home / ~ / $HOME = home directory; ~/foo = home/foo; anything else = literal\n"
    "#   path; empty = inherit cardterm's working directory.\n"
    "cwd=home\n"
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
    "[font]\n"
    "# Initial font size for every new window (runtime: FN+U / FN+I per window):\n"
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
    "splash=<fgcolor=80c8ff>cardterm</fgcolor> <fgcolor=a0a0a0>v<version> terminal emulator</fgcolor> <fgcolor=ffffff><pty_id></fgcolor><br>  Window size: <term_size_font_size><br>  fg16 <fgcolor=212421>##</fgcolor><fgcolor=F7285A>##</fgcolor><fgcolor=42B610>##</fgcolor><fgcolor=E7C700>##</fgcolor><fgcolor=21AED6>##</fgcolor><fgcolor=F7AEFF>##</fgcolor><fgcolor=10C39C>##</fgcolor><fgcolor=E7E7E7>##</fgcolor> <fgcolor=636163>##</fgcolor><fgcolor=FF4D52>##</fgcolor><fgcolor=31D752>##</fgcolor><fgcolor=EFEB31>##</fgcolor><fgcolor=5AC7FF>##</fgcolor><fgcolor=FFAAF7>##</fgcolor><fgcolor=21DFC6>##</fgcolor><fgcolor=FFFFFF>##</fgcolor><br>  bg16 <bgcolor=212421>  </bgcolor><bgcolor=F7285A>  </bgcolor><bgcolor=42B610>  </bgcolor><bgcolor=E7C700>  </bgcolor><bgcolor=21AED6>  </bgcolor><bgcolor=F7AEFF>  </bgcolor><bgcolor=10C39C>  </bgcolor><bgcolor=E7E7E7>  </bgcolor> <bgcolor=636163>  </bgcolor><bgcolor=FF4D52>  </bgcolor><bgcolor=31D752>  </bgcolor><bgcolor=EFEB31>  </bgcolor><bgcolor=5AC7FF>  </bgcolor><bgcolor=FFAAF7>  </bgcolor><bgcolor=21DFC6>  </bgcolor><bgcolor=FFFFFF>  </bgcolor><br>  RGB  <bgcolor=DC2828>  </bgcolor><bgcolor=28B450>  </bgcolor><bgcolor=325ADC>  </bgcolor><bgcolor=F0C828>  </bgcolor> <fgcolor=FF0000>TUI</fgcolor> <fgcolor=00FF00>CLI</fgcolor> <fgcolor=0000FF>colors</fgcolor><br>  Enter command (bash/ssh/codex...)<br>  to run it in this terminal.<br>  FN+W / FN+E: switch terminal window.<br>  Hold ESC for 2s: kill all and close.<br>  Type <fgcolor=F0C828>\"help\"</fgcolor> for more information.<br>\n";

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

std::string config_path()
{
  const char* home = getenv("HOME");
  std::string dir = (home && *home) ? home : "/home/me";
  dir += "/.config/cardterm";
  mkdir((std::string(getenv("HOME") ? getenv("HOME") : "/home/me") + "/.config").c_str(), 0755);
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

bool load(Config& out)
{
  const std::string path = config_path();
  std::ifstream f(path);
  if (!f.is_open()) {                                  // 缺省：写入默认样例后回读生效
    FILE* w = fopen(path.c_str(), "w");
    if (w) {
      fputs(kDefaultIni, w);
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
  while (f.getline(line, sizeof(line))) {
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
            fprintf(stderr, "[cfg] toast_ms=%d clamped to 60000\n", n);
            n = 60000;
          }
          out.toast_ms = n;
        }
      }
    } else if (!strcmp(section, "font")) {
      if (!strcmp(key, "font_size")) { int n = atoi(val); if (n > 0) out.font_size = n; }

    } else if (!strcmp(section, "colors")) {
      if (!strcmp(key, "background_color")) parse_hex_color(val, out.bg_color);
      else if (!strcmp(key, "cursor")) parse_hex_color(val, out.cursor_color);
      else if (!strcmp(key, "background_image")) out.bg_image = val;
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
            fprintf(stderr, "[cfg] lines=%d clamped to 30000\n", n);
            n = 30000;
          }
          if (n < 100) n = 100;                       // 下限 100：低于 100 仍按 100
          out.scrollback = n;
        }
      } else if (!strcmp(key, "hold_ms")) {
        int n = atoi(val);
        if (n > 0) {
          if (n > 60000) {                            // 上限 60000（60s）：超限钳位并记一行
            fprintf(stderr, "[cfg] hold_ms=%d clamped to 60000\n", n);
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
