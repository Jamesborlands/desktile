import { describe, expect, it } from "vitest";
import { api, mcp, post, tile } from "./helpers";

describe("/api/v1/todos", () => {
  it("rejects a missing or wrong token", async () => {
    expect((await api("", {}, "")).status).toBe(401);
    expect((await api("", {}, "nope")).status).toBe(401);
  });

  it("adds, lists, completes, uncompletes and removes", async () => {
    const r = await api("", post("Buy milk", "mac"));
    expect(r.status).toBe(201);
    const { todo } = await r.json<any>();
    expect(todo).toMatchObject({ text: "Buy milk", done: false, source: "mac" });
    expect(todo.id).toMatch(/^[0-9a-f]{8}$/);

    let list = (await (await api("")).json<any>()).todos;
    expect(list.map((t: any) => t.id)).toContain(todo.id);

    const done = (await (await api(`/${todo.id}/complete`, { method: "POST" })).json<any>()).todo;
    expect(done.done).toBe(true);
    expect(done.doneAt).toBeTypeOf("number");
    // complete is idempotent (a widget may retry)
    expect((await (await api(`/${todo.id}/complete`, { method: "POST" })).json<any>()).todo.done).toBe(true);

    const open = (await (await api(`/${todo.id}/uncomplete`, { method: "POST" })).json<any>()).todo;
    expect(open.done).toBe(false);
    expect(open.doneAt).toBeUndefined();

    expect((await api(`/${todo.id}`, { method: "DELETE" })).status).toBe(200);
    list = (await (await api("")).json<any>()).todos;
    expect(list.map((t: any) => t.id)).not.toContain(todo.id);
    expect((await api(`/${todo.id}`, { method: "DELETE" })).status).toBe(404);
  });

  it("lists open items first, in the order they were added", async () => {
    const a = (await (await api("", post("first"))).json<any>()).todo;
    const b = (await (await api("", post("second"))).json<any>()).todo;
    await api(`/${a.id}/complete`, { method: "POST" });
    const ids = (await (await api("")).json<any>()).todos.map((t: any) => t.id);
    expect(ids.indexOf(b.id)).toBeLessThan(ids.indexOf(a.id));
    expect(b.position).toBeGreaterThan(a.position);
  });

  it("edits the text", async () => {
    const t = (await (await api("", post("Buy mlik"))).json<any>()).todo;
    const patch = (body: unknown) => api(`/${t.id}`, { method: "PATCH", headers: { "content-type": "application/json" }, body: JSON.stringify(body) });
    const r = await patch({ text: "Buy milk" });
    expect(r.status).toBe(200);
    expect((await r.json<any>()).todo).toMatchObject({ id: t.id, text: "Buy milk", done: false });
    expect((await patch({ text: " " })).status).toBe(400);
    expect((await api("/nope1234", { method: "PATCH", body: JSON.stringify({ text: "x" }) })).status).toBe(404);
  });

  it("validates text and source", async () => {
    expect((await api("", post("   "))).status).toBe(400);
    expect((await api("", post("x".repeat(121)))).status).toBe(400);
    const t = (await (await api("", post("plain", "hacker"))).json<any>()).todo;
    expect(t.source).toBe("api");
  });

  it("clears completed items", async () => {
    const t = (await (await api("", post("to clear"))).json<any>()).todo;
    await api(`/${t.id}/complete`, { method: "POST" });
    const { cleared } = await (await api("/clear-completed", { method: "POST" })).json<any>();
    expect(cleared).toBeGreaterThanOrEqual(1);
    const list = (await (await api("")).json<any>()).todos;
    expect(list.some((x: any) => x.done)).toBe(false);
  });
});

describe("tile /todos (unchanged contract)", () => {
  it("needs X-Tile-Key", async () => {
    const { exports } = await import("cloudflare:workers");
    expect((await exports.default.fetch("https://skytile.test/todos")).status).toBe(401);
  });

  it("sees items added through the API and toggles them", async () => {
    const t = (await (await api("", post("Café run"))).json<any>()).todo;
    const body = await (await tile("")).text();
    expect(body).toContain('"text":"Cafe run"'); // ASCII-folded for the tile's fonts
    const item = JSON.parse(body).todos.find((x: any) => x.id === t.id);
    expect(item.done).toBe(false);
    expect((await (await tile(`/${t.id}/done`, { method: "POST" })).json<any>()).done).toBe(true);
    expect((await (await tile(`/${t.id}/done`, { method: "POST" })).json<any>()).done).toBe(false); // toggle
    expect((await tile("/nope/done", { method: "POST" })).status).toBe(404);
  });
});

describe("MCP tools (same names and output)", () => {
  it("add, list, complete, remove, clear", async () => {
    const added = await mcp("add_todo", { text: "Call the dentist" });
    expect(added).toMatch(/^Added: Call the dentist \(id [0-9a-f]{8}\)$/);
    const id = added.match(/id ([0-9a-f]{8})/)![1];

    expect(await mcp("list_todos")).toContain(`[ ] Call the dentist (id ${id})`);
    expect(await mcp("complete_todo", { item: "dentist" })).toBe("Done: Call the dentist");
    expect(await mcp("list_todos")).toContain(`[x] Call the dentist (id ${id})`);
    expect(await mcp("complete_todo", { item: id, done: false })).toBe("Reopened: Call the dentist");
    expect(await mcp("complete_todo", { item: "no such thing" })).toBe('No to-do matches "no such thing".');
    expect(await mcp("edit_todo", { item: "dentist", text: "Call the dentist at 9" })).toBe(`Edited: Call the dentist at 9 (id ${id})`);
    expect(await mcp("remove_todo", { item: "call the dentist" })).toBe("Removed: Call the dentist at 9");

    await mcp("add_todo", { text: "a" });
    await mcp("complete_todo", { item: "a" });
    expect(await mcp("clear_completed")).toMatch(/^Cleared \d+ completed item\(s\)\.$/);
  });

  it("an item added by MCP shows up for the API", async () => {
    await mcp("add_todo", { text: "From Claude" });
    const t = (await (await api("")).json<any>()).todos.find((x: any) => x.text === "From Claude");
    expect(t.source).toBe("mcp");
  });
});
