// ui.cpp — cardterm v0.3.0：配置加载 + 窗口管理器入口
//
//   - ~/.config/cardterm/config.ini → term::Options（调色板/背景图/回滚）
//   - 会话/多窗口/启动器逻辑在 session.cpp（窗口管理器）
#include "ui.h"
#include <cstdio>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include "keyboard.h"
#include "config.h"
#include "terminal.h"
#include "session.h"
#include "termfont_sizes.h"

int ui(const std::vector<std::string>& args)
{
  cfg::Config conf;
  cfg::load(conf);                       // ~/.config/cardterm/config.ini

  // 字号：4 = 4x7，6 = 6x10（默认），8 = 8x14（文泉驿点阵宋体 14px + 手绘西文）。
  // 新的窗口从该字号开始；每个窗口可在运行时用 FN+U/I 单独调整。
  if (conf.font_size != 4 && conf.font_size != 6 && conf.font_size != 8) {
    fprintf(stderr, "[cfg] font_size=%d: only 4 (4x7), 6 (6x10), 8 (8x14) are available\n",
            conf.font_size);
    conf.font_size = 6;
  }

  term::Options opt;
  opt.bg_color   = conf.bg_color;
  opt.bg_image   = conf.bg_image.empty() ? nullptr : conf.bg_image.c_str();
  opt.palette16  = conf.palette;
  opt.scrollback = conf.scrollback;
  opt.view_hold_ms = conf.view_hold_ms;
  opt.cursor_color = conf.cursor_color;
  opt.unicode    = conf.unicode;
  opt.font_size  = conf.font_size;

  if (!kbd::open()) {                    // 打开并独占板载键盘
    term::State* s = term::state_new(opt);
    if (s != nullptr) {
      term::activate(s);
      term::write("\033[38;2;255;80;80m(kbd open failed)\033[0m");
      term::present();
    }
    lgfx::delay(2000);
    return 1;
  }
  // config.ini [general] launch=<cmd>：无参数启动时运行默认命令
  std::vector<std::string> first = args;
  if (first.empty() && !conf.launch.empty()) {
    std::string cur;
    for (char c : conf.launch) {
      if (c == ' ' || c == '\t') { if (!cur.empty()) { first.push_back(cur); cur.clear(); } }
      else cur += c;
    }
    if (!cur.empty()) first.push_back(cur);
  }
  // config.ini [general] cwd=：启动工作目录。APPLaunch 以 /usr/share/APPLaunch
  // 为 WorkingDirectory 启动应用，不切换的话 PTY 子 shell 会落在那里。
  // home / ~ / $HOME → 家目录；~/xxx → 家目录下 xxx；其余按字面路径；空 = 继承。
  if (!conf.cwd.empty()) {
    const char* h = getenv("HOME");
    std::string home = (h && *h) ? h : "/home/me";
    std::string d = conf.cwd;
    if (d == "home" || d == "~" || d == "$HOME") d = home;
    else if (d.size() >= 2 && d[0] == '~' && d[1] == '/') d = home + d.substr(1);
    if (chdir(d.c_str()) != 0) fprintf(stderr, "[ui] chdir(%s) failed\n", d.c_str());
  }
  term::set_full_line_newline(conf.full_line_newline);   // 满宽行+LF 语义（默认 xterm）
  term::set_toast_enabled(conf.toast);                   // 事件通知（toast 叠加层）总开关
  term::set_toast_default_ms(conf.toast_ms);             // toast 显示时长（[general] toast_ms）
  int rc = session::run(first, opt);      // 窗口管理器（12 窗口 + 极简启动器）
  kbd::close();
  return rc;
}
