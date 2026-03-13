#!/usr/bin/env node
// Headless verification for the wasm/web build.
//
//   node scripts/web_verify.mjs --dir build-wasm-release/examples \
//        [--page probe.html] [--port 8099] [--devtools 9333] [--timeout 90] \
//        [--shot out.png] [--click x,y] [--probe-key __probe]
//
// Why CDP instead of `chrome --headless --screenshot --virtual-time-budget`:
// the virtual clock only fast-forwards timers, while the 16 MB .data preload
// is real network I/O — the screenshot fires long before main() has started.
// This driver waits on real time and asks the page for structured state.
//
// Needs a Node with a global WebSocket (>= 22; emsdk ships one).

import { spawn } from 'node:child_process';
import { mkdtempSync, writeFileSync, existsSync, readdirSync, copyFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';

// The CDP client below needs a global WebSocket (Node >= 22). The host's own
// node is often older, while emsdk always ships a recent one — re-exec under
// it instead of requiring the caller to pick the right interpreter.
if (typeof globalThis.WebSocket === 'undefined' && !process.env.CUI_WEB_NODE_RETRY) {
  const roots = [process.env.EMSDK, '/tools/dev-cache/emsdk',
                 join(process.env.HOME || '', 'emsdk')].filter(Boolean);
  for (const root of roots) {
    let names = [];
    try { names = readdirSync(join(root, 'node')); } catch { continue; }
    for (const n of names.sort().reverse()) {
      const bin = join(root, 'node', n, 'bin', 'node');
      if (!existsSync(bin)) continue;
      process.env.CUI_WEB_NODE_RETRY = '1';
      const child = spawn(bin, process.argv.slice(1),
        { stdio: 'inherit', env: process.env });
      child.on('exit', (code, signal) =>
        process.exit(signal ? 1 : (code ?? 1)));
      // Nothing else should run while the retrying child owns the ports.
      await new Promise(() => {});
    }
  }
  console.error('web_verify: needs Node >= 22 (global WebSocket); ' +
                `current ${process.version}, and no emsdk node found`);
  process.exit(2);
}

const argv = process.argv.slice(2);
function opt(name, def) {
  const i = argv.indexOf('--' + name);
  return i >= 0 && i + 1 < argv.length ? argv[i + 1] : def;
}
function optAll(name) {
  const out = [];
  argv.forEach((a, i) => { if (a === '--' + name && i + 1 < argv.length) out.push(argv[i + 1]); });
  return out;
}

const dir = resolve(opt('dir', '.'));
const page = opt('page', 'probe.html');
const port = opt('port', '8099');
const devtools = opt('devtools', '9333');
const timeoutS = Number(opt('timeout', '90'));
const shot = opt('shot', '');
const key = opt('probe-key', '__probe');
const clicks = optAll('click');
const chromeBin = opt('chrome', process.env.CHROME || '/usr/bin/google-chrome');
const serveBase = `http://127.0.0.1:${port}/`;

// probe.html is a tracked harness that lives next to shell.html; the build
// directory only gets the linker output, so copy it in beside the artifacts.
function findHarness(name) {
  for (let d = resolve(dir); ;) {
    const p = join(d, 'flutter', 'src', 'platform', 'web', name);
    if (existsSync(p)) return p;
    const up = resolve(d, '..');
    if (up === d) return '';
    d = up;
  }
}
if (!existsSync(join(dir, page))) {
  const src = findHarness(page);
  if (src) copyFileSync(src, join(dir, page));
}
if (!existsSync(join(dir, page))) {
  console.error(`web_verify: ${join(dir, page)} not found — build the wasm target first`);
  process.exit(2);
}

// ── static server (file:// refuses to fetch .wasm/.data) ────────────────────
const server = spawn('python3', ['-m', 'http.server', port, '--bind', '127.0.0.1'],
  { cwd: dir, stdio: 'ignore' });

// ── chrome ──────────────────────────────────────────────────────────────────
const profile = mkdtempSync(join(tmpdir(), 'cui_core-web-'));
const chrome = spawn(chromeBin, [
  '--headless=new', '--no-sandbox', '--disable-gpu', '--hide-scrollbars',
  '--window-size=1200x800', `--user-data-dir=${profile}`,
  `--remote-debugging-port=${devtools}`, 'about:blank',
], { stdio: 'ignore' });

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let ws = null;
let msgId = 0;
const pending = new Map();
const events = [];

function send(method, params = {}) {
  return new Promise((res, rej) => {
    const id = ++msgId;
    pending.set(id, { res, rej });
    ws.send(JSON.stringify({ id, method, params }));
  });
}

async function evalJson(expression, awaitPromise = false) {
  const r = await send('Runtime.evaluate', {
    expression, awaitPromise, returnByValue: true,
  });
  if (r.exceptionDetails) {
    throw new Error('page eval failed: ' +
      (r.exceptionDetails.exception?.description || r.exceptionDetails.text));
  }
  return r.result.value;
}

async function waitForTarget() {
  for (let i = 0; i < 100; i++) {
    try {
      const list = await (await fetch(`http://127.0.0.1:${devtools}/json/list`)).json();
      const t = list.find((t) => t.type === 'page');
      if (t) return t;
    } catch { /* chrome still booting */ }
    await sleep(200);
  }
  throw new Error('chrome devtools endpoint never came up');
}

function attach(url) {
  return new Promise((res, rej) => {
    ws = new WebSocket(url);
    ws.onopen = res;
    ws.onerror = (e) => rej(new Error('websocket failed: ' + e.message));
    ws.onmessage = (ev) => {
      const m = JSON.parse(ev.data);
      if (m.id && pending.has(m.id)) {
        const { res, rej } = pending.get(m.id);
        pending.delete(m.id);
        m.error ? rej(new Error(m.error.message)) : res(m.result);
      } else if (m.method) {
        events.push(m);
      }
    };
  });
}

let exitCode = 1;
let report = null;
try {
  const target = await waitForTarget();
  await attach(target.webSocketDebuggerUrl);
  await send('Runtime.enable');
  await send('Page.enable');
  await send('Emulation.setDeviceMetricsOverride',
    { width: 1200, height: 800, deviceScaleFactor: 1, mobile: false });

  await send('Page.navigate', { url: serveBase + page });
  // Wait for the load event, then for the page's own probe object.
  const loadDeadline = Date.now() + 30000;
  while (!events.some((e) => e.method === 'Page.loadEventFired') && Date.now() < loadDeadline)
    await sleep(100);

  const deadline = Date.now() + timeoutS * 1000;
  for (;;) {
    const state = await evalJson(
      `JSON.stringify(window[${JSON.stringify(key)}] || null)`);
    report = state ? JSON.parse(state) : null;
    if (report && report.stats && report.stats.nonBlank > 0) break;
    if (report && report.done) break;
    if (Date.now() > deadline) { report = report || { timeout: true }; break; }
    await sleep(500);
  }

  for (const c of clicks) {
    const [x, y] = c.split(',').map(Number);
    await evalJson(`window[${JSON.stringify(key)}].clickAt(${x}, ${y})`);
    await sleep(1200);
  }
  if (clicks.length) {
    const after = await evalJson(
      `JSON.stringify(window[${JSON.stringify(key)}] || null)`);
    report.afterClick = after ? JSON.parse(after) : null;
  }

  if (shot) {
    const img = await send('Page.captureScreenshot', { format: 'png' });
    writeFileSync(resolve(shot), Buffer.from(img.data, 'base64'), 'base64');
    console.log(`web_verify: screenshot -> ${resolve(shot)}`);
  }
} catch (e) {
  console.error('web_verify: ' + e.message);
  report = report || { error: e.message };
} finally {
  if (ws) try { ws.close(); } catch {}
  chrome.kill('SIGTERM');
  server.kill('SIGTERM');
}

const s = (report?.afterClick?.stats) || report?.stats;
console.log(JSON.stringify({
  painted: !!(s && s.nonBlank > 0),
  canvas: s ? `${s.w}x${s.h}@${s.dpr}` : null,
  nonBlank: s ? s.nonBlank : 0,
  distinctColors: s ? s.distinct : 0,
  topColors: s ? s.top : null,
  // The pre-click pixel signature, so an interaction run shows both sides.
  distinctColorsBeforeClick: report?.stats?.distinct,
  log: (report?.log || []).slice(-15),
  error: report?.error,
}, null, 2));
exitCode = s && s.nonBlank > 0 ? 0 : 1;
process.exit(exitCode);
