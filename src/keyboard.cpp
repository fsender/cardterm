// keyboard.cpp — CardputerZero 板载键盘驱动（TCA8418C → 标准 evdev）
// 键码语义依据 docs/keyboard-driver-analysis.md（DT 三层 keymap + M5 符号层表）。
//
// 架构：**队列解耦**——独立读键线程（生产者）持续消化 evdev 事件并翻译成
// 回显文本入队；UI 循环（消费者）只管出队渲染。绘制慢时不丢键、读键不被
// 显示阻塞。
//
// 修饰键三态语义（官方按键规范；状态机在内核驱动内实现，此处只读）：
//   sym/shift/fn 三键统一行为（Ctrl/Alt 为标准键盘行为）：
//     短按一次 = 指示灯慢闪，一次性生效（对下一字符生效），回显不输出；
//     短按两次 = 指示灯快闪，锁定生效（可连续输入），进入时输出首片段；
//     按住不放 = 指示灯常亮，按住期间生效，按住瞬间输出首片段；
//     退出锁定/松开时输出尾片段。
//   回显片段：Shift="Shi"/"ft"，Fn="F"/"n"，Sym="Sy"/"m"（按住/锁定成对出现）。
//
// One-shot（一次性）消费规则（**事件优先 + 恰好一次**）：
//   驱动在按下修饰键时合成 KEY_LEFTSHIFT/KEY_FN/KEY_SYM 的 down，在 One-shot
//   被下一个键"释放"触发消费/锁定退出/按住松开时才合成 up（中间没有物理松开
//   事件）。因此"当前字符是否上档"以 evdev 事件为准，轮询到的 modstate 只用于
//   识别 LOCKED 与 One-shot 两种固件模式；One-shot 用代次令牌保证只消费一次，
//   并识别"固件在字符之后才补报 One-shot"的迟到上膛（丢弃令牌 + 作废本代逻辑
//   按住），避免把上档带到第二个字符。详见 poll_mods()/shift_effective()。
//   Fn/Sym 的换层在内核驱动内完成（keymap1/keymap2），同样的迟到上膛会让第二个
//   键以已重映射的键码到达；这时用 MSC_SCAN 反查基础层键码还原（见 kBaseKeymap）。
//
// 其它行为：
//   - 长按自动重发：按住 500ms 后每 50ms 重发一次（松开结束）
//   - Ctrl/Alt：按下回显 "Ct"/"Al"，松开回显 "rl"/"t"
//   - Caps：直接跳过，不实现
//   - 长按 Esc 两段式（kEscHintMs / kEscHoldMs）：按住 0.5s → 提示事件（EscHint，
//     调用方弹 toast），按住 2s → Quit；中途松开不产生任何事件（提示由调用方自然
//     消失）；短按（<0.5s）按/松成对上报，回显 "Esc"
#include "keyboard.h"
#include "applaunch_env.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <deque>
#include <dirent.h>
#include <fcntl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <linux/input.h>
#include <mutex>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace kbd {
namespace {

constexpr int      kEscHintMs        = 500;    // 长按 Esc 提示阈值：按住 0.5s 报提示事件
constexpr int      kEscHoldMs        = 2000;   // 长按 Esc 退出阈值：按住 2s 报 Quit 退出
constexpr int      kRepeatDelayMs    = 500;    // 长按自动重发：按住 500ms 后……
constexpr int      kRepeatIntervalMs = 50;     // ……每 50ms 重发一次
constexpr int      kModPollMs        = 25;     // 修饰状态轮询周期
constexpr uint16_t kKeySym           = 0x17f;  // Sym 层切换键（DT keymap: KEY_CD）
constexpr uint16_t kKeyFn            = 0x185;  // Fn 层切换键（DT keymap: KEY_DVD）
constexpr uint16_t kKeyShift         = 42;     // KEY_LEFTSHIFT
// 新固件可能把 Fn/Sym 改用标准键码上报（Launcher/APPLaunch 同时兼容 KEY_FN、
// KEY_COMPOSE；见 global_hint_policy.cpp），两种驱动代都识别。
constexpr uint16_t kKeySymStd        = 127;    // KEY_COMPOSE
constexpr uint16_t kKeyFnStd         = 0x1d0;  // KEY_FN

// ── 回显队列（生产者=读键线程，消费者=UI 循环）─────────────────────
struct Item { enum Kind { Key, Quit, Mod, Aux, Cmd, EscHint } kind; std::string text; KeyEvent kev; };
std::deque<Item>        g_q;
std::mutex              g_mtx;
std::condition_variable g_cv;

std::atomic<bool> g_running{false};
std::atomic<bool> g_lost{false};      // 所有键盘设备丢失（驱动 rebind / 测试 uinput 被移除）
std::thread       g_thread;

uint64_t now_ms()
{
  return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct Map  { uint16_t code; const char* text; };
struct DMap { uint16_t code; char lo, up; };

// ── 基础层（DT linux,keymap）字母行 ────────────────────────────────
const DMap kLetters[] = {
  {16,'q','Q'},{17,'w','W'},{18,'e','E'},{19,'r','R'},{20,'t','T'},
  {21,'y','Y'},{22,'u','U'},{23,'i','I'},{24,'o','O'},{25,'p','P'},
  {30,'a','A'},{31,'s','S'},{32,'d','D'},{33,'f','F'},{34,'g','G'},
  {35,'h','H'},{36,'j','J'},{37,'k','K'},{38,'l','L'},
  {44,'z','Z'},{45,'x','X'},{46,'c','C'},{47,'v','V'},{48,'b','B'},
  {49,'n','N'},{50,'m','M'},
};

// ── 基础层数字行（Shift = US 上档符号）────────────────────────────
const DMap kDigits[] = {
  { 2,'1','!'},{ 3,'2','@'},{ 4,'3','#'},{ 5,'4','$'},{ 6,'5','%'},
  { 7,'6','^'},{ 8,'7','&'},{ 9,'8','*'},{10,'9','('},{11,'0',')'},
};

// ── 符号层（DT linux,keymap1）重定义键码 ───────────────────────────
// 语义来源：/usr/share/keymaps/tca8418_keypad_m5stack_keymap.map
// （APPLaunch 经 APPLAUNCH_LINUX_KEYBOARD_MAP 注入同一文件；运行期优先用
//   注入的 map，缺失/解析失败时静默回退本内置表，见 load_keymap()/sym_char()）
const Map kSymbols[] = {
  {26,"!"},{27,"@"},{39,"#"},{40,"$"},{41,"%"},{43,"^"},{51,"&"},{52,"*"},
  {53,"("},{94,")"},{55,"~"},{69,"`"},{70,"_"},{71,"-"},{72,"+"},{73,"="},
  {74,"["},{75,"]"},{76,"{"},{77,"}"},{79,";"},{80,":"},{81,"'"},{82,"\""},
  {83,"<"},{85,">"},{86,"\\"},{89,"|"},{90,","},{91,"."},{92,"/"},{93,"?"},
};

// ── APPLaUNCH_LINUX_KEYBOARD_MAP 运行期覆盖表 ─────────────────────
// loadkeys 格式（"keycode 26 = exclam"）解析为 键码→字符；查表优先于
// kSymbols。仅覆盖符号层（字符均为单字符），不触碰字母/数字/控制键。
struct SymEnt { uint16_t code; char ch; };
SymEnt g_sym_map[96];
int    g_sym_n = 0;

char keysym_to_char(const char* name)
{
  if (name[0] != '\0' && name[1] == '\0') return name[0];  // 单字符 keysym
  struct N { const char* name; char ch; };
  static const N tbl[] = {
    {"exclam",'!'},{"at",'@'},{"numbersign",'#'},{"dollar",'$'},{"percent",'%'},
    {"asciicircum",'^'},{"ampersand",'&'},{"asterisk",'*'},{"parenleft",'('},
    {"parenright",')'},{"asciitilde",'~'},{"grave",'`'},{"underscore",'_'},
    {"minus",'-'},{"plus",'+'},{"equal",'='},{"bracketleft",'['},{"bracketright",']'},
    {"braceleft",'{'},{"braceright",'}'},{"semicolon",';'},{"colon",':'},
    {"apostrophe",'\''},{"quotedbl",'"'},{"less",'<'},{"greater",'>'},
    {"backslash",'\\'},{"bar",'|'},{"comma",','},{"period",'.'},{"slash",'/'},
    {"question",'?'},{"space",' '},
    {"zero",'0'},{"one",'1'},{"two",'2'},{"three",'3'},{"four",'4'},
    {"five",'5'},{"six",'6'},{"seven",'7'},{"eight",'8'},{"nine",'9'},
  };
  for (const auto& t : tbl) if (strcmp(t.name, name) == 0) return t.ch;
  return 0;
}

void load_keymap(const char* path)
{
  FILE* f = fopen(path, "r");
  if (!f) {
    fprintf(stderr, "[kbd] keymap %s -> %s (builtin fallback)\n", path, strerror(errno));
    return;
  }
  char line[256];
  while (fgets(line, sizeof line, f))
  {
    char* c = strchr(line, '#'); if (c) *c = 0;           // 行注释
    unsigned code; char name[64];
    if (sscanf(line, " keycode %u = %63s", &code, name) != 2) continue;
    char ch = keysym_to_char(name);
    if (ch == 0 || g_sym_n >= (int)(sizeof g_sym_map / sizeof g_sym_map[0])) continue;
    g_sym_map[g_sym_n++] = { (uint16_t)code, ch };
  }
  fclose(f);
  fprintf(stderr, "[kbd] keymap %s: %d entries loaded\n", path, g_sym_n);
}

// 符号层键码 → 字符；0 = 非符号层键。注入 map 优先，内置表兜底。
char sym_char(uint16_t code)
{
  for (int i = 0; i < g_sym_n; i++) if (g_sym_map[i].code == code) return g_sym_map[i].ch;
  for (const auto& m : kSymbols) if (m.code == code) return m.text[0];
  return 0;
}

// ── 控制键回显名（Fn 层 keymap2 + 基础层特殊键）───────────────────
const Map kControls[] = {
  {15,"Tab"},{28,"Enter"},{96,"Enter"},{14,"Bksp"},
  {59,"F1"},{60,"F2"},{61,"F3"},{62,"F4"},{63,"F5"},{64,"F6"},
  {65,"F7"},{66,"F8"},{67,"F9"},{68,"F10"},{87,"F11"},{88,"F12"},
  {110,"Ins"},{111,"Del"},{102,"Home"},{107,"End"},
  {104,"PgUp"},{109,"PgDn"},{99,"PrSc"},{119,"Paus"},{138,"Help"},{139,"Menu"},
  {103,"Up"},{108,"Dn"},{105,"Lt"},{106,"Rt"},
  {113,"Mute"},{114,"Vol-"},{115,"Vol+"},{164,"Play"},{165,"Prev"},{163,"Next"},
  {224,"Bri-"},{225,"Bri+"},
};

// ── 外接键盘（USB/蓝牙）：标准 US/PC 布局表 ──────────────────────
// 板载键盘的 DT keymap 把 32 个符号键码重定义成了另一套字符（见 kSymbols），
// 标准 PC 键盘绝不能复用板载表（否则按 `[` 出 `!`）。这里是一套独立的
// 「标准 evdev 键码 → 字符」映射；字母行/数字行（kLetters/kDigits）与标准布局
// 一致，直接复用。只做 US 布局（不支持布局切换，见 README 已知限制）。
struct StdChar { uint16_t code; char lo, up; };
const StdChar kStdPunct[] = {
  {12,'-','_'}, {13,'=','+'}, {26,'[','{'}, {27,']','}'}, {39,';',':'},
  {40,'\'','"'}, {41,'`','~'}, {43,'\\','|'}, {51,',','<'}, {52,'.','>'},
  {53,'/','?'},
  // 数字小键盘（NumLock 开时内核对以上报 KP 键码；其中 74–77 等与板载符号层
  // 重定义的键码冲突，所以必须独立成表）
  {79,'1','1'}, {80,'2','2'}, {81,'3','3'}, {75,'4','4'}, {76,'5','5'},
  {77,'6','6'}, {71,'7','7'}, {72,'8','8'}, {73,'9','9'}, {82,'0','0'},
  {83,'.','.'}, {74,'-','-'}, {78,'+','+'}, {55,'*','*'}, {98,'/','/'},
};

// 标准 PC 键盘的控制键回显名。名字与 kControls 完全一致（Up/Dn/Lt/Rt、Bksp、
// PgUp…），session.cpp 的翻页/窗口快捷键/KKP 编码不用改即可复用。
const Map kStdControls[] = {
  {15,"Tab"},{28,"Enter"},{96,"Enter"},{14,"Bksp"},
  {59,"F1"},{60,"F2"},{61,"F3"},{62,"F4"},{63,"F5"},{64,"F6"},
  {65,"F7"},{66,"F8"},{67,"F9"},{68,"F10"},{87,"F11"},{88,"F12"},
  {110,"Ins"},{111,"Del"},{102,"Home"},{107,"End"},
  {104,"PgUp"},{109,"PgDn"},
  {103,"Up"},{108,"Dn"},{105,"Lt"},{106,"Rt"},
};

// 标准布局可打印键的未上档/上档字符（KKP 编码用；false=非打印键）
bool std_pair(uint16_t code, char* lo, char* up)
{
  for (const auto& m : kStdPunct) if (m.code == code) { *lo = m.lo; *up = m.up; return true; }
  for (const auto& m : kLetters)  if (m.code == code) { *lo = m.lo; *up = m.up; return true; }
  for (const auto& m : kDigits)   if (m.code == code) { *lo = m.lo; *up = m.up; return true; }
  if (code == KEY_SPACE) { *lo = ' '; *up = ' '; return true; }
  return false;
}

// 标准布局：键码 → 回显文本（空 = 不输出；多媒体键等不上屏）
std::string std_translate(uint16_t code, bool shift)
{
  char lo = 0, up = 0;
  if (std_pair(code, &lo, &up)) return std::string(1, shift ? up : lo);
  for (const auto& m : kStdControls) if (m.code == code) return m.text;
  return std::string();
}

// 键码 → 回显文本。顺序：符号层表优先（注入 map → 内置）→ 字母 → 数字行 → 控制键名 → 诊断。
std::string translate(uint16_t code, bool shift)
{
  if (char c = sym_char(code)) return std::string(1, c);

  for (const auto& m : kLetters) {
    if (m.code != code) continue;
    return std::string(1, shift ? m.up : m.lo);
  }

  for (const auto& m : kDigits)
    if (m.code == code) return std::string(1, shift ? m.up : m.lo);

  if (code == KEY_SPACE) return " ";

  for (const auto& m : kControls) if (m.code == code) return m.text;

  return std::string();                      // 未知键：不输出任何字符
}

// ── 修饰键状态（内核驱动维护，用户态只读）─────────────────────────
// 来源优先级：CARDTERM_MODSTATE 文件（测试接缝，legacy 语义 0-3）
//   → sysfs /sys/bus/i2c/devices/1-0034/modifier_state（raw: 0=Off 1=PRESSED
//     2=OneShot 3=Locked 4=HELD）
//   → /dev/i2c-1 上 m5ioe1(0x4F) 寄存器 0xBD/0xBF/0xBE（legacy: 0=Off
//     1=OneShot 2=Locked 3=Held，同 Keyboard-Guide 官方读法）
enum ModMode : uint8_t { MOD_OFF = 0, MOD_ONESHOT, MOD_LOCKED, MOD_HELD };

enum class ModSrc { None, Seam, Sysfs, I2c };
ModSrc  g_mod_src  = ModSrc::None;
int     g_mod_fd   = -1;          // sysfs/i2c fd
std::string g_mod_seam;           // 测试接缝文件路径

// Ctrl/Alt 按住态（标准修饰键；Shift/Fn/Sym 的按住态在各 ModEcho.down 里）
bool g_ctrl = false;
bool g_alt  = false;

// 修饰键索引（与 g_mods[] 顺序一致）
constexpr int kModShift = 0;
constexpr int kModSym   = 1;
constexpr int kModFn    = 2;

struct ModEcho {
  bool        announced = false;  // 首片段已输出（等待尾片段）
  ModMode     mode      = MOD_OFF;
  const char* first;              // 进入锁定/按住：首片段
  const char* second;             // 退出/松开：尾片段
  // ── One-shot 代次记账（事件优先，恰好一次）──────────────────────
  bool        down      = false;  // 逻辑按住（evdev）；驱动的"合成"事件也计入
  uint32_t    gen       = 0;      // 每次 down 事件 +1，代表一代逻辑按住
  bool        used      = false;  // 本代是否已被某个字符用掉（上档已实际生效过）
  bool        one_shot  = false;  // 轮询观测到 One-shot 后上膛的一次性令牌
  bool        stale     = false;  // 迟到上膛：本代已用掉后固件才报 One-shot
};
const char* const kModNames[3] = {"shift", "sym", "fn"};
// 顺序：shift, sym, fn
ModEcho g_mods[3] = {
  {false, MOD_OFF, "Shi", "ft"},
  {false, MOD_OFF, "Sy",  "m"},
  {false, MOD_OFF, "F",   "n"},
};

bool read_mod_raw(uint8_t raw[3])
{
  switch (g_mod_src) {
    case ModSrc::Seam: {                                  // 测试接缝：文件 "s y f"
      FILE* f = fopen(g_mod_seam.c_str(), "r");
      if (!f) return false;
      unsigned s = 0, y = 0, n = 0;
      int ok = fscanf(f, "%u %u %u", &s, &y, &n);
      fclose(f);
      if (ok != 3) return false;
      raw[0] = (uint8_t)s; raw[1] = (uint8_t)y; raw[2] = (uint8_t)n;
      return true;
    }
    case ModSrc::Sysfs: {                                 // sysfs 快照 "… shift=N sym=N fn=N …"
      char buf[256] = {0};
      ssize_t n = pread(g_mod_fd, buf, sizeof(buf) - 1, 0);
      if (n <= 0) return false;
      long s = -1, y = -1, fn = -1;
      for (char* tok = strtok(buf, " \t\n"); tok; tok = strtok(nullptr, " \t\n")) {
        long v = 0;
        if (sscanf(tok, "shift=%ld", &v) == 1) s = v;
        else if (sscanf(tok, "sym=%ld", &v) == 1) y = v;
        else if (sscanf(tok, "fn=%ld", &v) == 1) fn = v;
      }
      if (s < 0 || y < 0 || fn < 0) return false;
      // sysfs raw：1=PRESSED（按下未定型，不回显）按 OFF 处理；4=HELD
      auto map = [](long v) -> uint8_t {
        return v == 2 ? MOD_ONESHOT : v == 3 ? MOD_LOCKED : v == 4 ? MOD_HELD : MOD_OFF;
      };
      raw[0] = map(s); raw[1] = map(y); raw[2] = map(fn);
      return true;
    }
    case ModSrc::I2c: {                                   // legacy 寄存器 0xBD/0xBF/0xBE
      static const uint8_t regs[3] = {0xBD, 0xBF, 0xBE};
      for (int i = 0; i < 3; i++) {
        // I2C SMBus 字节读（同 Keyboard-Guide readModifierRegister）
        i2c_smbus_data data {};
        i2c_smbus_ioctl_data req {};
        req.read_write = I2C_SMBUS_READ;
        req.command    = regs[i];
        req.size       = I2C_SMBUS_BYTE_DATA;
        req.data       = &data;
        if (ioctl(g_mod_fd, I2C_SMBUS, &req) < 0) return false;
        raw[i] = data.byte > 3 ? 0 : data.byte;            // 0=Off 1=OneShot 2=Locked 3=Held
      }
      return true;
    }
    default:
      return false;
  }
}

void push(Item::Kind kind, std::string text, const KeyEvent& kev = KeyEvent())
{
  {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_q.push_back(Item{kind, std::move(text), kev});
  }
  g_cv.notify_one();
}

// 修饰键键码 → g_mods[] 下标（Shift 左右与两种驱动代的标准码都算；-1=非修饰键）
int mod_index_of(uint16_t code)
{
  if (code == kKeyShift || code == 54 /*KEY_RIGHTSHIFT*/) return kModShift;
  if (code == kKeySym || code == kKeySymStd) return kModSym;
  if (code == kKeyFn  || code == kKeyFnStd)  return kModFn;
  return -1;
}

// 修饰键 down：开启新一代"逻辑按住"。固件在物理按下时会合成该事件；
// 新一代清掉上一代遗留的令牌与作废标记（新一轮上档从零开始）。
void note_mod_down(int i)
{
  ModEcho& m = g_mods[i];
  m.gen++;
  m.down     = true;
  m.used     = false;
  m.one_shot = false;
  m.stale    = false;
}

// 修饰键 up：固件在 One-shot 被消费 / 锁定退出 / 按住松开时合成。
// 本次按下周期到此结束：令牌/作废标记不再跨周期保留。
void note_mod_up(int i)
{
  ModEcho& m = g_mods[i];
  m.down     = false;
  m.used     = false;
  m.one_shot = false;
  m.stale    = false;
}

// 修饰状态变化 → 回显片段（官方三态语义）+ One-shot 令牌（事件优先，恰好一次）
void poll_mods()
{
  uint8_t raw[3];
  if (!read_mod_raw(raw)) return;

  for (int i = 0; i < 3; i++) {
    ModEcho& m = g_mods[i];
    ModMode  nw = (ModMode)raw[i];
    if (nw == m.mode) continue;
    const ModMode old = m.mode;

    // 进入 One-shot：上膛一次性令牌（作用于下一个字符，消费恰好一次）。
    // 若本代 Shift 已经被某个字符用掉（used），说明固件是在字符之后才补报
    // One-shot（迟到的上膛）——丢弃令牌并作废本代逻辑按住，否则这一次性上档
    // 会落到第二个字符上（用户实测的"第二个字母仍大写"根因）。
    if (nw == MOD_ONESHOT && old != MOD_ONESHOT) {
      if (m.used) {
        m.one_shot = false;
        m.stale    = true;
        fprintf(stderr, "[kbd] late one-shot (%s): already used by a key, discarded\n",
                kModNames[i]);
      } else {
        m.one_shot = true;
        m.stale    = false;
      }
    }
    // 离开 One-shot（被消费 / 点按取消 / 转入锁定或按住）：令牌不跨字符保留
    if (old == MOD_ONESHOT && nw != MOD_ONESHOT) m.one_shot = false;
    // 回到 Off：本代结束（无 evdev 事件来源时也能重新上膛，如测试接缝；
    // 同时兜底清理迟到上膛的作废标记，防驱动未合成 up 时永久抑制）
    if (nw == MOD_OFF) { m.used = false; m.stale = false; m.one_shot = false; }

    // 首片段：进入【锁定/按住】（短按一次 = OneShot 不输出）
    if ((nw == MOD_LOCKED || nw == MOD_HELD) && !m.announced) {
      push(Item::Aux, m.first);
      m.announced = true;
    }
    // 尾片段：从【锁定/按住】退出（OneShot 的消耗/取消不输出）
    if (nw == MOD_OFF && m.announced) {
      push(Item::Aux, m.second);
      m.announced = false;
    }

    m.mode = nw;
  }
}

// 该修饰键对"当前这一次按键"是否生效：**事件优先** + 已上膛的 One-shot + 锁定。
//   - LOCKED：固件模式，连续生效（轮询识别，见硬件按键规范）；
//   - one_shot：轮询观测到 One-shot 后上膛的令牌（恰好消费一次）；
//   - stale：本代已被字符用掉、固件随后才补报 One-shot → 一律不生效；
//   - down：evdev 事件判定的逻辑按住（含驱动合成事件）；
//   - MOD_HELD 仅作无 evdev 事件来源（测试接缝 / 旧驱动）时的兜底。
bool mod_effective(int i)
{
  const ModEcho& m = g_mods[i];
  if (m.mode == MOD_LOCKED) return true;
  if (m.one_shot)           return true;
  if (m.stale)              return false;
  if (m.down)               return true;
  return m.mode == MOD_HELD;
}

// Shift 层由用户态应用到字符，所以这是唯一"决定字符大小写"的修饰键；
// Fn/Sym 的换层在 evdev 之前已由内核驱动完成（keymap1/keymap2），见下。
bool shift_effective() { return mod_effective(kModShift); }

// 普通键按下 = 固件眼里的"下一个字符"：三个修饰键的 One-shot 令牌**各恰好消费一次**，
// 并记下"本代逻辑按住已被字符用掉"（供 poll_mods 识别迟到的 One-shot 上膛）。
// 注意：Fn/Sym 的层选择发生在内核驱动内（evdev 之前），本地令牌不改变键码；
// 迟到上膛导致第二键被多换一层时，由 process() 里的 MSC_SCAN 反查补偿。
void consume_one_shot()
{
  for (int i = 0; i < 3; i++) {
    ModEcho& m = g_mods[i];
    const bool eff = mod_effective(i);
    if (m.one_shot) m.one_shot = false;   // 令牌只作用于这一个字符
    if (eff) m.used = true;               // 本代已被字符用掉
  }
}

// ── 读键线程状态 ──────────────────────────────────────────────────
int         g_rep_code = -1;     // 当前处于自动重发的键（-1=无）
std::string g_rep_text;
KeyEvent    g_rep_kev;           // 重发时携带的结构化事件
uint64_t    g_rep_next = 0;      // 下次重发时刻
bool        g_esc_down  = false;
bool        g_esc_fired = false;
bool        g_esc_hinted = false;   // 提示事件已报（防重复；松开时复位）
uint64_t    g_esc_since = 0;

// ── 多设备支持：板载 + 外接（USB/蓝牙）键盘并行输入 ────────────────
// 板载键盘沿用上面的板载布局与内核 FSM 修饰逻辑（全局状态，逻辑不变）；
// 外接键盘走标准 US 布局 + **每设备独立**的修饰键布尔状态，互不污染。
constexpr int      kMaxExtDevs   = 4;      // 外接键盘上限（板载/主设备另有 1 个）
constexpr int      kRescanMs     = 1800;   // /dev/input 热插拔重扫周期
constexpr uint16_t kKeyRightShift = 54;    // KEY_RIGHTSHIFT

struct Dev {
  int         fd       = -1;
  std::string path;                 // 实际打开的 devnode（日志/去重）
  std::string name;                 // EVIOCGNAME
  uint32_t    bustype  = 0;         // EVIOCGID.bustype
  bool        standard = false;     // true = 标准 US 布局（外接键盘）
  dev_t       rdev     = 0;         // st_rdev：按物理设备去重，避免重复打开
  // 标准布局设备的独立修饰状态（板载走 g_mods/g_ctrl/g_alt，不读这里）
  bool        shift = false, ctrl = false, alt = false, meta = false;
  // 长按 Esc 两段式（与板载同阈值）：0.5s 提示（esc_hinted 防重复）→ 2s Quit
  bool        esc_down = false, esc_fired = false, esc_hinted = false;
  uint64_t    esc_since = 0;
  // 自动重发（每设备一份，互不打断；板载仍用上面的 g_rep_* 全局状态）
  uint16_t    rep_code = 0xffff;
  std::string rep_text;
  KeyEvent    rep_kev;
  uint64_t    rep_next = 0;
};

Dev      g_primary;                    // 板载（或 CARDTERM_KBD 显式指定的）主设备
Dev      g_ext[kMaxExtDevs];           // 外接键盘（USB/蓝牙）
int      g_ext_n     = 0;
uint64_t g_next_scan = 0;              // 下次 /dev/input 重扫时刻
// 容量已满时被丢弃的设备（st_rdev）：只记一次日志，避免 1.8s 重扫刷屏
dev_t    g_ignored[kMaxExtDevs];
int      g_ignored_n = 0;

void close_dev(Dev& d)
{
  if (d.fd >= 0) {
    ioctl(d.fd, EVIOCGRAB, 0);
    ::close(d.fd);
  }
  d = Dev();
}

bool bit_set(const unsigned long* bits, int bit)
{
  constexpr int kBits = (int)(sizeof(unsigned long) * 8);
  return ((bits[bit / kBits] >> (bit % kBits)) & 1UL) != 0;
}

// 像键盘吗：有 EV_KEY 且同时具备字母键 A/Z（判据同 exclusive_display::grab_keyboards）
bool has_letter_keys(int fd)
{
  unsigned long evbits[(EV_MAX / (int)(sizeof(unsigned long) * 8)) + 1] = {0};
  unsigned long keybits[(KEY_MAX / (int)(sizeof(unsigned long) * 8)) + 1] = {0};
  if (ioctl(fd, EVIOCGBIT(0, sizeof evbits), evbits) < 0) return false;
  if (!bit_set(evbits, EV_KEY)) return false;
  if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keybits), keybits) < 0) return false;
  return bit_set(keybits, KEY_A) && bit_set(keybits, KEY_Z);
}

// EVIOCGID/EVIOCGNAME（失败不致命：未知设备按"非外接"处理，走板载旧路径）
bool dev_identity(int fd, uint32_t* bustype, std::string* name)
{
  struct input_id id {};
  if (ioctl(fd, EVIOCGID, &id) < 0) return false;
  *bustype = id.bustype;
  char nm[256] = {0};
  if (ioctl(fd, EVIOCGNAME(sizeof nm - 1), nm) >= 0) *name = nm;
  else name->clear();
  return true;
}

bool name_matches_onboard(const std::string& name)
{
  std::string lower;
  lower.reserve(name.size());
  for (char c : name) lower += (char)std::tolower((unsigned char)c);
  return lower.find("cardputer") != std::string::npos ||
         lower.find("tca8418")   != std::string::npos;
}

bool is_external_bus(uint32_t bustype)
{
  return bustype == BUS_USB || bustype == BUS_BLUETOOTH;
}

// 主设备（CARDTERM_KBD / 默认路径）的布局判定：
//   I2C 总线或板载驱动名 → 板载布局（旧行为，键码语义不变）
//   USB/蓝牙               → 标准 US 布局（外接键盘经 CARDTERM_KBD 指定时使用）
// 例外：既有 e2e 测试脚本用 uinput 注入 "cardterm-test-kbd"（BUS_USB）冒充板载
// 键盘并通过 CARDTERM_KBD 显式指定；此时保持旧板载语义，避免回归测试大面积误判。
// 自动发现的外接设备不受此例外影响（名字含 test-kbd 也按标准布局接管）。
bool primary_is_standard(uint32_t bustype, const std::string& name, bool explicit_env)
{
  if (bustype == BUS_I2C || name_matches_onboard(name)) return false;
  if (explicit_env && name.find("test-kbd") != std::string::npos) return false;
  if (is_external_bus(bustype)) return true;
  return false;   // 未知/虚拟总线：显式指定按旧语义（板载），自动路径必然是板载
}

uint8_t dev_mods_mask(const Dev& d)
{
  return (uint8_t)((d.shift ? 1 : 0) | (d.alt ? 2 : 0) | (d.ctrl ? 4 : 0));
}

// 设备已在打开列表中？（按 st_rdev 去重，path/symlink 不同也算同一物理设备）
bool devnode_already_open(dev_t rdev)
{
  if (g_primary.fd >= 0 && g_primary.rdev == rdev) return true;
  for (int i = 0; i < g_ext_n; i++)
    if (g_ext[i].rdev == rdev) return true;
  return false;
}

// 新的键盘按下：取消**其它**设备的自动重发（"最后按下的键"语义，避免两台设备
// 同时重发互相叠加；板载的旧行为是任何新键即接管重发，这里保持一致）。
void cancel_repeats_except(int slot)   // slot：-1=板载主设备，-2=标准布局主设备，>=0 外接下标
{
  if (slot != -1) g_rep_code = -1;
  if (slot != -2 && g_primary.standard) g_primary.rep_code = 0xffff;
  for (int i = 0; i < g_ext_n; i++)
    if (slot != i) g_ext[i].rep_code = 0xffff;
}

// 该设备最近一个定时死线的剩余毫秒（无死线时给 1000）
int dev_deadline_rem(const Dev& d, uint64_t now)
{
  int rem = 1000;
  if (d.esc_down && !d.esc_fired) {
    // 下一个死线：未提示时是提示阈值，已提示是退出阈值
    const int target = d.esc_hinted ? kEscHoldMs : kEscHintMs;
    const int r = target - (int)(now - d.esc_since);
    if (r < rem) rem = r;
  }
  if (d.rep_code != 0xffff) {
    const int r = (int)(d.rep_next - now);
    if (r < rem) rem = r;
  }
  return rem;
}

// ── 外接键盘事件处理（标准 US 布局 + 每设备修饰键）───────────────
void process_external(Dev& d, int slot, const struct input_event& ev)
{
  if (ev.type != EV_KEY) return;              // 标准设备无换层需求，忽略 MSC/SYN
  const uint16_t code = ev.code;
  const int      val  = ev.value;
  if (val == 2) return;                       // 内核重复忽略，本驱动自管重发

  // 系统电源键过滤（同板载：外接键盘的电源/睡眠键不能直达电源动作）
  if (code == 116 /*KEY_POWER*/ || code == 142 /*KEY_SLEEP*/ ||
      code == 205 /*KEY_SUSPEND*/ || code == 143 /*KEY_WAKEUP*/)
    return;

  const bool down = (val != 0);

  // 修饰键：每设备独立的简单 press/release 布尔（板载的 One-shot FSM 不参与）
  if (code == KEY_LEFTSHIFT || code == kKeyRightShift) { d.shift = down; return; }
  if (code == KEY_LEFTCTRL  || code == KEY_RIGHTCTRL) {
    d.ctrl = down;
    push(Item::Mod, down ? "ctrl_on" : "ctrl_off");
    return;
  }
  if (code == KEY_LEFTALT   || code == KEY_RIGHTALT) {
    d.alt = down;
    push(Item::Mod, down ? "alt_on" : "alt_off");
    return;
  }
  if (code == KEY_LEFTMETA || code == KEY_RIGHTMETA) { d.meta = down; return; }
  if (code == KEY_CAPSLOCK) return;

  // Super(Windows/Meta) 组合键：窗口切换 / 字号 / 关闭（与板载 Fn 快捷键等价）
  if (d.meta) {
    if (down) {
      Cmd c = Cmd::None;
      switch (code) {
        case KEY_LEFT:  case KEY_W: c = Cmd::PrevWin;  break;
        case KEY_RIGHT: case KEY_E: c = Cmd::NextWin;  break;
        case KEY_UP:    case KEY_I: c = Cmd::FontUp;   break;
        case KEY_DOWN:  case KEY_U: c = Cmd::FontDown; break;
        case KEY_Q:                 c = Cmd::Close;    break;
        case KEY_A:                 c = Cmd::Detach;   break;
        case KEY_H:                 c = Cmd::ShowHelp; break;
        default:
          if (code >= KEY_F1 && code <= KEY_F10)
            c = (Cmd)((int)Cmd::Win1 + (code - KEY_F1));
          else if (code == KEY_F11) c = Cmd::Win11;
          else if (code == KEY_F12) c = Cmd::Win12;
          break;
      }
      if (c != Cmd::None) { push(Item::Cmd, std::to_string((int)c)); return; }
    } else {
      // 松开也不透传给 PTY（避免组合键在应用里产生多余字符/转义）
      switch (code) {
        case KEY_LEFT: case KEY_RIGHT: case KEY_UP: case KEY_DOWN:
        case KEY_W: case KEY_E: case KEY_U: case KEY_I: case KEY_Q:
        case KEY_A: case KEY_H:
        case KEY_F1: case KEY_F2: case KEY_F3: case KEY_F4: case KEY_F5: case KEY_F6:
        case KEY_F7: case KEY_F8: case KEY_F9: case KEY_F10: case KEY_F11: case KEY_F12:
          return;
        default: break;
      }
    }
  }

  // 长按 Esc 两段式（0.5s 提示 / 2s Quit）；短按（<0.5s）按/松成对上报（同板载语义）。
  // 提示后中途松开：不产生任何事件（提示 toast 由调用方自然消失）。
  if (code == KEY_ESC) {
    if (down) {
      d.esc_down = true; d.esc_fired = false; d.esc_hinted = false;
      d.esc_since = now_ms();
      d.rep_code = 0xffff;
    } else {
      if (d.esc_down && !d.esc_hinted && !d.esc_fired) {
        KeyEvent k; k.code = code; k.mods = dev_mods_mask(d);
        k.ev = 1; push(Item::Key, "Esc", k);
        k.ev = 3; push(Item::Key, "Esc", k);
      }
      d.esc_down = false; d.esc_hinted = false;   // 松开：复位提示状态位
    }
    return;
  }

  KeyEvent kev;
  kev.code = code;
  kev.mods = dev_mods_mask(d);
  std_pair(code, &kev.lo, &kev.up);

  if (!down) {                                // 松开：结束自动重发 + 上报松开事件
    if (d.rep_code == code) d.rep_code = 0xffff;
    kev.ev = 3;
    push(Item::Key, std_translate(code, d.shift), kev);
    return;
  }

  cancel_repeats_except(slot);                // "最后按下的键"接管自动重发
  std::string text = std_translate(code, d.shift);
  if (text.empty()) return;                   // 未知键/多媒体键：不产生输出
  kev.ev = 1;
  push(Item::Key, text, kev);
  d.rep_code = code;
  d.rep_text = text;
  d.rep_kev  = kev;
  d.rep_next = now_ms() + kRepeatDelayMs;
}

// 标准布局设备的长按 Esc（两段式）/ 自动重发定时器（每轮 reader_loop 调用一次）
void tick_standard_dev(Dev& d, uint64_t now)
{
  if (d.fd < 0) return;
  if (d.esc_down && !d.esc_fired && !d.esc_hinted &&
      now - d.esc_since >= (uint64_t)kEscHintMs) {
    d.esc_hinted = true;
    push(Item::EscHint, "");                  // 按住 0.5s：提示"继续按住 2s 退出"（只报一次）
  }
  if (d.esc_down && !d.esc_fired && now - d.esc_since >= (uint64_t)kEscHoldMs) {
    d.esc_fired = true;
    d.esc_down  = false;
    d.rep_code  = 0xffff;
    push(Item::Quit, "");                     // 按住 2s = 退出（外接键盘唯一退出手段）
  }
  if (d.rep_code != 0xffff && now >= d.rep_next) {
    KeyEvent k = d.rep_kev;
    k.ev   = 2;                               // kitty 事件类型：重复
    k.mods = dev_mods_mask(d);
    push(Item::Key, d.rep_text, k);
    d.rep_next = now + kRepeatIntervalMs;
  }
}

void tick_standard_devs()
{
  const uint64_t now = now_ms();
  if (g_primary.fd >= 0 && g_primary.standard) tick_standard_dev(g_primary, now);
  for (int i = 0; i < g_ext_n; i++) tick_standard_dev(g_ext[i], now);
}

// 扫描 /dev/input：发现新的 USB/蓝牙键盘就打开并 grab（热插拔/启动时各一次）。
// 同一物理设备按 st_rdev 去重；非键盘（无字母键）/非外接总线一律不接管。
void scan_external()
{
  DIR* dir = opendir("/dev/input");
  if (dir == nullptr) return;
  while (struct dirent* e = readdir(dir)) {
    if (strncmp(e->d_name, "event", 5) != 0) continue;
    char path[280];                                  // "/dev/input/" + NAME_MAX(255)
    snprintf(path, sizeof path, "/dev/input/%s", e->d_name);
    struct stat st {};
    if (stat(path, &st) != 0 || !S_ISCHR(st.st_mode)) continue;
    if (devnode_already_open(st.st_rdev)) continue;       // 已打开（含主设备）

    int fd = ::open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) continue;
    if (!has_letter_keys(fd)) { ::close(fd); continue; }

    uint32_t bustype = 0;
    std::string name;
    if (!dev_identity(fd, &bustype, &name) || !is_external_bus(bustype)) {
      ::close(fd);                                // I2C/板载/虚拟总线：不在此接管
      continue;
    }
    if (g_ext_n >= kMaxExtDevs) {
      bool seen = false;
      for (int i = 0; i < g_ignored_n; i++)
        if (g_ignored[i] == st.st_rdev) { seen = true; break; }
      if (!seen) {
        if (g_ignored_n < kMaxExtDevs) g_ignored[g_ignored_n++] = st.st_rdev;
        fprintf(stderr, "[kbd] too many external keyboards (max %d), ignored: %s\n",
                kMaxExtDevs, path);
      }
      ::close(fd);
      continue;
    }

    // 独占：按键不泄漏给控制台；失败仍可用（只记日志，不阻止接管）
    const bool grabbed = ioctl(fd, EVIOCGRAB, 1) == 0;
    Dev& d = g_ext[g_ext_n];
    d = Dev();
    d.fd = fd;
    d.path = path;
    d.name = name;
    d.bustype = bustype;
    d.standard = true;
    d.rdev = st.st_rdev;
    g_ext_n++;
    fprintf(stderr, "[kbd] external keyboard: %s bustype=0x%02x name=%s%s\n",
            path, (unsigned)bustype, name.c_str(),
            grabbed ? "" : " (EVIOCGRAB failed, read-only)");
  }
  closedir(dir);
}

// 修饰原始位掩码：shift=1 alt=2 ctrl=4（编码 xterm/kitty 值时 +1）
uint8_t mods_mask(bool shift)
{
  return (uint8_t)((shift ? 1 : 0) | (g_alt ? 2 : 0) | (g_ctrl ? 4 : 0));
}

uint8_t mods_now()
{
  // 锁定/一次性 Shift 同样计入（否则翻页快捷键与 kitty 编码丢 shift 位）
  return mods_mask(shift_effective());
}

// 可打印键的未上档/上档字符（kitty 键码用；false=非打印键）
bool trans_pair(uint16_t code, char* lo, char* up)
{
  for (const auto& m : kLetters) if (m.code == code) { *lo = m.lo; *up = m.up; return true; }
  for (const auto& m : kDigits)  if (m.code == code) { *lo = m.lo; *up = m.up; return true; }
  if (char c = sym_char(code)) { *lo = c; *up = c; return true; }
  if (code == KEY_SPACE) { *lo = ' '; *up = ' '; return true; }
  return false;
}

// ── 基础层键位表（DT linux,keymap：扫描码 → 基础层键码）────────────
// 来源：project_clones/dtov-src/modules/tca8418-1.0/tca8418_m5stack.dts
// （CardputerZero v5 overlay；5×10 矩阵，row_shift=4 → scan=(row<<4)|col，
//  与驱动回放的 MSC_SCAN 完全一致）。
// 用途：Fn/Sym 的"换层"由内核驱动在 evdev 之前按 keymap1/keymap2 完成，
// 用户态拿到的已经是重映射后的键码；只有"迟到 One-shot 已作废"的窗口
// （见 poll_mods）才用本表把下一个键反查回基础层，抵消固件多留一次换层。
struct ScanKey { uint8_t scan; uint16_t code; };
const ScanKey kBaseKeymap[] = {
  {0x00,   3},  // KEY_2
  {0x01,   4},  // KEY_3
  {0x02,   5},  // KEY_4
  {0x03,   6},  // KEY_5
  {0x04,   7},  // KEY_6
  {0x05,   8},  // KEY_7
  {0x06,   9},  // KEY_8
  {0x07,  10},  // KEY_9
  {0x08,  11},  // KEY_0
  {0x09,  14},  // KEY_BACKSPACE
  {0x10,  16},  // KEY_Q
  {0x11,  17},  // KEY_W
  {0x12,  18},  // KEY_E
  {0x13,  19},  // KEY_R
  {0x14,  20},  // KEY_T
  {0x15,  21},  // KEY_Y
  {0x16,  22},  // KEY_U
  {0x17,  23},  // KEY_I
  {0x18,  24},  // KEY_O
  {0x19,  25},  // KEY_P
  {0x20,  30},  // KEY_A
  {0x21,  31},  // KEY_S
  {0x22,  32},  // KEY_D
  {0x23,  33},  // KEY_F
  {0x24,  34},  // KEY_G
  {0x25,  35},  // KEY_H
  {0x26,  36},  // KEY_J
  {0x27,  37},  // KEY_K
  {0x28,  38},  // KEY_L
  {0x29,  28},  // KEY_ENTER
  {0x30,  29},  // KEY_LEFTCTRL
  {0x31,  56},  // KEY_LEFTALT
  {0x32,  44},  // KEY_Z
  {0x33,  45},  // KEY_X
  {0x34,  46},  // KEY_C
  {0x35,  47},  // KEY_V
  {0x36,  48},  // KEY_B
  {0x37,  49},  // KEY_N
  {0x38,  50},  // KEY_M
  {0x39,  57},  // KEY_SPACE
  {0x40,   1},  // KEY_ESC
  {0x41,  42},  // KEY_LEFTSHIFT（FSM 拦截，不会作为普通键出现）
  {0x42, 389},  // KEY_DVD（Fn，FSM 拦截）
  {0x43, 383},  // KEY_CD（Sym，FSM 拦截）
  {0x44,   2},  // KEY_1
  {0x49,  15},  // KEY_TAB
};

uint16_t base_keycode_for_scan(int scan)
{
  if (scan < 0) return 0;
  for (const auto& k : kBaseKeymap) if (k.scan == scan) return k.code;
  return 0;
}

// ── 迟到 One-shot 换层补偿的按键映射（按下/松开成对，保证 KKP 键码一致）──
struct StaleRestore { uint16_t rx; uint16_t eff; };
constexpr int kStaleRestoreMax = 4;
StaleRestore g_restore[kStaleRestoreMax];
int          g_restore_n = 0;      // g_restore[] 有效条数
int          g_last_scan = -1;     // 最近一次 EV_MSC/MSC_SCAN 的矩阵扫描码
bool         g_scan_fresh = false; // 该扫描码是否属于紧接的这次 EV_KEY

void remember_restore(uint16_t rx, uint16_t eff)
{
  for (int i = 0; i < g_restore_n; i++)
    if (g_restore[i].rx == rx) { g_restore[i].eff = eff; return; }
  if (g_restore_n == kStaleRestoreMax) {           // 丢最旧（极端 rollover）
    memmove(&g_restore[0], &g_restore[1], sizeof(StaleRestore) * (kStaleRestoreMax - 1));
    g_restore_n--;
  }
  g_restore[g_restore_n++] = StaleRestore{rx, eff};
}

// 取出并清除该 rx 键码在按下时记录的还原码（0 = 无记录）
uint16_t take_restore(uint16_t rx)
{
  for (int i = 0; i < g_restore_n; i++) {
    if (g_restore[i].rx != rx) continue;
    const uint16_t eff = g_restore[i].eff;
    memmove(&g_restore[i], &g_restore[i + 1],
            sizeof(StaleRestore) * (size_t)(g_restore_n - i - 1));
    g_restore_n--;
    return eff;
  }
  return 0;
}

void process(const struct input_event& ev)
{
  if (ev.type == EV_MSC) {                   // 驱动给每个键先报 MSC_SCAN（矩阵扫描码）
    if (ev.code == MSC_SCAN) { g_last_scan = (int)ev.value; g_scan_fresh = true; }
    return;
  }
  if (ev.type != EV_KEY) return;
  const uint16_t rx_code = ev.code;          // 内核按当前层重映射后的键码
  const int      val     = ev.value;         // 0=释放 1=按下 2=内核重复
  const int      scan    = g_scan_fresh ? g_last_scan : -1;
  g_scan_fresh = false;

  if (val == 2) return;                      // 内核重复事件忽略，本驱动自管重发

  // 系统电源键过滤（第二道防线）：误触/注入 KEY_POWER/SLEEP/SUSPEND 不再直达电源动作
  if (rx_code == 116 /*KEY_POWER*/ || rx_code == 142 /*KEY_SLEEP*/ ||
      rx_code == 205 /*KEY_SUSPEND*/ || rx_code == 143 /*KEY_WAKEUP*/)
    return;

  // ── 三态修饰键 Shift/Fn/Sym：以 evdev 事件维护"逻辑按住"的代次；
  //    回显片段/锁定仍由状态源驱动（poll_mods），此处只跟踪事件。
  //    驱动在物理按下时合成 down，在 One-shot 被消费/锁定退出/按住松开时合成 up。
  const int mod_i = mod_index_of(rx_code);
  if (mod_i >= 0) {
    if (val != 0) note_mod_down(mod_i);
    else          note_mod_up(mod_i);
    return;
  }

  // ── 普通键按下：固件眼里的"下一个字符"——先同步校准一次修饰状态
  //    （消除 25ms 轮询窗口），再把 One-shot 令牌**恰好消费一次**。
  //    字符上档随后只在 shift_effective() 里用"当时的事件+令牌"判定，
  //    不把 poll_mods 的结果跨字符持久化。
  bool shift_applied = false;
  if (val == 1) {
    if (g_mod_src != ModSrc::None) poll_mods();   // 同步校准：消除 25ms 轮询窗口
    shift_applied = shift_effective();            // 先定格本字符生效的 shift
    consume_one_shot();                           // 再恰好消费一次
  }

  // ── 迟到 One-shot 的换层补偿：Fn/Sym 的层选择在内核完成，固件若把一次性
  //    留到第二个键，第二键会以 Fn/Sym 层的键码到达（例如 Fn+A 位置的
  //    KEY_MUTE）；用 MSC_SCAN 反查基础层键码把它还原成本来的按键。
  //    按下时记录 rx→base，松开复用同一映射（保持按下/松开键码一致）。
  uint16_t code = rx_code;
  if (val == 1) {
    if (g_mods[kModFn].stale || g_mods[kModSym].stale) {
      const uint16_t base = base_keycode_for_scan(scan);
      if (base != 0 && base != rx_code) {
        code = base;
        remember_restore(rx_code, code);
        fprintf(stderr, "[kbd] late one-shot: scan=0x%02x remapped %u -> base %u\n",
                (unsigned)(scan & 0xff), (unsigned)rx_code, (unsigned)code);
      }
    }
  } else if (val == 0) {
    const uint16_t eff = take_restore(rx_code);
    if (eff != 0) code = eff;
  }

  if (code == KEY_ESC) {
    if (val == 1) {
      g_esc_down = true; g_esc_fired = false; g_esc_hinted = false;
      g_esc_since = now_ms();
      g_rep_code = -1;
    } else if (val == 0) {
      // 短按（<0.5s，未到提示点）Esc：按/松成对上报（kitty 事件类型用；
      // 旧路径只消费按下）。已提示后中途松开：不产生事件（toast 自然消失）。
      if (g_esc_down && !g_esc_hinted && !g_esc_fired) {
        KeyEvent k; k.code = code; k.mods = mods_now();
        k.ev = 1; push(Item::Key, "Esc", k);
        k.ev = 3; push(Item::Key, "Esc", k);
      }
      g_esc_down = false; g_esc_hinted = false;   // 松开：复位提示状态位
    }
    return;
  }

  // ── Ctrl/Alt：标准键盘行为——按下/松开回显名称片段 ──────────────
  const char* frag_down = nullptr;
  const char* frag_up   = nullptr;
  switch (code) {
    case 29: case 97:  frag_down = "Ct"; frag_up = "rl"; break;   // Ctrl
    case 56: case 100: frag_down = "Al"; frag_up = "t";  break;   // Alt
    default: break;
  }
  if (frag_down != nullptr) {
    const bool down = (val != 0);
    const bool is_ctrl = (code == 29 || code == 97);
    if (is_ctrl) g_ctrl = down; else g_alt = down;
    push(Item::Mod, is_ctrl ? (down ? "ctrl_on" : "ctrl_off")
                            : (down ? "alt_on"  : "alt_off"));
    push(Item::Aux, down ? frag_down : frag_up);        // 仅调试显示，不参与输入
    return;
  }

  if (code == 125 || code == 126) return;    // Meta：静默跳过
  if (code == KEY_CAPSLOCK) return;          // Caps 直接跳过，不实现

  // ── Fn 层窗口快捷键（贴纸：播放/暂停、快退、快进、亮度-/+、静音、Help）──
  //  KEY_MUTE=Fn+A（剥离会话）、KEY_HELP=Fn+H（空窗显示帮助）：内核 keymap2
  //  只在 Fn 层产生这两个键码，无需再查 Fn 状态。
  {
    Cmd c = Cmd::None;
    switch (code) {
      case KEY_PLAYPAUSE:     c = Cmd::Close;    break;
      case KEY_PREVIOUSSONG:  c = Cmd::PrevWin;  break;
      case KEY_NEXTSONG:      c = Cmd::NextWin;  break;
      case KEY_BRIGHTNESSDOWN:c = Cmd::FontDown; break;
      case KEY_BRIGHTNESSUP:  c = Cmd::FontUp;   break;
      case KEY_MUTE:          c = Cmd::Detach;   break;
      case KEY_HELP:          c = Cmd::ShowHelp; break;
      default: break;
    }
    if (c != Cmd::None) {
      if (val == 1) push(Item::Cmd, std::to_string((int)c));
      return;                                 // 不产生文本、不自动重发
    }
  }

  // ── 结构化事件公共信息（kitty 键盘协议编码用）──────────────────
  KeyEvent kev;
  kev.code = code;
  // 按下事件必须带"本字符实际生效的 shift"（令牌已在上面消费，不能事后重算）
  kev.mods = (val == 0) ? mods_now() : mods_mask(shift_applied);
  trans_pair(code, &kev.lo, &kev.up);

  if (val == 0) {                            // 松开：结束自动重发 + 上报松开事件
    if (code == g_rep_code) g_rep_code = -1;
    kev.ev = 3;
    push(Item::Key, translate(code, shift_effective()), kev);
    return;
  }

  // ── 按下：回显一次并启动长按自动重发（500ms 后每 50ms）──────────
  std::string text = translate(code, shift_applied);
  if (text.empty()) return;                           // 未知键：不产生任何输出
  kev.ev = 1;
  push(Item::Key, text, kev);
  g_rep_code = code;
  g_rep_text = text;
  g_rep_kev  = kev;
  g_rep_next = now_ms() + kRepeatDelayMs;
}

void reader_loop()
{
  uint64_t next_mod_poll = now_ms();
  while (g_running.load(std::memory_order_acquire)) {
    if (g_primary.fd < 0 && g_ext_n == 0) break;       // 所有设备都没了

    // 唤醒时刻取最近的死线（重发 / 长按 Esc / 修饰状态轮询 / 外接设备 / 重扫）
    const uint64_t now = now_ms();
    int to = 100;
    if (g_rep_code >= 0) {
      int rem = (int)(g_rep_next - now);
      if (rem < to) to = rem;
    }
    if (g_esc_down && !g_esc_fired) {
      // 下一个死线：未提示时是提示阈值，已提示是退出阈值
      const int target = g_esc_hinted ? kEscHoldMs : kEscHintMs;
      int rem = target - (int)(now - g_esc_since);
      if (rem < to) to = rem;
    }
    if (g_primary.fd >= 0 && !g_primary.standard && g_mod_src != ModSrc::None) {
      int rem = (int)(next_mod_poll - now);
      if (rem < to) to = rem;
    }
    if (g_primary.fd >= 0 && g_primary.standard) {
      int rem = dev_deadline_rem(g_primary, now);
      if (rem < to) to = rem;
    }
    for (int i = 0; i < g_ext_n; i++) {
      int rem = dev_deadline_rem(g_ext[i], now);
      if (rem < to) to = rem;
    }
    {
      int rem = (int)(g_next_scan - now);
      if (rem < to) to = rem;
    }
    if (to < 0) to = 0;

    // 多 fd 并行 poll：主设备 + 所有外接键盘
    struct pollfd pfds[1 + kMaxExtDevs];
    int8_t        slots[1 + kMaxExtDevs];     // -1=板载主设备，-2=标准布局主设备，>=0=外接下标
    nfds_t nfds = 0;
    if (g_primary.fd >= 0) {
      pfds[nfds] = pollfd{g_primary.fd, POLLIN, 0};
      slots[nfds] = g_primary.standard ? -2 : -1;
      nfds++;
    }
    for (int i = 0; i < g_ext_n; i++) {
      pfds[nfds] = pollfd{g_ext[i].fd, POLLIN, 0};
      slots[nfds] = (int8_t)i;
      nfds++;
    }

    bool        dead_ext[kMaxExtDevs] = {false};
    const int   pr = (nfds > 0) ? ::poll(pfds, nfds, to) : 0;
    if (pr > 0) {
      for (nfds_t k = 0; k < nfds; k++) {
        if ((pfds[k].revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL)) == 0) continue;
        const int slot = slots[k];
        if (slot >= 0 && dead_ext[slot]) continue;

        struct input_event evs[16];
        const ssize_t n = read(pfds[k].fd, evs, sizeof evs);
        if (n > 0) {
          const size_t cnt = (size_t)n / sizeof(struct input_event);
          for (size_t j = 0; j < cnt; j++) {
            if (slot < 0) process(evs[j]);
            else          process_external(g_ext[slot], slot, evs[j]);
          }
          continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;

        // 设备消失（ENODEV/EOF 等）：只关这一个 fd，其它设备继续工作
        const char* reason = (n == 0) ? "EOF" : strerror(errno);
        if (slot < 0) {
          fprintf(stderr, "[kbd] primary keyboard lost: %s (%s)\n",
                  g_primary.path.c_str(), reason);
          const bool was_standard = g_primary.standard;
          close_dev(g_primary);
          if (!was_standard) {                 // 板载：清掉重发/Esc 计时，避免幽灵重发
            g_rep_code = -1;
            g_esc_down = false;
            g_esc_fired = false;
            g_esc_hinted = false;
          }
        } else {
          fprintf(stderr, "[kbd] external keyboard lost: %s\n", g_ext[slot].path.c_str());
          close_dev(g_ext[slot]);
          dead_ext[slot] = true;
        }
      }
      // 压缩外接设备表（保持顺序；指针/下标不跨轮持有）
      int w = 0;
      for (int i = 0; i < g_ext_n; i++) {
        if (dead_ext[i]) continue;
        if (w != i) g_ext[w] = g_ext[i];
        w++;
      }
      for (int i = w; i < g_ext_n; i++) g_ext[i] = Dev();
      g_ext_n = w;
    }

    // 修饰状态轮询：只对板载设备有意义（外接键盘的 Shift/Ctrl/Alt 由 evdev 事件维护）
    if (now_ms() >= next_mod_poll) {
      if (g_primary.fd >= 0 && !g_primary.standard && g_mod_src != ModSrc::None) poll_mods();
      next_mod_poll = now_ms() + kModPollMs;
    }

    // 板载长按 Esc 两段式：0.5s 提示（EscHint，只报一次）→ 2s Quit
    if (g_esc_down && !g_esc_fired && !g_esc_hinted &&
        now_ms() - g_esc_since >= (uint64_t)kEscHintMs) {
      g_esc_hinted = true;
      push(Item::EscHint, "");
      continue;
    }
    if (g_esc_down && !g_esc_fired && now_ms() - g_esc_since >= (uint64_t)kEscHoldMs) {
      g_esc_fired = true;
      g_esc_down  = false;
      g_rep_code  = -1;
      push(Item::Quit, "");
      continue;
    }

    // 板载自动重发（原逻辑不变）
    if (g_rep_code >= 0 && now_ms() >= g_rep_next) {
      KeyEvent k = g_rep_kev;
      k.ev = 2;                                   // kitty 事件类型：重复
      k.mods = mods_now();
      push(Item::Key, g_rep_text, k);
      g_rep_next = now_ms() + kRepeatIntervalMs;
    }

    // 外接键盘（及标准布局主设备）的长按 Esc / 自动重发定时器
    tick_standard_devs();

    // 热插拔：定期重扫 /dev/input，新插入的外接键盘自动接管
    if (now_ms() >= g_next_scan) {
      scan_external();
      g_next_scan = now_ms() + kRescanMs;
    }
  }

  if (g_primary.fd < 0 && g_ext_n == 0 && !g_lost.load(std::memory_order_acquire)) {
    fprintf(stderr, "[kbd] all keyboard devices lost, input thread exit\n");
    g_lost.store(true, std::memory_order_release);   // 通知 session：干净退出而非死键盘
  }
}

void open_mod_source()
{
  // ① 测试接缝：CARDTERM_MODSTATE=<file>（"shift sym fn"，legacy 语义 0-3）
  const char* seam = std::getenv("CARDTERM_MODSTATE");
  if (seam != nullptr && *seam != '\0') {
    g_mod_seam = seam;
    g_mod_src  = ModSrc::Seam;
    return;
  }
  // ② sysfs 快照（较新内核驱动）
  int fd = ::open("/sys/bus/i2c/devices/1-0034/modifier_state", O_RDONLY | O_CLOEXEC);
  if (fd >= 0) {
    g_mod_fd  = fd;
    g_mod_src = ModSrc::Sysfs;
    return;
  }
  // ③ legacy：m5ioe1(0x4F) 寄存器，同 Keyboard-Guide 官方读法
  fd = ::open("/dev/i2c-1", O_RDWR | O_CLOEXEC);
  if (fd >= 0 && ioctl(fd, I2C_SLAVE_FORCE, 0x4F) == 0) {
    g_mod_fd  = fd;
    g_mod_src = ModSrc::I2c;
    return;
  }
  if (fd >= 0) ::close(fd);
  g_mod_src = ModSrc::None;                 // 无状态源：三键退化为普通按键（不回显）
}

// 打开主设备（CARDTERM_KBD / APPLaunch 注入 / 板载默认路径）。失败返回 false
// 且不改变 g_primary。grab_required=true 时 EVIOCGRAB 失败即放弃（主键盘路径
// 的旧语义）；托管模式（APPLaunch 协作启动）下不独占：启动器靠共享 evdev fd
// 监视 ESC 长按做 SIGTERM→SIGKILL 兜底，EVIOCGRAB 会掐死这条兜底链。
bool open_primary(const char* path, bool explicit_env)
{
  int fd = ::open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) {
    if (explicit_env) fprintf(stderr, "[kbd] open %s -> %s\n", path, strerror(errno));
    return false;
  }
  struct stat st {};
  if (fstat(fd, &st) != 0) { ::close(fd); return false; }

  uint32_t bustype = 0;
  std::string name;
  dev_identity(fd, &bustype, &name);          // 失败不致命：未知设备按板载旧语义
  const bool standard = primary_is_standard(bustype, name, explicit_env);

  const bool coop = applaunch_env::coop_managed();
  if (!coop && ioctl(fd, EVIOCGRAB, 1) < 0) {  // 独占：按键不泄漏给控制台/其他进程
    fprintf(stderr, "[kbd] EVIOCGRAB %s -> %s\n", path, strerror(errno));
    ::close(fd);
    return false;
  }

  g_primary = Dev();
  g_primary.fd       = fd;
  g_primary.path     = path;
  g_primary.name     = name;
  g_primary.bustype  = bustype;
  g_primary.standard = standard;
  g_primary.rdev     = st.st_rdev;
  fprintf(stderr, "[kbd] open %s -> ok%s (bustype=0x%02x name=%s layout=%s%s)\n",
          path, explicit_env ? "" : " (auto)", (unsigned)bustype, name.c_str(),
          standard ? "standard-US" : "onboard", coop ? ", shared" : "");
  return true;
}

}  // namespace

bool open()
{
  if (g_primary.fd >= 0) return true;

  // APPLaunch 注入的键映射（loadkeys 格式）优先于内置符号表；未注入/解析
  // 失败时保持内置表语义（见 sym_char()）。
  if (const char* km = applaunch_env::keyboard_map_path()) load_keymap(km);

  const char* env = std::getenv("CARDTERM_KBD");
  // 主设备查找链：CARDTERM_KBD（测试注入，严格）→ APPLAUNCH 注入节点
  //   → /dev/input/cardputer-zero-internal → by-path 板载默认。
  const char* applaunch_dev = applaunch_env::keyboard_device_hint();
  const char* paths[3] = {
    applaunch_dev,
    "/dev/input/cardputer-zero-internal",
    "/dev/input/by-path/platform-3f804000.i2c-event",
  };
  if (env != nullptr && *env != '\0') {
    // 显式指定设备：严格模式，不静默回退
    if (!open_primary(env, /*explicit_env=*/true)) return false;
  } else {
    for (int i = 0; i < 3 && g_primary.fd < 0; i++)
      if (paths[i] != nullptr) open_primary(paths[i], false);
  }
  if (g_primary.fd < 0) { fprintf(stderr, "[kbd] no device\n"); return false; }

  // 修饰状态源只对板载键盘有意义：外接键盘的 Shift/Ctrl/Alt 由 evdev 事件维护，
  // 板载的 Shift/Fn/Sym 三态才需要读内核 FSM（sysfs / i2c / 测试接缝）。
  if (!g_primary.standard) open_mod_source();

  // 启动时先扫一次 /dev/input：启动前已插好的 USB/蓝牙键盘直接接管
  scan_external();

  g_lost.store(false, std::memory_order_release);
  g_running.store(true, std::memory_order_release);
  g_next_scan = now_ms() + kRescanMs;
  g_thread = std::thread(reader_loop);      // 生产者：读键线程
  return true;
}

void close()
{
  if (!g_running.load() && g_primary.fd < 0 && g_ext_n == 0) return;
  g_running.store(false, std::memory_order_release);
  if (g_thread.joinable()) g_thread.join();
  close_dev(g_primary);                     // 主设备：EVIOCGRAB 0 + close
  for (int i = 0; i < g_ext_n; i++) close_dev(g_ext[i]);
  g_ext_n = 0;
  g_ignored_n = 0;
  g_next_scan = 0;
  if (g_mod_fd >= 0) { ::close(g_mod_fd); g_mod_fd = -1; }
  g_mod_src = ModSrc::None;
  g_ctrl = false; g_alt = false;
  for (auto& m : g_mods) {
    m.announced = false; m.mode = MOD_OFF;
    m.down = false; m.gen = 0; m.used = false; m.one_shot = false; m.stale = false;
  }
  g_rep_code = -1; g_esc_down = false; g_esc_fired = false; g_esc_hinted = false;
  g_restore_n = 0; g_last_scan = -1; g_scan_fresh = false;
  std::lock_guard<std::mutex> lk(g_mtx);
  g_q.clear();
}

bool device_lost() { return g_lost.load(std::memory_order_acquire); }

// 板载逻辑 Shift 或任一外接键盘的物理 Shift 按住（无外接键盘时与旧行为一致）
bool shift_down()
{
  if (g_mods[kModShift].down) return true;   // 板载（含 One-shot 上膛窗口）
  if (g_primary.fd >= 0 && g_primary.standard && g_primary.shift) return true;
  for (int i = 0; i < g_ext_n; i++)
    if (g_ext[i].shift) return true;
  return false;
}

Poll poll(int timeout_ms, std::string& echo, KeyEvent* kev, Cmd* cmd)
{
  echo.clear();
  if (kev != nullptr) *kev = KeyEvent();
  if (cmd != nullptr) *cmd = Cmd::None;
  std::unique_lock<std::mutex> lk(g_mtx);
  g_cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                [] { return !g_q.empty(); });
  if (g_q.empty()) return Poll::Timeout;

  Item it = std::move(g_q.front());
  g_q.pop_front();
  if (it.kind == Item::Quit) return Poll::Quit;
  if (it.kind == Item::EscHint) return Poll::EscHint;   // 按住 Esc 0.5s 提示（无文本）
  if (it.kind == Item::Mod) { echo = std::move(it.text); return Poll::Mod; }
  if (it.kind == Item::Cmd) {
    if (cmd != nullptr) *cmd = (Cmd)atoi(it.text.c_str());
    return Poll::Cmd;
  }
  echo = std::move(it.text);
  if (kev != nullptr) *kev = it.kev;
  if (it.kind == Item::Aux) return Poll::Aux;
  return Poll::Echo;
}

}  // namespace kbd
