#!/bin/bash
# make-icon.sh — extra/logo.svg → packaging/cardterm_100.png（APPLaunch 100×100 图标）
#
# 用法: bash packaging/make-icon.sh   （在仓库任意位置执行均可）
# 依赖: ImageMagick `convert`；优先用 headless Chromium 渲染 SVG（对
#       clipPath/渐变/透明度还原准确），没有 Chromium 时退回 ImageMagick
#       自带 MSVG 渲染（细节可能略有差异）。
set -euo pipefail
cd "$(dirname "$0")/.."          # cardterm 仓库根目录

SRC=extra/logo.svg
OUT=packaging/cardterm_100.png
[ -f "$SRC" ] || { echo "缺少 $SRC"; exit 1; }
command -v convert >/dev/null 2>&1 || { echo "缺少 ImageMagick convert"; exit 1; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# 把 SVG 放进与 viewBox 同尺寸（800×500）的透明页面，保证 1:1 渲染
{
  printf '<!doctype html><html><head><meta charset="utf-8"><style>'
  printf 'html,body{margin:0;padding:0;overflow:hidden;background:transparent}'
  printf 'svg{display:block;width:800px;height:500px}</style></head><body>'
  cat "$SRC"
  printf '</body></html>'
} > "$TMP/logo.html"

if command -v chromium >/dev/null 2>&1; then
  chromium --headless=new --no-sandbox --disable-gpu --disable-dev-shm-usage \
    --hide-scrollbars --default-background-color=00000000 \
    --user-data-dir="$TMP/profile" --window-size=800,500 \
    --screenshot="$TMP/render.png" "file://$TMP/logo.html" >/dev/null 2>&1
  RENDER="$TMP/render.png"
else
  convert -background none "$SRC" -resize 800x500 "$TMP/render.png"
  RENDER="$TMP/render.png"
fi

# 等比缩到 100×100 内，居中补透明边 → 100×100 RGBA（APPLaunch 图标规格）
convert "$RENDER" -resize 100x100 -gravity center -background none -extent 100x100 "$OUT"
identify -format 'wrote %f: %wx%h %[channels] %b\n' "$OUT"
