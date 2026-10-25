import { env } from "cloudflare:test";
import { expect, it } from "vitest";
import { api } from "./helpers";

it("copies the old KV list into the store once, keeping ids, order and done state", async () => {
  const old = [
    { id: "e541a3f3", text: "Pay estimated taxes", done: false, created: 1792305228421 },
    { id: "1aa692ba", text: "Already done", done: true, created: 1792631359866, doneAt: Date.now() - 1000 },
    { id: "c6834304", text: "Third", done: false, created: 1792631437807 },
  ];
  await env.TODOS.put("todos", JSON.stringify(old));

  const list = (await (await api("")).json<any>()).todos;
  expect(list.map((t: any) => t.id)).toEqual(["e541a3f3", "c6834304", "1aa692ba"]); // open first
  expect(list.find((t: any) => t.id === "1aa692ba")).toMatchObject({ done: true, source: "kv" });
  expect(list.find((t: any) => t.id === "e541a3f3")).toMatchObject({ text: "Pay estimated taxes", created: 1792305228421 });

  // KV stays as a backup, and deleting everything doesn't bring the old items back.
  expect(await env.TODOS.get("todos")).not.toBeNull();
  for (const t of list) await api(`/${t.id}`, { method: "DELETE" });
  expect((await (await api("")).json<any>()).todos).toEqual([]);
});
