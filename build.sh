#!/usr/bin/env bash
# 一键编译 / 烧录。需要先装好 esp32 core 和三个库，见 README。
set -euo pipefail

if [ -n "${ARDUINO_CLI:-}" ]; then
  CLI="$ARDUINO_CLI"
elif command -v arduino-cli >/dev/null 2>&1; then
  CLI="arduino-cli"
elif [ -x "$HOME/tools/arduino-cli" ]; then
  CLI="$HOME/tools/arduino-cli"
else
  echo "找不到 arduino-cli，请先安装或设置 ARDUINO_CLI 环境变量" >&2; exit 1
fi
# 8MB flash + 3MB app 分区：两套 GB2312 中文点阵就要 1.7MB，默认的 1.2MB 放不下
FQBN="${FQBN:-esp32:esp32:esp32:FlashSize=8M,PartitionScheme=default_8MB}"
SKETCH="$(cd "$(dirname "$0")" && pwd)/firmware/epaper_week"

case "${1:-compile}" in
  compile)
    "$CLI" compile --fqbn "$FQBN" --warnings all "$SKETCH"
    ;;
  upload)
    : "${2:?用法: ./build.sh upload /dev/cu.usbserial-XXXX}"
    "$CLI" upload --fqbn "$FQBN" -p "$2" "$SKETCH"
    ;;
  *)
    echo "用法: ./build.sh [compile|upload <串口>]" >&2
    exit 1
    ;;
esac
