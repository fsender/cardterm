#!/bin/sh
# reinit_lcd.sh — 板载 LCD（ST7789v SPI 面板）花屏重初始化
# 机制: ①面板 reset sysfs（上电复位+初始化序列，安全、不断设备）
#       ②fbcon 重新点亮（消残影）
#       ③--full: SPI 面板驱动解绑重绑（完整重新 probe；注意：占用 /dev/fb0 的
#         图形程序（如 cardterm）会因 fb 映射失效而需重启，仅在花屏顽固时用）
# 用法: ./reinit_lcd.sh [--full]
set -u
[ "$(id -u)" = 0 ] || exec sudo "$0" "$@"

FULL=0
[ "${1:-}" = "--full" ] && FULL=1

# ── 定位 SPI 面板设备（兼容节点含 st7789） ──
DEV=""
for d in /sys/bus/spi/devices/*; do
  [ -e "$d/uevent" ] || continue
  if grep -q "st7789" "$d/uevent" 2>/dev/null; then DEV="$d"; break; fi
done
if [ -z "$DEV" ]; then
  echo "reinit_lcd: 未找到 SPI 面板设备(st7789)" >&2
  exit 1
fi
NAME=${DEV##*/}
echo "reinit_lcd: 面板设备 $NAME ($(grep OF_FULLNAME "$DEV/uevent" | cut -d= -f2))"

# ── ① 面板复位 + 初始化序列 ──
if [ -e "$DEV/reset" ]; then
  if echo 1 > "$DEV/reset" 2>/dev/null; then
    echo "reinit_lcd: ① 面板 reset+初始化序列 已触发"
  else
    echo "reinit_lcd: ① reset 写入失败(继续)" >&2
  fi
fi

# ── ② fbcon 重新点亮（blank/unblank 强制重画） ──
for fb in /sys/class/graphics/fb[0-9]; do
  [ -e "$fb/blank" ] || continue
  echo 1 > "$fb/blank" 2>/dev/null
  echo 0 > "$fb/blank" 2>/dev/null
done
echo "reinit_lcd: ② fbcon 重新点亮 完成"

# ── ③ 可选: 驱动重绑定（完整重 probe） ──
if [ "$FULL" = 1 ]; then
  if [ -L "$DEV/driver" ]; then
    DRV=$(readlink -f "$DEV/driver")       # 先解析真实路径(unbind 后软链会消失)
    DRVN=${DRV##*/}
    echo "reinit_lcd: ③ 解绑/重绑 $DRVN（屏幕将短暂黑屏）"
    echo "$NAME" > "$DRV/unbind" 2>/dev/null || echo "reinit_lcd: 解绑失败" >&2
    sleep 1
    echo "$NAME" > "$DRV/bind" 2>/dev/null || echo "reinit_lcd: 重绑失败" >&2
    sleep 1
    grep -q DRIVER "$DEV/uevent" 2>/dev/null \
      && echo "reinit_lcd: ③ 重 probe 完成 ($(grep DRIVER "$DEV/uevent"))" \
      || echo "reinit_lcd: ③ 重绑后驱动未挂上，请重启确认" >&2
  fi
fi

# ── 校验 ──
CON=""
for c in /sys/class/drm/card*-SPI-*/status; do
  [ -e "$c" ] && CON="$c"
done
if [ -n "$CON" ]; then
  echo "reinit_lcd: 校验 DRM 连接器 $(dirname "$CON" | sed 's|.*/||') = $(cat "$CON")"
fi
echo "reinit_lcd: 完成"
