# SkyTile (James's DIY desk display)

A desk tile modeled on theskytile.com: a 5" color touchscreen in a 3D-printed wedge case with a black anodized aluminum front and two clicky knobs. Four modes: **Planes, Ships, Space, To-do**. Live data comes from online APIs. The to-do list is updated by telling Claude (via an MCP server on Cloudflare).

Owner: James, San Diego. He prefers direct, concise answers and no em dashes in writing. He's new to soldering and embedded work, so explain hardware steps plainly.

## Status (Oct 10, 2026)
- [x] Display arrived and powers on over USB-C from the laptop.
- [ ] Confirm the board version printed on the back. The case was modeled on Elecrow's STEP model **v1.2**. If the board is a different version, compare the mounting holes, USB-C position and I2C port location before printing.
- [ ] Adafruit order (2x #5880, #4209, #4210): placed or about to be placed.
- [ ] SendCutSend faceplate: in cart (black anodized 5052, 0.063", $32.21 shipped).
- [ ] Craftcloud print: cart holds OLD v3 files. Re-upload `case/skytile_shell.stl` + `case/skytile_back.stl` (v4) before ordering. White PLA, about $39.
- [x] Toolchain: PlatformIO (Homebrew). Mac needs WCH CH34x DriverKit driver (activate via /Applications/CH34xVCPDriver.app); port is `/dev/cu.wchusbserial*` (number changes with the USB socket: flash with `--upload-port $(ls /dev/cu.wchusbserial* | head -1)`). Board is V1.2/V1.3 hardware (0x30 helper MCU present).
- [ ] Firmware (`firmware/skytile/`): live Planes screen with touch works. WiFi creds in gitignored `include/secrets.h`. adsb.lol needs a non-generic User-Agent. LVGL + JSON allocate in PSRAM (internal RAM is needed for TLS). Knobs not wired yet (Adafruit order pending).
  - Build with `~/.local/pio61/bin/pio` (PlatformIO 6.1.19 venv), not Homebrew pio 6.2. `custom_sdkconfig` in platformio.ini rebuilds the ESP32 libs (XIP from PSRAM, IRAM-safe LCD ISR, 64B cache lines) to stop the RGB screen shifting while WiFi runs. After a clean or sdkconfig change, the first `pio run` fails with `SCons.Tool.FortranCommon` (tool reinstall race); just run it again.
  - Don't copy Elecrow's `sdkconfig.defaults*` or the `-DCONFIG_SPIRAM_SPEED_120M` flag back in: 120 MHz flash/PSRAM boot-loops this board. Pixel clock 15 MHz (12 MHz makes the panel flash colors).
- [x] Cloudflare Worker `skytile` deployed: https://skytile.seobubbles.workers.dev (code in `worker/`, deploy with `npx wrangler deploy`). Endpoints `/route/:cs`, `/space`, `/ships`, `/todos` need header `X-Tile-Key`; MCP connector at `/mcp/<MCP_SECRET>`. Secrets TILE_KEY / MCP_SECRET are in gitignored `worker/.dev.vars`; AISSTREAM_KEY is set by James. Planes stay direct from the tile (adsb.lol rate-limits Cloudflare IPs). Launches: LL2 with RocketLaunch.live fallback; ISS TLE: CelesTrak with tle.ivanstanojevic.me fallback; last good copies kept in KV.
- [x] Firmware screens for Ships / Space / To-do flashed (Oct 10). Planes + Ships share the map (pan, pinch, tap-nearest). To-do check-offs POST to the Worker. MCP connector added in claude.ai as "Desktile". Verified working by James (Oct 10 night).
- [ ] Pick up here: (1) confirm plane/ship name tags now show (moved labels from the marker box onto the map, flashed but not yet seen); (2) knobs when the Adafruit order arrives (Step 2: I2C read, fix 0x36/0x37 clash, map to app_input(), hide the on-screen arrow/SAVE buttons when knobs are detected). Icons (sources in `design/icons/`, made in Quiver): planes by class (airliner, fighter, military cargo, business jet, small prop, military/civil helicopter) from adsb.lol mil flag + ADS-B category + type code; ships by AIS type (sailboat, cargo, tanker, cruise, navy, fishing, tug, yacht), side views mirrored when heading west; airports = control tower; selection = amber tint only. Generator scripts are in `firmware/skytile/tools/` (svgraster.py renders the SVGs, icons2c.py / tower2c.py make LVGL images, mapgen.py builds mapdata.h from OSM coastline + Natural Earth border).

## Repo
- GitHub (private): https://github.com/Jamesborlands/desktile, branch `main`.
- Not committed: `firmware/skytile/include/secrets.h` (copy from `secrets.example.h`), `worker/.dev.vars`, `vendor/` (Elecrow clone), build output.

## Hardware
- **Display:** Elecrow CrowPanel Advance 5.0" HMI, ESP32-S3, 800x480 IPS, capacitive touch (Amazon set "5" Advanced ESP32 Display with Acrylic Case"; the acrylic case is unused). Built-in WiFi/BLE. The optional plug-in wireless module is not needed.
  - Pin maps, display driver setup, LVGL config and examples: Elecrow's repo `github.com/Elecrow-RD/CrowPanel-Advance-5-HMI-ESP32-S3-AI-Powered-IPS-Touch-Screen-800x480` and the Elecrow wiki page "CrowPanel Advance 5.0-HMI ESP32 AI Display". Pull pins from there. Don't guess them.
  - I2C expansion port is HY2.0-4P (Grove-style). The box includes a Crowtail/Grove to 4-pin DuPont cable.
- **Knobs:** 2x Adafruit #5880 I2C STEMMA QT Rotary Encoder (seesaw firmware, PEC11 encoder with push switch, 6 mm D-shaft, M7 bushing).
  - Both default to I2C address **0x36**. Change one to **0x37**: try seesaw's `setI2CAddress()` (stored in EEPROM) first. Fallback: bridge solder jumper A0 on one board (James has a soldering kit).
  - Library: Adafruit_seesaw (`getEncoderPosition`, `digitalRead` on the switch pin with pullup, per Adafruit's 5880 guide).
- **Wiring:** panel I2C port -> Grove-to-DuPont cable (in the display box) -> Adafruit #4209 (QT to male headers, 150 mm) -> bottom knob board -> #4210 (QT 100 mm) -> top knob board. No soldering.
- **Knob hardware:** Taiss silver aluminum set-screw knobs (20 mm, or the 17 mm 4-pack if James swapped), PATIKIL M7x0.75 nuts.
- **Power:** right-angle USB-C (Anker 90-degree cable / AuviPal adapter). A straight plug hits the sloped base. The cable exits through the slot in the back cover.

## Knob controls (from the interactive simulator)
- **Top knob = Mode.** Turn: switch Planes / Ships / Space / To-do. Press: filter All / Interesting / Favorites.
- **Bottom knob = Select.** Turn: move through items (planes, ships, launches, tasks). Press: open details / follow (to-do: check off). Hold: add or remove favorite.
- Idle: after inactivity, return to the overview. Touch also works (tap to select, zoom buttons on the map).

## Data sources
- **Planes:** adsb.lol API (free, no key), fallback airplanes.live. Center on San Diego, about a 25 mi radius. No ADS-B antenna for now (maybe later).
- **Ships:** aisstream.io (free API key, WebSocket). Too heavy for the ESP32 directly, so the Cloudflare Worker should aggregate it and serve a compact JSON of vessels near San Diego.
- **Space:** Launch Library 2 (upcoming launches) + CelesTrak (ISS and other satellites overhead).
- **To-do:** Cloudflare Worker + KV or D1, exposed as an MCP server so James can say "add X to my tile" to Claude. The tile polls a small JSON endpoint.
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
