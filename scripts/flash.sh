#!/usr/bin/env bash
# Build dan flash satu node memakai sdkconfig lokalnya.
# Pemakaian: scripts/flash.sh <node1|node2|node3|node4> [port] [--monitor]
#   port       contoh /dev/cu.usbmodem1101 (kalau kosong, dipilih otomatis)
#   --monitor  buka serial monitor setelah flash (jalankan di terminal interaktif)

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IDF_EXPORT="${IDF_EXPORT:-$HOME/.espressif/v5.5.3/esp-idf/export.sh}"

usage() {
    echo "Pemakaian: $0 <node1|node2|node3|node4> [port] [--monitor]" >&2
    exit 1
}

[ $# -ge 1 ] || usage
NODE="$1"
shift

case "$NODE" in
    node1) DIR=node1_pump_server;  TARGET=esp32c3 ;;
    node2) DIR=node2_sensor_lpn;   TARGET=esp32s3 ;;
    node3) DIR=node3_relay_friend; TARGET=esp32s3 ;;
    node4) DIR=node4_gateway_prov; TARGET=esp32s3 ;;
    *) usage ;;
esac

PORT=""
ACTION="flash"
for arg in "$@"; do
    case "$arg" in
        --monitor) ACTION="flash monitor" ;;
        *) PORT="$arg" ;;
    esac
done

if [ -z "$PORT" ]; then
    PORT=$(ls /dev/cu.usbmodem* /dev/cu.usbserial* 2>/dev/null | head -n 1)
fi
if [ -z "$PORT" ]; then
    echo "Port tidak ditemukan. Sebutkan portnya, misal /dev/cu.usbmodem1101." >&2
    exit 1
fi

. "$IDF_EXPORT" >/dev/null 2>&1 || { echo "ESP-IDF tidak ditemukan di $IDF_EXPORT" >&2; exit 1; }

cd "$ROOT/$DIR" || exit 1
if ! grep -q "^CONFIG_IDF_TARGET=\"$TARGET\"" sdkconfig 2>/dev/null; then
    idf.py set-target "$TARGET" || exit 1
fi

echo "==> $NODE ($TARGET) ke $PORT"
idf.py -p "$PORT" $ACTION
