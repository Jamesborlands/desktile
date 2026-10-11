// Small to-do web app for the phone (add it to the home screen). Served at
// /t/<WEB_SECRET>; it talks to /t/<WEB_SECRET>/api/... so the secret in the URL
// is the only credential. The Siri Shortcut posts to the same API.
export function todoPage(): string {
  return `<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<meta name="apple-mobile-web-app-capable" content="yes">
<meta name="mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-status-bar-style" content="black-translucent">
<meta name="apple-mobile-web-app-title" content="SkyTile">
<meta name="theme-color" content="#0a1220">
<meta name="robots" content="noindex">
<title>SkyTile To-do</title>
<style>
  :root { --bg:#0a1220; --card:#121c32; --line:#1f2d47; --text:#e8edf5; --muted:#8796ad; --dim:#5f6e86; --accent:#dfe8f5; }
  * { box-sizing: border-box; -webkit-tap-highlight-color: transparent; }
  html, body { margin: 0; background: var(--bg); color: var(--text); font: 17px/1.4 -apple-system, BlinkMacSystemFont, "Segoe UI", system-ui, sans-serif; }
  main { max-width: 560px; margin: 0 auto; padding: calc(env(safe-area-inset-top) + 20px) 16px calc(env(safe-area-inset-bottom) + 32px); }
  header { display: flex; align-items: baseline; justify-content: space-between; margin-bottom: 18px; }
  h1 { font-size: 28px; font-weight: 700; margin: 0; letter-spacing: -0.01em; }
  .count { color: var(--muted); font-size: 14px; letter-spacing: 0.06em; text-transform: uppercase; }
  form { display: flex; gap: 8px; margin-bottom: 20px; }
  input[type=text] { flex: 1; min-width: 0; height: 48px; border-radius: 12px; border: 1px solid var(--line); background: var(--card); color: var(--text); padding: 0 14px; font: inherit; outline: none; }
  input[type=text]:focus { border-color: var(--muted); }
  button { font: inherit; cursor: pointer; }
  .add { height: 48px; padding: 0 18px; border: 0; border-radius: 12px; background: var(--accent); color: var(--bg); font-weight: 600; }
  ul { list-style: none; margin: 0; padding: 0; display: flex; flex-direction: column; gap: 6px; }
  li { display: flex; align-items: center; gap: 12px; min-height: 54px; padding: 8px 8px 8px 12px; background: var(--card); border-radius: 12px; }
  .box { flex: none; width: 26px; height: 26px; border-radius: 7px; border: 2px solid var(--dim); background: none; display: grid; place-items: center; padding: 0; }
  .box svg { width: 16px; height: 16px; opacity: 0; }
  li.done .box { background: var(--accent); border-color: var(--accent); }
  li.done .box svg { opacity: 1; }
  .text { flex: 1; min-width: 0; overflow-wrap: anywhere; }
  li.done .text { color: var(--dim); text-decoration: line-through; }
  .del { flex: none; width: 40px; height: 40px; border: 0; background: none; color: var(--dim); font-size: 22px; border-radius: 10px; }
  .del:active { background: var(--line); }
  .empty { color: var(--muted); text-align: center; padding: 40px 0; }
  .foot { margin-top: 18px; display: flex; justify-content: space-between; color: var(--dim); font-size: 14px; }
  .link { border: 0; background: none; color: var(--muted); padding: 6px 0; }
  .err { color: #e5665a; font-size: 14px; min-height: 20px; margin-top: 8px; }
</style>
</head>
<body>
<main>
  <header><h1>To-do</h1><span class="count" id="count"></span></header>
  <form id="f" autocomplete="off">
    <input type="text" id="t" placeholder="Add something" maxlength="120" enterkeyhint="done" aria-label="New to-do">
    <button class="add" type="submit">Add</button>
  </form>
  <ul id="list"></ul>
  <div class="empty" id="empty" hidden>Nothing to do.</div>
  <div class="foot"><span>Shows on your SkyTile</span><button class="link" id="clear" type="button">Clear completed</button></div>
  <div class="err" id="err"></div>
</main>
<script>
const api = location.pathname.replace(/\\/$/, '') + '/api/todos';
const $ = (id) => document.getElementById(id);
let todos = [];
const check = '<svg viewBox="0 0 16 16"><path d="M3 8.5l3.2 3L13 4.5" fill="none" stroke="#0a1220" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"/></svg>';

function render() {
  const list = $('list');
  list.textContent = '';
  for (const t of todos) {
    const li = document.createElement('li');
    if (t.done) li.className = 'done';
    const box = document.createElement('button');
    box.className = 'box'; box.innerHTML = check;
    box.setAttribute('aria-label', (t.done ? 'Mark not done: ' : 'Mark done: ') + t.text);
    box.onclick = () => toggle(t);
    const text = document.createElement('span');
    text.className = 'text'; text.textContent = t.text;
    const del = document.createElement('button');
    del.className = 'del'; del.textContent = '\\u00d7';
    del.setAttribute('aria-label', 'Delete: ' + t.text);
    del.onclick = () => remove(t);
    li.append(box, text, del);
    list.append(li);
  }
  const left = todos.filter((t) => !t.done).length;
  $('count').textContent = todos.length ? left + ' left' : '';
  $('empty').hidden = todos.length > 0;
  $('clear').hidden = !todos.some((t) => t.done);
}

async function call(method, path, body) {
  $('err').textContent = '';
  const r = await fetch(api + path, { method, headers: body ? { 'content-type': 'application/json' } : {}, body: body ? JSON.stringify(body) : undefined });
  if (!r.ok) throw new Error('HTTP ' + r.status);
  return r.json();
}
async function load() {
  try { todos = (await call('GET', '')).todos; render(); } catch (e) { $('err').textContent = "Couldn't load: " + e.message; }
}
async function toggle(t) {
  t.done = !t.done; render();
  try { await call('POST', '/' + t.id + '/done'); } catch (e) { $('err').textContent = "Couldn't save: " + e.message; load(); }
}
async function remove(t) {
  todos = todos.filter((x) => x !== t); render();
  try { await call('DELETE', '/' + t.id); } catch (e) { $('err').textContent = "Couldn't delete: " + e.message; load(); }
}
$('f').onsubmit = async (e) => {
  e.preventDefault();
  const text = $('t').value.trim();
  if (!text) return;
  $('t').value = '';
  try { todos.push(await call('POST', '', { text })); render(); } catch (err) { $('err').textContent = "Couldn't add: " + err.message; }
};
$('clear').onclick = async () => {
  todos = todos.filter((t) => !t.done); render();
  try { await call('POST', '/clear'); } catch (e) { $('err').textContent = "Couldn't clear: " + e.message; load(); }
};
document.addEventListener('visibilitychange', () => { if (!document.hidden) load(); });
load();
</script>
</body>
</html>`;
}
