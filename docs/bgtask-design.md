# cardterm 1.1.0 后台会话（bgtask）设计与实现规格

> 日期：2026-11-26 · 分支 `cardterm_bgtask`（提交者 anonymous）· 本文件是实现与精修的**权威规格**。
> 需求经用户逐条确认；架构选定 **方案 P（PTY 桥接）**（2026-11-26 用户拍板）。

---

## 0. 已确认的用户决策（Q&A 记录）

| 决策点 | 结论 |
|---|---|
| 架构 | **方案 P**：正常窗口无 tmux（cardterm 是真终端）；剥离时守护进程接管 PTY master 并桥接进 tmux；恢复时 fd 交还 + 贴回纯文本历史 + **销毁 tmux（字面“释放该复用器”）** |
| 帮助行拼写 | `Detach`（修正用户原稿笔误 Detech） |
| 帮助页排版 | 删 `Open ~/.config/cardterm...` **上方**的那个空行（其余换行不动） |
| Fn+H 兜底 | 所有窗口都有会话/程序 → **什么都不做** |
| tmux 设备端 | **已安装**（无需再装） |
| 键盘 | 板载无 F1~F12 实体键；直切窗口 = **Fn+Ctrl+Alt+数字(1~0)/O/P**（内核 keymap2 出 F1~F12）；外接键盘 **Super+A ≡ Fn+A**（剥离） |
| 版本/发布 | 测试通过后 1.1.0；README/商店文案/权限/Depends 更新；**提交仅限 cardterm_bgtask 分支**（anonymous，不签名） |

## 1. 键位映射（已实测核实）

来源：设备运行中 DT `/proc/device-tree/soc/i2c@7e804000/tca8418@34/linux,keymap2`（与
`project_clones/dtov-src/modules/CardputerZero/cardputerzero-v5-overlay.dts` 一致）：

| 组合 | 内核出键码 | cardterm 动作 |
|---|---|---|
| 板载 **Fn+A** | `KEY_MUTE`(113)，keymap2 `0x02000071` | `Cmd::Detach` |
| 板载 **Fn+H** | `KEY_HELP`(138)，keymap2 `0x0205008a` | `Cmd::ShowHelp` |
| 板载 Fn+Ctrl+Alt+1~0/O/P | KEY_F1~F12 | 切窗口 1~12（现 Echo 路径，不变） |
| 外接 **Super+A** | KEY_A + meta | `Cmd::Detach` |
| 外接 **Super+H** | KEY_H + meta | `Cmd::ShowHelp`（按 Super≡Fn 约定） |
| 外接 Super+F1~F12 / W/E / U/I / Q | 现状 | 不变 |

实现点：
- `keyboard.h` `Cmd` 枚举在 `FontUp` 后插入 `Detach, ShowHelp`（Win1..12 保持连续）。
- `keyboard.cpp` 内部键盘 Fn 层 Cmd 块（现 KEY_PLAYPAUSE 等）加 `KEY_MUTE→Detach`、`KEY_HELP→ShowHelp`（**不查 Fn 状态**：这两个键码只可能来自 Fn 层）。
- `keyboard.cpp` 外接 `d.meta` 组合表加 `KEY_A→Detach`、`KEY_H→ShowHelp`（按下派发 + 松开吞掉防漏给 PTY）。

## 2. 架构（方案 P：PTY 桥接）

```
正常运行:  cardterm ──openpty──> 程序 (bash/ssh/make...)      [cardterm=真终端, 无 tmux]
                 │
Fn+A 剥离:  cardterm ──SCM_RIGHTS──> bgtaskd(守护进程, setsid)  持有 master fd
                 │                    │
                 └─ 释放窗口全部内存    └─ tmux -L cardterm new -d -s cardterm_PTY_03
                                        'cardterm --bgtask-relay <sock>'
                                        (中继: master ⇄ relay ⇄ tmux pane 显示+256KB 历史)
恢复:      新 cardterm ──TAKEOVER──> bgtaskd
              ① 回传 child_pid/pgid/cols/rows + 纯文本历史(≤buffer_kb) + master fd
              ② bgtaskd 销毁 tmux 会话+中继（"释放该复用器"）
              ③ cardterm 贴历史进回滚 → TIOCSWINSZ(内核自动 SIGWINCH) → TUI 重绘
              ④ toast `Attached PTY 03`
```

### 2.1 守护进程 bgtaskd（`--bgtaskd`，由 cardterm 二进制兼任）

- 启动：首次剥离时由 cardterm fork+setsid 启动（double-fork，pid/token 文件防多实例）；
  路径 `/run/user/1000/cardterm-bgtask.sock`（0600；目录 0700 仅本人）。
- 协议（行长 + SCM_RIGHTS）：
  - `PARK <name> <cols> <rows> <child_pid> <pgid>` + fd → `OK` / `ERR <msg>`；
  - `TAKEOVER <name>` → `OK <child_pid> <pgid> <cols> <rows> <hist_len>\n<hist>` + fd；随后 daemon 自杀 tmux 会话；
  - `LIST` → 每行一个 `cardterm_PTY_NN`。
- 每会话：`tmux -L cardterm new -d -s <name> '<exe> --bgtask-relay <会话sock>'`；
  中继子进程连接 per-session unix socket，daemon 在 master fd ⇄ socket 之间全双工搬字节
  （ssh 端 `tmux attach -t cardterm_PTY_03` 可看可输入）。
- 历史来源：`tmux -L cardterm capture-pane -p -S -`（纯文本、无颜色），截断至 `buffer_kb`。
  tmux `history-limit ≈ buffer_kb*1024/cols` 行；`status off`。
- 生命周期：程序退出（master EOF/EIO）→ 杀 tmux 会话；无会话 → 自动退出。
- **已知限制**：bgtaskd 是 cardterm 的后代，退出后在 launcher（subreaper）下留一个僵尸条目
  至 APPLaunch 重启（沿用可行性报告 §2.2 的 caveat，影响极小）；bgtaskd 崩溃 = 其托管程序死。

### 2.2 cardterm 侧状态机（`Win` 增量字段）

| 状态 | 含义 | 退出检测 |
|---|---|---|
| open（现状） | cardterm 自己的 PTY，w.child 可 waitpid | `check_exits` waitpid |
| open+adopted | TAKEOVER 拿回的 PTY，w.child 非亲生 | **EOF/EIO**（read 0 字节/EIO）→ 走现有关窗流程 |
| parked（槽位空闲） | 无窗口无状态，tmux 会话存在 | `tmux has-session` 探测 |
| 空槽 | 无窗口无会话 | Fn+W/E 切到 → launcher 初始界面 |

- Fn+A 剥离：仅对 open 窗口生效（**开屏/帮助界面（launcher 态）不响应**）；
  PARK 成功 → 释放 term::State（req 4 不留缓冲）→ 关窗 → 切下一开窗 toast `Detached PTY 03. Now PTY 01`；
  无其他开窗 → 全屏倒计时（req 2 文案）。PARK 失败 → 错误 toast，窗口保持。
- Fn+Q（现状语义）：杀程序+杀会话（TAKEOVER 窗口杀 pgid）+关窗；最后一个窗口 → 倒计时（req 3 文案）。
- ESC 长按 2s：只杀**开着的窗口**（含其会话），**parked 会话保留**（req 7：退出 cardterm 不杀已剥离会话）；不加倒计时。
- 切窗到 parked 槽位 → TAKEOVER 流程 → toast **`Attached PTY 03`**（替代 `Switched to PTY 03`）；
  普通切窗 toast 不变。
- 启动扫描（req 8）：第一个窗口初始化后、显示 banner **之前**，`for N=01..windows` 找最小
  `cardterm_PTY_NN` 存在者 → 等效直接切到该窗并 TAKEOVER；**不初始化 PTY 01、不跑默认命令行**；
  toast `Attached PTY **`。无会话 → 维持现状（launcher banner）。

### 2.3 全屏倒计时提示（req 2/3 共用）

- 矩形边框：边距 **14px**（上下左右）、线宽 **2px**；三行文本水平+垂直居中；
- 文案 A（剥离末窗）：`Detached PTY 03.` / `No terminal running.` / `cardterm will exit in 3s.`
- 文案 B（Fn+Q 末窗）：`PTY 03 closed.` / `No terminal running.` / `cardterm will exit in 3s.`
- 第三行随秒更新 **3s→2s→1s**（每秒重绘），随后退出程序；倒计时期间忽略按键。

## 3. 文案改动

### 3.1 banner（`config.cpp` 模板 `splash=` 默认值）

- 快捷键说明 `FN`→`Fn` 全部（`FN+W / FN+E` → `Fn+W / Fn+E` 等）；
- 末行 `Type <fgcolor=F0C828>"help"</fgcolor> for more information.` →
  `Press <fgcolor=F0C828>Fn+H</fgcolor> for more information.`（沿用原高亮 F0C828）；
- **仅改模板默认值**；设备端既有 config.ini 只补 `windows=12`（改前备份）。

### 3.2 帮助页（`session.cpp launcher_help()`）

- `FN`→`Fn`（三处）；新增两行（插在 `FN+U / FN+I` 行后、`Hold ESC` 行前）：

```
Fn+A               Detach terminal and move it to
                   background. Reopen to attach.
```

- 删 `Open ~/.config/cardterm...` **上方**的空行（上方首个 `h += "\r\n"`）。
- 帮助页 53×17 排版上限 17 行——增删后需真机截图验证（现已达上限）。

## 4. 窗口数（可配置，2026-11-26 重新启用）

- `[general] windows=`（**2–32，默认 12**）；直切快捷键（Ctrl+Alt+F1~F12 /
  外接 Super+F1~F12）指向超出配置数的窗口时弹 `Max PTY is NN. Edit config.ini.`
  提示并忽略；13 号以上窗口用 Fn+W / Fn+E 循环到达；配置值越界钳位到最近边界。
- `Win g_wins[32]`（编译上限 32），`g_win_count` 收敛所有循环/取模。
  预留槽位成本 ~1.7KB/个（含空出站队列堆），常规使用几无感。

## 5. 配置新增

```ini
[background]
# Max bytes of detached-session output buffered for restore (16-1048576, default 262144 = 256KB).
buffer_kb=256
```

## 6. 交付清单（阶段 5，测试通过后执行）

- `CMakeLists.txt`/`app-builder.json` 版本 → **1.1.0**；
- README.md / README.zh-CN.md：新功能（后台会话/剥离/附着、Fn+A、Fn+H、窗口数配置）；
- `app-builder.json`：描述加 **“后台会话支持 / Background session support”**；
  `permissions.background_service: true`；
- `packaging/control.in`：`Depends: tmux`（外加 ${shlibs:Depends} 等现状保留）；
- CHANGELOG `[1.1.0]`；deb 构建+推设备 `~/apps/cardterm` 测试；
- **git commit 仅 cardterm_bgtask 分支**（anonymous / commit.gpgsign=false，已配置）。

## 7. 测试计划（真机，uinject2 键注入）

1. Fn+A（注入 KEY_MUTE=113）剥离 → toast `Detached PTY NN. Now PTY MM`；
2. 剥离期间后台输出（`seq`/`ping` 持续输出）→ 切回 → 历史贴回 + `Attached PTY NN` + 程序仍活；
3. 剥离末窗 → 全屏倒计时 3/2/1 → 退出；重启 cardterm → 自动 `Attached PTY NN`（req 8）；
4. Fn+Q 末窗 → 文案 B 倒计时；Fn+H → 空窗+帮助（无 banner）；全忙 → 无动作；
5. 窗口数 3 配置 → Fn+Ctrl+Alt+5 无反应、Fn+W/E 环 3；
6. 帮助页 17 行内截图核对（Fn+A 行、Fn 大小写、空行删除、banner 末行）；
7. ssh 远端 `tmux -L cardterm attach` 看/输入被剥离会话（方案 P 加分项）。
