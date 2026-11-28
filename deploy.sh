#!/bin/bash
# deploy.sh — Debug 构建部署到 CardputerZero ~/apps/cardterm 并运行
# 用途: 设备本地维护/排查问题(带调试符号, -O0)。正式分发请走
#       packaging/build-deb.sh (Release, 装入 APPLaunch 启动项)。
# 用法: bash deploy.sh   （在项目根目录执行；自动执行 aarch64-debug 预设构建）
set -u
PI=${PI:-me@10.42.0.205}
# SSH 密码不再入库：用密钥登录，或临时 export PI_PASS=...（仅本机实验网络）
SSHP=${PI_PASS:+"sshpass -p $PI_PASS"}
BIN=build-debug/cardterm
DIR=~/apps/cardterm

echo "== 0. debug build (cmake --preset aarch64-debug) =="
cmake --preset aarch64-debug >/dev/null || exit 1
cmake --build build-debug -j"$(nproc)" >/dev/null || exit 1
[ -x "$BIN" ] || { echo "构建产物缺失: $BIN"; exit 1; }

echo "== 1. connectivity =="
$SSHP ssh -o ConnectTimeout=5 -n $PI 'echo SSH_OK' >/dev/null 2>&1 || { echo "board offline"; exit 1; }

echo "== 2. push binary -> ~/apps/cardterm/cardterm =="
$SSHP ssh -n $PI 'mkdir -p '"$DIR"
$SSHP scp -q $BIN $PI:$DIR/cardterm
# 后台会话管理小二进制不再随维护部署（1.3.1）：~/apps/cardterm/cardterm-bgtask
# 不存在；cardterm 需要时走 --bgtaskd 自身回退（旧布局）或安装版同目录二进制。

echo "== 3. run (独占会话, 自动退出) =="
$SSHP ssh -n $PI "cd $DIR && ./cardterm 2>&1; echo 'app exit code: '$?"
