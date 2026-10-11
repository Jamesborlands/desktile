#!/bin/sh
# Build and flash SkyTile over WiFi (needs the tile on the same network).
# Usage: tools/ota.sh [host]   (default host: skytile.local)
set -e
cd "$(dirname "$0")/.."
HOST="${1:-skytile.local}"
PASS=$(sed -n 's/^#define OTA_PASS "\(.*\)"/\1/p' include/secrets.h)
[ -n "$PASS" ] || { echo "OTA_PASS missing from include/secrets.h"; exit 1; }
PIO="${PIO:-$HOME/.local/pio61/bin/pio}"
"$PIO" run
# Resolve skytile.local with macOS's resolver (espota's own lookup of .local names is unreliable).
IP=$(dscacheutil -q host -a name "$HOST" | awk '/^ip_address/ {print $2; exit}')
[ -n "$IP" ] || IP="$HOST"
echo "Sending to $HOST ($IP)"
ESPOTA="$HOME/.platformio/packages/framework-arduinoespressif32/tools/espota.py"
exec "$HOME/.platformio/penv/bin/python" "$ESPOTA" -i "$IP" -a "$PASS" -f .pio/build/advance-hmi/firmware.bin -r
