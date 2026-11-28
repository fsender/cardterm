// uinject2.c — uinput 键盘注入器 v2（带全程日志，cardterm 测试用）
// 指令：W:<file> 等文件出现 | S:<ms> 睡眠 | H:<code>:<ms> 长按 |
//       C:<mod>:<key> 组合键（mod 按住→敲击 key→松开） | <code> 敲击
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static FILE* lg;
static int ufd;

static void logmsg(const char* m) { fprintf(lg, "%s\n", m); fflush(lg); }

static void msleep(int ms)
{
  struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
  nanosleep(&ts, NULL);
}

static void emit(int type, int code, int val)
{
  struct input_event e;
  memset(&e, 0, sizeof e);
  e.type = type; e.code = (unsigned short)code; e.value = val;
  gettimeofday(&e.time, NULL);
  if (write(ufd, &e, sizeof e) != (ssize_t)sizeof e)
    fprintf(lg, "emit write fail: %s\n", strerror(errno));
}

static void tap(int code)
{
  emit(EV_KEY, code, 1); emit(EV_SYN, SYN_REPORT, 0);
  msleep(30);
  emit(EV_KEY, code, 0); emit(EV_SYN, SYN_REPORT, 0);
  msleep(30);
}

int main(int argc, char** argv)
{
  lg = fopen("/tmp/inj.log", "w");
  if (!lg) lg = stderr;
  logmsg("uinject2: start");

  ufd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
  if (ufd < 0) { fprintf(lg, "open uinput: %s\n", strerror(errno)); return 1; }
  logmsg("uinput opened");

  if (ioctl(ufd, UI_SET_EVBIT, EV_KEY) < 0) { logmsg("SET_EVBIT fail"); return 1; }
  // 启用全键码范围（含 Fn=KEY_DVD 389 / Sym=KEY_CD 383 / KEY_FN 464 等 >255 的
  // 功能键码；旧版只开 1..255，注入 Fn 层组合键会被内核静默丢弃）
  for (int c = 1; c < KEY_MAX; c++) ioctl(ufd, UI_SET_KEYBIT, c);

  struct uinput_user_dev uidev;
  memset(&uidev, 0, sizeof uidev);
  snprintf(uidev.name, UINPUT_MAX_NAME_SIZE, "cardterm-test-kbd");
  uidev.id.bustype = BUS_USB;
  uidev.id.vendor  = 0x1234;
  uidev.id.product = 0x5678;
  fprintf(lg, "uidev size=%zu\n", sizeof uidev);
  if (write(ufd, &uidev, sizeof uidev) != (ssize_t)sizeof uidev)
    fprintf(lg, "uidev write fail: %s\n", strerror(errno));
  if (ioctl(ufd, UI_DEV_CREATE) < 0) { fprintf(lg, "DEV_CREATE fail: %s\n", strerror(errno)); return 1; }
  logmsg("device created");

  msleep(400);

  for (int i = 1; i < argc; i++) {
    const char* a = argv[i];
    fprintf(lg, "arg[%d]=%s\n", i, a); fflush(lg);
    if (a[0] == 'W' && a[1] == ':') {
      // W:<file> 等文件出现（至多 300s：分步黑盒测试的人工核帧间隔可能较长）
      for (int t = 0; t < 30000 && access(a + 2, F_OK) != 0; t++) msleep(10);
      fprintf(lg, "W done (t file=%s)\n", access(a + 2, F_OK) == 0 ? "yes" : "no");
    } else if (a[0] == 'S' && a[1] == ':') {
      int ms = atoi(a + 2);
      fprintf(lg, "S %dms\n", ms);
      msleep(ms);
    } else if (a[0] == 'M' && a[1] == ':') {
      // M:<file>:<shift>:<sym>:<fn> 写修饰状态接缝文件（legacy 语义 0-3）
      char path[128] = {0};
      unsigned sv = 0, yv = 0, nv = 0;
      if (sscanf(a + 2, "%127[^:]:%u:%u:%u", path, &sv, &yv, &nv) == 4) {
        FILE* f = fopen(path, "w");
        if (f) { fprintf(f, "%u %u %u\n", sv, yv, nv); fclose(f); }
        fprintf(lg, "M %s = %u %u %u\n", path, sv, yv, nv);
      } else {
        fprintf(lg, "M parse fail: %s\n", a);
      }
      msleep(150);   // 给被测程序观察状态变化的时间
    } else if (a[0] == 'H' && a[1] == ':') {
      int code = atoi(a + 2);
      const char* col = strchr(a + 2, ':');
      int ms = col ? atoi(col + 1) : 500;
      fprintf(lg, "H code=%d ms=%d\n", code, ms);
      emit(EV_KEY, code, 1); emit(EV_SYN, SYN_REPORT, 0);
      msleep(ms);
      emit(EV_KEY, code, 0); emit(EV_SYN, SYN_REPORT, 0);
      msleep(30);
    } else if (a[0] == 'C' && a[1] == ':') {
      // C:<mod>[,<mod>…]:<key> 组合键（全部 mod 按住→敲击 key→依次松开）
      int mods[4] = {0, 0, 0, 0};
      int nmods = 0;
      const char* p = a + 2;
      while (nmods < 4) {
        char* end = NULL;
        long v = strtol(p, &end, 10);
        if (end == p) break;
        mods[nmods++] = (int)v;
        if (*end == ',') { p = end + 1; continue; }
        p = end;
        break;
      }
      int key = (*p == ':') ? atoi(p + 1) : 0;
      fprintf(lg, "C mods=%d key=%d\n", nmods, key);
      for (int i = 0; i < nmods; i++) emit(EV_KEY, mods[i], 1);
      emit(EV_SYN, SYN_REPORT, 0);
      msleep(40);
      tap(key);
      for (int i = nmods - 1; i >= 0; i--) emit(EV_KEY, mods[i], 0);
      emit(EV_SYN, SYN_REPORT, 0);
      msleep(30);
    } else {
      int code = atoi(a);
      fprintf(lg, "tap %d\n", code);
      tap(code);
    }
    fflush(lg);
  }

  logmsg("all args done, destroying");
  ioctl(ufd, UI_DEV_DESTROY);
  close(ufd);
  logmsg("exit ok");
  fclose(lg);
  return 0;
}
