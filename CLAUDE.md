# SkyTile (James's DIY desk display)

A desk tile modeled on theskytile.com: a 5" color touchscreen in a 3D-printed wedge case with a black anodized aluminum front and two clicky knobs. Four modes: **Planes, Ships, Space, To-do**. Live data comes from online APIs. The to-do list is updated by telling Claude (via an MCP server on Cloudflare).

Owner: James, San Diego. He prefers direct, concise answers and no em dashes in writing. He's new to soldering and embedded work, so explain hardware steps plainly.

## Status (Oct 25, 2026)
- [x] Display arrived and powers on over USB-C from the laptop.
- [x] Board version: the label is hidden under the Amazon acrylic back panel, but firmware sees the 0x30 helper MCU, so it's V1.2/V1.3 hardware (the case was modeled on v1.2). Remove the acrylic panel before fitting the case.
- [x] Adafruit order (2x #5880, #4209, #4210): arrived, knobs wired.
- [x] SendCutSend faceplate: ORDERED Oct 25, 5052 0.063", clear anodized (James switched from black to a plain aluminum look), $32.21 shipped.
- [x] Case print: ORDERED Oct 25. v4 `case/skytile_shell.stl` + `case/skytile_back.stl` on Craftcloud, gray PLA, $35.88 shipped (3D For Me Studio, Nov 3-10). Compared: Form Now white PLA $46.99 with shipping, Craftcloud ivory $58.07, JLC3DP ~$21 but 2-3 weeks from China; SendCutSend doesn't print.
- [x] Toolchain: PlatformIO (Homebrew). Mac needs WCH CH34x DriverKit driver (activate via /Applications/CH34xVCPDriver.app); port is `/dev/cu.wchusbserial*` (number changes with the USB socket: flash with `--upload-port $(ls /dev/cu.wchusbserial* | head -1)`). Board is V1.2/V1.3 hardware (0x30 helper MCU present).
- [ ] Firmware (`firmware/skytile/`): live Planes screen with touch works. WiFi creds in gitignored `include/secrets.h`. adsb.lol needs a non-generic User-Agent. LVGL + JSON allocate in PSRAM (internal RAM is needed for TLS). Knobs not wired yet (Adafruit order pending).
  - Flash over WiFi: `firmware/skytile/tools/ota.sh` (builds, then sends to skytile.local with the OTA_PASS from secrets.h; two 6 MB app slots). Claude must run it with the sandbox disabled (the sandbox blocks the tile's connection back to the Mac). USB still works as a fallback.
  - Debug over USB serial: `tools/shot.py` grabs a screenshot of the framebuffer (tile answers 'S'); sending '0'-'3' puts it to sleep on that mode; 'H' = next hotspot, 'N' = next mode (first key just wakes it if asleep). Run the tools with `~/.local/pio61/bin/python` (has pyserial). If a photo shows glitches but the screenshot is clean, it's panel scan-out starving for PSRAM bandwidth: reduce redraw load.
  - Build with `~/.local/pio61/bin/pio` (PlatformIO 6.1.19 venv), not Homebrew pio 6.2. `custom_sdkconfig` in platformio.ini rebuilds the ESP32 libs (XIP from PSRAM, IRAM-safe LCD ISR, 64B cache lines) to stop the RGB screen shifting while WiFi runs. After a clean or sdkconfig change, the first `pio run` fails with `SCons.Tool.FortranCommon` (tool reinstall race); just run it again.
  - Don't copy Elecrow's `sdkconfig.defaults*` or the `-DCONFIG_SPIRAM_SPEED_120M` flag back in: 120 MHz flash/PSRAM boot-loops this board. Keep `CONFIG_LCD_RGB_RESTART_IN_VSYNC=n`: with bounce buffers it left the picture stuck shifted ~30 lines (top rows at the bottom) after one missed DMA interrupt. Pixel clock 15 MHz (12 MHz makes the panel flash colors).
- [x] Cloudflare Worker `skytile` deployed: https://skytile.seobubbles.workers.dev (code in `worker/`, deploy with `npx wrangler deploy`). Endpoints `/route/:cs`, `/space`, `/ships`, `/todos` need header `X-Tile-Key`; MCP connector at `/mcp/<MCP_SECRET>`; client API `/api/v1/todos` needs `Authorization: Bearer <API_TOKEN>` (token also in gitignored root `.env` as SKYTILE_API_TOKEN). To-dos live in the `TodoStore` Durable Object (SQLite, `worker/src/todos.ts`) since Oct 24; the old KV `todos` key is a backup only. Tests: `cd worker && npm test`. Secrets TILE_KEY / MCP_SECRET are in gitignored `worker/.dev.vars`; AISSTREAM_KEY is set by James. Planes stay direct from the tile (adsb.lol rate-limits Cloudflare IPs). Launches: LL2 with RocketLaunch.live fallback; ISS TLE: CelesTrak with tle.ivanstanojevic.me fallback; last good copies kept in KV.
- [x] Firmware screens for Ships / Space / To-do flashed (Oct 10). Planes + Ships share the map (pan, pinch, tap-nearest). To-do check-offs POST to the Worker. MCP connector added in claude.ai as "Desktile". Verified working by James (Oct 10 night).
- [x] Hotspots (Oct 17; split Oct 25): Planes and Ships have separate lists (`screens` bitmask in `tools/hotspotgen.py`). Planes: San Diego, Los Angeles, Denver, London Heathrow, Tokyo. Ships: San Diego, Strait of Hormuz, Persian Gulf, Dover Strait, Rotterdam, Singapore Strait, Hong Kong, Taiwan Strait, Tokyo Bay, Puget Sound, SF Bay, New York Harbor. Switching screens while on a spot that isn't on the new screen's list goes home. Tap the place pill (top-left of the map) = next, long-press = home; top knob press = next, hold = home. List + coastlines (Natural Earth 10m) are generated by `tools/hotspotgen.py` into `src/hotspots.h`; ship hotspot ids must match `REGIONS` in `worker/src/ships.ts`. The Worker's one aisstream connection subscribes to every region asked for in the last 15 min. Hormuz/Gulf default to the new MILITARY planes filter. After 3 min with no touch or knob input the map goes back to San Diego (HOTSPOT_MS); boot always starts in San Diego. aisstream coverage tested Oct 17: strong in US/Europe/East Asia ports, thin in Hormuz/Gulf (mostly anchored ships off Dubai), none in Red Sea, Suez, Panama, Bosphorus, Gibraltar (dropped).
  - airplanes.live now returns 403 unless you email them for access: not a usable fallback.
  - Plane feeds (Oct 25): adsb.lol, then adsb.fi, then OpenSky (`g_feeds` in app.cpp). adsb.lol caps each IP at 500 requests/day (header `500;w=86400`, seen since ~Oct 20); the tile's IPv4 counts, the Mac's IPv6 is separate, so test with `curl -4`. adsb.fi has no daily cap (1 req/s). OpenSky logs in with OAuth2 client credentials (`OPENSKY_ID`/`OPENSKY_SECRET` in secrets.h, 4,000 credits/day; James's credentials.json is in ~/Downloads) and needs `useHTTP10` or reads time out. Planes fetch only on the Planes screen (10 s awake, 5 min asleep); a 429 parks a feed for 10 min.
  - Airports: `AIRPORTS[]` in app.cpp covers SoCal plus each hotspot (generated from OurAirports). Route card says "Private flight" for tail-number callsigns; routes: the tile sends the plane's position to `/route/:cs?lat=&lon=`; the Worker tries Flightradar24's live map feed first (`feed.js?callsign=`, unofficial, James chose it Oct 25; only codes, so cities/positions come from `worker/src/airports.ts`, generated by `worker/scripts/airportsgen.py` from OurAirports), then legs from adsb.lol's VRS standing data (vrs-standing-data.adsb.lol, right for most airlines) and adsbdb (better for Southwest, whose numbers change daily) and returns the first leg the plane fits (`legFits`, same test as `route_fits`). Oct 25 check: 19 of 36 live flights fit VRS vs about 15 for adsbdb; most misses are Southwest.
  - Map: HOME button next to the place pill while away from San Diego.
  - USB 'D' = demo to-dos on screen only (for screenshots); a restart brings the real list back.
- [x] To-do clients (Oct 24): Mac menu bar via SwiftBar (`clients/swiftbar/`, config `~/.config/skytile/config`), iPhone app + widget (`clients/ios/`, XcodeGen; runs in the simulator only: James decided against a paid Apple developer account, so the free path is the `/t/<WEB_SECRET>` web app added to the home screen). Tile polls /todos every 5 s on the To-do screen and redraws only when the list changed. Sleep dashboard: TO-DO and NEXT LAUNCH sections hide when empty, to-dos are bulleted and wrap, "+N more" when they don't fit; To-do screen rows grow for wrapped text (flashed, not yet confirmed by James). Setup for each client is in README.md.
- [x] Confirmed by James (Oct 25): name tags, to-do layouts, knobs (turn direction, press = next location, hold = home), live routes incl. Southwest, per-screen location lists.
- [ ] Pick up here: assemble when the parts arrive (case ~Nov 3-10, faceplate ~Nov 4): VHB-tape the faceplate, remove the Amazon acrylic back panel, panel on 4x M3x6, back on 4x M3x8. Icons (sources in `design/icons/`, made in Quiver): planes by class (airliner, fighter, military cargo, business jet, small prop, military/civil helicopter) from adsb.lol mil flag + ADS-B category + type code; ships by AIS type (sailboat, cargo, tanker, cruise, navy, fishing, tug, yacht), side views mirrored when heading west; airports = control tower; selection = amber tint only. Generator scripts are in `firmware/skytile/tools/` (svgraster.py renders the SVGs, icons2c.py / tower2c.py make LVGL images, mapgen.py builds mapdata.h from OSM coastline + Natural Earth border).

## Repo
- GitHub (public since Oct 25): https://github.com/Jamesborlands/desktile, branch `main`.
- Not committed: `firmware/skytile/include/secrets.h` (copy from `secrets.example.h`), `worker/.dev.vars`, root `.env` (client API token), `worker/backups/`, `vendor/` (Elecrow clone), build output, the generated iOS `.xcodeproj`.

## Hardware
- **Display:** Elecrow CrowPanel Advance 5.0" HMI, ESP32-S3, 800x480 IPS, capacitive touch (Amazon set "5" Advanced ESP32 Display with Acrylic Case"; the acrylic case is unused). Built-in WiFi/BLE. The optional plug-in wireless module is not needed.
  - Pin maps, display driver setup, LVGL config and examples: Elecrow's repo `github.com/Elecrow-RD/CrowPanel-Advance-5-HMI-ESP32-S3-AI-Powered-IPS-Touch-Screen-800x480` and the Elecrow wiki page "CrowPanel Advance 5.0-HMI ESP32 AI Display". Pull pins from there. Don't guess them.
  - I2C expansion port is HY2.0-4P (Grove-style). The box includes a Crowtail/Grove to 4-pin DuPont cable.
- **Knobs:** 2x Adafruit #5880 I2C STEMMA QT Rotary Encoder (seesaw firmware, PEC11 encoder with push switch, 6 mm D-shaft, M7 bushing).
  - Both default to I2C address **0x36**. Changing the address in software does NOT work on these boards (tried Oct 25: EEPROM write to 0x7F and 0xFF, then reset; the knob stayed at 0x36). Done instead: solder jumper A0 bridged on the top knob = **0x37**.
  - Library: Adafruit_seesaw (`getEncoderPosition`, `digitalRead` on the switch pin with pullup, per Adafruit's 5880 guide).
- **Wiring:** panel I2C port -> Grove-to-DuPont cable (in the display box) -> Adafruit #4209 (QT to male headers, 150 mm) -> bottom knob board -> #4210 (QT 100 mm) -> top knob board. The top knob has solder jumper A0 bridged (0x37); the firmware accepts the top knob at 0x37-0x3D.
- **Knob hardware:** Taiss silver aluminum set-screw knobs (20 mm, or the 17 mm 4-pack if James swapped), PATIKIL M7x0.75 nuts.
- **Power:** right-angle USB-C (Anker 90-degree cable / AuviPal adapter). A straight plug hits the sloped base. The cable exits through the slot in the back cover.

## Knob controls (from the interactive simulator)
- **Top knob = Mode.** Turn: switch Planes / Ships / Space / To-do. Press: next hotspot. Hold: home to San Diego. (Oct 25: the filter moved off the knob; tap the ALL chip in the top bar.)
- **Bottom knob = Select.** Turn: move through items (planes, ships, launches, tasks). Press: open details / follow (to-do: check off). Hold: add or remove favorite.
- Idle: after inactivity, return to the overview. Touch also works (tap to select, zoom buttons on the map).

## Data sources
- **Planes:** adsb.lol API (free, no key), fallback airplanes.live. Center on San Diego, about a 25 mi radius. No ADS-B antenna for now (maybe later).
- **Ships:** aisstream.io (free API key, WebSocket). Too heavy for the ESP32 directly, so the Cloudflare Worker should aggregate it and serve a compact JSON of vessels near San Diego.
- **Space:** Launch Library 2 (upcoming launches) + CelesTrak (ISS and other satellites overhead).
- **To-do:** Cloudflare Worker + a Durable Object (SQLite), exposed as an MCP server so James can say "add X to my tile" to Claude, plus `/api/v1/todos` for the Mac and iPhone clients. The tile polls a small JSON endpoint.
- **Backend decisions:** Cloudflare only (cheaper). **No Notion, no Supabase.**

## Suggested build order
1. Get an Elecrow example running (Arduino IDE or PlatformIO) to confirm the toolchain, display and touch.
2. Read both knobs over I2C, fix the address clash, print turns and presses to serial.
3. A minimal Planes screen with live adsb.lol data over WiFi.
4. Cloudflare Worker: `/planes`, `/ships`, `/space`, `/todos` proxies with caching, plus the to-do MCP server.
5. Full LVGL UI for all four modes matching the mockups, plus knob navigation.

## Case and design files
- `case/`: v4 parametric CadQuery case (`skytile_case.py`) and exports.
  - `skytile_shell.stl` (print face-down), `skytile_back.stl`.
  - `skytile_faceplate.dxf` (SendCutSend), STEP files.
  - Front pocket 2.24 mm deep (1.6 mm plate + 0.64 mm 3M VHB 5925 tape), 1.2 mm rim, 0.15 mm side gap, 6 crush ribs.
  - 15 degree wedge, 171.6 x 92.3 mm face, knobs 42 mm apart in a 34 mm column at the viewer's right.
  - Panel mounts with 4x M3x6 into printed bosses. The back cover uses 4x M3x8.
- Mockups and references (claude.ai artifacts; local copies of the simulator and parts list are in `design/`):
  - Knob simulator (UI and interaction spec): https://claude.ai/artifact/EqjFGJA46fk2nkSqCuXGSb
  - 3D assembly viewer: https://claude.ai/artifact/C2kd6wGDwgbSvWa5sWEj6i
  - Desk mockup (all four modes): https://claude.ai/artifact/J4Cq6XaEreuQV7RG6extmg
  - Parts list: https://claude.ai/artifact/UvxsysKeymWo3XKZMTeaac
