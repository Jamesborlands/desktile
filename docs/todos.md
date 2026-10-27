# To-dos: backend and clients

The to-do list lives in one place (a Cloudflare Durable Object) and is shared by the desk
tile, Claude (MCP), a Mac menu bar item, an iPhone app and widget, and a phone web app.
Back to the [main README](../README.md).

## Architecture

```
 Desk tile (ESP32)  --X-Tile-Key-->  /todos ----------.
 Phone web app      --secret URL-->  /t/<WEB_SECRET>  |
 Claude (MCP)       --secret URL-->  /mcp/<MCP_SECRET>+--> Worker "skytile" --> TodoStore Durable Object
 Mac menu bar       --Bearer------>  /api/v1/todos    |    (Cloudflare)         (SQLite, one instance)
 iPhone app/widget  --Bearer------>  /api/v1/todos ---'
```

- **One source of truth:** the `TodoStore` Durable Object (`worker/src/todos.ts`). It has a
  SQLite table `todos (id, text, done, position, source, created_at, completed_at)`.
  Every change runs inside the object one at a time, so two clients can't overwrite each
  other, and every read sees the latest write.
- **Ids** are 8 hex characters (the tile stores ids in `char[10]`).
- **Done items** disappear 24 hours after they were checked off.
- **How fast changes show up:** the tile polls every 5 s on the To-do screen (every minute
  otherwise). The Mac menu bar refreshes every 30 s. The iPhone app polls every 10 s while
  open. The widget refreshes when iOS allows it (about every 15 min) and right after you
  tap one of its checkboxes.
- **Old storage:** the list used to live in KV (key `todos`). It was copied into TodoStore
  on Oct 24, 2026, and the KV key is kept as a backup.

## Client API

Base URL `https://skytile.seobubbles.workers.dev`, header `Authorization: Bearer $SKYTILE_API_TOKEN`.

| Method | Path | Body | Returns |
|---|---|---|---|
| GET | `/api/v1/todos` | | `{todos: [...]}`, open first |
| POST | `/api/v1/todos` | `{text, source?}` (source: mac, ios, widget, siri, web, api) | 201 `{todo}` |
| POST | `/api/v1/todos/:id/complete` | | `{todo}` |
| POST | `/api/v1/todos/:id/uncomplete` | | `{todo}` |
| PATCH | `/api/v1/todos/:id` | `{text}` | `{todo}` |
| DELETE | `/api/v1/todos/:id` | | `{ok: true}` |
| POST | `/api/v1/todos/clear-completed` | | `{cleared: n}` |

A todo looks like `{id, text, done, position, source, created, doneAt?}` (times in unix ms).

```sh
set -a; . ./.env; set +a
curl -H "Authorization: Bearer $SKYTILE_API_TOKEN" $SKYTILE_URL/api/v1/todos
```

The MCP tools (`list_todos`, `add_todo`, `complete_todo`, `remove_todo`, `clear_completed`)
and the tile's `/todos` endpoints are unchanged. `edit_todo` (item, text) was added.

## Secrets

- `.env` (gitignored, copy `.env.example`): `SKYTILE_URL`, `SKYTILE_API_TOKEN` for clients and scripts.
- `worker/.dev.vars` (gitignored): the Worker's secrets for local dev.
- On Cloudflare: `cd worker && npx wrangler secret put API_TOKEN` (same value as
  `SKYTILE_API_TOKEN`). Also `TILE_KEY`, `MCP_SECRET`, `WEB_SECRET`, `AISSTREAM_KEY`.
- To rotate the token: generate one (`openssl rand -hex 32`), put it on the Worker, in
  `.env`, in `~/.config/skytile/config`, and in the iPhone app's Settings.

## Worker

```sh
cd worker
npm install
npm test             # vitest in the Workers runtime: API, auth, tile contract, MCP output, KV migration
npm run typecheck
npx wrangler deploy
```

## Desk tile

- **To-do screen:** one row per item, taller rows for text that wraps. Tap to check off.
  It polls every 5 s and only redraws when the list changed.
- **Sleep dashboard:** clock and date, then TO-DO (bulleted, wrapped, with "+N more" when
  they don't all fit) and NEXT LAUNCH. Each section is hidden when it's empty, and NEXT
  LAUNCH moves up when there are no to-dos.

## Mac menu bar

`clients/mac/` is a native SwiftUI menu bar app (macOS 26+, Liquid Glass). The menu bar shows
a checklist icon and the open count. Click it, or press Cmd+Option+T from anywhere, and a glass
panel drops down with the add field already focused: type and press Return, and it stays ready
for the next one.

- **Check off:** click the circle. Click a completed item's circle to undo.
- **Edit:** double-click the text (or hover and click the pencil) to edit in place. Return
  saves, Esc cancels.
- **Delete:** hover and click the trash, or right-click for Done, Edit, Copy and Delete.
- **Completed:** items checked off today, collapsible. "Clear" removes them now; otherwise
  they disappear 24 hours after being checked off.
- The ••• menu has Refresh, Open at Login, Edit Config File and Quit.

Changes show at once and sync in the background; it refreshes every 30 s and whenever the
panel opens. Build and install (needs Xcode and `brew install xcodegen`, no developer account):

```sh
mkdir -p ~/.config/skytile
cp .env ~/.config/skytile/config && chmod 600 ~/.config/skytile/config
clients/mac/install.sh     # builds, copies to ~/Applications/SkyTile.app, opens it
```

Then turn on "Open at Login" in the ••• menu. Launch it with `open`, not by running the
binary from a terminal (macOS then files the menu bar icon under the terminal app).

`clients/mac/snapshot.sh` retakes the README screenshot (`docs/images/menubar.png`) with
sample to-dos instead of the real list. It needs Screen Recording for the terminal.

## iPhone

### Free option: the web app

The Worker serves a phone-friendly list at `$SKYTILE_URL/t/<WEB_SECRET>` (the secret is in
`worker/.dev.vars`). Open it in Safari, tap Share > Add to Home Screen, and it runs
full screen like an app. No Apple developer account needed. No widget, though.

### Native app + widget

`clients/ios/` is a SwiftUI app with a WidgetKit extension (iOS 17+). The Xcode project is
generated from `project.yml` with XcodeGen.

- **App:** list, add field (stays focused for quick entry), tap or swipe right to check off,
  swipe left to delete, pull to refresh, Settings for the URL and token.
- **Widget:** small, medium and large home screen sizes, plus lock screen circular,
  rectangular and inline. Tap a circle to check an item off without opening the app.
  The "+" on medium and large opens the app ready to type.
- **Siri and Shortcuts:** "Add a to-do in SkyTile" (Siri asks what to add). The "Add To-do"
  action is also available in the Shortcuts app and for the Action button.
- **Token:** stored in the keychain under the App Group, so the widget can read it too.

### Run it on your iPhone

1. `brew install xcodegen`, then `cd clients/ios && xcodegen && open SkyTile.xcodeproj`.
2. Xcode > Settings > Accounts: add your Apple ID.
3. Select the **SkyTile** target > Signing & Capabilities: tick "Automatically manage
   signing" and pick your team. Do the same for **SkyTileWidget**. To make it stick across
   `xcodegen` runs, put your Team ID in `DEVELOPMENT_TEAM` in `project.yml`.
4. If Xcode says the bundle id or App Group is taken, change `BUNDLE_ID` and `APP_GROUP`
   in `project.yml` (for example `com.yourname.skytile` and `group.com.yourname.skytile`)
   and run `xcodegen` again.
5. Plug in the iPhone and trust the Mac. On the phone, turn on Settings > Privacy &
   Security > Developer Mode, then restart.
6. Pick the iPhone as the run destination and press Run (Cmd+R). The first time, trust the
   developer on the phone: Settings > General > VPN & Device Management.
7. In the app, Settings: paste the token. To copy it from the Mac, run
   `grep SKYTILE_API_TOKEN .env | cut -d= -f2 | tr -d '\n' | pbcopy` and paste on the
   phone (Universal Clipboard). Tap "Save and test".
8. Add the widget: long-press the home screen > Edit > Add Widget > SkyTile. For the lock
   screen: long-press the lock screen > Customize > Lock Screen > add a SkyTile widget.

With a free Apple ID, the app stops opening after 7 days until you press Run in Xcode
again, and Xcode may refuse the App Groups capability, which the widget needs. A paid
developer account ($99/yr) avoids both, and a build lasts a year. As of Oct 24, 2026 James
is not using a paid account, so the native app is built and tested in the simulator only.
