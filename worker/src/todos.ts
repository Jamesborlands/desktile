// To-do list: one Durable Object with SQLite storage is the single source of truth for
// the tile, the MCP server, the phone web app, the Mac menu bar and the iPhone widget.
// Every change runs inside the object, one at a time, so two clients writing at once
// can't overwrite each other, and reads always see the latest write (KV could lag ~60 s).
import { DurableObject } from "cloudflare:workers";

export type Todo = {
  id: string; // 8 hex chars: the tile stores ids in char[10]
  text: string;
  done: boolean;
  position: number;
  source: string; // who added it: tile, mcp, web, mac, ios, api, kv (migrated)
  created: number; // unix ms
  doneAt?: number; // unix ms, set while done
};

// Checked-off items disappear 24 hours after they were checked off.
export const DONE_TTL_MS = 24 * 60 * 60 * 1000;
export const MAX_TEXT = 120;

type Row = { id: string; text: string; done: number; position: number; source: string; created_at: number; completed_at: number | null };

const toTodo = (r: Row): Todo => {
  const t: Todo = { id: r.id, text: r.text, done: !!r.done, position: r.position, source: r.source, created: r.created_at };
  if (r.completed_at != null) t.doneAt = r.completed_at;
  return t;
};

// Find by id, or by case-insensitive text match (exact first, then substring).
export function findTodo(todos: Todo[], ref: string) {
  const r = ref.trim().toLowerCase();
  return todos.find((t) => t.id === ref) ?? todos.find((t) => t.text.toLowerCase() === r) ?? todos.find((t) => t.text.toLowerCase().includes(r));
}

export class TodoStore extends DurableObject<Env> {
  sql = this.ctx.storage.sql;

  constructor(ctx: DurableObjectState, env: Env) {
    super(ctx, env);
    ctx.blockConcurrencyWhile(async () => {
      this.sql.exec(`CREATE TABLE IF NOT EXISTS todos (
        id TEXT PRIMARY KEY,
        text TEXT NOT NULL,
        done INTEGER NOT NULL DEFAULT 0,
        position INTEGER NOT NULL,
        source TEXT NOT NULL DEFAULT 'api',
        created_at INTEGER NOT NULL,
        completed_at INTEGER
      )`);
      this.sql.exec("CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT)");
      await this.importFromKv();
    });
  }

  // One-time copy of the old KV list ("todos" key), keeping ids, order and done state.
  // The KV key is left in place as a backup.
  private async importFromKv() {
    if (this.sql.exec("SELECT 1 FROM meta WHERE key = 'kv_imported'").toArray().length) return;
    const old = (await this.env.TODOS.get<any[]>("todos", "json")) ?? [];
    old.forEach((t, i) =>
      this.sql.exec(
        "INSERT OR IGNORE INTO todos (id, text, done, position, source, created_at, completed_at) VALUES (?, ?, ?, ?, 'kv', ?, ?)",
        String(t.id), String(t.text), t.done ? 1 : 0, i, Number(t.created) || Date.now(), t.done ? Number(t.doneAt) || Date.now() : null,
      ),
    );
    this.sql.exec("INSERT INTO meta (key, value) VALUES ('kv_imported', ?)", `${old.length} items at ${new Date().toISOString()}`);
  }

  private get(id: string): Todo | undefined {
    const r = this.sql.exec<Row>("SELECT * FROM todos WHERE id = ?", id).toArray()[0];
    return r && toTodo(r);
  }

  list(): Todo[] {
    this.sql.exec("DELETE FROM todos WHERE done = 1 AND completed_at < ?", Date.now() - DONE_TTL_MS);
    return this.sql.exec<Row>("SELECT * FROM todos ORDER BY position, created_at").toArray().map(toTodo);
  }

  add(text: string, source = "api"): Todo {
    let id: string;
    do id = crypto.randomUUID().slice(0, 8);
    while (this.get(id));
    const pos = this.sql.exec<{ p: number }>("SELECT COALESCE(MAX(position), -1) + 1 AS p FROM todos").one().p;
    this.sql.exec(
      "INSERT INTO todos (id, text, done, position, source, created_at) VALUES (?, ?, 0, ?, ?, ?)",
      id, text.trim().slice(0, MAX_TEXT), pos, source.slice(0, 16), Date.now(),
    );
    return this.get(id)!;
  }

  // done undefined = toggle (what the tile and the web app send).
  setDone(id: string, done?: boolean): Todo | null {
    const t = this.get(id);
    if (!t) return null;
    const d = done ?? !t.done;
    this.sql.exec("UPDATE todos SET done = ?, completed_at = ? WHERE id = ?", d ? 1 : 0, d ? (t.done ? t.doneAt ?? Date.now() : Date.now()) : null, id);
    return this.get(id)!;
  }

  edit(id: string, text: string): Todo | null {
    if (!this.get(id)) return null;
    this.sql.exec("UPDATE todos SET text = ? WHERE id = ?", text.trim().slice(0, MAX_TEXT), id);
    return this.get(id)!;
  }

  remove(id: string): Todo | null {
    const t = this.get(id);
    if (!t) return null;
    this.sql.exec("DELETE FROM todos WHERE id = ?", id);
    return t;
  }

  clearCompleted(): number {
    this.list(); // drop expired items first so the count matches what clients saw
    const n = this.sql.exec<{ n: number }>("SELECT COUNT(*) AS n FROM todos WHERE done = 1").one().n;
    this.sql.exec("DELETE FROM todos WHERE done = 1");
    return n;
  }

  // For MCP: match by id or text, then act. Returns the matched item (before removal).
  completeMatch(ref: string, done: boolean): Todo | null {
    const t = findTodo(this.list(), ref);
    return t ? this.setDone(t.id, done) : null;
  }
  editMatch(ref: string, text: string): Todo | null {
    const t = findTodo(this.list(), ref);
    return t ? this.edit(t.id, text) : null;
  }
  removeMatch(ref: string): Todo | null {
    const t = findTodo(this.list(), ref);
    return t ? this.remove(t.id) : null;
  }
}
