// session.h — 窗口管理器 + 伪终端会话（0.3.0 多窗口）
//
//   - 12 个终端窗口（Ctrl+Alt+F1~F12 切换），仿 tty 多控制台语义
//   - 每窗口独立 term::State（屏缓冲/回滚/解析状态），后台窗口输出持续解析
//   - 无参数启动 = 窗口 1 显示极简启动器：`>` 后输入命令进入该命令的会话
//     （help 显示帮助；命令退出即关窗——无其它开窗时退出 cardterm）
//   - 窗口退出规则：当前窗退出→无开窗则退出 cardterm，否则跳最小开窗；
//     后台窗退出→静默关闭
//   - 长按 Esc = 关闭全部窗口并退出 cardterm（等效图形终端点按关闭按钮）
//   - PTY：openpty + setsid + TIOCSCTTY，53×17，TERM=xterm-256color
#pragma once

#include <string>
#include <vector>

#include "terminal.h"

namespace session {

// 启动窗口管理器。first_cmd 非空时窗口 1 直接运行该命令（cardterm <cmd>），
// 否则窗口 1 显示启动器。返回退出码（取退出窗口的程序退出码）。
// 调用前需已 kbd::open()，调用方负责 kbd::close()。
int run(const std::vector<std::string>& first_cmd, const term::Options& opt);

}  // namespace session
