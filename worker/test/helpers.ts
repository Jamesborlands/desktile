import { exports } from "cloudflare:workers";

export const BASE = "https://skytile.test";

export const api = (path: string, init: RequestInit = {}, token = "api-test") =>
  exports.default.fetch(new Request(BASE + "/api/v1/todos" + path, { ...init, headers: { authorization: `Bearer ${token}`, ...init.headers } }));

export const tile = (path: string, init: RequestInit = {}) =>
  exports.default.fetch(new Request(BASE + "/todos" + path, { ...init, headers: { "x-tile-key": "tile-test", ...init.headers } }));

export const post = (text: string, source?: string): RequestInit => ({
  method: "POST",
  headers: { "content-type": "application/json" },
  body: JSON.stringify(source ? { text, source } : { text }),
});

// Call an MCP tool over Streamable HTTP and return its text output.
export async function mcp(name: string, args: Record<string, unknown> = {}) {
  const r = await exports.default.fetch(
    new Request(BASE + "/mcp/mcp-test", {
      method: "POST",
      headers: { "content-type": "application/json", accept: "application/json, text/event-stream" },
      body: JSON.stringify({ jsonrpc: "2.0", id: 1, method: "tools/call", params: { name, arguments: args } }),
    }),
  );
  const body = await r.text();
  const data = body.trimStart().startsWith("{") ? body : body.split("\n").find((l) => l.startsWith("data:"))!.slice(5);
  const msg = JSON.parse(data);
  if (msg.error) throw new Error(JSON.stringify(msg.error));
  return msg.result.content[0].text as string;
}
