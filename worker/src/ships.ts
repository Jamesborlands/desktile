// Ships from aisstream.io, for San Diego and a few world hotspots.
// aisstream is a firehose WebSocket, too heavy for the ESP32. This Durable Object
// connects for ~25 s once a minute and keeps a vessel cache the tile can poll. It only
// subscribes to the regions the tile has asked for in the last 15 minutes, all on one
// connection (one bounding box per region).
import { DurableObject } from "cloudflare:workers";

type Vessel = {
  mmsi: number;
  name: string;
  type: number; // AIS ship type code (0 = unknown)
  lat: number;
  lon: number;
  sog: number; // knots
  cog: number; // degrees
  hdg: number; // degrees, 511 = unknown
  len: number; // meters (0 = unknown)
  dest: string;
  seen: number; // unix ms of last position
};

// Hotspots. The tile has the same ids (firmware tools/hotspotgen.py). r = box half-size, nm.
export type Region = { lat: number; lon: number; r: number };
export const REGIONS: Record<string, Region> = {
  sandiego: { lat: 32.95, lon: -117.25, r: 0 }, // box below (mostly offshore)
  hormuz: { lat: 26.35, lon: 56.45, r: 90 },
  gulf: { lat: 26.5, lon: 52.0, r: 260 },
  dover: { lat: 51.0, lon: 1.45, r: 60 },
  rotterdam: { lat: 51.95, lon: 4.0, r: 35 },
  singapore: { lat: 1.25, lon: 103.9, r: 55 },
  hongkong: { lat: 22.3, lon: 114.1, r: 45 },
  taiwan: { lat: 24.4, lon: 119.6, r: 160 },
  tokyobay: { lat: 35.4, lon: 139.8, r: 50 },
  pugetsound: { lat: 47.8, lon: -122.5, r: 60 },
  sfbay: { lat: 37.8, lon: -122.4, r: 45 },
  nyc: { lat: 40.6, lon: -74.0, r: 45 },
};

// [[minLat, minLon], [maxLat, maxLon]] as aisstream wants it.
export function regionBox(id: string): [[number, number], [number, number]] {
  const g = REGIONS[id];
  if (id === "sandiego") return [[g.lat - 1.25, g.lon - 1.4], [g.lat + 0.75, g.lon + 0.35]]; // ~75 nm, offshore
  const dLat = g.r / 60, dLon = g.r / (60 * Math.cos((g.lat * Math.PI) / 180));
  return [[g.lat - dLat, g.lon - dLon], [g.lat + dLat, g.lon + dLon]];
}

const inBox = (b: [[number, number], [number, number]], lat: number, lon: number) =>
  lat >= b[0][0] && lat <= b[1][0] && lon >= b[0][1] && lon <= b[1][1];

const COLLECT_MS = 25_000;
const EVERY_MS = 60_000;
const ACTIVE_MS = 15 * 60_000; // keep collecting this long after the last request
const STALE_MS = 30 * 60_000; // drop vessels not heard from for this long
const MAX_VESSELS = 4000; // storage cap across all regions (busy straits have ~1000 each)

export class Ships extends DurableObject<Env> {
  private vessels = new Map<number, Vessel>();
  private loaded = false;

  private async load() {
    if (this.loaded) return;
    const saved = await this.ctx.storage.get<Vessel[]>("vessels");
    for (const v of saved ?? []) this.vessels.set(v.mmsi, v);
    this.loaded = true;
  }

  // Region id -> unix ms of the tile's last request for it.
  private async activeRegions(): Promise<Record<string, number>> {
    return (await this.ctx.storage.get<Record<string, number>>("lastRequests")) ?? {};
  }

  // Called by the Worker for GET /ships?r=<region>.
  async list(region: string): Promise<{ vessels: Vessel[]; collecting: boolean }> {
    await this.load();
    const active = await this.activeRegions();
    const isNew = !active[region] || Date.now() - active[region] > ACTIVE_MS;
    active[region] = Date.now();
    await this.ctx.storage.put("lastRequests", active);
    const alarm = await this.ctx.storage.getAlarm();
    // A region nobody was watching: collect right away instead of waiting up to a minute.
    if (alarm === null || (isNew && !this.collecting)) await this.ctx.storage.setAlarm(Date.now());
    const now = Date.now();
    const box = regionBox(region);
    const vessels = [...this.vessels.values()].filter((v) => now - v.seen < STALE_MS && inBox(box, v.lat, v.lon));
    return { vessels, collecting: true };
  }

  private collecting = false;

  async alarm() {
    await this.load();
    const now0 = Date.now();
    const regions = Object.entries(await this.activeRegions())
      .filter(([id, t]) => REGIONS[id] && now0 - t < ACTIVE_MS)
      .map(([id]) => id);
    if (!regions.length) return; // nobody is watching: stop until next request
    this.collecting = true;
    try {
      await this.collect(regions);
    } catch (err) {
      console.error("aisstream collect failed", err);
    } finally {
      this.collecting = false;
    }
    const now = Date.now();
    for (const [k, v] of this.vessels) if (now - v.seen > STALE_MS) this.vessels.delete(k);
    if (this.vessels.size > MAX_VESSELS) {
      const keep = [...this.vessels.values()].sort((a, b) => b.seen - a.seen).slice(0, MAX_VESSELS);
      this.vessels = new Map(keep.map((v) => [v.mmsi, v]));
    }
    await this.ctx.storage.put("vessels", [...this.vessels.values()]);
    // A region first asked for while we were collecting wasn't in this subscription: go again now.
    const missed = Object.entries(await this.activeRegions()).some(
      ([id, t]) => REGIONS[id] && Date.now() - t < ACTIVE_MS && !regions.includes(id),
    );
    await this.ctx.storage.setAlarm(Date.now() + (missed ? 0 : EVERY_MS));
  }

  private async collect(regions: string[]) {
    if (!this.env.AISSTREAM_KEY) throw new Error("AISSTREAM_KEY secret not set");
    const resp = await fetch("https://stream.aisstream.io/v0/stream", { headers: { Upgrade: "websocket" } });
    const ws = resp.webSocket;
    if (!ws) throw new Error(`aisstream upgrade failed: HTTP ${resp.status}`);
    ws.accept();
    console.log(`aisstream connected: HTTP ${resp.status}`);
    ws.send(
      JSON.stringify({
        APIKey: this.env.AISSTREAM_KEY,
        BoundingBoxes: regions.map(regionBox),
        FilterMessageTypes: ["PositionReport", "StandardClassBPositionReport", "ShipStaticData"],
      }),
    );
    let count = 0;
    await new Promise<void>((resolve) => {
      const done = () => {
        try { ws.close(1000, "done"); } catch {}
        resolve();
      };
      const timer = setTimeout(done, COLLECT_MS);
      ws.addEventListener("message", async (ev) => {
        // aisstream sends JSON as binary frames; depending on the runtime they arrive as
        // a string, an ArrayBuffer, a typed array or a Blob.
        const d: any = ev.data;
        const text = typeof d === "string" ? d
          : d instanceof ArrayBuffer || ArrayBuffer.isView(d) ? new TextDecoder().decode(d)
          : typeof d?.text === "function" ? await d.text() : String(d);
        count++;
        try {
          this.ingest(JSON.parse(text));
        } catch (err) {
          console.warn("aisstream bad message", String(err));
        }
      });
      ws.addEventListener("close", (ev) => { console.log(`aisstream closed: ${ev.code} ${ev.reason}`); clearTimeout(timer); resolve(); });
      ws.addEventListener("error", (ev) => { console.log("aisstream error", String((ev as any).message ?? ev)); clearTimeout(timer); resolve(); });
    });
    console.log(`aisstream [${regions.join(",")}]: ${count} messages, ${this.vessels.size} vessels`);
  }

  private ingest(msg: any) {
    if (msg.error) throw new Error(`aisstream: ${msg.error}`);
    const meta = msg.MetaData ?? {};
    const mmsi = Number(meta.MMSI);
    if (!mmsi) return;
    const v: Vessel = this.vessels.get(mmsi) ?? {
      mmsi, name: "", type: 0, lat: 0, lon: 0, sog: 0, cog: 0, hdg: 511, len: 0, dest: "", seen: 0,
    };
    if (meta.ShipName) v.name = String(meta.ShipName).trim();
    const body = msg.Message ?? {};
    const pos = body.PositionReport ?? body.StandardClassBPositionReport;
    if (pos) {
      v.lat = pos.Latitude ?? meta.latitude;
      v.lon = pos.Longitude ?? meta.longitude;
      v.sog = pos.Sog >= 102.2 ? 0 : pos.Sog ?? 0; // 102.3 kn is AIS for "speed not available"
      v.cog = pos.Cog ?? 0;
      v.hdg = pos.TrueHeading ?? 511;
      v.seen = Date.now();
    }
    const st = body.ShipStaticData;
    if (st) {
      v.type = st.Type ?? v.type;
      const d = st.Dimension ?? {};
      v.len = (d.A ?? 0) + (d.B ?? 0) || v.len;
      v.dest = String(st.Destination ?? "").trim() || v.dest;
      if (st.Name) v.name = String(st.Name).trim();
    }
    if (v.seen) this.vessels.set(mmsi, v); // only keep vessels with a position
  }
}

export function shipTypeName(t: number): string {
  if (t === 30) return "Fishing";
  if (t === 31 || t === 32 || t === 52) return "Tug";
  if (t === 35) return "Military";
  if (t === 36) return "Sailing";
  if (t === 37) return "Pleasure craft";
  if (t >= 40 && t <= 49) return "High-speed craft";
  if (t === 50) return "Pilot boat";
  if (t === 51) return "Search and rescue";
  if (t === 55) return "Law enforcement";
  if (t >= 60 && t <= 69) return "Passenger";
  if (t >= 70 && t <= 79) return "Cargo";
  if (t >= 80 && t <= 89) return "Tanker";
  return "Vessel";
}
