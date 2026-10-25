import { cloudflareTest } from "@cloudflare/vitest-plugin";
import { defineConfig } from "vitest/config";

// Tests run inside the Workers runtime (Miniflare) with fake secrets.
export default defineConfig({
  plugins: [
    cloudflareTest({
      wrangler: { configPath: "./wrangler.jsonc" },
      miniflare: {
        bindings: { TILE_KEY: "tile-test", MCP_SECRET: "mcp-test", WEB_SECRET: "web-test", API_TOKEN: "api-test" },
      },
    }),
  ],
});
