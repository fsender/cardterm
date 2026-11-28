// bgtask.cpp — 后台会话守护进程（bgtaskd）/ tmux 中继 / 客户端
// 架构与协议见 docs/bgtask-design.md §2 与 bgtask.h 头注释。
//
// 协议（AF_UNIX SOCK_STREAM，路径 $XDG_RUNTIME_DIR/cardterm-bgtask.sock，0600）：
//   PARK <name> <cols> <rows> <child> <pgid> <hist_lines>\n   + SCM_RIGHTS(master fd)
//       → OK\n / ERR <msg>\n
//   TAKEOVER <name> <max_hist>\n
//       → OK <child> <pgid> <cols> <rows> <hist_len>\n  + SCM_RIGHTS(master fd)
//         + <hist_len 字节纯文本历史>          （守护进程随后销毁 tmux 会话）
//   RELAY <name>\n                            （tmux pane 内中继进程接入，连接随后
//       → （无回复）                           变为该会话的数据通道）
//   LIST\n → 每行一个会话名，空行结束

#include "bgtask.h"

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <fcntl.h>
#include <pwd.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <cstdint>
#include <time.h>
#include <unistd.h>

namespace bgtask {
namespace {

// ── 基础工具 ─────────────────────────────────────────────────────

std::string ctrl_sock_path()
{
  const char* rt = getenv("XDG_RUNTIME_DIR");
  std::string dir = (rt != nullptr && *rt != '\0') ? rt : "/tmp";
  return dir + "/cardterm-bgtask.sock";
}

std::string shell_quote(const std::string& s)
{
  std::string out = "'";
  for (char c : s) { if (c == '\'') out += "'\\''"; else out += c; }
  out += "'";
  return out;
}

bool set_nonblock(int fd)
{
  int fl = fcntl(fd, F_GETFL, 0);
  return fl >= 0 && fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0;
}

// 单调毫秒钟（自退空闲窗口用；rc219）
uint64_t now_ms()
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

// fork + /bin/sh -c <cmd>，同步等退出；返回退出码（-1 = fork/exec 失败）
int run_sh(const std::string& cmd)
{
  pid_t pid = fork();
  if (pid < 0) return -1;
  if (pid == 0) {
    execl("/bin/sh", "sh", "-c", cmd.c_str(), (char*)nullptr);
    _exit(127);
  }
  int st = 0;
  while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
  return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

// 把 fd 与一行文本同消息发出（SCM_RIGHTS）
bool send_line_fd(int sock, const std::string& line, int fd)
{
  struct iovec iov;
  iov.iov_base = (void*)line.data();
  iov.iov_len = line.size();
  union { struct cmsghdr align; char buf[CMSG_SPACE(sizeof(int))]; } u;
  struct msghdr msg;
  memset(&msg, 0, sizeof msg);
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  if (fd >= 0) {
    msg.msg_control = u.buf;
    msg.msg_controllen = sizeof u.buf;
    struct cmsghdr* c = CMSG_FIRSTHDR(&msg);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &fd, sizeof(int));
  }
  ssize_t n;
  do { n = sendmsg(sock, &msg, 0); } while (n < 0 && errno == EINTR);
  return n == (ssize_t)line.size();
}

// 收一行（含 \n）；顺带取出随行的 SCM_RIGHTS fd（无则 *fd_out = -1）
bool recv_line_fd(int sock, std::string& line, int* fd_out)
{
  if (fd_out != nullptr) *fd_out = -1;
  line.clear();
  char buf[1024];
  union { struct cmsghdr align; char buf[CMSG_SPACE(sizeof(int))]; } u;
  for (;;) {
    struct iovec iov;
    iov.iov_base = buf;
    iov.iov_len = sizeof buf;
    struct msghdr msg;
    memset(&msg, 0, sizeof msg);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = u.buf;
    msg.msg_controllen = sizeof u.buf;
    ssize_t n;
    do { n = recvmsg(sock, &msg, 0); } while (n < 0 && errno == EINTR);
    if (n <= 0) return false;
    for (struct cmsghdr* c = CMSG_FIRSTHDR(&msg); c != nullptr; c = CMSG_NXTHDR(&msg, c)) {
      if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS && fd_out != nullptr) {
        memcpy(fd_out, CMSG_DATA(c), sizeof(int));
      }
    }
    line.append(buf, (size_t)n);
    if (line.find('\n') != std::string::npos) return true;
    if (line.size() > 65536) return false;
  }
}

// 读满 n 字节（阻塞 fd）
bool read_full(int fd, void* dst, size_t n)
{
  char* p = (char*)dst;
  while (n > 0) {
    ssize_t r = read(fd, p, n);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) return false;
    p += r; n -= (size_t)r;
  }
  return true;
}

// 截取输出的“最后 cap 字节”（保留最新内容），行尾归一为 \r\n
// （tmux capture-pane 以 \n 分行；终端解析器 LF 语义 = 下移一行不回列，
//  直接喂会产生阶梯错位），并去掉尾部多余空白。
std::string tail_text(const std::string& s, size_t cap)
{
  std::string out = (s.size() > cap) ? s.substr(s.size() - cap) : s;
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' '))
    out.pop_back();
  std::string norm;
  norm.reserve(out.size() + 16);
  for (size_t i = 0; i < out.size(); i++) {
    const char c = out[i];
    if (c == '\n' && (i == 0 || out[i - 1] != '\r')) norm += '\r';   // LF → CRLF
    norm += c;
  }
  if (!norm.empty()) norm += "\r\n";
  return norm;
}

std::string exe_path()
{
  char buf[4096];
  ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
  if (n <= 0) return "cardterm";
  buf[n] = '\0';
  return std::string(buf);
}

// ── 有界队列（守护进程泵：满则停止读 → 天然反压，不丢字节）────────
struct Q {
  static constexpr size_t kCap = 131072;
  char buf[kCap];
  size_t off = 0, len = 0;
  size_t space() const { return kCap - (off + len); }
  size_t avail() const { return len; }
  bool empty() const { return len == 0; }
  const char* rd() const { return buf + off; }
  char* wr() { return buf + off + len; }
  void produce(size_t n) { len += n; }
  void consume(size_t n)
  {
    off += n; len -= n;
    if (len == 0) off = 0;
    else if (off >= kCap / 2) { memmove(buf, buf + off, len); off = 0; }
  }
};

// ── 守护进程状态 ─────────────────────────────────────────────────
struct Sess {
  int master = -1;          // 被剥离程序的 PTY master
  int relay = -1;           // tmux pane 中继连接
  pid_t child = 0, pgid = 0;
  int cols = 0, rows = 0;
  Q m2r, r2m;
};
std::map<std::string, Sess> g_sess;

std::string g_exe;
std::string g_sock_path;

// 守护进程诊断日志（/tmp/bgtaskd.log；仅错误/生命周期事件）
void dlog(const char* fmt, ...)
{
  FILE* fp = fopen("/tmp/bgtaskd.log", "a");
  if (fp == nullptr) return;
  va_list ap;
  va_start(ap, fmt);
  vfprintf(fp, fmt, ap);
  va_end(ap);
  fputc('\n', fp);
  fclose(fp);
}

void tmux_kill_session(const std::string& name)
{
  run_sh("tmux -L cardterm kill-session -t " + shell_quote(name) + " 2>/dev/null");
}

std::string tmux_capture(const std::string& name, size_t cap)
{
  FILE* fp = popen(("tmux -L cardterm capture-pane -p -t " + shell_quote(name) +
                    " -S - 2>/dev/null").c_str(), "r");
  if (fp == nullptr) return std::string();
  std::string out;
  char buf[8192];
  const size_t kHardCap = cap + 65536;    // 防御上限（cap 已由调用方限幅）
  while (out.size() < kHardCap) {
    size_t r = fread(buf, 1, sizeof buf, fp);
    if (r == 0) break;
    out.append(buf, r);
  }
  pclose(fp);
  return tail_text(out, cap);
}

// 死亡会话记录文件（Q6 降级）：$XDG_RUNTIME_DIR（或 /tmp）/cardterm-dead-<name>
std::string dead_record_path(const std::string& name)
{
  const std::string sock = ctrl_sock_path();
  const size_t slash = sock.rfind('/');
  const std::string dir = (slash == std::string::npos) ? std::string("/tmp")
                                                       : sock.substr(0, slash);
  return dir + "/cardterm-dead-" + name;
}

// 读取程序退出码：/proc/<pid>/stat 第 52 字段 exit_code（僵尸态可见）。
// 程序刚死（master EOF）时一般已是僵尸且收养者（launcher）不会主动收尸，
// 读取窗口足够；读不到时回退 0（无法区分，罕见）。
int read_exit_code(pid_t pid)
{
  char path[64], buf[512];
  snprintf(path, sizeof path, "/proc/%d/stat", (int)pid);
  for (int i = 0; i < 25; i++) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
      ssize_t n = read(fd, buf, sizeof buf - 1);
      close(fd);
      if (n > 0) {
        buf[n] = '\0';
        char* rp = strrchr(buf, ')');
        if (rp != nullptr) {
          // ')' 后第 1 个字段=state(3)，exit_code=第 52 字段 → 第 50 个
          int f = 0;
          long code = -1;
          char state = ' ';
          char* p = rp + 1;
          while (*p != '\0') {
            while (*p == ' ') p++;
            if (*p == '\0') break;
            f++;
            if (f == 1) state = *p;
            if (f == 50) code = strtol(p, nullptr, 10);
            while (*p != '\0' && *p != ' ') p++;
          }
          if (state == 'Z' && code >= 0) {
            // /proc/<pid>/stat 第 52 字段是 raw wait 状态字（waitpid(2) 格式），
            // 需解码：正常退出取 WEXITSTATUS，被信号杀按 shell 惯例 128+sig。
            const int st = (int)code;
            if (WIFEXITED(st)) return WEXITSTATUS(st);
            if (WIFSIGNALED(st)) return 128 + WTERMSIG(st);
            return st >> 8;
          }
        }
      }
    } else if (errno == ENOENT) {
      break;                              // 已被收尸：退出码不可得
    }
    usleep(20000);
  }
  return 0;
}

// 会话程序在剥离期间退出：清理前把输出文本 + 退出码存档（Q6 降级），
// 供切回/重启到该槽位时贴回展示（take_dead 读取后删除）。
void write_dead_record(const std::string& name, pid_t child, size_t cap)
{
  const int code = read_exit_code(child);            // 先抢退出码（僵尸窗口）
  const std::string hist = tmux_capture(name, cap);  // 再存输出文本
  const std::string path = dead_record_path(name);
  FILE* fp = fopen(path.c_str(), "w");
  if (fp == nullptr) {
    dlog("[daemon] dead record open failed: %s", path.c_str());
    return;
  }
  fprintf(fp, "EXIT %d\n", code);
  fwrite(hist.data(), 1, hist.size(), fp);
  fclose(fp);
  dlog("[daemon] dead record: %s code=%d hist=%zu", name.c_str(), code, hist.size());
}

void sess_cleanup(Sess& s, const std::string& name)
{
  if (s.master >= 0) close(s.master);
  if (s.relay >= 0) close(s.relay);
  tmux_kill_session(name);
}

}  // namespace

// ── 公开：名字/探测（客户端与 session 侧共用）────────────────────

std::string slot_name(int n1)
{
  char b[32];
  snprintf(b, sizeof b, "cardterm_PTY_%02d", n1);
  return std::string(b);
}

bool session_exists(int n1)
{
  const std::string cmd = "tmux -L cardterm has-session -t " + shell_quote(slot_name(n1)) +
                          " 2>/dev/null";
  FILE* fp = popen(cmd.c_str(), "r");
  if (fp == nullptr) return false;
  return pclose(fp) == 0;
}

std::vector<std::string> list_sessions()
{
  // 一次 list-sessions 枚举（避免逐个 popen；rc211）
  std::vector<std::string> names;
  FILE* fp = popen("tmux -L cardterm list-sessions -F '#{session_name}' 2>/dev/null", "r");
  if (fp == nullptr) return names;
  char line[256];
  while (fgets(line, sizeof line, fp) != nullptr) {
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
    names.push_back(line);
  }
  pclose(fp);
  return names;
}

int first_existing_session(int win_count)
{
  // 一次 list-sessions 枚举（避免逐个 popen）
  const std::vector<std::string> names = list_sessions();
  for (int i = 1; i <= win_count; i++) {
    const std::string want = slot_name(i);
    for (const auto& s : names) if (s == want) return i - 1;
  }
  return -1;
}

namespace {

// 后台会话管理二进制（rc216/M1）：优先与本可执行同目录的 cardterm-bgtask
// （deb 装 /usr/share/APPLaunch/bin/、维护部署在 ~/apps/cardterm/ 都满足）；
// 找不到再回退本可执行（旧布局：cardterm --bgtaskd 自身）。
std::string bgtask_exe()
{
  const std::string self = exe_path();
  const size_t slash = self.rfind('/');
  if (slash != std::string::npos) {
    const std::string cand = self.substr(0, slash + 1) + "cardterm-bgtask";
    if (access(cand.c_str(), X_OK) == 0) return cand;
  }
  return self;
}

// 生成用户级 systemd 单元（1.3.1）：单元不再随 deb 装 /usr/lib/systemd/user/
// （商店安装路径规则不允许该目录），改为首次使用时写到用户自己的
// ~/.config/systemd/user/cardterm-bgtask.service——仍是用户态 systemd 服务
// （Restart=no，守护自管 5s 空闲自退）。已存在则尊重用户修改不覆盖；删除该
// 文件即恢复默认（下次拉起时重新生成）。返回是否刚写入（调用方据此 reload）。
bool ensure_user_unit(const std::string& exe)
{
  const char* h = getenv("HOME");
  std::string home = (h != nullptr && *h != '\0') ? h : "";
  if (home.empty()) {
    struct passwd* pw = getpwuid(getuid());
    if (pw != nullptr && pw->pw_dir != nullptr && *pw->pw_dir != '\0') home = pw->pw_dir;
  }
  if (home.empty()) return false;
  const std::string dir = home + "/.config/systemd/user";
  const std::string path = dir + "/cardterm-bgtask.service";
  struct stat st;
  if (stat(path.c_str(), &st) == 0) return false;         // 已存在（含用户自改）：不覆盖
  mkdir((home + "/.config").c_str(), 0755);
  mkdir((home + "/.config/systemd").c_str(), 0755);
  mkdir(dir.c_str(), 0755);
  std::string s;
  s += "# cardterm-bgtask.service — cardterm 后台会话管理（用户级 systemd）\n";
  s += "# cardterm 首次拉起时自动生成；删除本文件即恢复默认（下次重新生成）。\n";
  s += "# Restart=no：守护进程/后台会话退出都不重启（守护自管 5s 空闲自退）。\n";
  s += "[Unit]\n";
  s += "Description=cardterm background session manager (bgtask daemon)\n";
  s += "\n[Service]\n";
  s += "Type=simple\n";
  s += "ExecStart=" + exe + " --bgtaskd\n";
  s += "Restart=no\n";
  FILE* fp = fopen(path.c_str(), "w");
  if (fp == nullptr) return false;
  const size_t n = fwrite(s.data(), 1, s.size(), fp);
  fclose(fp);
  return n == s.size();
}

// 连接守护进程；未运行则启动（systemd 用户单元 → 直接 fork+exec 回退）并重试
int connect_daemon()
{
  auto try_connect = []() -> int {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", ctrl_sock_path().c_str());
    if (connect(fd, (struct sockaddr*)&addr, sizeof addr) == 0) return fd;
    close(fd);
    return -1;
  };

  int fd = try_connect();
  if (fd >= 0) return fd;

  // ① 用户级 systemd 总体管理（rc216/M1；1.3.1 单元改为运行时生成到
  //    ~/.config/systemd/user/，行为不变）。单元缺失或用户会话无 systemd 时
  //    systemctl 非零退出，直接走 ②。
  const std::string exe1 = bgtask_exe();
  if (ensure_user_unit(exe1))
    run_sh("systemctl --user daemon-reload 2>/dev/null");
  if (run_sh("systemctl --user start cardterm-bgtask.service 2>/dev/null") == 0) {
    for (int i = 0; i < 15; i++) {
      usleep(100000);
      fd = try_connect();
      if (fd >= 0) return fd;
    }
  }

  // ② 直接拉起 cardterm-bgtask（路径/argv 全部 fork 前备好：子进程 exec 前只用
  //    async-signal-safe 调用，防多线程 fork 死锁；M1）
  //    双 fork 脱壳（rc218）：孙进程交 init/会话回收者收养，守护退出不给
  //    cardterm 留僵尸；中间父由下方 waitpid 立即收割。
  const std::string exe = bgtask_exe();
  const char* av[3] = { exe.c_str(), "--bgtaskd", nullptr };
  pid_t pid = fork();
  if (pid == 0) {
    setsid();
    if (fork() != 0) _exit(0);                   // 中间父立即退出
    int nd = open("/dev/null", O_RDWR);
    if (nd >= 0) {
      dup2(nd, STDIN_FILENO); dup2(nd, STDOUT_FILENO); dup2(nd, STDERR_FILENO);
      if (nd > 2) close(nd);
    }
    long open_max = sysconf(_SC_OPEN_MAX);
    if (open_max < 0 || open_max > 4096) open_max = 4096;
    for (int f = 3; f < (int)open_max; f++) close(f);
    // argv[0] 用完整路径：pkill -f / ps 可识别（"cardterm --bgtaskd" 模式匹配不到）
    execv(av[0], (char* const*)av);
    _exit(127);
  }
  if (pid > 0) { int st = 0; waitpid(pid, &st, 0); }   // 收割中间父（立即退出）
  for (int i = 0; i < 30; i++) {          // 等守护进程绑定 socket
    usleep(100000);
    fd = try_connect();
    if (fd >= 0) return fd;
  }
  return -1;
}

}  // namespace

bool park(int n1, int master_fd, pid_t child, pid_t pgid,
          int cols, int rows, int hist_lines)
{
  if (master_fd < 0 || n1 <= 0) return false;
  int sock = connect_daemon();
  if (sock < 0) return false;
  char line[160];
  snprintf(line, sizeof line, "PARK %s %d %d %ld %ld %d\n",
           slot_name(n1).c_str(), cols, rows, (long)child, (long)pgid, hist_lines);
  bool ok = send_line_fd(sock, line, master_fd);
  std::string reply;
  if (ok) ok = recv_line_fd(sock, reply, nullptr);
  close(sock);
  return ok && reply.rfind("OK", 0) == 0;
}

bool takeover(int n1, size_t max_hist, Takeover& out)
{
  if (n1 <= 0) return false;
  int sock = connect_daemon();
  if (sock < 0) return false;
  char line[96];
  snprintf(line, sizeof line, "TAKEOVER %s %zu\n", slot_name(n1).c_str(), max_hist);
  if (send_line_fd(sock, line, -1) == false) { close(sock); return false; }

  std::string header;
  int fd = -1;
  if (!recv_line_fd(sock, header, &fd) || header.rfind("OK", 0) != 0) {
    if (fd >= 0) close(fd);
    close(sock);
    return false;
  }
  long child = 0, pgid = 0;
  int cols = 0, rows = 0;
  long hist_len = 0;
  sscanf(header.c_str(), "OK %ld %ld %d %d %ld", &child, &pgid, &cols, &rows, &hist_len);
  if (hist_len < 0 || hist_len > (long)(16u << 20) || fd < 0) {
    if (fd >= 0) close(fd);
    close(sock);
    return false;
  }
  out.child = (pid_t)child;
  out.pgid = (pid_t)pgid;
  out.cols = cols;
  out.rows = rows;
  out.master = fd;
  out.hist.clear();
  out.hist.resize((size_t)hist_len);
  bool ok = hist_len == 0 || read_full(sock, &out.hist[0], (size_t)hist_len);
  close(sock);
  return ok;
}

bool has_dead(int n1)
{
  if (n1 <= 0) return false;
  struct stat st;
  return stat(dead_record_path(slot_name(n1)).c_str(), &st) == 0;
}

bool take_dead(int n1, std::string& text, int& code)
{
  text.clear();
  code = 0;
  if (n1 <= 0) return false;
  const std::string path = dead_record_path(slot_name(n1));
  FILE* fp = fopen(path.c_str(), "r");
  if (fp == nullptr) return false;
  char line[64];
  if (fgets(line, sizeof line, fp) != nullptr) sscanf(line, "EXIT %d", &code);
  char buf[8192];
  size_t n;
  const size_t kMax = 256 * 1024;
  while ((n = fread(buf, 1, sizeof buf, fp)) > 0) {
    if (text.size() + n > kMax) { text.append(buf, kMax - text.size()); break; }
    text.append(buf, n);
  }
  fclose(fp);
  unlink(path.c_str());                       // 消费后删除
  return true;
}

// ── 守护进程主循环 ───────────────────────────────────────────────

int run_daemon()
{
  signal(SIGPIPE, SIG_IGN);
  g_exe = exe_path();
  g_sock_path = ctrl_sock_path();
  dlog("[daemon] start pid=%d sock=%s exe=%s", (int)getpid(),
       g_sock_path.c_str(), g_exe.c_str());

  int lfd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (lfd < 0) return 1;
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof addr);
  addr.sun_family = AF_UNIX;
  snprintf(addr.sun_path, sizeof addr.sun_path, "%s", g_sock_path.c_str());
  // 绑定竞态防护（rc220/L14）：unlink+bind 原本非原子——两个守护同时首拉时
  // 后启动者会删掉先启动者刚绑定的 socket 造成互踩/孤儿。改为：先 bind；
  // EADDRINUSE 时先探测既有守护（活着 = 本方让位正常退出），仅当确认是
  // 陈旧 socket 文件才 unlink 重试；lstat 闸门兼防 /tmp 回退目录里被栽的
  // 非 socket 文件被误删（竞态面收敛）。
  bool bound = false;
  for (int attempt = 0; attempt < 2 && !bound; attempt++) {
    if (bind(lfd, (struct sockaddr*)&addr, sizeof addr) == 0) { bound = true; break; }
    if (errno != EADDRINUSE) break;
    int probe = socket(AF_UNIX, SOCK_STREAM, 0);
    if (probe >= 0) {
      const bool alive = connect(probe, (struct sockaddr*)&addr, sizeof addr) == 0;
      close(probe);
      if (alive) { close(lfd); dlog("[daemon] another daemon alive, yield"); return 0; }
    }
    struct stat st;
    if (lstat(g_sock_path.c_str(), &st) == 0 && S_ISSOCK(st.st_mode)) unlink(g_sock_path.c_str());
    else break;
  }
  if (!bound || listen(lfd, 8) != 0) {
    close(lfd);
    return 1;
  }
  chmod(g_sock_path.c_str(), 0600);

  uint64_t empty_since = now_ms();
  for (;;) {
    // 无托管会话持续 5s → 自动退出（刚启动时给 PARK 留接入窗口）。
    // 空闲窗口按墙钟计（rc219）：循环体内的阻塞工作（死亡记录的退出码轮询/
    // tmux 捕获）不再把"5 个 tick"拉长成十几二十秒。
    if (g_sess.empty()) {
      if (now_ms() - empty_since > 5000) { dlog("[daemon] idle exit"); break; }
    } else {
      empty_since = now_ms();
    }

    std::vector<struct pollfd> pfds;
    pfds.push_back({lfd, POLLIN, 0});
    std::vector<std::string> pnames;           // 与 pfds[1..] 对应的会话名
    std::vector<int> pkind;                    // 0=master 1=relay
    for (auto& kv : g_sess) {
      Sess& s = kv.second;
      if (s.master >= 0) {
        short ev = 0;
        if (s.m2r.space() > 0) ev |= POLLIN;
        if (!s.r2m.empty()) ev |= POLLOUT;
        if (ev) { pfds.push_back({s.master, ev, 0}); pnames.push_back(kv.first); pkind.push_back(0); }
      }
      if (s.relay >= 0) {
        short ev = 0;
        if (s.r2m.space() > 0) ev |= POLLIN;
        if (!s.m2r.empty()) ev |= POLLOUT;
        if (ev) { pfds.push_back({s.relay, ev, 0}); pnames.push_back(kv.first); pkind.push_back(1); }
      }
    }
    int pr = poll(pfds.data(), (nfds_t)pfds.size(), 1000);
    if (pr < 0 && errno == EINTR) continue;

    // ① listener：新连接（请求同步处理；RELAY 连接登记后长驻）
    if (pfds[0].revents & POLLIN) {
      int cfd = accept(lfd, nullptr, nullptr);
      if (cfd >= 0) {
        std::string line;
        int in_fd = -1;
        if (!recv_line_fd(cfd, line, &in_fd)) {
          if (in_fd >= 0) close(in_fd);
          close(cfd);
        } else if (line.rfind("RELAY ", 0) == 0) {
          std::string name = line.substr(6);
          while (!name.empty() && (name.back() == '\n' || name.back() == '\r')) name.pop_back();
          auto it = g_sess.find(name);
          if (it != g_sess.end() && it->second.relay < 0) {
            it->second.relay = cfd;
            set_nonblock(cfd);
            dlog("[daemon] relay up: %s fd=%d", name.c_str(), cfd);
            cfd = -1;
          } else {
            dlog("[daemon] relay rejected: %s", name.c_str());
          }
          if (cfd >= 0) close(cfd);
        } else if (line.rfind("PARK ", 0) == 0) {
          char name[64];
          long child = 0, pgid = 0;
          int cols = 0, rows = 0, hist_lines = 0;
          sscanf(line.c_str(), "PARK %63s %d %d %ld %ld %d",
                 name, &cols, &rows, &child, &pgid, &hist_lines);
          bool ok = (in_fd >= 0 && name[0] != '\0');
          if (ok) {
            auto it = g_sess.find(name);
            if (it != g_sess.end()) { sess_cleanup(it->second, it->first); g_sess.erase(it); }
            Sess s;
            s.master = in_fd; set_nonblock(in_fd); in_fd = -1;
            s.child = (pid_t)child; s.pgid = (pid_t)pgid;
            s.cols = cols > 0 ? cols : 53; s.rows = rows > 0 ? rows : 17;
            g_sess[name] = s;
            // tmux：起 cols x rows 空 pane 跑中继；history-limit 对新建窗口就位设置
            // （注意：单独 start-server 后 server 无会话会立即退出，set -g 落空）。
            char cmd[512];
            snprintf(cmd, sizeof cmd, "tmux -L cardterm new -d -s %s -x %d -y %d %s 2>/dev/null",
                     shell_quote(name).c_str(), s.cols, s.rows,
                     shell_quote(g_exe + " --bgtask-relay " + g_sock_path + " " + name).c_str());
            if (run_sh(cmd) != 0) {
              // tmux 起不来（未安装等）：拒绝，调用方保留窗口
              dlog("[daemon] PARK tmux spawn failed: %s", name);
              sess_cleanup(g_sess[name], name);
              g_sess.erase(name);
              ok = false;
            } else {
              snprintf(cmd, sizeof cmd,
                       "tmux -L cardterm set -w -t %s history-limit %d 2>/dev/null",
                       shell_quote(name).c_str(), hist_lines > 0 ? hist_lines : 5000);
              run_sh(cmd);
              dlog("[daemon] PARK %s master=%d child=%ld relay_pending",
                   name, s.master, (long)child);
            }
          }
          send_line_fd(cfd, ok ? "OK\n" : "ERR tmux spawn failed\n", -1);
          if (in_fd >= 0) close(in_fd);
          close(cfd);
        } else if (line.rfind("TAKEOVER ", 0) == 0) {
          char name[64];
          long cap = 262144;
          sscanf(line.c_str(), "TAKEOVER %63s %ld", name, &cap);
          if (cap < 1024) cap = 1024;
          if (cap > (long)(4u << 20)) cap = 4 << 20;
          auto it = g_sess.find(name);
          if (it == g_sess.end()) {
            dlog("[daemon] TAKEOVER miss: %s", name);
            send_line_fd(cfd, "ERR no such session\n", -1);
          } else {
            const int mfd = it->second.master;
            const int rfd = it->second.relay;
            const long ch = (long)it->second.child, pg = (long)it->second.pgid;
            const int cols = it->second.cols, rows = it->second.rows;
            // 防御：程序已退出但泵还没来得及清理的会话不交付——否则附着方会
            // 拿到一个立即 EOF 的 master（闪现 Attached 后马上关窗退出）。
            struct pollfd pfd;
            pfd.fd = mfd; pfd.events = POLLIN | POLLHUP | POLLERR; pfd.revents = 0;
            if (poll(&pfd, 1, 0) > 0 && (pfd.revents & (POLLHUP | POLLERR))) {
              dlog("[daemon] TAKEOVER %s: master hung up, cleaning", name);
              if (rfd >= 0) close(rfd);
              tmux_kill_session(name);
              g_sess.erase(it);
              close(mfd);
              send_line_fd(cfd, "ERR session dead\n", -1);
              close(cfd);
              continue;
            }
            std::string hist = tmux_capture(name, (size_t)cap);   // 先取历史再杀会话
            dlog("[daemon] TAKEOVER %s master=%d child=%ld hist=%zu", name, mfd, ch, hist.size());
            if (rfd >= 0) close(rfd);
            tmux_kill_session(name);                               // “释放该复用器”
            g_sess.erase(it);
            char hdr[96];
            snprintf(hdr, sizeof hdr, "OK %ld %ld %d %d %zu\n",
                     ch, pg, cols, rows, hist.size());
            if (!send_line_fd(cfd, hdr, mfd)) {                    // fd 随头行交给调用方
              close(mfd);
              close(cfd);
              continue;
            }
            close(mfd);                                            // 自己这份副本随后关
            if (!hist.empty()) {
              size_t off = 0;
              while (off < hist.size()) {
                ssize_t w = write(cfd, hist.data() + off, hist.size() - off);
                if (w < 0 && errno == EINTR) continue;
                if (w <= 0) break;
                off += (size_t)w;
              }
            }
            close(cfd);
          }
        } else if (line.rfind("LIST", 0) == 0) {
          std::string out;
          for (auto& kv : g_sess) { out += kv.first; out += "\n"; }
          out += "\n";
          size_t off = 0;
          while (off < out.size()) {
            ssize_t w = write(cfd, out.data() + off, out.size() - off);
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) break;
            off += (size_t)w;
          }
          close(cfd);
        } else {
          close(cfd);
        }
      }
    }

    // ② 数据泵：master ⇄ relay（有界队列，满则反压）
    std::vector<std::string> dead;
    for (size_t i = 1; i < pfds.size(); i++) {
      const std::string& nm = pnames[i - 1];
      auto it = g_sess.find(nm);
      if (it == g_sess.end()) continue;
      Sess& s = it->second;
      const int fd = (pkind[i - 1] == 0) ? s.master : s.relay;
      if (fd < 0) continue;
      short rev = pfds[i].revents;
      // POLLHUP/POLLERR 必须处理：程序退出后 master 挂断是电平触发事件，
      // 不消费它会让 poll 立即返回 → 忙等空转（实测 bgtaskd 80%+ CPU），
      // 且永远走不到 read 的 EOF 判定 → 会话永不自动清理。
      if (rev & (POLLHUP | POLLERR)) {
        if (pkind[i - 1] == 0) {
          dead.push_back(nm);                 // 程序侧挂断：清理会话
          continue;
        }
        close(s.relay); s.relay = -1;         // 中继侧挂断：丢弃积压
        s.m2r.len = 0; s.m2r.off = 0;
        continue;
      }
      if (rev & POLLIN) {
        Q& q = (pkind[i - 1] == 0) ? s.m2r : s.r2m;
        ssize_t r = read(fd, q.wr(), q.space());
        if (r > 0) q.produce((size_t)r);
        else if (r == 0 || (r < 0 && errno != EAGAIN && errno != EINTR)) {
          if (pkind[i - 1] == 0) dead.push_back(nm);           // 程序退出（master EOF）
          else { close(s.relay); s.relay = -1; s.m2r.len = 0; s.m2r.off = 0; }  // 中继消失
        }
      }
      if (rev & POLLOUT) {
        Q& q = (pkind[i - 1] == 0) ? s.r2m : s.m2r;
        ssize_t w = write(fd, q.rd(), q.avail());
        if (w > 0) q.consume((size_t)w);
        else if (w < 0 && errno != EAGAIN && errno != EINTR) {
          if (pkind[i - 1] == 0) dead.push_back(nm);
          else { close(s.relay); s.relay = -1; s.m2r.len = 0; s.m2r.off = 0; }
        }
      }
    }
    for (const auto& nm : dead) {
      auto it = g_sess.find(nm);
      if (it != g_sess.end()) {
        dlog("[daemon] master EOF/err: %s (child=%ld)", nm.c_str(), (long)it->second.child);
        write_dead_record(nm, it->second.child, 256 * 1024);   // Q6：死亡记录存档
        sess_cleanup(it->second, nm);
        g_sess.erase(it);
      }
    }
  }
  close(lfd);
  unlink(g_sock_path.c_str());
  return 0;
}

// ── tmux pane 内的中继：socket ⇄ stdin/stdout ────────────────────

int run_relay(const char* sock, const char* name)
{
  if (sock == nullptr || name == nullptr) return 1;
  signal(SIGPIPE, SIG_IGN);
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return 1;
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof addr);
  addr.sun_family = AF_UNIX;
  snprintf(addr.sun_path, sizeof addr.sun_path, "%s", sock);
  if (connect(fd, (struct sockaddr*)&addr, sizeof addr) != 0) { close(fd); return 1; }

  char line[128];
  snprintf(line, sizeof line, "RELAY %s\n", name);
  if (send_line_fd(fd, line, -1) == false) { close(fd); return 1; }

  struct pollfd pf[2];
  for (;;) {
    pf[0].fd = STDIN_FILENO; pf[0].events = POLLIN; pf[0].revents = 0;
    pf[1].fd = fd;           pf[1].events = POLLIN; pf[1].revents = 0;
    if (poll(pf, 2, -1) < 0 && errno == EINTR) continue;
    char buf[8192];
    if (pf[0].revents & (POLLIN | POLLHUP | POLLERR)) {
      ssize_t r = read(STDIN_FILENO, buf, sizeof buf);
      if (r > 0) {
        ssize_t off = 0;
        while (off < r) {
          ssize_t w = write(fd, buf + off, (size_t)(r - off));
          if (w < 0 && errno == EINTR) continue;
          if (w <= 0) return 0;
          off += w;
        }
      } else if (r == 0 || (r < 0 && errno != EAGAIN && errno != EINTR)) return 0;
    }
    if (pf[1].revents & (POLLIN | POLLHUP | POLLERR)) {
      ssize_t r = read(fd, buf, sizeof buf);
      if (r > 0) {
        ssize_t off = 0;
        while (off < r) {
          ssize_t w = write(STDOUT_FILENO, buf + off, (size_t)(r - off));
          if (w < 0 && errno == EINTR) continue;
          if (w <= 0) return 0;
          off += w;
        }
      } else if (r == 0 || (r < 0 && errno != EAGAIN && errno != EINTR)) return 0;
    }
  }
}

}  // namespace bgtask
