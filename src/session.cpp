// session.cpp — PTY 会话实现（见 session.h）
#include "session.h"

#include "bgtask.h"
#include "config.h"
#include "exclusive_display.h"
#include "keyboard.h"
#include "terminal.h"

#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

namespace session {
namespace {

// 网格维度为运行时值（terminal.h: term::kRows / kCols），由字体包围框计算

bool g_dbg = false;                     // CARDTERM_SESS_DEBUG：打印输入/写出细节
int  g_master = -1;                     // 当前窗口 master（终端 DSR/DA/OSC52 应答写回用）
bool g_unicode = true;                  // config.ini [general] unicode=：是否向程序宣称 Unicode
int  g_fg_tick = 0;                     // sync_fg_programs 降频计数
bool g_quit_after_close = false;        // 关闭最后一个终端窗口 → 退出程序

// ── PTY 出站写路径：非阻塞 + 有界队列 ──────────────────────────────
// 背景：master 若直接阻塞写，子进程不读 stdin（或 OSC52 大应答塞满 slave
// 输入队列）时主线程会卡死在 write() 上，键盘/渲染全部停摆。
// 方案：master 设 O_NONBLOCK，写不完的字节进各窗口自己的出站队列，主循环按
// 字节/时间预算分轮排空；队列有上限，超限丢弃数据并记一次日志。
//   按键流 out_key  ：保序（一次按键序列整体入队/整体丢，保证流内字节顺序）
//   应答流 out_reply：可丢（OSC52/DA 大应答截断/丢弃远好于卡死 UI）
constexpr size_t   kOutCapBytes   = 64 * 1024;  // 每窗口每条流出站队列上限
constexpr size_t   kOutRoundBytes = 16 * 1024;  // 主循环每轮出站写入字节预算
constexpr uint64_t kOutRoundMs    = 2;          // 主循环每轮出站写入时间预算(ms)

uint64_t now_ms()
{
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// 有界出站字节队列：以"一次写入调用"为元素整体入队/丢弃（不切开转义序列），
// 队首元素允许只写了一半（off = 已写出的偏移）。
struct OutQueue {
  std::deque<std::string> items;
  size_t bytes = 0;               // 未写出字节总数（含队首剩余）
  size_t off = 0;                 // 队首已写出的偏移
  size_t dropped = 0;             // 累计丢弃字节（诊断）
  bool overflow_logged = false;   // 本次积压只记一条日志（队空后复位）
  bool empty() const { return items.empty(); }
  void clear() { items.clear(); bytes = 0; off = 0; overflow_logged = false; }
};

// 单个终端窗口（12 槽轮换）
struct Win {
  bool open = false;                // 有会话在运行
  bool eof = false;                 // PTY 读到 EOF
  bool exec_failed = false;         // execvp 失败（命令未启动；与普通退出同规则）
  int  master = -1;
  pid_t child = 0;
  int  status = 0;
  term::State* ts = nullptr;        // 该窗口的终端状态
  bool launcher = false;            // 未开窗且显示启动器
  int  font_size = -1;              // 本窗口字号（4/6/8；-1 = 用配置默认）
  std::string line;                 // 启动器输入行
  OutQueue out_key;                 // 出站：按键流（保序）
  OutQueue out_reply;               // 出站：终端应答流（可丢）
  pid_t fg_pgrp = 0;                // 上次观测到的 PTY 前台进程组（回到 shell = TUI 退出 → 复位光标呈现）
  bool  cursor_reset_pending = false;  // 后台窗口待复位（切到前台时再做，避免污染当前屏）
  bool  adopted = false;            // 重附着接管的会话：child 非亲生（waitpid 不可用，
                                    //   退出检测走 master EOF，补刀按 pgid）
};

Win g_wins[32];
int g_win_count = 12;             // 窗口槽位数（[general] windows，2–32，默认 12；
                                  //   run() 里按配置赋值。上限 32：状态/回滚/出站队列
                                  //   均按打开的窗口分配，预留槽位仅 ~1.7KB/个）

int win_index(const Win& w) { return (int)(&w - g_wins); }

// 非阻塞写：返回已写出字节数；EINTR 重试，EAGAIN/EWOULDBLOCK 立即返回（0），
// 其它错误（EIO：slave 全关，写通道已断）返回 -1。
ssize_t out_write_some(int fd, const char* p, size_t n)
{
  size_t done = 0;
  while (n > 0) {
    ssize_t w = ::write(fd, p, n);
    if (w > 0) { done += (size_t)w; p += w; n -= (size_t)w; continue; }
    if (w < 0 && errno == EINTR) continue;
    if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
    if (w < 0) return -1;
    break;                                  // w == 0：PTY 不该出现，按异常处理
  }
  return (ssize_t)done;
}

// 溢出日志：一次积压只记一条（队空后复位），避免持续积压时刷屏
void out_log_overflow(const OutQueue& q, const char* tag, int win, const char* why)
{
  fprintf(stderr, "[session] out overflow win=%d stream=%s: %s"
                  " (dropped=%zu bytes, queued=%zu/%zu)\n",
          win + 1, tag, why, q.dropped, q.bytes, kOutCapBytes);
}

// 入队：超上限时先丢最旧元素（整条，不切开转义序列）直到放得下；
// 单条就超过上限则整条丢弃（半条 OSC52/转义序列比没有更糟）。
void out_enqueue(OutQueue& q, const char* tag, int win, const char* p, size_t n)
{
  if (n == 0) return;
  if (n > kOutCapBytes) {
    q.dropped += n;
    if (!q.overflow_logged) { q.overflow_logged = true; out_log_overflow(q, tag, win, "single chunk > cap, dropped whole"); }
    return;
  }
  while (!q.items.empty() && q.bytes + n > kOutCapBytes) {
    size_t avail = q.items.front().size() - q.off;
    q.bytes -= avail;
    q.dropped += avail;
    q.items.pop_front();
    q.off = 0;
    if (!q.overflow_logged) { q.overflow_logged = true; out_log_overflow(q, tag, win, "dropped oldest bytes"); }
  }
  q.items.emplace_back(p, n);
  q.bytes += n;
}

// 送一段字节到窗口 PTY：队列为空时先尝试立刻写（非阻塞），剩余入队。
// 绝不阻塞：EAGAIN 只意味着"下一轮再写"，不会拖住主循环。
void out_write(Win& w, OutQueue& q, const char* tag, const char* p, size_t n)
{
  if (n == 0 || w.master < 0) return;
  const int wi = win_index(w);
  if (q.empty()) {
    ssize_t ww = out_write_some(w.master, p, n);
    if (ww < 0) {                           // EIO：slave 全关（窗口即将 EOF 收尾）
      if (g_dbg) fprintf(stderr, "[session] out %s win=%d write: %s\n", tag, wi + 1, strerror(errno));
      q.clear();
      return;
    }
    if (g_dbg) fprintf(stderr, "[session] out %s win=%d direct write=%zd/%zu\n", tag, wi + 1, ww, n);
    p += ww; n -= (size_t)ww;
    if (n == 0) return;
  }
  out_enqueue(q, tag, wi, p, n);
  if (g_dbg) fprintf(stderr, "[session] out %s win=%d queued=%zu (+%zu)\n", tag, wi + 1, q.bytes, n);
}

// 按键流：整条按键序列入队（保序；队满时整条丢最旧按键，不切碎序列）
void feed_key(Win& w, const char* p, size_t n) { out_write(w, w.out_key, "key", p, n); }

// 把队列里的字节尽量写入 fd（非阻塞）。返回写出字节数；
// budget = 本轮剩余字节预算，deadline = 本轮时间上限（now_ms 绝对时刻）。
size_t out_flush(int fd, int win, OutQueue& q, size_t budget, uint64_t deadline)
{
  size_t total = 0;
  while (!q.empty() && total < budget) {
    if (now_ms() > deadline) break;
    std::string& f = q.items.front();
    size_t left = f.size() - q.off;
    size_t want = (left < budget - total) ? left : (budget - total);
    ssize_t w = out_write_some(fd, f.data() + q.off, want);
    if (w < 0) {                            // EIO 等：写通道已断，弃队列（窗口随后按退出收尾）
      fprintf(stderr, "[session] out flush win=%d: %s, drop queued %zu bytes\n",
              win + 1, strerror(errno), q.bytes);
      q.clear();
      break;
    }
    if (w == 0) break;                      // EAGAIN：内核输入缓冲满，下一轮再来
    total += (size_t)w;
    q.bytes -= (size_t)w;
    q.off += (size_t)w;
    if (q.off >= f.size()) { q.items.pop_front(); q.off = 0; }
  }
  if (q.empty()) q.overflow_logged = false;  // 队空：允许下次积压重新记日志
  return total;
}

// 终端向应用回复（term::set_writer 注入）：走当前窗口的应答队列，绝不阻塞
void write_reply(const char* s, size_t n)
{
  Win* w = nullptr;
  for (auto& c : g_wins)
    if (c.open && c.master == g_master) { w = &c; break; }
  if (w == nullptr) return;                 // 无会话窗口（启动器 / 已关闭）：应答无处可去
  out_write(*w, w->out_reply, "reply", s, n);
}


// 特殊键 → 终端序列（对齐 xterm-256color terminfo 与 foot keymap.h）
// 修饰键编码 m = 1 + shift + 2*alt + 4*ctrl：
//   单字母结尾键（箭头/Home/End）\e[1;m<final>；波浪键（Ins/Del/PgUp/PgDn/F5+）
//   \e[n;m~；F1-F4 \e[1;mP/Q/R/S。无修饰时用普通序列；DECCKM 应用光标键模式
//   （term::app_cursor_keys）下箭头/Home/End 用 SS3 形式（\eOA…）。
std::string key_seq(const std::string& t, int m, bool app)
{
  char b[24];
  auto fin = [&](char c, const char* plain, const char* ss3) {
    if (m > 1) { snprintf(b, sizeof b, "\033[1;%d%c", m, c); return std::string(b); }
    return std::string((app && ss3 != nullptr) ? ss3 : plain);
  };
  auto tild = [&](int n2) {
    if (m > 1) snprintf(b, sizeof b, "\033[%d;%d~", n2, m);
    else       snprintf(b, sizeof b, "\033[%d~", n2);
    return std::string(b);
  };
  if (t == "Up")                return fin('A', "\033[A", "\033OA");
  if (t == "Dn" || t == "Down") return fin('B', "\033[B", "\033OB");
  if (t == "Rt" || t == "Right") return fin('C', "\033[C", "\033OC");   // kcuf1=\EOC
  if (t == "Lt" || t == "Left") return fin('D', "\033[D", "\033OD");    // kcub1=\EOD
  if (t == "Home")              return fin('H', "\033[H", "\033OH");
  if (t == "End")               return fin('F', "\033[F", "\033OF");
  if (t == "Ins")  return tild(2);
  if (t == "Del")  return tild(3);
  if (t == "PgUp") return tild(5);
  if (t == "PgDn") return tild(6);
  if (t == "F1") return fin('P', "\033OP", nullptr);
  if (t == "F2") return fin('Q', "\033OQ", nullptr);
  if (t == "F3") return fin('R', "\033OR", nullptr);
  if (t == "F4") return fin('S', "\033OS", nullptr);
  if (t == "F5")  return tild(15);
  if (t == "F6")  return tild(17);
  if (t == "F7")  return tild(18);
  if (t == "F8")  return tild(19);
  if (t == "F9")  return tild(20);
  if (t == "F10") return tild(21);
  if (t == "F11") return tild(23);
  if (t == "F12") return tild(24);
  return std::string();
}

// ── kitty 键盘协议（KKP）键编码 ────────────────────────────────────
// 参照 foot input.c 与官方规格（sw.kovidgoyal.net/kitty/keyboard-protocol/）：
//   u 形态:     CSI key[:alt][;mods[:ev]][;text] u   （Esc/Enter/Tab/Bksp/可打印键）
//   波浪形态:   同上但以 ~ 结尾（Ins/Del/PgUp/PgDn/F5-F12）
//   final 形态: CSI 1;mods[:ev]<final> 或 CSI <final>（箭头/Home/End/F1-F4）
//   文本键（无 alt/ctrl 且未开 report-all）按旧路径直发文本；
//   事件类型 :2=重复 :3=松开 仅在 flag 2 下发送（按下省略 :1）。
// 规格三要点（字段顺序 key[:alt] ; mods[:ev] ; text）：
//   ① 备用键（flag 4）= 上档键码，规格要求"shift 在修饰中"才出现，且仅当
//      上档字符非 0 且与主键不同才有意义（base-layout 键码与主键相同恒省略）；
//   ② text-as-codepoints（flag 16）是第三字段：text 前必须有完整 mods 字段，
//      否则严格解析器把 text 读成 modifiers:event-type；
//   ③ u 形态的 mods 字段始终输出（无修饰 = 1；event 类型按 flag 2 追加 :ev）。
// 返回空 = 交回旧编码路径。
std::string kkp_seq(const kbd::KeyEvent& e, uint32_t f)
{
  struct K { uint32_t key; char fin; };
  static const struct { uint16_t code; K k; } kTab[] = {
    {1,   {27,  'u'}}, {28, {13, 'u'}}, {15, {9, 'u'}}, {14, {127, 'u'}},
    {103, {1,  'A'}}, {108, {1, 'B'}}, {106, {1, 'C'}}, {105, {1, 'D'}},
    {102, {1,  'H'}}, {107, {1, 'F'}},
    {110, {2,  '~'}}, {111, {3, '~'}}, {104, {5, '~'}}, {109, {6, '~'}},
    {59,  {1,  'P'}}, {60, {1, 'Q'}},  {61, {13, '~'}}, {62, {1, 'S'}},
    {63,  {15, '~'}}, {64, {17, '~'}}, {65, {18, '~'}}, {66, {19, '~'}},
    {67,  {20, '~'}}, {68, {21, '~'}}, {87, {23, '~'}}, {88, {24, '~'}},
  };
  if (e.ev == 3 && !(f & 2)) return std::string();   // 松开仅事件上报模式发送

  const bool has_ac  = (e.mods & 6) != 0;            // alt 或 ctrl（原始位）
  const bool printable = (e.lo != 0 || e.up != 0);
  K k{0, 'u'};
  bool func = false;
  for (const auto& r : kTab)
    if (r.code == e.code) { k = r.k; func = true; break; }

  // 纯文本键：保持旧路径直发文本（flag 8 除外）
  if (printable && !has_ac && !(f & 8)) {
    if (e.ev == 3) return std::string();
    char c = ((e.mods & 1) && e.up) ? e.up : e.lo;
    return std::string(1, c);
  }
  if (!func) k.key = (uint32_t)(unsigned char)(e.lo ? e.lo : e.up);

  // Esc/Enter/Tab/Bksp 有旧字节：仅 flag 8 或（flag 1 且 Esc/带 alt/ctrl）时转 u 形态
  const bool plain_old = (e.code == 1 || e.code == 28 || e.code == 15 || e.code == 14);
  bool u_form;
  if (func && k.fin == 'u' && plain_old)
    u_form = (f & 8) || ((f & 1) && (e.code == 1 || has_ac));
  else if (!func)
    u_form = true;                                    // 可打印键的转义形态即 u 形态
  else
    u_form = false;                                   // 箭头/F 键/~ 键：final 形态

  if (!u_form && func && k.fin == 'u') return std::string();  // 旧字节交回旧路径

  // 事件类型后缀（flag 2；按下省略 :1）
  char evs[4] = "";
  if ((f & 2) && e.ev == 2) strcpy(evs, ":2");
  else if ((f & 2) && e.ev == 3) strcpy(evs, ":3");

  const uint32_t mods = (uint32_t)e.mods + 1;        // xterm/kitty 修饰值 = 原始位+1
  // 文本字段（flag 16，按下/重复）：必须落在第三字段，前面要有完整 mods 字段占位
  const bool want_text = (f & 16) && printable && e.ev != 3;
  char buf[64], *p = buf;
  size_t left = sizeof buf;
  int n2;

  if (u_form || k.fin == '~') {
    n2 = snprintf(p, left, "\033[%u", k.key);  p += n2; left -= (size_t)n2;
    // ① 备用键（flag 4）= 上档键码。规格：shift 在修饰中才出现；仅当上档字符
    //    非 0 且与主键不同才有意义（base-layout 键码与主键相同恒省略）。
    if ((f & 4) && printable && (e.mods & 1)) {
      uint32_t alt = (uint32_t)(unsigned char)(e.up ? e.up : 0);
      if (alt != 0 && alt != k.key) {
        n2 = snprintf(p, left, ":%u", alt); p += n2; left -= (size_t)n2;
      }
    }
    // ②③ mods 字段：u 形态始终输出（无修饰 = 1）——text 是第三字段，mods 缺席
    //    时严格解析器会把 text 误读成 modifiers:event-type；~ 形态维持旧规则。
    if (u_form || mods > 1 || evs[0] != '\0' || want_text) {
      n2 = snprintf(p, left, ";%u%s", mods, evs); p += n2; left -= (size_t)n2;
    }
    if (want_text) {                                  // 附带文本（按下/重复）
      uint32_t tcp = (uint32_t)(unsigned char)(((e.mods & 1) && e.up) ? e.up : e.lo);
      n2 = snprintf(p, left, ";%u", tcp);
      p += n2; left -= (size_t)n2;
    }
    snprintf(p, left, "%c", k.fin);
    return std::string(buf);
  }

  // final 形态（箭头/Home/End/F1-F4）
  if (mods > 1 || evs[0] != '\0') {
    snprintf(buf, sizeof buf, "\033[1;%u%s%c", mods, evs, k.fin);
    return std::string(buf);
  }
  snprintf(buf, sizeof buf, "\033[%c", k.fin);
  return std::string(buf);
}

// Ctrl+字母/符号 → 控制字符（0x00-0x1f / DEL）
int ctrl_code(char c)
{
  if (c >= 'a' && c <= 'z') return c - 'a' + 1;
  if (c >= 'A' && c <= 'Z') return c - 'A' + 1;
  switch (c) {
    case '@': case ' ': return 0x00;
    case '[': return 0x1b;
    case '\\': return 0x1c;
    case ']': return 0x1d;
    case '^': return 0x1e;
    case '_': return 0x1f;
    case '?': return 0x7f;
    default: return -1;
  }
}

int spawn(const std::vector<std::string>& argv, int* master_out, bool* exec_failed)
{
  if (exec_failed != nullptr) *exec_failed = false;
  int master = -1, slave = -1;
  if (openpty(&master, &slave, nullptr, nullptr, nullptr) < 0) {
    fprintf(stderr, "[session] openpty: %s\n", strerror(errno));
    return -1;
  }

  // master 设 O_NONBLOCK：出站走"尽量写 + 有界队列"，绝不阻塞主循环。
  // 注意只改 master（ptmx）的文件描述状态；slave 的阻塞语义保持默认，
  // 子程序 read/write 行为不变（openpty 的 slave 是独立的打开实例）。
  {
    int fl = fcntl(master, F_GETFL, 0);
    if (fl < 0 || fcntl(master, F_SETFL, fl | O_NONBLOCK) < 0)
      fprintf(stderr, "[session] fcntl(master, O_NONBLOCK): %s\n", strerror(errno));
  }

  struct winsize ws;
  memset(&ws, 0, sizeof ws);
  ws.ws_row = term::kRows; ws.ws_col = term::kCols;
  ws.ws_xpixel = term::kCols * term::kCellW; ws.ws_ypixel = term::kRows * term::kCellH;
  ioctl(slave, TIOCSWINSZ, &ws);

  // exec 失败检测：CLOEXEC 管道——exec 成功自动关（读到 EOF），失败写 errno
  int errp[2] = {-1, -1};
  if (pipe(errp) == 0) fcntl(errp[1], F_SETFD, FD_CLOEXEC);

  pid_t pid = fork();
  if (pid < 0) {
    fprintf(stderr, "[session] fork: %s\n", strerror(errno));
    close(master); close(slave);
    return -1;
  }
  if (pid == 0) {
    // exec 失败报告 fd：dup 到 >=3 的小号，供后续 close-others 时保留
    int efd = -1;
    if (errp[1] >= 0) efd = fcntl(errp[1], F_DUPFD, 3);
    setsid();                                // 新会话 → 让 slave 成为控制终端
    ioctl(slave, TIOCSCTTY, 0);
    dup2(slave, STDIN_FILENO);
    dup2(slave, STDOUT_FILENO);
    dup2(slave, STDERR_FILENO);
    if (slave > STDERR_FILENO) close(slave);
    close(master);
    // 关闭全部继承 fd（其他窗口 PTY master / evdev grab / 显示锁 / fb0 / tty1 / i2c）
    // 否则 cardterm 死后这些独占资源会被 shell 继续持有：键盘对系统不可用、显示锁被占。
    {
      long open_max = sysconf(_SC_OPEN_MAX);
      if (open_max < 0 || open_max > 4096) open_max = 4096;
      for (int fd = 3; fd < (int)open_max; fd++)
        if (fd != efd) close(fd);
    }

    // 终端类型：支持 256 色与真彩（本终端已实现）
    setenv("TERM", "xterm-256color", 1);
    setenv("COLORTERM", "truecolor", 1);
    setenv("TERM_PROGRAM", "cardterm", 1);
    // 注意：这里【不要】setenv COLUMNS/LINES。真实终端（xterm 等）都不导出它们；
    // 一旦导出，ncurses 的尺寸判定会被 env 钉死在启动值（tinfo/lib_setup.c
    // _nc_get_screensize：ioctl 之后 env 覆盖 *linep/*colp），导致 nano/htop 等
    // ncurses 系 TUI 在 Fn+U/I 缩放后不重新布局（实测 A/B 已证）。程序要知道
    // 尺寸请走 ioctl(TIOCGWINSZ) / terminfo，与主流终端行为一致。

    // Unicode 宣称：默认 UTF-8 locale；关闭时 LANG=C（TUI 程序改用 ACS 框线，
    // 不再发宽字符/生僻码点，规避字体覆盖与全半角差异）
    if (g_unicode) {
      const char* lang = getenv("LANG");
      const char* lc   = getenv("LC_ALL");
      const bool utf8 = (lang && (strstr(lang, "UTF-8") || strstr(lang, "utf8"))) ||
                        (lc && (strstr(lc, "UTF-8") || strstr(lc, "utf8")));
      if (!utf8) {
        setenv("LANG", "C.UTF-8", 1);
        setenv("LC_ALL", "C.UTF-8", 1);
      }
    } else {
      setenv("LANG", "C", 1);
      setenv("LC_ALL", "C", 1);
      setenv("LC_CTYPE", "C", 1);
    }

    std::vector<char*> av;
    av.reserve(argv.size() + 1);
    for (const auto& a : argv) av.push_back(const_cast<char*>(a.c_str()));
    av.push_back(nullptr);
    if (errp[0] >= 0) close(errp[0]);
    execvp(av[0], av.data());

    if (efd >= 0) {                                  // exec 失败：先告知父进程
      int ecode = errno;
      ssize_t wn = write(efd, &ecode, sizeof ecode);
      (void)wn;
      close(efd);
    }
    std::string e = "cardterm: exec failed: ";
    e += argv[0];
    e += ": ";
    e += strerror(errno);
    e += "\r\n";
    ssize_t r = ::write(STDERR_FILENO, e.data(), e.size());
    (void)r;
    _exit(127);
  }

  close(slave);
  if (errp[1] >= 0) close(errp[1]);
  if (errp[0] >= 0) {                                // 等 exec 结果，最多 500ms（超时视为已 exec）
    int ecode = 0;
    struct pollfd pf = { errp[0], POLLIN, 0 };
    ssize_t rn = -1;
    for (;;) {
      int pr = poll(&pf, 1, 500);
      if (pr < 0) { if (errno == EINTR) continue; break; }
      if (pr == 0) break;                            // 500ms 超时：视为已 exec（交互式程序不会关 errp）
      rn = read(errp[0], &ecode, sizeof ecode);
      if (rn <= 0) break;                            // EOF(0)=exec 成功；错误也退出（原代码对 0 不 break → 忙等死循环）
      break;                                         // 读到错误码
    }
    if (rn == (ssize_t)sizeof ecode && exec_failed != nullptr) *exec_failed = true;
    close(errp[0]);
  }
  *master_out = master;
  { std::string a; for (const auto& s : argv) { a += s; a += '|'; }
    fprintf(stderr, "[session] child pid=%d: [%s]\n", (int)pid, a.c_str()); }
  return (int)pid;
}

}  // namespace

// ══════════════════════════════════════════════════════════════════
// 窗口管理器（0.3.0 多窗口）：12 槽 + 极简启动器 + 退出规则
// ══════════════════════════════════════════════════════════════════
namespace {

int g_cur = 0;                      // 活跃窗口下标（0 = 窗口 1）
term::Options g_opt;
uint64_t g_last_data = 0;
bool g_out_pending = false;         // 有出站队列未排空（主循环据此缩短 poll 超时）
bool g_out_progress = false;        // 上一轮出站排空确有字节写出（无进展 = 内核缓冲满）

// ── 配置快照（ui.cpp 已 load 过一次；这里取 cfg::current() 的副本）────
//   用于 [text] splash 自定义文本与 [general] launch 的启动程序名。
cfg::Config g_conf;
std::string g_prompt_cmd = "bash";               // 启动器提示行：launch 第一个词（默认 bash）
std::vector<std::string> g_default_cmd{"bash"};  // 启动器空回车运行的命令（launch 分词）

// 按空白切分命令行（与 ui.cpp 对 [general] launch 的解析一致）
std::vector<std::string> split_words(const std::string& s)
{
  std::vector<std::string> v;
  std::string cur;
  for (char c : s) {
    if (c == ' ' || c == '\t') { if (!cur.empty()) { v.push_back(cur); cur.clear(); } }
    else cur += c;
  }
  if (!cur.empty()) v.push_back(cur);
  return v;
}

// 自定义开屏/帮助文本的占位符（0.7.6 修订5 定稿语法，在颜色标签解析之前替换）：
//   <pty_id>              → "PTY 01"   （本窗口编号，1 基两位，整串含 "PTY " 前缀）
//   <version>             → "0.7.6"    （CARDTERM_VERSION，不含 'v'）
//   <term_size_font_size> → "53x17 cells, 6x10px"（网格与单元尺寸整串）
// 未知的 <...> 一律原样保留（与标签解析"非法标签原样显示"一致，不会被吞）。
std::string expand_placeholders(const std::string& s)
{
  std::string out;
  out.reserve(s.size() + 32);
  for (size_t i = 0; i < s.size(); ) {
    if (s[i] != '<') { out += s[i++]; continue; }
    size_t j = s.find('>', i + 1);
    if (j == std::string::npos) { out += s[i++]; continue; }
    const std::string name = s.substr(i + 1, j - i - 1);
    char b[40];
    const char* rep = nullptr;
    if (name == "pty_id") {
      snprintf(b, sizeof b, "PTY %02d", g_cur + 1); rep = b;
    } else if (name == "term_size_font_size") {
      snprintf(b, sizeof b, "%dx%d cells, %dx%dpx",
               term::kCols, term::kRows, term::kCellW, term::kCellH); rep = b;
    } else if (name == "version") {
      rep = CARDTERM_VERSION;
    }
    if (rep == nullptr) { out += s[i++]; continue; }   // 未知 <...>：原样输出（交给标签解析）
    out += rep;
    i = j + 1;
  }
  return out;
}

// ── 富文本输出：只解析颜色标签（不是 XML 解析器）──────────────────
// 语法（[text] splash / [text] help）：
//   <br>                          → 换行（\r\n）
//   <fgcolor=RRGGBB> … </fgcolor> → 该段文字用该前景色（真彩 SGR 38;2;r;g;b）
//   <bgcolor=RRGGBB> … </bgcolor> → 该段文字用该背景色（真彩 SGR 48;2;r;g;b）
//   · 未闭合的颜色标签一直生效到文本末尾；颜色可嵌套（关闭时恢复外层颜色）；
//   · 不合法的标签（<fgcolor=zzz>、单独 </fgcolor>、半截 <br 等）原样输出字符，
//     不吞字符也不报错——想显示 '<'/'>' 写成不成对/不合法的形式即可；
//   · 标签名与十六进制大小写不敏感；结尾统一 SGR 复位，再按需补换行。
// 文本一次拼好交给 term::write（它解析 ANSI 转义序列），避免逐字符上屏。
void emit_rich_text(const std::string& s, bool add_newline)
{
  const std::string text = expand_placeholders(s);   // 先替换占位符，再扫颜色标签
  constexpr uint32_t kNone = 0xFFFFFFFFu;    // "未设置"哨兵（真彩只有 24 位）
  uint32_t cur_fg = kNone, cur_bg = kNone;   // 当前生效颜色
  std::vector<uint32_t> fg_stack, bg_stack;  // 嵌套颜色栈
  std::string out;

  auto sgr = [&out](bool bg, uint32_t c) {
    char b[32];
    if (bg) snprintf(b, sizeof b, "\033[48;2;%u;%u;%um",
                     (unsigned)((c >> 16) & 0xFFu), (unsigned)((c >> 8) & 0xFFu), (unsigned)(c & 0xFFu));
    else    snprintf(b, sizeof b, "\033[38;2;%u;%u;%um",
                     (unsigned)((c >> 16) & 0xFFu), (unsigned)((c >> 8) & 0xFFu), (unsigned)(c & 0xFFu));
    out += b;
  };
  auto set_fg = [&](uint32_t c) {
    if (c == cur_fg) return;
    cur_fg = c;
    if (c == kNone) out += "\033[39m"; else sgr(false, c);
  };
  auto set_bg = [&](uint32_t c) {
    if (c == cur_bg) return;
    cur_bg = c;
    if (c == kNone) out += "\033[49m"; else sgr(true, c);
  };
  // s[i...] 是否以 tag（全小写）开头，大小写不敏感
  auto match = [&text](size_t i, const char* tag) {
    size_t n = strlen(tag);
    if (i + n > text.size()) return false;
    for (size_t k = 0; k < n; k++)
      if (tolower((unsigned char)text[i + k]) != tag[k]) return false;
    return true;
  };
  // 解析 <fgcolor=RRGGBB> / <bgcolor=RRGGBB>：合法返回消耗长度 16，否则 0
  auto open_tag = [&text, &match](size_t i, bool bg, uint32_t& color) -> size_t {
    const char* pfx = bg ? "<bgcolor=" : "<fgcolor=";
    if (!match(i, pfx)) return 0;
    const size_t h = i + 9;                  // 9 = "<fgcolor=" / "<bgcolor=" 长度
    if (h + 7 > text.size() || text[h + 6] != '>') return 0;
    uint32_t c = 0;
    for (size_t k = 0; k < 6; k++) {
      char ch = text[h + k];
      int d;
      if (ch >= '0' && ch <= '9') d = ch - '0';
      else if (ch >= 'a' && ch <= 'f') d = ch - 'a' + 10;
      else if (ch >= 'A' && ch <= 'F') d = ch - 'A' + 10;
      else return 0;                         // 非法十六进制：整个标签按字面显示
      c = (c << 4) | (uint32_t)d;
    }
    color = c;
    return 16;
  };

  for (size_t i = 0; i < text.size(); ) {
    if (text[i] != '<') {                       // 普通文本：整段拷贝到下一个 '<'
      size_t j = text.find('<', i);
      if (j == std::string::npos) j = text.size();
      out.append(text, i, j - i);
      i = j;
      continue;
    }
    if (match(i, "<br>")) { out += "\r\n"; i += 4; continue; }
    uint32_t c = 0;
    if (size_t n = open_tag(i, false, c); n != 0) {
      fg_stack.push_back(cur_fg); set_fg(c); i += n; continue;
    }
    if (size_t n = open_tag(i, true, c); n != 0) {
      bg_stack.push_back(cur_bg); set_bg(c); i += n; continue;
    }
    if (match(i, "</fgcolor>")) {
      if (fg_stack.empty()) { out += '<'; i++; continue; }   // 单独闭合：按字面显示
      set_fg(fg_stack.back()); fg_stack.pop_back(); i += 10; continue;
    }
    if (match(i, "</bgcolor>")) {
      if (bg_stack.empty()) { out += '<'; i++; continue; }
      set_bg(bg_stack.back()); bg_stack.pop_back(); i += 10; continue;
    }
    out += '<';                              // 其它（半截/非法标签）：'<' 原样输出
    i++;
  }
  if (cur_fg != kNone || cur_bg != kNone) out += "\033[0m";
  if (add_newline) out += "\r\n";
  if (!out.empty()) term::write(out.c_str(), out.size());
}

term::Options win_opt(const Win& w)  // 以窗口字号覆盖全局选项
{
  term::Options o = g_opt;
  o.font_size = (w.font_size > 0) ? w.font_size : g_opt.font_size;
  return o;
}

void use_win(int n)                 // 设为解析/绘制目标
{
  term::activate(g_wins[n].ts);
  term::set_writer(write_reply);   // 回复通道随窗口走（DSR/DA/KKP 查询）
  g_master = g_wins[n].master;
}

// ── 开屏（splash）文本 + 极简启动器 ──────────────────────────────
// 内置开屏文本 = 原有 cardterm 横幅（版本/网格/调色板示例/操作提示）。

// 开屏入口：配置了 [text] splash 就用自定义富文本，否则用内置横幅。
// 调用时机：首次运行（配置文件刚创建）在命令输出前 / 启动器窗口显示时。
void splash_show(int win)
{
  (void)win;
  // [text] splash 缺省（空/缺键）= 不显示新窗口/新终端界面的提示词；
  // 全新安装生成的默认配置会把这段提示词写成实际值（见 config.cpp 模板），
  // 用户可随意改写或清空（清空后即回到"无提示词"）。
  if (!g_conf.splash.empty()) emit_rich_text(g_conf.splash, true);
}

// 启动器底部提示行：显示配置的启动程序名（[general] launch 第一个词，默认 bash），
// 不再显示 '>'。空回车即运行该程序（见 launcher_enter）。
void launcher_prompt()
{
  // 提示符固定为 regular7 的 '>'（与旧版一致）
  static const char kPrompt[] = "\033[37m>\033[0m ";
  term::write(kPrompt, sizeof kPrompt - 1);
}

void launcher_banner(int win)        // 启动器界面 = 开屏文本 + 程序名提示行
{
  splash_show(win);
  launcher_prompt();
}

void launcher_help()                // help：清屏 + 帮助 + 回到启动器
{
  // 帮助文本按 6×10（53×17）排版，8×14（40×12）下会折行看不清——
  // 进入帮助页时若当前窗口是 8×14，自动降档到 6×10（用户随后可 FN+U/I 改回）。
  {
    Win& w = g_wins[g_cur];
    if (w.font_size < 0) w.font_size = g_opt.font_size;
    if (w.font_size == 8) {
      w.font_size = 6;
      term::fill_bg();
      if (w.ts != nullptr) { term::state_free(w.ts); w.ts = nullptr; }
      w.ts = term::state_new(win_opt(w));
      if (w.ts == nullptr) {                 // OOM 等极端情况：回滚字号并放弃本次帮助显示
        fprintf(stderr, "[session] state_new failed (help font)\n");
        w.font_size = 8;
        return;
      }
      w.launcher = true; w.line.clear();
      use_win(g_cur);
    }
  }
  term::write("\033[2J\033[H");
  // 帮助页为内置固定内容（不再支持自定义；开屏页 [text] splash 仍可自定义）。
  // 第一行/末尾 '>' 蓝色，快捷键列黄色，说明列默认灰
  const char* kBlue   = "\033[38;2;80;200;255m";
  const char* kYellow = "\033[38;2;231;199;0m";
  const char* kGray   = "\033[38;2;160;160;160m";
  const char* kWhite  = "\033[38;2;255;255;255m";
  const char* kReset  = "\033[0m";
  std::string h;
  h += kBlue; h += "  cardterm v" CARDTERM_VERSION " help"; h += kReset; h += "\r\n";
  // 左列 19 字符定宽（与帮助页排版一致），右列为说明文字
  auto line = [&](const char* key, const char* desc) {
    h += kYellow; h += key;
    h += kGray;   h += desc;
    h += kReset;  h += "\r\n";
  };
  line("<command>          ", "Run interactive shell or app in");
  line("                   ", "this window (bash/ssh/codex...)");
  line("Ctrl+Alt+F1~F12    ", "Switch terminal window");
  line("Fn+Q               ", "Kill and close current window");
  line("Fn+W / Fn+E        ", "Previous / Next terminal window");
  line("Fn+U / Fn+I        ", "Zoom out / Zoom in");
  line("Fn+A               ", "Detach terminal and move it to");
  line("                   ", "background. Reopen to attach.");
  line("Hold ESC for 2s    ", "Kill, close all window and exit");
  // 配置文件提示（文件名亮绿色，其余默认颜色）
  h += kReset;
  h += "Open ";
  h += "\033[38;2;80;255;80m"; h += "~/.config/cardterm/config.ini";   // 亮绿色文件名
  h += kReset; h += " file to edit\r\n";
  h += "settings and configurations.\r\n";
  // 署名两行：人名白色 (FFFFFF)，URL 与标题同 kBlue (#50C8FF)
  h += "Created by ";
  h += kWhite; h += "FriendshipEnder (GitHub: fsender)";
  h += kReset; h += ". Visit\r\n";
  h += kBlue; h += "https://github.com/fsender/cardterm";
  h += kReset; h += " for more info.\r\n";
  h += "\r\n";
  h += kBlue; h += "> "; h += kReset;
  term::write(h.c_str(), h.size());
}

void launcher_show(int n, bool show_banner = true)   // 初始界面（重建状态；切走即弃）
{
  Win& w = g_wins[n];
  if (w.font_size < 0) w.font_size = g_opt.font_size;
  if (w.ts != nullptr) term::state_free(w.ts);
  w.ts = term::state_new(win_opt(w));
  if (w.ts == nullptr) { fprintf(stderr, "[session] state_new failed (launcher)\n"); return; }
  w.open = false; w.eof = false; w.master = -1;
  w.out_key.clear(); w.out_reply.clear();   // 启动器无 PTY：出站队列必须为空
  w.launcher = true; w.line.clear();
  w.fg_pgrp = 0;
  use_win(n);
  term::reset_cursor_style();         // 回到启动器：光标呈现恢复默认
  term::fill_bg();                    // 切窗：先填背景/背景图清屏，再显示新窗口内容（T=新状态后才有画布）
  if (show_banner) launcher_banner(n);   // Fn+H 帮助路径：不显示 banner，直接写帮助
  term::present();
}

// Fn+H / Super+H：切到下一个"空终端窗口"（无会话/程序、无后台会话的槽位）显示帮助
// （等同在初始界面输入 help，不显示 banner）。当前窗口是唯一空窗时就地显示；
// 所有窗口都有会话/程序时什么都不做。
void switch_to(int n, bool toast, bool banner);   // 前置声明（定义见下）
void show_help_in_empty_window()
{
  int target = -1;
  for (int k = 1; k < g_win_count; k++) {
    int i = (g_cur + k) % g_win_count;
    // 空槽 = 无会话/程序、无后台会话、无死亡记录（死亡记录槽位不算空，会走 attach_dead）
    if (!g_wins[i].open && !bgtask::session_exists(i + 1) && !bgtask::has_dead(i + 1)) {
      target = i;
      break;
    }
  }
  if (target < 0) {
    if (g_wins[g_cur].open) return;     // 全部窗口都有会话/程序：无动作
    target = g_cur;                     // 当前即唯一空窗：就地显示帮助
  }
  if (target == g_cur) {
    if (g_wins[g_cur].ts == nullptr) launcher_show(g_cur, false);
    launcher_help();
    term::present();
    return;
  }
  switch_to(target, false, false);      // 不弹 toast、不显示 banner
  launcher_help();
  term::present();
}

// 启动器 Enter：help 特例 / 空行 = 运行默认启动程序（launch 第一个词）/
// 其余分词后 spawn 进本窗口
void close_current();   // 前置声明（launcher_enter 的失败分支要用）

// 判断启动器输入的第一个词是否是可执行文件：含 '/' 按路径判断，否则按 PATH 搜索。
// 目的：在 spawn 之前拦下"程序不可执行"，给出 toast 而不是让子进程以 127 退出。
static const char* resolve_executable(const std::string& first)
{
  static char buf[4096];
  if (first.empty()) return nullptr;
  if (first.find('/') != std::string::npos) {
    struct stat st;
    if (stat(first.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || access(first.c_str(), X_OK) != 0)
      return nullptr;
    snprintf(buf, sizeof buf, "%s", first.c_str());
    return buf;
  }
  const char* pe = getenv("PATH");
  std::string path = pe ? pe : "/usr/local/bin:/usr/bin:/bin";
  size_t i = 0;
  while (i <= path.size()) {
    size_t j = path.find(':', i);
    if (j == std::string::npos) j = path.size();
    std::string dir = path.substr(i, j - i);
    if (dir.empty()) dir = ".";
    std::string cand = dir + "/" + first;
    struct stat st;
    if (stat(cand.c_str(), &st) == 0 && S_ISREG(st.st_mode) && access(cand.c_str(), X_OK) == 0) {
      snprintf(buf, sizeof buf, "%s", cand.c_str());
      return buf;
    }
    i = j + 1;
  }
  return nullptr;
}

void launcher_enter(Win& w)
{
  std::string line = w.line;
  w.line.clear();
  term::write("\r\n");
  std::vector<std::string> av = split_words(line);
  if (av.size() == 1 && av[0] == "help") { launcher_help(); return; }
  if (av.empty()) {                     // 空回车：不执行任何程序，只重新给提示符
    launcher_prompt();
    return;
  }
  if (resolve_executable(av[0]) == nullptr) {   // 程序不可执行：关窗后提示（带切换目标）
    const int bad = g_cur + 1;
    close_current();                             // 关窗（若是最后一个 -> 置退出标志）
    if (!g_quit_after_close) {
      char tb[64];
      snprintf(tb, sizeof tb, "Bad command at PTY %02d. Now PTY %02d", bad, g_cur + 1);
      term::toast(tb);
    }
    return;
  }

  int master = -1;
  bool ef = false;
  pid_t pid = spawn(av, &master, &ef);
  if (pid < 0) {
    const int bad = g_cur + 1;
    close_current();
    if (!g_quit_after_close) {
      char tb[64];
      snprintf(tb, sizeof tb, "Bad command at PTY %02d. Now PTY %02d", bad, g_cur + 1);
      term::toast(tb);
    }
    return;
  }
  w.open = true; w.launcher = false;
  w.master = master; w.child = pid; w.eof = false; w.status = 0;
  w.exec_failed = ef;
  g_master = master;
  if (ef) {                             // exec 失败（非法命令）：窗口随后按退出收尾
    const int bad = g_cur + 1;
    close_current();
    if (!g_quit_after_close) {
      char tb[64];
      snprintf(tb, sizeof tb, "Bad command at PTY %02d. Now PTY %02d", bad, g_cur + 1);
      term::toast(tb);
    }
    return;
  }
}

void launcher_key(Win& w, const std::string& t, const kbd::KeyEvent& kev)
{
  if (kev.ev == 3) return;                       // 松开不回显
  if (t == "Enter") { launcher_enter(w); return; }
  if (t == "Bksp") {
    if (!w.line.empty()) { w.line.pop_back(); term::backspace(); }
    return;
  }
  if (t.size() == 1 && (kev.mods & 6) == 0 && (unsigned char)t[0] >= 0x20) {
    w.line += t[0];
    term::putc(t[0]);
  }
}

// ── 会话按键转发（0.2.3 的 KKP/legacy 路由，目标为窗口 PTY）───────
void send_key(Win& w, const std::string& t, const kbd::KeyEvent& kev)
{
  const bool sh = (kev.mods & 1) != 0;
  const bool al = (kev.mods & 2) != 0;
  const bool ct = (kev.mods & 4) != 0;
  const int  m  = kev.mods + 1;
  char bb[24];

  // ── 本终端 UI 组合键（优先于转发；仅按下/重复触发）──────────────
  if (kev.ev != 3) {
    if (t == "PgUp" && sh && !ct && !al) { term::scroll_view(term::kRows - 1); return; }
    if (t == "PgDn" && sh && !ct && !al) { term::scroll_view(-(term::kRows - 1)); return; }
    if (t == "Up" && ct && sh && !al)   { term::scroll_view(1); return; }
    if (t == "Dn" && ct && sh && !al)   { term::scroll_view(-1); return; }
    term::cursor_reblink();           // 非翻页键：光标从亮块相重新开始闪烁
  }

  // ── kitty 键盘协议启用：CSI u 编码（空=交回旧路径）──────────────
  bool handled = false;
  const uint32_t kf = term::kkp_flags();
  if (kf != 0) {
    std::string s = kkp_seq(kev, kf);
    if (!s.empty()) { feed_key(w, s.data(), s.size()); handled = true; }
  }

  // ── 旧路径（xterm 传统编码；松开事件不发）───────────────────────
  if (!handled && kev.ev != 3) {
    if (t == "Esc")   feed_key(w, "\x1b", 1);
    else if (t == "Enter") {
      if (m == 3) feed_key(w, "\033\r", 2);                          // Alt+Enter
      else if (m == 1) feed_key(w, "\r", 1);
      else { int n2 = snprintf(bb, sizeof bb, "\033[27;%d;13~", m); feed_key(w, bb, (size_t)n2); }
    } else if (t == "Bksp") {
      if (al) { char s2[2] = { '\x1b', ct ? '\x08' : '\x7f' }; feed_key(w, s2, 2); }
      else feed_key(w, ct ? "\x08" : "\x7f", 1);
    } else if (t == "Tab") {
      if (m == 2)      feed_key(w, "\033[Z", 3);                     // Shift+Tab = BackTab
      else if (m == 3) feed_key(w, "\033\t", 2);                     // Alt+Tab
      else if (m == 1) feed_key(w, "\t", 1);
      else { int n2 = snprintf(bb, sizeof bb, "\033[27;%d;9~", m); feed_key(w, bb, (size_t)n2); }
    }
    else if (std::string seq = key_seq(t, m, term::app_cursor_keys()); !seq.empty())
      feed_key(w, seq.data(), seq.size());
    else if (t.size() == 1) {
      char c = t[0];
      if (ct) {
        int code = ctrl_code(c);
        if (code >= 0) { char b = (char)code; feed_key(w, &b, 1); }
      } else if (al) {
        char seq[2] = { '\x1b', c };
        feed_key(w, seq, 2);
      } else {
        feed_key(w, &c, 1);
      }
    }
  }
}

// ── 输出泵：所有开窗 PTY → 各自状态（后台窗口持续解析）───────────
bool pump_all()
{
  bool got = false;
  term::set_quiet(true);            // 后台窗口解析不自动上屏
  char buf[4096];
  // 预算：单次主循环最多解析 ~128KB / 6ms，保证输入与 tick 总能被及时处理
  // （否则 `yes` 之类生产者的速度足以让内层循环不返回，键盘事件永远送不进去）
  const uint64_t t0 = now_ms();
  size_t budget = 128 * 1024;
  for (int i = 0; i < g_win_count && budget > 0; i++) {
    Win& w = g_wins[i];
    if (!w.open || w.eof) continue;
    for (;;) {
      struct pollfd pf = { w.master, POLLIN, 0 };
      if (poll(&pf, 1, 0) <= 0) break;
      ssize_t n = read(w.master, buf, sizeof buf);
      if (n > 0) {
        use_win(i);
        term::write(buf, (size_t)n);
        got = true; g_last_data = now_ms();
        if ((size_t)n >= budget) { budget = 0; break; }
        budget -= (size_t)n;
        if (now_ms() - t0 >= 6) { budget = 0; break; }
        continue;
      }
      if (n == 0) {                              // slave 全关：出站队列不再有意义，清空释放
        w.eof = true; w.out_key.clear(); w.out_reply.clear(); break;
      }
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      w.eof = true; w.out_key.clear(); w.out_reply.clear(); break;
    }
  }
  term::set_quiet(false);
  use_win(g_cur);
  return got;
}

// ── 出站排空：每轮 ≤ kOutRoundBytes 字节且 ≤ kOutRoundMs ──────────
// 先按键（用户输入要跟手）后应答（OSC52/DA 大回包可缓）。有界预算保证
// 任何窗口的积压都不会把本轮主循环拖长（输入/渲染/退出巡检始终及时）。
void pump_out()
{
  const uint64_t t0 = now_ms();
  const uint64_t deadline = t0 + kOutRoundMs;
  size_t budget = kOutRoundBytes;
  bool progress = false;
  bool pending = false;
  for (int pass = 0; pass < 2 && budget > 0; pass++) {
    for (int i = 0; i < g_win_count && budget > 0; i++) {
      Win& w = g_wins[i];
      if (!w.open || w.master < 0) continue;
      OutQueue& q = (pass == 0) ? w.out_key : w.out_reply;
      if (q.empty()) continue;
      size_t wro = out_flush(w.master, i, q, budget, deadline);
      budget -= (wro < budget) ? wro : budget;
      if (wro > 0) progress = true;
      if (!q.empty()) pending = true;
      if (now_ms() > deadline) { budget = 0; break; }
    }
  }
  // 有积压但本轮一个字节都没写出 = 内核输入缓冲满（子进程不读）：
  // 主循环据 g_out_progress 放慢 poll，避免空转烧 CPU。
  if (budget == 0) pending = true;   // 预算用尽（字节或时间）：可能还有没轮到的窗口
  g_out_pending = pending;
  g_out_progress = progress;
}

void resize_pty(Win& w)             // 字号变化后同步 PTY 尺寸（内核发 SIGWINCH）
{
  // TIOCSWINSZ 是控制 ioctl，不占 PTY 出站字节缓冲，无需走出站队列
  if (!w.open || w.master < 0) return;
  struct winsize ws;
  memset(&ws, 0, sizeof ws);
  ws.ws_row = term::kRows; ws.ws_col = term::kCols;
  ws.ws_xpixel = term::kCols * term::kCellW;
  ws.ws_ypixel = term::kRows * term::kCellH;
  ioctl(w.master, TIOCSWINSZ, &ws);
}

void change_font(int dir)           // FN+U(-1) / FN+I(+1)：本窗口字号循环
{
  static const int kSizes[3] = {4, 6, 8};
  Win& w = g_wins[g_cur];
  if (w.font_size < 0) w.font_size = g_opt.font_size;
  int idx = 1;
  for (int i = 0; i < 3; i++) if (kSizes[i] == w.font_size) idx = i;
  idx += (dir < 0 ? -1 : 1);
  if (idx < 0) idx = 0;
  if (idx > 2) idx = 2;
  if (kSizes[idx] == w.font_size) return;
  const int old_fs = w.font_size;               // A3：下方失败分支回滚用
  w.font_size = kSizes[idx];

  if (w.open) {
    term::fill_bg();                                     // ① 先整屏填背景（防 rewrap 后右侧/下方残影）
    if (w.ts != nullptr) w.ts = term::state_resize(w.ts, w.font_size);  // ② rewrap（较慢：颜色压缩/解压）
    use_win(g_cur);                                     // ③ 先激活实际网格（OOM 回滚时是旧网格）
    // A3：OOM 回滚时 state_resize 保持旧网格，w.font_size 会与实际脱钩 → 按实际网格反推
    w.font_size = (term::kCols >= 80) ? 4 : (term::kCols <= 40 ? 8 : 6);
    resize_pty(w);                                      // ④ 再按实际全局网格同步 PTY 尺寸（发 SIGWINCH）
    term::invalidate();
    term::present();                                     // ③ 呈现缩放后界面
    char tb[80];
    snprintf(tb, sizeof tb, "PTY %02d Terminal size set to %dx%d.",
             g_cur + 1, term::kCols, term::kRows);
    term::toast(tb);                                     // 事件通知（toast 叠加层）
  } else {
    // 启动器窗口：先填背景，再按新字号重建状态 + 重绘横幅
    term::fill_bg();
    if (w.ts != nullptr) { term::state_free(w.ts); w.ts = nullptr; }
    w.ts = term::state_new(win_opt(w));
    if (w.ts == nullptr) { fprintf(stderr, "[session] state_new failed (font)\n"); w.font_size = old_fs; return; }
    w.launcher = true; w.line.clear();
    use_win(g_cur);
    launcher_banner(g_cur);
    term::present();
    char tb[80];
    snprintf(tb, sizeof tb, "PTY %02d Terminal size set to %dx%d.",
             g_cur + 1, term::kCols, term::kRows);
    term::toast(tb);
  }
}

void close_current()                // FN+Q：关闭当前终端（类似关闭标签页）
{
  Win& w = g_wins[g_cur];
  const int  closed_win = g_cur;    // toast 用：被关闭的槽位（1 基）
  const bool was_open = w.open;
  if (w.open) {
    if (w.child > 0) {
      kill(-w.child, SIGHUP);
      for (int k = 0; k < 20; k++) {
        if (waitpid(w.child, &w.status, WNOHANG) != 0) break;
        usleep(50000);
      }
      if (w.child > 0 && waitpid(w.child, &w.status, WNOHANG) == 0) {
        kill(-w.child, SIGKILL);
        waitpid(w.child, &w.status, 0);
      } else if (w.adopted && w.child > 0 && kill(-w.child, 0) == 0) {
        kill(-w.child, SIGKILL);        // 附着会话：非亲生 waitpid 不可用，按 pgid 补刀
      }
    }
    if (w.master >= 0) close(w.master);
    w.out_key.clear(); w.out_reply.clear();   // 出站队列随窗口关闭释放
    w.master = -1; w.open = false; w.eof = false; w.child = 0;
  }
  if (w.ts != nullptr) { term::state_free(w.ts); w.ts = nullptr; }
  w.launcher = false; w.line.clear();
  g_master = -1;

  int nxt = -1;                     // 切到下一个仍开着的窗口
  for (int k = 1; k < g_win_count; k++) {
    int i = (g_cur + k) % g_win_count;
    if (g_wins[i].open) { nxt = i; break; }
  }
  if (nxt >= 0) {
    g_cur = nxt;
    g_wins[nxt].launcher = false;
    term::fill_bg();                 // 切窗：先填背景/背景图清屏，再显示新窗口内容
    use_win(nxt);
    term::view_follow();
    term::invalidate();
    term::present();
    if (was_open) {                  // 事件通知：仅真正关掉了会话窗口时弹
      char tb[48];
      snprintf(tb, sizeof tb, "PTY %02d closed. Now PTY %02d", closed_win + 1, g_cur + 1);
      term::toast(tb);
    }
  } else {
    // 没有其它打开的窗口：按"关闭最后一个标签页"语义退出程序（不回启动页）
    if (was_open) {                  // req 3：全屏倒计时提示（复用 req 2 的提示框）
      char tb[48];
      snprintf(tb, sizeof tb, "PTY %02d closed.", closed_win + 1);
      term::exit_prompt(tb);         // "No terminal running." + 3s→2s→1s
    }
    g_quit_after_close = true;
  }
}

int first_open()
{
  for (int i = 0; i < g_win_count; i++) if (g_wins[i].open) return i;
  return -1;
}

// Fn+A / Super+A：把当前窗口的会话剥离到后台（方案 P：master fd 移交守护进程，
// 桥接进 tmux 会话 cardterm_PTY_NN），随后关闭窗口并释放其全部内存（req 4）。
// 开屏/帮助界面（无 PTY）不响应。剥离后会话持续在后台运行（req 5）；
// 是最后一个窗口时走全屏倒计时退出（req 2），会话不被杀（req 7）。
void detach_current()
{
  Win& w = g_wins[g_cur];
  if (!w.open || w.master < 0) return;      // 仅活跃终端窗口
  const int slot1 = g_cur + 1;
  struct winsize ws;
  memset(&ws, 0, sizeof ws);
  if (ioctl(w.master, TIOCGWINSZ, &ws) < 0 || ws.ws_col <= 0 || ws.ws_row <= 0) {
    ws.ws_col = 53; ws.ws_row = 17;
  }
  int hist_lines = (g_conf.bg_buffer_kb * 1024) / ws.ws_col + 64;   // ≈buffer_kb 的行数
  if (hist_lines < 64) hist_lines = 64;
  if (!bgtask::park(slot1, w.master, w.child, w.child,
                    ws.ws_col, ws.ws_row, hist_lines)) {
    term::toast("Detach failed. Session kept in this window");
    return;
  }
  // 交接成功：本窗口全部内存释放（term 状态/回滚/出站队列；req 4）
  close(w.master); w.master = -1; g_master = -1;
  w.open = false; w.eof = false; w.exec_failed = false; w.adopted = false;
  w.child = 0; w.fg_pgrp = 0; w.cursor_reset_pending = false;
  w.out_key.clear(); w.out_reply.clear();
  if (w.ts != nullptr) { term::state_free(w.ts); w.ts = nullptr; }
  w.launcher = false; w.line.clear();

  int nxt = -1;                     // 切到下一个仍开着的窗口（同 close_current 语义）
  for (int k = 1; k < g_win_count; k++) {
    int i = (g_cur + k) % g_win_count;
    if (g_wins[i].open) { nxt = i; break; }
  }
  if (nxt >= 0) {
    g_cur = nxt;
    g_wins[nxt].launcher = false;
    term::fill_bg();
    use_win(nxt);
    term::view_follow();
    term::invalidate();
    term::present();
    char tb[64];
    snprintf(tb, sizeof tb, "Detached PTY %02d. Now PTY %02d", slot1, g_cur + 1);
    term::toast(tb);
  } else {
    char tb[48];
    snprintf(tb, sizeof tb, "Detached PTY %02d.", slot1);
    term::exit_prompt(tb);          // req 2：全屏倒计时 3s→2s→1s（会话保活）
    g_quit_after_close = true;
  }
}

int status_code(int st)
{
  return WIFEXITED(st) ? WEXITSTATUS(st) : (WIFSIGNALED(st) ? 128 + WTERMSIG(st) : 0);
}

// 重附着：收回被剥离会话（req 6/7/8 共用）。贴回纯文本历史（≤buffer_kb，
// 无颜色）→ 重建窗口状态 → TIOCSWINSZ（内核自动 SIGWINCH → 全屏 TUI 重绘）。
// 成功后窗口成为 adopted（child 非亲生：退出检测走 master EOF）。
bool attach_parked(int n)
{
  Win& w = g_wins[n];
  bgtask::Takeover tk;
  const size_t max_hist = (size_t)g_conf.bg_buffer_kb * 1024;
  if (!bgtask::takeover(n + 1, max_hist, tk)) return false;
  if (w.ts != nullptr) term::state_free(w.ts);
  if (w.font_size < 0) w.font_size = g_opt.font_size;
  w.ts = term::state_new(win_opt(w));
  if (w.ts == nullptr) { close(tk.master); return false; }
  w.open = true; w.eof = false; w.exec_failed = false; w.launcher = false;
  w.master = tk.master; w.child = tk.child; w.adopted = true;
  w.fg_pgrp = 0; w.cursor_reset_pending = false; w.line.clear();
  w.out_key.clear(); w.out_reply.clear();
  g_cur = n;
  use_win(n);
  term::reset_cursor_style();
  if (!tk.hist.empty()) term::write(tk.hist.data(), tk.hist.size());   // 贴回历史文本
  struct winsize ws;
  memset(&ws, 0, sizeof ws);
  ws.ws_col = tk.cols > 0 ? tk.cols : 53;
  ws.ws_row = tk.rows > 0 ? tk.rows : 17;
  // 恢复前后 PTY 尺寸相同 → 内核 pty_resize 判定无变化 → 不发 SIGWINCH，
  // TUI（nano/htop 等）不会重新布局。先设一个"±1 行"的过渡尺寸，**留出处理
  // 间隔**（两次 ioctl 之间若无间隔，SIGWINCH 会被应用合并处理，采样时尺寸
  // 已还原 → ncurses 判定无变化、只做差异重绘 → 界面残缺），再设真实尺寸：
  // 两次各触发一次全量重绘，最终画面正确。
  {
    struct winsize ws2;
    memset(&ws2, 0, sizeof ws2);
    ws2.ws_col = ws.ws_col;
    ws2.ws_row = ws.ws_row > 1 ? ws.ws_row - 1 : 2;
    ioctl(w.master, TIOCSWINSZ, &ws2);
    usleep(150000);                 // 等 TUI 处理第一个 SIGWINCH 并全量重绘
  }
  ioctl(w.master, TIOCSWINSZ, &ws);
  // Q3/Q4：附着后发送"重绘命令"（Ctrl+L = 0x0C，nano/less/vim/多数 TUI 的
  // 强制重绘键）触发程序重新绘制；bash 等会清屏重显提示符，历史仍在滚动区。
  feed_key(w, "\f", 1);
  term::invalidate();
  term::present();
  return true;
}

// Q6 降级：切换/启动到"已剥离但程序已退出"的槽位——贴回输出文本（滚动区）
// → 其后显示起始页面文本 → toast `Background PTY NN exited with X`（消费死亡记录）。
// 返回 false = 无死亡记录。
bool attach_dead(int n)
{
  Win& w = g_wins[n];
  std::string hist;
  int code = 0;
  if (!bgtask::take_dead(n + 1, hist, code)) return false;
  if (w.ts != nullptr) term::state_free(w.ts);
  if (w.font_size < 0) w.font_size = g_opt.font_size;
  w.ts = term::state_new(win_opt(w));
  if (w.ts == nullptr) return false;
  w.open = false; w.eof = false; w.exec_failed = false;
  w.master = -1; w.child = 0; w.adopted = false;
  w.launcher = true; w.line.clear();
  w.out_key.clear(); w.out_reply.clear();
  w.fg_pgrp = 0; w.cursor_reset_pending = false;
  g_cur = n;
  use_win(n);
  term::reset_cursor_style();
  term::fill_bg();
  if (!hist.empty()) term::write(hist.data(), hist.size());   // ① 输出文本（滚动区）
  launcher_banner(n);                                          // ② 起始页面文本 + 提示符
  term::invalidate();
  term::present();
  char tb[64];
  snprintf(tb, sizeof tb, "Background PTY %02d exited with %d", n + 1, code);
  term::toast(tb);
  return true;
}

void switch_to(int n, bool toast = true, bool banner = true)   // Ctrl+Alt+F1~F12 目标
{
  if (n < 0 || n >= g_win_count || n == g_cur) return;
  Win& old = g_wins[g_cur];
  if (!old.open) {                  // 点4：启动器下切窗 = 丢弃原窗口
    if (old.ts != nullptr) term::state_free(old.ts);
    old.ts = nullptr;
    old.launcher = false; old.line.clear();
  }
  g_cur = n;
  Win& w = g_wins[n];
  if (w.open) {
    w.launcher = false;
    term::fill_bg();                 // 切窗：先填背景/背景图清屏，再显示新窗口内容
    use_win(n);
    if (w.cursor_reset_pending) {    // 后台期间该窗前台程序退出过：此刻复位光标呈现
      w.cursor_reset_pending = false;
      term::reset_cursor_style();
    }
    term::view_follow();            // 切窗目标也跳底（离开侧已跳）
    term::invalidate();             // 切窗 = 整屏重画（防旧窗口残留）
    term::present();
  } else if (bgtask::session_exists(n + 1)) {   // 槽位有被剥离的会话 → 重附着（req 6）
    if (attach_parked(n)) {
      if (toast) {
        char tb[48];
        snprintf(tb, sizeof tb, "Attached PTY %02d", n + 1);
        term::toast(tb);
      }
      return;
    }
    launcher_show(n, banner);       // 附着失败（守护进程/tmux 异常）：退化为初始界面
  } else if (attach_dead(n)) {
    return;                          // Q6：死亡记录已展示并自带专用 toast
  } else {
    launcher_show(n, banner);       // 点3：非活跃窗口 → 初始界面（banner=false：Fn+H 帮助路径）
  }
  if (toast) {                      // 事件通知：切到 PTY nn（1 基）
    char tb[48];
    snprintf(tb, sizeof tb, "Switched to PTY %02d", n + 1);
    term::toast(tb);
  }
}

void kill_all();                // 前置声明（exec failed 直接退出时用）

// 窗口退出巡检。返回 true = 关闭 cardterm（*rc = 退出码）。
// 前台进程组变化 = 上一个前台程序（vim/crush/less…）退出或切走。
// 很多 TUI 用 OSC 12 改光标颜色后不会复位，这里在它们退出时兜底恢复默认呈现。
static void sync_fg_programs()
{
  for (int i = 0; i < g_win_count; i++) {
    Win& w = g_wins[i];
    if (!w.open || w.master < 0) { w.fg_pgrp = 0; continue; }
    pid_t pg = tcgetpgrp(w.master);
    if (pg <= 0) continue;
    // 只有"前台进程组回到会话首进程（shell）"才代表上一个前台程序退出；
    // 命令启动时的 pgrp 变化不复位（否则会覆盖应用刚设的光标形状/闪烁）。
    if (w.fg_pgrp != 0 && pg == w.child && w.fg_pgrp != w.child) {
      if (i == g_cur) {
        term::reset_cursor_style();      // 仅当前可见窗口：会 present，安全
        term::present();
      } else {
        w.cursor_reset_pending = true;   // 后台窗口：等切到前台再做，避免污染当前屏
      }
    }
    w.fg_pgrp = pg;
  }
}

bool check_exits(int* rc)
{
  for (int i = 0; i < g_win_count; i++) {
    Win& w = g_wins[i];
    if (!w.open) continue;
    bool gone = false;
    if (waitpid(w.child, &w.status, WNOHANG) == w.child) gone = true;
    else if (w.eof) {               // pty 全关但进程活着：关窗语义收尾
      kill(-w.child, SIGHUP);
      for (int k = 0; k < 20; k++) {
        if (waitpid(w.child, &w.status, WNOHANG) != 0) break;
        usleep(50000);
      }
      if (waitpid(w.child, &w.status, WNOHANG) == 0) {
        kill(-w.child, SIGKILL);
        waitpid(w.child, &w.status, 0);
      } else if (w.adopted && w.child > 0 && kill(-w.child, 0) == 0) {
        kill(-w.child, SIGKILL);        // 附着会话：非亲生 waitpid 不可用，按 pgid 补刀
      }
      gone = true;
    }
    if (!gone) continue;

    close(w.master); w.master = -1;
    w.out_key.clear(); w.out_reply.clear();   // 进程退出：出站队列清空释放
    w.cursor_reset_pending = false; w.fg_pgrp = 0;
    int st = w.status;
    const int code = status_code(st);
    fprintf(stderr, "[exit] win=%d status=%d raw=%d\n", i, code, st);
    w.open = false;
    // 事件通知：窗口（含后台窗口）的程序退出 —— 先切到可见窗口再弹 toast
    char tb[96];
    if (i == g_cur) {
      int nxt = first_open();
      if (nxt >= 0) {                  // 当前窗程序结束：切到仍打开的窗口并提示
        switch_to(nxt);
        snprintf(tb, sizeof tb, "PTY %02d exited with %d. Now PTY %02d",
                 i + 1, code, g_cur + 1);
        term::toast(tb);
        continue;
      }
      // 没有其它开窗：直接退出程序（与"关掉最后一个窗口"一致）
      if (w.ts != nullptr) { term::state_free(w.ts); w.ts = nullptr; }
      *rc = code;
      return true;
    }
    // 后台窗：保持当前活跃窗口不变，只弹 toast
    snprintf(tb, sizeof tb, "background PTY %02d exited with %d", i + 1, code);
    term::toast(tb);
    // 后台窗退出：静默关闭并立即释放终端状态（屏缓冲/回滚随窗释放）
    if (w.ts != nullptr) { term::state_free(w.ts); w.ts = nullptr; }
  }
  return false;
}

// 长按 Esc：退出所有程序、关闭全部窗口、退出 cardterm
void kill_all()
{
  for (int i = 0; i < g_win_count; i++) {
    Win& w = g_wins[i];
    if (w.open) {
      kill(-w.child, SIGHUP);
      for (int k = 0; k < 20; k++) {
        if (waitpid(w.child, &w.status, WNOHANG) != 0) break;
        usleep(50000);
      }
      if (waitpid(w.child, &w.status, WNOHANG) == 0) {
        kill(-w.child, SIGKILL);
        waitpid(w.child, &w.status, 0);
      } else if (w.adopted && w.child > 0 && kill(-w.child, 0) == 0) {
        kill(-w.child, SIGKILL);        // 附着会话：非亲生 waitpid 不可用，按 pgid 补刀
      }
      close(w.master); w.master = -1; w.open = false;
      w.out_key.clear(); w.out_reply.clear();   // 出站队列清空释放
    }
    if (w.ts != nullptr) { term::state_free(w.ts); w.ts = nullptr; }
  }
}

}  // namespace

int run(const std::vector<std::string>& first_cmd, const term::Options& opt)
{
  g_opt = opt;
  g_unicode = opt.unicode;
  // 配置快照：ui.cpp 已 cfg::load() 过（cfg::current() 为那次解析结果）。
  // 启动器底部提示行显示 [general] launch 的第一个词（默认 bash）；
  // 空回车运行的默认程序 = [general] launch 分词（空则该行显示/运行 bash）。
  g_conf = cfg::current();
  g_win_count = g_conf.windows;          // 槽位数（[general] windows；config 已钳位 2–32）
  if (g_win_count < 2) g_win_count = 2;
  if (g_win_count > 32) g_win_count = 32;
  g_default_cmd = split_words(g_conf.launch);
  if (g_default_cmd.empty()) g_default_cmd.push_back("bash");
  g_prompt_cmd = g_default_cmd.front();
  std::string t;
  kbd::KeyEvent kev;
  kbd::Cmd cmd = kbd::Cmd::None;
  bool ctrl = false, alt = false;

  // 点5：默认位于窗口 1
  // req 8：启动扫描——在第一个窗口初始化/显示 banner 之前检查后台会话
  // （cardterm_PTY_01..NN）：有则直接附着最小号窗口（不初始化 PTY 01、
  //  不跑默认启动命令行），并提示 Attached PTY **。
  const int bg_slot = bgtask::first_existing_session(g_win_count);
  int dead_slot = -1;               // Q6：已剥离但程序已退出的槽位（最小号优先）
  for (int i = 0; i < g_win_count && dead_slot < 0; i++)
    if (bgtask::has_dead(i + 1)) dead_slot = i;
  if (bg_slot >= 0) {
    g_cur = bg_slot;
    if (attach_parked(bg_slot)) {
      char tb[48];
      snprintf(tb, sizeof tb, "Attached PTY %02d", bg_slot + 1);
      term::toast(tb);
    } else {
      launcher_show(bg_slot);       // 附着失败（守护进程/tmux 异常）：退化为初始界面
    }
  } else if (dead_slot >= 0) {
    g_cur = dead_slot;
    if (!attach_dead(dead_slot)) launcher_show(dead_slot);   // 自带专用 toast
  } else if (!first_cmd.empty()) {          // cardterm <cmd>：窗口 1 直接开会话
    Win& w = g_wins[0];
    w.font_size = opt.font_size;
    w.ts = term::state_new(win_opt(w));
    if (w.ts == nullptr) { fprintf(stderr, "[session] state_new failed\n"); return 1; }
    g_cur = 0;
    use_win(0);
    // 每次启动都与"新建窗口"一致：开屏文本 + `> 启动命令`（等同用户在新建窗口
    // 里输入命令并回车），换行后由程序接续自己的真实提示符（光标落在其中）。
    // 命令显示 = 新建窗口时会输入的命令 = 实际启动命令（launch= 配置或命令行参数）。
    // launch 未配置时走下方 launcher_show(0)，与新建窗口完全一致（`> [光标]` 等输入）。
    splash_show(0);
    launcher_prompt();                                   // regular7 的 '> '
    {
      std::string joined;
      for (size_t i = 0; i < first_cmd.size(); i++) {
        if (i) joined += ' ';
        joined += first_cmd[i];
      }
      term::write(joined.c_str(), joined.size());        // 命令文本（默认前景色，同启动器回显）
    }
    term::write("\r\n", 2);
    int master = -1;
    bool ef = false;
    pid_t pid = spawn(first_cmd, &master, &ef);
    if (pid < 0) {
      term::write("\033[38;2;255;80;80mcardterm: cannot start session\033[0m\r\n");
      term::present();
      return 1;
    }
    w.open = true; w.launcher = false;
    w.master = master; w.child = pid;
    w.exec_failed = ef;
    g_master = master;
    term::present();
  } else {                                  // 无参数：窗口 1 = 初始界面
    launcher_show(0);
  }

  if (g_wins[g_cur].ts == nullptr) {          // state_new 失败（OOM）：干净退出，由 atexit 还原显示
    fprintf(stderr, "[session] no window state, aborting\n");
    return 1;
  }

  for (;;) {
    if (hw::exit_requested()) return 0;      // SIGINT/SIGTERM/SIGHUP：交给 atexit 还原显示
    if (++g_fg_tick >= 8) { g_fg_tick = 0; sync_fg_programs(); }   // 前台程序退出 → 复位光标呈现（降频省 ioctl）
    if (kbd::device_lost()) {                // 键盘消失：留提示后退出，避免死终端
      term::write("\r\n\033[38;2;255;80;80m[cardterm] keyboard device lost\033[0m\r\n");
      term::present();
      usleep(300000);
      return 0;
    }
    term::tick();                           // 光标 + 合帧（活跃窗口）
    const bool got = pump_all();
    pump_out();                             // 出站排空（有界预算：≤16KB / ≤2ms / 轮）
    term::tick();                           // 输出攒批后上屏

    int rc = 0;
    if (check_exits(&rc)) return rc;

    // 输出流期间（60ms 内有数据）键盘等待不阻塞；出站有积压时也缩短等待：
    //   有进展 → 内核还有空间，1ms 快转尽快排空；
    //   无进展 → 子进程不读（输入缓冲满），5ms 等它腾空间，避免空转烧 CPU。
    const bool flowing = now_ms() - g_last_data < 60;
    int kbd_to = 30;
    if (flowing || (g_out_pending && g_out_progress)) kbd_to = 1;
    else if (g_out_pending) kbd_to = 5;
    if (kbd_to < 1) kbd_to = 1;                // 防御：0 超时会让主循环忙等（冒充卡死）
    kbd::Poll r = kbd::poll(kbd_to, t, &kev, &cmd);
    switch (r) {
      case kbd::Poll::Quit:                 // 按住 Esc 2s = 关闭全部窗口并退出
        kill_all();
        return 0;
      case kbd::Poll::EscHint:              // 按住 Esc 0.5s：提示继续按住 2s 退出
        term::toast("Hold ESC for 2s to exit");   // 显示时长用配置默认（toast_ms）
        break;
      case kbd::Poll::Mod:
        if (t == "ctrl_on") ctrl = true;
        else if (t == "ctrl_off") ctrl = false;
        else if (t == "alt_on") alt = true;
        else if (t == "alt_off") alt = false;
        (void)ctrl; (void)alt;
        break;
      case kbd::Poll::Aux:
        break;
      case kbd::Poll::Cmd:
        term::view_follow();                       // FN 组合 = 翻页以外的键 → 跳底
        if (cmd == kbd::Cmd::Close) { close_current(); if (g_quit_after_close) return 0; }
        else if (cmd == kbd::Cmd::PrevWin) switch_to((g_cur + g_win_count - 1) % g_win_count);
        else if (cmd == kbd::Cmd::NextWin) switch_to((g_cur + 1) % g_win_count);
        else if (cmd == kbd::Cmd::FontDown) change_font(-1);
        else if (cmd == kbd::Cmd::FontUp)   change_font(+1);
        else if (cmd == kbd::Cmd::ShowHelp) show_help_in_empty_window();
        else if (cmd == kbd::Cmd::Detach) {
          detach_current();                       // Fn+A / Super+A：剥离到后台
          if (g_quit_after_close) return 0;       // 剥离末窗：倒计时已结束，退出
        }
        else if ((int)cmd >= (int)kbd::Cmd::Win1 && (int)cmd <= (int)kbd::Cmd::Win12) {
          const int slot = (int)cmd - (int)kbd::Cmd::Win1;   // Super+F1~F12
          if (slot < g_win_count) switch_to(slot);
          else {
            // 目标槽位超出配置的窗口数：提示后不做任何事（见 config [general] windows）
            char tb[64];
            snprintf(tb, sizeof tb, "Max PTY is %02d. Edit config.ini.", g_win_count);
            term::toast(tb);
          }
        }
        // Fn+Space（弹出中文输入法）：预留接线。keyboard.h 的 kbd::Cmd 目前没有
        // Space/IME 命令（未改 keyboard.*），等键盘层新增后在此接：
        //   else if (cmd == kbd::Cmd::ImePopup) term::toast("弹出中文输入法 (占位符)");
        break;
      case kbd::Poll::Echo: {
        // ── 视图跟随：手动翻页键（含松开/长按重发）豁免；
        //    其它任何按键按下即跳底（不管是否触发缓冲区写入）
        {
          const bool sh = (kev.mods & 1) != 0, al = (kev.mods & 2) != 0, ct = (kev.mods & 4) != 0;
          const bool paging =
            (t == "PgUp" && sh && !ct && !al) || (t == "PgDn" && sh && !ct && !al) ||
            ((t == "Dn" || t == "Down") && ct && sh && !al) || (t == "Up" && ct && sh && !al);
          if (!paging) term::view_follow();
        }
        // ── Ctrl+Alt+F1~F12：切换窗口（仅按下/重复触发）──────────
        if (kev.ev != 3 && (kev.mods & 6) == 6 && t.size() >= 2 && t[0] == 'F') {
          int n = atoi(t.c_str() + 1);
          if (n >= 1 && n <= 12) {
            if (n <= g_win_count) switch_to(n - 1);
            else {
              // 目标槽位超出配置的窗口数：提示后不做任何事
              char tb[64];
              snprintf(tb, sizeof tb, "Max PTY is %02d. Edit config.ini.", g_win_count);
              term::toast(tb);
            }
            break;   // 拦截：无论切换还是超限提示，都不再转发给 PTY
          }
        }
        Win& w = g_wins[g_cur];
        if (w.open) send_key(w, t, kev);
        else launcher_key(w, t, kev);
        break;
      }
      case kbd::Poll::Timeout:
        break;
    }
  }
}

}  // namespace session
