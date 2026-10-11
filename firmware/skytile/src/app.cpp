// SkyTile app: networking, data, and the four screens (Planes, Ships, Space, To-do).
// Touch and knobs both drive the UI through app_input().
//
// Data sources: planes come straight from adsb.lol (it rate-limits Cloudflare's IPs);
// routes, ships, space and to-dos come from the SkyTile Cloudflare Worker.
#include "app.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <lvgl.h>
#include <math.h>
#include <time.h>

#include "map_icons.h"
#include "mapdata.h"
#include "worlddata.h"
#include "tower_icon.h"
#include "secrets.h"

// ---------------------------------------------------------------- constants
static const int MAXP = 150;           // planes kept (nearest first)
static const int MAXRAW = 600;         // planes parsed per fetch before trimming
static const int MAXS = 80;            // ships kept
static const int MAXT = 30;            // to-dos kept
static const int MAXL = 6;             // launches kept
static const float RANGE_MIN = 5, RANGE_MAX = 100, RANGE_DEFAULT = 25;  // outer ring radius, nm
static const uint32_t FETCH_MS = 10000;

static const int MAP_W = 500, MAP_H = 436, BAR_H = 44;
// The map is MAP_W x MAP_H normally and full screen (800 x 480) while idle.
static int g_mw = MAP_W, g_mh = MAP_H;
static inline float ring_px() { return g_mh / 2 - 20; }  // outer range ring radius in px

static volatile float g_range = RANGE_DEFAULT;  // current map range (nm); read by the net task too
static bool g_lite_map = false;  // set while dragging/pinching: coarser coastline, faster redraws
static volatile float g_pan_e = 0, g_pan_n = 0;  // view center offset from home, nm east / north
static const float KLON = 60.0f * 0.8389f;       // nm per degree of longitude at home (cos 32.95)

// Map projection: local flat-earth around home, north up, px relative to the map.
static inline void project(float lat, float lon, float &x, float &y) {
  float ppn = ring_px() / g_range;
  x = g_mw / 2 + ((lon - HOME_LON) * KLON - g_pan_e) * ppn;
  y = g_mh / 2 - ((lat - HOME_LAT) * 60.0f - g_pan_n) * ppn;
}

static const uint32_t C_BG = 0x0a1220, C_LINE = 0x1a2740, C_MUTED = 0x8796ad,
                      C_DIM = 0x5f6e86, C_TEXT = 0xe8edf5, C_PLANE = 0x9fb4d8,
                      C_AMBER = 0xf2a93b, C_TEAL = 0x3fc1b0, C_TODO = 0xdfe8f5,
                      C_SPACE = 0x8fa8ff, C_RED = 0xe5665a;

// ---------------------------------------------------------------- UI mode (read by the net task)
enum Mode { M_PLANES, M_SHIPS, M_SPACE, M_TODO };
static const uint32_t MODE_ACCENT[4] = {C_AMBER, C_TEAL, C_SPACE, C_TODO};
static volatile Mode g_mode = M_PLANES;

// ---------------------------------------------------------------- data model
struct Plane {
  char hex[8];
  char flight[10];
  char type[6];
  char reg[12];
  char desc[40];
  char cat[4];  // ADS-B emitter category: A1 light, A2 small, A3 large, A5 heavy, A7 rotorcraft...
  int alt;  // feet, -1 = on ground / unknown
  float gs, track, lat, lon, dist_nm;
  bool mil;
};

struct Ship {
  char key[12];  // MMSI as text
  char name[24];
  char kind[20];
  char dest[24];
  float lat, lon, sog, cog, dist_nm;
  int hdg, len, age_s, type;  // type = AIS ship type code
};

struct Launch {
  char name[48];
  char rocket[28];
  char loc[40];
  uint32_t t;  // unix seconds
  int dist_mi;
  bool visible;
};

struct SpaceInfo {
  Launch l[MAXL];
  int n;
  bool iss_ok;
  float iss_lat, iss_lon;
  int iss_alt;
  bool overhead;
  bool pass_ok;
  uint32_t pass_t;
  int pass_dur, pass_el;
  char from[4], to[4];
  int16_t track[64][2];  // ISS ground track, (lon, lat) in tenths of a degree
  int track_n;
  uint32_t fetched_ms;
};

struct Todo {
  char id[10];
  char text[124];
  bool done;
};

static SemaphoreHandle_t g_mtx;
static volatile int g_net_status = 0;  // 0 connecting, 1 live, 2 error

// Each feed: the net task fills the *_net copy and sets the flag; app_loop copies it.
static Plane *g_net, *g_pl;
static int g_net_n = 0, g_pl_n = 0;
static volatile bool g_net_new = false;

static Ship *g_sh_net, *g_sh;
static int g_sh_net_n = 0, g_sh_n = 0;
static volatile bool g_sh_new = false;
static bool g_sh_loaded = false;

static SpaceInfo g_sp_net, g_sp;
static volatile bool g_sp_new = false;

static Todo *g_td_net, *g_td;
static int g_td_net_n = 0, g_td_n = 0;
static volatile bool g_td_new = false;
static bool g_td_loaded = false;

// To-do check-offs waiting to be sent to the Worker.
static char g_tq[8][10];
static int g_tq_n = 0;

static bool is_heavy(const char *t) {
  static const char *H[] = {"B744", "B748", "B74S", "A388", "A124", "A225",
                            "C17",  "C5M",  "A346", "A345", "B52",  "B703"};
  for (auto h : H)
    if (!strcmp(t, h)) return true;
  return false;
}
static bool is_interesting(const Plane &p) { return p.mil || is_heavy(p.type); }

// ---------------------------------------------------------------- favorites
static Preferences g_prefs;
static String g_favs;  // comma separated hex codes, e.g. ",a1b2c3,"
static bool is_fav(const char *hex) { return g_favs.indexOf(String(",") + hex + ",") >= 0; }
static void toggle_fav(const char *hex) {
  String k = String(",") + hex + ",";
  if (g_favs.indexOf(k) >= 0) g_favs.replace(k, ",");
  else g_favs += (g_favs.length() ? String(hex) + "," : String(hex) + ",");
  if (g_favs.length() == 0 || g_favs[0] != ',') g_favs = "," + g_favs;
  g_prefs.putString("favs", g_favs);
}

// ---------------------------------------------------------------- network
// ArduinoJson allocator that uses PSRAM, so parsing never fragments internal RAM.
struct PsramAllocator : ArduinoJson::Allocator {
  void *allocate(size_t n) override { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }
  void deallocate(void *p) override { heap_caps_free(p); }
  void *reallocate(void *p, size_t n) override { return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM); }
};
static PsramAllocator g_psram_alloc;

// One HTTPS request. worker=true adds the device key header for the SkyTile Worker.
static bool http_req(const char *method, const char *url, bool worker, String &out, int &code) {
  WiFiClientSecure client;
  client.setInsecure();  // TODO: pin CAs
  HTTPClient http;
  http.setTimeout(8000);
  http.setUserAgent("SkyTile/0.1 (DIY desk display)");  // adsb.lol rejects generic agents
  if (!http.begin(client, url)) { code = -1; return false; }
  if (worker) http.addHeader("X-Tile-Key", TILE_KEY);
  code = strcmp(method, "POST") == 0 ? http.POST("") : http.GET();
  bool ok = code >= 200 && code < 300;
  if (ok) out = http.getString();
  http.end();
  return ok;
}

static void parse_planes(const String &body, float clat, float clon) {
  JsonDocument filter(&g_psram_alloc);
  JsonObject f = filter["ac"].add<JsonObject>();
  for (auto k : {"hex", "flight", "t", "r", "desc", "alt_baro", "gs", "track", "lat", "lon", "dbFlags", "category"})
    f[k] = true;
  JsonDocument doc(&g_psram_alloc);
  if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) {
    g_net_status = 2;
    return;
  }
  static Plane *tmp = (Plane *)heap_caps_malloc(sizeof(Plane) * MAXRAW, MALLOC_CAP_SPIRAM);
  int n = 0;
  for (JsonObject a : doc["ac"].as<JsonArray>()) {
    if (n >= MAXRAW) break;
    if (!a["lat"].is<float>() || !a["lon"].is<float>()) continue;
    Plane &p = tmp[n];
    memset(&p, 0, sizeof p);
    strlcpy(p.hex, a["hex"] | "", sizeof p.hex);
    strlcpy(p.flight, a["flight"] | "", sizeof p.flight);
    for (int i = strlen(p.flight) - 1; i >= 0 && p.flight[i] == ' '; i--) p.flight[i] = 0;
    strlcpy(p.type, a["t"] | "", sizeof p.type);
    strlcpy(p.reg, a["r"] | "", sizeof p.reg);
    strlcpy(p.desc, a["desc"] | "", sizeof p.desc);
    strlcpy(p.cat, a["category"] | "", sizeof p.cat);
    p.alt = a["alt_baro"].is<int>() ? a["alt_baro"].as<int>() : -1;
    p.gs = a["gs"] | 0.0f;
    p.track = a["track"] | 0.0f;
    p.lat = a["lat"];
    p.lon = a["lon"];
    p.mil = ((a["dbFlags"] | 0) & 1) != 0;
    float dx = (p.lon - HOME_LON) * KLON;
    float dy = (p.lat - HOME_LAT) * 60.0f;
    p.dist_nm = sqrtf(dx * dx + dy * dy);
    if (!p.flight[0]) strlcpy(p.flight, p.reg[0] ? p.reg : p.hex, sizeof p.flight);
    n++;
  }
  // nearest to the view center first (that's what is on screen)
  auto key = [&](const Plane &q) {
    float ex = (q.lon - clon) * KLON, ny = (q.lat - clat) * 60.0f;
    return ex * ex + ny * ny;
  };
  for (int i = 1; i < n; i++) {
    Plane k = tmp[i];
    float kk = key(k);
    int j = i - 1;
    while (j >= 0 && key(tmp[j]) > kk) { tmp[j + 1] = tmp[j]; j--; }
    tmp[j + 1] = k;
  }
  if (n > MAXP) n = MAXP;
  xSemaphoreTake(g_mtx, portMAX_DELAY);
  memcpy(g_net, tmp, sizeof(Plane) * n);
  g_net_n = n;
  g_net_new = true;
  xSemaphoreGive(g_mtx);
  g_net_status = 1;
  Serial.printf("[net] %d planes\n", n);
}

static void parse_ships(const String &body) {
  JsonDocument doc(&g_psram_alloc);
  if (deserializeJson(doc, body)) return;
  static Ship *tmp = (Ship *)heap_caps_malloc(sizeof(Ship) * MAXS, MALLOC_CAP_SPIRAM);
  int n = 0;
  for (JsonObject s : doc["ships"].as<JsonArray>()) {
    if (n >= MAXS) break;
    Ship &v = tmp[n];
    memset(&v, 0, sizeof v);
    snprintf(v.key, sizeof v.key, "%ld", (long)(s["mmsi"] | 0L));
    strlcpy(v.name, s["name"] | "", sizeof v.name);
    strlcpy(v.kind, s["kind"] | "Vessel", sizeof v.kind);
    strlcpy(v.dest, s["dest"] | "", sizeof v.dest);
    v.lat = s["lat"] | 0.0f;
    v.lon = s["lon"] | 0.0f;
    v.sog = s["sog"] | 0.0f;
    v.cog = s["cog"] | 0.0f;
    v.hdg = s["hdg"] | 511;
    v.len = s["len"] | 0;
    v.dist_nm = s["dist_nm"] | 0.0f;
    v.age_s = s["age_s"] | 0;
    v.type = s["type"] | 0;
    n++;
  }
  xSemaphoreTake(g_mtx, portMAX_DELAY);
  memcpy(g_sh_net, tmp, sizeof(Ship) * n);
  g_sh_net_n = n;
  g_sh_new = true;
  xSemaphoreGive(g_mtx);
  Serial.printf("[net] %d ships\n", n);
}

static void parse_space(const String &body) {
  JsonDocument doc(&g_psram_alloc);
  if (deserializeJson(doc, body)) return;
  SpaceInfo s = {};
  for (JsonObject l : doc["launches"].as<JsonArray>()) {
    if (s.n >= MAXL) break;
    Launch &x = s.l[s.n++];
    strlcpy(x.name, l["name"] | "", sizeof x.name);
    strlcpy(x.rocket, l["rocket"] | "", sizeof x.rocket);
    strlcpy(x.loc, l["location"] | "", sizeof x.loc);
    x.t = l["t"] | 0UL;
    x.dist_mi = l["dist_mi"] | -1;
    x.visible = l["visible"] | false;
  }
  JsonObject iss = doc["iss"];
  if (!iss.isNull()) {
    s.iss_ok = true;
    s.iss_lat = iss["lat"] | 0.0f;
    s.iss_lon = iss["lon"] | 0.0f;
    s.iss_alt = iss["alt_km"] | 0;
    s.overhead = iss["overhead"] | false;
  }
  JsonObject ps = doc["iss_pass"];
  if (!ps.isNull()) {
    s.pass_ok = true;
    s.pass_t = ps["t"] | 0UL;
    s.pass_dur = ps["dur_min"] | 0;
    s.pass_el = ps["max_el"] | 0;
    strlcpy(s.from, ps["from"] | "", sizeof s.from);
    strlcpy(s.to, ps["to"] | "", sizeof s.to);
  }
  for (JsonArray pt : doc["iss_track"].as<JsonArray>()) {
    if (s.track_n >= 64) break;
    s.track[s.track_n][0] = (int16_t)lroundf((pt[0] | 0.0f) * 10);
    s.track[s.track_n][1] = (int16_t)lroundf((pt[1] | 0.0f) * 10);
    s.track_n++;
  }
  s.fetched_ms = millis();
  xSemaphoreTake(g_mtx, portMAX_DELAY);
  g_sp_net = s;
  g_sp_new = true;
  xSemaphoreGive(g_mtx);
  Serial.printf("[net] space: %d launches\n", s.n);
}

static void parse_todos(const String &body) {
  JsonDocument doc(&g_psram_alloc);
  if (deserializeJson(doc, body)) return;
  static Todo *tmp = (Todo *)heap_caps_malloc(sizeof(Todo) * MAXT, MALLOC_CAP_SPIRAM);
  int n = 0;
  for (JsonObject t : doc["todos"].as<JsonArray>()) {
    if (n >= MAXT) break;
    strlcpy(tmp[n].id, t["id"] | "", sizeof tmp[n].id);
    strlcpy(tmp[n].text, t["text"] | "", sizeof tmp[n].text);
    tmp[n].done = t["done"] | false;
    n++;
  }
  xSemaphoreTake(g_mtx, portMAX_DELAY);
  memcpy(g_td_net, tmp, sizeof(Todo) * n);
  g_td_net_n = n;
  g_td_new = true;
  xSemaphoreGive(g_mtx);
  Serial.printf("[net] %d todos\n", n);
}

// Routes (origin/destination) come from the Worker (adsbdb), looked up per callsign
// for the selected plane only and cached. The data is crowd-sourced and can be
// stale, so the UI only shows a route when the plane's position fits it.
struct Route {
  char cs[10];
  uint8_t st;  // 1 found, 2 unknown
  char o[4], d[4];
  char oc[24], dc[24];
  char airline[28];
  float olat, olon, dlat, dlon;
};
static Route g_routes[24];
static int g_route_next = 0;
static char g_route_req[10] = "";
static volatile bool g_route_new = false;

static const Route *find_route(const char *cs) {
  for (auto &r : g_routes)
    if (r.st && !strcmp(r.cs, cs)) return &r;
  return nullptr;
}

static void fetch_route(const char *cs) {
  Route r = {};
  strlcpy(r.cs, cs, sizeof r.cs);
  r.st = 2;
  char url[128];
  snprintf(url, sizeof url, "%s/route/%s", WORKER_URL, cs);
  String body;
  int code;
  if (http_req("GET", url, true, body, code)) {
    JsonDocument doc(&g_psram_alloc);
    if (!deserializeJson(doc, body) && (doc["found"] | false)) {
      strlcpy(r.o, doc["o"] | "", sizeof r.o);
      strlcpy(r.d, doc["d"] | "", sizeof r.d);
      strlcpy(r.oc, doc["oc"] | "", sizeof r.oc);
      strlcpy(r.dc, doc["dc"] | "", sizeof r.dc);
      strlcpy(r.airline, doc["airline"] | "", sizeof r.airline);
      r.olat = doc["olat"] | 0.0f;
      r.olon = doc["olon"] | 0.0f;
      r.dlat = doc["dlat"] | 0.0f;
      r.dlon = doc["dlon"] | 0.0f;
      if (r.o[0] && r.d[0]) r.st = 1;
    }
  }
  Serial.printf("[route] %s -> %s\n", cs, r.st == 1 ? (String(r.o) + "-" + r.d).c_str() : "unknown");
  xSemaphoreTake(g_mtx, portMAX_DELAY);
  g_routes[g_route_next] = r;
  g_route_next = (g_route_next + 1) % 24;
  g_route_new = true;
  xSemaphoreGive(g_mtx);
}

static void worker_get(const char *path, void (*parse)(const String &)) {
  char url[128];
  snprintf(url, sizeof url, "%s%s", WORKER_URL, path);
  String body;
  int code;
  if (http_req("GET", url, true, body, code)) parse(body);
  else Serial.printf("[net] %s HTTP %d\n", path, code);
}

// ---------------------------------------------------------------- wireless updates
// tools/ota.sh builds and sends firmware to skytile.local over WiFi (password in
// secrets.h). While an update runs, data fetching pauses and the screen shows progress.
static volatile int g_ota_pct = -1;  // -1 idle, 0..100 updating, -2 failed

static void ota_task(void *) {
  ArduinoOTA.setHostname("skytile");  // also advertises skytile.local via mDNS
  ArduinoOTA.setPassword(OTA_PASS);
  ArduinoOTA.onStart([]() { g_ota_pct = 0; Serial.println("[ota] start"); });
  ArduinoOTA.onProgress([](unsigned int done, unsigned int total) { g_ota_pct = total ? done * 100 / total : 0; });
  ArduinoOTA.onEnd([]() { g_ota_pct = 100; Serial.println("[ota] done, rebooting"); });
  ArduinoOTA.onError([](ota_error_t e) { g_ota_pct = -2; Serial.printf("[ota] error %d\n", (int)e); });
  ArduinoOTA.begin();
  Serial.println("[ota] ready at skytile.local");
  for (;;) {
    ArduinoOTA.handle();  // runs the whole transfer inside this call once an upload starts
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

static void net_task(void *) {
  WiFi.persistent(false);  // no flash writes on (re)connect: they stall the display
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t started = millis();
  bool clock_set = false;
  uint32_t t_pl = 0, t_sh = 0, t_sp = 0, t_td = 0;
  int last_nm = 0;
  float last_e = 0, last_n = 0;
  Mode last_mode = g_mode;
  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      g_net_status = 0;
      if (millis() - started > 25000) {  // join attempt timed out: start over
        Serial.println("[net] WiFi join timed out, retrying");
        WiFi.disconnect();
        vTaskDelay(pdMS_TO_TICKS(500));
        WiFi.begin(WIFI_SSID, WIFI_PASS);
        started = millis();
      }
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    if (started) {
      Serial.printf("[net] WiFi up, IP %s\n", WiFi.localIP().toString().c_str());
      started = 0;
      static bool ota_started = false;
      if (!ota_started) { ota_started = true; xTaskCreatePinnedToCore(ota_task, "ota", 8192, nullptr, 1, nullptr, 0); }
    }
    if (g_ota_pct >= 0) { vTaskDelay(pdMS_TO_TICKS(500)); continue; }  // updating: leave the network to OTA
    if (!clock_set) {  // wall clock for launch countdowns, Pacific time
      configTzTime("PST8PDT,M3.2.0,M11.1.0", "pool.ntp.org", "time.google.com");
      clock_set = true;
    }

    Mode m = g_mode;
    if (m != last_mode) {  // just switched screens: refresh that screen's data now
      if (m == M_PLANES) t_pl = 0;
      if (m == M_SHIPS) t_sh = 0;
      if (m == M_SPACE) t_sp = 0;
      if (m == M_TODO) t_td = 0;
      last_mode = m;
    }
    uint32_t now = millis();

    // Planes: every 10 s on the Planes screen, every minute otherwise.
    float want = g_range * sqrtf((float)g_mw * g_mw + (float)g_mh * g_mh) / 2 / ring_px();  // reach the map corners
    int nm = want <= 30 ? 30 : (want <= 60 ? 60 : 130);
    float pe = g_pan_e, pn = g_pan_n;
    bool moved = m == M_PLANES && fabsf(pe - last_e) + fabsf(pn - last_n) > nm * 0.3f;
    if (!t_pl || now - t_pl >= (m == M_PLANES ? FETCH_MS : 60000) || (m == M_PLANES && nm > last_nm) || moved) {
      t_pl = now;
      last_nm = nm;
      last_e = pe;
      last_n = pn;
      float clat = HOME_LAT + pn / 60.0f, clon = HOME_LON + pe / KLON;
      Serial.printf("[mem] internal free %u, largest %u\n", heap_caps_get_free_size(MALLOC_CAP_INTERNAL), heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
      char url[128];
      snprintf(url, sizeof url, "https://api.adsb.lol/v2/point/%.4f/%.4f/%d", clat, clon, nm);
      String body;
      int code;
      static int fails = 0;  // one rate-limited fetch (HTTP 429) is normal: keep showing the last data
      if (http_req("GET", url, false, body, code)) { fails = 0; parse_planes(body, clat, clon); }
      else { Serial.printf("[net] planes HTTP %d\n", code); if (++fails >= 3) g_net_status = 2; }
    }
    // Ships: every 20 s on the Ships screen, every 2 min otherwise. The background
    // polls keep the Worker's aisstream collector awake, so the screen opens ready.
    if (!t_sh || now - t_sh >= (m == M_SHIPS ? 20000UL : 120000UL)) { t_sh = now; worker_get("/ships", parse_ships); }
    // Space: every minute on its screen (ISS moves), every 10 minutes otherwise.
    if (!t_sp || now - t_sp >= (m == M_SPACE ? 60000UL : 600000UL)) { t_sp = now; worker_get("/space", parse_space); }

    // To-do check-offs from the screen, then a refresh.
    char q[8][10];
    int qn;
    xSemaphoreTake(g_mtx, portMAX_DELAY);
    qn = g_tq_n;
    memcpy(q, g_tq, sizeof q);
    g_tq_n = 0;
    xSemaphoreGive(g_mtx);
    for (int i = 0; i < qn; i++) {
      char url[128];
      snprintf(url, sizeof url, "%s/todos/%s/done", WORKER_URL, q[i]);
      String body;
      int code;
      if (!http_req("POST", url, true, body, code)) Serial.printf("[net] todo toggle HTTP %d\n", code);
    }
    if (qn) t_td = 0;
    // To-dos: every 15 s on the To-do screen, every minute otherwise.
    if (!t_td || now - t_td >= (m == M_TODO ? 15000 : 60000)) { t_td = millis(); worker_get("/todos", parse_todos); }

    char req[10];
    xSemaphoreTake(g_mtx, portMAX_DELAY);
    strlcpy(req, g_route_req, sizeof req);
    g_route_req[0] = 0;
    xSemaphoreGive(g_mtx);
    if (req[0] && !find_route(req)) fetch_route(req);
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

// ---------------------------------------------------------------- UI state
enum Filter { F_ALL, F_INTERESTING, F_FAVS };
static const char *MODE_NAMES[] = {"PLANES", "SHIPS", "SPACE", "TO-DO"};
static const char *FILTER_NAMES[] = {"ALL", "INTERESTING", "FAVORITES"};

static Filter g_filter = F_ALL;
static uint32_t g_last_touch = 0;  // millis of the last touch (idle timer)
static char g_sel[2][12] = {"", ""};  // selected item key: [0] plane hex, [1] ship MMSI
static int g_todo_sel = -1;  // knob cursor on the to-do list; -1 = hidden (no knob used yet)

static int g_vis[MAXP];  // indices into the current map list (planes or ships) that are shown
static int g_vis_n = 0;

static inline bool map_mode() { return g_mode == M_PLANES || g_mode == M_SHIPS; }
static inline int sel_slot() { return g_mode == M_SHIPS ? 1 : 0; }

// widgets: top bar
static lv_obj_t *tabs[4], *tab_lbl[4];
static lv_obj_t *fchip, *chip_filter_lbl, *chip_status_lbl, *chip_dot, *bar_obj, *map_card, *zoom_btn[2], *north_lbl;
// widgets: map screen (Planes + Ships)
static lv_obj_t *scr_map;
static lv_obj_t *map_obj, *geo, *empty_lbl, *ring_lbl[2], *ring_obj[2], *home_obj, *recenter_btn;
static lv_obj_t *mk[MAXP], *mk_img[MAXP], *mk_lbl[MAXP];
static lv_obj_t *c_type;
static lv_obj_t *c_eye, *c_call, *c_desc, *c_tag, *c_route, *c_cities, *c_reg, *c_favbtn, *c_favbtn_lbl;
static lv_obj_t *c_k[4], *c_v[4];  // 2x2 stat grid: key / value labels
// widgets: space
static lv_obj_t *scr_space;
static lv_obj_t *sp_rocket, *sp_mission, *sp_where, *sp_count, *sp_when, *sp_vis, *sp_row[4];
static lv_obj_t *world;
static lv_obj_t *iss_pos, *iss_alt, *iss_tag, *pass_when, *pass_info, *pass_dir, *pass_in;
// widgets: to-do
static lv_obj_t *scr_todo, *todo_list, *todo_head, *todo_empty;
static lv_obj_t *ota_overlay, *ota_lbl, *ota_bar;
static lv_obj_t *idle_ov, *idle_tag, *tag_eye, *tag_name, *tag_line;
static const int DASH_TD = 7;  // to-dos shown on the idle dashboard
static lv_obj_t *dash_time, *dash_date, *dash_td[DASH_TD], *dash_l_count, *dash_l_name, *dash_l_vis;
static bool g_idle = false;

static void set_text(lv_obj_t *l, const char *t) {
  if (strcmp(lv_label_get_text(l), t)) lv_label_set_text(l, t);
}

static lv_obj_t *mk_label(lv_obj_t *parent, const char *txt, const lv_font_t *f, uint32_t col, int x, int y) {
  lv_obj_t *l = lv_label_create(parent);
  lv_label_set_text(l, txt);
  lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
  lv_obj_set_pos(l, x, y);
  return l;
}

static lv_obj_t *mk_dot_label(lv_obj_t *parent, const lv_font_t *f, uint32_t col, int x, int y, int w) {
  lv_obj_t *l = mk_label(parent, "", f, col, x, y);
  lv_obj_set_width(l, w);
  lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
  return l;
}

static lv_obj_t *mk_box(lv_obj_t *parent, int x, int y, int w, int h) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_set_pos(o, x, y);
  lv_obj_set_size(o, w, h);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  return o;
}

static lv_obj_t *mk_btn(lv_obj_t *parent, const char *txt, int x, int y, int w, int h, lv_event_cb_t cb, void *ud,
                        lv_obj_t **lbl_out = nullptr) {
  lv_obj_t *b = mk_box(parent, x, y, w, h);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(b, lv_color_hex(0x0e182a), 0);
  lv_obj_set_style_bg_color(b, lv_color_hex(0x1c2b47), LV_STATE_PRESSED);
  lv_obj_set_style_border_width(b, 1, 0);
  lv_obj_set_style_border_color(b, lv_color_hex(0x2b3c57), 0);
  lv_obj_set_style_radius(b, 8, 0);
  lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
  lv_obj_t *l = lv_label_create(b);
  lv_label_set_text(l, txt);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(0xc9d3e2), 0);
  lv_obj_center(l);
  if (lbl_out) *lbl_out = l;
  return b;
}

static void show(lv_obj_t *o, bool on) {
  if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

// Marker icon (plane or boat image) rotated to its heading, centered in the 36x36 marker.
static const lv_image_dsc_t *g_mk_src[MAXP];

static void set_marker(int k, const lv_image_dsc_t *src, float hdg_deg, uint16_t scale, uint32_t tint, lv_opa_t tint_opa, lv_opa_t opa) {
  lv_obj_t *img = mk_img[k];
  if (g_mk_src[k] != src) {
    g_mk_src[k] = src;
    lv_image_set_src(img, src);
    lv_image_set_pivot(img, src->header.w / 2, src->header.h / 2);
  }
  int rot = ((int)lroundf(hdg_deg * 10) % 3600 + 3600) % 3600;  // 0.1 degree units
  if (lv_image_get_rotation(img) != rot) lv_image_set_rotation(img, rot);
  if (lv_image_get_scale(img) != scale) lv_image_set_scale(img, scale);
  lv_obj_set_style_image_recolor(img, lv_color_hex(tint), 0);
  lv_obj_set_style_image_recolor_opa(img, tint_opa, 0);
  lv_obj_set_style_image_opa(img, opa, 0);
}

// ---------------------------------------------------------------- icon classes
// Planes: from adsb.lol's military flag, the ADS-B emitter category and the ICAO type code.
static bool type_in(const char *t, const char *list) {  // list: " C172 C182 ... "
  if (!t[0]) return false;
  char k[8];
  snprintf(k, sizeof k, " %s ", t);
  return strstr(list, k) != nullptr;
}
static const char *HELI_T = " H60 S70 S76 S92 EC30 EC35 EC45 EC55 EC75 AS50 AS55 AS65 B06 B407 B412 B429 B505 R22 R44 R66 A109 A119 A139 A169 H500 MD52 MD60 EN48 BK17 H125 H130 H135 H145 H160 UH1 UH1Y AH1 AH64 CH47 CH53 H47 H53 H64 MH60 V22 ";
static const char *PROP_T = " C150 C152 C162 C170 C172 C177 C180 C182 C185 C195 C206 C207 C208 C210 P28A P28B P28R P28T PA18 PA20 PA22 PA24 PA28 PA30 PA31 PA32 PA34 PA44 PA46 SR20 SR22 S22T BE33 BE35 BE36 BE55 BE58 BE9L BE20 M20P M20T DA40 DA42 DA62 DV20 C310 C340 C421 TBM7 TBM8 TBM9 PC12 RV7 RV8 RV10 AA5 GLAS COL4 ";
static const char *BIZJET_T = " C25A C25B C25C C25M C501 C510 C525 C550 C560 C56X C650 C680 C68A C700 C750 CL30 CL35 CL60 GLF4 GLF5 GLF6 GL5T GL7T GLEX G150 G200 G280 E35L E545 E550 E50P E55P LJ31 LJ35 LJ40 LJ45 LJ55 LJ60 LJ75 FA10 FA20 FA50 FA6X FA7X FA8X F2TH F900 H25B H25C PC24 HDJT BE40 PRM1 SF50 ";
static const char *MILCARGO_T = " C17 C5M C130 C30J K35R KC10 KC46 KC30 P8 E3TF E3CF C40 C32 C37A C2 E2 A400 IL76 MRTT B52 B1 E6 C12 C26 V22 ";

static const lv_image_dsc_t *plane_icon(const Plane &p) {
  bool heli = !strcmp(p.cat, "A7") || type_in(p.type, HELI_T);
  if (p.mil) {
    if (heli) return &HELI_MIL_IMG;
    if (type_in(p.type, MILCARGO_T) || !strcmp(p.cat, "A3") || !strcmp(p.cat, "A4") || !strcmp(p.cat, "A5")) return &MILCARGO_IMG;
    return &FIGHTER_IMG;
  }
  if (heli) return &HELI_CIV_IMG;
  if (type_in(p.type, BIZJET_T)) return &BIZJET_IMG;
  if (type_in(p.type, PROP_T) || !strcmp(p.cat, "A1")) return &PROP_IMG;
  if (!strcmp(p.cat, "A2")) return &BIZJET_IMG;  // small (15-75k lb): mostly business jets
  return &AIRLINER_IMG;
}

// Ships: from the AIS ship type code (and length when the type is unknown).
// Side views face right; the _L copies face left (used when heading west).
static const lv_image_dsc_t *ship_icon(const Ship &s, bool west) {
  int t = s.type;
  if (t == 36) return west ? &SAIL_L_IMG : &SAIL_R_IMG;
  if (t == 30) return west ? &FISHING_L_IMG : &FISHING_R_IMG;
  if (t == 35) return west ? &NAVY_L_IMG : &NAVY_R_IMG;
  if (t >= 60 && t <= 69) return west ? &CRUISE_L_IMG : &CRUISE_R_IMG;
  if (t >= 70 && t <= 79) return west ? &CARGO_L_IMG : &CARGO_R_IMG;
  if (t >= 80 && t <= 89) return west ? &TANKER_L_IMG : &TANKER_R_IMG;
  if (t == 31 || t == 32 || t == 33 || t == 50 || t == 51 || t == 52 || t == 53 || t == 54 || t == 55 || t == 58)
    return west ? &TUG_L_IMG : &TUG_R_IMG;  // tugs, pilots, rescue, law enforcement, work boats
  if (t == 37 || (t >= 40 && t <= 49)) return west ? &YACHT_L_IMG : &YACHT_R_IMG;
  if (s.len >= 100) return west ? &CARGO_L_IMG : &CARGO_R_IMG;  // unknown type, big: most likely cargo
  return west ? &YACHT_L_IMG : &YACHT_R_IMG;                   // unknown, small: most likely a pleasure boat
}

static bool ship_unnamed(const Ship &s) { return !s.name[0] || !strncmp(s.name, "MMSI ", 5); }

// ---------------------------------------------------------------- map items (planes or ships)
static void item_pos(int i, float &lat, float &lon) {
  if (g_mode == M_SHIPS) { lat = g_sh[i].lat; lon = g_sh[i].lon; }
  else { lat = g_pl[i].lat; lon = g_pl[i].lon; }
}
static const char *item_key(int i) { return g_mode == M_SHIPS ? g_sh[i].key : g_pl[i].hex; }

static int selected_pos() {
  for (int k = 0; k < g_vis_n; k++)
    if (!strcmp(item_key(g_vis[k]), g_sel[sel_slot()])) return k;
  return -1;
}

static void rebuild_visible() {
  g_vis_n = 0;
  int n = g_mode == M_SHIPS ? g_sh_n : g_pl_n;
  for (int i = 0; i < n && g_vis_n < MAXP; i++) {
    float lat, lon, x, y;
    item_pos(i, lat, lon);
    project(lat, lon, x, y);
    if (x < -10 || x > g_mw + 10 || y < -10 || y > g_mh + 10) continue;  // off the map
    if (g_mode == M_PLANES) {
      const Plane &p = g_pl[i];
      if (p.alt < 0) continue;  // on the ground: hidden (too noisy at airports)
      if (g_filter == F_INTERESTING && !is_interesting(p)) continue;
      if (g_filter == F_FAVS && !is_fav(p.hex)) continue;
    } else if (g_idle) {
      const Ship &sh = g_sh[i];  // asleep: moving ships only (moored harbors are a pile-up)
      if (sh.sog < 0.5f && strcmp(sh.key, g_sel[1])) continue;
    }
    g_vis[g_vis_n++] = i;
  }
  if (selected_pos() < 0) {  // default: nearest (lists are sorted), but for ships prefer a moving, named one
    int pick = g_vis_n ? g_vis[0] : -1;
    if (g_mode == M_SHIPS)
      for (int k = 0; k < g_vis_n; k++) {
        const Ship &sh = g_sh[g_vis[k]];
        if (sh.sog >= 0.5f && !ship_unnamed(sh)) { pick = g_vis[k]; break; }
      }
    if (pick >= 0) strlcpy(g_sel[sel_slot()], item_key(pick), sizeof g_sel[0]);
    else g_sel[sel_slot()][0] = 0;
  }
}

// True if the plane sits near one of the airports or near the great-circle
// path between them (crowd-sourced routes are sometimes for another day's flight).
static bool route_fits(const Route &r, const Plane &p) {
  const float R = 3440.065f;  // earth radius, nm
  auto rad = [](float d) { return d * DEG_TO_RAD; };
  auto dist = [&](float la1, float lo1, float la2, float lo2) {
    float a = sinf(rad(la2 - la1) / 2), b = sinf(rad(lo2 - lo1) / 2);
    return 2 * asinf(sqrtf(a * a + cosf(rad(la1)) * cosf(rad(la2)) * b * b));  // radians
  };
  auto brg = [&](float la1, float lo1, float la2, float lo2) {
    float y = sinf(rad(lo2 - lo1)) * cosf(rad(la2));
    float x = cosf(rad(la1)) * sinf(rad(la2)) - sinf(rad(la1)) * cosf(rad(la2)) * cosf(rad(lo2 - lo1));
    return atan2f(y, x);
  };
  float d13 = dist(r.olat, r.olon, p.lat, p.lon), d23 = dist(r.dlat, r.dlon, p.lat, p.lon);
  if (d13 * R < 40 || d23 * R < 40) return true;
  float d12 = dist(r.olat, r.olon, r.dlat, r.dlon);
  float xt = asinf(sinf(d13) * sinf(brg(r.olat, r.olon, p.lat, p.lon) - brg(r.olat, r.olon, r.dlat, r.dlon)));
  float at = acosf(cosf(d13) / cosf(xt));
  return fabsf(xt) * R < 100 && at <= d12 + 40 / R;
}

static void set_grid(const char *const k[4]) {
  for (int i = 0; i < 4; i++) set_text(c_k[i], k[i]);
}

static void refresh_card_empty(const char *eye, const char *title, const char *detail) {
  set_text(c_eye, eye);
  set_text(c_call, "--");
  set_text(c_desc, title);
  set_text(c_type, detail);
  set_text(c_tag, "");
  set_text(c_route, "");
  set_text(c_cities, "");
  for (int i = 0; i < 4; i++) set_text(c_v[i], "--");
  set_text(c_reg, "");
}

static void refresh_card_plane() {
  static const char *const K[4] = {"ALTITUDE", "SPEED", "HEADING", "DISTANCE"};
  set_grid(K);
  show(c_favbtn, true);
  lv_obj_set_style_text_font(c_call, &lv_font_montserrat_40, 0);
  int pos = selected_pos();
  if (pos < 0) {
    refresh_card_empty("NO AIRCRAFT", g_filter == F_FAVS ? "No favorites here" : "Nothing in range",
                       g_filter == F_FAVS ? "Hold select (or SAVE) to add one" : "Zoom out or wait a moment");
    return;
  }
  const Plane *p = &g_pl[g_vis[pos]];
  char b[64];
  snprintf(b, sizeof b, "PLANE %d OF %d", pos + 1, g_vis_n);
  set_text(c_eye, b);
  set_text(c_call, p->flight);
  const char *type = p->desc[0] ? p->desc : (p->type[0] ? p->type : "Unknown type");
  bool has_cs = strcmp(p->flight, p->reg) && strcmp(p->flight, p->hex);  // real callsign, not a tail number
  xSemaphoreTake(g_mtx, portMAX_DELAY);
  const Route *rt = find_route(p->flight);
  Route r = rt ? *rt : Route{};
  if (!rt && has_cs) strlcpy(g_route_req, p->flight, sizeof g_route_req);
  xSemaphoreGive(g_mtx);
  // airline big, aircraft type small; without an airline the type moves up
  if (r.st == 1 && r.airline[0]) { set_text(c_desc, r.airline); set_text(c_type, type); }
  else { set_text(c_desc, type); set_text(c_type, ""); }
  if (!rt && has_cs) {
    set_text(c_route, "...");
    set_text(c_cities, "");
  } else if (r.st == 1 && route_fits(r, *p)) {
    snprintf(b, sizeof b, "%s  " LV_SYMBOL_RIGHT "  %s", r.o, r.d);
    set_text(c_route, b);
    snprintf(b, sizeof b, "%s to %s", r.oc, r.dc);
    set_text(c_cities, b);
  } else {
    set_text(c_route, "Route unknown");
    set_text(c_cities, "");
  }
  const char *tag = p->mil ? "MILITARY" : (is_heavy(p->type) ? "HEAVY" : "");
  if (is_fav(p->hex)) snprintf(b, sizeof b, tag[0] ? "FAVORITE  +  %s" : "FAVORITE", tag);
  else snprintf(b, sizeof b, "%s", tag);
  set_text(c_tag, b);
  snprintf(b, sizeof b, "%d ft", p->alt);
  set_text(c_v[0], b);
  snprintf(b, sizeof b, "%d kt", (int)p->gs);
  set_text(c_v[1], b);
  snprintf(b, sizeof b, "%d deg", (int)p->track);
  set_text(c_v[2], b);
  snprintf(b, sizeof b, "%.1f nm", p->dist_nm);
  set_text(c_v[3], b);
  snprintf(b, sizeof b, "%s  %s", p->reg, p->type);
  set_text(c_reg, b);
  set_text(c_favbtn_lbl, is_fav(p->hex) ? "UNSAVE" : "SAVE");
}

static void refresh_card_ship() {
  static const char *const K[4] = {"SPEED", "COURSE", "DISTANCE", "LENGTH"};
  set_grid(K);
  show(c_favbtn, false);
  int pos = selected_pos();
  if (pos < 0) {
    lv_obj_set_style_text_font(c_call, &lv_font_montserrat_40, 0);
    refresh_card_empty("NO SHIPS", g_sh_loaded ? "No ships here" : "Looking for ships",
                       g_sh_loaded ? "Try zooming out" : "They appear within a minute");
    return;
  }
  const Ship *s = &g_sh[g_vis[pos]];
  char b[64];
  snprintf(b, sizeof b, "SHIP %d OF %d", pos + 1, g_vis_n);
  set_text(c_eye, b);
  const char *nm = ship_unnamed(*s) ? "Unknown vessel" : s->name;
  lv_obj_set_style_text_font(c_call, strlen(nm) <= 9 ? &lv_font_montserrat_40 : &lv_font_montserrat_24, 0);
  set_text(c_call, nm);
  set_text(c_desc, s->kind);
  if (s->len > 0) snprintf(b, sizeof b, "%d m long", s->len);
  else b[0] = 0;
  set_text(c_type, b);
  set_text(c_tag, s->sog < 0.5f ? "STOPPED" : "");
  if (s->dest[0]) snprintf(b, sizeof b, LV_SYMBOL_RIGHT "  %s", s->dest);
  else snprintf(b, sizeof b, "No destination");
  set_text(c_route, b);
  if (s->age_s < 90) snprintf(b, sizeof b, "Position just now");
  else snprintf(b, sizeof b, "Position %d min ago", s->age_s / 60);
  set_text(c_cities, b);
  snprintf(b, sizeof b, "%.1f kn", s->sog);
  set_text(c_v[0], b);
  snprintf(b, sizeof b, "%d deg", (int)s->cog);
  set_text(c_v[1], b);
  snprintf(b, sizeof b, "%.1f nm", s->dist_nm);
  set_text(c_v[2], b);
  if (s->len > 0) snprintf(b, sizeof b, "%d m", s->len);
  else snprintf(b, sizeof b, "--");
  set_text(c_v[3], b);
  snprintf(b, sizeof b, "MMSI %s", s->key);
  set_text(c_reg, b);
}

static void refresh_card() {
  if (g_mode == M_SHIPS) refresh_card_ship();
  else refresh_card_plane();
}

// ---------------------------------------------------------------- base map
// Approximate positions. "major" ones stay visible when zoomed far out.
struct Airport { const char *code; float lat, lon; bool major; };
static const Airport AIRPORTS[] = {
    {"SAN", 32.7336f, -117.1897f, true},  {"MYF", 32.8157f, -117.1396f, false},
    {"CRQ", 33.1283f, -117.2803f, false}, {"NKX", 32.8684f, -117.1428f, true},
    {"NZY", 32.6992f, -117.2153f, false}, {"SEE", 32.8262f, -116.9724f, false},
    {"SDM", 32.5723f, -116.9801f, false}, {"TIJ", 32.5411f, -116.9700f, true},
    {"OCN", 33.2196f, -117.3494f, false}, {"NFG", 33.3011f, -117.3550f, false},
    {"LAX", 33.9425f, -118.4081f, true},  {"SNA", 33.6757f, -117.8682f, true},
    {"LGB", 33.8177f, -118.1516f, true},  {"ONT", 34.0560f, -117.6012f, true},
    {"PSP", 33.8297f, -116.5067f, true},  {"IPL", 32.8342f, -115.5786f, false},
    {"MXL", 32.6306f, -115.2416f, true},
};

// Draws coastline, border and airports straight into the map's layer: one
// object instead of dozens, and it only repaints when the view changes.
static void draw_geo(lv_event_t *e) {
  lv_layer_t *layer = lv_event_get_layer(e);
  lv_area_t a;
  lv_obj_get_coords(geo, &a);
  lv_draw_line_dsc_t ln;
  lv_draw_line_dsc_init(&ln);
  ln.round_start = ln.round_end = 1;
  for (int i = 0; i < MAP_LINE_COUNT; i++) {
    const MapLine &m = MAP_LINES[i];
    ln.color = lv_color_hex(m.border ? 0x5a3f57 : 0x3a5070);  // soft slate coast, muted border
    ln.width = m.border ? 1 : 2;
    ln.dash_width = m.border ? 6 : 0;
    ln.dash_gap = m.border ? 5 : 0;
    float px0, py0;
    project(MAP_PTS[m.start][0] / 1e4f, MAP_PTS[m.start][1] / 1e4f, px0, py0);
    int step = g_lite_map ? 4 : 1;
    for (int k = step; k < m.count + step - 1; k += step) {
      if (k >= m.count) k = m.count - 1;  // always end on the last point
      float px1, py1;
      project(MAP_PTS[m.start + k][0] / 1e4f, MAP_PTS[m.start + k][1] / 1e4f, px1, py1);
      bool off = (px0 < -40 && px1 < -40) || (px0 > g_mw + 40 && px1 > g_mw + 40) ||
                 (py0 < -40 && py1 < -40) || (py0 > g_mh + 40 && py1 > g_mh + 40);
      if (!off) {
        ln.p1.x = a.x1 + px0; ln.p1.y = a.y1 + py0;
        ln.p2.x = a.x1 + px1; ln.p2.y = a.y1 + py1;
        lv_draw_line(layer, &ln);
      }
      px0 = px1; py0 = py1;
    }
  }
  if (g_mode != M_PLANES) return;  // airports only matter for planes
  // airports: control tower icon (base on the airport) plus the code
  lv_draw_image_dsc_t ti;
  lv_draw_image_dsc_init(&ti);
  ti.src = &TOWER_IMG;
  const int TW = TOWER_IMG.header.w, TH = TOWER_IMG.header.h;
  lv_draw_label_dsc_t lb;
  lv_draw_label_dsc_init(&lb);
  lb.font = &lv_font_montserrat_12;
  lb.color = lv_color_hex(C_SPACE);
  for (auto &ap : AIRPORTS) {
    if (!ap.major && g_range > 50) continue;
    float x, y;
    project(ap.lat, ap.lon, x, y);
    if (x < 0 || x > g_mw || y < 0 || y > g_mh) continue;
    int cx = a.x1 + (int)x, cy = a.y1 + (int)y;
    lv_area_t ia = {cx - TW / 2, cy - TH + 4, cx - TW / 2 + TW - 1, cy + 3};
    lv_draw_image(layer, &ti, &ia);
    lb.text = ap.code;
    lv_area_t t = {cx + TW / 2 + 3, cy - 10, cx + TW / 2 + 43, cy + 5};
    lv_draw_label(layer, &lb, &t);
  }
}

static void refresh_map() {
  static float geo_range = -1, geo_e = 1e9, geo_n = 1e9;
  static int geo_mode = -1, geo_w = -1;
  if (geo_range != g_range || geo_e != g_pan_e || geo_n != g_pan_n || geo_mode != g_mode || geo_w != g_mw) {
    geo_w = g_mw;
    geo_range = g_range;
    geo_e = g_pan_e;
    geo_n = g_pan_n;
    geo_mode = g_mode;
    lv_obj_invalidate(geo);
    float hx, hy;  // home position on screen: rings are distance from home
    project(HOME_LAT, HOME_LON, hx, hy);
    for (int r = 0; r < 2; r++) {
      int rad = ring_px() * (r + 1) / 2;
      lv_obj_set_size(ring_obj[r], rad * 2, rad * 2);
      lv_obj_set_pos(ring_obj[r], (int)hx - rad, (int)hy - rad);
      lv_obj_set_pos(ring_lbl[r], (int)(hx + rad * 0.707f) + 4, (int)(hy + rad * 0.707f) - 4);
    }
    lv_obj_set_pos(home_obj, (int)hx - 4, (int)hy - 4);
    show(recenter_btn, (g_pan_e != 0 || g_pan_n != 0) && g_mw == MAP_W);
    char b[16];
    for (int r = 0; r < 2; r++) {
      float nm = g_range * (r + 1) / 2;
      snprintf(b, sizeof b, nm < 10 ? "%.1f nm" : "%.0f nm", nm);
      set_text(ring_lbl[r], b);
    }
  }
  bool ships = g_mode == M_SHIPS;
  for (int i = g_vis_n; i < MAXP; i++) {
    lv_obj_add_flag(mk[i], LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(mk_lbl[i], LV_OBJ_FLAG_HIDDEN);
  }
  static int16_t mx[MAXP], my[MAXP];  // icon centers this frame (for label placement)
  int sel_k = -1;
  for (int k = 0; k < g_vis_n; k++) {
    int i = g_vis[k];
    float lat, lon, px, py;
    item_pos(i, lat, lon);
    project(lat, lon, px, py);
    mx[k] = (int16_t)px;
    my[k] = (int16_t)py;
    bool on = !strcmp(item_key(i), g_sel[sel_slot()]);
    if (on) sel_k = k;
    lv_obj_t *m = mk[k];
    lv_obj_set_pos(m, (int)px - 18, (int)py - 18);
    lv_obj_remove_flag(m, LV_OBJ_FLAG_HIDDEN);
    const char *label;
    if (ships) {
      const Ship &s = g_sh[i];
      bool stopped = s.sog < 0.5f;
      // Side-view boat icon by ship type: stays upright, faces left when heading west.
      // Moored boats are smaller and faded so busy harbors stay readable.
      float dir = s.hdg < 360 ? s.hdg : s.cog;
      bool west = !stopped && dir > 180 && dir < 360;
      set_marker(k, ship_icon(s, west), 0, stopped ? 180 : 256, C_AMBER, on ? LV_OPA_70 : LV_OPA_TRANSP,
                 on || !stopped ? LV_OPA_COVER : LV_OPA_60);
      if (ship_unnamed(s)) label = on ? "Unknown" : "";
      else label = (on || !stopped) ? s.name : "";  // moored boats stay unlabeled
    } else {
      const Plane &p = g_pl[i];
      bool fav = is_fav(p.hex);
      set_marker(k, plane_icon(p), p.track, is_heavy(p.type) ? 300 : 256, on ? C_AMBER : 0xf2d04b, on ? LV_OPA_70 : (fav ? LV_OPA_50 : LV_OPA_TRANSP), LV_OPA_COVER);
      label = p.flight;
    }
    set_text(mk_lbl[k], label);
    lv_obj_set_pos(mk_lbl[k], (int)px + 16, (int)py - 8);
    lv_obj_set_style_text_color(mk_lbl[k], lv_color_hex(on ? C_BG : C_MUTED), 0);
    lv_obj_set_style_bg_opa(mk_lbl[k], on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(mk_lbl[k], lv_color_hex(C_AMBER), 0);
    if (on) { lv_obj_move_foreground(m); lv_obj_move_foreground(mk_lbl[k]); }
  }
  // Label placement: selected first, then nearest first (list order). A label only
  // shows if it overlaps no label already placed and no other icon, so busy
  // harbors and airports show a few names instead of a pile. Tap for the rest.
  static lv_area_t placed[MAXP];
  int np = 0;
  for (int n = -1; n < g_vis_n; n++) {
    int k = n < 0 ? sel_k : n;
    if (k < 0 || (n >= 0 && k == sel_k)) continue;
    const char *txt = lv_label_get_text(mk_lbl[k]);
    bool ok = txt[0] != 0;
    lv_area_t r;
    if (ok) {
      int w = lv_text_get_width(txt, strlen(txt), &lv_font_montserrat_12, 0) + 8;
      r = {mx[k] + 16, my[k] - 8, mx[k] + 16 + w, my[k] + 8};
      if (k != sel_k) {
        for (int j = 0; ok && j < np; j++) ok = !_lv_area_is_on(&r, &placed[j]);
        for (int j = 0; ok && j < g_vis_n; j++) {
          if (j == k) continue;
          lv_area_t icon = {mx[j] - 10, my[j] - 10, mx[j] + 10, my[j] + 10};
          ok = !_lv_area_is_on(&r, &icon);
        }
      }
    }
    if (ok) placed[np++] = r;
    show(mk_lbl[k], ok);
  }
  set_text(empty_lbl, ships ? (g_sh_loaded ? "No ships here" : "Looking for ships...") : "No aircraft to show");
  show(empty_lbl, g_vis_n == 0);
}

// ---------------------------------------------------------------- space screen
static void fmt_local(char *b, size_t n, uint32_t t, const char *fmt) {
  time_t tt = t;
  struct tm tm;
  localtime_r(&tt, &tm);
  strftime(b, n, fmt, &tm);
  // drop a leading zero on hours/days: "04:00 PM" -> "4:00 PM", "Oct 05" -> "Oct 5"
  for (char *c = b; *c; c++)
    if (*c == '0' && (c == b || c[-1] == ' ') && c[1] >= '0' && c[1] <= '9') memmove(c, c + 1, strlen(c));
}
static bool clock_ok() { return time(nullptr) > 1700000000; }

static void fmt_countdown(char *b, size_t n, long dt) {
  bool neg = dt < 0;
  long a = neg ? -dt : dt;
  long d = a / 86400, h = a / 3600 % 24, m = a / 60 % 60, s = a % 60;
  if (d) snprintf(b, n, "T%c %ldd %02ld:%02ld:%02ld", neg ? '+' : '-', d, h, m, s);
  else snprintf(b, n, "T%c %02ld:%02ld:%02ld", neg ? '+' : '-', h, m, s);
}

// Mini world map (equirectangular) with the ISS ground track, the ISS, and home.
static const int WM_W = 260, WM_H = 130;
static void draw_world(lv_event_t *e) {
  lv_layer_t *layer = lv_event_get_layer(e);
  lv_area_t a;
  lv_obj_get_coords(world, &a);
  auto px = [&](float lon, float lat, float &x, float &y) {
    x = a.x1 + (lon + 180) * WM_W / 360.0f;
    y = a.y1 + (90 - lat) * WM_H / 180.0f;
  };
  // one segment between two (lon, lat) points; skipped where it wraps around the date line
  auto seg = [&](lv_draw_line_dsc_t &ln, float lon0, float lat0, float lon1, float lat1) {
    float x0, y0, x1, y1;
    px(lon0, lat0, x0, y0);
    px(lon1, lat1, x1, y1);
    if (fabsf(x1 - x0) >= WM_W / 2) return;
    ln.p1.x = x0; ln.p1.y = y0;
    ln.p2.x = x1; ln.p2.y = y1;
    lv_draw_line(layer, &ln);
  };
  lv_draw_line_dsc_t ln;
  lv_draw_line_dsc_init(&ln);
  ln.color = lv_color_hex(0x3a5070);
  ln.width = 1;
  for (int i = 0; i < WORLD_LINE_COUNT; i++) {
    const WorldLine &w = WORLD_LINES[i];
    for (int k = 1; k < w.count; k++)
      seg(ln, WORLD_PTS[w.start + k - 1][0] / 10.0f, WORLD_PTS[w.start + k - 1][1] / 10.0f,
          WORLD_PTS[w.start + k][0] / 10.0f, WORLD_PTS[w.start + k][1] / 10.0f);
  }
  // ground track: dashed
  ln.color = lv_color_hex(C_SPACE);
  ln.width = 2;
  ln.dash_width = 4;
  ln.dash_gap = 3;
  for (int k = 1; k < g_sp.track_n; k++)
    seg(ln, g_sp.track[k - 1][0] / 10.0f, g_sp.track[k - 1][1] / 10.0f, g_sp.track[k][0] / 10.0f, g_sp.track[k][1] / 10.0f);
  lv_draw_rect_dsc_t dot;
  lv_draw_rect_dsc_init(&dot);
  dot.radius = LV_RADIUS_CIRCLE;
  float x, y;
  px(HOME_LON, HOME_LAT, x, y);  // home
  dot.bg_color = lv_color_hex(C_TEXT);
  lv_area_t h = {(int)x - 2, (int)y - 2, (int)x + 2, (int)y + 2};
  lv_draw_rect(layer, &dot, &h);
  if (g_sp.iss_ok) {  // ISS: bright dot with a soft halo
    px(g_sp.iss_lon, g_sp.iss_lat, x, y);
    dot.bg_color = lv_color_hex(C_SPACE);
    dot.bg_opa = LV_OPA_30;
    lv_area_t halo = {(int)x - 8, (int)y - 8, (int)x + 8, (int)y + 8};
    lv_draw_rect(layer, &dot, &halo);
    dot.bg_opa = LV_OPA_COVER;
    lv_area_t d = {(int)x - 4, (int)y - 4, (int)x + 4, (int)y + 4};
    lv_draw_rect(layer, &dot, &d);
  }
}

// Called every second while the Space screen is open (countdowns tick locally).
static void refresh_space() {
  char b[96];
  long now = time(nullptr);
  // the next launch that hasn't happened (or launched within the last hour)
  int first = 0;
  while (first < g_sp.n && clock_ok() && (long)g_sp.l[first].t < now - 3600) first++;
  if (first >= g_sp.n) {
    set_text(sp_rocket, g_sp.n ? "--" : "Loading...");
    set_text(sp_mission, "");
    set_text(sp_where, "");
    set_text(sp_count, "");
    set_text(sp_when, "");
    set_text(sp_vis, "");
  } else {
    const Launch &l = g_sp.l[first];
    set_text(sp_rocket, l.rocket[0] ? l.rocket : "Launch");
    set_text(sp_mission, l.name);
    if (l.dist_mi > 0) snprintf(b, sizeof b, "%s, %d mi away", l.loc, l.dist_mi);
    else snprintf(b, sizeof b, "%s", l.loc);
    set_text(sp_where, b);
    if (clock_ok()) {
      fmt_countdown(b, sizeof b, (long)l.t - now);
      set_text(sp_count, b);
      fmt_local(b, sizeof b, l.t, "%a %b %d, %I:%M %p");
      set_text(sp_when, b);
    } else {
      set_text(sp_count, "T- --:--:--");
      set_text(sp_when, "Setting clock...");
    }
    set_text(sp_vis, l.visible ? "May be visible from home: look northwest" : "");
  }
  for (int i = 0; i < 4; i++) {
    int k = first + 1 + i;
    if (k < g_sp.n) {
      char d[16];
      fmt_local(d, sizeof d, g_sp.l[k].t, "%b %d");
      snprintf(b, sizeof b, "%s   %s   %s", d, g_sp.l[k].rocket, g_sp.l[k].name);
      set_text(sp_row[i], b);
      lv_obj_set_style_text_color(sp_row[i], lv_color_hex(g_sp.l[k].visible ? C_SPACE : C_TEXT), 0);
    } else {
      set_text(sp_row[i], "");
    }
  }
  if (g_sp.iss_ok) {
    snprintf(b, sizeof b, "%.1f %c, %.1f %c", fabsf(g_sp.iss_lat), g_sp.iss_lat >= 0 ? 'N' : 'S', fabsf(g_sp.iss_lon), g_sp.iss_lon >= 0 ? 'E' : 'W');
    set_text(iss_pos, b);
    snprintf(b, sizeof b, "%d km up", g_sp.iss_alt);
    set_text(iss_alt, b);
    set_text(iss_tag, g_sp.overhead ? "OVERHEAD NOW" : "");
  } else {
    set_text(iss_pos, "--");
    set_text(iss_alt, "");
    set_text(iss_tag, "");
  }
  if (g_sp.pass_ok && clock_ok()) {
    fmt_local(b, sizeof b, g_sp.pass_t, "%a %I:%M %p");
    set_text(pass_when, b);
    snprintf(b, sizeof b, "%d min, up to %d deg high", g_sp.pass_dur, g_sp.pass_el);
    set_text(pass_info, b);
    snprintf(b, sizeof b, "From %s to %s", g_sp.from, g_sp.to);
    set_text(pass_dir, b);
    long dt = (long)g_sp.pass_t - now;
    if (dt > 0) snprintf(b, sizeof b, "in %ldh %02ldm", dt / 3600, dt / 60 % 60);
    else snprintf(b, sizeof b, "happening now");
    set_text(pass_in, b);
  } else {
    set_text(pass_when, g_sp.pass_ok ? "--" : "No pass in 24 h");
    set_text(pass_info, "");
    set_text(pass_dir, "");
    set_text(pass_in, "");
  }
}

// ---------------------------------------------------------------- to-do screen
static void on_todo_row(lv_event_t *e);

static void style_todo_row(lv_obj_t *row, int i) {
  lv_obj_set_style_bg_opa(row, i == g_todo_sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
}

static void refresh_todo() {
  int left = 0;
  for (int i = 0; i < g_td_n; i++) left += !g_td[i].done;
  char b[48];
  snprintf(b, sizeof b, g_td_n ? "%d LEFT OF %d" : "", left, g_td_n);
  set_text(todo_head, b);
  if (g_todo_sel >= g_td_n) g_todo_sel = g_td_n ? g_td_n - 1 : -1;
  lv_obj_clean(todo_list);
  for (int i = 0; i < g_td_n; i++) {
    lv_obj_t *row = lv_obj_create(todo_list);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 740, 54);
    lv_obj_set_style_radius(row, 8, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x1c2b47), 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(row, on_todo_row, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    style_todo_row(row, i);
    lv_obj_t *box = mk_box(row, 14, 15, 24, 24);
    lv_obj_set_style_radius(box, 5, 0);
    lv_obj_set_style_border_width(box, 2, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(g_td[i].done ? C_TODO : 0x5f6e86), 0);
    lv_obj_set_style_bg_opa(box, g_td[i].done ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(box, lv_color_hex(C_TODO), 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
    if (g_td[i].done) {
      lv_obj_t *ck = mk_label(box, LV_SYMBOL_OK, &lv_font_montserrat_16, C_BG, 0, 0);
      lv_obj_center(ck);
    }
    lv_obj_t *t = mk_label(row, g_td[i].text, &lv_font_montserrat_24, g_td[i].done ? C_DIM : C_TEXT, 54, 13);
    lv_obj_set_width(t, 670);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    if (g_td[i].done) lv_obj_set_style_text_decor(t, LV_TEXT_DECOR_STRIKETHROUGH, 0);
  }
  set_text(todo_empty, g_td_loaded ? "Nothing to do. Ask Claude to add something." : "Loading...");
  show(todo_empty, g_td_n == 0);
}

// Open items first, checked-off ones after (stable: keeps each group's order).
static void sort_todos() {
  for (int i = 1; i < g_td_n; i++) {
    Todo k = g_td[i];
    int j = i - 1;
    while (j >= 0 && g_td[j].done && !k.done) { g_td[j + 1] = g_td[j]; j--; }
    g_td[j + 1] = k;
  }
}

static void toggle_todo(int i) {
  if (i < 0 || i >= g_td_n) return;
  g_td[i].done = !g_td[i].done;  // update the screen now; the Worker catches up
  xSemaphoreTake(g_mtx, portMAX_DELAY);
  if (g_tq_n < 8) strlcpy(g_tq[g_tq_n++], g_td[i].id, sizeof g_tq[0]);
  xSemaphoreGive(g_mtx);
  sort_todos();
  refresh_todo();
}

static void on_todo_row(lv_event_t *e) {
  int i = (int)(intptr_t)lv_event_get_user_data(e);
  toggle_todo(i);
}

static void todo_step(int d) {
  if (!g_td_n) return;
  g_todo_sel = g_todo_sel < 0 ? 0 : (g_todo_sel + d + g_td_n) % g_td_n;
  for (uint32_t k = 0; k < lv_obj_get_child_count(todo_list); k++) style_todo_row(lv_obj_get_child(todo_list, k), k);
  lv_obj_scroll_to_view(lv_obj_get_child(todo_list, g_todo_sel), LV_ANIM_ON);
}

// ---------------------------------------------------------------- idle screen + backlight
// After 30 s without a touch the tile shows a calm full-screen view of what you were
// looking at (per the simulator spec); any touch wakes it. The backlight dims at night
// and further when idle. Backlight levels go to the helper MCU at I2C 0x30: 0 = brightest, 245 = off.
void sendI2CCommand(uint8_t command);  // main.cpp
static const uint32_t IDLE_MS = 30000;
static const int NIGHT_FROM = 22, NIGHT_TO = 7;  // local hours

static bool is_night() {
  if (!clock_ok()) return false;
  time_t now = time(nullptr);
  struct tm tm;
  localtime_r(&now, &tm);
  return tm.tm_hour >= NIGHT_FROM || tm.tm_hour < NIGHT_TO;
}

static void apply_backlight() {
  static int last = -1;
  bool night = is_night();
  int level = night ? (g_idle ? 215 : 120) : (g_idle ? 40 : 0);
  if (level != last) {
    last = level;
    sendI2CCommand((uint8_t)level);
  }
}

static void refresh_all();
static void set_mode(Mode m);

// Idle dashboard: live map on the left (full height), and a column on the right with
// the time, open to-dos and the next launch countdown.
static const int DASH_MAP_W = 520;

static void set_map_full(bool full) {
  g_mw = full ? DASH_MAP_W : MAP_W;
  g_mh = full ? 480 : MAP_H;
  lv_obj_set_pos(scr_map, 0, full ? 0 : BAR_H);
  lv_obj_set_size(scr_map, 800, g_mh);
  lv_obj_set_size(map_obj, g_mw, g_mh);
  lv_obj_set_size(geo, g_mw, g_mh);
  lv_obj_set_x(north_lbl, g_mw / 2 - 4);
  lv_obj_set_width(empty_lbl, g_mw);
  show(bar_obj, !full);
  show(map_card, !full);
  for (auto b : zoom_btn) show(b, !full);
  refresh_all();
}

static void refresh_idle_tag() {
  int pos = selected_pos();
  lv_obj_set_style_text_color(tag_eye, lv_color_hex(MODE_ACCENT[g_mode]), 0);
  char b[96];
  if (pos < 0) {
    set_text(tag_eye, MODE_NAMES[g_mode]);
    snprintf(b, sizeof b, "%d in view", g_vis_n);
    set_text(tag_name, b);
    set_text(tag_line, "");
  } else if (g_mode == M_PLANES) {
    const Plane &p = g_pl[g_vis[pos]];
    set_text(tag_eye, "FOLLOWING");
    set_text(tag_name, p.flight);
    xSemaphoreTake(g_mtx, portMAX_DELAY);
    const Route *rt = find_route(p.flight);
    Route r = rt ? *rt : Route{};
    xSemaphoreGive(g_mtx);
    if (r.st == 1 && route_fits(r, p)) snprintf(b, sizeof b, "%s  " LV_SYMBOL_RIGHT "  %s", r.oc[0] ? r.oc : r.o, r.dc[0] ? r.dc : r.d);
    else snprintf(b, sizeof b, "%s", p.desc[0] ? p.desc : p.type);
    set_text(tag_line, b);
  } else {
    const Ship &sh = g_sh[g_vis[pos]];
    set_text(tag_eye, "FOLLOWING");
    set_text(tag_name, ship_unnamed(sh) ? "Unknown vessel" : sh.name);
    if (sh.dest[0]) snprintf(b, sizeof b, "%s, bound for %s", sh.kind, sh.dest);
    else snprintf(b, sizeof b, "%s", sh.kind);
    set_text(tag_line, b);
  }
}

static void refresh_idle() {
  char b[96];
  refresh_idle_tag();
  // time + date
  if (clock_ok()) {
    fmt_local(b, sizeof b, time(nullptr), "%I:%M %p");
    set_text(dash_time, b);
    fmt_local(b, sizeof b, time(nullptr), "%A, %b %d");
    set_text(dash_date, b);
  } else {
    set_text(dash_time, "--:--");
    set_text(dash_date, "");
  }
  // to-dos: count + first open items
  int left = 0;
  for (int i = 0; i < g_td_n; i++) left += !g_td[i].done;
  int shown = 0;
  for (int i = 0; i < g_td_n && shown < DASH_TD; i++) {
    if (g_td[i].done) continue;
    set_text(dash_td[shown++], g_td[i].text);
  }
  if (!left) set_text(dash_td[shown++], g_td_loaded ? "All done" : "...");
  while (shown < DASH_TD) set_text(dash_td[shown++], "");
  // next launch
  long now = time(nullptr);
  int first = 0;
  while (first < g_sp.n && clock_ok() && (long)g_sp.l[first].t < now - 3600) first++;
  if (first < g_sp.n && clock_ok()) {
    const Launch &l = g_sp.l[first];
    fmt_countdown(b, sizeof b, (long)l.t - now);
    set_text(dash_l_count, b);
    snprintf(b, sizeof b, "%s, %s", l.rocket, l.name);
    set_text(dash_l_name, b);
    set_text(dash_l_vis, l.visible ? "Visible from home, look NW" : l.loc);
  } else {
    set_text(dash_l_count, "--");
    set_text(dash_l_name, "");
    set_text(dash_l_vis, "");
  }
}

static Mode g_mode_before_idle = M_PLANES;

static void go_idle() {
  g_idle = true;
  g_mode_before_idle = g_mode;
  if (!map_mode()) g_mode = M_PLANES;  // the dashboard map shows planes unless you were on Ships
  if (g_mode == M_SHIPS) {  // following a moored or unnamed boat is dull: let the default pick a moving one
    for (int i = 0; i < g_sh_n; i++)
      if (!strcmp(g_sh[i].key, g_sel[1]) && (g_sh[i].sog < 0.5f || ship_unnamed(g_sh[i]))) { g_sel[1][0] = 0; break; }
  }
  set_map_full(true);
  refresh_idle();
  show(idle_ov, true);
  lv_obj_invalidate(lv_screen_active());  // full redraw: no leftovers from the previous screen
  apply_backlight();
}

static void wake() {
  g_idle = false;
  g_last_touch = millis();
  show(idle_ov, false);
  set_map_full(false);
  set_mode(g_mode_before_idle);
  lv_obj_invalidate(lv_screen_active());
  apply_backlight();
}

static void on_idle_press(lv_event_t *) { wake(); }

static lv_obj_t *dash_eye(lv_obj_t *p, const char *t, uint32_t col, int y) {
  lv_obj_t *l = mk_label(p, t, &lv_font_montserrat_12, col, 20, y);
  lv_obj_set_style_text_letter_space(l, 1, 0);
  return l;
}
static void dash_rule(lv_obj_t *p, int y) {
  lv_obj_t *r = mk_box(p, 20, y, 240, 1);
  lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(r, lv_color_hex(C_LINE), 0);
}

static void build_idle() {
  // transparent full-screen catcher: swallows the waking touch, lets the map show through
  idle_ov = mk_box(lv_layer_top(), 0, 0, 800, 480);
  lv_obj_add_flag(idle_ov, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(idle_ov, on_idle_press, LV_EVENT_PRESSED, nullptr);

  // "following" tag over the map, bottom left
  idle_tag = mk_box(idle_ov, 12, 400, DASH_MAP_W - 24, 68);
  lv_obj_set_style_bg_opa(idle_tag, LV_OPA_80, 0);
  lv_obj_set_style_bg_color(idle_tag, lv_color_hex(C_BG), 0);
  lv_obj_set_style_radius(idle_tag, 10, 0);
  lv_obj_set_style_border_width(idle_tag, 1, 0);
  lv_obj_set_style_border_color(idle_tag, lv_color_hex(C_LINE), 0);
  lv_obj_remove_flag(idle_tag, LV_OBJ_FLAG_CLICKABLE);
  tag_eye = mk_label(idle_tag, "", &lv_font_montserrat_12, C_AMBER, 14, 8);
  lv_obj_set_style_text_letter_space(tag_eye, 1, 0);
  tag_name = mk_dot_label(idle_tag, &lv_font_montserrat_20, C_TEXT, 14, 22, DASH_MAP_W - 52);
  tag_line = mk_dot_label(idle_tag, &lv_font_montserrat_16, C_MUTED, 14, 44, DASH_MAP_W - 52);

  // right column
  lv_obj_t *col = mk_box(idle_ov, DASH_MAP_W, 0, 800 - DASH_MAP_W, 480);
  lv_obj_set_style_bg_opa(col, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(col, lv_color_hex(0x070c18), 0);
  lv_obj_set_style_border_width(col, 1, 0);
  lv_obj_set_style_border_side(col, LV_BORDER_SIDE_LEFT, 0);
  lv_obj_set_style_border_color(col, lv_color_hex(C_LINE), 0);
  lv_obj_remove_flag(col, LV_OBJ_FLAG_CLICKABLE);
  dash_time = mk_label(col, "", &lv_font_montserrat_48, C_TEXT, 20, 18);
  dash_date = mk_label(col, "", &lv_font_montserrat_16, C_MUTED, 22, 74);
  dash_rule(col, 104);
  dash_eye(col, "TO-DO", C_TODO, 118);
  for (int i = 0; i < DASH_TD; i++) dash_td[i] = mk_dot_label(col, &lv_font_montserrat_16, C_TEXT, 20, 142 + i * 26, 240);
  dash_rule(col, 334);
  dash_eye(col, "NEXT LAUNCH", C_SPACE, 348);
  dash_l_count = mk_label(col, "", &lv_font_montserrat_24, C_SPACE, 20, 366);
  dash_l_name = mk_dot_label(col, &lv_font_montserrat_12, C_MUTED, 20, 398, 240);
  dash_l_vis = mk_dot_label(col, &lv_font_montserrat_12, C_SPACE, 20, 416, 240);

  lv_obj_add_flag(idle_ov, LV_OBJ_FLAG_HIDDEN);
}

// ---------------------------------------------------------------- top bar + screen switching
static void refresh_bar() {
  for (int i = 0; i < 4; i++) {
    bool on = (i == g_mode);
    lv_obj_set_style_text_color(tab_lbl[i], lv_color_hex(on ? C_TEXT : C_DIM), 0);
    lv_obj_set_style_border_width(tabs[i], on ? 3 : 0, 0);
    lv_obj_set_style_border_color(tabs[i], lv_color_hex(MODE_ACCENT[i]), 0);
  }
  show(fchip, g_mode == M_PLANES);
  set_text(chip_filter_lbl, FILTER_NAMES[g_filter]);
  // Status only appears when something is wrong; a healthy tile shows nothing here.
  bool wifi_down = g_net_status == 0, planes_err = g_net_status == 2 && g_mode == M_PLANES;
  set_text(chip_status_lbl, wifi_down ? "NO WIFI" : (planes_err ? "NO DATA" : ""));
  lv_obj_set_style_text_color(chip_status_lbl, lv_color_hex(C_MUTED), 0);
  lv_obj_set_style_bg_color(chip_dot, lv_color_hex(wifi_down ? C_DIM : C_RED), 0);
  show(chip_dot, wifi_down || planes_err);
  // with the status hidden, the Planes filter chip sits at the right edge
  lv_obj_set_x(fchip, wifi_down || planes_err ? 560 : 654);
}

static void refresh_all() {
  refresh_bar();
  show(scr_map, map_mode());
  show(scr_space, g_mode == M_SPACE);
  show(scr_todo, g_mode == M_TODO);
  if (map_mode()) {
    rebuild_visible();
    refresh_map();
    refresh_card();
  } else if (g_mode == M_SPACE) {
    refresh_space();
  } else {
    refresh_todo();
  }
}

// Planes and Ships each remember their own zoom (ships are mostly 20-50 nm offshore).
static float g_mode_range[2] = {RANGE_DEFAULT, 50};
static void set_mode(Mode m) {
  if (map_mode()) g_mode_range[sel_slot()] = g_range;
  g_mode = m;
  if (map_mode()) g_range = g_mode_range[sel_slot()];
  refresh_all();
}

// ---------------------------------------------------------------- input
static void select_step(int d) {
  if (!g_vis_n) return;
  int p = selected_pos();
  if (p < 0) p = 0;
  else p = (p + d + g_vis_n) % g_vis_n;
  strlcpy(g_sel[sel_slot()], item_key(g_vis[p]), sizeof g_sel[0]);
  refresh_map();
  refresh_card();
}

void app_input(InputEvt e) {
  if (g_idle) { wake(); return; }  // first knob action just wakes the screen
  g_last_touch = millis();
  switch (e) {
    case IN_MODE_NEXT: set_mode((Mode)((g_mode + 1) % 4)); break;
    case IN_MODE_PREV: set_mode((Mode)((g_mode + 3) % 4)); break;
    case IN_MODE_PRESS:
      if (g_mode == M_PLANES) { g_filter = (Filter)((g_filter + 1) % 3); refresh_all(); }
      break;
    case IN_SEL_NEXT:
      if (map_mode()) select_step(1);
      else if (g_mode == M_TODO) todo_step(1);
      break;
    case IN_SEL_PREV:
      if (map_mode()) select_step(-1);
      else if (g_mode == M_TODO) todo_step(-1);
      break;
    case IN_SEL_PRESS:
      if (g_mode == M_TODO && g_todo_sel >= 0) toggle_todo(g_todo_sel);
      break;
    case IN_SEL_HOLD:
      if (g_mode == M_PLANES && g_sel[0][0]) { toggle_fav(g_sel[0]); refresh_all(); }
      break;
  }
}

// ---------------------------------------------------------------- touch handlers
static void on_tab(lv_event_t *e) { set_mode((Mode)(intptr_t)lv_event_get_user_data(e)); }
static void on_filter(lv_event_t *) { app_input(IN_MODE_PRESS); }
static void on_prev(lv_event_t *) { app_input(IN_SEL_PREV); }
static void on_next(lv_event_t *) { app_input(IN_SEL_NEXT); }
static void on_fav(lv_event_t *) { app_input(IN_SEL_HOLD); }
static void set_range(float r) {
  if (r < RANGE_MIN) r = RANGE_MIN;
  if (r > RANGE_MAX) r = RANGE_MAX;
  g_range = r;
}
static void on_zoom(lv_event_t *e) {
  int d = (int)(intptr_t)lv_event_get_user_data(e);
  set_range(d < 0 ? g_range / 1.6f : g_range * 1.6f);
  refresh_all();
}
static volatile bool g_view_dirty = false;  // map needs a redraw (drag / pinch)

// One-finger drag pans the map.
static bool g_dragging = false;
static void clamp_pan() {
  const float LIM = 100;  // stay within the coastline data
  if (g_pan_e > LIM) g_pan_e = LIM;
  if (g_pan_e < -LIM) g_pan_e = -LIM;
  if (g_pan_n > LIM) g_pan_n = LIM;
  if (g_pan_n < -LIM) g_pan_n = -LIM;
}
static void on_map_press(lv_event_t *e) {
  static int moved = 0;
  if (lv_event_get_code(e) == LV_EVENT_PRESSED) { moved = 0; g_dragging = false; return; }
  lv_point_t v;
  lv_indev_get_vect(lv_indev_active(), &v);
  moved += abs(v.x) + abs(v.y);
  if (moved < 10) return;  // small jitter is still a tap
  g_dragging = true;
  g_lite_map = true;
  float ppn = ring_px() / g_range;
  g_pan_e -= v.x / ppn;
  g_pan_n += v.y / ppn;
  clamp_pan();
  g_view_dirty = true;
}
static void on_recenter(lv_event_t *) {
  g_pan_e = g_pan_n = 0;
  refresh_all();
}

// Tap anywhere on the map: select the nearest item within finger distance.
static void on_map_click(lv_event_t *e) {
  if (g_dragging) return;
  lv_point_t pt;
  lv_indev_get_point(lv_indev_active(), &pt);
  lv_area_t a;
  lv_obj_get_coords(map_obj, &a);
  float tx = pt.x - a.x1, ty = pt.y - a.y1, best = 45 * 45;
  int pick = -1;
  for (int k = 0; k < g_vis_n; k++) {
    float lat, lon, x, y;
    item_pos(g_vis[k], lat, lon);
    project(lat, lon, x, y);
    float d = (x - tx) * (x - tx) + (y - ty) * (y - ty);
    if (d < best) { best = d; pick = k; }
  }
  if (pick < 0) return;
  strlcpy(g_sel[sel_slot()], item_key(g_vis[pick]), sizeof g_sel[0]);
  refresh_map();
  refresh_card();
}

// Two-finger pinch zoom (called from the touch driver every read).
void app_touch(int n, int x0, int y0, int x1, int y1) {
  static bool pinching = false;
  if (n > 0) g_last_touch = millis();
  static float d0, r0;
  if (n >= 2 && !g_idle && map_mode() && x0 < MAP_W && x1 < MAP_W) {
    float d = sqrtf((float)(x1 - x0) * (x1 - x0) + (float)(y1 - y0) * (y1 - y0));
    if (d < 20) return;
    if (!pinching) { pinching = true; d0 = d; r0 = g_range; return; }
    set_range(r0 * d0 / d);  // fingers apart = smaller range = zoom in
    g_lite_map = true;
    g_view_dirty = true;
  } else if (n < 2) {
    pinching = false;
  }
}

// ---------------------------------------------------------------- build UI
static lv_obj_t *mk_card(lv_obj_t *parent, int x, int w) {
  lv_obj_t *card = mk_box(parent, x, 0, w, MAP_H);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_border_side(card, LV_BORDER_SIDE_LEFT, 0);
  lv_obj_set_style_border_color(card, lv_color_hex(C_LINE), 0);
  return card;
}

static void build_map_screen(lv_obj_t *scr) {
  scr_map = mk_box(scr, 0, BAR_H, 800, MAP_H);
  map_obj = mk_box(scr_map, 0, 0, MAP_W, MAP_H);
  lv_obj_set_style_clip_corner(map_obj, true, 0);
  geo = mk_box(map_obj, 0, 0, MAP_W, MAP_H);
  lv_obj_remove_flag(geo, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(geo, draw_geo, LV_EVENT_DRAW_MAIN, nullptr);
  // range rings around home, with scale labels
  for (int r = 1; r <= 2; r++) {
    int rad = ring_px() * r / 2;
    lv_obj_t *ring = ring_obj[r - 1] = mk_box(map_obj, MAP_W / 2 - rad, MAP_H / 2 - rad, rad * 2, rad * 2);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(ring, 1, 0);
    lv_obj_set_style_border_color(ring, lv_color_hex(0x1f2d47), 0);
    lv_obj_remove_flag(ring, LV_OBJ_FLAG_CLICKABLE);
    ring_lbl[r - 1] = mk_label(map_obj, "", &lv_font_montserrat_12, 0x4a5d7c, 0, 0);
  }
  lv_obj_add_event_cb(map_obj, on_map_click, LV_EVENT_CLICKED, nullptr);
  lv_obj_add_event_cb(map_obj, on_map_press, LV_EVENT_PRESSED, nullptr);
  lv_obj_add_event_cb(map_obj, on_map_press, LV_EVENT_PRESSING, nullptr);
  home_obj = mk_box(map_obj, MAP_W / 2 - 4, MAP_H / 2 - 4, 8, 8);
  lv_obj_set_style_radius(home_obj, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(home_obj, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(home_obj, lv_color_hex(0x7fa8e8), 0);
  lv_obj_remove_flag(home_obj, LV_OBJ_FLAG_CLICKABLE);
  north_lbl = mk_label(map_obj, "N", &lv_font_montserrat_12, C_DIM, MAP_W / 2 - 4, 4);

  for (int i = 0; i < MAXP; i++) {
    mk[i] = mk_box(map_obj, 0, 0, 36, 36);
    lv_obj_set_style_radius(mk[i], LV_RADIUS_CIRCLE, 0);
    lv_obj_remove_flag(mk[i], LV_OBJ_FLAG_CLICKABLE);  // taps go to the map (nearest item)
    lv_obj_add_flag(mk[i], LV_OBJ_FLAG_OVERFLOW_VISIBLE);  // rotated icons can poke past the 36 px box
    mk_img[i] = lv_image_create(mk[i]);
    lv_obj_align(mk_img[i], LV_ALIGN_CENTER, 0, 0);  // stays centered when the icon changes
    lv_obj_remove_flag(mk_img[i], LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(mk[i], LV_OBJ_FLAG_HIDDEN);
  }
  // Name tags live on the map itself, next to each icon. (Inside the 36 px marker
  // LVGL limits a label to the marker's width, which left no room for the text.)
  for (int i = 0; i < MAXP; i++) {
    mk_lbl[i] = mk_label(map_obj, "", &lv_font_montserrat_12, C_MUTED, 0, 0);
    lv_obj_set_style_pad_hor(mk_lbl[i], 4, 0);
    lv_obj_set_style_radius(mk_lbl[i], 3, 0);
    lv_obj_remove_flag(mk_lbl[i], LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(mk_lbl[i], LV_OBJ_FLAG_HIDDEN);
  }
  empty_lbl = mk_label(map_obj, "", &lv_font_montserrat_20, C_MUTED, 0, 200);
  lv_obj_set_width(empty_lbl, MAP_W);
  lv_obj_set_style_text_align(empty_lbl, LV_TEXT_ALIGN_CENTER, 0);
  recenter_btn = mk_btn(map_obj, LV_SYMBOL_HOME, 14, MAP_H - 138, 44, 38, on_recenter, nullptr);
  lv_obj_add_flag(recenter_btn, LV_OBJ_FLAG_HIDDEN);
  zoom_btn[0] = mk_btn(map_obj, "+", 14, MAP_H - 94, 44, 38, on_zoom, (void *)(intptr_t)-1);  // zoom in = smaller range
  zoom_btn[1] = mk_btn(map_obj, "-", 14, MAP_H - 50, 44, 38, on_zoom, (void *)(intptr_t)1);

  lv_obj_t *card = map_card = mk_card(scr_map, MAP_W, 800 - MAP_W);
  c_eye = mk_label(card, "", &lv_font_montserrat_12, C_MUTED, 20, 18);
  c_tag = mk_label(card, "", &lv_font_montserrat_12, C_AMBER, 170, 18);
  c_call = mk_dot_label(card, &lv_font_montserrat_40, C_TEXT, 20, 40, 270);
  c_desc = mk_dot_label(card, &lv_font_montserrat_20, C_TEXT, 20, 90, 265);   // airline / ship type
  c_type = mk_dot_label(card, &lv_font_montserrat_12, C_MUTED, 20, 115, 265);  // aircraft type / length
  c_route = mk_dot_label(card, &lv_font_montserrat_24, C_TEXT, 20, 136, 270);
  c_cities = mk_dot_label(card, &lv_font_montserrat_12, C_MUTED, 20, 165, 260);
  for (int i = 0; i < 4; i++) {
    int x = 20 + (i % 2) * 140, y = 190 + (i / 2) * 60;
    c_k[i] = mk_label(card, "", &lv_font_montserrat_12, C_MUTED, x, y);
    c_v[i] = mk_label(card, "", &lv_font_montserrat_24, C_TEXT, x, y + 20);
  }
  c_reg = mk_label(card, "", &lv_font_montserrat_12, C_DIM, 20, 326);
  // Stand-ins for the knobs until they arrive.
  mk_btn(card, LV_SYMBOL_LEFT, 20, 360, 70, 52, on_prev, nullptr);
  c_favbtn = mk_btn(card, "SAVE", 102, 360, 96, 52, on_fav, nullptr, &c_favbtn_lbl);
  mk_btn(card, LV_SYMBOL_RIGHT, 210, 360, 70, 52, on_next, nullptr);
}

static void build_space_screen(lv_obj_t *scr) {
  scr_space = mk_box(scr, 0, BAR_H, 800, MAP_H);
  mk_label(scr_space, "NEXT LAUNCH", &lv_font_montserrat_12, C_MUTED, 24, 18);
  sp_rocket = mk_dot_label(scr_space, &lv_font_montserrat_40, C_TEXT, 24, 38, 450);
  sp_mission = mk_dot_label(scr_space, &lv_font_montserrat_20, C_TEXT, 24, 90, 450);
  sp_where = mk_dot_label(scr_space, &lv_font_montserrat_16, C_MUTED, 24, 120, 450);
  sp_count = mk_label(scr_space, "", &lv_font_montserrat_40, C_SPACE, 24, 152);
  sp_when = mk_label(scr_space, "", &lv_font_montserrat_16, C_MUTED, 24, 204);
  sp_vis = mk_label(scr_space, "", &lv_font_montserrat_16, C_SPACE, 24, 230);
  lv_obj_t *div = mk_box(scr_space, 24, 268, 452, 1);
  lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(div, lv_color_hex(C_LINE), 0);
  mk_label(scr_space, "LATER", &lv_font_montserrat_12, C_MUTED, 24, 282);
  for (int i = 0; i < 4; i++) sp_row[i] = mk_dot_label(scr_space, &lv_font_montserrat_16, C_TEXT, 24, 306 + i * 30, 452);

  lv_obj_t *card = mk_card(scr_space, MAP_W, 800 - MAP_W);
  mk_label(card, "INTERNATIONAL SPACE STATION", &lv_font_montserrat_12, C_MUTED, 20, 18);
  iss_pos = mk_label(card, "", &lv_font_montserrat_24, C_TEXT, 20, 38);
  iss_alt = mk_label(card, "", &lv_font_montserrat_12, C_MUTED, 20, 70);
  iss_tag = mk_label(card, "", &lv_font_montserrat_12, C_AMBER, 150, 70);
  world = mk_box(card, 20, 94, WM_W, WM_H);
  lv_obj_set_style_bg_opa(world, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(world, lv_color_hex(0x0d182b), 0);
  lv_obj_set_style_radius(world, 6, 0);
  lv_obj_set_style_clip_corner(world, true, 0);
  lv_obj_remove_flag(world, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(world, draw_world, LV_EVENT_DRAW_MAIN, nullptr);
  mk_label(card, "NEXT PASS OVER YOU", &lv_font_montserrat_12, C_MUTED, 20, 244);
  pass_when = mk_label(card, "", &lv_font_montserrat_24, C_TEXT, 20, 264);
  pass_in = mk_label(card, "", &lv_font_montserrat_20, C_SPACE, 20, 296);
  pass_info = mk_label(card, "", &lv_font_montserrat_16, C_MUTED, 20, 330);
  pass_dir = mk_label(card, "", &lv_font_montserrat_16, C_MUTED, 20, 354);
  mk_label(card, "Best at dawn and dusk, when the\nISS is sunlit and your sky is dark.", &lv_font_montserrat_12, C_DIM, 20, 388);
}

static void build_todo_screen(lv_obj_t *scr) {
  scr_todo = mk_box(scr, 0, BAR_H, 800, MAP_H);
  todo_head = mk_label(scr_todo, "", &lv_font_montserrat_12, C_MUTED, 30, 14);
  todo_list = lv_obj_create(scr_todo);
  lv_obj_remove_style_all(todo_list);
  lv_obj_set_pos(todo_list, 24, 36);
  lv_obj_set_size(todo_list, 752, 390);
  lv_obj_set_flex_flow(todo_list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(todo_list, 4, 0);
  lv_obj_set_scroll_dir(todo_list, LV_DIR_VER);
  todo_empty = mk_label(scr_todo, "", &lv_font_montserrat_24, C_MUTED, 0, 170);
  lv_obj_set_width(todo_empty, 800);
  lv_obj_set_style_text_align(todo_empty, LV_TEXT_ALIGN_CENTER, 0);
}

static void build_ui() {
  lv_obj_t *scr = lv_screen_active();
  lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  // top bar
  lv_obj_t *bar = bar_obj = mk_box(scr, 0, 0, 800, BAR_H);
  lv_obj_set_style_border_width(bar, 1, 0);
  lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
  lv_obj_set_style_border_color(bar, lv_color_hex(C_LINE), 0);
  int tx = 16;
  const int tw[4] = {104, 92, 92, 92};
  for (int i = 0; i < 4; i++) {
    tabs[i] = mk_box(bar, tx, 0, tw[i], BAR_H - 1);
    lv_obj_add_flag(tabs[i], LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(tabs[i], on_tab, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    lv_obj_set_style_border_side(tabs[i], LV_BORDER_SIDE_BOTTOM, 0);
    tab_lbl[i] = mk_label(tabs[i], MODE_NAMES[i], &lv_font_montserrat_16, C_DIM, 0, 0);
    lv_obj_set_style_text_letter_space(tab_lbl[i], 1, 0);
    lv_obj_center(tab_lbl[i]);
    tx += tw[i];
  }
  fchip = mk_box(bar, 560, 7, 130, 30);
  lv_obj_add_flag(fchip, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(fchip, on_filter, LV_EVENT_CLICKED, nullptr);
  lv_obj_set_style_border_width(fchip, 1, 0);
  lv_obj_set_style_border_color(fchip, lv_color_hex(0x33455f), 0);
  lv_obj_set_style_radius(fchip, 4, 0);
  chip_filter_lbl = mk_label(fchip, "ALL", &lv_font_montserrat_12, 0xc9d3e2, 0, 0);
  lv_obj_center(chip_filter_lbl);
  chip_dot = mk_box(bar, 704, 18, 8, 8);
  lv_obj_set_style_radius(chip_dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(chip_dot, LV_OPA_COVER, 0);
  chip_status_lbl = mk_label(bar, "WIFI...", &lv_font_montserrat_12, C_MUTED, 718, 14);

  build_map_screen(scr);
  build_space_screen(scr);
  build_todo_screen(scr);

  build_idle();

  // full-screen "updating" overlay on the top layer
  ota_overlay = mk_box(lv_layer_top(), 0, 0, 800, 480);
  lv_obj_set_style_bg_opa(ota_overlay, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(ota_overlay, lv_color_hex(C_BG), 0);
  ota_lbl = mk_label(ota_overlay, "Updating firmware", &lv_font_montserrat_24, C_TEXT, 0, 190);
  lv_obj_set_width(ota_lbl, 800);
  lv_obj_set_style_text_align(ota_lbl, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_t *track = mk_box(ota_overlay, 200, 240, 400, 8);
  lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(track, lv_color_hex(C_LINE), 0);
  lv_obj_set_style_radius(track, 4, 0);
  ota_bar = mk_box(track, 0, 0, 0, 8);
  lv_obj_set_style_bg_opa(ota_bar, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(ota_bar, lv_color_hex(C_SPACE), 0);
  lv_obj_set_style_radius(ota_bar, 4, 0);
  lv_obj_add_flag(ota_overlay, LV_OBJ_FLAG_HIDDEN);
}

// ---------------------------------------------------------------- public
void app_init() {
  g_mtx = xSemaphoreCreateMutex();
  g_net = (Plane *)heap_caps_calloc(MAXP, sizeof(Plane), MALLOC_CAP_SPIRAM);
  g_pl = (Plane *)heap_caps_calloc(MAXP, sizeof(Plane), MALLOC_CAP_SPIRAM);
  g_sh_net = (Ship *)heap_caps_calloc(MAXS, sizeof(Ship), MALLOC_CAP_SPIRAM);
  g_sh = (Ship *)heap_caps_calloc(MAXS, sizeof(Ship), MALLOC_CAP_SPIRAM);
  g_td_net = (Todo *)heap_caps_calloc(MAXT, sizeof(Todo), MALLOC_CAP_SPIRAM);
  g_td = (Todo *)heap_caps_calloc(MAXT, sizeof(Todo), MALLOC_CAP_SPIRAM);
  g_prefs.begin("skytile", false);
  g_favs = g_prefs.getString("favs", ",");
  if (g_favs.length() == 0 || g_favs[0] != ',') g_favs = "," + g_favs;
  build_ui();
  refresh_all();
  xTaskCreatePinnedToCore(net_task, "net", 16384, nullptr, 1, nullptr, 0);
}

void app_loop() {
  static uint32_t last_bar = 0, last_view = 0, last_tick = 0;
  static int ota_shown = -1;
  if (g_ota_pct != ota_shown) {  // wireless update progress
    ota_shown = g_ota_pct;
    show(ota_overlay, ota_shown != -1);
    if (ota_shown >= 0) {
      char b[48];
      snprintf(b, sizeof b, ota_shown < 100 ? "Updating firmware  %d%%" : "Update done, restarting", ota_shown);
      set_text(ota_lbl, b);
      lv_obj_set_width(ota_bar, 4 * ota_shown);
    } else if (ota_shown == -2) {
      set_text(ota_lbl, "Update failed. Still running the old version.");
    }
  }
  if (g_ota_pct == -2) {  // hide the failure message after a few seconds
    static uint32_t fail_at = 0;
    if (!fail_at) fail_at = millis();
    if (millis() - fail_at > 5000) { g_ota_pct = -1; fail_at = 0; }
  }
  if ((g_pan_e != 0 || g_pan_n != 0) && millis() - g_last_touch > 120000) {  // idle: back to the overview
    g_pan_e = g_pan_n = 0;
    g_view_dirty = true;
  }
  if (!g_idle && g_ota_pct == -1 && millis() - g_last_touch > IDLE_MS) go_idle();
  static uint32_t last_idle_tick = 0;
  if (millis() - last_idle_tick >= 1000) {  // once a second: idle view (countdowns, clock) + backlight schedule
    last_idle_tick = millis();
    if (g_idle) refresh_idle();
    apply_backlight();
  }
  if (g_lite_map && millis() - g_last_touch > 150) {  // finger lifted: redraw in full detail
    g_lite_map = false;
    lv_obj_invalidate(geo);
  }
  if (g_view_dirty && millis() - last_view > 60) {  // drag / pinch: redraw at most ~15 fps
    g_view_dirty = false;
    last_view = millis();
    refresh_all();
    return;
  }
  if (g_net_new) {
    xSemaphoreTake(g_mtx, portMAX_DELAY);
    memcpy(g_pl, g_net, sizeof(Plane) * g_net_n);
    g_pl_n = g_net_n;
    g_net_new = false;
    xSemaphoreGive(g_mtx);
    if (g_mode == M_PLANES) refresh_all();
  } else if (g_sh_new) {
    xSemaphoreTake(g_mtx, portMAX_DELAY);
    memcpy(g_sh, g_sh_net, sizeof(Ship) * g_sh_net_n);
    g_sh_n = g_sh_net_n;
    g_sh_new = false;
    g_sh_loaded = true;
    xSemaphoreGive(g_mtx);
    if (g_mode == M_SHIPS) refresh_all();
  } else if (g_sp_new) {
    xSemaphoreTake(g_mtx, portMAX_DELAY);
    g_sp = g_sp_net;
    g_sp_new = false;
    xSemaphoreGive(g_mtx);
    lv_obj_invalidate(world);
    if (g_mode == M_SPACE) refresh_space();
  } else if (g_td_new) {
    xSemaphoreTake(g_mtx, portMAX_DELAY);
    bool pending = g_tq_n > 0;  // a check-off is still on its way: keep the local state
    if (!pending) {
      memcpy(g_td, g_td_net, sizeof(Todo) * g_td_net_n);
      g_td_n = g_td_net_n;
      sort_todos();
      g_td_loaded = true;
    }
    g_td_new = false;
    xSemaphoreGive(g_mtx);
    if (!pending && g_mode == M_TODO) refresh_todo();
  } else if (g_route_new) {
    g_route_new = false;
    if (g_mode == M_PLANES) refresh_card();
  } else if (g_mode == M_SPACE && millis() - last_tick >= 1000) {  // countdowns
    last_tick = millis();
    refresh_space();
  } else if (millis() - last_bar > 2000) {
    last_bar = millis();
    refresh_bar();
  }
}
