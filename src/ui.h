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

namespace term {
// 叠加层绘制目标（半透明合成画布存在时指向画布，否则为屏幕 tft）；
// toast/全屏提示/输入框的形状与文本图元一律经此绘制（terminal 实现）。
// 颜色口径：参数均为 RGB565；画布（LGFX_Sprite，565 格式）用 setRawColor
// 原样写入、读屏不转换（省内存），屏幕路径维持原 tft 调用（设备 565 原样存储）。
struct OvDst {
  lgfx::LGFX_Sprite* cv = nullptr;   // 非空 = 绘制到画布
  int ox = 0, oy = 0;                // 画布原点（屏幕坐标平移为画布 (0,0)）
  void fillRect(int x, int y, int w, int h, uint16_t col);
  void fillCircle(int x, int y, int r, uint16_t col);
  void drawRoundRect(int x, int y, int w, int h, int r, uint16_t col);
  void pushImage(int x, int y, int w, int h, const uint16_t* data);
};
OvDst& ov_dst();
}

// ── M5Stack / Arduino 风格兼容宏（可按需扩充 TFT_RED 等）──
#ifndef TFT_BLACK
#define TFT_BLACK 0x000000u
#define TFT_WHITE 0xFFFFFFu
#endif

// UI 入口：在 ui.cpp 中实现你的界面代码。
// args 为命令行参数（不含程序名）：非空时在 PTY 中运行该命令（交互式终端）。
int ui(const std::vector<std::string>& args);
