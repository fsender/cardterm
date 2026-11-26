// main.cpp — 非 UI 底座：显示初始化、独占会话、时序与退出还原
//
// 界面代码请写在 ui.cpp（契约见 ui.h）。
// 本文件负责其余一切：
//   - 创建绘图对象 tft（fb 设备路径由 APPLaunch 环境变量注入，见 applaunch_env.h）
//   - 独占显示会话（静默控制台、独占键盘、画面快照/还原；托管模式下生命周期
//     交还 APPLaunch，见 exclusive_display.h）
//   - 显示时长与退出收尾
#define LGFX_USE_V1
#include "ui.h"
#include "applaunch_env.h"
#include "bgtask.h"
#include "exclusive_display.h"

#include <cstring>

#include <string>
#include <vector>

// 板载 1.9" ST7789v3 面板（UI 经 ui.h 的 extern tft 使用）。
// fb 设备不写死：APPLAUNCH_LINUX_FBDEV_DEVICE 注入优先，回退板载 /dev/fb0。
LGFX tft(320, 170, applaunch_env::fbdev_path());

static const unsigned long SHOW_MS = 5000;   // UI 显示时长（取消下方注释即恢复保持）

int main(int argc, char** argv)
{
  // 后台会话模式（见 docs/bgtask-design.md §2）：守护进程 / tmux pane 中继，
  // 不初始化显示/键盘/独占会话。
  if (argc >= 2 && strcmp(argv[1], "--bgtaskd") == 0) return bgtask::run_daemon();
  if (argc >= 4 && strcmp(argv[1], "--bgtask-relay") == 0)
    return bgtask::run_relay(argv[2], argv[3]);

  std::vector<std::string> args;
  for (int i = 1; i < argc; i++) args.emplace_back(argv[i]);

  // 键盘独占由 kbd 驱动负责（EVIOCGRAB 具体键盘设备）；
  // 这里不再统一 grab 全部键盘，否则 kbd::open() 拿不到按键事件。
  if (!hw::acquire(/*grab_keyboard=*/false)) { return 1; }

  tft.init();
  tft.setRotation(0);

  int rc = ui(args);       // ← 用户 UI（ui.cpp）；args 非空 = 在 PTY 中运行命令
  //lgfx::delay(SHOW_MS);  // 保持显示（当前按你的改动：注释掉 = 显示后立即退出）

  hw::release();           // 还原画面，启动器/终端回到原状态
  return rc;               // 终端会话退出码（无参演示模式为 0）
}
