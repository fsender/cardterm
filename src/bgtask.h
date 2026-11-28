// bgtask.h — 后台会话（方案 P：PTY 桥接）客户端/守护进程接口
//
// 架构见 docs/bgtask-design.md §2：
//   正常窗口：cardterm 自己持有 PTY master（cardterm 是真终端，无 tmux）。
//   Fn+A 剥离（park）：master fd 经 SCM_RIGHTS 交给守护进程 bgtaskd；bgtaskd 把
//     程序输出桥接进 tmux 会话 cardterm_PTY_NN（tmux pane 内跑中继进程），
//     ssh 端 `tmux -L cardterm attach -t cardterm_PTY_NN` 可看可输入。
//   重附着（takeover）：新 cardterm 收回 master fd + 纯文本历史（≤buffer_kb），
//     bgtaskd 随即销毁 tmux 会话（"释放该复用器"），程序全程不察觉。
//
// 进程模型：bgtaskd 由 cardterm 按需 fork+setsid+exec（`cardterm --bgtaskd`），
// 存活于 cardterm 之外；程序退出（master EOF）自动清理对应会话，无会话时自退。
#pragma once

#include <string>
#include <vector>
#include <sys/types.h>

namespace bgtask {

// 会话名：cardterm_PTY_NN（n1 = 1 基窗口号，两位零填充）
std::string slot_name(int n1);

// tmux 里是否已有该后台会话（tmux -L cardterm has-session）
bool session_exists(int n1);

// 一次 tmux list-sessions 枚举全部后台会话名（多槽查询用，避免逐槽 popen；rc211）
std::vector<std::string> list_sessions();

// 扫描 01..win_count，返回最小存在会话的 0 基槽位；都没有则 -1（req 8 启动扫描）
int first_existing_session(int win_count);

// 剥离：把 master fd 与子进程信息交给守护进程（成功后调用方关闭自己的 fd 副本，
// 释放窗口）。child/pgid = 会话首进程（spawn 里 setsid，pgid == child）。
bool park(int n1, int master_fd, pid_t child, pid_t pgid,
          int cols, int rows, int hist_lines);

struct Takeover {
  pid_t child = 0;      // 程序 pid（非 cardterm 亲生：退出检测走 EOF）
  pid_t pgid = 0;
  int cols = 0, rows = 0;
  std::string hist;     // 纯文本历史（无颜色；已按 max_hist 截尾）
  int master = -1;      // 拿回的 PTY master fd
};

// 重附着：收回 master fd + 纯文本历史；守护进程随后销毁 tmux 会话
bool takeover(int n1, size_t max_hist, Takeover& out);

// 死亡会话记录（Q6 降级）：会话程序在剥离期间退出时，守护进程在清理前把
// 输出文本与退出码存档；切回/重启到该槽位时由 cardterm 读出展示并删除。
// 文件直读（守护进程可能已退出）；返回 false = 无记录。
bool has_dead(int n1);
bool take_dead(int n1, std::string& text, int& code);

int run_daemon();                       // cardterm --bgtaskd
int run_relay(const char* sock, const char* name);   // cardterm --bgtask-relay <sock> <name>

}  // namespace bgtask
