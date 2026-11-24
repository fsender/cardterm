#!/bin/sh
# set_lcd_spi.sh <MHz> — 设置板载 LCD（ST7789v SPI 面板）SPI 主频
# 机制: ①反编译 boot overlay → 改 spi-max-frequency → 重编译安装（持久，重启保留）
#       ②运行时属性覆盖 overlay（live 设备树立即更新）
#       ③面板复位重初始化（配 ./reinit_lcd.sh 语义）
# 用法: ./set_lcd_spi.sh 60        # 设置为 60MHz
set -u
[ "$(id -u)" = 0 ] || exec sudo "$0" "$@"

if [ $# -ne 1 ]; then
  echo "用法: $0 <MHz>      例: $0 60" >&2
  exit 2
fi
case "$1" in
  ''|*[!0-9]*) echo "错误: 速率必须为整数 MHz（得到 '$1'）" >&2; exit 2 ;;
esac
MHZ=$1
if [ "$MHZ" -lt 1 ] || [ "$MHZ" -gt 200 ]; then
  echo "错误: 速率范围 1-200 MHz（得到 $MHZ）" >&2
  exit 2
fi
HZ=$((MHZ * 1000000))

command -v dtc >/dev/null 2>&1 || { echo "错误: 缺少 dtc（apt install device-tree-compiler）" >&2; exit 1; }

# ── 定位 boot overlay 与面板节点 ──
DTBO=""
for p in /boot/firmware/overlays/cardputerzero-v5-overlay.dtbo \
         /boot/overlays/cardputerzero-v5-overlay.dtbo; do
  [ -e "$p" ] && DTBO="$p" && break
done
[ -n "$DTBO" ] || { echo "错误: 未找到 cardputerzero-v5-overlay.dtbo" >&2; exit 1; }

DEV=""
for d in /sys/bus/spi/devices/*; do
  [ -e "$d/uevent" ] && grep -q "st7789" "$d/uevent" 2>/dev/null && DEV="$d" && break
done
NODEPATH=""
[ -n "$DEV" ] && NODEPATH=$(grep OF_FULLNAME "$DEV/uevent" | cut -d= -f2)
OLD="?"
[ -n "$DEV" ] && [ -e "$DEV/of_node/spi-max-frequency" ] && \
  OLD=$(od -A n -t u4 --endian=big "$DEV/of_node/spi-max-frequency" | tr -d ' ')

echo "set_lcd_spi: 目标 ${MHZ}MHz (${HZ}Hz)  当前 ${OLD}Hz"
echo "set_lcd_spi: overlay = $DTBO"
BAK="${DTBO}.orig-bak"

# ── ① 持久: fdtput 直改 dtbo 的 spi-max-frequency ──
if [ ! -e "$BAK" ]; then cp "$DTBO" "$BAK" && echo "set_lcd_spi: 原始备份 → $BAK"; fi
TMP=$(mktemp -d -p /tmp lcdspi.XXXXXX) || { echo "错误: 无法创建临时目录" >&2; exit 1; }
cp "$DTBO" "$TMP/work.dtbo"
FPATH=""
for f in 0 1 2 3 4 5 6 7 8; do
  for ov in "/fragment@$f/__overlay__/st7789v@0" "/fragment@$f/__overlay__/display"; do
    if fdtget "$TMP/work.dtbo" "$ov" spi-max-frequency >/dev/null 2>&1; then
      FPATH="$ov"; break 2
    fi
  done
done
if [ -n "$FPATH" ]; then
  if fdtput -t u "$TMP/work.dtbo" "$FPATH" spi-max-frequency "$HZ" 2>/dev/null; then
    CHK=$(fdtget "$TMP/work.dtbo" "$FPATH" spi-max-frequency 2>/dev/null)
    if [ "$CHK" = "$HZ" ]; then
      cp "$TMP/work.dtbo" "$DTBO" && echo "set_lcd_spi: ① 持久 overlay 已更新 ($FPATH → ${CHK}Hz, 重启保留)"
    else
      echo "set_lcd_spi: ① 校验失败(回读=$CHK)" >&2
    fi
  else
    echo "set_lcd_spi: ① fdtput 写入失败" >&2
  fi
else
  echo "set_lcd_spi: ① dtbo 中未找到 spi-max-frequency 节点" >&2
fi

# ── ② 运行时: 属性覆盖 overlay（live 设备树） ──
if [ -n "$NODEPATH" ]; then
  cat > "$TMP/spi.dts" <<DEOF
/dts-v1/;
/plugin/;
/ {
  fragment@0 {
    target-path = "$NODEPATH";
    __overlay__ {
      spi-max-frequency = <$HZ>;
    };
  };
};
DEOF
  if dtc -@ -I dts -O dtb "$TMP/spi.dts" -o "$TMP/spi.dtbo" 2>/dev/null; then
    OV=/sys/kernel/config/device-tree/overlays/lcdspi
    rmdir "$OV" 2>/dev/null
    if mkdir "$OV" 2>/dev/null && cat "$TMP/spi.dtbo" > "$OV/dtbo" 2>/dev/null; then
      NOW=$(od -A n -t u4 --endian=big /proc/device-tree${NODEPATH}/spi-max-frequency 2>/dev/null | tr -d ' ')
      echo "set_lcd_spi: ② 运行时设备树已更新 → ${NOW}Hz"
    else
      echo "set_lcd_spi: ② 运行时 overlay 不可用（重启后完全生效）" >&2
    fi
  fi
else
  echo "set_lcd_spi: ② 未定位面板节点，跳过运行时更新" >&2
fi

# ── ③ 面板复位重初始化 ──
if [ -n "$DEV" ] && [ -e "$DEV/reset" ]; then
  echo 1 > "$DEV/reset" 2>/dev/null && echo "set_lcd_spi: ③ 面板已复位重初始化"
fi

rm -rf "$TMP"
echo "set_lcd_spi: 完成（${MHZ}MHz）。如画面异常请执行 ./reinit_lcd.sh；建议重启一次确保主频完全生效。"
