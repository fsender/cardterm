# tmux 后端开发前准备：会话命名 / 剥离 / 恢复设计

> 日期：2026-11-26 · 分支 `cardterm_bgtask`（提交者 anonymous）· 依据：宿主机 tmux 3.4 语义实测 + 设备只读探查
> 结论先行：**命名标识方案完全可行**——每个 cardterm 窗口对应一个带前缀的 tmux 会话，
> 启动时枚举 `cardterm_PTY_xx` 自动恢复；全部能力都是 tmux 原生语义，无需任何 I/O 重构。

---

## 1. 命名规范（已实测确认）

| 项 | 规范 | 实测依据 |
|---|---|---|
| 会话名 | **`cardterm_PTY_NN`**，NN = 两位零填充窗口号（01–12，与 toast "PTY %02d" 同编号） | tmux 3.4 接受该名字，rc=0；`list-sessions` 原样回显 |
| 非法字符 | 避免 `.` 和 `:`（tmux 目标语法分隔符；实测会被规整为 `_` 并可能撞名报 `duplicate session`） | `bad.name` → 列表显示 `bad_name`；`bad:name` → `duplicate session: bad_name` |
| 空格/纯数字/中文 | tmux 均接受，但我们的命名不用 | 实测 rc=0 |
| 专用 socket | **`tmux -L cardterm`** → socket 文件 `/tmp/tmux-1000/cardterm`（0660, me:me） | 实测 `-L` 隔离生效；与用户可能手动开的 tmux 互不可见 |
| 存在性检查 | `tmux -L cardterm has-session -t cardterm_PTY_03` → **rc=0 存在 / rc=1 不存在**（无需 tty） | 实测 ✓ |
| 枚举 | `tmux -L cardterm list-sessions -F '#{session_name}'` → 逐行名字 | 实测 ✓，`grep '^cardterm_PTY_'` 即可过滤 |

## 2. 关键语义实测（宿主机 tmux 3.4，逐项验证）

| # | 场景 | 结果 |
|---|---|---|
| 1 | `new -A -s cardterm_PTY_03`（同名已存在） | **幂等 create-or-attach**——存在即 attach、不存在即新建，一个命令完成"检查+恢复" |
| 2 | `new -A` 在**非 tty** 环境（cardterm 主进程） | **失败 rc=1** `open terminal failed: not a terminal`（`-A -d` 也一样）→ **必须在 spawn 出的 PTY child 里 exec**（cardterm 现有架构正好如此：child `setsid+TIOCSCTTY` 后才有 tty）。主进程若要预检，用 `has-session`，不要用 `new -A` |
| 3 | client 被 **SIGKILL** | **会话存活、server 存活**（`destroy-unattached`/`exit-unattached` 默认 off）；后续新 client 可正常 attach ✓ |
| 4 | pane 里 shell 正常 `exit` | **会话自动销毁**（`has-session` rc=1）→ 陈旧会话自清理，不会积累僵尸会话 |
| 5 | cardterm 崩溃/被 SIGKILL（master 关闭） | client 因 ctty 挂断自行退出 → 同场景 3：**会话存活**（等价于剥离） |
| 6 | client 死后重新 attach | 新 client attach 成功，**全屏重绘**走 cardterm 现有 `pump_all()` 解析路径恢复画面（I/O 不动的依据，见 `feasibility-background-sessions.md`） |

> ⚠️ 实测注意：`new -A`（含 `-A -d`）**只能在有 tty 的 child 里调用**；主进程的预检/枚举一律用
> `has-session` / `list-sessions`（都不需要 tty）。这是设计里唯一的调用位置约束。

## 3. 恢复流程设计

### 3.1 启动（reattach）

```
cardterm 启动
  ├─ 枚举：tmux -L cardterm list-sessions -F '#{session_name}' | grep '^cardterm_PTY_'
  │         → 得到已存在的窗口槽位集合（如 {03, 07}）
  ├─ 窗口集合 = 枚举结果 ∪ 默认初始窗口（无任何会话时按现有默认逻辑开 1 窗）
  └─ 每个槽位照常 spawn()（openpty + fork + exec）：
        exec: tmux -L cardterm -f <cardterm-tmux.conf> new -A -s cardterm_PTY_NN
        child 有 tty → create-or-attach 一步完成；attach 触发全屏重绘 → 现有解析路径恢复画面
```

- **不需要单独的"检查存在再决定 attach"两步协议**：`-A` 在 child 里天然幂等；
  枚举只用来决定**开哪些槽位的窗口**。
- 槽位号即窗口号：窗口 03 ↔ 会话 `cardterm_PTY_03`，映射纯约定，**无需状态文件**（会话名本身就是持久化状态）。

### 3.2 剥离（detach，cardterm 退出）

| 触发 | 行为 |
|---|---|
| FN+Q / 长按 ESC / 收到信号（现有退出路径） | `kill_all()` SIGHUP→SIGKILL 打到 **client 进程组** → client 死、**会话与 server 存活**（实测 #3/#5）；master 关闭、显示还原、回启动器——**现有代码路径零修改** |
| 彻底退出（杀掉会话，可选功能） | 退出前对每个会话 `tmux -L cardterm kill-session -t cardterm_PTY_NN`（或 `kill-server`）——新增一个键绑定/配置项 |

### 3.3 运行期

- 窗口内 shell `exit` → client 退出 → 会话销毁（实测 #4）→ `check_exits()` 现有逻辑关窗+toast，**零修改**；
- 字号/尺寸变化 → `resize_pty()` TIOCSWINSZ → client SIGWINCH → tmux server 改会话尺寸 → pane TUI 重绘，**零修改**。

## 4. tmux 配置文件（随 deb 分发）

新建 `projects/cardterm/packaging/cardterm-tmux.conf`（spawn 时 `-f` 显式指定，不依赖用户 `~/.tmux.conf`）：

```
set -g status off                 # 320×170 上不占行（17 行宝贵）
set -g history-limit 500          # 每会话历史 0.4MB→约 0.1MB（见 §8 内存实测）
set -g destroy-unattached off     # 显式声明：剥离后会话保留（本就是默认，写死防用户改动）
set -g exit-unattached off
set -g mouse off                  # 设备无指针
set -g assume-paste-time 1        # 键入节奏（可调）
```

## 5. 改动清单（按层归属，I/O 层 0 改动）

| 层 | 改动 | 量级 |
|---|---|---|
| **I/O 层（读泵/双队列/应答/resize/退出巡检）** | **无** | 0 |
| spawn | argv 包 `tmux -L cardterm -f … new -A -s cardterm_PTY_NN`（NN=窗口号） | ~5 行 |
| 启动恢复 | 枚举 `list-sessions` 决定初始窗口集（§3.1） | ~20 行 |
| 退出策略 | 现有路径即"剥离"；可选新增"彻底退出"绑定（kill-session） | ~10 行 |
| 滚动 | `Fn+PgUp/PgDn` → tmux copy-mode 映射（原生回滚历史重附着后为空，见 §6） | 几行 |
| 打包 | 新增 `cardterm-tmux.conf` + `Depends: tmux`（商店安装器会走 apt-get 解依赖，已核实） | control.in + 1 文件 |
| 文档 | README/CHANGELOG/商店文案补"会话保活/剥离/恢复" | 若干行 |

## 6. 已知限制（沿用可行性报告 §4）

1. 重附着后 **cardterm 原生回滚历史为空**（tmux client 只流转可见屏）——权威历史在 tmux 侧（copy-mode）；
2. tmux server 是 cardterm 后代 → subreaper **僵尸 caveat**（client 死后 server 常驻不易触发退出，影响小）；
3. 双层终端：快捷键约定（前缀键默认 `C-b`，可在 conf 里改 `C-a` 之类避开 cardterm 快捷键——**待定项**）。

## 7. 开发前待办 / 待定项

- [ ] **设备安装 tmux**（`apt-get install tmux`，3.5a-3，454KB）——**写设备操作，需你确认后执行**；
- [ ] 真机冒烟：SSH/ADB 下 `tmux -L cardterm` 基本读写、53×17 下 TUI（htop/vim）实测；
- [ ] 待定：前缀键选择（避开 cardterm 快捷键）；
- [ ] 待定：FN+Q 默认=剥离，"彻底退出"放哪个键（Fn 层空间紧张，可先只做剥离）；
- [ ] 待定：恢复时"枚举∪默认窗口"的细节（全部恢复 vs 只恢复非空会话）。

## 8. 实测环境说明

宿主机 tmux 3.4（x86_64 Ubuntu）逐项验证 §2；设备 tmux 未安装（只读阶段），候选 3.5a-3（trixie 三源可达）。
3.4→3.5 上述语义（命名/`-A`/`has-session`/destroy 行为）无变更记录，仍需真机复核 §7 第 2 项。
