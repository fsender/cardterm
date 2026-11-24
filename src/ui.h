// ui.h — UI 契约（界面代码写在 ui.cpp）
//
// 与 M5Stack / ESP32 的 LovyanGFX 代码风格一致，便于移植：
//   - 全局绘图对象 `tft`（已在 main.cpp 创建，直接使用）
//   - 默认白字黑底、光标 (0,0)
//   - TFT_BLACK / TFT_WHITE / delay() 已做 Arduino 风格兼容
// 其余一切（屏幕独占、设备初始化、显示时长、退出还原）都在 main.cpp。
#pragma once

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <LGFX_AUTODETECT.hpp>
#include <string>
#include <vector>

// 全局绘图对象（定义于 main.cpp）
extern LGFX tft;

// ── M5Stack / Arduino 风格兼容宏（可按需扩充 TFT_RED 等）──
#ifndef TFT_BLACK
#define TFT_BLACK 0x000000u
#define TFT_WHITE 0xFFFFFFu
#endif

// UI 入口：在 ui.cpp 中实现你的界面代码。
// args 为命令行参数（不含程序名）：非空时在 PTY 中运行该命令（交互式终端）。
int ui(const std::vector<std::string>& args);
