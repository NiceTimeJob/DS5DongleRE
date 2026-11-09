/* 宏录制器的手柄按键录制（假 DOM + 假 0x10 遥测）：
 * 开始录制 → 手柄 □ 按下 → 松开，events 里应该出现 ACT_PAD 的按下/松开两条。
 * 与 vm 之间只传函数引用。用法：node DongleRE/tools/test_macro_record.js */
'use strict';
const fs = require('fs');
const vm = require('vm');
const WEB = require('path').join(__dirname, '..', 'webgui', 'js') + '/';
const ORDER = ['protocol.js', 'stratagems.js', 'macros.js', 'pad-art.js', 'page-map.js'];

const noop = () => {};
const mkEl = () => new Proxy({
  style: {}, dataset: {}, classList: { add: noop, remove: noop, toggle: noop, contains: () => false },
  addEventListener: noop, removeEventListener: noop, appendChild: noop,
  querySelector: () => mkEl(), querySelectorAll: () => [], closest: () => null,
  setAttribute: noop, getAttribute: () => null, remove: noop, click: noop,
  value: '', textContent: '', innerHTML: '', scrollTop: 0, scrollHeight: 0,
}, { get(t, k) { return k in t ? t[k] : (typeof k === 'string' ? mkEl() : undefined); } });

let current = null;
let api = null;
const toasts = [];
const sandbox = {
  console, setTimeout, clearTimeout, setInterval, clearInterval,
  performance: { now: () => Date.now() },
  Date, Math, JSON, Promise, Set, Map, Uint8Array, Uint16Array, Int8Array, DataView, ArrayBuffer,
  Blob: class {}, URL: { createObjectURL: () => '', revokeObjectURL: noop },
  fetch: () => Promise.reject(new Error('no net')),
  document: {
    getElementById: () => null, querySelector: () => null, querySelectorAll: () => [],
    createElement: () => mkEl(), createElementNS: () => mkEl(),
    addEventListener: noop, removeEventListener: noop, body: mkEl(), hidden: false,
  },
  navigator: { userAgent: 'node' },
  localStorage: { getItem: () => null, setItem: noop, removeItem: noop },
  location: { protocol: 'http:', href: 'http://127.0.0.1:8080/' },
  App: { toast: (m, e) => toasts.push([m, !!e]), switchPage: noop, init: noop },
  __readTlm: async () => current,
  __expose: o => { api = o; },
};
sandbox.window = sandbox;
sandbox.addEventListener = noop;
sandbox.removeEventListener = noop;
const ctx = vm.createContext(sandbox);

for (const f of ORDER) {
  let src = fs.readFileSync(WEB + f, 'utf8');
  if (f === 'macros.js') {
    /* 把录制状态挂到返回对象上（源文件不动）。锚点用单行，免得踩 CRLF/LF 的坑。 */
    const anchor = '    startRecording,';
    if (!src.includes(anchor)) { console.log('FAIL 找不到 macros.js 的导出，测试没生效'); process.exit(1); }
    src = src.replace(anchor,
      '    startRecording, __events: () => (rec ? rec.events : null), __stop: stopRecording,');
  }
  vm.runInContext(src, ctx, { filename: f });
}
vm.runInContext('DS5.readTlm = __readTlm;', ctx);
const mac = vm.runInContext('Macros', ctx);
if (typeof mac.startRecording !== 'function' || typeof mac.__events !== 'function') {
  console.log('FAIL 没能拿到录制接口'); process.exit(1);
}

const sleep = ms => new Promise(r => setTimeout(r, ms));
const mk = over => {
  const p = new Uint8Array(63);
  p[0] = p[1] = p[2] = p[3] = 0x80;
  p[7] = 0x08;
  Object.assign(p, over);
  return p;
};
let bad = 0;
const check = (name, ok, extra) => {
  if (ok) console.log('ok   ' + name);
  else { bad++; console.log('FAIL ' + name + (extra ? ' → ' + extra : '')); }
};

(async () => {
  current = mk({});
  mac.startRecording();
  await sleep(40);                       /* 基准帧 */
  current = mk({ 7: 0x18 });             /* □ 按下（byte7 bit0x10 → id 0） */
  await sleep(40);
  current = mk({});                      /* □ 松开 */
  await sleep(40);
  const evs = mac.__events() || [];
  mac.__stop(true);                      /* cancel：不碰 MapPage / 编辑区 */

  /* 事件 = { act, key, edge, t }；ACT_PAD = 3，EDGE_DOWN = 1 / EDGE_UP = 0 */
  const pad = evs.filter(e => e.act === 3);
  check('手柄按键录到了按下 + 松开两条', pad.length === 2,
        JSON.stringify(evs.map(e => [e.act, e.key, e.edge])));
  check('按下 = ACT_PAD key 0 edge 1', pad[0] && pad[0].key === 0 && pad[0].edge === 1,
        JSON.stringify(pad[0]));
  check('松开 = ACT_PAD key 0 edge 0', pad[1] && pad[1].key === 0 && pad[1].edge === 0,
        JSON.stringify(pad[1]));
  check('两次采样之间有时间戳差（dt 有意义）', pad.length === 2 && pad[1].t - pad[0].t >= 15,
        pad.length === 2 ? String(pad[1].t - pad[0].t) : '—');

  console.log(bad ? `\n${bad} 项失败` : '\n全部通过');
  process.exit(bad ? 1 : 0);
})();
