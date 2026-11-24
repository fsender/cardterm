# M5Cardputer Launcher 板载键盘驱动分析报告

> 分析对象：`launcher-src/`（M5Cardputer Launcher，LVGL 主界面工程）
> 分析目标：板载键盘的驱动方式、键码语义、**是否可作为标准输入设备**、以及 cardterm 键盘驱动（回显 + 长按 Esc 退出）的设计依据。
> 硬件：M5Stack CardputerZero（Raspberry Pi CM0），内核 6.18.39+rpt-rpi-v8，Debian 13。

---

## 1. 结论速览

| 问题 | 结论 |
|---|---|
| 板载键盘是什么？ | **TCA8418C** I2C 键盘扫描芯片（5 行 × 10 列矩阵），挂 `i2c-1` 地址 `0x34`，IRQ = GPIO27 |
| 用什么驱动？ | Linux 内核 `tca8418_keypad`（compatible `m5stack,tca8418c`） |
| **能否作为标准输入设备？** | **能**。它注册为标准 Linux evdev 输入设备（`/dev/input/event3`），任何 evdev/libinput 程序可直接读取，无需厂商协议 |
| 键码有无特殊之处？ | 有：**符号层（Sym）使用 32 个"重定义"键码**（26→`!`、27→`@` …），语义由 `/usr/share/keymaps/tca8418_keypad_m5stack_keymap.map` 定义，不能按标准 xkb 语义解读 |
| 按键自动重复？ | 内核不提供（无 `EV_REP`）；Launcher 自己用 timerfd 实现重复。cardterm 驱动按下即回显，不实现重复 |

---

## 2. 硬件层

设备树节点（`dtov-src/modules/CardputerZero/cardputerzero-v5-overlay.dts` L88 起）：

```
tca8418@34 {
    compatible = "m5stack,tca8418c";
    reg = <0x34>;                       /* i2c-1 总线地址 */
    interrupts = <27 8>;                /* IRQ GPIO27，低电平触发 */
    keypad,num-rows = <5>;              /* 5 行 × 10 列矩阵 = 50 键位 */
    keypad,num-columns = <10>;
    sym-button-code  = <0x43>;          /* Sym 层切换键（矩阵原始扫描码） */
    fn-button-code   = <0x42>;          /* Fn 层切换键 */
    shift-button-code= <0x41>;          /* Shift 键 */
    /* LED 经 m5ioe1 IO 扩展器：capslock / fn / tables-sel 指示灯 */
};
```

- 键盘矩阵由 TCA8418C 硬件扫描，通过 I2C 上报按键事件，**不占用 GPIO 行列扫描资源**；
- Fn / Sym / Shift 是矩阵上的普通按键位置，内核驱动识别后用于**切换三套键位表**（见 §4）。

## 3. 内核驱动层

- 驱动：`tca8418_keypad`（内核自带，M5 定制 compatible），开机注册：

  ```
  input: tca8418c as /devices/platform/soc/3f804000.i2c/i2c-1/1-0034/input/input3
  ```

- 节点：`/dev/input/event3`，别名 `/dev/input/cardputer-zero-internal`，by-path
  `/dev/input/by-path/platform-3f804000.i2c-event`（Launcher 默认用 by-path）；
- 能力位：`EV = SYN | KEY | MSC`（`B: EV=13`）——**无 `EV_REP`**，内核不做按键重复；
- 权限：`crw-rw---- root:input`；用户 `me` 属于 `input` 组，**无需 root 即可读取**；
- 每键事件为标准 `struct input_event`（`EV_KEY` + `MSC_SCAN`），与任何 PC 键盘一致。

## 4. DT 三层键位映射（键码语义的真相）

内核驱动支持三套 keymap（同一物理键在不同层产生不同 Linux 键码）：

| 层 | DT 属性 | 进入方式 | 内容 |
|---|---|---|---|
| 基础层 | `linux,keymap` | 直接按键 | QWERTY 字母、数字、Esc/Tab/Enter/Backspace/Space/Shift |
| 符号层 | `linux,keymap1` | 按住 **Sym** | 标点符号（**键码被 M5 重定义**，见下表） |
| Fn 层 | `linux,keymap2` | 按住 **Fn** | F1–F12、导航簇、媒体键、亮度 |

### 4.1 Fn 层物理键位（keymap2 实测摘录）

| 物理键 | Fn+输出 | 物理键 | Fn+输出 | 物理键 | Fn+输出 |
|---|---|---|---|---|---|
| `1`…`0` | **F1–F10** | `O`/`P` | **F11 / F12** | `Backspace` | **Del** |
| `Q`/`W`/`E` | Play / Prev / Next | `U`/`I` | Bri− / Bri+ | `H` | Help |
| `A`/`S`/`D` | Mute / Vol− / Vol+ | `F` | **↑ Up** | `J` | **PrSc**(SysRq) |
| `K`/`L` | Home / PgUp | `Z`/`X`/`C` | ← / ↓ / → | `B` | **Ins** |
| `N`/`M` | End / **PgDn** | Enter/Space | 原样 | | |

### 4.2 符号层重定义键码表（keymap1 + keymap 文件）

以下键码在本键盘上**不取标准 xkb 语义**，以
`/usr/share/keymaps/tca8418_keypad_m5stack_keymap.map` 为准（Launcher 内置同款默认表）：

| 键码 | 字符 | 键码 | 字符 | 键码 | 字符 | 键码 | 字符 |
|---|---|---|---|---|---|---|---|
| 26 | `!` | 27 | `@` | 39 | `#` | 40 | `$` |
| 41 | `%` | 43 | `^` | 51 | `&` | 52 | `*` |
| 53 | `(` | 94 | `)` | 55 | `~` | 69 | `` ` `` |
| 70 | `_` | 71 | `-` | 72 | `+` | 73 | `=` |
| 74 | `[` | 75 | `]` | 76 | `{` | 77 | `}` |
| 79 | `;` | 80 | `:` | 81 | `'` | 82 | `"` |
| 83 | `<` | 85 | `>` | 86 | `\` | 89 | `\|` |
| 90 | `,` | 91 | `.` | 92 | `/` | 93 | `?` |

> 注：这些键码与标准键码空间（如 26=KEY_LEFTBRACE）冲突，但在本键盘上只由符号层产生，
> 按"符号层表优先"的顺序翻译即可正确区分（与 Launcher 的查找顺序一致）。

## 5. Launcher 用户态驱动架构

Launcher（`ext_components/cp0_lvgl/src/cp0/cp0_lvgl_keyboard.c`）的取键链路：

```
/dev/input/by-path/platform-3f804000.i2c-event
   └─ libinput（path 模式，校验 LIBINPUT_DEVICE_CAP_KEYBOARD）
        └─ xkbcommon（"us" 布局 + compose 死键处理）
             ├─ cp0_keyboard_keymap_lookup()  ← 符号层重定义表（.map 文件/内置表）优先
             ├─ cp0_keyboard_control_utf8()   ← 控制键 → ANSI 转义序列（\033OP = F1 …）
             └─ key_item 队列 → LVGL indev（含 Fn 修饰态 KBD_MOD_FN）
   另有 timerfd 自实现按键重复（内核无 EV_REP）
```

要点：
1. **libinput + xkbcommon** 完成布局/修饰/组合键处理——这本身就证明键盘是标准输入设备；
2. 自定义键码表优先于 xkb 查询（§4.2）；
3. Fn 键本体发出 `keycode 0x185`（DT 注释 KEY_DVD），Sym 键发出 `0x17f`（KEY_CD），Launcher 只作修饰态跟踪；
4. `keycode 0x42/0x43` 是**矩阵原始扫描码**（驱动内部换层用），不进 evdev。

## 6. ESC 长按约定（平台既有语义）

`cp0_esc_exit_policy.hpp`（Launcher 管理外部程序时的 Esc 策略）：

| 常量 | 值 | 含义 |
|---|---|---|
| `HINT_DELAY_MS` | 500 ms | 按住 0.5s 显示"即将退出"提示 |
| `TERMINATE_DELAY_MS` | 3000 ms | 按住 3s 发 SIGTERM |
| `KILL_GRACE_MS` | 3000 ms | 再等 3s 发 SIGKILL |

cardterm 作为**独立应用**自管退出，取 **长按 Esc 1 秒 = 退出**（比 Launcher 的 3s 短，符合"长按退出"直觉；阈值集中在 `keyboard.cpp` 的 `kEscHoldMs`，可调）。

## 7. 是否可作为标准输入设备 —— 判定

**可以。** 依据：

1. 内核注册的是标准 input 子系统设备（evdev），`EV_KEY` 事件结构与 PC 键盘一致；
2. libinput 识别为 `LIBINPUT_DEVICE_CAP_KEYBOARD` 并正常驱动（Launcher 实证）；
3. 权限开放给 `input` 组，普通用户可读；
4. 多层键位切换在**内核驱动内完成**（收到的已是最终键码），用户态无需处理层逻辑。

**注意事项**（使用时的坑）：
- 符号层 32 个重定义键码必须查表（§4.2），直接套 xkb 会得到错误字符；
- 无 `EV_REP`：长按不会自动重复，需要重复语义须自行实现；
- 需要独占时用 `EVIOCGRAB`（Launcher 与 cardterm 均如此，防止按键泄漏给控制台/其他进程）。

## 8. cardterm 键盘驱动设计（本次实现）

`src/keyboard.{h,cpp}` —— 轻量直读 evdev（不依赖 libinput/xkbcommon），**队列解耦**
（独立读键线程生产 → UI 循环消费，绘制慢时不丢键）：

- 打开 `CARDTERM_KBD`（测试注入用）→ `/dev/input/cardputer-zero-internal` → by-path，`EVIOCGRAB` 独占；
- 回显翻译顺序：**符号层表 → 字母（Shift 大小写）→ 数字行（Shift 上档符号）→ 控制键名 → `K<code>` 诊断**；
- 控制符号回显名：`F1`…`F12`、`Ins`、`Del`、`Home`、`End`、`PgUp`、`PgDn`、`PrSc`、`Up/Dn/Lt/Rt`、`Tab`、`Enter`、`Bksp`、`Esc`、`Mute/Vol±/Play/Prev/Next/Bri±` 等；
- **长按自动重发**：按住 500ms 后每 50ms 重发一次；
- **长按 Esc（≥1s）→ 退出程序**；短按回显 `Esc`；Caps 跳过不实现。

### 8.1 三态修饰键（Shift/Fn/Sym，官方按键规范；**内核驱动零改动**）

状态机在内核驱动内实现（驱动按短按/双击/按住自动切换 LED 慢闪/快闪/常亮并切换键位层），
用户态**只读**其状态：sysfs `/sys/bus/i2c/devices/1-0034/modifier_state`
（raw 0=Off 1=PRESSED 2=OneShot 3=Locked 4=HELD）→ legacy m5ioe1(0x4F) 寄存器
0xBD/0xBF/0xBE（raw 0=Off 1=OneShot 2=Locked 3=Held，同 Keyboard-Guide 官方读法）。

| 操作 | 指示灯 | 生效范围 | 回显 |
|---|---|---|---|
| 短按一次 | 慢闪 | 下一字符（一次性） | **不输出** |
| 短按两次 | 快闪 | 锁定，可连续输入 | 进入：`Shi`/`F`/`Sy`；退出：`ft`/`n`/`m` |
| 按住不放 | 常亮 | 按住期间 | 按住瞬间：`Shi`/`F`/`Sy`；松开：`ft`/`n`/`m` |

片段成对拼出键名：Shift=`Shi`+`ft`、Fn=`F`+`n`、Sym=`Sy`+`m`。
一次性上档以本地锁存补齐（状态轮询 25ms 竞态防护）；Ctrl/Alt 为标准键盘行为
（按下 `Ct`/`Al`、松开 `rl`/`t`）。测试经 `CARDTERM_MODSTATE` 状态接缝文件注入
（uinject2 的 `M:` 指令），e2e 已验证全部三种模式 + Fn/Sym 层回显。

## 9. 显示刷新策略（配套改造）

`Panel_fb::display()` 改为**纯内存读写**：绘制直接写 mmap 帧缓冲，`display()` 不再触发
`FBIOPAN_DISPLAY`、不等待 SPI 传输。实测（ftrace `mipi_dbi_fb_dirty`）：纯 mmap 写即可由内核
deferred-io 定时器（~50ms）在后台自动上屏——**程序运行永不被刷屏阻塞**，正是将来终端模拟器
所需语义（打字/输出与上屏解耦）。
