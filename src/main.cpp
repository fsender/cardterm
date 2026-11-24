// main.cpp — 非 UI 底座：显示初始化、独占会话、时序与退出还原
//
// 界面代码请写在 ui.cpp（契约见 ui.h）。
// 本文件负责其余一切：
//   - 创建绘图对象 tft（板载 /dev/fb0，320x170 ST7789v3）
//   - 独占显示会话（冻结启动器、静默控制台、独占键盘、画面快照/还原）
//   - 显示时长与退出收尾
#define LGFX_USE_V1
#include "ui.h"
#include "exclusive_display.h"

#include <string>
#include <vector>

// 板载 1.9" ST7789v3 面板（UI 经 ui.h 的 extern tft 使用）
LGFX tft(320, 170, "/dev/fb0");

static const unsigned long SHOW_MS = 5000;   // UI 显示时长（取消下方注释即恢复保持）

int main(int argc, char** argv)
{
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
