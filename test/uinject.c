// uinject.c — 极简 uinput 键盘注入器（cardterm 键盘驱动测试用）
//
// 用法（板上，root）：
//   ./uinject W /tmp/kbin.ready S:<ms> H:<code>:<ms> <code> ...
//     W:<file>   等待文件出现后才开始注入（对齐被测程序启动）
//     S:<ms>     睡眠
//     H:<c>:<ms> 按住某键 ms 毫秒后释放
//     <code>     敲击一次某键
// 例：./uinject W:/tmp/kbin.ready 35 23 57 59 110 111 109 99 S:1200 H:1:1300
//     → 输入 "hi F1 Ins Del PgDn PrSc"，再长按 Esc 1.3s
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int ufd;

static void emit(int type, int code, int val)
{
  struct input_event e;
  memset(&e, 0, sizeof e);
  e.type = type; e.code = code; e.value = val;
  gettimeofday(&e.time, NULL);
  write(ufd, &e, sizeof e);
}

static void tap(int code)
{
  emit(EV_KEY, code, 1); emit(EV_SYN, SYN_REPORT, 0);
  usleep(30000);
  emit(EV_KEY, code, 0); emit(EV_SYN, SYN_REPORT, 0);
  usleep(30000);
}

int main(int argc, char** argv)
{
  ufd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
  if (ufd < 0) { perror("uinput"); return 1; }

  ioctl(ufd, UI_SET_EVBIT, EV_KEY);
  for (int c = 1; c < 256; c++) ioctl(ufd, UI_SET_KEYBIT, c);

  struct uinput_user_dev uidev;
  memset(&uidev, 0, sizeof uidev);
  snprintf(uidev.name, UINPUT_MAX_NAME_SIZE, "cardterm-test-kbd");
  uidev.id.bustype = BUS_USB;
  uidev.id.vendor  = 0x1234;
  uidev.id.product = 0x5678;
  write(ufd, &uidev, sizeof uidev);
  ioctl(ufd, UI_DEV_CREATE);
  usleep(400000);                          // 等 udev 建好 /dev/input/eventN

  for (int i = 1; i < argc; i++) {
    const char* a = argv[i];
    if (a[0] == 'W' && a[1] == ':') {      // 等待就绪文件
      for (int t = 0; t < 1000 && access(a + 2, F_OK) != 0; t++) usleep(10000);
    } else if (a[0] == 'S' && a[1] == ':') {
      usleep(atoi(a + 2) * 1000);
    } else if (a[0] == 'H' && a[1] == ':') {
      int code = atoi(a + 2);
      const char* col = strchr(a + 2, ':');
      int ms = col ? atoi(col + 1) : 500;
      emit(EV_KEY, code, 1); emit(EV_SYN, SYN_REPORT, 0);
      usleep(ms * 1000);
      emit(EV_KEY, code, 0); emit(EV_SYN, SYN_REPORT, 0);
      usleep(30000);
    } else {
      tap(atoi(a));
    }
  }

  usleep(300000);
  ioctl(ufd, UI_DEV_DESTROY);
  close(ufd);
  return 0;
}
