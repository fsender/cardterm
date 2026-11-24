// exclusive_display.cpp — see exclusive_display.h for the contract.
#include "exclusive_display.h"

#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/kd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/file.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <dirent.h>
#include <signal.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace hw {

namespace {

int  g_lock_fd   = -1;
int  g_tty_fd    = -1;
int  g_fb_fd     = -1;
void* g_fb_map   = MAP_FAILED;
size_t g_fb_len  = 0;
std::vector<uint8_t> g_fb_backup;
std::vector<int> g_kbd_fds;
std::vector<pid_t> g_frozen_pids;
bool g_active = false;

#define BIT_PER_LONG   (sizeof(unsigned long) * 8)
#define test_bit(bit, array) ((array[(bit) / BIT_PER_LONG] >> ((bit) % BIT_PER_LONG)) & 1)

pid_t g_watchdog = -1;
volatile sig_atomic_t g_exit_req = 0;   // 信号处理只置标志，由主循环安全退出

// 进程存活判据：僵尸（Z）视为已死亡。
// 背景：cardterm 会 SIGSTOP 父进程 APPLaunch；自身崩溃后若父进程不回 waitpid，
// 本进程会成为僵尸，而 kill(pid,0) 对僵尸仍返回 0 → 看门狗永不触发（设备永久冻结）。
bool pid_alive(pid_t p)
{
  if (p <= 0) return false;
  char path[64];
  snprintf(path, sizeof path, "/proc/%d/stat", (int)p);
  FILE* f = fopen(path, "r");
  if (!f) return false;
  char buf[256] = {0};
  size_t n = fread(buf, 1, sizeof buf - 1, f);
  fclose(f);
  if (n == 0) return false;
  // 格式: pid (comm) state ...；comm 可能含空格/括号 → 从最后一个 ')' 后取
  char* rp = strrchr(buf, ')');
  if (rp == nullptr || rp[1] == '\0') return true;
  char st = rp[2];
  return st != 'Z' && st != 'X' && st != 'x';
}

bool lock_display()
{
  g_lock_fd = open("/run/lock/hw-display.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0666);
  if (g_lock_fd < 0) g_lock_fd = open("/tmp/hw-display.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0666);
  if (g_lock_fd < 0) return true;             // no lock file: proceed best-effort
  return flock(g_lock_fd, LOCK_EX | LOCK_NB) == 0;
}

void vt_graphics()
{
  g_tty_fd = open("/dev/tty1", O_RDWR | O_CLOEXEC);
  if (g_tty_fd >= 0 && ioctl(g_tty_fd, KDSETMODE, KD_GRAPHICS) != 0)
  {
    close(g_tty_fd);
    g_tty_fd = -1;
  }
}

void vt_text()
{
  if (g_tty_fd >= 0)
  {
    ioctl(g_tty_fd, KDSETMODE, KD_TEXT);
    close(g_tty_fd);
    g_tty_fd = -1;
  }
}

bool fb_backup()
{
  g_fb_fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
  if (g_fb_fd < 0) { perror("open /dev/fb0"); return false; }

  fb_fix_screeninfo fix{};
  fb_var_screeninfo var{};
  if (ioctl(g_fb_fd, FBIOGET_FSCREENINFO, &fix) != 0 ||
      ioctl(g_fb_fd, FBIOGET_VSCREENINFO, &var) != 0)
  {
    perror("FBIOGET");
    return false;
  }
  g_fb_len = fix.smem_len;
  g_fb_map = mmap(nullptr, g_fb_len, PROT_READ | PROT_WRITE, MAP_SHARED, g_fb_fd, 0);
  if (g_fb_map == MAP_FAILED) { perror("mmap fb"); return false; }

  g_fb_backup.resize(g_fb_len);
  memcpy(g_fb_backup.data(), g_fb_map, g_fb_len);
  fprintf(stderr, "[exclusive] fb0: %ux%u %u bpp, %zu bytes mapped\n",
          var.xres, var.yres, var.bits_per_pixel, g_fb_len);
  if (var.xres != 320 || var.yres != 170 || var.bits_per_pixel != 16)
    fprintf(stderr, "[exclusive] WARN: fb geometry %ux%u/%ubpp != expected 320x170/16bpp\n",
            var.xres, var.yres, var.bits_per_pixel);
  return true;
}

void fb_restore()
{
  if (g_fb_map != MAP_FAILED && !g_fb_backup.empty())
    memcpy(g_fb_map, g_fb_backup.data(), g_fb_len);
}

void fb_close()
{
  if (g_fb_map != MAP_FAILED) { munmap(g_fb_map, g_fb_len); g_fb_map = MAP_FAILED; }
  if (g_fb_fd >= 0) { close(g_fb_fd); g_fb_fd = -1; }
}

// Grab every keyboard-like evdev (has letter keys) so nothing else sees input.
void grab_keyboards()
{
  DIR* d = opendir("/dev/input");
  if (!d) return;
  while (dirent* e = readdir(d))
  {
    if (strncmp(e->d_name, "event", 5) != 0) continue;
    char path[128];
    snprintf(path, sizeof path, "/dev/input/%s", e->d_name);
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) continue;

    unsigned long evbits[(EV_MAX / BIT_PER_LONG) + 1] = {0};
    unsigned long keybits[(KEY_MAX / BIT_PER_LONG) + 1] = {0};
    if (ioctl(fd, EVIOCGBIT(0, sizeof evbits), evbits) == 0 &&
        test_bit(EV_KEY, evbits) &&
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keybits), keybits) == 0 &&
        test_bit(KEY_A, keybits) && test_bit(KEY_Z, keybits))
    {
      if (ioctl(fd, EVIOCGRAB, 1) == 0)
      {
        fprintf(stderr, "[exclusive] keyboard grabbed: %s\n", path);
        g_kbd_fds.push_back(fd);
        continue;
      }
    }
    close(fd);
  }
  closedir(d);
}

void ungrab_keyboards()
{
  for (int fd : g_kbd_fds) { ioctl(fd, EVIOCGRAB, 0); close(fd); }
  g_kbd_fds.clear();
}

// Find running M5CardputerZero-APPLaunch processes (matched by cmdline;
// /proc/PID/comm is truncated to 15 chars and cannot match the full name).
std::vector<pid_t> find_applaunch_pids()
{
  std::vector<pid_t> out;
  DIR* d = opendir("/proc");
  if (!d) return out;
  while (dirent* e = readdir(d))
  {
    if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
    // 以可执行文件路径为准（cmdline 子串会误伤 grep/编辑器等进程）
    char path[64];
    snprintf(path, sizeof path, "/proc/%s/exe", e->d_name);
    char exe[512] = {0};
    ssize_t en = readlink(path, exe, sizeof exe - 1);
    bool match = (en > 0 && strstr(exe, "M5CardputerZero-APPLaunch") != nullptr);
    if (!match) {                            // 兜底：exe 不可读时再看 cmdline
      snprintf(path, sizeof path, "/proc/%s/cmdline", e->d_name);
      FILE* f = fopen(path, "rb");
      if (!f) continue;
      char buf[512] = {0};
      size_t n = fread(buf, 1, sizeof buf - 1, f);
      fclose(f);
      match = (n > 0 && strstr(buf, "M5CardputerZero-APPLaunch") != nullptr);
    }
    if (match) out.push_back((pid_t)atoi(e->d_name));
  }
  closedir(d);
  return out;
}

// Freeze (never kill) the launcher so it keeps its exact runtime state and
// resumes untouched after we leave — "exit returns it exactly as it was".
// Always frozen, even when we were launched from it: its Terminal=true pages
// keep redrawing the screen over our output unless stopped.
void freeze_applaunch()
{
  for (pid_t p : find_applaunch_pids())
  {
    if (kill(p, SIGSTOP) == 0)
    {
      g_frozen_pids.push_back(p);
      fprintf(stderr, "[exclusive] APPLaunch frozen (SIGSTOP, pid %d)\n", (int)p);
    }
  }
  if (!g_frozen_pids.empty()) usleep(100000);   // let its last frame settle
}

void thaw_applaunch()
{
  for (pid_t p : g_frozen_pids)
  {
    if (kill(p, SIGCONT) == 0)
      fprintf(stderr, "[exclusive] APPLaunch resumed (SIGCONT, pid %d)\n", (int)p);
  }
  g_frozen_pids.clear();
}

// Safety net: if this process dies uncleanly (SIGKILL, crash), the child
// restores the screen and resumes the launcher instead of leaving them frozen.
void start_watchdog()
{
  pid_t parent = getpid();
  pid_t pid = fork();
  if (pid == 0)
  {
    while (pid_alive(parent)) { sleep(1); }
    fb_restore();
    fb_close();
    thaw_applaunch();
    vt_text();
    _exit(0);
  }
  g_watchdog = pid;
}

void stop_watchdog()
{
  if (g_watchdog > 0)
  {
    kill(g_watchdog, SIGKILL);
    waitpid(g_watchdog, nullptr, 0);
    g_watchdog = -1;
  }
}

void on_signal(int sig)
{
  (void)sig;
  g_exit_req = 1;        // async-signal-safe：只置标志，清理由主循环/atexit 完成
}

}  // namespace

bool acquire(bool grab_keyboard)
{
  if (g_active) return true;

  if (!lock_display())
  {
    fprintf(stderr, "[exclusive] another display session is active (lock busy)\n");
    return false;
  }

  // 失败回滚标记要先置位（否则下面的失败分支 release() 会早退）。
  g_active = true;
  atexit(release);
  signal(SIGINT,  on_signal);
  signal(SIGTERM, on_signal);
  signal(SIGHUP,  on_signal);
  signal(SIGQUIT, on_signal);

  vt_graphics();
  freeze_applaunch();
  if (!fb_backup()) { release(); return false; }   // release() 会真正回滚
  // 看门狗必须在**资源齐备之后** fork：它靠继承的内存快照恢复
  // （fb 备份 / 冻结 pid 表 / tty fd）。提前 fork 会继承空快照 → 恢复体全空转。
  start_watchdog();
  if (grab_keyboard) grab_keyboards();

  fprintf(stderr, "[exclusive] display + input acquired\n");
  return true;
}

bool exit_requested() { return g_exit_req != 0; }

void release()
{
  if (!g_active) return;
  g_active = false;

  ungrab_keyboards();
  fb_restore();                    // 先还原画面
  fb_close();
  vt_text();                       // 再切回文本模式（fbcon 复位）
  thaw_applaunch();                // 最后才让启动器继续绘制，避免其首帧被 fbcon 覆盖
  if (g_lock_fd >= 0) { flock(g_lock_fd, LOCK_UN); close(g_lock_fd); g_lock_fd = -1; }
  stop_watchdog();                 // 安全网最后停：清理中途被杀仍由 watchdog 兜底
  fprintf(stderr, "[exclusive] released, everything restored\n");
}

}  // namespace hw
