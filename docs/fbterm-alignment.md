# 与 FbTerm 的行为对齐（参考项目）

本项目的两块参考实现：

| 项目 | 位置 | 参考点 |
|---|---|---|
| `fbterm`（GPLv2，dragchan 等） | 上级目录 `fbterm/`（`git clone`） | 直接驱动 framebuffer 的终端模拟器：fbdev + 伪终端(pty) + 内置 VTerm 解析器 + 基于控制台 keymap 的输入 |
| `fbcon`（Linux 内核） | 上级目录 `fbcon/` | 屏缓冲 / putcs / 滚动 / 光标等控制台模型 |

cardterm 与 fbterm 的总体结构一致（`/dev/fb0` 直写 + pty 运行子进程 + 用户态解析 ANSI），差异是输入从 evdev 键盘驱动进入、VT 解析器为本项目自研的轻量实现。

## 1. 已对齐的行为（`cardterm <cmd> [args...]`）

- **pty 会话**：`openpty` + `fork`/`setsid`/`TIOCSCTTY`，子进程拥有控制终端；
  窗口大小 `53×17`（`TIOCSWINSZ`），环境 `TERM=xterm-256color`、`COLORTERM=truecolor`、
  `LINES/COLUMNS`；子进程退出后返回其退出码。
- **键盘 → 字节流**：普通字符、Enter(`\r`)、Bksp(`0x7f`)、Tab(`\t`)、Esc(`0x1b`)；
  Ctrl/Alt 组合（`Ctrl-C`=0x03 等、Alt→ESC 前缀）；特殊键按 Linux 控制台 keymap 的序列：
  `F1-F12/Ins/Del/PgUp/PgDn/Home/End` 按设备 `xterm-256color` terminfo 的序列
  （`kf1=\EOP`、`kf5=\E[15~`、`khome=\EOH`、`kend=\EOF`、`kich1=\E[2~` …）；
  **Shift+PgUp/PgDn = 翻看回滚**（与 fbterm 的 Shift+PageUp/Dn 一致），普通 PgUp/PgDn 发给应用。
- **粗体→亮色**：SGR `1` 时若前景取自 16 色的 0-7，绘制改用对应亮色（8-15）；`22` 取消。
- **输入零泄漏**：修饰键的显示片段（`Ct`/`rl`/`Shi`/`ft`/`Sy`/`m`/`F`/`n`/`Al`/`t`）只作为
  调试显示事件（`Aux`），**绝不写入 pty**；未知键码一律不产生任何字符；只有真实字符、
  控制键与组合键会到达程序。全键表已逐键实测（字母/数字/符号字节完全一致，修饰键与
  Fn/Sym 长按不产生输出，特殊键序列与 terminfo 一致）。
- **VT 解析**（对齐 fbterm 的 VTerm 语义）：OSC/DCS/PM/APC 整段忽略、`ESC(B` 等字符集指定、
  DECTCEM(`?25h/l`) 光标隐藏、备用屏(`?1049/?1047/?47`)、DECSTBM 滚动区域、IND/RI、
  ESC7/8 与 CSI s/u 保存/恢复光标、ICH/DCH/IL/DL/ECH、ED 0/1/2/3（含清回滚）与 EL 三种变体、
  CHA/VPA、DECAWM(`?7`) 自动换行、DECSCUSR 光标形状（方块/下划线/竖条，闪烁/常亮）、
  `DSR 5/6` 与 `DA` 的应答（经 pty 回写）。
- **颜色**：16 色（配置调色板）、256 色（标准 xterm 调色板，含 0-15 标准基色）、
  真彩 RGB（分号与 T.416 冒号两种写法）。

## 2. 已知差异（受本项目规格/字库限制）

| 项 | fbterm | cardterm |
|---|---|---|
| 鼠标上报 | 支持 X11/X10 | 未实现（本机无鼠标） |
| 字体 | Unicode 字体 + wcwidth | GLCD 5×8 ASCII；UTF-8 近似 ASCII（框线/方块），其余用豆腐块：单宽 5×8、双宽 11×8（中间 1px 空隙） |
| 修饰属性 | 实际仅 **粗体→亮色**（`adjustCharAttr`: intensity==2 且 fcolor<8 时 +8）与 **反显**；斜体/下划线/闪烁的映射在 fbterm 源码中是被注释掉的 | 已对齐 **粗体→亮色**（SGR 1/22）与反显；斜体/下划线/闪烁按规格忽略 |
| 256 色 0-15 | VGA 风格 `0xAA` 基色 | 按用户确认使用标准 xterm 基色（16-255 与 fbterm 完全一致） |
| 其它 VT 特性 | origin mode、制表位、双宽字符、选择/复制粘贴 | 未实现（不影响 bash/vim/less/htop 基本使用） |
| 输入来源 | 内核控制台 keymap (`KDGKBENT`) | 板载 TCA8418C evdev 驱动 + 本项目的键位/符号表 |

### 会话终止语义（都回到之前的界面）

| 触发 | 行为 |
|---|---|
| 长按 Esc | 关窗：SIGHUP 会话 → 宽限 → SIGKILL（同真终端） |
| Ctrl+D | 作为 EOF（0x04）发给程序；shell/Python/`cat` 等据此退出 → 会话返回（TUI 若把 ^D 另作它用则照常发送，不劫持） |
| 输入 `exit` | 程序正常退出 → 返回其退出码 |
| 程序自行退出/崩溃/被外部杀死 | waitpid 检测 → 返回退出码（信号为 128+signum） |
| pty 从端已关闭但进程仍活着 | 按关窗语义 SIGHUP→宽限→SIGKILL，避免卡死 |
| 会话结束（含正常退出） | 对同进程组发 SIGHUP，清理后台作业；恢复画面/启动器 |

## 3. 实测（设备 `/dev/fb0` 截屏）

- `cardterm bash`：提示符/命令回显/`stty size`=`17 53`/`exit` → 退出码 0；
- `cardterm vim -u NONE /etc/os-release`：备用屏/滚动区正常，`:q!`（注入 `:`=80 `q`=16 `!`=26 Enter）退出 0；
- `cardterm less /etc/os-release`：滚动/状态行正常，`q` 退出 0；
- `cardterm htop`：颜色/仪表条/进程表正常，`q` 退出 0；
- `cardterm python3`：REPL 正常，`quit()` 退出 0；
- `DSR`：程序发 `ESC[6n` 收到 `ESC[1;1R`；`Ctrl-C` → 子进程 130；长按 Esc → 关窗（SIGHUP→SIGKILL）。
