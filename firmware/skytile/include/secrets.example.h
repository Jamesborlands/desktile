// Copy to secrets.h (gitignored) and fill in.
#pragma once
#define WIFI_SSID "your-2.4GHz-network"
#define WIFI_PASS "your-password"
#define HOME_LAT 32.95
#define HOME_LON -117.25
#define WORKER_URL "https://skytile.<your-subdomain>.workers.dev"
#define TILE_KEY "same value as the Worker's TILE_KEY secret"
#define OTA_PASS "password for wireless updates (tools/ota.sh reads it from secrets.h)"
