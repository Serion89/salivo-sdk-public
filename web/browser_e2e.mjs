// Real-browser end-to-end runner for Stage 37.2 / 37.3 (no npm dependencies).
// Launches headless Chrome or Edge, drives it over the DevTools protocol with Node's built-in
// WebSocket, serves the page, the host bridge, the module and HTTP fixtures from a local server,
// and dispatches trusted mouse/keyboard input (Input.dispatch*), not synthetic JS events.
//
//   node scripts/browser_e2e.mjs <scenarios.mjs> <dir with NAME.wasm> [name ...]
//   node scripts/browser_e2e.mjs --serve <dir with NAME.wasm>   serve for a manual look: open
//                                                               http://127.0.0.1:PORT/?m=NAME
// prints one JSON line per scenario: {"name","pass","error","stdout","stats","ms"} and exits 1 on
// any failure. A scenarios module exports `default { NAME: async (page) => { ... } }`.
import { spawn } from "node:child_process";
import { createServer } from "node:http";
import { existsSync, mkdtempSync, readFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname, resolve } from "node:path";
import { pathToFileURL, fileURLToPath } from "node:url";
import { createHash } from "node:crypto";

const HERE = dirname(fileURLToPath(import.meta.url));
const BROWSERS = [
  process.env.SALIVO_BROWSER,
  "C:/Program Files/Google/Chrome/Application/chrome.exe",
  "C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe",
  "/usr/bin/google-chrome", "/usr/bin/chromium", "/usr/bin/chromium-browser",
  "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
].filter(Boolean);

export function findBrowser() { return BROWSERS.find((p) => existsSync(p)) || null; }

const PAGE = `<!doctype html><meta charset="utf-8"><title>salivo e2e</title><body><div id="app"></div>
<script>
window.fixture = { nul: "a\\u0000b", nan: NaN, inf: Infinity, negzero: -0, big: 2 ** 60, frac: 1.5,
  unicode: "h\\u00e9llo \\u4e16\\u754c \\ud83c\\udf89", flag: true, num: 42, nothing: null, obj: { k: "v" }, empty: "" };
window.fixture.thrower = () => { throw new RangeError("boom"); };
window.fixture.api = new URLSearchParams(location.search).get("api") || "";
</script>
<script type="module">
import { startSalivo } from "/salivo_browser.mjs";
const name = new URLSearchParams(location.search).get("m");
window.__out = []; window.__t = [];
try {
  const bytes = await (await fetch("/m/" + name + ".wasm")).arrayBuffer();
  window.__app = await startSalivo(bytes, { stdout: (l) => { window.__out.push(l); window.__t.push(performance.now()); } });
} catch (e) { window.__loadError = String(e); }
window.__ready = true;
</script>`;

function fixtureServer(wasmDir) {
  const server = createServer((req, res) => {
    const url = new URL(req.url, "http://x");
    const send = (code, type, body, extra = {}) => { res.writeHead(code, { "content-type": type, ...extra }); res.end(body); };
    if (url.pathname === "/") return send(200, "text/html", PAGE);
    if (url.pathname === "/salivo_browser.mjs") return send(200, "text/javascript", readFileSync(join(HERE, "salivo_browser.mjs")));
    if (url.pathname.startsWith("/m/")) {
      const f = join(wasmDir, url.pathname.slice(3).replace(/[^\w.-]/g, ""));
      return existsSync(f) ? send(200, "application/wasm", readFileSync(f)) : send(404, "text/plain", "no module");
    }
    if (url.pathname === "/api/ok") return send(200, "application/json", '{"n":7,"msg":"h\u00e9llo"}', { "x-test": "yes" });
    if (url.pathname === "/api/err") return send(500, "text/plain", "server broke");
    if (url.pathname === "/api/missing") return send(404, "text/plain", "nope");
    if (url.pathname === "/api/delay") return setTimeout(() => send(200, "application/json", '{"n":7,"msg":"héllo"}'), 150);
    if (url.pathname === "/api/slow") return setTimeout(() => send(200, "text/plain", "late"), 400);
    if (url.pathname === "/api/echo") {
      let body = "";
      req.on("data", (c) => (body += c));
      return req.on("end", () => send(200, "text/plain", `${req.method} ${req.headers["x-salivo"] || ""} ${body}`));
    }
    if (url.pathname === "/api/count") return send(200, "text/plain", String(++server.hits));
    send(404, "text/plain", "not found");
  });
  server.hits = 0;
  return new Promise((ok) => server.listen(0, "127.0.0.1", () => ok(server)));
}

class Cdp {
  constructor(ws) {
    this.ws = ws; this.id = 0; this.waiting = new Map();
    ws.onmessage = (m) => {
      const msg = JSON.parse(m.data);
      const w = this.waiting.get(msg.id);
      if (w) { this.waiting.delete(msg.id); msg.error ? w.no(new Error(msg.error.message)) : w.ok(msg.result); }
    };
  }
  send(method, params = {}) {
    const id = ++this.id;
    this.ws.send(JSON.stringify({ id, method, params }));
    return new Promise((ok, no) => this.waiting.set(id, { ok, no }));
  }
}

async function launch(browserPath) {
  const profile = mkdtempSync(join(tmpdir(), "salivo-e2e-"));
  const proc = spawn(browserPath, ["--headless=new", "--remote-debugging-port=0", `--user-data-dir=${profile}`,
    "--no-first-run", "--no-default-browser-check", "--disable-gpu", "--disable-extensions", "about:blank"], { stdio: "ignore" });
  const portFile = join(profile, "DevToolsActivePort");
  for (let i = 0; i < 200 && !existsSync(portFile); i++) await new Promise((r) => setTimeout(r, 50));
  if (!existsSync(portFile)) { proc.kill(); throw new Error("browser did not start"); }
  const [port] = readFileSync(portFile, "utf8").split("\n");
  const close = () => { proc.kill(); try { rmSync(profile, { recursive: true, force: true }); } catch {} };
  return { port, close, version: await (await fetch(`http://127.0.0.1:${port}/json/version`)).json() };
}

async function openPage(port, url) {
  const t = await (await fetch(`http://127.0.0.1:${port}/json/new?${encodeURIComponent(url)}`, { method: "PUT" })).json();
  const ws = new WebSocket(t.webSocketDebuggerUrl);
  await new Promise((ok, no) => { ws.onopen = ok; ws.onerror = no; });
  const cdp = new Cdp(ws);
  const page = {
    async eval(expr) {
      const r = await cdp.send("Runtime.evaluate", { expression: expr, awaitPromise: true, returnByValue: true });
      if (r.exceptionDetails) throw new Error(`page: ${r.exceptionDetails.exception?.description || r.exceptionDetails.text}`);
      return r.result.value;
    },
    async waitFor(expr, ms = 5000) {
      const end = Date.now() + ms;
      while (Date.now() < end) { if (await page.eval(expr)) return; await new Promise((r) => setTimeout(r, 15)); }
      throw new Error(`timed out waiting for ${expr}`);
    },
    async click(selector) {
      const box = await page.eval(`(() => { const e = document.querySelector(${JSON.stringify(selector)}); if (!e) return null;
        e.scrollIntoView(); const r = e.getBoundingClientRect(); return { x: r.x + r.width / 2, y: r.y + r.height / 2 }; })()`);
      if (!box) throw new Error(`no element ${selector}`);
      for (const type of ["mouseMoved", "mousePressed", "mouseReleased"])
        await cdp.send("Input.dispatchMouseEvent", { type, x: box.x, y: box.y, button: "left", clickCount: 1 });
    },
    async type(selector, text) {
      await page.eval(`document.querySelector(${JSON.stringify(selector)}).focus()`);
      await cdp.send("Input.insertText", { text });
    },
    async key(key) {
      await cdp.send("Input.dispatchKeyEvent", { type: "keyDown", key, text: key.length === 1 ? key : undefined });
      await cdp.send("Input.dispatchKeyEvent", { type: "keyUp", key });
    },
    html: (sel = "#app") => page.eval(`document.querySelector(${JSON.stringify(sel)}).innerHTML`),
    text: (sel) => page.eval(`document.querySelector(${JSON.stringify(sel)})?.textContent ?? null`),
    stdout: () => page.eval("window.__out"),
    times: () => page.eval("window.__t"),
    stats: () => page.eval("window.__app && window.__app.stats()"),
    app: () => page.eval("window.__app && { status: window.__app.status, dead: window.__app.dead, error: window.__app.error }"),
    sleep: (ms) => new Promise((r) => setTimeout(r, ms)),
    close: () => { ws.close(); return fetch(`http://127.0.0.1:${portOf}/json/close/${t.id}`).catch(() => {}); },
  };
  const portOf = port;
  await page.waitFor("window.__ready === true", 10000);
  const loadError = await page.eval("window.__loadError || null");
  if (loadError) throw new Error(`module load failed: ${loadError}`);
  return page;
}

export function assert(cond, msg) { if (!cond) throw new Error(`assertion failed: ${msg}`); }

/// Common check: the program's own self-checks printed no FAIL line and ended with `marker`
export async function selfChecked(page, marker) {
  await page.waitFor(`window.__out.includes(${JSON.stringify(marker)})`);
  const out = await page.stdout();
  const fails = out.filter((l) => l.startsWith("FAIL"));
  assert(fails.length === 0, fails.join("; "));
}

async function main() {
  if (process.argv[2] === "--serve") {
    const server = await fixtureServer(resolve(process.argv[3] || "build"));
    console.log(`serving http://127.0.0.1:${server.address().port}/?m=NAME (Ctrl+C to stop)`);
    return;
  }
  const [scenFile, wasmDir, ...only] = process.argv.slice(2);
  const browserPath = findBrowser();
  if (!browserPath) { console.log(JSON.stringify({ name: "*", pass: false, error: "no Chrome/Edge/Chromium found" })); process.exit(2); }
  const scenarios = (await import(pathToFileURL(resolve(scenFile)).href)).default;
  const server = await fixtureServer(resolve(wasmDir));
  const base = `http://127.0.0.1:${server.address().port}`;
  const browser = await launch(browserPath);
  let failed = 0;
  for (const [name, fn] of Object.entries(scenarios)) {
    if (only.length && !only.includes(name)) continue;
    const module = fn.module || name;
    const t0 = Date.now();
    let page, rec = { name, browser: browser.version.Browser };
    try {
      page = await openPage(browser.port, `${base}/?m=${module}${fn.query ? fn.query() : ""}`);
      page.base = base; page.server = server;
      const extra = await fn(page);
      if (extra) rec.result = extra;
      rec.pass = true;
    } catch (e) { rec.pass = false; rec.error = String(e.message || e); failed++; }
    if (page) {
      try {
        rec.stdout = await page.stdout(); rec.stats = await page.stats(); rec.app = await page.app();
        // final DOM, for comparing the same program built at different optimization levels
        rec.dom = createHash("sha256").update(await page.eval(`document.querySelector("#app").innerHTML`)).digest("hex");
      } catch {}
      await page.close();
    }
    rec.ms = Date.now() - t0;
    console.log(JSON.stringify(rec));
  }
  browser.close();
  server.close();
  process.exit(failed ? 1 : 0);
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) main();
