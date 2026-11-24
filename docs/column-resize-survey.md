# GUI 终端模拟器列数（宽度）变化处理调研

> 调研对象：xterm / VTE(gnome-terminal) / alacritty / kitty / wezterm 的源码（raw.githubusercontent 抓取单文件），
> 共性问题：缩放导致**列数变化**后 (1) 回滚行数语义 (2) 内存布局与峰值 (3) 视口/滚动位置保持。
> foot / fbterm 见 `scrollback-reflow-analysis.md`，本文不重复。
> 结论后括注 `文件:行号`（源码为 2026-11 master/main 抓取）或 URL；标"未核实"的请勿当定论。
> **一句话总结：rewrap 家族（VTE/alacritty/kitty/wezterm）全部按"rewrap 后的物理行"结算 scrollback 上限、超限丢最老行；只有 xterm 不重排；字节预算仅 kitty 的 pager 历史（MB）一家。**

## 1. xterm（不重排的"方案 B"鼻祖）

- **数据结构**：屏幕 + 滚回是两套 `ScrnBuf`（行指针表 + 数据块；滚回为 saveBuf/savedlines，行数固定）。
  行按当前列宽定长存（`LineData`，`lineSize` 现宽 / `maxLineSize` 存储宽），不存软换行合并结构。
- **列数变化**：`ScreenResize`（screen.c:2217）→ `Reallocate`（screen.c:447）**整体重新分配并逐行复制**，
  但**绝不 rewrap**：1 行还是 1 行，行尾内容在变窄时被截断。`save-lines buffer may change width, but will
  not change its height`（screen.c:2317 注释）——**行数语义完全不变**，`saveLines` 配额与列数无关。
  变窄截断不是绝对的：编译选项 `OPT_RESIZE_ADJUST`（ptyx.h:760，默认 0，提供 `-rca`）时按 `max(旧宽,新宽)`
  分配行存储（screen.c:471-487），**变窄保留隐藏列、变宽原样恢复**；默认编译 `allocCol=ncol`，变窄即丢。
- **视口**：`resizeGravity` 资源（ptyx.h:3435）；SouthWest 重力时底部锚定，变矮把行挤入 saveBuf
  （`saveEditBufLines`，screen.c:403）。
- **拖拽/防抖**：无 reflow 成本低，未见防抖机制（未核实）。**无字节预算**（按行）。

## 2. VTE（gnome-terminal 等；库级 reflow，可开关）

- **数据结构**：`Ring` = 行记录 + 三条流（`m_text_stream / m_attr_stream / m_row_stream`，ring.hh:218-231），
  行冻结后按 UTF-8 文本 + 属性增量压缩存（`freeze_one_row`，ring.cc:192），**紧凑变长，不按列定长**。
- **列数变化**：`Ring::rewrap(new_columns, markers)`（ring.cc:1376，设计文档 `doc/rewrap.txt`：
  段落=两个硬换行之间的软换行组，整段合并后按新宽重切）。**可配置**：`vte_terminal_set_rewrap_on_resize()`
  （vte.cc:10563，属性 `rewrap-on-resize`，默认 true——默认值为文档口径，未在源码核实）。
  仅列数变化才 rewrap（vte.cc:8249）；备用屏永不 rewrap（vte.cc:8373，bug 336238），
  主屏即使被备用屏遮住也 rewrap（vte.cc:8371，bug 415277）。
- **行数语义**：变窄拆行 → 行数变多，rewrap 末尾立即按上限砍**最老**行：
  `if (m_end > m_max) m_start = m_end - m_max`（ring.cc:1649）；变宽合行 → 行数变少、同额度存下更多文本。
  上限 = ring 容量 = `max(scrollback_lines, rows)`（`set_scrollback_lines`，vte.cc:10692-10716），
  rewrap 后重套一次（vte.cc:8377-8378 "Ensure scrollback buffers cover the screen"）。
  **配额按 rewrap 后物理行计，变窄会真实烧掉旧历史。**
- **内存**：rewrap 冻结全部行、新建文件流整体重建 ring（ring.cc:1439-1445），旧+新共存 → **峰值≈2 倍**
  （文本流可落盘，未必全在 RAM）。
- **视口**：7 个跟踪点（markers）转成文本偏移再回映射：光标、保存光标、视口下沿 `below_viewport`、
  当前段落下沿、选区起止（vte.cc:8210-8246）。滚动位置：在底保底、在顶保顶，
  否则"视口底行的硬换行保持原位"（vte.cc:8297-8311）。
- **拖拽**：每次 size_allocate 即 set_size+rewrap，未见防抖（未核实）。**无字节预算**。

## 3. alacritty（rewrap；最省内存的实现）

- **数据结构**：`Grid{raw: Storage<T>, max_scroll_limit}`（grid/mod.rs:115-137）；
  `Row<T> = Vec<T> + occ`（grid/row.rs:17-24），**行宽紧凑=当前列数**，软换行用末格 `WRAPLINE` 标志
  （term/mod.rs:958-968）。
- **列数变化**：`Grid::resize(reflow, ...)`（grid/resize.rs:14）；主屏 reflow、备用屏不 reflow
  （term/mod.rs: `self.grid.resize(!is_alt, ...)`）。变宽 `grow_columns`：末格带 WRAPLINE 的行并入下一行；
  变窄 `shrink_columns`：按新宽拆行（resize.rs:100-380）。
- **行数语义**：拆并后总行数截断到 `max_scroll_limit + lines`，**保留底部（最新）、丢顶部（最老）**
  （resize.rs:367-368 `reversed.truncate(...)`）。`max_scroll_limit = scrolling.history`，默认 **10,000**、
  硬上限 100,000 行（alacritty/src/config/scrolling.rs:5,34,47）。按 rewrap 后行数计。
- **内存**：drain 旧 `Vec<Row>` 构建新集合，行缓冲以 move/split_off/append 搬运，不做全量 cell 拷贝
  （resize.rs:121-231）；**峰值≈历史总量+两份行头**，比 VTE/kitty 的双缓冲低。
- **视口**：`display_offset`（距底行数）rewrap 后 `min(display_offset, history_size)` 夹取
  （resize.rs:241,372）——**按"距底行数"锚定**；vi 光标随内容平移再夹取（term/mod.rs:655-700）。
- **拖拽**：未见防抖（未核实）。**无字节预算**。

## 4. kitty（rewrap + 防抖最完善 + 唯一字节预算）

- **数据结构**：LineBuf（屏幕）+ HistoryBuf（分段环形，每段定行数、行=当前列宽的 CPUCell/GPUCell 数组，
  按需分段）；软换行记在行末格 `next_char_was_wrapped`（kitty/resize.c:350-368）。
  另有 **pagerhist：UTF-8 字节环形**，`scrollback_pager_history_size`（MB，≤4GB，约 1 万行/MB），
  **仅喂 pager、不参与交互滚动**（kitty/options/definition.py:757-770）。
- **列数变化**：`screen_resize`（kitty/screen.c:804）→ `rewrap()`（screen.c:321）→
  `resize_screen_buffers(..., HISTORY_CONSUME, ...)`（kitty/resize.c:383）：历史+屏幕作为一条流整体重 wrap；
  备用屏 `resize_screen_buffer_without_rewrap` 不 rewrap（screen.c:338）。
  列宽不变、只变行数时走 `historybuf_fast_rewrap` **零拷贝窃取存储段**（kitty/history.c:796-815）。
  变大后是否用历史填新空白行由 `scrollback_fill_enlarged_window`（默认 no，definition.py:774-778）控制。
- **行数语义**：新 HistoryBuf 容量 = 旧容量 `self->ynum`（history.c:776-778），
  构造时即 `MAX(scrollback_lines, 屏幕行数)`（screen.c:173）；写满丢最老。**按行计、resize 不改配额**，
  变窄拆行同样吃掉历史。字节预算只有 pagerhist。
- **内存**：先整体分配新 LineBuf+HistoryBuf（"All allocations must be done above…nothing below may fail"，
  resize.c:416-419），随后搬移/窃取；**列变时峰值≈2 倍**（另有 prompt_copy 防 prompt 闪烁，screen.c:838,913）。
- **视口**：光标+两个保存光标用 `TrackCursor` 跟踪，光标在内容外则保持在内容外（screen.c:818-821,887-893）；
  当前 shell 提示行不参与 rewrap 防抖动（`prevent_current_prompt_from_rewrapping`，screen.c:722）；
  滚动量 `clamp_scroll_position` 夹取。
- **拖拽**：**`resize_debounce_time` 默认 "0.1 0.5" 秒**——live resize 期间暂停向子进程要尺寸/重绘，
  静默 0.1s 后才收尾（definition.py:2048-2066）；另有 `resize_in_steps` 按格步进。

## 5. wezterm（rewrap；Windows/ConPTY 下退化为"不重排"）

- **数据结构**：`Screen{lines: VecDeque<Line>}`，容量 `physical_rows + scrollback_size()`（term/src/screen.rs:16-73）；
  termwiz `Line` 变长单元格序列，末格 `last_cell_was_wrapped` 标志。`scrollback_lines` 默认 **3500**
  （wezterm.org/config/lua/config/scrollback_lines.html）。
- **列数变化**：主屏 `rewrap_lines`（screen.rs:101-193）：按 wrapped 标志并逻辑行、`Line::wrap(new_cols)` 重拆；
  备用屏不 rewrap，只截断/标记重绘（screen.rs:238-247）。**无用户 reflow 开关**（配置索引无 reflow 项；
  搜索 "enable_reflow" 无结果——此前资料称 wezterm 可配置 reflow，**未能证实**）。
  Windows ConPTY 下 `resize_preserves_scrollback=true`：滚回视为不可变，只补空白行（screen.rs:267-290）。
- **行数语义**：rewrap 后 `capacity = rows + scrollback_size`；超限先删**尾部空白行**保历史
  （screen.rs:178-187），常规滚动路径 `pop_front` 丢最老（screen.rs:682,767）。按 rewrap 后行数计。
- **内存**：`rewrapped` 新 VecDeque 全量重建、drain 旧行 move 语义；**峰值≈2 倍**（行对象复用为主）。
- **视口**：以光标为锚反算 `new_cursor_y`（screen.rs:314-317，注释称 "bottom gravity"）；
  StableRowIndex 保证选区/复制跨 resize 稳定（term/src/lib.rs:61-75）。
- **拖拽**：未核实。**无字节预算**。

## 6. 其他（时间预算内未读源码，仅备忘）

konsole（HistoryType 行/文件两种历史，resize 行为未核实）、iTerm2、Windows Terminal（TextBuffer::Reflow，
scrollbackRows 行计）、rio、Contour 均未展开；来源类型：常识备忘，**未核实**。字节预算先例是 tmux
`history-size`（按字节，背景资料）。

## 7. 对比表

| 模拟器 | 行存储 | 列变宽 | 列变窄 | 行数语义 | 内存重分配 | 拖拽处理 | 字节预算 |
|---|---|---|---|---|---|---|---|
| xterm | 定长=现宽（可选 max 宽） | 不重排，空白补齐 | 不重排，截断（-rca 可恢复） | 行数 1:1 不变 | 全量 Reallocate 逐行复制 | 无（便宜） | 无 |
| VTE | 紧凑变长（文本+属性流） | 段落合并、行数减 | 拆行、行数增、丢最老 | max(scrollback,rows)，rewrap 后按物理行结算 | 全量重建流，峰值≈2x | 未见防抖 | 无 |
| alacritty | 紧凑变长（Row=Vec） | WRAPLINE 并行 | 拆行、truncate 丢最老 | history+屏幕行，rewrap 后结算（默认 10k） | move 语义重建，峰值≈1x+行头 | 未见防抖 | 无 |
| kitty | 定长=现宽，分段 | 并行（历史可上屏） | 拆行、写满丢最老 | 容量=MAX(scrollback,rows) 不变 | 全量新缓冲+交换，峰值≈2x | resize_debounce_time 0.1/0.5s | pagerhist（MB，仅 pager） |
| wezterm | 紧凑变长（termwiz Line） | 并逻辑行重拆 | 拆行、先删尾空白再丢老 | rows+scrollback，rewrap 后结算 | 全量重建，峰值≈2x | 未核实 | 无 |

## 8. 对 cardterm 的启示（现状：定长 `Row s_buf[32]`，Row=Cell[80]，历史 deque<Row>）

**a) 谁最像"方案 B（不重排、只重映射）"？** xterm——而且它的 `OPT_RESIZE_ADJUST`（ptyx.h:760）正是
方案 B 的加强版：行按 max(旧,新) 存储，变窄只藏不丢、变宽自动恢复。行数语义是 1:1（savedlines 与列数
无关，screen.c:2317），用户预期即"resize 不动历史、不多不少还是那些行"。我们 80 格定长甚至比 xterm 默认
更强：53→40→80 往返零损失。**方案 B 的语义与 xterm 数十年的用户习惯一致，不需要担心"行数对不上"的投诉。**

**b) rewrap 家族的上限怎么结算？有字节预算吗？** 全部按 **rewrap 后物理行**结算、超限丢最老：
VTE `ring.cc:1649`、alacritty `grid/resize.rs:368`、kitty ring push（容量不变）、wezterm `screen.rs:178`。
副作用：**变窄拆行会真实烧掉历史**（配 2000 行，缩小窗口后一半是新拆的碎片行）；变宽则同额度存更多文本。
按字节预算的只有 kitty `scrollback_pager_history_size`（MB，且仅 pager）与 tmux `history-size`；
GUI 终端交互滚动缓冲**没有**一家按字节计。我们定长 80×8B/格 → 640B/行近似常数，行预算≈字节预算，可预测。

**c) 具体建议**

1. **坚持方案 B，补齐两个 xterm 式细节**：每行记录 `wrapped`（软换行）与有效长度 `used_len`；
   列变窄时只改渲染映射、变宽时恢复显示（xterm -rca 的效果，但零拷贝）。行数恒 N 行、内存恒 N×640B，
   字号切换对回滚零冲击——这是定长行相对 VTE/kitty 的**结构性优势**，值得写进用户文档。
2. **给"字号切换"加防抖与一次性结算**：kitty 的 `resize_debounce_time`（definition.py:2048，0.1/0.5s）
   证明连 rewrap 家族都不敢每次 resize 事件都重排。我们若做任何重映射/截断动作，应在字号切换稳定后
   执行一次；拖拽/连续按键期间只重绘视口。
3. **若将来上紧凑行+reflow，抄 VTE 的骨架、抄 alacritty 的搬运**：rewrap 后按物理行 truncate 丢最老
   （VTE ring.cc:1649 / alacritty resize.rs:368），光标/视口用跟踪点回映射（VTE 7 markers，vte.cc:8210；
   视口锚定用"距底行数"，alacritty resize.rs:241），数据搬运用 move/split_off 把峰值压向 1x
   （alacritty），而不是 VTE/kitty 的全量双缓冲（2x 峰值在 512MB 设备上是硬成本）。
   同时明确告知用户：变窄会按行数配额丢最老历史（三家皆然，无更好先例）。

## 附：主要出处

- xterm: `charproc.c:9663`（调用）、`screen.c:2217 ScreenResize`、`screen.c:447 Reallocate`、`ptyx.h:760 OPT_RESIZE_ADJUST`
  — https://raw.githubusercontent.com/ThomasDickey/xterm-snapshots/master/{screen.c,ptyx.h}
- VTE: `src/ring.cc:1376 Ring::rewrap / :1649 上限`、`src/vte.cc:8189 screen_set_size / :8210 markers / :10563 set_rewrap_on_resize / :10692 set_scrollback_lines`、`doc/rewrap.txt`
  — https://raw.githubusercontent.com/GNOME/vte/master/{src/ring.cc,src/vte.cc,doc/rewrap.txt}
- alacritty: `alacritty_terminal/src/grid/resize.rs:14,368`、`grid/row.rs:17`、`term/mod.rs:655 Term::resize`、`alacritty/src/config/scrolling.rs:5,34`
  — https://raw.githubusercontent.com/alacritty/alacritty/master/alacritty_terminal/src/grid/resize.rs
- kitty: `kitty/screen.c:321 rewrap / :804 screen_resize`、`kitty/resize.c:383 resize_screen_buffers`、`kitty/history.c:751-815`、`kitty/options/definition.py:537,759,776,2048`
  — https://raw.githubusercontent.com/kovidgoyal/kitty/master/kitty/{screen.c,resize.c,history.c}
- wezterm: `term/src/screen.rs:101 rewrap_lines / :194 resize / :682,767 截断`、`term/src/lib.rs:61-75`、docs `scrollback_lines.html`
  — https://raw.githubusercontent.com/wez/wezterm/main/term/src/screen.rs
