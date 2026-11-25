# 可行性调研：后台运行终端会话（会话脱离 cardterm 存活、可再附着）

> 调研日期：2026-11-26 · 基线：cardterm 1.0.1（staged 未提交）· **设备全程只读**（仅 adb shell 只读探查 + 本地源码阅读，未安装/未修改任何东西）
> 结论：**可行**。"会话级后台"（shell/后台任务在 cardterm 退出后继续存活、下次打开可再附着）**不需要改 APPLaunch**，cardterm 单方面可落地；
> "应用级后台"（cardterm 自身最小化后仍占着前台会话、随时切回）仍受 launcher `ExecBlocking` 架构约束，维持 `feasibility-minimize.md` 的"暂缓/需上游"结论。
> 本篇是 2026-11-24 `feasibility-minimize.md`（最小化评估）的**续篇**：补上当时未评估的 tmux / systemd --user 两条新路径，以及本轮真机只读实测数据。

---

## 1. 问题界定

| 概念 | 含义 | 可行性 |
|---|---|---|
| **会话级后台**（本篇主题） | shell / make / ssh 等**会话进程**在 cardterm UI 退出后继续跑；重新打开 cardterm（或从首页再点图标）能**重新附着**到原会话 | ✅ 可行，cardterm 单方面实现 |
| **应用级后台/最小化** | cardterm 自身不退出、释放显示后回首页，随时切回原画面 | ❌ 仍需 APPLaunch 提供挂起/恢复协议（见 `feasibility-minimize.md` §3 路径 A），暂缓 |

两者的共同前置：**PTY master fd 必须有一个"活着的持有者"**——UI 退出后没人读 master，子进程写满 64KB 缓冲就会阻塞，且 master 关闭会让内核向 slave 会话发 SIGHUP。因此"后台会话"必然意味着一个 **detached 持有进程**（守护进程或 tmux server）。

---

## 2. 本轮核实的事实（全部只读）

### 2.1 cardterm 现状（本地源码）

- `session.cpp spawn()`：`openpty()` → 子进程 `setsid()+TIOCSCTTY`，**master fd 由 cardterm UI 进程持有**；shell 已经在自己的会话/进程组里。
- 退出语义 `kill_all()`（`session.cpp:1222`）：对每个窗口 `SIGHUP → 1s 宽限 → SIGKILL`，再 `close(master)`。**会话与 UI 同生共死**——这就是"后台会话"目前不存在的原因。
- `check_exits()`：单窗口退出只弹 toast；只有前台窗退出且无其他开窗才关 cardterm。**多窗口"后台窗"已经存在**，但仅限 UI 存活期间。
- 退出路径：SIGTERM/SIGINT/SIGHUP 只置标志 → 主循环干净退出 → `atexit` 还原显示（`exclusive_display.cpp`）。

### 2.2 APPLaunch 约束（本地只读副本 `project_clones/launcher-src/`）

- `cp0_external_app_runner.cpp`：`fork + setpgid(0,0)` 后**阻塞等待**；长按 ESC → `killpg(app_pgid, SIGTERM)` → 3s → `killpg(SIGKILL)`。
- 关键点①：**kill 只打在整个 cardterm 进程组上**。shell 已 `setsid()` 本就在组外；任何再 `setsid()` 的守护进程也在组外 → **ESC 杀不掉后台会话**，且 launcher 的等待循环 `exists(app_pgid)` 会正常归零 → **UI 退出后 launcher 正常回首页**（与 `feasibility-minimize.md` §3-B 一致）。
- 关键点②（**新发现，本轮补查**）：launcher `enable_subreaper()`（`PR_SET_CHILD_SUBREAPER`），但回收只做 `waitpid(-app_pgid)`；cp0_lvgl 全目录 **grep 不到任何 `SIGCHLD`/`waitpid(-1)` 全局回收**。后果：**从 cardterm fork 出来的守护进程在 UI 退出后会 reparent 到 launcher；它将来退出时会变成僵尸，launcher 不会 reap，直到 APPLaunch 重启**。这是纯自研守护进程路径的新增 caveat（见 §3 P2/P3 对比）。
- `.desktop` 卡片（设备实读）：`Exec=/usr/share/APPLaunch/bin/cardterm`、`Terminal=false`；应用由 launcher 以配置用户启动，**实测运行 uid=1000（me）** → `systemd --user`、`/run/user/1000` 都可用。

### 2.3 设备实测（adb 只读，2026-11-26）

| 探查项 | 结果 |
|---|---|
| tmux / screen / byobu / dtach | **均未安装** |
| apt 源 | trixie 三源（USTC / TUNA / deb.debian.org）+ raspi 源；**`apt-cache policy` 可见 tmux 3.5a-3**（454 KB 下载 / 1.2 MB 装机）、screen 4.9.1-3 |
| 商店安装器 | AppStore `appstore_native.cpp:1939-1949`：依赖不满足时**自动改走 `apt-get -y`** → deb 声明 `Depends: tmux` **可被商店解析**（前提：设备 apt 源可达，本轮只读未实测下载） |
| systemd | **systemd 257**；`user@1000.service` 运行中；**`Linger=yes` 已开启**（用户服务可开机自启、登出不退）；`/usr/lib/systemd/user/` 里已有 `APPLaunch.service` 与多个 `.socket`（socket activation 有先例） |
| XDG_RUNTIME_DIR | `/run/user/1000` 存在（me:me 0700）→ AF_UNIX socket 落点 |
| 内存 | 总 446 MB，**可用 283 MB**，swap 445 MB（已用 47）；bash RSS 数 MB 级 → 12 个后台会话 + 每窗 256 KB 环形缓冲（≈3 MB）**余量充足** |
| CPU | 4×A53，load ~0.3；空闲会话零开销 |
| 登录 | sshd / adbd 常驻；`journalctl` 可用 |

---

## 3. 实现路径分析（按代价排序）

### P1 · tmux 封装（**新路径，本轮新增**）

**形态**：deb 增加 `Depends: tmux`（或 `Recommends:` + 运行时探测）。cardterm 启动窗口时把默认命令包成
`tmux new -A -s ct<N>`（已有会话即 attach）。tmux server 由 client 首次启动时 **fork+setsid 自我守护**，
cardterm 退出后照常存活；下次打开 cardterm 自动 attach 回原会话。

| 项 | 评估 |
|---|---|
| 工作量 | **0.5–1.5 人日**：shell 包装 + `use_tmux` 配置项 + tmux.conf（`status off` 省掉 320×170 上仅有的 17 行中的 1 行）+ 真机验证 |
| 保留活会话 | ✅（tmux server 持有 PTY，独立进程组，ESC 杀不到） |
| 改 APPLaunch | ❌ 不需要 |
| 回滚 | 配置关掉即回原生 spawn |
| 风险 | 低：tmux 语义成熟；TERM 已是 `xterm-256color` ✓；cardterm 的回滚缓冲作用在 tmux client 输出流上仍可用 |
| 缺点 | ① 引入外部依赖（商店安装需 apt 可达）；② **双层终端**：tmux 状态栏/前缀键与 cardterm 快捷键并存，需一份 tmux.conf 约定（关状态栏、改前缀、绑定 copy-mode 可选）；③ tmux server 同样是 cardterm 的后代 → **同样命中 §2.2 僵尸 caveat**（tmux server 很少退出，影响小但存在）；④ 滚动历史归 tmux 管理，与 cardterm 回滚缓冲并存时语义要定义清楚 |

**判定：验证需求的最快路径。** 若目标只是"命令跑着别断、回来还能接上"，先做 P1。

### P2 · cardtermd + systemd --user（socket activation）——**推荐终态**

**形态**：deb 增加两个用户单元（`/usr/lib/systemd/user/`，有 `APPLaunch.service` 先例）：

```
cardterm-sessions.socket   # ListenStream=%t/cardterm.sock（/run/user/1000/cardterm.sock）
cardterm-sessions.service  # ExecStart=cardterm --daemon；Restart=on-failure
```

- **PTY 由守护端创建**（UI 通过 socket 请求"开窗"，或启动时用 `SCM_RIGHTS` 移交 master fd）——UI 进程从此**纯渲染**，退出完全不碰会话；
- 守护端每窗维护有界环形缓冲（256 KB/窗），attach 时回放原始字节流重建终端状态，随后 `SIGWINCH` 让 htop/crush 等全屏 TUI 重绘；
- 生命周期：socket activation 首次连接即拉起；全部会话关闭或空闲超时（如 10 min）自动退出；**开机常驻（Linger=yes 已开）**。

| 项 | 评估 |
|---|---|
| 工作量 | MVP（daemon + 协议 + 环形缓冲 + 回放 + SIGWINCH + 单测）**3–5 人日**；+systemd 单元/开机常驻/崩溃恢复 **5–7 人日** |
| 保留活会话 | ✅ 真后台，且**跨重启前常驻** |
| 改 APPLaunch | ❌ 不需要 |
| 僵尸 caveat | ✅ **规避**：daemon 是 user@1000 的后代（不经 cardterm fork），退出由 user manager 正常 reap |
| 风险 | 中高：协议设计、fd/CLOEXEC 审计（本项目历史 Critical 项）、回放正确性、TUI 重绘依赖 SIGWINCH、多实例/残留 socket 清理、socket 0600+token 防劫持 |

**判定：需要"产品级后台会话"时的正解。** 相比 11-24 评估的纯路径 B，新增 systemd 化解决了僵尸与开机常驻两个问题。

### P3 · 纯 cardtermd（不接 systemd，11-24 的路径 B 原案）

fork+`setsid` 守护 + `/run` 下 AF_UNIX socket + pid/token 文件。**3–5 人日**。
新增 caveat（本轮发现）：UI 退出后 daemon reparent 到 launcher（subreaper），**daemon 退出时留僵尸直到 APPLaunch 重启**；无开机常驻。
可接受则仍是合法的最小依赖方案；否则升级为 P2。

### P4 · APPLaunch 挂起/恢复协议（应用级后台）

launcher 把 `ExecBlocking` 改为可挂起的进程管理 + 首页"运行中应用"入口。体验最好（真·最小化切回），
但**依赖上游仓库/发布节奏**，维持 `feasibility-minimize.md` 的判定：暂缓。

### 对比总表

| 路径 | 活会话 | 改 APPLaunch | 工作量 | 开机常驻 | 僵尸问题 | 风险 | 判定 |
|---|---|---|---|---|---|---|---|
| P1 tmux 封装 | ✅ | 否 | **0.5–1.5 人日** | 需另配（tmux server 非开机起） | 有（影响小） | 低 | **先做，验证需求** |
| P2 cardtermd + systemd | ✅ | 否 | 3–5（MVP）/ 5–7（systemd 化） | ✅（Linger=yes） | ✅ 规避 | 中高 | **产品级正解** |
| P3 纯 cardtermd | ✅ | 否 | 3–5 人日 | ❌ | 有 | 中高 | P2 的降级替代 |
| P4 launcher 协议 | ✅ | **是（上游）** | 2–3 + 上游 | ✅ | — | 中 | 暂缓 |
| （参照）C：布局+历史文本恢复 | ❌ 不保进程 | 否 | 0.5–1 人日 | — | — | 低 | 廉价替代，见 minimize 篇 |

---

## 4. 无论选哪条路都要处理的硬约束

1. **退出语义分叉**：`kill_all()` 的"退出即 SIGHUP 杀会话"必须改成按模式分支——后台模式下 UI 退出**不杀会话、只关 master（移交后连 master 都不关）**；看门狗/`atexit` 只还原显示，不得误伤守护端。
2. **fd 审计**：守护端绝不能继承 fb0 / evdev grab / 显示锁 / tty1（`spawn()` 现有"关闭 ≥3 全部 fd"的教训要复用）；CLOEXEC 逐 fd 核对。
3. **显示锁与单实例锁**：后台态必须已释放 `/run/lock/hw-display.lock`（否则下次启动拿不到锁）；恢复时成对 acquire。
4. **回放是有损的**：环形缓冲外的滚动历史丢失；全屏 TUI 靠 SIGWINCH 重绘（不响应的程序不保证）；UI 上要如实提示。
5. **内存预算**：12 窗 × 256 KB ≈ 3 MB 缓冲 + 每会话 shell 数 MB；283 MB 可用 + 445 MB swap 足够，但需在文档写明"后台会话数"建议上限。
6. **安全**：socket `0600`（`/run/user/1000` 已是 0700）+ token 校验；tmux 路径则依赖 tmux 自身的 socket 权限。

## 5. 建议的落地顺序

1. **先 P1（tmux）**：1 人日内上线，真实用户反馈能区分需求是"会话保活"还是"应用切回"；
2. 需求确认为"会话保活"且要产品化 → **P2 MVP**（先把 daemon+socket+回放跑通，systemd 单元随后补）；
3. P4 仅在上游明确愿意配合时启动。

## 6. 本轮只读探查未覆盖（留待实现时真机验证）

- apt 源**实际下载**是否可达（`apt-cache policy` 有候选 ≠ 能拉下来；本轮禁止写设备）；
- tmux 在 53×17、`TERM=xterm-256color` 下 TUI 实测（htop/vim 宽字符、resize 行为）；
- APPLaunch 对 user unit / `.socket` 的实际交互（如是否受 polkit 限制 `systemctl --user`）；
- 僵尸 caveat 的实证（构造 daemon 退出场景观察 launcher 子进程表）。

## 7. 核实所用文件/数据源

**cardterm（本地）**：`src/session.cpp`（`spawn()`/`check_exits()`/`kill_all()`）、`src/exclusive_display.cpp`（信号/atexit）、`src/applaunch_env.h`、`docs/feasibility-minimize.md`、`CHANGELOG.md:257`。

**launcher 只读副本**：`ext_components/cp0_lvgl/src/cp0/cp0_external_app_runner.cpp`（阻塞循环+killpg）、`cp0_external_process_group.hpp`（subreaper/waitpid(-pgid)）、grep 全目录无 `SIGCHLD`；`projects/APPLaunch/main/ui/launch.cpp`（ExecBlocking）；`projects/AppStore/main/backend/appstore_native.cpp:1939-1949`（apt-get 依赖解析）。

**设备（adb 只读，serial CardputerZero-b46fe11c）**：`which/dpkg -l`（无 tmux）、`apt-cache policy tmux screen`、`systemctl --version`、`loginctl show-user me`（Linger=yes）、`ls /usr/lib/systemd/user/`、`free -m`、`ls /run/user/1000`、`cat cardterm.desktop`、`ps`（应用 uid=1000）。
