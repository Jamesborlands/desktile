// Copy to secrets.h (gitignored) and fill in.
#pragma once
#define WIFI_SSID "your-2.4GHz-network"
#define WIFI_PASS "your-password"
#define HOME_LAT 32.95
#define HOME_LON -117.25
#define WORKER_URL "https://skytile.<your-subdomain>.workers.dev"
#define TILE_KEY "same value as the Worker's TILE_KEY secret"
#define OTA_PASS "password for wireless updates (tools/ota.sh reads it from secrets.h)"
// Optional: OpenSky API client (opensky-network.org > Account > API client) raises the
// fallback plane feed from 400 to 4,000 credits a day. Leave out to use it anonymously.
// #define OPENSKY_ID "you@example.com-api-client"
// #define OPENSKY_SECRET "..."
