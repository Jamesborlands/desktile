// Ships near San Diego from aisstream.io.
// aisstream is a firehose WebSocket, too heavy for the ESP32. This Durable Object
// connects for ~25 s once a minute, but only while the tile has asked for ships in
// the last 15 minutes, and keeps a small vessel cache the tile can poll.
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

const COLLECT_MS = 25_000;
const EVERY_MS = 60_000;
const ACTIVE_MS = 15 * 60_000; // keep collecting this long after the last request
const STALE_MS = 30 * 60_000; // drop vessels not heard from for this long

export class Ships extends DurableObject<Env> {
  private vessels = new Map<number, Vessel>();
  private loaded = false;

  private async load() {
    if (this.loaded) return;
    const saved = await this.ctx.storage.get<Vessel[]>("vessels");
    for (const v of saved ?? []) this.vessels.set(v.mmsi, v);
    this.loaded = true;
  }

  // Called by the Worker for GET /ships.
  async list(): Promise<{ vessels: Vessel[]; collecting: boolean }> {
    await this.load();
    await this.ctx.storage.put("lastRequest", Date.now());
    if ((await this.ctx.storage.getAlarm()) === null) await this.ctx.storage.setAlarm(Date.now());
    const now = Date.now();
    const vessels = [...this.vessels.values()].filter((v) => now - v.seen < STALE_MS);
    return { vessels, collecting: true };
  }

  async alarm() {
    await this.load();
    const last = (await this.ctx.storage.get<number>("lastRequest")) ?? 0;
    if (Date.now() - last > ACTIVE_MS) return; // nobody is watching: stop until next request
    try {
      await this.collect();
    } catch (err) {
      console.error("aisstream collect failed", err);
    }
    const now = Date.now();
    for (const [k, v] of this.vessels) if (now - v.seen > STALE_MS) this.vessels.delete(k);
    await this.ctx.storage.put("vessels", [...this.vessels.values()]);
    await this.ctx.storage.setAlarm(Date.now() + EVERY_MS);
  }

  private async collect() {
    if (!this.env.AISSTREAM_KEY) throw new Error("AISSTREAM_KEY secret not set");
    const lat = Number(this.env.HOME_LAT), lon = Number(this.env.HOME_LON);
    const resp = await fetch("https://stream.aisstream.io/v0/stream", { headers: { Upgrade: "websocket" } });
    const ws = resp.webSocket;
    if (!ws) throw new Error(`aisstream upgrade failed: HTTP ${resp.status}`);
    ws.accept();
    console.log(`aisstream connected: HTTP ${resp.status}`);
    ws.send(
      JSON.stringify({
        APIKey: this.env.AISSTREAM_KEY,
        // ~75 nm box, mostly offshore of San Diego
        BoundingBoxes: [[[lat - 1.25, lon - 1.4], [lat + 0.75, lon + 0.35]]],
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
    console.log(`aisstream: ${count} messages, ${this.vessels.size} vessels`);
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
