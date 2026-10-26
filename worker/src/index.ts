// SkyTile Worker: compact data feeds for the ESP32 tile (planes come straight from
// adsb.lol on the tile: it rate-limits Cloudflare's shared IPs), the to-do list, and an
// MCP server so Claude can manage the to-do list.
//
//   GET  /route/:callsign?lat=&lon=  origin/destination of the leg the plane is flying
//                                (adsb.lol VRS data + adsbdb, cached a day)
//   GET  /space                  next launches + ISS position and next pass
//   GET  /ships?r=<region>       vessels near San Diego or a hotspot (aisstream.io via Durable Object)
//   GET  /todos                  list; POST /todos {text}; POST /todos/:id/done; DELETE /todos/:id
//   *    /mcp/<MCP_SECRET>       MCP server (Streamable HTTP) for Claude
//   GET  /t/<WEB_SECRET>         to-do web app for the phone; its API (and the Siri
//                                Shortcut) lives under /t/<WEB_SECRET>/api/todos
//   *    /api/v1/todos           client API (Mac menu bar, iPhone app + widget), header
//                                Authorization: Bearer <API_TOKEN>
//
// Everything else needs the X-Tile-Key header (secret TILE_KEY).
import { McpServer } from "@modelcontextprotocol/server";
import { createMcpHandler } from "agents/mcp/server";
import * as sat from "satellite.js";
import { z } from "zod";
import { REGIONS, Ships, shipTypeName } from "./ships";
import { MAX_TEXT, Todo, TodoStore } from "./todos";
import { todoPage } from "./webapp";

export { Ships, TodoStore };

const UA = "SkyTile/0.1 (DIY desk display)"; // adsb.lol rejects generic agents

const json = (data: unknown, status = 200, maxAge = 0) =>
  new Response(JSON.stringify(data), {
    status,
    headers: { "content-type": "application/json", "cache-control": maxAge ? `public, max-age=${maxAge}` : "no-store" },
  });

// The tile's fonts only cover plain ASCII: fold accents (é -> e, ñ -> n) and drop
// anything else (emoji, symbols) from responses sent to the tile.
const tileJson = (data: unknown, status = 200) =>
  new Response(
    JSON.stringify(data)
      .normalize("NFD")
      .replace(/[\u0300-\u036f]/g, "")
      .replace(/[^\x00-\x7f]/g, ""),
    { status, headers: { "content-type": "application/json", "cache-control": "no-store" } },
  );

// Cache API wrapper: serve a cached JSON body if fresh, else build and store it.
async function cached(key: string, ttl: number, ctx: ExecutionContext, build: () => Promise<unknown>) {
  const cache = caches.default;
  const req = new Request(`https://cache.skytile/${key}`);
  const hit = await cache.match(req);
  if (hit) return hit.json();
  const data = await build();
  ctx.waitUntil(cache.put(req, json(data, 200, ttl)));
  return data;
}

// Like cached(), but on a cache miss answer at once with the last good copy in KV
// and rebuild in the background. Upstreams (Launch Library especially) can take 20 s,
// longer than the tile waits. Only waits for the build when KV has nothing yet.
async function cachedOrStale(env: Env, key: string, ttl: number, ctx: ExecutionContext, build: () => Promise<unknown>) {
  const cache = caches.default;
  const req = new Request(`https://cache.skytile/${key}`);
  const hit = await cache.match(req);
  if (hit) return hit.json();
  const refresh = build().then(async (data) => {
    await Promise.all([cache.put(req, json(data, 200, ttl)), env.TODOS.put(`cache:${key}`, JSON.stringify(data))]);
    return data;
  });
  const stale = await env.TODOS.get(`cache:${key}`, "json");
  if (stale) {
    ctx.waitUntil(refresh.catch((err) => console.warn(`${key} refresh failed`, err)));
    return stale;
  }
  return refresh;
}

const UPSTREAM_TIMEOUT = 8000;

function distNm(lat1: number, lon1: number, lat2: number, lon2: number) {
  const r = Math.PI / 180;
  const a = Math.sin(((lat2 - lat1) * r) / 2) ** 2 + Math.cos(lat1 * r) * Math.cos(lat2 * r) * Math.sin(((lon2 - lon1) * r) / 2) ** 2;
  return 2 * 3440.065 * Math.asin(Math.sqrt(a));
}

// ---------------------------------------------------------------- routes
type Airport = { code: string; city: string; lat: number; lon: number };
type Leg = { o: Airport; d: Airport };

// Every leg either crowd-sourced database knows for a callsign (cached a day). Both are often
// stale: adsb.lol's VRS standing data is right for most airlines, adsbdb for more Southwest
// flights (their numbers change daily), so collect both and let the plane's position decide.
async function routeLegs(cs: string, ctx: ExecutionContext) {
  return cached(`legs/${cs}`, 86400, ctx, async () => {
    const get = (url: string) =>
      fetch(url, { headers: { "user-agent": UA }, signal: AbortSignal.timeout(UPSTREAM_TIMEOUT) })
        .then((r) => (r.ok ? r.json() : null))
        .catch(() => null) as Promise<any>;
    const [vrs, adsbdb] = await Promise.all([
      get(`https://vrs-standing-data.adsb.lol/routes/${cs.slice(0, 2)}/${cs}.json`),
      get(`https://api.adsbdb.com/v0/callsign/${cs}`),
    ]);
    const legs: Leg[] = [];
    const stops: Airport[] = (vrs?._airports ?? [])
      .filter((a: any) => a.iata && isFinite(a.lat))
      .map((a: any) => ({ code: a.iata, city: a.location ?? "", lat: a.lat, lon: a.lon }));
    for (let i = 0; i + 1 < stops.length; i++) legs.push({ o: stops[i], d: stops[i + 1] });
    const fr = adsbdb?.response?.flightroute;
    if (fr?.origin?.iata_code && fr?.destination?.iata_code) {
      const ap = (a: any): Airport => ({ code: a.iata_code, city: a.municipality ?? "", lat: a.latitude, lon: a.longitude });
      legs.push({ o: ap(fr.origin), d: ap(fr.destination) });
    }
    return { airline: (fr?.airline?.name ?? "") as string, legs };
  }) as Promise<{ airline: string; legs: Leg[] }>;
}

// True if the plane is near either airport or near the great-circle path between them
// (same test as route_fits() in the firmware).
function legFits(leg: Leg, lat: number, lon: number) {
  const R = 3440.065, rad = Math.PI / 180;
  const dist = (la1: number, lo1: number, la2: number, lo2: number) => distNm(la1, lo1, la2, lo2) / R;
  const brg = (la1: number, lo1: number, la2: number, lo2: number) =>
    Math.atan2(
      Math.sin((lo2 - lo1) * rad) * Math.cos(la2 * rad),
      Math.cos(la1 * rad) * Math.sin(la2 * rad) - Math.sin(la1 * rad) * Math.cos(la2 * rad) * Math.cos((lo2 - lo1) * rad),
    );
  const { o, d } = leg;
  const d13 = dist(o.lat, o.lon, lat, lon), d23 = dist(d.lat, d.lon, lat, lon);
  if (d13 * R < 40 || d23 * R < 40) return true;
  const xt = Math.asin(Math.sin(d13) * Math.sin(brg(o.lat, o.lon, lat, lon) - brg(o.lat, o.lon, d.lat, d.lon)));
  const at = Math.acos(Math.min(1, Math.cos(d13) / Math.cos(xt)));
  return Math.abs(xt) * R < 100 && at <= dist(o.lat, o.lon, d.lat, d.lon) + 40 / R;
}

// With the plane's position, the first leg it's actually flying; without, the first known leg.
async function route(callsign: string, lat: number, lon: number, ctx: ExecutionContext) {
  const cs = callsign.toUpperCase().replace(/[^A-Z0-9]/g, "").slice(0, 8);
  const { airline, legs } = await routeLegs(cs, ctx);
  const leg = isFinite(lat) && isFinite(lon) ? legs.find((l) => legFits(l, lat, lon)) : legs[0];
  if (!leg) return { cs, found: false, airline };
  return {
    cs,
    found: true,
    airline,
    o: leg.o.code, oc: leg.o.city, olat: leg.o.lat, olon: leg.o.lon,
    d: leg.d.code, dc: leg.d.city, dlat: leg.d.lat, dlon: leg.d.lon,
  };
}

// ---------------------------------------------------------------- space
const COMPASS = ["N", "NE", "E", "SE", "S", "SW", "W", "NW"];
const compass = (deg: number) => COMPASS[Math.round(((deg % 360) + 360) % 360 / 45) % 8];

async function fetchLaunches(home: { lat: number; lon: number }) {
  try {
    const r = await fetch("https://ll.thespacedevs.com/2.3.0/launches/upcoming/?limit=6&mode=normal", { headers: { "user-agent": UA }, signal: AbortSignal.timeout(UPSTREAM_TIMEOUT) });
    if (!r.ok) throw new Error(`Launch Library HTTP ${r.status}`);
    const d: any = await r.json();
    return (d.results ?? []).map((l: any) => {
      const plat = Number(l.pad?.latitude), plon = Number(l.pad?.longitude);
      const mi = isFinite(plat) ? Math.round(distNm(home.lat, home.lon, plat, plon) * 1.15078) : null;
      return {
        name: l.mission?.name ?? l.name ?? "",
        rocket: l.rocket?.configuration?.name ?? "",
        provider: l.launch_service_provider?.name ?? "",
        pad: l.pad?.name ?? "",
        location: l.pad?.location?.name ?? "",
        t: Math.floor(Date.parse(l.net) / 1000),
        status: l.status?.abbrev ?? "",
        dist_mi: mi,
        // Vandenberg (~245 mi) launches are often visible from San Diego, especially at twilight.
        visible: mi !== null && mi < 400,
      };
    });
  } catch (err) {
    console.warn("Launch Library failed, using RocketLaunch.live", err);
  }
  const r = await fetch("https://fdo.rocketlaunch.live/json/launches/next/5", { headers: { "user-agent": UA }, signal: AbortSignal.timeout(UPSTREAM_TIMEOUT) });
  if (!r.ok) throw new Error(`RocketLaunch.live HTTP ${r.status}`);
  const d: any = await r.json();
  return (d.result ?? []).map((l: any) => {
    const loc = l.pad?.location?.name ?? "";
    const when = l.t0 ?? l.win_open;
    return {
      name: l.name ?? "",
      rocket: l.vehicle?.name ?? "",
      provider: l.provider?.name ?? "",
      pad: l.pad?.name ?? "",
      location: loc,
      t: when ? Math.floor(Date.parse(when) / 1000) : Number(l.sort_date),
      status: l.t0 ? "Go" : "TBD",
      dist_mi: /vandenberg/i.test(loc) ? 245 : null,
      visible: /vandenberg/i.test(loc),
    };
  });
}

async function fetchIssTle() {
  try {
    const r = await fetch("https://celestrak.org/NORAD/elements/gp.php?CATNR=25544&FORMAT=TLE", { headers: { "user-agent": UA }, signal: AbortSignal.timeout(UPSTREAM_TIMEOUT) });
    if (!r.ok) throw new Error(`CelesTrak HTTP ${r.status}`);
    const lines = (await r.text()).trim().split(/\r?\n/).map((x) => x.trim());
    if (lines.length >= 3 && lines[1].startsWith("1 ")) return { l1: lines[1], l2: lines[2] };
    throw new Error("CelesTrak: bad TLE");
  } catch (err) {
    console.warn("CelesTrak failed, trying tle.ivanstanojevic.me", err);
  }
  const r = await fetch("https://tle.ivanstanojevic.me/api/tle/25544", { headers: { "user-agent": UA }, signal: AbortSignal.timeout(UPSTREAM_TIMEOUT) });
  if (!r.ok) throw new Error(`TLE API HTTP ${r.status}`);
  const d: any = await r.json();
  return { l1: d.line1, l2: d.line2 };
}

async function space(env: Env, ctx: ExecutionContext) {
  const home = { lat: Number(env.HOME_LAT), lon: Number(env.HOME_LON) };

  // Launches: Launch Library 2 (rich data, but its 15 req/hour per-IP limit is often
  // already used up on Cloudflare's shared IPs), falling back to RocketLaunch.live,
  // then to the last good list saved in KV.
  let launches: any[] = [];
  try {
    launches = (await cachedOrStale(env, "space/launches", 1800, ctx, () => fetchLaunches(home))) as any[];
  } catch (err) {
    console.warn("launches failed", err);
    launches = (await env.TODOS.get<any[]>("cache:space/launches", "json")) ?? [];
  }

  // ISS orbit elements (TLE): CelesTrak, else tle.ivanstanojevic.me, else the last
  // good copy in KV (a TLE stays accurate enough for days). Cached 6 hours.
  let tle: { l1: string; l2: string } | null = null;
  try {
    tle = (await cachedOrStale(env, "space/iss-tle", 21600, ctx, fetchIssTle)) as { l1: string; l2: string };
  } catch (err) {
    console.warn("ISS TLE failed", err);
    tle = await env.TODOS.get<{ l1: string; l2: string }>("cache:space/iss-tle", "json");
  }
  if (!tle) return { t: Math.floor(Date.now() / 1000), launches, iss: null, iss_pass: null };
  const rec = sat.twoline2satrec(tle.l1, tle.l2);
  const observer = { latitude: sat.degreesToRadians(home.lat), longitude: sat.degreesToRadians(home.lon), height: 0.1 };
  const at = (date: Date) => {
    const pv = sat.propagate(rec, date);
    if (!pv || typeof pv.position === "boolean" || !pv.position) return null;
    const gmst = sat.gstime(date);
    const geo = sat.eciToGeodetic(pv.position, gmst);
    const look = sat.ecfToLookAngles(observer, sat.eciToEcf(pv.position, gmst));
    return { lat: sat.degreesLat(geo.latitude), lon: sat.degreesLong(geo.longitude), alt: geo.height, el: sat.radiansToDegrees(look.elevation), az: sat.radiansToDegrees(look.azimuth) };
  };
  const now = new Date();
  const p = at(now);
  const iss = p ? { lat: Number(p.lat.toFixed(3)), lon: Number(p.lon.toFixed(3)), alt_km: Math.round(p.alt), overhead: p.el > 10 } : null;

  // Next good pass (max elevation 20+ degrees) in the next 48 h, 2-minute steps,
  // cached 10 minutes. Falls back to the next pass above 10 degrees if none is good.
  const pass = await cached(`space/iss-pass/${Math.floor(now.getTime() / 600000)}`, 600, ctx, async () => {
    let start: number | null = null, maxEl = 0, startAz = 0, endAz = 0;
    let fallback: unknown = null;
    for (let m = 0; m < 48 * 60; m += 2) {
      const t = new Date(now.getTime() + m * 60000);
      const s = at(t);
      if (!s) continue;
      if (s.el > 10) {
        if (start === null) { start = t.getTime(); startAz = s.az; maxEl = 0; }
        if (s.el > maxEl) maxEl = s.el;
        endAz = s.az;
      } else if (start !== null) {
        const p = { t: Math.floor(start / 1000), dur_min: Math.max(1, Math.round((t.getTime() - start) / 60000)), max_el: Math.round(maxEl), from: compass(startAz), to: compass(endAz) };
        if (maxEl >= 20) return p;
        fallback ??= p;
        start = null;
      }
    }
    return fallback;
  });

  // Ground track for the mini map: 30 min back to 95 min ahead (about one orbit).
  const track: [number, number][] = [];
  for (let m = -30; m <= 95; m += 2.5) {
    const s = at(new Date(now.getTime() + m * 60000));
    if (s) track.push([Number(s.lon.toFixed(1)), Number(s.lat.toFixed(1))]);
  }

  return { t: Math.floor(Date.now() / 1000), launches, iss, iss_pass: pass, iss_track: track };
}

// ---------------------------------------------------------------- ships
async function ships(env: Env, region: string) {
  if (!REGIONS[region]) region = "sandiego";
  // One Durable Object for every region: aisstream gets a single connection.
  const stub = env.SHIPS.get(env.SHIPS.idFromName("sandiego"));
  const { vessels } = await stub.list(region);
  const { lat, lon } = REGIONS[region];
  const now = Date.now();
  const rows = vessels
    .map((v) => ({ ...v, dist: distNm(lat, lon, v.lat, v.lon), moving: v.sog >= 0.5 && v.sog < 102.2 }))
    // Moving ships first (nearest first), then moored ones: San Diego Bay alone has
    // more than 80 moored boats, which would otherwise crowd out everything offshore.
    // Distances are from the region center.
    .sort((a, b) => Number(b.moving) - Number(a.moving) || a.dist - b.dist)
    .slice(0, 80)
    .map((v) => ({
      mmsi: v.mmsi, name: v.name || `MMSI ${v.mmsi}`, kind: shipTypeName(v.type), type: v.type,
      lat: Number(v.lat.toFixed(5)), lon: Number(v.lon.toFixed(5)), sog: v.sog >= 102.2 ? 0 : Number(v.sog.toFixed(1)), cog: Math.round(v.cog),
      hdg: v.hdg, len: v.len, dest: v.dest, dist_nm: Number(v.dist.toFixed(1)), age_s: Math.round((now - v.seen) / 1000),
    }));
  return { t: Math.floor(now / 1000), region, total: vessels.length, ships: rows };
}

// ---------------------------------------------------------------- to-dos
// Stored in the TodoStore Durable Object (src/todos.ts); one instance holds the list.
const todoStore = (env: Env) => env.TODO_STORE.get(env.TODO_STORE.idFromName("main"));

const todoLine = (t: Todo) => `${t.done ? "[x]" : "[ ]"} ${t.text} (id ${t.id})`;

// ---------------------------------------------------------------- MCP server
function mcpServer(env: Env) {
  return () => {
    const server = new McpServer({ name: "skytile", version: "1.0.0" });
    const text = (s: string) => ({ content: [{ type: "text" as const, text: s }] });

    server.registerTool(
      "list_todos",
      { description: "List the to-do items shown on James's SkyTile desk display. Checked-off items disappear 24 hours after being checked off." },
      async () => {
        const todos = await todoStore(env).list();
        return text(todos.length ? todos.map(todoLine).join("\n") : "The to-do list is empty.");
      },
    );
    server.registerTool(
      "add_todo",
      { description: "Add a to-do item to the SkyTile desk display. Keep it short; it is shown on a small screen.", inputSchema: { text: z.string().min(1).max(120) } },
      async ({ text: item }) => {
        const t = await todoStore(env).add(item, "mcp");
        return text(`Added: ${t.text} (id ${t.id})`);
      },
    );
    server.registerTool(
      "complete_todo",
      { description: "Mark a to-do item done (or not done). Identify it by id or by its text.", inputSchema: { item: z.string(), done: z.boolean().optional() } },
      async ({ item, done }) => {
        const t = await todoStore(env).completeMatch(item, done ?? true);
        if (!t) return text(`No to-do matches "${item}".`);
        return text(`${t.done ? "Done" : "Reopened"}: ${t.text}`);
      },
    );
    server.registerTool(
      "edit_todo",
      { description: "Change the text of a to-do item. Identify it by id or by its current text.", inputSchema: { item: z.string(), text: z.string().min(1).max(120) } },
      async ({ item, text: newText }) => {
        const t = await todoStore(env).editMatch(item, newText);
        if (!t) return text(`No to-do matches "${item}".`);
        return text(`Edited: ${t.text} (id ${t.id})`);
      },
    );
    server.registerTool(
      "remove_todo",
      { description: "Delete a to-do item. Identify it by id or by its text.", inputSchema: { item: z.string() } },
      async ({ item }) => {
        const t = await todoStore(env).removeMatch(item);
        if (!t) return text(`No to-do matches "${item}".`);
        return text(`Removed: ${t.text}`);
      },
    );
    server.registerTool(
      "clear_completed",
      { description: "Delete all to-do items that are marked done." },
      async () => {
        const n = await todoStore(env).clearCompleted();
        return text(`Cleared ${n} completed item(s).`);
      },
    );
    return server;
  };
}

// ---------------------------------------------------------------- to-do HTTP APIs
// Constant-time compare for the API token.
function safeEqual(a: string, b: string) {
  const x = new TextEncoder().encode(a), y = new TextEncoder().encode(b);
  if (x.length !== y.length) return false;
  let d = 0;
  for (let i = 0; i < x.length; i++) d |= x[i] ^ y[i];
  return d === 0;
}

// Body text: JSON {"text": "..."} or plain text (Siri Shortcut).
async function bodyFields(request: Request): Promise<{ text: string; source?: string }> {
  const raw = await request.text();
  try {
    const b = JSON.parse(raw);
    if (b && typeof b === "object") return { text: String(b.text ?? "").trim(), source: typeof b.source === "string" ? b.source : undefined };
  } catch {}
  return { text: raw.trim() };
}

// The tile's /todos and the web app's /t/<WEB_SECRET>/api/todos (unchanged contract):
//   GET "" list, POST "" add, POST /clear, POST /:id/done toggle, DELETE /:id
async function legacyTodos(request: Request, env: Env, sub: string, source: string, out: (d: unknown, s?: number) => Response) {
  const store = todoStore(env);
  if (sub === "" && request.method === "GET") return out({ todos: await store.list() });
  if (sub === "" && request.method === "POST") {
    const { text } = await bodyFields(request);
    if (!text) return json({ error: "text required" }, 400);
    return json(await store.add(text, source), 201);
  }
  if (sub === "/clear" && request.method === "POST") {
    await store.clearCompleted();
    return json({ ok: true });
  }
  const m = sub.match(/^\/([^/]+)(\/done)?$/);
  if (m && m[2] && request.method === "POST") {
    const t = await store.setDone(m[1]);
    return t ? json(t) : json({ error: "not found" }, 404);
  }
  if (m && !m[2] && request.method === "DELETE") return (await store.remove(m[1])) ? json({ ok: true }) : json({ error: "not found" }, 404);
  return json({ error: "not found" }, 404);
}

// /api/v1/todos (Bearer API_TOKEN):
//   GET    /api/v1/todos                      {todos: [...]}, open items first, then by position
//   POST   /api/v1/todos {text, source?}      201 {todo}
//   POST   /api/v1/todos/:id/complete         {todo}
//   POST   /api/v1/todos/:id/uncomplete       {todo}
//   PATCH  /api/v1/todos/:id {text}           {todo}
//   DELETE /api/v1/todos/:id                  {ok: true}
//   POST   /api/v1/todos/clear-completed      {cleared: n}
const SOURCES = new Set(["mac", "ios", "widget", "siri", "web", "api"]);
async function apiV1(request: Request, env: Env, sub: string) {
  const store = todoStore(env);
  const method = request.method;
  if (sub === "" && method === "GET") {
    const todos = await store.list();
    return json({ todos: [...todos.filter((t) => !t.done), ...todos.filter((t) => t.done)] });
  }
  if (sub === "" && method === "POST") {
    const { text, source } = await bodyFields(request);
    if (!text) return json({ error: "text required" }, 400);
    if (text.length > MAX_TEXT) return json({ error: `text longer than ${MAX_TEXT} characters` }, 400);
    return json({ todo: await store.add(text, source && SOURCES.has(source) ? source : "api") }, 201);
  }
  if (sub === "/clear-completed" && method === "POST") return json({ cleared: await store.clearCompleted() });
  const m = sub.match(/^\/([0-9a-z]{1,16})(?:\/(complete|uncomplete))?$/);
  if (m && m[2] && method === "POST") {
    const t = await store.setDone(m[1], m[2] === "complete");
    return t ? json({ todo: t }) : json({ error: "not found" }, 404);
  }
  if (m && !m[2] && method === "PATCH") {
    const { text } = await bodyFields(request);
    if (!text) return json({ error: "text required" }, 400);
    if (text.length > MAX_TEXT) return json({ error: `text longer than ${MAX_TEXT} characters` }, 400);
    const t = await store.edit(m[1], text);
    return t ? json({ todo: t }) : json({ error: "not found" }, 404);
  }
  if (m && !m[2] && method === "DELETE") return (await store.remove(m[1])) ? json({ ok: true }) : json({ error: "not found" }, 404);
  return json({ error: "not found" }, 404);
}

// ---------------------------------------------------------------- router
export default {
  async fetch(request, env, ctx): Promise<Response> {
    const url = new URL(request.url);
    const path = url.pathname;

    // MCP: the secret lives in the path. Rewrite to /mcp for the handler.
    if (path.startsWith("/mcp/")) {
      if (!env.MCP_SECRET || path !== `/mcp/${env.MCP_SECRET}`) return new Response("Not found", { status: 404 });
      const inner = new URL(request.url);
      inner.pathname = "/mcp";
      return createMcpHandler(mcpServer(env), { route: "/mcp" })(new Request(inner, request), env, ctx);
    }

    // To-do web app + Siri Shortcut: the secret in the path is the credential.
    if (path.startsWith("/t/")) {
      if (!env.WEB_SECRET) return new Response("Not found", { status: 404 });
      const base = `/t/${env.WEB_SECRET}`;
      if (path === base || path === base + "/")
        return new Response(todoPage(), { headers: { "content-type": "text/html; charset=utf-8", "cache-control": "no-store", "x-robots-tag": "noindex" } });
      if (!path.startsWith(base + "/api/todos")) return new Response("Not found", { status: 404 });
      return legacyTodos(request, env, path.slice((base + "/api/todos").length), "web", json);
    }

    // Client API for the Mac menu bar, iPhone app/widget and scripts: Bearer API_TOKEN.
    if (path === "/api/v1/todos" || path.startsWith("/api/v1/todos/")) {
      if (!env.API_TOKEN || !safeEqual(request.headers.get("authorization") ?? "", `Bearer ${env.API_TOKEN}`))
        return json({ error: "unauthorized" }, 401);
      return apiV1(request, env, path.slice("/api/v1/todos".length));
    }

    if (!env.TILE_KEY || request.headers.get("x-tile-key") !== env.TILE_KEY) return json({ error: "unauthorized" }, 401);

    try {
      if (path.startsWith("/route/")) {
        const q = url.searchParams;
        return tileJson(await route(decodeURIComponent(path.slice(7)), parseFloat(q.get("lat") ?? ""), parseFloat(q.get("lon") ?? ""), ctx));
      }
      if (path === "/space") return tileJson(await space(env, ctx));
      if (path === "/ships") return tileJson(await ships(env, url.searchParams.get("r") ?? "sandiego"));
      if (path === "/todos" || path.startsWith("/todos/")) return await legacyTodos(request, env, path.slice(6), "tile", tileJson);
      return json({ error: "not found" }, 404);
    } catch (err) {
      console.error(path, err);
      return json({ error: String((err as Error).message ?? err) }, 502);
    }
  },
} satisfies ExportedHandler<Env>;
