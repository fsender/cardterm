// keyboard.h — CardputerZero 键盘驱动（板载 TCA8418C + 外接 USB/蓝牙键盘）
//
// 直接读取 /dev/input，自带：
//   - 板载键盘：板载 DT keymap 专用键码 → 回显文本（符号层重定义表 + 字母/数字
//     + 控制键名）、内核修饰键三态（Shift/Fn/Sym）跟踪；
//   - 外接键盘（USB/蓝牙）：枚举 /dev/input/event* 自动接管，标准 US 布局键码 →
//     字符，修饰键（Shift/Ctrl/Alt）按设备独立跟踪；
//   - 多设备并行 poll + 热插拔重扫（插入自动接管，拔掉只移除该设备）；
//   - 长按 Esc 两段式：按住 ≥ kEscHintMs（0.5s）→ 提示信号（EscHint，弹 toast）
//     ；继续按住 ≥ kEscHoldMs（2s）→ Quit 信号（程序退出）；中途松开不产生任何信号；
//   - EVIOCGRAB 独占，按键不泄漏给控制台/其他进程。
// 不依赖 libinput / xkbcommon。分析依据见 docs/keyboard-driver-analysis.md。
#pragma once

#include <cstdint>
#include <string>

namespace kbd {

enum class Poll { Timeout, Echo, Quit, Mod, Aux, Cmd, EscHint };

// 窗口管理快捷键（Fn 层 keymap 直接产生的键码，不经 PTY）：
//   FN+Q=KEY_PLAYPAUSE 关闭当前终端；FN+W=KEY_PREVIOUSSONG / FN+E=KEY_NEXTSONG
//   切换上/下一个窗口；FN+U=KEY_BRIGHTNESSDOWN / FN+I=KEY_BRIGHTNESSUP 缩小/放大字号。
enum class Cmd {
  None, Close, PrevWin, NextWin, FontDown, FontUp,
  // Super+F1..F12（外接键盘）：切到第 1..12 个窗口。用枚举尾段做整数区间判断：
  //   if (c >= Cmd::Win1 && c <= Cmd::Win12) slot = int(c) - int(Cmd::Win1);
  Win1, Win2, Win3, Win4, Win5, Win6, Win7, Win8, Win9, Win10, Win11, Win12
};

// 结构化键事件（kitty 键盘协议 / 通用按键语义）：
//   code：evdev 键码；ev：1=按下 2=重复 3=松开；
//   mods：修饰原始位掩码 shift=1 alt=2 ctrl=4（xterm/kitty 编码值 = mods+1）；
//   lo/up：可打印键的未上档/上档字符（0=非打印键）。
struct KeyEvent {
  uint16_t code = 0;
  uint8_t  ev   = 1;
  uint8_t  mods = 1;
  char     lo   = 0;
  char     up   = 0;
};

// 打开键盘并独占。主设备查找顺序：
//   环境变量 CARDTERM_KBD（测试注入用）→ /dev/input/cardputer-zero-internal
//   → /dev/input/by-path/platform-3f804000.i2c-event
// 主设备打开后还会枚举 /dev/input/event*，把 USB/蓝牙外接键盘（有字母键）一并
// 打开：板载键码走板载布局，外接键码走标准 US 布局，修饰键状态按设备独立跟踪。
// 运行期每 ~1.8s 重扫一次 /dev/input 实现热插拔；外接设备掉线只移除该设备。
// CARDTERM_KBD 显式指定时仍是严格模式（打不开就失败、不回退）；若该设备是
// USB/蓝牙键盘则按标准 US 布局解析（名字含 test-kbd 的 uinput 注入器为兼容
// 既有 e2e 测试仍按板载布局）。
bool open();

void close();

// 等待至多 timeout_ms 的一次输入。
//   Echo：echo 为回显文本（单字符或控制键名，如 "PgDn"）；
//   Mod：Ctrl/Alt 状态变化，echo 为 "ctrl_on"/"ctrl_off"/"alt_on"/"alt_off"
//        （PTY 会话据此组合 ^C 等控制字符；本地回显模式可忽略）；
//   Aux：修饰键的显示片段（"Ct"/"rl"/"Shi"/"ft"/"Sy"/"m"/"F"/"n"/"Al"/"t"）。
//        仅供调试演示显示；PTY 会话必须忽略，不能作为输入发给程序；
//   Quit：按住 Esc 到退出阈值（2s），调用方应退出程序；
//   EscHint：按住 Esc 到提示阈值（0.5s，整段按住只报一次），调用方弹提示
//        （toast "Hold ESC for 2s to exit"）；随后松开不再有任何事件；
//   Timeout：无输入。
//   kev 非空时随 Echo 一并返回结构化键事件（kitty 键盘协议编码用）。
Poll poll(int timeout_ms, std::string& echo, KeyEvent* kev = nullptr,
          Cmd* cmd = nullptr);

// Shift 是否处于逻辑按住态（evdev 事件判定；驱动合成 down/up 也计入，
// 因此 One-shot 上膛窗口内同样为真）。板载与外接键盘的 Shift 任一按住即返回 true
// （无外接键盘时与旧行为一致）。fbterm 语义：Shift+PgUp/PgDn 翻看回滚，
// PgUp/PgDn 发给应用。字符是否上档由内部 shift_effective() 判定（Token 恰好一次）。
bool shift_down();

// 键盘设备是否已全部丢失（读线程异常退出）。板载或外接单设备掉线不算丢失
// （热插拔会继续工作）；只有所有键盘设备都不在了才置位。session 应据此提示并
// 干净退出，避免出现"画面还在、按键全无"的死终端。
bool device_lost();

}  // namespace kbd
