// Host test for the BossQuickDock control page script (node): runs the
// <script> of src/web/index.html against a fake DOM and XHR. Reads are GET
// without the token, every action is a POST carrying the token from
// /api/status, a stale token is fetched again once, codec buttons follow
// the headset's codec_avail bits (aptX included).
// Usage: node tests/test_page.js src/web/index.html
const fs = require('fs');
const html = fs.readFileSync(process.argv[2], 'utf8');
const js = html.match(/<script>([\s\S]*)<\/script>/)[1];
const TOKEN = 'deadbeefdeadbeefdeadbeefdeadbeef';

const els = {};
function mkEl(id) {
  return { id, style: { setProperty(k, v) { this[k] = v; } }, className: '', textContent: '', innerHTML: '', value: 0,
    min: 0, max: 100, disabled: false, attrs: {}, clientWidth: 0, clientHeight: 0,
    setAttribute(k, v) { this.attrs[k] = v; }, getAttribute(k) { return this.attrs[k] === undefined ? null : this.attrs[k]; },
    querySelectorAll: () => [], getContext: () => ({}), appendChild() {}, click() {} };
}
function el(id) { return els[id] || (els[id] = mkEl(id)); }
// The codec buttons, as many as the page has.
const codecBtns = [...html.matchAll(/<button class="b" data-m="(\d)">([^<]*)<\/button>/g)].map(m => {
  const b = mkEl('codec' + m[1]); b.attrs['data-m'] = m[1]; b.label = m[2]; return b;
});

let STATUS = {};
const calls = [];
let reply = () => [200, STATUS];
class XHR {
  open(m, p) { this.m = m; this.p = p; this.h = {}; }
  setRequestHeader(k, v) { this.h[k] = v; }
  send() {
    calls.push({ m: this.m, p: this.p, h: this.h });
    const [st, body] = reply(this.m, this.p, this.h);
    this.status = st; this.responseText = JSON.stringify(body);
    if (this.onload) this.onload();
  }
}
const timers = [];
const ctx = {
  document: { getElementById: el, querySelectorAll: q => (q === '#codec .b' ? codecBtns : []), querySelector: () => null,
    addEventListener() {}, documentElement: { style: { setProperty() {} } }, createElement: () => mkEl('a'), body: mkEl('body') },
  window: { addEventListener() {}, requestAnimationFrame() {} },
  XMLHttpRequest: XHR, localStorage: { getItem: () => null, setItem() {} }, confirm: () => true,
  setTimeout(fn) { timers.push(fn); return timers.length; }, setInterval() {}, clearTimeout() {},
  Date, JSON, Math, String, parseInt, URL: {}, Blob: function () {}, FileReader: function () {},
};

function status(extra) {
  return Object.assign({ version: '1.1.0', token: TOKEN, state: 'streaming', connected: 1, device: 'Buds', gain_pct: 250,
    muted: 0, tone: 0, paused: 0, headset_volume: 64, avrcp: { connected: 1, absolute_volume: 1 }, pkts: 10, frames: 40,
    peak: 0.2, out_peak: 0.3, sample_rate: 48000, bitpool: 35, bitpool_min: 2, bitpool_max: 53, dropped: 0,
    latency: { target_ms: 200, estimate_ms: 187, capture_ms: 21, packet_ms: 11, queue_ms: 20, radio_ms: 5, sink_ms: 130 },
    codec: 'SBC', codec_pref: 0, codec_avail: 2, eq: { on: 0, db: [0, 0, 0, 0, 0] }, events: [], stream_s: 5, uptime_s: 9 }, extra);
}
STATUS = status();
new Function(...Object.keys(ctx), js)(...Object.values(ctx));

let fails = 0;
const check = (c, m) => { console.log((c ? 'ok   ' : 'FAIL ') + m); if (!c) fails++; };
function tick() { const fns = timers.splice(0); for (const f of fns) f(); }   // run what the page scheduled

check(calls.length && calls[0].m === 'GET' && calls[0].p === '/api/status' && !calls[0].h['X-HB-Token'],
  'page: first request is a plain GET of /api/status, no token');
check(!calls.some(c => c.m === 'POST'), 'page: loading the page changes nothing on the console (no POST)');
check(!calls.some(c => c.p.indexOf('/api/scan') === 0), 'page: loading the page never starts a scan');

const actions = [['mute', '/api/mute?on=1'], ['hTone', '/api/tone?on=1'], ['tClean', '/api/clean'],
  ['tNight', '/api/night?on=1'], ['tAuto', '/api/latency?auto=1'], ['hConn', '/api/disconnect']];
for (const [id, path] of actions) {
  calls.length = 0; el(id).onclick();
  const c = calls[0];
  check(c && c.m === 'POST' && c.p === path && c.h['X-HB-Token'] === TOKEN, `page: ${id} -> POST ${path} with the token`);
}
calls.length = 0; el('gain').value = 300; el('gain').onchange.call(el('gain'));
check(calls[0] && calls[0].m === 'POST' && calls[0].p === '/api/volume?pct=300' && calls[0].h['X-HB-Token'] === TOKEN,
  'page: volume slider -> POST with the token');
calls.length = 0; el('lat').value = 120; el('lat').onchange.call(el('lat'));
check(calls[0] && calls[0].p === '/api/latency?ms=120' && calls[0].m === 'POST', 'page: latency slider -> POST');
check(/id="lat" min="40" max="400" step="1"/.test(html), 'page: latency slider goes 40-400 ms in 1 ms steps');

// Stale token (the app restarted): the page fetches /api/status again and retries once.
{
  let n = 0;
  reply = (m, p) => (m === 'POST' && n++ === 0 ? [403, { error: 'token' }] : [200, STATUS]);
  calls.length = 0; el('mute').onclick();
  check(calls.length === 3 && calls[0].m === 'POST' && calls[1].m === 'GET' && calls[1].p === '/api/status' &&
    calls[2].m === 'POST' && calls[2].p === calls[0].p, 'page: 403 -> status fetched again, the action retried once');
  reply = (m) => (m === 'POST' ? [403, { error: 'token' }] : [200, STATUS]);
  calls.length = 0; el('mute').onclick();
  check(calls.length === 3 && /Reload this page/.test(el('msg').textContent), 'page: still 403 -> asks to reload, no loop');
  reply = () => [200, STATUS];
}

// Codec buttons.
const btn = m => codecBtns.find(b => b.attrs['data-m'] === String(m));
check(codecBtns.map(b => b.label).join(',') === 'Auto,SBC,SBC HQ,SBC-XQ,aptX', 'page: codec choices Auto, SBC, SBC HQ, SBC-XQ, aptX');
function show(st) { STATUS = st; el('mute').onclick(); }   // any action answers with the status and redraws
show(status({ codec_avail: (1 << 1) | (1 << 4) }));
check(!btn(0).disabled && !btn(1).disabled && btn(2).disabled && btn(3).disabled && !btn(4).disabled,
  'page: aptX headset (SBC + aptX): HQ and XQ greyed out, aptX available');
show(status({ codec_avail: (1 << 1) | (1 << 2) | (1 << 3), codec_pref: 3 }));
check(!btn(2).disabled && !btn(3).disabled && btn(4).disabled && /\bon\b/.test(btn(3).className),
  'page: Xbox-like headset (SBC/HQ/XQ): aptX greyed out, the picked codec is highlighted');
show(status({ codec_avail: 0 }));
check(btn(2).disabled && btn(3).disabled && btn(4).disabled && !btn(1).disabled, 'page: nothing connected: only Auto and SBC');
show(status({ codec_avail: (1 << 1) | (1 << 4) }));
calls.length = 0; btn(4).onclick.call(btn(4));
check(calls[0] && calls[0].m === 'POST' && calls[0].p === '/api/codec?mode=4' && calls[0].h['X-HB-Token'] === TOKEN,
  'page: aptX button -> POST /api/codec?mode=4 with the token');

// What the page shows for the codec in use.
show(status({ codec: 'SBC', bitpool: 35 }));
check(/bitpool 35/.test(el('hchips').innerHTML) && /bitpool 35 \(2/.test(el('kv').innerHTML), 'page: SBC shows its bitpool');
show(status({ codec: 'aptX', bitpool: 0, codec_pref: 4, codec_avail: (1 << 1) | (1 << 4) }));
check(/aptX/.test(el('hchips').innerHTML) && !/bitpool/.test(el('hchips').innerHTML) && !/bitpool/.test(el('kv').innerHTML),
  'page: aptX shows the codec and no bitpool (it has none)');
check(/aptX/.test(el('codecNote').textContent), 'page: the codec note mentions aptX');

// Polling keeps going on its own timer.
calls.length = 0; tick();
check(calls.some(c => c.m === 'GET' && c.p === '/api/status'), 'page: status polled again on its timer');

check(!/https?:\/\/(?!127\.0\.0\.1)[^"'\s]*\.(js|css|woff2?)/.test(html), 'page: no scripts, styles or fonts from the internet');
console.log(fails ? `FAILED (${fails})` : 'ALL OK (0 failures)');
process.exit(fails ? 1 : 0);
