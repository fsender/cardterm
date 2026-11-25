// applaunch_env.h — APPLaunch 运行时约定接入（规范遵循层）
//
// 商店规范要求外部应用不得写死设备节点、不得与启动器争夺生命周期管理：
//
//   1. 设备路径由 APPLaunch 注入的环境变量给出（缺省回退板载默认值）：
//        APPLAUNCH_LINUX_FBDEV_DEVICE     framebuffer 设备路径
//        APPLAUNCH_LINUX_KEYBOARD_DEVICE  键盘 evdev 节点
//        APPLAUNCH_LINUX_KEYBOARD_MAP     loadkeys 格式键映射文件
//
//   2. 生命周期由 APPLaunch 托管：Terminal=false 启动外部应用时（.desktop
//      Exec 走 cp0_process_exec_blocking），启动器自行暂停 LVGL 定时器与键盘
//      线程（协作让路），并保留一个**共享（非 EVIOCGRAB）evdev fd** 监视 ESC：
//      长按 ~3s 对应用进程组 SIGTERM、再 3s SIGKILL —— 这是"长按 ESC 也能退出
//      卡死程序"的兜底链。
//
//      因此托管模式下应用**不得**：
//        - SIGSTOP 启动器（冻结其 ESC 监视线程 → 兜底失效）；
//        - EVIOCGRAB 启动器监视的键盘设备（独占后其共享 fd 收不到 ESC）。
//      正常退出仍走应用自身逻辑（cardterm 为长按 ESC 2s），SIGTERM 兜底只在
//      应用卡死时由启动器触发，两层互不冲突。
//
// 本头文件只做环境解析与模式判定，无全局状态、无副作用。
#pragma once

#include <cstdlib>
#include <unistd.h>

namespace applaunch_env {

// APPLaunch 注入的环境变量是否存在（= 由启动器拉起）。
inline bool injected()
{
  return std::getenv("APPLAUNCH_LINUX_FBDEV_DEVICE") != nullptr ||
         std::getenv("APPLAUNCH_LINUX_KEYBOARD_DEVICE") != nullptr ||
         std::getenv("APPLAUNCH_LINUX_KEYBOARD_MAP") != nullptr;
}

// 协作托管模式判定：由 APPLaunch 的 Terminal=false 外部应用路径拉起。
//   - 注入环境存在，且标准输入不是终端。
//     Terminal=false：子进程继承启动器（systemd --user 服务）的非 tty stdio；
//     Terminal=true（在启动器内 PTY 终端页手动执行）：stdin 为 PTY，此时启动器
//     不暂停自身绘制、也没有 SIGTERM 兜底，仍走防御模式（SIGSTOP + EVIOCGRAB）。
//   - SSH/控制台直跑：注入环境不存在，同样走防御模式，行为与历史版本一致。
inline bool coop_managed()
{
  return injected() && !isatty(STDIN_FILENO);
}

// framebuffer 设备路径（不写死）：APPLaunch 注入值优先，回退板载 /dev/fb0。
// 返回静态缓冲拷贝：LGFX 全局对象在 main() 之前构造、Panel_fb 保存裸指针，
// 不依赖 getenv 返回指针的生命周期。
inline const char* fbdev_path()
{
  static char buf[128];
  const char* p = std::getenv("APPLAUNCH_LINUX_FBDEV_DEVICE");
  if (p && *p) {
    size_t n = 0;
    while (p[n] && n + 1 < sizeof buf) { buf[n] = p[n]; n++; }
    buf[n] = '\0';
    return buf;
  }
  return "/dev/fb0";
}

// 键盘 evdev 节点提示（主设备查找链的一环）；nullptr = 未注入。
inline const char* keyboard_device_hint()
{
  const char* p = std::getenv("APPLAUNCH_LINUX_KEYBOARD_DEVICE");
  return (p && *p) ? p : nullptr;
}

// 键映射文件路径；nullptr = 未注入（使用内置符号表）。
inline const char* keyboard_map_path()
{
  const char* p = std::getenv("APPLAUNCH_LINUX_KEYBOARD_MAP");
  return (p && *p) ? p : nullptr;
}

}  // namespace applaunch_env
