#!/bin/bash
# build-deb.sh — 把 cardterm 打包为 APPLaunch 启动项应用 (.deb)
#
# 前置: cmake --preset aarch64-cross && cmake --build build   (Release 分发线;
#       Debug 维护线走 deploy.sh → 设备 ~/apps/cardterm, 勿混用)
# 产物: dist/cardterm_<版本>-<修订>_arm64.deb
#
# 打包规范: documents/dev/dev_tutorial_applaunch_packaging_guide_zh.md
# 图标: 本包自带 packaging/cardterm_100.png (100x100 RGBA, 由 extra/logo.svg 渲染;
# 生成方式见仓库 README/交付说明: headless Chromium 渲染 SVG → ImageMagick 缩放),
# 装入 /usr/share/APPLaunch/share/images/ 并由 cardterm.desktop 的 Icon= 引用。
set -euo pipefail
cd "$(dirname "$0")/.."          # cardterm 仓库根目录

PKG=cardterm
ARCH=arm64
REV=1
BIN=build/cardterm
APPBIN_NAME=cardterm
ICON=packaging/cardterm_100.png

VER=$(sed -n 's/^project(cardterm VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
[ -n "$VER" ] || { echo "无法从 CMakeLists.txt 读取版本号"; exit 1; }
[ -x "$BIN" ] || { echo "缺少 $BIN, 请先用 Release 预设编译"; exit 1; }
[ -f "$ICON" ] || { echo "缺少 $ICON (100x100 APPLaunch 图标)"; exit 1; }

DEB="dist/${PKG}_${VER}-${REV}_${ARCH}.deb"
ROOT=build/deb-root

rm -rf "$ROOT"
mkdir -p "$ROOT/DEBIAN" \
         "$ROOT/usr/share/APPLaunch/bin" \
         "$ROOT/usr/share/APPLaunch/applications" \
         "$ROOT/usr/share/APPLaunch/share/images" \
         "$ROOT/usr/share/doc/cardterm" \
         dist

# 主程序(裁剪符号, Release 发布) + 后台会话管理小二进制（rc216/M1，同包分发）
cp "$BIN" "$ROOT/usr/share/APPLaunch/bin/$APPBIN_NAME"
chmod 755 "$ROOT/usr/share/APPLaunch/bin/$APPBIN_NAME"
cp build/cardterm-bgtask "$ROOT/usr/share/APPLaunch/bin/cardterm-bgtask"
chmod 755 "$ROOT/usr/share/APPLaunch/bin/cardterm-bgtask"

# 用户级 systemd 单元不再随包安装（1.3.1，商店安装路径规则）：cardterm 首次
# 拉起守护前把它生成到用户的 ~/.config/systemd/user/cardterm-bgtask.service
# （仍是用户态 systemd 服务，Restart=no；详见 src/bgtask.cpp ensure_user_unit）。

# 启动项快捷方式 + 本包自带 100x100 图标（.desktop 的 Icon= 指向该绝对路径）
install -m 0644 packaging/cardterm.desktop \
        "$ROOT/usr/share/APPLaunch/applications/cardterm.desktop"
install -m 0644 "$ICON" \
        "$ROOT/usr/share/APPLaunch/share/images/cardterm_100.png"

# 第三方资产许可（字体/图形库）：随包分发必须附原文
cp -r packaging/fonts "$ROOT/usr/share/doc/cardterm/fonts"
# 项目自身许可与资产归属
cp -f LICENSE "$ROOT/usr/share/doc/cardterm/LICENSE" 2>/dev/null || true
cp -f docs/ATTRIBUTION.md "$ROOT/usr/share/doc/cardterm/ATTRIBUTION.md" 2>/dev/null || true
find "$ROOT/usr/share/doc/cardterm" -type f -exec chmod 0644 {} +

# 控制文件
sed "s/@VERSION@/$VER/" packaging/control.in > "$ROOT/DEBIAN/control"
chmod 0644 "$ROOT/DEBIAN/control"

dpkg-deb -b "$ROOT" "$DEB"
echo "== 生成: $DEB =="
dpkg-deb -I "$DEB"
