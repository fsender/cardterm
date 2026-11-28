// ime.cpp — 中文输入法引擎接入 + 组词状态（1.3.0-rc003）
//
// 职责（渲染在 ime_ui.cpp，本文件不碰像素）：
//   ① 引擎动态加载：dlopen librime.so.1 → rime_get_api() 版本化函数表 →
//      核验词典必要文件 → RimeSetup/Initialize；用后常驻（“输入完毕之后，
//      不自动释放”），未使用中文输入时不占内存（dlopen 惰性）。
//   ② 首次进入中文模式：建会话；luna_pinyin 未注册则 deploy_schema 单 schema
//      部署（预编译词典在位 → 复制产物而非全量编译；勿用 start_maintenance，
//      全量部署在 CM0 上 >22s）。用户目录的 luna_pinyin.custom.yaml 提供简繁/
//      简繁初值（config [ime] simplified_traditional
//      生成），其增删改在下次引擎加载时重部署生效，用户可改它做简繁/标点切换。
//      随后 select_schema("luna_pinyin")。
//   ③ 组词状态机（handle_key）：
//        字母/标点 → rime process_key（标点由 punctuator 转全角并直接上屏）；
//        空格/数字 1~9 → select_candidate（页内高亮/编号，绝对序）→ 上屏选中词；
//        Enter → rime 原样上屏键入的字母串（get_commit 原始输入）；
//        Esc → 清空；Bksp → 删输入；Tab/PgDn 下页、Shift+Tab/PgUp 上页（环回）；
//        方向键 → 页内 1~9 高亮移动（双列网格：左 0-4 / 右 5-8）。
//   ④ `i` 符号框：组词为空时按 i → 符号表（内置全角表，config custom_symbols
//      可自定义，上限 891 个）走同一候选框 UI。
//
// rime_api.h 取自 rime/librime（BSD License，Copyright RIME Developers），
// 动态绑定 rime_get_api()，cardterm 不链接 librime（未安装也能跑）。
#include "ime.h"
#include "rime_api.h"

#include "exclusive_display.h"   // hw::release（退出守卫：先还原显示）

#include <dlfcn.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace ime {
namespace {

// ── 引擎句柄（进程级常驻）────────────────────────────────────────
void*          s_dl = nullptr;
RimeApi*       s_api = nullptr;
RimeSessionId  s_sess = 0;
bool           s_loaded = false;    // 引擎+会话+schema 均可用
bool           s_cn = false;        // 中文输入模式开

// ── 组词快照（rime 键处理/选词后刷新）─────────────────────────────
bool                    s_composing = false;
std::string             s_preedit;
std::vector<std::string> s_cands;   // 全部候选（绝对序，截 cand_cap()）
int                     s_page = 0; // 0-based
int                     s_sel = 0;  // 0-based 页内高亮

// ── i 符号框（静态表；cand 文本即符号本身）────────────────────────
bool s_symbol = false;
int  s_sym_page = 0, s_sym_sel = 0;
// i 符号框符号表：config [ime] custom_symbols 自定义（每字符一个符号）；
// 空 = 内置默认表（70 项，1.3.0 用户规格；8×14 字库全覆盖无豆腐块）。
// 旧键 symbols=half 的半角预设由 config 层映射进 custom_symbols，本文件只认这一张表。
constexpr int kMaxSyms = 891;                 // 自定义符号上限（99 页 × 9）
const char* kSymsFull[] = {
  "，", "。", "？", "！", "、", "：", "；", "—", "…",
  "“", "”", "（", "）", "《", "》", "「", "」", "‘",
  "’", "【", "】", "〈", "〉", "『", "』", "￥", "℃",
  "·", "①", "②", "③", "④", "⑤", "⑥", "⑦", "⑧",
  "⑨", "⑩", "≤", "≥", "±", "≠", "≈", "∞", "√",
  "°", "↑", "↓", "←", "→", "■", "□", "●", "○",
  "◉", "◎", "△", "▷", "▽", "◁", "▲", "▼", "◆",
  "◇", "＠", "＃", "％", "＆", "※", "～",
};
std::vector<std::string> s_syms;              // 生效符号表（UTF-8 单字符项）
void build_syms(const std::string& custom)
{
  s_syms.clear();
  if (custom.empty()) {
    for (const char* p : kSymsFull) s_syms.push_back(p);
    return;
  }
  const char* p = custom.c_str();
  while (*p != '\0' && (int)s_syms.size() < kMaxSyms) {
    const unsigned char c = (unsigned char)*p;
    int len = 1;
    if ((c & 0xE0) == 0xC0) len = 2;
    else if ((c & 0xF0) == 0xE0) len = 3;
    else if ((c & 0xF8) == 0xF0) len = 4;
    else if ((c & 0xC0) == 0x80) { p++; continue; }   // 孤立续字节：跳过
    bool ok = true;
    for (int i = 1; i < len; i++)
      if ((((unsigned char)p[i]) & 0xC0) != 0x80) { ok = false; break; }
    if (!ok) break;
    s_syms.push_back(std::string(p, len));
    p += len;
  }
}
int sym_count() { return (int)s_syms.size(); }
const std::string& sym_item(int i) { return s_syms[i]; }

constexpr int kPerPage  = 9;        // 每页候选数（双列：左 5 右 4+状态行）
// 候选索引上限分级（用户规格）：preedit 无分隔符 = 键入一个字的拼音 →
// 891 条（99 页），确保同音字全部索引得到；单字母（非 a/e/o 完整音节）
// 与含分隔符的多字输入 → 90 条（10 页）。
constexpr int kMaxCandsOne  = 891;
constexpr int kMaxCandsMulti = 90;

// 引擎数据（Debian rime-data-* 包布局；预编译词典在 build/ 下）
constexpr const char* kSharedDir   = "/usr/share/rime-data";
constexpr const char* kPrebuiltDir = "/usr/share/rime-data/build";
constexpr const char* kSchemaId    = "luna_pinyin";
constexpr const char* kSchemaFile  = "/usr/share/rime-data/luna_pinyin.schema.yaml";

bool file_ok(const char* p)
{
  struct stat st;
  return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

// 7 汉字上限：一次组词最多 7 个音节（≈7 个汉字），preedit 里最多 6 个分隔符
// （rime 自动插入的 0x20；手动分隔符 ' 一并计数）。超限 = 本次按键会令输入
// 超过 7 个汉字——撤销并丢弃刚键入的字母（7 个本身是允许的）。
int sep_count(const std::string& s)
{
  int n = 0;
  for (char c : s) if (c == ' ' || c == '\'') n++;
  return n;
}
constexpr int kMaxSep = 6;                 // 7 音节 ↔ 6 个分隔符

// 候选索引上限（kMaxCandsOne/kMaxCandsMulti 分级）：preedit 含分隔符 =
// 至少两个字（例：aq → a q → 安全…）→ 90；单字母是缩写匹配的重灾区，
// 除 a/e/o（本身即一个字的完整拼音）外 → 90；其余 = 一个字的拼音
// （无分隔符）→ 891，同音字全部索引得到。
int cand_cap(const std::string& pre)
{
  if (sep_count(pre) > 0) return kMaxCandsMulti;
  if (pre.size() == 1) {
    const char c = pre[0];
    if (c != 'a' && c != 'e' && c != 'o') return kMaxCandsMulti;
  }
  return kMaxCandsOne;
}

// ── 简体默认（rime 用户配置）──────────────────────────────────────
// 首次运行向 rime 用户目录写 luna_pinyin.custom.yaml（simplification 开关
// 默认开 = 简体）；之后该文件的增删改一律尊重用户（删文件/改 reset:0 = 繁体，
// 用户可借此做简繁转换）。变化在下次引擎加载时重新部署单 schema 生效。
constexpr const char* kCustomYamlName = "luna_pinyin.custom.yaml";
constexpr const char* kStampName      = ".cardterm_luna_pinyin.stamp";
bool s_pref_enabled = true;                  // config [ime] enable_zh_cn_input
Script s_pref_script = Script::All;          // config [ime] simplified_traditional
std::string s_pref_custom_symbols;           // config [ime] custom_symbols
// 默认 luna_pinyin.custom.yaml（首次生成；此后该文件归用户，删/改 = 自定义）
std::string default_custom_yaml(Script script)
{
  std::string s;
  s += "# cardterm 生成的 rime 用户配置（luna_pinyin.custom.yaml）。\n";
  s += "# 由 config.ini [ime] 的 simplified_traditional 首次写入；\n";
  s += "# 此后本文件归用户：直接改下面的值，重启 cardterm 生效（删本文件 = 恢复默认）。\n";
  s += "# 简繁（switches/@2/reset）：1=启用字形转换，0=不转换（简繁字形都出候选）；\n";
  s += "#   仅简体 = 1 + 默认 t2s.json；仅繁体 = 1 + 下一行的 s2t.json。\n";
  s += "patch:\n";
  if (script == Script::Traditional) {
    s += "  switches/@2/reset: 1\n";
    s += "  simplifier/opencc_config: s2t.json\n";
  } else {
    s += "  switches/@2/reset: ";
    s += (script == Script::Simplified) ? "1" : "0";
    s += "\n";
  }
  return s;
}

void mkdir_p(const char* dir)
{
  std::string d(dir);
  for (size_t i = 1; i < d.size(); i++) {
    if (d[i] == '/') { d[i] = '\0'; mkdir(d.c_str(), 0755); d[i] = '/'; }
  }
  mkdir(d.c_str(), 0755);
}

bool write_text_file(const char* path, const std::string& text)
{
  FILE* f = fopen(path, "w");
  if (f == nullptr) return false;
  const size_t n = fwrite(text.data(), 1, text.size(), f);
  fclose(f);
  return n == text.size();
}

std::string read_text_file(const char* path)
{
  std::string out;
  FILE* f = fopen(path, "r");
  if (f == nullptr) return out;
  char buf[256];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
  fclose(f);
  return out;
}

// 配置指纹：mtime + 大小（不存在 = "absent"），变化即需重新部署
std::string file_sig(const char* path)
{
  struct stat st;
  if (stat(path, &st) != 0) return "absent";
  char b[64];
  snprintf(b, sizeof b, "%lld %lld", (long long)st.st_mtime, (long long)st.st_size);
  return b;
}

// 退出守卫（on_exit，带退出码）：先幂等还原显示/键盘/APPLaunch，再 _exit
// 跳过 librime 静态析构（见 engine_load 注册处）
void ime_exit_guard(int status, void*)
{
  hw::release();
  fflush(nullptr);
  _exit(status);
}

uint64_t now_ms()
{
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return (uint64_t)tv.tv_sec * 1000 + (uint64_t)tv.tv_usec / 1000;
}

void refresh();          // 前置声明
void take_commit(std::string* out);

// ── rime 键处理 + 上屏收割 ────────────────────────────────────────
// 返回 rime 是否消费该键（未消费时调用方放行给程序）。
bool rime_key(int keysym, int mask, std::string* commit)
{
  if (s_sess == 0 || s_api == nullptr) return false;
  const Bool handled = s_api->process_key(s_sess, keysym, mask);
  refresh();
  take_commit(commit);
  return handled != 0;
}

// 选中绝对序 idx 的候选（空格/数字共用）；选中后可能直接上屏
void select_at(int idx, std::string* commit)
{
  if (s_sess == 0 || idx < 0 || idx >= (int)s_cands.size()) return;
  s_api->select_candidate(s_sess, (size_t)idx);
  refresh();
  take_commit(commit);
}

// ── 页内高亮移动（候选框/符号框共用双列网格语义）──────────────────
// 网格：左列 0-4（#1~#5）↔ 右列 5-8（#6~#9），第 5 行右侧是状态位。
//   上/下：列内移动（止于本列实际存在的行）；
//   左键·右列→左列同排（#6→#1 … #9→#4）；
//   左键·左列：第一页无动作；否则翻上页并落到上页右列同排（#1~#4→#6~#9，#5→#9）；
//   右键·左列→右列同排（#1→#6 … #4→#9；#5→#9），目标本页不存在则无动作；
//   右键·右列：最后一页无动作；否则翻下页并落到下页左列同排（#6~#9→#1~#4）。
void move_grid(const std::string& name, int n, int& page, int& sel)
{
  if (n <= 0) return;
  const int pages = (n + kPerPage - 1) / kPerPage;
  auto page_count = [&](int p) {
    const int rest = n - p * kPerPage;
    return (rest > kPerPage) ? kPerPage : (rest < 0 ? 0 : rest);
  };
  const int count = page_count(page);
  if (count <= 0) return;
  if (sel >= count) sel = count - 1;
  const bool left = sel < 5;
  const int row = left ? sel : sel - 5;

  if (name == "Up" || name == "Dn") {
    int r = row;
    if (name == "Up") r = (r > 0) ? r - 1 : r;
    else {
      int rows = left ? (count < 5 ? count : 5) : (count > 5 ? count - 5 : 0);
      if (rows > 4 && !left) rows = 4;
      r = (r < rows - 1) ? r + 1 : r;
    }
    sel = left ? r : 5 + r;
    if (sel >= count) sel = count - 1;
    return;
  }
  if (name == "Lt") {
    if (!left) { sel = (row < 4) ? row : 4; return; }      // 右列→左列同排
    if (page == 0) return;                                 // 第一页·左列：无动作
    page--;                                                // 翻上页 → 右列同排
    const int t = 5 + ((row < 4) ? row : 3);               // #1~#4→#6~#9；#5→#9
    const int c2 = page_count(page);
    sel = (t < c2) ? t : c2 - 1;
    return;
  }
  if (name == "Rt") {
    if (left) {
      const int t = 5 + ((row < 4) ? row : 3);             // 同排右列；#5→#9
      if (t >= count) return;                              // 右边没字了：无动作
      sel = t;
      return;
    }
    if (page + 1 >= pages) return;                         // 最后一页·右列：无动作
    page++;                                                // 翻下页 → 左列同排
    sel = (row < 4) ? row : 3;                             // #6~#9→#1~#4
    const int c2 = page_count(page);
    if (sel >= c2) sel = c2 - 1;
    return;
  }
}

void page_turn(int delta)
{
  const int n = (int)s_cands.size();
  const int pages = (n + kPerPage - 1) / kPerPage;
  if (pages <= 1) { s_page = 0; s_sel = 0; return; }
  s_page = (s_page + delta + pages) % pages;
  s_sel = 0;
}

// ── i 符号框 ─────────────────────────────────────────────────────
void symbol_open()
{
  s_symbol = true;
  s_sym_page = 0;
  s_sym_sel = 0;
  s_preedit.clear();
  s_cands.clear();
  s_composing = false;
}

void symbol_close()
{
  s_symbol = false;
}

// 符号框按键：返回 true = 已消费；关框后落到 rime 路径的键返回 false
bool symbol_key(const std::string& name, const kbd::KeyEvent& kev, std::string* commit)
{
  const bool sh = (kev.mods & 1) != 0;
  const int pages = (sym_count() + kPerPage - 1) / kPerPage;
  auto cur = [&]() { return s_sym_page * kPerPage + s_sym_sel; };
  if (name == "Esc" || name == "Bksp") { symbol_close(); return true; }
  if (name.size() == 1 && name[0] >= '1' && name[0] <= '9') {
    const int idx = s_sym_page * kPerPage + (name[0] - '1');
    if (idx < sym_count()) { *commit = sym_item(idx); symbol_close(); }
    return true;
  }
  if (name == " " || name == "Enter") {
    if (cur() < sym_count()) *commit = sym_item(cur());
    symbol_close();
    return true;
  }
  if (name == "Tab" || name == "PgDn" || name == "PgUp") {
    const int delta = (name == "PgUp" || (name == "Tab" && sh)) ? -1 : +1;
    s_sym_page = (s_sym_page + delta + pages) % pages;
    s_sym_sel = 0;
    return true;
  }
  if (name == "Up" || name == "Dn" || name == "Lt" || name == "Rt") {
    move_grid(name, sym_count(), s_sym_page, s_sym_sel);
    return true;
  }
  // 字母：用户其实想打拼音（如 im/help）——关符号框并把 i 与该字母一起送组词，
  // 保留 i 前缀不丢字（1.3.0-rc003 修订：原实现丢弃 i 导致中文模式打不出 im）
  if (name.size() == 1 && ((name[0] >= 'a' && name[0] <= 'z') ||
                           (name[0] >= 'A' && name[0] <= 'Z')) &&
      (kev.mods & 6) == 0) {
    symbol_close();
    rime_key((unsigned char)'i', 0, commit);
    rime_key((unsigned char)name[0], sh ? 1 : 0, commit);
    return true;
  }
  symbol_close();        // 其余按键：关符号框，回落到 rime/程序路径
  return false;
}

// ── 组词快照刷新 ─────────────────────────────────────────────────
void refresh()
{
  s_preedit.clear();
  s_cands.clear();
  s_page = 0;
  s_sel = 0;
  s_composing = false;
  if (s_sess == 0 || s_api == nullptr) return;

  RimeContext ctx;
  RIME_STRUCT_INIT(RimeContext, ctx);
  if (s_api->get_context(s_sess, &ctx)) {
    if (ctx.composition.preedit != nullptr) s_preedit = ctx.composition.preedit;
    s_api->free_context(&ctx);
  }

  RimeCandidateListIterator it;
  memset(&it, 0, sizeof it);
  const int cap = cand_cap(s_preedit);
  if (s_api->candidate_list_begin(s_sess, &it)) {
    do {
      if (it.candidate.text != nullptr) s_cands.push_back(it.candidate.text);
    } while ((int)s_cands.size() < cap && s_api->candidate_list_next(&it));
    s_api->candidate_list_end(&it);
  }
  s_composing = !s_preedit.empty();
}

void take_commit(std::string* out)
{
  RimeCommit c;
  RIME_STRUCT_INIT(RimeCommit, c);
  if (s_api->get_commit(s_sess, &c)) {
    if (c.text != nullptr) *out = c.text;
    s_api->free_commit(&c);
  }
}

// ── 引擎加载（常驻）──────────────────────────────────────────────
bool engine_load()
{
  if (s_loaded) return true;

  // ① 引擎库（dlopen：未安装即失败，不构成链接依赖）
  s_dl = dlopen("librime.so.1", RTLD_NOW | RTLD_LOCAL);
  if (s_dl == nullptr) {
    return false;
  }
  auto get_api = (RimeApi * (*)()) dlsym(s_dl, "rime_get_api");
  if (get_api == nullptr) { dlclose(s_dl); s_dl = nullptr; return false; }
  s_api = get_api();
  if (s_api == nullptr || s_api->setup == nullptr || s_api->initialize == nullptr ||
      s_api->create_session == nullptr || s_api->process_key == nullptr ||
      s_api->select_schema == nullptr) {
    dlclose(s_dl); s_dl = nullptr; s_api = nullptr; return false;
  }

  // ② 词典/配置必要文件（缺任一 = 输入法数据包未安装）
  const bool data_ok =
      file_ok("/usr/share/rime-data/build/luna_pinyin.table.bin") &&
      file_ok("/usr/share/rime-data/build/luna_pinyin.prism.bin") &&
      file_ok("/usr/share/rime-data/build/luna_pinyin.reverse.bin") &&
      file_ok("/usr/share/rime-data/build/luna_pinyin.schema.yaml") &&
      file_ok("/usr/share/rime-data/default.yaml");
  if (!data_ok) {
    dlclose(s_dl); s_dl = nullptr; s_api = nullptr; return false;
  }

  // ③ 初始化引擎（用户目录 ~/.local/share/cardterm/pinyin_data，XDG 用户数据：
  //    词频/自造词 luna_pinyin.userdb（librime 自动落库）与部署产物都在这里；
  //    旧版目录 ~/.local/share/cardterm/rime 一次性整目录迁移，用户数据不丢）
  RimeTraits traits;
  memset(&traits, 0, sizeof traits);
  traits.data_size = (int)sizeof(RimeTraits);
  const char* home = getenv("HOME");
  if (home == nullptr || *home == '\0') {
    // HOME 缺失：回退 passwd 家目录（不写死任何用户名）
    struct passwd* pw = getpwuid(getuid());
    home = (pw && pw->pw_dir && *pw->pw_dir) ? pw->pw_dir : "/tmp";
  }
  static char user_dir[256];
  snprintf(user_dir, sizeof user_dir, "%s/.local/share/cardterm/pinyin_data", home);
  char old_dir[256];
  snprintf(old_dir, sizeof old_dir, "%s/.local/share/cardterm/rime", home);
  {
    struct stat st;
    if (stat(user_dir, &st) != 0 && stat(old_dir, &st) == 0) rename(old_dir, user_dir);
  }

  // 简繁/标点初值：用户目录写 luna_pinyin.custom.yaml（文件存在 = 尊重用户；
  // 删本文件 = 恢复默认 → 按 config 重新播种，与文件头注释一致）；
  // 由 config [ime] 的 simplified_traditional 生成；
  // 指纹变化 → 下面重新部署单 schema 使配置生效
  char custom_path[320], stamp_path[320];
  snprintf(custom_path, sizeof custom_path, "%s/%s", user_dir, kCustomYamlName);
  snprintf(stamp_path, sizeof stamp_path, "%s/%s", user_dir, kStampName);
  mkdir_p(user_dir);
  if (!file_ok(custom_path))                        // 全新安装或用户删除：按 config 播种
    write_text_file(custom_path, default_custom_yaml(s_pref_script));
  const std::string sig_now = file_sig(custom_path);
  const std::string sig_old = read_text_file(stamp_path);
  const bool cfg_changed = (sig_now != sig_old);

  traits.shared_data_dir   = kSharedDir;
  traits.user_data_dir     = user_dir;
  traits.prebuilt_data_dir = kPrebuiltDir;
  traits.distribution_name = "cardterm";
  traits.app_name          = "rime.cardterm";
  traits.min_log_level     = 3;      // 只留 FATAL（"Missing ascii bindings" 等良性 ERROR 不刷屏）
  traits.log_dir           = "";
  s_api->setup(&traits);
  s_api->initialize(&traits);

  s_sess = s_api->create_session();
  if (s_sess == 0) {
    s_api->finalize();
    dlclose(s_dl); s_dl = nullptr; s_api = nullptr; return false;
  }

  // ④ schema 就绪判定：select_schema 失败或简繁配置变化 → 单 schema 部署
  //    （预编译复制）再试
  bool sel_ok = s_api->select_schema(s_sess, kSchemaId);
  if (sel_ok && cfg_changed) sel_ok = false;   // custom.yaml 增删改：重部署生效
  if (!sel_ok) {
    if (s_api->deployer_initialize != nullptr) s_api->deployer_initialize(&traits);
    if (s_api->deploy_schema != nullptr) s_api->deploy_schema(kSchemaFile);
    write_text_file(stamp_path, sig_now);      // 记录配置指纹（部署失败也不重写用户文件）
    // 部署后重建会话：switch reset（简体默认等）只在会话创建时套用，
    // 沿用部署前的旧会话会保留旧开关状态（表现为简体补丁不生效）
    s_api->destroy_session(s_sess); s_sess = 0;
    s_sess = s_api->create_session();
    if (s_sess == 0 || !s_api->select_schema(s_sess, kSchemaId)) {
      if (s_sess != 0) s_api->destroy_session(s_sess); s_sess = 0;
      s_api->finalize();
      dlclose(s_dl); s_dl = nullptr; s_api = nullptr; return false;
    }
  }
  // 退出守卫：librime 的 Service 静态析构在 exit() 清理会话/引擎时崩溃
  // （rime::Poet::~Poet SIGSEGV，Debian librime.so.1 实测），且会压住更早
  // 注册的 atexit(hw::release) 造成显示/键盘不还原。本处理器注册在引擎
  // 初始化之后（exit 时最先执行）：先还原显示，再 _exit 跳过雷区。
  on_exit(ime_exit_guard, nullptr);
  s_loaded = true;
  return true;
}

}  // namespace

// ── 公开 API ─────────────────────────────────────────────────────

// 依赖探针（帮助页/splash 用；不初始化引擎、不占 rime 常驻内存）：
// librime.so.1 可加载且 luna_pinyin 预编译数据/配置齐备 = true；
// 缺任一 = false（对应"缺少部分库"状态）。
bool deps_ok()
{
  static int s = -1;
  if (s < 0) {
    void* dl = dlopen("librime.so.1", RTLD_NOW | RTLD_LOCAL);
    const bool lib = (dl != nullptr) && (dlsym(dl, "rime_get_api") != nullptr);
    if (dl != nullptr) dlclose(dl);
    const bool data = file_ok("/usr/share/rime-data/build/luna_pinyin.table.bin") &&
                      file_ok("/usr/share/rime-data/build/luna_pinyin.prism.bin") &&
                      file_ok("/usr/share/rime-data/build/luna_pinyin.reverse.bin") &&
                      file_ok("/usr/share/rime-data/build/luna_pinyin.schema.yaml") &&
                      file_ok("/usr/share/rime-data/default.yaml");
    s = (lib && data) ? 1 : 0;
  }
  return s == 1;
}

// rc002 语义保留：引擎可用性（加载成功即缓存；引擎与会话常驻不释放）。
bool try_load()
{
  static int s_state = -1;
  if (s_state < 0) s_state = engine_load() ? 1 : 0;
  return s_state == 1;
}

bool begin()
{
  if (!s_pref_enabled) return false;   // config [ime] enable_zh_cn_input=false：功能禁用
  if (s_cn) return true;
  if (!engine_load()) return false;
  s_cn = true;
  return true;
}

void end()
{
  s_cn = false;
  cancel();
}

bool cn_mode() { return s_cn; }

void set_prefs(bool enabled, Script script, const std::string& custom_symbols)
{
  s_pref_enabled = enabled;
  s_pref_script = script;
  s_pref_custom_symbols = custom_symbols;
  build_syms(custom_symbols);
}

bool composing() { return s_symbol || s_composing; }

void cancel()
{
  s_symbol = false;
  s_preedit.clear();
  s_cands.clear();
  s_composing = false;
  s_page = 0;
  s_sel = 0;
  if (s_sess != 0 && s_api != nullptr && s_api->clear_composition != nullptr)
    s_api->clear_composition(s_sess);
}

bool handle_key(const std::string& name, const kbd::KeyEvent& kev, std::string* commit)
{
  if (commit != nullptr) commit->clear();
  if (!s_cn) return false;

  if (kev.ev == 3) return composing();       // 松开：组词中消费，避免泄漏给程序

  const bool sh = (kev.mods & 1) != 0;
  const bool ct = (kev.mods & 4) != 0;
  const bool al = (kev.mods & 2) != 0;

  if (s_symbol && symbol_key(name, kev, commit)) return true;

  // ── 组词中 ────────────────────────────────────────────────────
  if (s_composing) {
    if (name == "Esc")   { rime_key(0xff1b, 0, commit); return true; }
    if (name == "Bksp")  { rime_key(0xff08, 0, commit); return true; }
    if (name == "Enter") { rime_key(0xff0d, 0, commit); return true; }  // 原样上屏字母
    if (name == " ")     {
      if (s_cands.empty()) {
        // 0 候选：空格不再吞——原样上屏已输入字母并补一个空格（"hello world"）
        if (commit != nullptr) { *commit = s_preedit; *commit += ' '; }
        cancel();
      } else {
        select_at(s_page * kPerPage + s_sel, commit);
      }
      return true;
    }
    // 句号与逗号语义对齐（rime 把 "." 当音节分隔符会原样出 "shijie."）：
    // 先上屏高亮词（0 候选则原样上屏字母），再交 rime 出全角句号。
    if (name == "." && !ct && !al) {
      if (s_cands.empty()) {
        if (commit != nullptr) *commit = s_preedit;
        cancel();
      } else {
        select_at(s_page * kPerPage + s_sel, commit);
      }
      std::string extra;
      rime_key('.', sh ? 1 : 0, &extra);
      if (commit != nullptr) *commit += extra;
      return true;
    }
    if (name.size() == 1 && name[0] >= '1' && name[0] <= '9') {
      select_at(s_page * kPerPage + (name[0] - '1'), commit);
      return true;
    }
    if (name == "Tab" || name == "PgDn" || name == "PgUp") {
      const int delta = (name == "PgUp" || (name == "Tab" && sh)) ? -1 : +1;
      page_turn(delta);
      return true;
    }
    if (name == "Up" || name == "Dn" || name == "Lt" || name == "Rt") {
      move_grid(name, (int)s_cands.size(), s_page, s_sel);
      return true;
    }
    // 字母/标点 → rime（标点由 punctuator 转全角直接上屏）
    if (name.size() == 1 && (unsigned char)name[0] >= 0x20 && !ct && !al) {
      rime_key((unsigned char)name[0], sh ? 1 : 0, commit);
      // 7 汉字上限：本键令分隔符超过 6 个（音节 > 7）→ 退格撤销、丢弃该字母
      if (sep_count(s_preedit) > kMaxSep) {
        std::string drop;
        rime_key(0xff08, 0, &drop);
        if (commit != nullptr && !drop.empty()) *commit += drop;
      }
      return true;
    }
    return false;                            // 控制键等放行（Ctrl+C 逃生口）
  }

  // ── 未组词 ────────────────────────────────────────────────────
  if (name == "i" && !sh && !ct && !al) { symbol_open(); return true; }
  if (name.size() == 1 && (unsigned char)name[0] >= 0x21 && !ct && !al) {
    return rime_key((unsigned char)name[0], sh ? 1 : 0, commit);
    // rime 未消费（如裸数字）→ false → 调用方放行给程序
  }
  return false;
}

void ui_state(UiState* out)
{
  if (out == nullptr) return;
  *out = UiState();
  out->on = s_cn;
  if (!s_cn) return;

  if (s_symbol) {
    const int pages = (sym_count() + kPerPage - 1) / kPerPage;
    out->box_visible = true;
    out->symbol = true;
    out->page = s_sym_page;
    out->pages = pages;
    out->sel = s_sym_sel;
    const int base = s_sym_page * kPerPage;
    for (int i = 0; i < kPerPage && base + i < sym_count(); i++) {
      out->cand[i] = sym_item(base + i);
      out->ncand++;
    }
    return;
  }

  out->text_visible = s_composing;
  out->preedit = s_preedit;
  out->box_visible = s_composing;
  const int n = (int)s_cands.size();
  out->pages = (n + kPerPage - 1) / kPerPage;
  if (out->pages < 1) out->pages = 1;
  out->page = s_page;
  out->sel = s_sel;
  const int base = s_page * kPerPage;
  for (int i = 0; i < kPerPage && base + i < n; i++) {
    out->cand[i] = s_cands[base + i];
    out->ncand++;
  }
}

}  // namespace ime
