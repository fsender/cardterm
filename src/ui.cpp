// ui.cpp — cardterm v0.3.0：配置加载 + 窗口管理器入口
//
//   - ~/.config/cardterm/config.ini → term::Options（调色板/背景图/回滚）
//   - 会话/多窗口/启动器逻辑在 session.cpp（窗口管理器）
#include "ui.h"
#include <cstdio>
#include <cstring>
#include <errno.h>
#include <pwd.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include "log.h"
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
  if (conf.font_size != 4 && conf.font_size != 6 && conf.font_size != 8)
    conf.font_size = 6;


  term::Options opt;
  opt.bg_color   = conf.bg_color;
  opt.bg_image   = conf.bg_image.empty() ? nullptr : conf.bg_image.c_str();
  opt.bg_image_transparency = conf.bg_image_transparency;
  opt.palette16  = conf.palette;
  opt.scrollback = conf.scrollback;
  opt.view_hold_ms = conf.view_hold_ms;
  opt.cursor_color = conf.cursor_color;
  opt.unicode    = conf.unicode;
  opt.font_size  = conf.font_size;
  opt.ui         = conf.ui;      // toast/提示框/输入法框配色覆盖（0 = 默认 bright 色系）

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
    std::string home;
    if (h && *h) home = h;
    else {
      // HOME 缺失：回退 passwd 家目录（不写死任何用户名）
      struct passwd* pw = getpwuid(getuid());
      home = (pw && pw->pw_dir && *pw->pw_dir) ? pw->pw_dir : "/tmp";
    }
    std::string d = conf.cwd;
    if (d == "home" || d == "~" || d == "$HOME") d = home;
    else if (d.size() >= 2 && d[0] == '~' && d[1] == '/') d = home + d.substr(1);
    // 进程级 chdir（1.3.0-rc207 落地）：此后所有 PTY 子进程继承该目录。
    // 失败（目录不存在/无权限）：保持继承目录不阻断启动，日志留痕供排查。
    if (chdir(d.c_str()) != 0)
      logf("[ui] cwd chdir %s: %s\n", d.c_str(), strerror(errno));
  }
  term::set_full_line_newline(conf.full_line_newline);   // 满宽行+LF 语义（默认 xterm）
  term::set_toast_enabled(conf.toast);                   // 事件通知（toast 叠加层）总开关
  term::set_toast_default_ms(conf.toast_ms);             // toast 显示时长（[general] toast_ms）
  int rc = session::run(first, opt);      // 窗口管理器（12 窗口 + 极简启动器）
  kbd::close();
  return rc;
}
