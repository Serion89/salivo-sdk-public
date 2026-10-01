// Stage 37.2 browser host for Salivo wasm32 modules (docs/STAGE_37_2_BROWSER.md).
// Implements the `browser.*` imports declared in crates/salivo_backend/src/salivo_browser_abi.def
// on top of the Stage 37.1 `salivo.*` imports. Runs in a browser page (ES module):
//
//   import { startSalivo } from "./salivo_browser.mjs";
//   const app = await startSalivo(await (await fetch("app.wasm")).arrayBuffer());
//
// The browser event loop owns all scheduling: listeners, timers and fetch completions call back
// into the module through its `salivo_br_invoke` export, one at a time. No eval, no Function.

const OK = 0, E_ARG = -7, E_HANDLE = -10, E_FULL = -11, E_JS = -20, E_TYPE = -21;
const INDEX_BITS = 20, MAX_INDEX = (1 << INDEX_BITS) - 1, MAX_GEN = 2047;
const MAX_SAFE = BigInt(Number.MAX_SAFE_INTEGER);

export async function startSalivo(bytes, opts = {}) {
  const out = opts.stdout || ((line) => console.log(line));
  const enc = new TextEncoder();
  const dec = new TextDecoder("utf-8", { fatal: true });
  let memory, exports, lastError = "", pending = null, lineBuf = "";
  const app = { status: null, dead: false, error: null, stdout: [], dispatched: 0 };

  // ---- handle table: index + generation, so a stale handle never names a newer object --------
  // slot = { gen, value, kind } ; kind: "value" | "listener" | "timer" | "fetch". Permanent slots
  // (window, document) and borrowed ones (the event or response of a running callback, ended by
  // the host when it returns) cannot be released by the module: -7. A slot whose generation
  // reaches MAX_GEN is retired.
  const slots = [null];
  const free = [];
  const counts = { value: 0, listener: 0, timer: 0, fetch: 0 };
  function alloc(value, kind = "value", permanent = false, borrowed = false) {
    let i = free.pop();
    if (i === undefined) {
      if (slots.length > MAX_INDEX) return E_FULL;
      i = slots.length;
      slots.push({ gen: 0 });
    }
    const s = slots[i];
    s.gen += 1; s.value = value; s.kind = kind; s.live = true; s.permanent = permanent; s.borrowed = borrowed;
    counts[kind] += 1;
    return (s.gen << INDEX_BITS) | i;
  }
  function slot(h, kind) {
    const s = slots[h & MAX_INDEX];
    if (!s || !s.live || s.gen !== h >>> INDEX_BITS || (kind && s.kind !== kind)) return null;
    return s;
  }
  function get(h) { const s = slot(h, "value"); return s ? s.value : undefined; }
  function drop(h) {
    const i = h & MAX_INDEX, s = slots[i];
    s.live = false; s.value = null;
    counts[s.kind] -= 1;
    if (s.gen < MAX_GEN) free.push(i);
  }
  function release(h) {
    const s = slot(h);
    if (!s) return E_HANDLE;
    if (s.permanent || s.borrowed) return E_ARG;
    if (s.kind === "listener") s.value.target.removeEventListener(s.value.type, s.value.fn);
    else if (s.kind === "timer") (s.value.repeat ? clearInterval : clearTimeout)(s.value.id);
    else if (s.kind === "fetch") s.value.abort.abort();
    drop(h);
    return OK;
  }
  const WINDOW = alloc(globalThis, "value", true);
  const DOCUMENT = alloc(globalThis.document, "value", true);

  // ---- strings: UTF-8 in linear memory, staged for the module on the way back ----------------
  function str(p, n) { return dec.decode(new Uint8Array(memory.buffer, p, n)); }
  function give(s) {
    if (typeof s !== "string") return E_TYPE;
    if (s.includes("\0")) { lastError = "string contains NUL"; return E_TYPE; } // Salivo strings are NUL-terminated
    pending = enc.encode(s);
    return pending.length;
  }
  // Every import runs inside guard: a JS exception becomes E_JS with its message kept, invalid
  // UTF-8 from the module becomes E_TYPE. Nothing is swallowed into a fake success.
  function guard(f) {
    return (...a) => {
      if (app.dead) return E_HANDLE;
      try { return f(...a); } catch (e) {
        if (e instanceof Exit) throw e;
        lastError = `${e && e.name}: ${e && e.message}`;
        return e instanceof TypeError && /encoded data was not valid/.test(e.message) ? E_TYPE : E_JS;
      }
    };
  }
  const node = (h) => { const v = get(h); return v instanceof Node ? v : undefined; };
  const obj = (h) => { const v = get(h); return v !== undefined && v !== null && (typeof v === "object" || typeof v === "function") ? v : undefined; };
  const ref = (v) => (v === null || v === undefined ? 0 : alloc(v));

  // ---- callbacks: the registration is checked live before every call ------------------------
  class Exit { constructor(code) { this.code = code; } }
  function invoke(h, ev) {
    const s = slot(h);
    if (!s || app.dead) return;
    const { cb, ctx } = s.value;
    app.dispatched += 1;
    try { exports.salivo_br_invoke(cb, ctx, ev); }
    catch (e) { stop(e instanceof Exit ? e.code : 134, e instanceof Exit ? null : `wasm trap: ${e.message}`); }
  }
  // Ends the program: every listener, timer and pending fetch is detached so nothing calls a
  // module that is no longer running.
  function stop(code, err) {
    if (app.dead) return;
    flush();
    app.status = code; app.error = err;
    for (let i = 1; i < slots.length; i++) {
      const s = slots[i];
      if (s && s.live && s.kind !== "value") release((s.gen << INDEX_BITS) | i);
    }
    app.dead = true;
    if (err) console.error(err);
    if (opts.onexit) opts.onexit(app);
  }
  function flush() { if (lineBuf) { app.stdout.push(lineBuf); out(lineBuf); lineBuf = ""; } }

  const salivo = {
    write(fd, p, n) {
      const text = new TextDecoder().decode(new Uint8Array(memory.buffer, p, n));
      const parts = (lineBuf + text).split("\n");
      lineBuf = parts.pop();
      for (const line of parts) { app.stdout.push(line); out(line); }
    },
    exit(code) { throw new Exit(code); },
  };

  const browser = {
    window: () => WINDOW,
    document: () => DOCUMENT,
    release: guard((h) => release(h)),
    stat: guard((k) => [counts.value, counts.listener, counts.timer, counts.fetch, app.dispatched, memory.buffer.byteLength][k]),
    same: guard((a, b) => { const x = slot(a), y = slot(b); return !x || !y ? E_HANDLE : x.value === y.value ? 1 : 0; }),
    error: () => give(lastError),
    take(p) { new Uint8Array(memory.buffer, p, pending.length).set(pending); pending = null; },
    query: guard((h, p, n) => {
      const r = node(h);
      return r && r.querySelector ? ref(r.querySelector(str(p, n))) : E_HANDLE;
    }),
    byid: guard((p, n) => ref(document.getElementById(str(p, n)))),
    create: guard((p, n) => alloc(document.createElement(str(p, n)))),
    createtext: guard((p, n) => alloc(document.createTextNode(str(p, n)))),
    insert: guard((ph, ch, bh) => {
      const p = node(ph), c = node(ch), b = bh ? node(bh) : null;
      if (!p || !c || b === undefined) return E_HANDLE;
      p.insertBefore(c, b);
      return OK;
    }),
    remove: guard((h) => { const n = node(h); if (!n) return E_HANDLE; n.remove ? n.remove() : n.parentNode && n.parentNode.removeChild(n); return OK; }),
    parent: guard((h) => { const n = node(h); return n ? ref(n.parentNode) : E_HANDLE; }),
    childcount: guard((h) => { const n = node(h); return n ? n.childNodes.length : E_HANDLE; }),
    child: guard((h, i) => { const n = node(h); return n ? ref(n.childNodes[i]) : E_HANDLE; }),
    settext: guard((h, p, n) => { const e = node(h); if (!e) return E_HANDLE; e.textContent = str(p, n); return OK; }),
    text: guard((h) => { const e = node(h); return e ? give(e.textContent ?? "") : E_HANDLE; }),
    setattr: guard((h, kp, kn, vp, vn) => { const e = node(h); if (!e || !e.setAttribute) return E_HANDLE; e.setAttribute(str(kp, kn), str(vp, vn)); return OK; }),
    attr: guard((h, kp, kn) => { const e = node(h); return e && e.getAttribute ? give(e.getAttribute(str(kp, kn)) ?? "") : E_HANDLE; }),
    hasattr: guard((h, kp, kn) => { const e = node(h); return e && e.hasAttribute ? (e.hasAttribute(str(kp, kn)) ? 1 : 0) : E_HANDLE; }),
    removeattr: guard((h, kp, kn) => { const e = node(h); if (!e || !e.removeAttribute) return E_HANDLE; e.removeAttribute(str(kp, kn)); return OK; }),
    tag: guard((h) => { const e = node(h); return e ? give(e.nodeType === 1 ? e.tagName.toLowerCase() : e.nodeName) : E_HANDLE; }),
    typeof: guard((h, kp, kn) => { const o = obj(h); if (!o) return E_HANDLE; const v = o[str(kp, kn)]; return give(v === null ? "null" : typeof v); }),
    getstr: guard((h, kp, kn) => {
      const o = obj(h); if (!o) return E_HANDLE;
      const v = o[str(kp, kn)];
      if (typeof v !== "string") { lastError = `property is ${v === null ? "null" : typeof v}, not string`; return E_TYPE; }
      return give(v);
    }),
    setstr: guard((h, kp, kn, vp, vn) => { const o = obj(h); if (!o) return E_HANDLE; o[str(kp, kn)] = str(vp, vn); return OK; }),
    getint: guard((h, kp, kn, outp) => {
      const o = obj(h); if (!o) return E_HANDLE;
      let v = o[str(kp, kn)];
      if (typeof v === "boolean") v = v ? 1 : 0;
      if (typeof v !== "number" || !Number.isSafeInteger(v)) {
        lastError = typeof v === "number" ? `number ${v} is not an exact integer` : `property is ${v === null ? "null" : typeof v}, not number`;
        return E_TYPE;
      }
      new BigInt64Array(memory.buffer, outp, 1)[0] = BigInt(v);
      return OK;
    }),
    setint: guard((h, kp, kn, v) => {
      const o = obj(h); if (!o) return E_HANDLE;
      if (v > MAX_SAFE || v < -MAX_SAFE) { lastError = `${v} does not fit a JavaScript number exactly`; return E_TYPE; }
      o[str(kp, kn)] = Number(v);
      return OK;
    }),
    getbool: guard((h, kp, kn) => {
      const o = obj(h); if (!o) return E_HANDLE;
      const v = o[str(kp, kn)];
      if (typeof v !== "boolean") { lastError = `property is ${v === null ? "null" : typeof v}, not boolean`; return E_TYPE; }
      return v ? 1 : 0;
    }),
    setbool: guard((h, kp, kn, v) => { const o = obj(h); if (!o) return E_HANDLE; o[str(kp, kn)] = !!v; return OK; }),
    getobj: guard((h, kp, kn) => {
      const o = obj(h); if (!o) return E_HANDLE;
      const v = o[str(kp, kn)];
      if (v === null || v === undefined) return 0;
      if (typeof v !== "object" && typeof v !== "function") { lastError = `property is ${typeof v}, not object`; return E_TYPE; }
      return alloc(v);
    }),
    call: guard((h, kp, kn) => callMethod(h, str(kp, kn), [])),
    callstr: guard((h, kp, kn, vp, vn) => callMethod(h, str(kp, kn), [str(vp, vn)])),
    listen: guard((h, tp, tn, fn, ctx) => {
      const target = obj(h);
      if (!target || !target.addEventListener) return E_HANDLE;
      const type = str(tp, tn);
      const reg = { target, type, cb: fn, ctx, fn: null };
      const id = alloc(reg, "listener");
      if (id < 0) return id;
      // The event handle is borrowed: it ends when the callback returns
      reg.fn = (e) => { const ev = alloc(e, "value", false, true); invoke(id, ev); if (ev > 0 && slot(ev)) drop(ev); };
      target.addEventListener(type, reg.fn);
      return id;
    }),
    timer: guard((ms, repeat, fn, ctx) => {
      const reg = { repeat: !!repeat, id: 0, cb: fn, ctx };
      const h = alloc(reg, "timer");
      if (h < 0) return h;
      const tick = () => {
        invoke(h, 0);
        if (!reg.repeat && slot(h, "timer")) drop(h); // a one-shot handle ends after its callback
      };
      reg.id = reg.repeat ? setInterval(tick, ms) : setTimeout(tick, ms);
      return h;
    }),
    fetch: guard((mp, mn, up, un, hp, hn, bp, bn, fn, ctx) => {
      const method = str(mp, mn) || "GET", url = str(up, un), body = str(bp, bn);
      const headers = new Headers();
      for (const line of str(hp, hn).split("\n")) {
        if (!line.trim()) continue;
        const i = line.indexOf(":");
        if (i <= 0) { lastError = `malformed header line: ${line}`; return E_ARG; }
        headers.append(line.slice(0, i).trim(), line.slice(i + 1).trim());
      }
      const reg = { abort: new AbortController(), cb: fn, ctx };
      const h = alloc(reg, "fetch");
      if (h < 0) return h;
      const done = (res) => {
        if (!slot(h, "fetch")) return; // released (aborted) or program ended: never call back
        const ev = alloc(res, "value", false, true);
        invoke(h, ev);
        if (slot(ev)) drop(ev);
        if (slot(h, "fetch")) drop(h);
      };
      const init = { method, headers, signal: reg.abort.signal };
      if (body && method !== "GET" && method !== "HEAD") init.body = body;
      fetch(url, init).then(async (r) => {
        const hdrs = {};
        r.headers.forEach((v, k) => { hdrs[k] = v; });
        let text;
        try { text = await r.text(); } catch (e) { return done({ status: r.status, ok: false, error: "invalid", message: String(e), body: "", headers: hdrs }); }
        done({ status: r.status, ok: r.ok, error: r.ok ? "" : "http", message: r.statusText, body: text, headers: hdrs });
      }, (e) => {
        if (e && e.name === "AbortError") return;
        done({ status: 0, ok: false, error: "network", message: String(e && e.message), body: "", headers: {} });
      });
      return h;
    }),
  };
  function callMethod(h, name, args) {
    const o = obj(h); if (!o) return E_HANDLE;
    if (typeof o[name] !== "function") { lastError = `${name} is not a function`; return E_TYPE; }
    o[name](...args);
    return OK;
  }

  const { instance } = await WebAssembly.instantiate(bytes, { salivo, browser });
  exports = instance.exports;
  memory = exports.memory;
  try { exports._start(); }
  catch (e) {
    // main returned: status 0 keeps the module alive for its callbacks; anything else ends it
    if (e instanceof Exit) { if (e.code !== 0) stop(e.code, null); else { flush(); app.status = 0; } }
    else stop(134, `wasm trap: ${e.message}`);
  }
  app.stats = () => ({ ...counts, dispatched: app.dispatched, memoryBytes: memory.buffer.byteLength });
  app.exit = (code = 0) => stop(code, null);
  return app;
}
