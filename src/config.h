// config.h — cardterm 配置文件（INI）解析
//
// 路径：~/.config/cardterm/config.ini（缺省时自动创建默认样例）
// 默认样例（每一项都带英文注释，见 config.cpp 的 kDefaultIni）：
//   [general]
//   launch=bash                 # 启动命令（留空 = 显示启动器）
//   cwd=home                    # 启动工作目录（home/~/$HOME = 家目录；留空 = 继承）
//   unicode=yes                 # 是否向程序宣称 Unicode（no → LANG=C，TUI 走 ACS 框线）
//   full_line_newline=no        # 满宽行 + LF 语义（no = xterm/vt.c 行为）
//   toast=yes                   # 事件通知（toast 叠加层）开关
//   toast_ms=2000               # toast 显示时长 ms（200–60000，超出钳位）
//
//   [font]
//   font_size=6                 # 初始字号：4 = 4x7，6 = 6x10（默认），8 = 8x14
//
//   [colors]
//   background_color=242424     # 背景色（RRGGBB，foot 默认）
//   #background_image=…         # 背景图（注释状态，提供图片后自行启用）
//   #cursor=…                   # 光标色（注释状态；缺省跟随 bright7）
//   regular0…7 / bright0…7      # 16 色调色板（foot 默认配色）
//
//   [scrollback]
//   lines=2000                  # 最大回滚行数（100–30000；低于 100 按 100，超限钳到 30000）
//   hold_ms=2000                # 手动翻页后保持不跳底时长（上限 60000，超限钳位）
//
//   [text]
//   splash=                     # 开屏文本（空 = 内置默认；颜色标签语法见 session.cpp）
//   help=                       # 帮助页文本（空 = 内置默认帮助页）
#pragma once

#include <cstdint>
#include <string>

namespace cfg {

struct Config {
  uint32_t    bg_color  = 0x212421;  // 默认背景色（RGB888，foot 242424）
  uint32_t    cursor_color = 0;      // 光标颜色 RGB888（0 = 同 bright7）
  std::string bg_image;              // 背景图路径（空 = 纯色背景）
  uint32_t    palette[16] = {        // ANSI 16 色 RGB888（regular0-7 + bright0-7，foot 默认）
    0x212421, 0xF7285A, 0x42B610, 0xE7C700, 0x21AED6, 0xF7AEFF, 0x10C39C, 0xE7E7E7,
    0x636163, 0xFF4D52, 0x31D752, 0xEFEB31, 0x5AC7FF, 0xFFAAF7, 0x21DFC6, 0xFFFFFF,
  };
  std::string launch;                // 启动时默认运行命令（空 = 启动器）
  std::string cwd;                   // 启动工作目录（home/~/$HOME = 家目录；
                                     //   ~/xxx = 家目录下 xxx；其余按字面路径；
                                     //   空 = 继承父进程目录）
  bool        unicode = true;        // 向程序宣称 Unicode 终端（UTF-8 locale）；
                                     //   false = 非 Unicode 环境（LANG=C，TUI 用 ACS 框线）
  int         scrollback = 2000;    // 最大回滚行数（100–30000：少于 100 按 100 计，
                                    //   超过 30000 钳到 30000 并记 stderr）
  int         view_hold_ms = 2000;  // 手动翻页(Shift+PgUp 等)后保持不跳底时长 ms
                                    //   （上限 60000，超出钳位）
  // 满宽行 + 换行符的处理（[general] full_line_newline）
  //   false（默认，xterm/vt.c 语义）：LF 只下移一行、取消延迟环绕 → 无空行
  //   true：满列行先提交自动换行（软换行标记）再下移 → 显示「满列行/空行/后续文本」
  bool        full_line_newline = false;
  // 事件通知（toast 叠加层）：切窗/字号/关窗/退出等瞬时提示，[general] toast
  bool        toast = true;
  int         toast_ms = 2000;      // toast 显示时长 ms（[general] toast_ms；
                                    //   范围 200–60000，超出钳位）
  int         font_size = 6;        // 初始字号：4 = 4x7，6 = 6x10（默认），8 = 8x14
                                    //   （每个窗口都可用 FN+U/I 单独调整）
  // 开屏/帮助页自定义文本（[text] splash / [text] help）：
  //   空 = 用内置默认内容。语法只解析颜色标签，见 session.cpp emit_rich_text：
  //   <br> 换行；<fgcolor=RRGGBB>…</fgcolor>；<bgcolor=RRGGBB>…</bgcolor>；
  //   未闭合的颜色标签生效到末尾；不合法的标签原样显示。
  std::string splash;
  std::string help;
};

// 读取 ~/.config/cardterm/config.ini；文件不存在时写入默认样例。
// 解析失败的项保持默认值。返回是否读到已有文件（false = 用了默认并创建）。
bool load(Config& out);

// 最近一次 load() 成功解析的配置（session 读 splash/help/launch/toast 用）
const Config& current();

// 最近一次 load() 是否因配置文件不存在而新建了默认模板（= 首次运行）
bool config_created();

// 十六进制颜色 "RRGGBB" / "#RRGGBB" → RGB565；失败返回 false
bool parse_hex_color(const char* s, uint32_t& out);

}  // namespace cfg
