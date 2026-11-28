// ime.h — 中文输入法引擎接入 + 组词状态（1.3.0-rc003 拼音 / 中州韵后端）
//
// 职责（与 ime_ui 解耦：本模块只管引擎与状态，不碰像素）：
//   - 引擎动态加载（dlopen librime.so.1，未安装不构成构建/运行依赖），
//     用后常驻不自动释放（“输入完毕之后，不自动释放”）；
//   - 首次进入中文模式时部署 luna_pinyin 单 schema（预编译词典在位，免全量编译）；
//   - 组词状态机：预编辑串 / 候选（绝对序全量）/ 分页（每页 9 条，N/M 不补零）/
//     页内高亮 / 数字选词 / 空格上屏选中项 / Enter 上屏原始字母 / Esc 清空；
//   - `i` 符号框（符号表默认内置全角符号表，可用 config custom_symbols 自定义，
//     复用同一候选框 UI；rime 标点转换兜底）。
//
// 按键契约（handle_key）：name 为 kbd 回显名（"a"/"1"/" "/"Enter"/"Esc"/
// "Bksp"/"Tab"/"PgUp"/"PgDn"/"Up"/"Dn"/"Lt"/"Rt" 等），kev 给出修饰位。
// 返回 true = 已消费；*commit 非空 = 有 UTF-8 文本要上屏（调用方送 PTY/输入行）。
#pragma once

#include <string>

#include "keyboard.h"

namespace ime {

// 动态加载 rime 引擎并核验词典必要文件（luna_pinyin 预编译词典等）；
// 结果缓存（进程内只探测一次）。全部加载成功返回 true。（rc002 语义保留）
bool try_load();

// 依赖探针（帮助页/splash 状态显示用）：librime 可加载且 luna_pinyin 预编译
// 词典/配置齐备 = true。不初始化引擎、不占 rime 常驻内存。
bool deps_ok();

// 未完成组词的键入原文（去 rime 插入的分隔空格，如显示 "ni hao" → "nihao"），
// 并清空组词状态。中→英切换时按原文上屏用。
std::string take_raw();

// 进入中文模式：加载引擎并常驻（dlopen 后不释放）、首次部署单 schema、
// 建会话并选中 luna_pinyin。false = 引擎/数据不可用（提示“未安装”）。
bool begin();
// 退出中文模式：清组词/符号框；引擎与会话保留（不 finalize、不 dlclose）。
void end();
bool cn_mode();                 // 当前是否中文模式

// 输入法偏好（config [ime]，session 启动时灌入一次）：
//   enabled       — 功能开关（enable_zh_cn_input；false 时 begin() 直接失败）
//   script        — 简繁模式（simplified_traditional，首次生成 rime 用户配置
//                   luna_pinyin.custom.yaml 用，之后以该 rime 文件为准）：
//                   All 不转换（简繁字形都出候选）/ Simplified 仅简体 / Traditional 仅繁体
//   custom_symbols — `i` 符号框自定义符号串（custom_symbols；每字符一个符号，
//                   上限 891 个；空 = 内置全角中文符号表）
enum class Script { All, Simplified, Traditional };
void set_prefs(bool enabled, Script script, const std::string& custom_symbols);

bool composing();               // 组词中/符号框显示中（“正在输入中文”：快捷键屏蔽判据）
void cancel();                  // 强制清空组词/符号框（切窗/关窗等外部事件收尾）

// 按键入口（见文件头契约）
bool handle_key(const std::string& name, const kbd::KeyEvent& kev, std::string* commit);

// ── 渲染状态（ime_ui 每帧读取；只读快照）────────────────────────
struct UiState {
  bool        on = false;           // 中文模式开
  bool        text_visible = false; // 编辑行（拼音字母）显示中
  std::string preedit;              // 编辑行文本（rime 预编辑串，含分隔符）
  bool        box_visible = false;  // 候选框显示中
  std::string cand[9];              // 当前页候选文本（不含 "N." 前缀）
  int         ncand = 0;            // 本页条数（0..9）
  int         page = 0;             // 0-based 页号
  int         pages = 0;            // 总页数（≥1）
  int         sel = 0;              // 0-based 页内高亮（0..8）
  bool        symbol = false;       // i 符号框模式（cand 为全角符号）
};
void ui_state(UiState* out);

}  // namespace ime
