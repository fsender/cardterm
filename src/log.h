// log.h — 防阻塞日志（1.3.0-rc206）：写满即丢，绝不阻塞或崩溃
//
// 机制：日志走 stderr 的独立 dup fd（O_NONBLOCK + FD_CLOEXEC）——
//   · 非阻塞：目标（SSH pty 等）缓冲塞满时 write 立即返回 EAGAIN，日志丢弃；
//   · CLOEXEC：子进程不继承该 fd，它们继承的 stderr 仍是正常阻塞语义；
//   · stdio 不参与（无缓冲、无错误标志连锁），单次 write 失败只丢这一条。
#pragma once

#include <fcntl.h>
#include <unistd.h>

#include <cstdarg>
#include <cstdio>

inline void logf(const char* fmt, ...)
{
  static int fd = -2;
  if (fd == -2) {
    fd = fcntl(2, F_DUPFD, 3);     // 不用 dup(2)：0/1 被关时 dup 会拿到它们（L11）
    if (fd >= 0) {
      fcntl(fd, F_SETFD, FD_CLOEXEC);
      const int fl = fcntl(fd, F_GETFL, 0);
      if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    }
  }
  if (fd < 0) return;
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  const int n = vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  if (n > 0) {
    const ssize_t r = write(fd, buf, (size_t)(n < 255 ? n : 255));
    (void)r;                       // EAGAIN/短写 = 丢弃该条，不影响运行
  }
}
