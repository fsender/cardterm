// bgtask_main.cpp — cardterm-bgtask：后台会话管理专用小二进制（1.3.0-rc216 / 审计 M1）
//
// 动机：后台会话的守护进程（--bgtaskd）与 tmux 中继（--bgtask-relay）此前都以
// cardterm 主二进制 fork+exec 常驻（12 个剥离会话 ≈ 12 个完整 cardterm 驻留，
// ≈10MB）。本二进制只链 bgtask.cpp（零 LGFX/字体/IME 依赖），管理逻辑、协议、
// 行为与原实现完全一致；由用户级 systemd（cardterm-bgtask.service，Restart=no）
// 总体管理，或由 cardterm 按需直接拉起（systemd 用户会话不可用时回退）。
// 后台会话死掉不重启（死亡记录降级照旧）。
#include <cstring>

#include "bgtask.h"

int main(int argc, char** argv)
{
  if (argc >= 2 && strcmp(argv[1], "--bgtaskd") == 0) return bgtask::run_daemon();
  if (argc >= 4 && strcmp(argv[1], "--bgtask-relay") == 0)
    return bgtask::run_relay(argv[2], argv[3]);
  return 2;
}
