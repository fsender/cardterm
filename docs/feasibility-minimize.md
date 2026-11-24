# 可行性评估：cardterm 最小化（回到启动器、保留会话、可恢复）

> 评估日期：2026-11-24 · 基线 HEAD `7fd9a3e`（cardterm 0.7.2）
> 结论：**暂缓**。cardterm 单方面实现“真正的最小化（保留活会话）”不可行；
> 需要 APPLaunch（上游启动器）提供挂起/恢复协议（外部依赖），或引入常驻会话守护进程
> （cardterm 内可行，但 3–5 人日、中等风险）。若只是想要“看起来像恢复了”的体验，
> 有一个 0.5–1 人日的低风险替代（窗口布局+历史文本恢复，不保留进程）。
> 本报告只做评估，未改任何代码。

---

## 1. 现状：两套“独占”是怎么叠起来的

### cardterm 侧（`src/exclusive_display.cpp` + `src/main.cpp` + `src/session.cpp`）

1. `main()` 调 `hw::acquire(/*grab_keyboard=*/false)`：
   - `flock(/run/lock/hw-display.lock)` 单实例锁；
   - `/dev/tty1` 切 `KD_GRAPHICS`；
   - 遍历 `/proc` 找 `M5CardputerZero-APPLaunch`，**SIGSTOP 冻结（不杀）**；
   - 备份整块 `/dev/fb0`（`fb_backup()`），退出时逐字节还原；
   - fork 一个看门狗子进程，cardterm 异常死亡时由它还原画面并 `SIGCONT` 启动器。
2. `session::run()` 用 `openpty()` 为每个窗口建 PTY，master fd 由 **cardterm 进程本身**持有；
   子进程 `setsid()+TIOCSCTTY` 成为会话首进程。
3. 退出语义：FN+Q/长按 Esc/信号 → `kill_all()` 对所有子进程 **SIGHUP → 宽限 → SIGKILL**，
   然后 `hw::release()` 还原画面、`SIGCONT` 启动器、返回启动器。
   **会话与 cardterm 进程同生共死**：master fd 一关，slave 侧收 HUP，shell/TUI 全部退出。

### APPLaunch 侧（上游 launcher，本次只读本地副本 `project_clones/launcher-src` 核实）

4. 启动外部程序（cardterm 就是 `Terminal=false` 的 `.desktop` 应用）走
   `Launch::launch_Exec()` → `cp0_signal_process_api({"ExecBlocking", exec, keep_root})`
   （`projects/APPLaunch/main/ui/launch.cpp`；调用前 `lv_refr_now`、`lv_timer_enable(false)`，
   调用后 `lv_timer_enable(true)` 并恢复首页，顺序被
   `tests/test_external_framebuffer_ownership.py` 逐点断言）。
5. `ExecBlocking` 的实现 `cp0_external_app_runner::run()`
   （`ext_components/cp0_lvgl/src/cp0/cp0_external_app_runner.cpp`）是**同步阻塞循环**：
   `keyboard_pause()` → `fork()` → 子进程 `setpgid` + `exec` →
   父进程（= APPLaunch 主线程）进入 `while(true)`：回收子进程、从板载键盘 evdev 读 ESC、
   执行“长按 ESC → SIGTERM/SIGKILL”策略、`sleep(10ms)`；
   **只有 cardterm 进程退出后**才 `keyboard_resume()`、打印 `[cp0] Returned to launcher` 并返回。
6. 平台层的进程 API **确实**有后台模式 `Spawn executable keep_root`（返回 PID）、`Stop`、`Kill`
   （`docs/cp0_lvgl.md`），但 **APPLaunch 的 UI/应用模型不使用它**：没有“应用切换器”、
   没有“重新前台化（re-raise）”、没有按键路由到后台应用。可选的自动化 RPC
   （ZeroMQ，`tcp://127.0.0.1:5557/5558`，需编译期开关）只能注入按键/文本、截图、ping——
   **不是窗口/应用管理协议**（`documents/dev/dev_debug_launcher_automation_rpc.md`）。

**一句话**：APPLaunch 的“前台”就是“阻塞等待这个子进程退出”。只要 cardterm 进程还活着，
启动器主线程就卡在 `ExecBlocking` 里，首页不会回来、按键不会被它处理。

---

## 2. 为什么“自挂 SIGSTOP + 启动器再 exec + 共享状态文件恢复”不可行

逐条对照题目里的设想：

| 设想 | 为什么不行 |
|---|---|
| cardterm 自己 `SIGSTOP` | 停止的是 cardterm 自己，APPLaunch 仍在 `while(true)` 里等这个子进程退出（§1.5）。启动器不会因此回到首页。 |
| 回到启动器后再“exec 一次 cardterm”恢复 | ① 启动器只在子进程退出后才恢复（§1.5），所以“回到启动器”要求 cardterm **退出**；② 一退出就没人持有 PTY master fd，子 shell 立即挂断（§1.3）；③ `SIGSTOP` 的进程不能被“exec 恢复”，它只能被 `SIGCONT`，而重新 `exec` 会创建**新进程**，拿不到旧进程的 fd/内存。 |
| 共享状态文件恢复 | 状态文件能存“窗口布局/回滚文本/命令历史”，但**存不了活的进程状态**：PTY 的 master 端在旧进程里，新进程无法接管（fd 不跨进程共享，除非通过已建立的 Unix socket `SCM_RIGHTS` 传 fd——而旧进程若已退出就无从传递）。 |
| PTY 保活是否需要常驻进程 | **需要**。PTY 的 master fd 必须有人一直持有并持续读走输出（否则 slave 写满 64KB 后子进程阻塞）。`SIGSTOP` 的进程不会读，等于把子进程冻在写阻塞上；真正“保活”必须有**运行中的**持有者。 |

另外两个现实约束，常被忽略：

- **键盘/输入**：`exclusive_display::acquire()` 冻结 APPLaunch 的同时，键盘 grab 由 `kbd` 独占。
  若 cardterm 释放显示但**不退出**，启动器即使被 `SIGCONT`，它自己的 `ExecBlocking` 循环
  还在按 ESC 策略读键盘（`keyboard_pause/resume` 只是让 LVGL 输入线程停），
  它会与 cardterm 争同一 evdev；ESC 长按还可能直接把 cardterm `SIGTERM/SIGKILL` 掉。
- **画面**：`/dev/fb0` 是全局唯一 surface。cardterm 还原备份帧只是让启动器**最后一帧**静止
  显示；启动器主线程阻塞中，这一帧不接受输入、也不刷新——用户看到的是“假首页”。
  按 HOME 键也无济于事（APPLaunch 的首页按键处理在 LVGL 主循环里，而主循环在 `ExecBlocking` 中）。

---

## 3. 可行路径（按代价排序）

### 路径 A：APPLaunch 提供“挂起/恢复”协议（需要上游配合）

**形态**：launcher 把外部应用从 `ExecBlocking` 改为“可挂起”的进程管理：
启动后继续跑 LVGL 主循环，记录 `pid/pgid`，首页增加“运行中的应用”入口；
选择 → 给应用发“恢复”信号（如 `SIGCONT` + 约定的共享内存/文件标志），应用重新
`acquire` 显示与键盘；应用主动“最小化”时释放显示/键盘并等待 `SIGCONT`/标志。

**cardterm 侧改动**：
- `hw::release()` 后不退出，进入“挂起等待”状态（释放 grab、还原 fb、`SIGCONT` 启动器后轮询恢复信号）；
- 增加“最小化”触发键（当前 Fn 层媒体键都被占用，需要定一个，如 `Ctrl+Alt+M` 或长按某键）；
- 恢复时重新 `hw::acquire()`（含锁、VT、冻结、fb 备份）并全屏重绘；
- 处理挂起期间的 SIGWINCH/输出积压（启动器在跑，PTY 输出继续，需要缓存上限与丢弃策略）；
- 看门狗/atexit 语义扩展（挂起中被杀也要还原）。

**工作量**：cardterm 侧 **2–3 人日**（含真机验证）；**外加 launcher 侧的开发/发布/兼容性**
（另一个仓库、另一个团队/上游，不在本仓控制内）。风险中：两个进程轮换持有 fb/键盘、
`SIGSTOP` 与 `SIGCONT` 的竞态、启动器版本兼容（用户可能装旧 launcher）、
watchdog 与挂起态的交互（历史上已有“看门狗撞僵尸”的 Critical 审计项）。

**判定：暂缓（依赖上游，且需求未确认）。**

### 路径 B：会话守护进程（唯一不需要 APPLaunch 改动的“真最小化”）

**形态**：最小化时 cardterm fork 一个 **detached 会话守护进程**（`cardtermd`），
它继承所有 PTY master fd 继续读、把每个窗口的输出写入有界环形缓冲（例如每窗 256KB；
12 窗 ≈ 3MB，真机内存余量足够——本项目 12 窗稳定态约 15MB）；
cardterm UI 释放显示/键盘后**正常退出** → APPLaunch 的 `ExecBlocking` 返回 → 首页恢复（真正回到启动器）；
守护进程在 `/run` 下监听一个 `AF_UNIX` socket，并写一个 pid/token 文件。
用户从启动器再打开 cardterm（或点击首页“恢复会话”）→ 新 UI 连上 socket → 守护进程把
缓冲输出回放给新 UI 重建终端状态 → 发送 `SIGWINCH` 让全屏 TUI（htop/crush）重绘。

**关键取舍**：
- UI 退出后 APPLaunch 正常回首页，这是本路径的最大优点（完全不用改启动器）。
- 但“恢复”不是原样恢复：回放的是**原始字节流**，需要重新解析；滚动历史超过环形缓冲的部分会丢；
  全屏 TUI 依赖 `SIGWINCH` 重绘（对 bash/htop/crush 一般有效，对不响应 SIGWINCH 的程序不保证）。
- 守护进程生命周期/清理：所有窗口都关掉、或空闲超时（如 10 分钟）、或收到信号时退出；
  还要防止多实例、防止 `kill -9` 后残留 pid 文件。
- 安全：socket 权限（`0600`，仅 `input`/同用户）、token 校验，避免本机其他进程劫持会话。
- 与看门狗/单实例锁的关系：挂起期间**不能**再持有 `/run/lock/hw-display.lock` 与键盘 grab；
  恢复时重新获取；launcher 的 ESC 策略在 UI 退出后重新生效（会向 cardtermd 发信号吗？
  不会——它监视的是当初 fork 的子进程，子进程已退出，所以不会误杀守护进程）。

**工作量**：MVP（fork 守护 + socket + 环形缓冲 + 回放 + 恢复后 SIGWINCH + 测试）
**3–5 人日**；高保真（保留每窗口完整输出/状态、精确重绘、崩溃恢复、systemd 化）
**6–8 人日**。风险中高：PTY 语义、回放正确性、TUI 重绘、fd 生命周期（本项目审计对 fd/CLOEXEC
极敏感）、以及“用户心智模型”——用户可能以为会话是完整恢复的。

**判定：暂缓。** 若产品确实需要“保留会话”的最小化，这是**唯一**可单方面落地的路线，
建议先做 MVP 并明确“滚动历史可能截断”的限制。

### 路径 C：低成本替代——窗口布局 + 历史文本恢复（不保留进程）

**形态**：退出/最小化时把每个窗口的 `cwd`、启动命令、字号、以及回滚文本（纯文本）
写入 `~/.local/state/cardterm/session.json`（或按窗口分文件）；
下次启动 cardterm 时提供“恢复上次窗口”（启动器里加一个入口或命令行 `--restore`）：
按记录重新 `spawn` 命令、切到记录的 `cwd`、把历史文本预先压入回滚缓冲（或直接显示为只读历史）。

**代价**：**0.5–1 人日**，低风险（不碰显示/PTY 生命周期，只加一个状态文件 + 启动参数）。
**局限**：进程不存活（正在跑的 `make`/`ssh` 不会继续），严格说不是“最小化”，
只是“会话形状恢复”。对“回到启动器、稍后接着用”的使用者仍有价值。

**判定：可选做（若只需要 UX 层面的‘恢复’）。**

---

## 4. 工作量/风险对比

| 路径 | 是否保留活会话 | 是否需改 APPLaunch | 工作量 | 风险 | 结论 |
|---|---|---|---|---|---|
| 自挂 SIGSTOP + exec + 状态文件（题目设想） | 否（技术上不成立） | — | — | — | **不可行** |
| A. launcher 挂起/恢复协议 | 是 | **是（上游仓库）** | cardterm 2–3 人日 + 上游协作 | 中 | 暂缓（外部依赖） |
| B. 会话守护进程（cardtermd） | 是（有损：滚动历史截断/TUI 重绘） | 否 | 3–5 人日（MVP）/ 6–8（高保真） | 中高 | 暂缓（需求未确认） |
| C. 布局+历史文本恢复 | 否 | 否 | 0.5–1 人日 | 低 | 可选的廉价替代 |

---

## 5. 结论与建议

1. **“最小化”在当前架构下需要架构级改动**：APPLaunch 把外部应用当作“阻塞的前台子进程”，
   没有任何“切回/再前台化”的协议；cardterm 的会话状态（PTY master、终端缓冲）与进程强绑定。
   题目设想的“自挂 SIGSTOP + 启动器再 exec + 共享状态文件”**不成立**；
   **PTY 保活必须有一个常驻的、正在运行的持有者**。
2. **推荐：暂缓**。若产品明确需要，按优先级：
   - 先做 **C（0.5–1 人日）** 验证需求是否只是“窗口/历史找回”；
   - 真需要活会话时做 **B 的 MVP（3–5 人日）**，并在 UI 上如实说明“历史可能截断”；
   - 只有 APPLaunch 愿意提供挂起/恢复协议时才考虑 **A**（体验最好，但依赖上游发布节奏）。
3. 无论走哪条路，都要先补一个**架构前置条件**：把当前“退出即 `kill_all()` 杀会话”
   的语义扩展到“挂起态”，并审计看门狗/单实例锁/键盘 grab 在挂起-恢复过程中的成对性
   （这块此前已有“看门狗撞僵尸导致设备冻结”“acquire 失败 release 空转”等 Critical 级历史问题，
   任何最小化改动都必须复用既有安全网并补测试）。

---

## 6. 附：本次核实所用文件

**cardterm**：`src/main.cpp`、`src/exclusive_display.{h,cpp}`、`src/session.cpp`
（`spawn()` 的 `openpty`/`setsid`/`TIOCSCTTY`、`kill_all()`）、`src/keyboard.cpp`、
`README.md`、`CHANGELOG.md`。

**上游 launcher（本地只读副本 `project_clones/launcher-src/`，不属于 cardterm 仓库）**：
`projects/APPLaunch/main/ui/launch.cpp`（`ExecBlocking` 调用与 LVGL 暂停/恢复）、
`projects/APPLaunch/tests/test_external_framebuffer_ownership.py`（顺序断言）、
`ext_components/cp0_lvgl/src/cp0/cp0_lvgl_process.cpp`（进程 API 命令表）、
`ext_components/cp0_lvgl/src/cp0/cp0_external_app_runner.cpp`（阻塞等待 + ESC 策略 + 子进程组）、
`ext_components/cp0_lvgl/src/cp0/cp0_process_lifecycle.*`（`Spawn/Stop/Kill` 后台能力）、
`docs/cp0_lvgl.md`（`Spawn` 等命令）、`docs/launcher工程详细说明/04-应用模型与启动机制.md`、
`AGENTS.md` §6.1（外部程序阻塞式启动）。

**工作区文档**：`documents/dev/dev_debug_launcher_automation_rpc.md`
（ZeroMQ 自动化 RPC：仅按键/文本/截图/ping，非应用切换协议）、
`documents/m5docs_zh_cardputerzero_product_page.md`（USB Host/Slave 开关与 Hub 拓扑，供外接键盘评估引用）。
