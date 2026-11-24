# 超简易终端模拟器 —— fbcon 实现逻辑复现笔记

> 项目：cardterm（LovyanGFX 纯 fbdev，M5Stack CardputerZero）
> 参考源码：`fbcon/`（torvalds/linux sparse 克隆，`drivers/video/fbdev/core/fbcon.c` 3452 行）

## 1. 规格

| 项 | 值 |
|---|---|
| 屏幕网格 | **53 列 × 17 行** |
| 字符块 | **6 × 10 像素** = 默认 GLCD 5×8 字形 + **上/下/右各留 1px** |
| 字体 | 默认字体（GLCD，`tft.drawChar` 透明底） |
| 输入 | 板载键盘（TCA8418C evdev）本地回显；Enter 换行、Bksp 退格、Tab 制表 |
| 退出 | **长按 Esc = 关闭终端窗口**（SIGHUP→宽限→SIGKILL，见 §4） |
| 颜色 | ANSI SGR：16 色 / 256 色 / `38;2;r;g;b`、`48;2;r;g;b` **真彩 RGB**（文字与背景均可自定义，TUI 应用可自定义显示颜色） |
| 默认色 | 字符色 = 调色板 **regular7**（fbcon 控制台默认属性 fg=7；SGR 0/39 复位到它）；背景 = `background_color`（未设背景色的单元格透出画布） |

## 2. fbcon → 本实现的对应关系

| fbcon（内核） | terminal.cpp（用户态） | 说明 |
|---|---|---|
| `vc_data.screenbuf`（u16 字符+属性字） | `Cell { ch, fg, bg }` 屏缓冲 | 属性字 nibble → RGB565 双色字段 |
| `fbcon_putcs()` 按属性取 fg/bg 画字符行 | `draw_cell()`（fillRect 背景 + 透明底字形） | 字形放 (0,+1)，右列留白 |
| `fbcon_clear()` 区域擦除 | `clear_region()` | 按当前属性填空格 |
| `fbcon_scroll()` 软滚动（行块搬移+重绘） | `scroll_up()`（`memmove` 行 + 重绘） | 超简易版不做硬件滚动/bmove |
| `fbcon_cursor()` 光标 | `cursor_draw()/cursor_erase()` | 5×1 下划线光标 |
| `get_fg_color/get_bg_color`（属性解码） | SGR 状态机写 `s_fg/s_bg` | 30-37/40-47/90-97/100-107/38;5;n/38;2;rgb/48;… |
| VT 控制台换行/回车/退格/制表 | `putc()` 的 `\n \r \b \t` | `\n`=下一行行首 |
| 旋转/余辉/边距等 fbcon 特性 | 不实现 | 超简易裁剪 |

## 3. ANSI 支持范围

- **SGR (`m`)**：0 复位、1 粗体（→ 对应亮色，同 fbterm）、7 反显、22/27 取消、30-37/39/40-47/49、90-97/100-107、
  `38;5;n`/`48;5;n`（256 色：16 标准 + 6×6×6 立方 + 灰阶）、`38;2;r;g;b`/`48;2;r;g;b`（RGB 真彩）
- **CUP (`H`/`f`)**、**ED (`J`)**、**EL (`K`)**、光标移动 `A/B/C/D`（TUI 常用最小集）
- 其它序列忽略（超简易）

## 4. 窗口关闭语义（长按 Esc ≡ 关闭终端窗口）

与普通电脑关闭终端模拟器窗口的行为一致：

1. 对窗口会话的子进程组发 **SIGHUP**（PTY 挂断语义）；
2. 宽限 **~1.5s** 等待其自行退出；
3. 仍未退出者 **SIGKILL**（关闭窗口杀死遗留进程语义）；
4. 退出程序，`hw::release()` 还原画面/键盘/启动器（窗口消失、回到桌面）。

`term::attach_process(pid)` 登记前台子进程（将来 shell/PTY 的挂接点）。
e2e 实测：关窗前 2 进程（主+会话子进程）→ 长按 Esc → `exit 0` → 关窗后 0 进程 ✓。

## 5. 架构（与既有模块的关系）

```
键盘(evdev, kbd 队列解耦) ──本地回显──▶ term::write/putc ──▶ 屏缓冲+ANSI 解析
                                                    │
                                                    ▼
                            LovyanGFX drawChar/fillRect → mmap fb（纯内存）
                                                    │
                                  内核 deferred-io ~50ms 后台上屏（永不阻塞）
```

`term::write()` 同时是"伪终端上屏口"：将来接 PTY 后，shell/TUI 应用输出直接灌入即可，
颜色/控制序列已就绪。

## 6. 背景图管线（PNG / JPG，内存解码 + 防碎片顺序）

`[colors] background_image=...`（**仅 PNG/JPG**；BMP 已移除，其它扩展名直接回退纯色）。

```
① malloc 画布 320×170×2B（唯一常驻，必须最先分配）
② fopen + fread 整个文件到内存（紧跟画布分配，不留小块间隔）
③ LGFX 内存解码 drawPng/drawJpg（tjpgd/pngle，不走 drawXxxFile；
   解码目标四周各留 1px 背景色边——AA 在图像边缘会向外采样，不留边会越界段错误）
④ free 文件内存（解码完成立即归还；堆内小文件块随后被 AA 目标精灵复用）
⑤ pushImageRotateZoomWithAA 拉伸铺满 320×170（不保长宽比）→ 字节交换 → 画布
```

要点（均经实测验证）：

- 必须传 `(const uint16_t*)buffer`：传 `void*` 会命中 LGFX 的 `bgr888_t` 重载（3 字节/像素），
  按 16bpp 数据读取会越界 → 段错误；传 `uint16_t*` 才按 swap565 16bpp 解释。
- 该 API 语义是 `dst = dst锚点 + (src − src锚点) × zoom`：**dst 锚点不是图片中心**。
  目标取 (0,0)、源取带边原点 (1,1) 才能整幅铺满画布（误传中心会让图片只出现在右下四分之一）。
- 图片不透明处覆盖画布；ANSI 显式设置背景色的单元格用 `fillRect` 直接盖住底图（优先于图片）。
- 内存顺序防止碎片化：常驻大块（画布）先分配；临时块（文件缓冲/解码精灵/AA 目标）
  按"分配→用完→立即释放"处理。

### 6.1 调色板与默认前景（与 fbcon 一致性）

- `regular0-7` → ANSI 索引 0-7（SGR `30-37` / `40-47`），`bright0-7` → 索引 8-15（`90-97` / `100-107`）；
  `256 色` 的 0-15 也走同一调色板。
- fbcon 控制台默认属性是 **fg=7**（`vc_video_erase_char = 0x0720` 语义），因此本实现的初始前景色
  取 `regular7`，SGR `0`（复位）与 `39`（默认前景）同样回到 `regular7`；默认背景是
  `background_color`（透出画布，未启用的单元格显示底色/底图）。
- 实测（16 色逐一采样 + 默认字符/背景）：与配置文件完全一致。

### 6.2 256 色 / RGB 模式与光标

- **256 色模式**（`38;5;n` / `48;5;n`）一律使用**标准 xterm-256 调色板**：0-15 为标准基色
  （黑/红/绿/…/亮白），16-231 为 6×6×6 立方，232-255 为灰阶 —— 不借用配置的 16 色；
  **16 色 SGR**（`30-37/40-47/90-97/100-107`）仍使用 `regular0-7/bright0-7`。
- **RGB 模式**支持分号与 ITU T.416 冒号两种写法：`38;2;r;g;b`、`38:2::r:g:b`、
  `38:2:0:r:g:b`、`38;5;n`、`38:5:n`、`48:2::r:g:b`、`48;5:n` 等；`ESC[m`（空参数）等价 `SGR 0`。
- 默认配置常驻背景图 `background_image=/home/me/Pictures/image1.png`（PNG/JPG，拉伸铺满画布）。
- 光标：**亮 500ms / 灭 500ms（周期 1s）**，全高方块，颜色 `bright7`；打字后立即亮起。
- 测试钩子：`CARDTERM_COLORTEST=1` 在提示符后输出 A/B/C 三行扩展色探针（256 前景/背景 0-15、
  冒号形式），便于逐格采样核对。
- **非 ASCII 字符**：有 ASCII 近似（框线/方块等）时用近似；否则画"豆腐块"占位——
  单宽 5×8、双宽 11×8（两个 5×8 方块 + 中间 1px 空隙），暂时替代真实 CJK 字形。

## 7. 伪终端会话（`cardterm <cmd> [args...]`）

带参数运行时，cardterm 通过 `openpty()` 启动子进程并作为其终端：

- 子进程 `setsid()` + `TIOCSCTTY` 获得控制终端；窗口 `53×17`（`TIOCSWINSZ`）；
  `TERM=xterm-256color`、`COLORTERM=truecolor`；
- 子进程输出 → 本终端解析上屏；键盘字节 → pty（Ctrl/Alt 组合、特殊键序列、
  Shift+PgUp/PgDn 翻看回滚，普通 PgUp/PgDn 发给应用）；
- `DSR`/`DA` 查询经回写通道应答；长按 Esc = 关窗（SIGHUP→宽限→SIGKILL），
  程序退出后返回其退出码并还原画面。

`cardterm bash`、`cardterm python3`、`cardterm ssh user@host`、`vim`、`less`、`htop`
均已实测可用；与 fbterm 的对齐细节见 `docs/fbterm-alignment.md`。

会话终止：长按 Esc（关窗 SIGHUP→SIGKILL）、Ctrl+D（EOF，程序退出即返回）、
输入 `exit`、程序自行退出/崩溃/被外部杀死（返回其退出码）、pty 关闭但进程仍活着
（SIGHUP→宽限→SIGKILL）——都会终止并回到之前的界面，同时向同进程组发 SIGHUP 清理后台作业。
