# foot / fbterm 滚回缓冲（scrollback）与 resize 机制分析

> 分析对象：foot（MIT，Daniel Eklöf）与 fbterm（GPLv2，dragchan）的本地克隆源码。
> 目标：弄清两者的滚回缓冲数据结构，以及 **resize（行/列数变化）时滚回与屏幕内容如何变化**，
> 为 cardterm 的字号切换（FN+U/I，网格列/行数变化）提供"resize 保缓冲"的参考方案。
> 只做分析，未修改任何源码；代码片段均为少量引用（foot 为 MIT，可自由引用；fbterm 为 GPLv2，此处仅作评注性短引用）。

**源码路径**（本报告引用的行号以此为准）：

| 项目 | 路径 | 关键文件 |
|---|---|---|
| foot | `.tmp/foot/`（完整源码树） | `grid.c` / `grid.h` / `terminal.h` / `terminal.c` / `render.c` / `commands.c` / `config.c` |
| fbterm | `project_clones/fbterm/` | `src/lib/vterm.h` / `src/lib/vterm.cpp` / `src/lib/shell.cpp` / `src/fbshell.cpp` / `src/screen.cpp` / `src/fbconfig.cpp` |

---

## 1. 滚回缓冲的数据结构

### 1.1 foot：一个 2 的幂环形缓冲，屏幕 + 滚回是同一块内存

foot 没有独立的 "scrollback 队列"。**屏幕和滚回历史是同一个环形缓冲**，
`struct grid`（`terminal.h:220-247`）持有 `struct row **rows`——一个 `struct row *` 指针数组，
容量 `num_rows`（= 滚回行数 + 屏幕行数，向上取整到 2 的幂），行号换算靠掩码取模：

```c
// grid.h:34-38
static inline int
grid_row_absolute(const struct grid *grid, int row_no)
{
    return (grid->offset + row_no) & (grid->num_rows - 1);
}
```

- `grid->offset`：屏幕第 0 行在环里的绝对下标；
- `offset` 之前（环序上）的行就是滚回历史，`offset + r`（r < 屏幕行数）是屏幕第 r 行；
- 行指针可以为 `NULL` = 尚未写过的空行（惰性分配，见 `grid_sb_start_ignore_uninitialized()`，`grid.c:53-63`）。

**一行怎么存**（`terminal.h:148-161`）：

```c
struct row {
    struct cell *cells;      // 定长数组，长度 = grid->num_cols
    struct row_data *extra;  // 行外挂：OSC-8 URI 区间、下划线区间（不占 cell 空间）
    bool dirty;
    bool linebreak;          // ★ 硬换行标志（见 §3 reflow）
    struct { ... } shell_integration;
};
```

每个 cell 定长 12 字节（`terminal.h:68-72`）：

```c
struct cell {
    char32_t wc;              // 码点；组合字符用 CELL_COMB_CHARS_LO..HI 索引；宽字符第 2 格放 CELL_SPACER+n
    struct attributes attrs;  // 8 字节位域：bold/italic/.../fg:24/bg:24
};
```

- **定长 cell 数组**，每行 `num_cols` 个；不是变长。
- 属性就存在 cell 里（12 字节全带宽度语义：宽字符用 `CELL_SPACER` 标记后继格），
  行级不重复存属性；URI/下划线这类"区间"属性放 `row->extra`，保持 cell 定长。
- `linebreak` 布尔值记录"这一行是硬换行结尾还是自动折行（soft wrap）"——这是 rewrap 的关键（§3）。

**容量上限**：配置 `scrollback.lines`，默认 **1000** 行（`config.c:3571`）；
环总行数在 `render.c:4689-4703` 计算：

```c
const unsigned scrollback_lines_not_yet_power_of_two =
    min((uint64_t)term->render.scrollback_lines + new_rows - 1, max_scrollback);
const int new_normal_grid_rows =
    min(1u << (32 - __builtin_clz(scrollback_lines_not_yet_power_of_two)), max_scrollback);
```

即 **滚回行数 + 屏幕行数 向上取整为 2 的幂**（掩码取模要求）；alt-screen 网格不带滚回
（`new_alt_grid_rows = pow2(new_rows)`，`render.c:4703`）。理论上限 1<<30（`render.c:4670-4688` 注释）。

**写满后淘汰**：环形覆盖。reflow 的 `_line_wrap()`（`grid.c:698-715`）在新行落回已被占用的环槽时
直接复用该行（`/* Scrollback is full, need to reuse a row */`），即最老的行被无声覆盖；
平时滚动则由 `offset += rows` 的掩码换行自然实现"顶出最老行"。

### 1.2 fbterm：两块扁平二维数组 + 行号重映射表 + 历史环

fbterm 的 `VTerm`（`src/lib/vterm.h:178-184`）把整个终端存成 **扁平二维数组**：

```cpp
u16 *text;              // 码点，1 个 u16/cell
CharAttr *attrs;        // 每 cell 一个属性位域（vterm.h:26-37：fg/bg/intensity/italic/.../type）
u16 *linenumbers;       // 屏幕行 y -> 物理行下标 的重映射表
u16 width, height, max_width, max_height;
```

内存布局（`vterm.cpp:207-209`）：

```cpp
text  = new u16      [new_max_width * (history_lines + new_max_height)];
attrs = new CharAttr [new_max_width * (history_lines + new_max_height)];
```

- 物理行 `0 .. history_lines-1`：**历史环**（写游标 `history_save_line`，`history_full` 标志）；
- 物理行 `history_lines .. history_lines+max_height-1`：屏幕行，经 `linenumbers[y]` 做旋转重映射
  （滚动时不搬 cell，只改 `linenumbers` 的指向，`vterm.cpp:651-672`）。
- **一行 = 定长 `max_width` 个 cell**（行距 = `max_width`）；cell = u16 码点 + 4 字节 CharAttr。
  宽字符占 2 格（`CharAttr::DoubleLeft/DoubleRight`，`vterm.h:28`），没有 foot 那样的 SPACER 码点，
  也没有行级 `linebreak` 标志——**fbterm 完全不区分硬换行与自动折行**。
- 属性逐 cell 内联存储（含颜色位域），行本身没有变长结构；滚回时按当前 `width` 拷贝、
  补空格到 `max_width`（`vterm.cpp:785-793`）。

**容量上限**：配置 `history-lines`，默认 **1000**，范围 0–65535（0 = 禁用），
`static u16 history_lines` 全局共享（`fbconfig.cpp:186-187`、`fbshell.cpp:310-316`）。

**写满后淘汰**：同样是环覆盖（`vterm.cpp:796-800`）：

```cpp
history_save_line++;
if (history_save_line == history_lines) {
    history_save_line = 0;
    if (!history_full) history_full = true;
}
```

---

## 2. 屏幕与缓冲的关系

### 2.1 新行滚出屏幕时如何进缓冲

**foot：零拷贝——行不"进"缓冲，它本来就在缓冲里。**
滚动只是推进 `offset`（`terminal.c:3094-3101`，`term_scroll_partial`）：

```c
bool view_follows = term->grid->view == term->grid->offset;
term->grid->offset += rows;
term->grid->offset &= term->grid->num_rows - 1;
if (likely(view_follows)) {
    ...
    term->grid->view = term->grid->offset;
}
```

行随 `offset` 越过屏幕边界自动变成"历史"，没有任何拷贝/转移。反向滚动（`terminal.c:3176-3200`）
则把将被屏幕重新覆盖的行**释放**（`grid_row_free`，行指针置 NULL）再转 `offset`——
即"行从历史里拉回屏幕"时反而是丢弃历史槽位所有权的时机。

**fbterm：深拷贝进历史环 + 屏幕槽位回收。**
`scroll_region()` 在整屏上滚时（`vterm.cpp:638-641`）：

```cpp
if (start_y == 0 && num > 0) {
    history_scroll(num);
}
```

`history_scroll()`（`vterm.cpp:778-805`）把将滚出顶部的 `num` 行 **memcpy 拷贝**进历史环
（按当前 `width` 拷贝、`width..max_width` 补空格），随后 `linenumbers` 轮转回收屏幕行槽位。
所以是"拷贝入环 + 指针轮转"，不是所有权转移。

cardterm 现状与此同构：`scroll_up()` 里 `T->s_hist.push_back(T->s_buf[0])`（`terminal.cpp:524`）
即 fbterm 式"顶行拷入 deque"。

### 2.2 滚回视图与 follow-bottom

| | foot | fbterm |
|---|---|---|
| 视图表示 | `grid->view`：环内绝对下标（与 `offset` 同空间） | `visual_start_line`：**距底部的行数**（0=贴底），范围 `[0, total_history_lines()]` |
| follow 判断 | `grid->view == grid->offset`（`terminal.c:3094`） | `visual_start_line == total_history_lines()`（`vterm.cpp:769` 光标可见性判断） |
| 新输出时 | 跟随时 `view = offset` 同步推进；不跟随时若滚动越过了视图则把视图往下推（`terminal.c:3097-3103`） | **任何输入都强制回底**：`input()` 开头 `if (visual_start_line != total_history_lines()) historyDisplay(true, total_history_lines())`（`vterm.cpp:286-288`） |
| 滚动是否重置视图 | 不重置（仅跟随时同步） | `history_scroll()` 末尾 `visual_start_line = total_history_lines()`（`vterm.cpp:803`）——一旦有行滚入历史，视图强制贴底 |

视图寻址：foot 用 `grid_row_abs_to_sb()` / `grid_row_sb_to_abs()` 在"环绝对坐标 ↔ 滚回相对坐标"
之间换算（`grid.c:33-51`）；fbterm 的 `get_line(y)`（`vterm.cpp:854-862`）直接解析：

```cpp
u16 line = visual_start_line + y;
if (line >= total_history_lines()) return linenumbers[line - total_history_lines()];
return ((history_full ? history_save_line : 0) + line) % history_lines;
```

---

## 3. resize / 窗口尺寸变化时的行为

### 3.1 foot：真正的 rewrap（重排）

入口 `render_resize()`（`render.c:4529-`），核心分两路：
`grid_resize_and_reflow()`（全量重排，`grid.c:815-1298`）与 `grid_resize_without_reflow()`
（截断快路径，`grid.c:470-637`）。

#### 列数变化 → **rewrap（合并/拆分行）**

`grid_resize_and_reflow` 从**最老的滚回行**开始遍历整个旧环
（`int offset = grid->offset + old_screen_rows;`，`grid.c:846`），把 cell 逐个重新"发射"进新网格：

- **变宽（合并）**：依赖每行的 `linebreak` 标志区分逻辑行。自动折行的行（`linebreak=false`，
  由 `print_linewrap()` 在折行瞬间置位，`terminal.c:3892`）会被**并回上一行**：

  ```c
  // grid.c:924-927
  if (!old_row->linebreak && col_count > 0) {
      /* Don't truncate logical lines */
      while (col_count < old_cols && old_row->cells[col_count].wc == 0)
          col_count++;
  }
  ```

- **变窄（拆分）**：新行放不下时 `line_wrap()` 换行继续放；宽字符跨不出新行宽时补
  `CELL_SPACER` 并把字符推到下一行（`grid.c:1000-1016`）；宽字符比一整行还宽时退化为空格
  （`grid.c:1095-1117`）。
- 尾部空行的连续硬换行被合并（`coalesced_linebreaks`，`grid.c:1158-1175`），避免空窗口把 prompt 顶出屏幕。
- 行上挂的 URI/下划线区间随 reflow 跨行开闭（`reflow_range_start/end`，`grid.c:640-696`）。

#### 行数变化 → 无需搬动，窗口在同一个环上滑动

因为屏幕 + 历史是一体的环：

- **行数变多**：屏幕窗口变高，顶部**自动露出更多历史行**（内容不动，`offset` 重定位后
  `offset = new_row_idx - new_screen_rows + 1`，`grid.c:1216-1221`，让最后一行内容贴屏幕底）；
- **行数变少**：顶部行自然落入历史区，光标被 clamp 到新底（`cursor.row = min(cursor.row, new_screen_rows - 1)`，`grid.c:1251`）。

#### resize 时缓冲会不会丢弃

- reflow 路径：**不主动丢弃**；只有当重排后的总行数超过新环容量时，`_line_wrap` 复用最老的环槽
  （自然淘汰最老历史）。
- `grid_resize_without_reflow` 路径：**丢弃全部滚回**，只拷贝可见屏幕行、列方向截断
  （`new_offset = 0; grid->view = grid->offset = new_offset;`，`grid.c:604`）。它用于：
  1. alt-screen 网格（无滚回语义，`render.c:4919-4921`）；
  2. **交互式拖拽 resize 过程中的快速截断**（`render.c:4875-4885`）——
     拖拽期间先 stash 一份原始网格快照（`term->interactive_resizing.grid`，`render.c:4755-4770`，
     与 URL 模式共用的 `grid_snapshot` 思路，`grid.c:273-401`），显示用截断网格凑合，
     拖拽结束（或停顿 `resize-delay-ms`）后由 `delayed_reflow_of_normal_grid()`
     （`render.c:4381-4425`）**一次性**对原始快照做完整 reflow 再换回。
- 另有 `resize-keep-grid` 配置（`config.c:1042`）：浮动窗口按格子对齐窗口像素
  （`set_size_from_grid`，`render.c:4511-4527`），让网格尺寸不变、从根本上回避 resize。

#### resize 与视图 offset 的交互

reflow 前记录 `view_follows = grid->view == grid->offset`（`grid.c:832`）；
不跟随时把视图当作 **tracking point** 跟着 reflow 一起重映射坐标
（`struct coord viewport = {0, grid->view};`，`grid.c:874`；光标、saved cursor、选区端点同理）。
reflow 后用滚回相对坐标夹取视图（`grid.c:1239-1245`）：

```c
int sb_view = grid_row_abs_to_sb(
    grid, new_screen_rows, view_follows ? grid->offset : viewport.row);
grid->view = grid_row_sb_to_abs(
    grid, new_screen_rows, min(sb_view, new_rows - new_screen_rows));
```

即：**跟随状态保持跟随；不跟随时视图内容尽量保持（换算成"距底部/距顶部的行数"再夹回合法范围）**，
offset 与 view 都不会悬空失效。

### 3.2 fbterm：不 rewrap，缓冲基本原样保留

fbterm 全屏跑在 framebuffer 上，`mCols/mRows` 启动时由 `屏宽/FW(1)` 一次算定
（`screen.cpp:80-81`），运行期没有窗口 resize；`VTerm::resize()`（`vterm.cpp:145-271`）
主要服务于初始设尺寸（`FbShell` 构造时 `resize(screen->cols(), screen->rows())`，`fbshell.cpp:353`；
`Shell::resize` 附带 `TIOCSWINSZ`，`shell.cpp:119-130`）。但它的语义仍值得参考：

#### 列数变化 → **完全没有 rewrap**

- `max_width` **只增不减**（`new_max_width = (w > max_width) ? w : max_width`，`vterm.cpp:148`），
  缓冲仅在上限增长时重分配；历史行逐行按 `max_width` 行距 **原样 memcpy**，
  新增列补空格（`vterm.cpp:213-237`）：

  ```cpp
  for (u16 j = max_width; j < new_max_width; j++) {
      new_text [new_start + j] = ' ';
      new_attrs[new_start + j] = default_char_attr;
  }
  ```

- 变窄时**不截断存储**，只是显示宽度变小；此后滚入历史的行按当前 `width` 拷贝、
  `width..max_width` 补空格（`vterm.cpp:785-793`）——即"存的时候裁掉"。
- 变宽时屏幕新露出的列被清掉（`clear_area(minw, 0, w - 1, h - 1)`，`vterm.cpp:263-265`），
  历史行却保留旧列内容（存多宽就显示多宽）——**折行只发生在显示端，行本身永远是物理行**。

#### 行数变化

- **变少**（`h < height`）：若光标会掉出屏幕，把顶部 `cursor_y - h + 1` 行**滚入历史**
  （借用 `scroll_region(0, height-1, cursor_y-h+1)`，`vterm.cpp:249-254`），
  光标钳到新底；屏幕底部多余行被清掉。效果：**光标以上的进历史，光标以下的丢弃**。
- **变多**（`h > height`）：底部补空行（`clear_area(0, minh, minw, h - 1)`，`vterm.cpp:266-268`），
  **不从历史拉回内容**（与 foot 相反：foot 行数变多会自动露出更多历史）。

#### resize 时缓冲会不会丢弃 / 视图怎么办

- 历史环不丢不缩（缓冲只增）；`history_save_line / history_full` 原样保留。
- `visual_start_line` 是"距底部行数"，不指向移动中的下标，因此 resize 后天然有效，无需修正；
  若 shrink 触发了 `scroll_region` → `history_scroll`，末尾会把视图强制拉回底部
  （`vterm.cpp:803`）；`h` 变化时发 `historyChanged` 回调（`vterm.cpp:260-262`）通知 UI 刷新进度条。

---

## 4. 差异总结

| 维度 | foot | fbterm |
|---|---|---|
| 数据结构 | 单个 **2 的幂环形缓冲**（`row*` 指针数组），屏幕+历史一体；行=定长 `num_cols` cell 数组（12B/cell，属性内联），行外挂 URI/下划线区间 | 两块**扁平二维数组**（text/attrs），历史区=行号环 + `linenumbers` 重映射；行=定长 `max_width` cell（u16+CharAttr） |
| 容量/淘汰 | `scrollback.lines` 默认 1000，+屏幕行取整 2 的幂；环覆盖淘汰最老 | `history-lines` 默认 1000（0–65535，全局）；`history_save_line` 环游标覆盖 |
| 行滚出屏幕 | **零拷贝**，`offset += n` 即入历史 | **memcpy 进历史环** + linenumbers 轮转 |
| 硬换行标记 | 有（`row.linebreak`），区分 soft wrap | **无**，无法区分逻辑行 |
| 列数变化 | **rewrap**：soft-wrap 行合并/拆分，宽字符防拆，URI 区间跨行续接 | **不 rewrap**：物理行原样，显示端折行；存历史时按当前 width 裁剪补空 |
| 行数变化 | 环上滑动窗口：增多自动露历史，减少顶行入历史 | 增多=底部补空行；减少=顶部滚入历史（保光标以上），光标以下丢弃 |
| resize 丢内容 | reflow 路径仅按容量自然淘汰；快速路径丢全部历史 | 不丢（缓冲只增），shrink 掉屏幕尾部 |
| 视图 offset | 环绝对下标 + `view==offset` 跟随判断；reflow 时作为 tracking point 重映射并按"滚回相对坐标"夹取 | `visual_start_line`（距底部行数）；resize 免修正；输入/滚入历史强制贴底 |
| 结论 | **复杂但完整**：支持 reflow、跟踪点重映射、交互 resize 快照+延迟重排 | **简单**：无 reflow，resize 语义保守（裁剪/补空），胜在结构朴素 |

**哪个更简单**：fbterm——没有 rewrap、没有逻辑行概念，代价是列数变化后历史显示错位/截断、
行数增多看不到历史。**哪个支持 reflow**：只有 foot。

---

## 5. 对 cardterm 的启示（resize 保缓冲方案）

cardterm 现状（`terminal.cpp:81-97`）：per-window `std::deque<Row> s_hist`
（`Row = Cell c[kMaxCols]`，kMaxCols=80 定长）+ 屏幕 `s_buf` + `s_view`（0=贴底，N=向上翻 N 行）；
`state_set_font()`（`terminal.cpp:959-988`）字号切换时直接 `s_hist.clear()` + 清屏。
以下方案按改动量从大到小排列：

### 方案 A：foot 式 rewrap（推荐给"列数变化"，可保留全部历史与逻辑行）

1. 给每行加一个 `bool hard_break`（等价 foot 的 `row.linebreak`，`terminal.h:153`）：
   自动折行处（`s_wrap_pending` 触发的换行）置 false，显式换行/回车置 true。
2. 字号切换时把 `s_hist + s_buf` 视为逻辑行流，按新 `s_cols` 重新排放 cell：
   soft-wrap 行合并（变宽）/拆分（变窄），宽字符放不下补 spacer 推下一行
   ——直接对照 foot `grid_resize_and_reflow`（`grid.c:815-1298`）的发射循环。
3. cardterm 的 Row 已是 kMaxCols=80 定长，rewrap 只需在 deque 里重建行序列，
   无需分配器改造；kMaxCols ≥ 任何字号下的列数（320/4px=80），可继续复用。

参考：foot `grid.c:815-1298`（rewrap 主循环）、`grid.c:924-927`（soft-wrap 合并）、
`terminal.c:3892`（折行时打 linebreak=false）。

### 方案 B：fbterm 式"不重排、只重映射"（最小改动的保缓冲）

若 rewrap 成本（行数×列数逐 cell 搬运）在换字号瞬间不可接受，可退一步保缓冲不重排：

1. 字号切换时 **不清 `s_hist`、不清屏内容**：Row 本来就是 kMaxCols 定长 cell 数组，
   列变窄只是显示端截断（现有 `kCols` clamp 已天然支持），列变宽时旧内容原样可见
   ——这正是 fbterm `resize()` 的语义（`vterm.cpp:213-237` 原样拷贝 + `vterm.cpp:263-268` 新列清空；
   cardterm 甚至比 fbterm 干净：Row 始终存 80 列，不存在 fbterm "存历史时按当前 width 裁剪"的损耗）。
2. 行数变化按 fbterm 的折中：**变少**时把顶部 `旧rows - 新rows` 行 push 进 `s_hist`
   （等价 `vterm.cpp:249-254` 的 scroll_region 技巧，保住光标以上的输出）；
   **变多**时更进一步——不必学 fbterm 补空行，可从 `s_hist` 尾部 pop 回屏幕顶部
   （deque 结构下是 O(1) 的，且比 fbterm 体验好，接近 foot"增多自动露历史"）。
3. 把 `apply_grid()` 后的 `s_col/s_row` 按比例 clamp 到新网格（fbterm 用
   `move_cursor(w-1, ...)` clamp，`vterm.cpp:256-258`）。

参考：fbterm `vterm.cpp:145-271`（grow-only 缓冲、行数增减）。

### 方案 C：视图 offset 的修正与防失效（A/B 都建议加上）

1. 保持 `s_view` 的"距底部行数"语义不变（cardterm 已是，`terminal.cpp:392-393`；
   与 fbterm `visual_start_line` 同构）——resize 后天然不失效，只需
   `s_view = min(s_view, (int)s_hist.size())` 夹回合法范围；
   若采用方案 A 的 rewrap，行总数变化后同样按"距底部行数"夹取
   （对照 foot `grid.c:1239-1245` 的 `min(sb_view, new_rows - new_screen_rows)`）。
2. follow 判断继续用 `s_view == 0`（等价 foot `view == offset`）；rewrap 时先记录
   `bool view_follows = (s_view == 0)`，跟随则重排后仍贴底，不跟随则视图内容尽量保留
   （foot 把视图作为 tracking point 跟着重映射，`grid.c:874`；cardterm 用"距底部行数"表达即可）。
3. 防抖：FN+U/I 连按时不要每次都全量 rewrap——foot 的做法是 resize 过程先用截断网格顶着、
   停顿后对**原始快照**一次性重排（`render.c:4755-4770` stash + `render.c:4381-4425` 延迟 reflow）；
   cardterm 可简化为：按键触发时记 dirty + 50–100ms 合并窗口，只在最终字号上重排一次，
   并同步发一次 `TIOCSWINSZ`（`state_set_font` 的调用方已有此约定）。

---

## 附：关键代码位置速查

| 主题 | 位置 |
|---|---|
| foot 环下标换算 | `.tmp/foot/grid.h:34-44` |
| foot 行/cell 结构 | `.tmp/foot/terminal.h:148-161`、`68-72` |
| foot 滚动入历史（零拷贝） | `.tmp/foot/terminal.c:3089-3110` |
| foot reflow 主循环 | `.tmp/foot/grid.c:815-1298` |
| foot soft-wrap 合并 | `.tmp/foot/grid.c:924-927`；折行打标 `.tmp/foot/terminal.c:3892` |
| foot reflow 后 offset/view 重建 | `.tmp/foot/grid.c:1216-1221`、`1239-1245` |
| foot 无 reflow 快路径（丢历史） | `.tmp/foot/grid.c:470-637`（`grid.c:604`） |
| foot 交互 resize 快照 + 延迟重排 | `.tmp/foot/render.c:4755-4770`、`4381-4425` |
| foot 环容量计算 | `.tmp/foot/render.c:4689-4703`；默认 1000 行 `.tmp/foot/config.c:3571` |
| fbterm 缓冲布局 | `project_clones/fbterm/src/lib/vterm.cpp:207-240` |
| fbterm resize 全函数 | `project_clones/fbterm/src/lib/vterm.cpp:145-271` |
| fbterm 滚动入历史（memcpy） | `project_clones/fbterm/src/lib/vterm.cpp:632-677`、`778-805` |
| fbterm 视图寻址 get_line | `project_clones/fbterm/src/lib/vterm.cpp:854-862` |
| fbterm 输入强制贴底 | `project_clones/fbterm/src/lib/vterm.cpp:286-288` |
| fbterm history-lines 配置 | `project_clones/fbterm/src/fbconfig.cpp:186-187`、`src/fbshell.cpp:310-316` |
| cardterm 现状（供方案落地） | `cardterm/src/terminal.cpp:81-97`（Row/s_hist/s_view）、`510-535`（scroll_up）、`959-988`（state_set_font） |
