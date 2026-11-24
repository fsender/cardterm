#!/bin/sh
# get_lcd_spi.sh — 只读查询：当前 LCD SPI 主频 与 标称刷新率（无需 root/sudo）
# 数据源: /proc/device-tree 设备树节点（世界可读，反映当前运行态，含运行时覆盖）
#   - spi-max-frequency            → SPI 主频
#   - panel-timing/{clock-frequency,hactive,vactive,前后肩/同步宽} → 标称刷新率
#     标称刷新率 = 像素时钟 / (行合计 x 场合计)，行合计=hactive+HFP+HSYNC+HBP，
#     场合计=vactive+VFP+VSYNC+VBP
# 用法: ./get_lcd_spi.sh
set -u

# u32 大端读取（设备树属性格式）
dt_u32() { od -A n -t u4 --endian=big "$1" 2>/dev/null | tr -d ' '; }

# ── 定位面板节点（st7789，兼容不同 overlay 布局） ──
NODE=""
for n in /proc/device-tree/soc/spi*/st7789v@0 /proc/device-tree/soc/spi*/*/st7789*; do
  [ -d "$n" ] && NODE="$n" && break
done
if [ -z "$NODE" ]; then
  N=$(find /proc/device-tree -maxdepth 5 -type d -name "st7789*" 2>/dev/null | head -1)
  [ -n "$N" ] && NODE="$N"
fi
if [ -z "$NODE" ]; then
  echo "get_lcd_spi: 未找到 LCD 面板设备树节点(st7789)" >&2
  exit 1
fi

# ── SPI 主频 ──
HZ=$(dt_u32 "$NODE/spi-max-frequency")
if [ -z "$HZ" ]; then
  echo "get_lcd_spi: 节点无 spi-max-frequency" >&2
  exit 1
fi
MHZ_INT=$((HZ / 1000000))
MHZ_FRAC=$(( (HZ % 1000000) / 10000 ))

# ── 标称刷新率（panel-timing） ──
PT="$NODE/panel-timing"
CK=$(dt_u32 "$PT/clock-frequency")
HA=$(dt_u32 "$PT/hactive");   VA=$(dt_u32 "$PT/vactive")
HFP=$(dt_u32 "$PT/hfront-porch"); HSW=$(dt_u32 "$PT/hsync-len"); HBP=$(dt_u32 "$PT/hback-porch")
VFP=$(dt_u32 "$PT/vfront-porch"); VSW=$(dt_u32 "$PT/vsync-len"); VBP=$(dt_u32 "$PT/vback-porch")

echo "LCD SPI 主频   : ${MHZ_INT}.${MHZ_FRAC} MHz  (${HZ} Hz)"
if [ -n "$CK" ] && [ -n "$HA" ] && [ -n "$VA" ]; then
  HT=$((HA + HFP + HSW + HBP))
  VT=$((VA + VFP + VSW + VBP))
  if [ "$HT" -gt 0 ] && [ "$VT" -gt 0 ]; then
    # 刷新率 = CK / (HT*VT)，定点一位小数
    DEN=$((HT * VT))
    TENTH=$((CK * 10 / DEN))
    echo "面板像素时钟   : $((CK / 1000000)).$(( (CK % 1000000) / 100000 )) MHz"
    echo "标称刷新率     : $((TENTH / 10)).$((TENTH % 10)) Hz   (${HA}x${VA}, 行合计${HT} x 场合计${VT})"
  else
    echo "get_lcd_spi: panel-timing 行/场合计为 0，无法计算刷新率" >&2
  fi
else
  echo "面板标称刷新率 : (panel-timing 不完整: clock=${CK:-?} h=${HA:-?} v=${VA:-?})" >&2
fi
