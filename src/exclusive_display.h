// exclusive_display.h
//
// Exclusive display & input session for the CardputerZero onboard screen.
// Reusable foundation for full-screen apps (e.g. the planned terminal emulator).
//
// Contract of acquire():
//   1. cooperative single-instance lock (flock) — a second instance fails fast
//   2. VT switched to KD_GRAPHICS so the fbcon console cannot draw over us
//   3. the stock launcher (M5CardputerZero-APPLaunch, which mmaps /dev/fb0)
//      is frozen with SIGSTOP — never killed (even when we were launched from
//      it: its Terminal=true pages would keep drawing over our output). On release() it is
//      resumed with SIGCONT and continues from its exact previous state.
//   4. keyboard evdev devices are grabbed (EVIOCGRAB) so no other process
//      receives key events while we own the screen
//   5. the current /dev/fb0 content is snapshotted
//
// release() restores all of the above (fb content, KD_TEXT, keyboard,
// APPLaunch). It is called automatically on SIGINT/SIGTERM and at exit().
#pragma once

namespace hw {

// Returns false if another display session holds the lock.
// 注意：grab_keyboard 默认 false —— 键盘由 keyboard.cpp 自己 EVIOCGRAB；
// 若此处再 grab 同一设备，kbd::open() 会 EBUSY（历史自锁陷阱）。
bool acquire(bool grab_keyboard = false);

// Idempotent；只允许在主线程/atexit 路径调用（内部有 fprintf/flock/munmap 等，
// 不是 async-signal-safe）。
void release();

// 信号处理只置标志：主循环应轮询此函数并在为真时干净退出（由 atexit 走 release）。
bool exit_requested();

}  // namespace hw
