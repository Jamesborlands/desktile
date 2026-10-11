// SkyTile Worker: compact data feeds for the ESP32 tile (planes come straight from
// adsb.lol on the tile: it rate-limits Cloudflare's shared IPs), the to-do list, and an
// MCP server so Claude can manage the to-do list.
//
//   GET  /route/:callsign        adsbdb origin/destination (cached a day)
//   GET  /space                  next launches + ISS position and next pass
//   GET  /ships                  vessels near San Diego (aisstream.io via Durable Object)
//   GET  /todos                  list; POST /todos {text}; POST /todos/:id/done; DELETE /todos/:id
//   *    /mcp/<MCP_SECRET>       MCP server (Streamable HTTP) for Claude
//   GET  /t/<WEB_SECRET>         to-do web app for the phone; its API (and the Siri
//                                Shortcut) lives under /t/<WEB_SECRET>/api/todos
//
// Everything except /mcp needs the X-Tile-Key header (secret TILE_KEY).
import { McpServer } from "@modelcontextprotocol/server";
import { createMcpHandler } from "agents/mcp/server";
import * as sat from "satellite.js";
import { z } from "zod";
import { Ships, shipTypeName } from "./ships";
import { todoPage } from "./webapp";

export { Ships };

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

function distNm(lat1: number, lon1: number, lat2: number, lon2: number) {
  const r = Math.PI / 180;
  const a = Math.sin(((lat2 - lat1) * r) / 2) ** 2 + Math.cos(lat1 * r) * Math.cos(lat2 * r) * Math.sin(((lon2 - lon1) * r) / 2) ** 2;
  return 2 * 3440.065 * Math.asin(Math.sqrt(a));
}

// ---------------------------------------------------------------- routes
async function route(callsign: string, ctx: ExecutionContext) {
  const cs = callsign.toUpperCase().replace(/[^A-Z0-9]/g, "").slice(0, 8);
  return cached(`route/${cs}`, 86400, ctx, async () => {
    const r = await fetch(`https://api.adsbdb.com/v0/callsign/${cs}`, { headers: { "user-agent": UA } });
    if (!r.ok) return { cs, found: false };
    const fr = ((await r.json()) as any)?.response?.flightroute;
    if (!fr?.origin?.iata_code || !fr?.destination?.iata_code) return { cs, found: false };
    return {
      cs,
      found: true,
      airline: fr.airline?.name ?? "",
      o: fr.origin.iata_code, oc: fr.origin.municipality ?? "", olat: fr.origin.latitude, olon: fr.origin.longitude,
      d: fr.destination.iata_code, dc: fr.destination.municipality ?? "", dlat: fr.destination.latitude, dlon: fr.destination.longitude,
    };
  });
}

// ---------------------------------------------------------------- space
const COMPASS = ["N", "NE", "E", "SE", "S", "SW", "W", "NW"];
const compass = (deg: number) => COMPASS[Math.round(((deg % 360) + 360) % 360 / 45) % 8];

async function fetchLaunches(home: { lat: number; lon: number }) {
  try {
    const r = await fetch("https://ll.thespacedevs.com/2.3.0/launches/upcoming/?limit=6&mode=normal", { headers: { "user-agent": UA } });
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
  const r = await fetch("https://fdo.rocketlaunch.live/json/launches/next/5", { headers: { "user-agent": UA } });
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
    const r = await fetch("https://celestrak.org/NORAD/elements/gp.php?CATNR=25544&FORMAT=TLE", { headers: { "user-agent": UA } });
    if (!r.ok) throw new Error(`CelesTrak HTTP ${r.status}`);
    const lines = (await r.text()).trim().split(/\r?\n/).map((x) => x.trim());
    if (lines.length >= 3 && lines[1].startsWith("1 ")) return { l1: lines[1], l2: lines[2] };
    throw new Error("CelesTrak: bad TLE");
  } catch (err) {
    console.warn("CelesTrak failed, trying tle.ivanstanojevic.me", err);
  }
  const r = await fetch("https://tle.ivanstanojevic.me/api/tle/25544", { headers: { "user-agent": UA } });
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
    launches = (await cached("space/launches", 1800, ctx, () => fetchLaunches(home))) as any[];
    ctx.waitUntil(env.TODOS.put("cache:launches", JSON.stringify(launches)));
  } catch (err) {
    console.warn("launches failed", err);
    launches = (await env.TODOS.get<any[]>("cache:launches", "json")) ?? [];
  }

  // ISS orbit elements (TLE): CelesTrak, else tle.ivanstanojevic.me, else the last
  // good copy in KV (a TLE stays accurate enough for days). Cached 6 hours.
  let tle: { l1: string; l2: string } | null = null;
  try {
    tle = (await cached("space/iss-tle", 21600, ctx, fetchIssTle)) as { l1: string; l2: string };
    ctx.waitUntil(env.TODOS.put("cache:iss-tle", JSON.stringify(tle)));
  } catch (err) {
    console.warn("ISS TLE failed", err);
    tle = await env.TODOS.get<{ l1: string; l2: string }>("cache:iss-tle", "json");
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
async function ships(env: Env) {
  const stub = env.SHIPS.get(env.SHIPS.idFromName("sandiego"));
  const { vessels } = await stub.list();
  const lat = Number(env.HOME_LAT), lon = Number(env.HOME_LON);
  const now = Date.now();
  const rows = vessels
    .map((v) => ({ ...v, dist: distNm(lat, lon, v.lat, v.lon), moving: v.sog >= 0.5 && v.sog < 102.2 }))
    // Moving ships first (nearest first), then moored ones: San Diego Bay alone has
    // more than 80 moored boats, which would otherwise crowd out everything offshore.
    .sort((a, b) => Number(b.moving) - Number(a.moving) || a.dist - b.dist)
    .slice(0, 80)
    .map((v) => ({
      mmsi: v.mmsi, name: v.name || `MMSI ${v.mmsi}`, kind: shipTypeName(v.type), type: v.type,
      lat: Number(v.lat.toFixed(5)), lon: Number(v.lon.toFixed(5)), sog: v.sog >= 102.2 ? 0 : Number(v.sog.toFixed(1)), cog: Math.round(v.cog),
      hdg: v.hdg, len: v.len, dest: v.dest, dist_nm: Number(v.dist.toFixed(1)), age_s: Math.round((now - v.seen) / 1000),
    }));
  return { t: Math.floor(now / 1000), total: vessels.length, ships: rows };
}

// ---------------------------------------------------------------- to-dos
type Todo = { id: string; text: string; done: boolean; created: number; doneAt?: number };

// Checked-off items disappear 24 hours after they were checked off.
const DONE_TTL_MS = 24 * 60 * 60 * 1000;

function setDone(t: Todo, done: boolean) {
  t.done = done;
  if (done) t.doneAt = Date.now();
  else delete t.doneAt;
}

async function getTodos(env: Env): Promise<Todo[]> {
  const todos = (await env.TODOS.get<Todo[]>("todos", "json")) ?? [];
  const now = Date.now();
  let changed = false;
  for (const t of todos)
    if (t.done && !t.doneAt) { t.doneAt = now; changed = true; }  // checked off before timestamps existed
  const kept = todos.filter((t) => !(t.done && now - (t.doneAt ?? now) > DONE_TTL_MS));
  if (changed || kept.length !== todos.length) await putTodos(env, kept);
  return kept;
}
async function putTodos(env: Env, todos: Todo[]) {
  await env.TODOS.put("todos", JSON.stringify(todos));
}
async function addTodo(env: Env, text: string) {
  const todos = await getTodos(env);
  const t: Todo = { id: crypto.randomUUID().slice(0, 8), text: text.trim().slice(0, 120), done: false, created: Date.now() };
  todos.push(t);
  await putTodos(env, todos);
  return t;
}
// Find by id, or by case-insensitive text match (exact first, then substring).
function findTodo(todos: Todo[], ref: string) {
  const r = ref.trim().toLowerCase();
  return todos.find((t) => t.id === ref) ?? todos.find((t) => t.text.toLowerCase() === r) ?? todos.find((t) => t.text.toLowerCase().includes(r));
}

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
        const todos = await getTodos(env);
        return text(todos.length ? todos.map(todoLine).join("\n") : "The to-do list is empty.");
      },
    );
    server.registerTool(
      "add_todo",
      { description: "Add a to-do item to the SkyTile desk display. Keep it short; it is shown on a small screen.", inputSchema: { text: z.string().min(1).max(120) } },
      async ({ text: item }) => {
        const t = await addTodo(env, item);
        return text(`Added: ${t.text} (id ${t.id})`);
      },
    );
    server.registerTool(
      "complete_todo",
      { description: "Mark a to-do item done (or not done). Identify it by id or by its text.", inputSchema: { item: z.string(), done: z.boolean().optional() } },
      async ({ item, done }) => {
        const todos = await getTodos(env);
        const t = findTodo(todos, item);
        if (!t) return text(`No to-do matches "${item}".`);
        setDone(t, done ?? true);
        await putTodos(env, todos);
        return text(`${t.done ? "Done" : "Reopened"}: ${t.text}`);
      },
    );
    server.registerTool(
      "remove_todo",
      { description: "Delete a to-do item. Identify it by id or by its text.", inputSchema: { item: z.string() } },
      async ({ item }) => {
        const todos = await getTodos(env);
        const t = findTodo(todos, item);
        if (!t) return text(`No to-do matches "${item}".`);
        await putTodos(env, todos.filter((x) => x !== t));
        return text(`Removed: ${t.text}`);
      },
    );
    server.registerTool(
      "clear_completed",
      { description: "Delete all to-do items that are marked done." },
      async () => {
        const todos = await getTodos(env);
        const left = todos.filter((t) => !t.done);
        await putTodos(env, left);
        return text(`Cleared ${todos.length - left.length} completed item(s).`);
      },
    );
    return server;
  };
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
      const sub = path.slice((base + "/api/todos").length);
      if (sub === "" && request.method === "GET") return json({ todos: await getTodos(env) });
      if (sub === "" && request.method === "POST") {
        // JSON {"text": "..."} from the web app, or plain text from a Shortcut
        const raw = await request.text();
        let text = raw;
        try { text = String(JSON.parse(raw).text ?? ""); } catch {}
        text = text.trim();
        if (!text) return json({ error: "text required" }, 400);
        return json(await addTodo(env, text), 201);
      }
      if (sub === "/clear" && request.method === "POST") {
        await putTodos(env, (await getTodos(env)).filter((t) => !t.done));
        return json({ ok: true });
      }
      const m = sub.match(/^\/([^/]+)(\/done)?$/);
      if (m) {
        const todos = await getTodos(env);
        const t = todos.find((x) => x.id === m[1]);
        if (!t) return json({ error: "not found" }, 404);
        if (m[2] && request.method === "POST") { setDone(t, !t.done); await putTodos(env, todos); return json(t); }
        if (!m[2] && request.method === "DELETE") { await putTodos(env, todos.filter((x) => x !== t)); return json({ ok: true }); }
      }
      return json({ error: "not found" }, 404);
    }

    if (!env.TILE_KEY || request.headers.get("x-tile-key") !== env.TILE_KEY) return json({ error: "unauthorized" }, 401);

    try {
      if (path.startsWith("/route/")) return tileJson(await route(decodeURIComponent(path.slice(7)), ctx));
      if (path === "/space") return tileJson(await space(env, ctx));
      if (path === "/ships") return tileJson(await ships(env));
      if (path === "/todos" && request.method === "GET") return tileJson({ todos: await getTodos(env) });
      if (path === "/todos" && request.method === "POST") {
        const body: any = await request.json().catch(() => ({}));
        if (!body.text) return json({ error: "text required" }, 400);
        return json(await addTodo(env, String(body.text)), 201);
      }
      const m = path.match(/^\/todos\/([^/]+)(\/done)?$/);
      if (m) {
        const todos = await getTodos(env);
        const t = todos.find((x) => x.id === m[1]);
        if (!t) return json({ error: "not found" }, 404);
        if (m[2] && request.method === "POST") { setDone(t, !t.done); await putTodos(env, todos); return json(t); }
        if (!m[2] && request.method === "DELETE") { await putTodos(env, todos.filter((x) => x !== t)); return json({ ok: true }); }
      }
      return json({ error: "not found" }, 404);
    } catch (err) {
      console.error(path, err);
      return json({ error: String((err as Error).message ?? err) }, 502);
    }
  },
} satisfies ExportedHandler<Env>;
