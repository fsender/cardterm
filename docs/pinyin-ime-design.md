# cardterm 中文拼音输入法（1.3.0）设计备忘

> 状态：准备阶段（帮助部分已完成：`im` 帮助页 / Ctrl+H / Fn+空格键位表）。
> 引擎：**librime（中州韵后端）**；前端框架完全自研（不依赖 fcitx/ibus）。

## 1. 键位契约（已写入 `im` 帮助页，实现须逐条对齐）

| 键 | 行为 |
|---|---|
| Fn+空格 | 切换中/英文输入法 |
| Tab | 向后切换候选页；按住 Shift 则向前 |
| 空格 / Enter | 选中高亮词并上屏 |
| 数字 1~9 | 选中对应编号的词并上屏 |

## 2. 集成缝隙（按依赖顺序）

1. **键盘层** `keyboard.h/cpp`：新增 `kbd::Cmd::ImeToggle`（Fn+空格；run-loop 现有
   "Fn+Space 预留接线" 注释处接上）。候选翻页/选词键在 IME 激活时不再转发 PTY。
2. **引擎封装** `ime.{h,cpp}`（新增）：稳定 C API `rime_api.h`（单头文件，可 vendor）。
   - 链接：`-lrime`（设备已装 `librime1t64` 1.13.1 + lua/octagram 插件）；
     交叉编译取 Debian arm64 头文件或直接 vendor rime_api.h。
   - 生命周期：`RimeSetup/RimeInitialize` → `RimeStartMaintenance`（首次把
     luna-pinyin schema 编译成 .bin，之后只读加载）→ `RimeCreateSession`；
     键入 `RimeProcessKey`；取预编辑/候选 `RimeGetContext`；上屏 `RimeCommitText`。
   - 数据：设备已装 `rime-prelude rime-essay rime-data-luna-pinyin`
     （即 `im` 帮助页里的 apt 命令）。
3. **状态**：中/英切换与候选页号按**每窗口**独立（Win 结构加 ime 状态位）；
   IME 激活时启动器行编辑（launcher_key）与会话按键（send_key）先喂引擎，
   未消费才走原路径。
4. **候选 UI**：toast 同款圆角卡片叠加层（`kToastR` 外 8px/内 6px；其他半径用
   "外r + 内r-1 + 内r-2" 画法补 Bresenham 弧环空洞——2026-11-27 实验结论，
   见 helloworld_customfont/shots）。预编辑串 + `1..9` 编号候选，样式同帮助页配色。
5. **上屏通道**：焦点在启动器 → 写入 `w.line`；在会话窗口 → `feed_key` UTF-8
   （终端自身即前端，无 bridge.so/注入问题——cardterm 掌握键盘与 PTY）。

## 3. 设备现状（2026-11-27 实测）

- 运行库 6 件套齐备（librime/boost/yaml-cpp/marisa/leveldb/opencc），零缺口；
- `librime-bin`（rime_deployer 部署工具）在位，但非必需（`RimeStartMaintenance`
  可进程内自部署）；
- 版本：Debian librime 1.13.1（上游 1.18），C API 兼容。

## 4. 已知约束（rc002/rc003 实测）

- **部署（maintenance）在 CM0 上极慢**：`start_maintenance(true)`+join 会把
  schema_list 全部 8 个 schema 的词典从源码编译，实测 >22s 不返回（阻塞主循环）。
  **rc003 实现**：只做 `select_schema("luna_pinyin")` 就绪判定，失败才
  `deploy_schema(/usr/share/rime-data/luna_pinyin.schema.yaml)` 单 schema 部署
  （预编译 .bin 复制而非编译）；实测 select_schema 直接成功（用户目录已带
  部署产物），initialize 28~37ms，无阻塞。
- Fn+空格 键码通路：板载 Fn+Space 的 keymap2 仍映射 KEY_SPACE，靠 Fn 修饰态区分
  （`mod_effective(kModFn)`）；uinject2 需启用 >255 键码（KEY_DVD 389）才能注入。

## 5. rc003 实现要点（2026-11-27）

- **模块解耦**：`ime.cpp`（引擎+组词状态机，不碰像素）/ `ime_ui.cpp`（候选框与
  编辑行渲染，只读 ime 状态快照）/ terminal 仅增叠加层 API（overlay 钩子、
  光标格查询、配置调色板、文本原语、像素行标脏）。旧渲染路径零改动语义。
- **候选枚举**：`candidate_list_begin/next/end` 全量绝对序（截 999），分页按
  每页 9 条自行管理（Tab/Shift+Tab/PgUp/PgDn 环回，`N/M` 不补零）；选词走
  `select_candidate(绝对序)`；Enter 走 `process_key(Return)` 由 rime 原样上屏
  原始字母；Esc=clear、Bksp=process_key。高亮移动（方向键）为本地双列网格
  状态（左 0-4 / 右 5-8），不依赖 rime 内部菜单高亮。
- **残影治理**：ime_ui 记录上一帧覆盖矩形 + 编辑行文本，变化/消失时
  `mark_pixel_rows` 标脏旧行，由 tick 合帧路径重画终端内容（≤30ms 恢复）。
- **上屏通道**：焦点在启动器 → `w.line` 追加 + 回显；在会话窗口 → `feed_key`
  UTF-8。编辑行只画墨迹（regular7、透明底），不写终端缓冲、光标不移动。
- **`i` 符号框**：静态 36 条全角符号表复用同一候选框 UI；rime punctuator 兜底
  标点全角化。

## 6. 待决问题

- rc 版本号递增规则：`1.3.0-rc00x…`（未经用户指示不升级 1.3.0 正式号）。
