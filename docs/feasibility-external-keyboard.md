# 可行性评估：USB / 蓝牙外接键盘接管输入

> 评估日期：2026-11-24 · 基线 HEAD `7fd9a3e`（cardterm 0.7.2）
> 结论：**值得做（最小方案，≈0.5 人日，低风险）**；完整的“多设备并行 + 热插拔”**暂缓**
> （2–3 人日，中等风险）。本报告只做评估，未改任何代码。
>
> **基线说明**：本评估基于 HEAD `7fd9a3e`。写作期间工作区已出现**并行未提交改动**
> （`src/keyboard.cpp` 的 One-shot 修饰键语义重构，顺带覆盖了 `KEY_RIGHTSHIFT(54)` 等）。
> 该改动落地后，本报告方案 A 里的“右 Shift”子项即已顺带完成；其余结论
> （标准 PC 布局表、bustype 选择、多设备/热插拔）不受影响，估算时按“不含右 Shift”略作下调即可。

---

## 1. 现状（代码证据）

| 环节 | 现状 | 位置 |
|---|---|---|
| 设备打开 | **只打开 1 个** evdev：`CARDTERM_KBD`（严格模式，不回退）→ `/dev/input/cardputer-zero-internal` → by-path `platform-3f804000.i2c-event`；打开即 `EVIOCGRAB(1)` 独占 | `src/keyboard.cpp::open()` |
| 读事件 | 单 fd、单线程 `poll(&p, 1, to)`，读 16 个 `input_event` 后逐条 `process()` | `src/keyboard.cpp::reader_loop()` |
| 设备丢失 | 读错误 → `g_lost=true` → `session::run()` 主循环检测到后打印提示并**整个程序退出** | `src/keyboard.cpp`、`src/session.cpp`（`kbd::device_lost()` 分支） |
| 热插拔 | **无** inotify / udev / 重新扫描逻辑 | 全仓库无 `inotify`/`udev` |
| 键码翻译 | 硬编码 CardputerZero 专用表：符号层 32 个“重定义键码”优先，再字母/数字/控制键名 | `src/keyboard.cpp::translate()`、`docs/keyboard-driver-analysis.md` §4.2 |
| 修饰键 | 物理 Shift 只跟踪 `KEY_LEFTSHIFT(42)`；Ctrl 29/97、Alt 56/100 跟踪；Fn/Sym 三态状态来自**板载键盘内核驱动**的 sysfs `/sys/bus/i2c/devices/1-0034/modifier_state`（或 legacy i2c 0x4F 寄存器） | `src/keyboard.cpp` |
| 自动重复 | 用户态自管：最后一个键触发，500ms 后 50ms/次（内核无 `EV_REP`） | `keyboard.cpp` |
| 独占显示层的 grab | `hw::acquire(grab_keyboard=false)`——**生产路径不枚举/不 grab 全部键盘**（避免与 kbd 冲突）；但 `exclusive_display.cpp::grab_keyboards()` 保留了“枚举 `/dev/input/event*`，凡有 `KEY_A..KEY_Z` 就 `EVIOCGRAB`”的实现，目前未被调用 | `src/main.cpp:27`、`src/exclusive_display.cpp`、`src/exclusive_display.h`（注释：再 grab 同一设备会使 kbd `EBUSY`） |

**现在插入一个 USB/蓝牙键盘会发生什么**：它的 evdev 节点会被系统创建，但 cardterm 既不打开它
（只认固定路径/环境变量），也不（生产路径下）grab 它；VT 处于 `KD_GRAPHICS` 且 APPLaunch 被
`SIGSTOP`，所以按键不会有任何可见效果——**外接键盘当前完全无效**，但也不会破坏现有行为。
若把 `CARDTERM_KBD` 指向它，程序能跑起来，但**字符语义是错的**（见 §2.1）。

---

## 2. 需要改什么

### 2.1 键码 → 字符映射不是通用的（最小方案的核心）

`translate()` 的顺序是“**符号层重定义表优先** → 字母 → 数字行 → 控制键名”。这套表来自
`/usr/share/keymaps/tca8418_keypad_m5stack_keymap.map`：板载键盘的键码空间被 M5 的 DT keymap
重定义了 32 个位置（`docs/keyboard-driver-analysis.md` §4.2）：

| evdev code | 在板载键盘上= | 标准键盘上实际是 |
|---|---|---|
| 26 / 27 | `!` / `@` | `[` / `]` |
| 39 / 40 / 41 / 43 | `#` `$` `%` `^` | `;` `'` `` ` `` `\` |
| 51 / 52 / 53 | `&` `*` `(` | `,` `.` `/` |
| 69 / 70 / 71 / 72 / 73 | `` ` `` `_` `-` `+` `=` | NumLock/ScrollLock 等 |
| 74–77 / 79–83 / 85 / 86 / 89–93 | `[ ] { } ; : ' " < > \ | , . / ?` | 方向/编辑键区等 |

也就是说：**外接键盘按 `[` 会打出 `!`，按 `,` 会打出 `&`，按 `-`/`=`（12/13）根本不在任何表里
（被丢弃）**。字母 A–Z、0–9、空格是对的（标准键码相同），Esc/Tab/Enter/Backspace/F1–F12/
方向键/Home/End/PgUp/PgDn 等控制键名表也基本通用（F1–F12 键码 59–68/87/88 与标准一致）。
另外 `KEY_RIGHTSHIFT(54)` 没有被跟踪（只有 42），外接键盘用右 Shift 会失效；CapsLock 被主动跳过。

**因此最小可用方案必须先加一套“标准 US/PC 键盘布局”翻译表**，并**按设备类型选择布局**：

- 用 `EVIOCGID` 取 `bustype` 区分来源：板载 = `BUS_I2C(0x18)`，USB = `BUS_USB(0x03)`，
  蓝牙 = `BUS_BLUETOOTH(0x05)`；也可辅以 `EVIOCGNAME`。
- 非 i2c 设备走标准 PC 布局表（含 12 `-`、13 `=`、26 `[`、27 `]`、39 `;`、40 `'`、41 `` ` ``、
  43 `\`、51 `,`、52 `.`、53 `/` 及其 Shift 上档），板载设备保持现有表不变。
- 顺带补 `KEY_RIGHTSHIFT(54)`（两种设备都受益），并可选择让 CapsLock 生效或维持跳过。

### 2.2 多 fd 的 poll/epoll（仅完整方案需要）

- `kbd` 内部从“单 fd”改成“设备列表”：`struct Dev { int fd; uint32_t bustype; layout_t layout; }`；
  `reader_loop` 每轮构造 `pollfd[]`，按就绪 fd 分发给同一个 `process()`。
- 每个设备独立 `EVIOCGRAB`（失败只跳过该设备，不影响其它设备）；同一物理设备不能重复打开
  （`kbd` 与 `exclusive_display::grab_keyboards()` 二选一，后者生产路径已关闭，`exclusive_display.h`
  已记录这个“自锁陷阱”）。
- 建议暴露 `CARDTERM_KBD` 支持逗号分隔列表（测试注入/显式指定多设备），并保留现有严格模式语义。

### 2.3 热插拔（仅完整方案需要）

- `inotify` 监听 `/dev/input`（`IN_CREATE|IN_MOVED_TO|IN_ATTRIB`），目录变化后 300–500ms 防抖，
  重新扫描 `/dev/input/event*`：过滤 `EV_KEY` 且同时具备 `KEY_A` 与 `KEY_Z` 的设备（与
  `exclusive_display.cpp::grab_keyboards()` 同一判据），新设备 `EVIOCGRAB` 后加入列表。
- 设备消失（`POLLHUP/POLLERR`、`read` 返回 `ENODEV`）→ 从列表移除并释放 grab；
  **只有“所有键盘设备都没了”才提示并退出**，单设备掉线不再杀终端（这是完整方案相比现状的必要语义修正）。
- 不引入 libudev 依赖（cardterm 目前无该依赖；`inotify` + 目录扫描足够，且可用 `test/uinject2.c`
  的 uinput 注入器创建/销毁虚拟键盘来做自动化测试）。

### 2.4 修饰键 / kitty 协议 / 快捷键冲突（完整方案）

- 现状 `g_shift/g_ctrl/g_alt` 是全局单份状态：多设备时改为“按设备记录 pressed 集合，再合并成
  全局 mask”；板载 Fn/Sym 三态只对 i2c 设备有意义，外接设备没有 Fn/Sym，轮询可继续（不影响）。
- kitty 键盘协议（`session.cpp::kkp_seq()`）用的是 `KeyEvent{code,mods,lo,up}`：标准布局表的
  `lo/up` 必须与外接键盘布局一致，否则 kitty 报告的文本会错（这是布局表改造的附带收益）。
- 窗口快捷键（`Ctrl+Alt+F1~F12`、`Shift+PgUp/PgDn`、`Ctrl+Shift+↑↓`、长按 Esc 退出）在标准键盘上都
  有对应键码，天然可用；Fn 层快捷键（`Fn+Q/W/E/U/I`）外接键盘没有 Fn，无法使用——这是预期限制，
  文档写清即可。
- 自动重复是“最后按键”全局状态：两台设备同时按键时重复对象会互相打断（可接受；若要严格可改
  为按 `(dev,code)` 记录，但收益低）。
- 安全过滤：`KEY_POWER/SLEEP/WAKEUP(116/142/205/143)` 已被过滤，外接键盘的电源/媒体键不会把
  设备关机（第二道防线保留）。

### 2.5 独占显示层的 grab 策略

- 生产路径 `hw::acquire(false)` 已经避免与 `kbd` 抢设备，**不需要改**；外接键盘的独占统一由
  `kbd` 负责，这样退出时 `kbd::close()` 会逐一 `EVIOCGRAB(0)`，进程被杀时内核自动释放。
- 如果将来有人把 `hw::acquire(true)` 打开：它会先 grab 板载键盘，`kbd::open()` 随后 `EBUSY` 失败
  → 这也是现有注释里写的历史陷阱。建议保持 `false`，并在 `exclusive_display.h` 注释里补一句
  “多键盘枚举归 keyboard.cpp 所有”。

---

## 3. 工作量与风险

### 方案 A（推荐，最小）：允许外接键盘接管 `CARDTERM_KBD` + 标准 PC 布局表

**范围**：`src/keyboard.cpp` 内加标准布局表 + `EVIOCGID` bustype 选择布局 + 右 Shift；
不做多设备、不做热插拔；设备掉线维持现有“提示后退出”行为（因为内置键盘仍在，用户可重开）。

| 项 | 估计 |
|---|---|
| 代码改动 | ≈ 120–200 行，集中在 `keyboard.cpp`（1 个文件，不动 ABI/头文件） |
| 开发 | 0.2–0.3 人日 |
| 测试 | 0.2 人日：`test/uinject2.c` 注入 uinput 键盘做自动化；真机插 USB 键盘 + 蓝牙键盘各过一遍主要键位 |
| 合计 | **≈0.5 人日**（含真机验证） |
| 风险 | **低**：内置键盘路径不变；只影响 `CARDTERM_KBD` 指向的非 i2c 设备；失败可回退（不设环境变量即原行为） |
| 已知限制（写进文档即可） | 无热插拔（须先接键盘再启动，或重启 cardterm）；断连=当前会话退出；无 Fn 层快捷键；CapsLock 仍不实现 |

**判定**：工作量 ≤0.5 人日、风险低 → **值得做**。

### 方案 B（完整）：多设备并行 + 热插拔

**范围**：`keyboard.{h,cpp}` 设备列表重构 + 每设备 grab/布局 + inotify 热插拔 + `device_lost`
语义改为“全部设备丢失才退出” + 修饰键/重复状态调整 + 测试。

| 项 | 估计 |
|---|---|
| 代码改动 | ≈ 300–450 行（`keyboard.{h,cpp}` 为主，`session.cpp` 少量） |
| 开发 | 1.5–2 人日 |
| 测试 | 0.5–1 人日：uinput 多设备并发/热插拔自动化 + 真机 USB/BT 断连重连 + 回归（历史审计对 fd 泄漏/CLOEXEC 很敏感） |
| 合计 | **2–3 人日** |
| 风险 | **中**：① 多设备同时 `EVIOCGRAB` 后系统级热键（VT 切换等）全部失效，只能靠长按 Esc 退出；② 蓝牙键盘休眠/断连重连的 evdev 节点可能被销毁重建，重挂逻辑需要真机反复验证；③ 修饰键合并与自动重复的边界情况；④ 读线程/fd 生命周期改动，容易引入泄漏或崩溃（本项目此前有过 fd 未 CLOEXEC 的审计发现） |
| 硬件/环境约束 | 真机验证需要把机身左侧 USB 开关切到 **Host**（USB-A/Hub 才工作），按 M5 文档此模式下 CM0 USB 走内部 Hub，**可能与 USB ADB 调试互斥**（`documents/m5docs_zh_cardputerzero_product_page.md` §USB 2.0：Slave 模式断开 Hub，右侧 USB-C 直连 CM0）——即“插 USB 键盘调试”和“USB ADB”可能不能同时用；蓝牙键盘则需要先完成系统级配对（BlueZ），cardterm 不负责配对 |

**判定**：2–3 人日、中等风险、需求未确认 → **暂缓**；等真有“内置键盘 + 外接键盘同时用/随时插拔”
的产品需求再做。

---

## 4. 推荐方案

1. **现在做方案 A**（≈0.5 人日，低风险）：`CARDTERM_KBD=/dev/input/by-id/...`（或 by-path）指向
   USB/蓝牙键盘即可使用，标准 US 布局由 bustype 自动选择；不设该环境变量时行为与今天完全一致。
2. 若产品明确要求“外接键盘与内置键盘**并行**、**热插拔**、外接键盘掉线不退出”，再排方案 B；
   实现时把 `exclusive_display::grab_keyboards()` 的枚举判据复用过来，但**只由 `kbd` 持有 grab**。
3. 无论做哪个方案，都应同步更新 `docs/keyboard-driver-analysis.md` 与 `README` 的
   `CARDTERM_KBD` 说明（当前 README 未提及该环境变量，`keyboard.h` 里有注释）。

---

## 5. 附：可复用的现成件

- `test/uinject.c` / `test/uinject2.c`：uinput 键盘注入器（v2 带全程日志），可创建/销毁虚拟键盘，
  正好覆盖“多设备/热插拔”的自动化测试。
- `exclusive_display.cpp::grab_keyboards()`：现成的“枚举有字母键的 evdev 并 grab”实现（未启用），
  可作为设备筛选判据的参考。
- `docs/keyboard-driver-analysis.md`：板载键盘三层 keymap、重定义键码表、内核修饰键状态源。
