/* 端到端跑一遍「捕获输入」的新代码路径（假 DOM + 假 0x10 遥测，不碰设备）：
 *   ① 基准帧（第一帧只记"现在按着什么"）不触发捕获
 *   ② 空闲 → 按 □  → 捕获到 id 0
 *   ③ 已经按着 □ 时开始捕获 → 松开 → 再按 □  → 仍然能捕获到（基准随帧更新）
 *   ④ 按 DSE 左Fn（id 31）→ 只弹提示、不捕获；接着按 × → 捕获到 id 1
 *   ⑤ 十字键（hat）也能捕获：↑ = id 15
 *   ⑥ 全 0 缓冲（固件还没收到过手柄报文）当成没数据，不误判成「↑ 按着」
 * 与 vm 之间只传函数/对象引用（不往沙箱里补写全局），避免跨 realm 的可见性问题。
 * 用法：node DongleRE/tools/test_capture_flow.js */
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
  value: '', textContent: '', innerHTML: '',
}, { get(t, k) { return k in t ? t[k] : (typeof k === 'string' ? mkEl() : undefined); } });

/* ---- 宿主侧状态（沙箱只通过下面这几个函数/对象引用跟它打交道） ---- */
let current = null;            /* 当前这一帧的"遥测" */
let startCaptureFn = null;     /* page-map 内部的 startCapture */
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
  /* page-map 会调 App.toast；这里不加载 app.js，直接给个假的记下来 */
  App: { toast: (m, e) => toasts.push([m, !!e]), switchPage: noop, init: noop },
  __readTlm: async () => current,
  __expose: fn => { startCaptureFn = fn; },
};
/* window 上的 keydown/keyup/mousedown 要真记下来，测试才点得到 */
const listeners = {};
const addL = (t, fn) => { (listeners[t] = listeners[t] || []).push(fn); };
const delL = (t, fn) => { const a = listeners[t] || []; const i = a.indexOf(fn); if (i >= 0) a.splice(i, 1); };
sandbox.window = sandbox;
sandbox.addEventListener = addL;
sandbox.removeEventListener = delL;
const fire = (type, ev) => (listeners[type] || []).slice().forEach(fn => fn(ev));
const ctx = vm.createContext(sandbox);

for (const f of ORDER) {
  let src = fs.readFileSync(WEB + f, 'utf8');
  if (f === 'page-map.js') {
    /* 只把 startCapture 交给宿主的 __expose（源文件不动）。
     * 注意必须插在 return 之前——插在 IIFE 结尾是执行不到的死代码。 */
    /* 只认行首这一截：后面的导出清单会加东西（r57 加了 markClean），别每次都把这里撞坏 */
    const anchor = '  return { init, state, load, save, refreshMacroDir,';
    if (!src.includes(anchor)) { console.log('FAIL 找不到 page-map.js 的 return，测试没生效'); process.exit(1); }
    src = src.replace(anchor, '  __expose(startCapture);\n' + anchor);
  }
  vm.runInContext(src, ctx, { filename: f });
}
vm.runInContext('DS5.readTlm = __readTlm;', ctx);
if (typeof startCaptureFn !== 'function') { console.log('FAIL 没能拿到 startCapture'); process.exit(1); }

const sleep = ms => new Promise(r => setTimeout(r, ms));
const mk = over => {
  const p = new Uint8Array(63);
  p[0] = p[1] = p[2] = p[3] = 0x80;    /* 摇杆回中 */
  p[7] = 0x08;                         /* hat = 8 = 没按十字键 */
  Object.assign(p, over);
  return p;
};

let bad = 0;
const check = (name, ok, extra) => {
  if (ok) console.log('ok   ' + name);
  else { bad++; console.log('FAIL ' + name + (extra ? ' → ' + extra : '')); }
};

/* 开一次捕获，按步骤喂帧（每步 [帧, 等多久]），返回捕到的结果 */
async function run(initial, steps) {
  current = initial;
  toasts.length = 0;
  const picked = [];
  const btnEl = { isConnected: true, textContent: '', classList: { add: noop, remove: noop } };
  startCaptureFn(btnEl, r => picked.push(r));
  for (const [payload, ms] of steps) {
    current = payload;
    await sleep(ms);
  }
  return { picked, btnEl };
}

(async () => {
  /* ① 一开始就按着 □：基准帧记下它，不该被捕获 */
  let r = await run(mk({ 7: 0x18 }), [[mk({ 7: 0x18 }), 60]]);
  check('① 已经按着的键不会被捕获', r.picked.length === 0, JSON.stringify(r.picked));

  /* ② 空闲 → 按 □ */
  r = await run(mk({}), [[mk({ 7: 0x18 }), 20], [mk({ 7: 0x18 }), 60]]);
  check('② 按 □ 捕获到 id 0', r.picked.length === 1 && r.picked[0].kind === 'pad' && r.picked[0].val === 0,
        JSON.stringify(r.picked));
  check('② 捕获后按钮恢复成「捕获」', r.btnEl.textContent === '捕获', r.btnEl.textContent);

  /* ③ 按住 □ 开始捕获 → 松开 → 再按 □ */
  r = await run(mk({ 7: 0x18 }), [[mk({}), 40], [mk({ 7: 0x18 }), 60]]);
  check('③ 松开再按同一个键能捕获', r.picked.length === 1 && r.picked[0].val === 0, JSON.stringify(r.picked));

  /* ④ DSE 左Fn 只提示不捕获，之后的 ×（byte7 bit0x20）照常捕获 */
  r = await run(mk({}), [[mk({ 9: 0x10 }), 60], [mk({ 7: 0x28 }), 60]]);
  check('④ DSE 左Fn 不产生捕获', r.picked.length === 1 && r.picked[0].val === 1, JSON.stringify(r.picked));
  check('④ 弹过一次「只能当映射源」提示',
        toasts.length === 1 && /只能当映射源/.test(toasts[0][0]), JSON.stringify(toasts));

  /* ⑤ 十字键：↑ = hat 0 → id 15 */
  r = await run(mk({}), [[mk({ 7: 0x00 }), 60]]);
  check('⑤ 十字↑ 捕获到 id 15', r.picked.length === 1 && r.picked[0].val === 15, JSON.stringify(r.picked));

  /* ⑥ 全 0 缓冲被忽略（不误判成「↑ 按着」）：之后第一帧真实报文当基准，再按 □ 照常捕获 */
  r = await run(new Uint8Array(63), [[mk({}), 40], [mk({ 7: 0x18 }), 60]]);
  check('⑥ 全 0 缓冲被忽略，真实帧照常捕获', r.picked.length === 1 && r.picked[0].val === 0,
        JSON.stringify(r.picked));

  /* ⑦ 键盘捕获那条路没被动过：按 A 应该捕到 usage 4 */
  r = await run(mk({}), []);
  fire('keydown', {
    code: 'KeyA', preventDefault: noop, stopPropagation: noop,
  });
  check('⑦ 键盘捕获仍走 DOM 事件（KeyA → usage 4）',
        r.picked.length === 1 && r.picked[0].kind === 'kbd' && r.picked[0].val === 4,
        JSON.stringify(r.picked));

  console.log(bad ? `\n${bad} 项失败` : '\n全部通过');
  process.exit(bad ? 1 : 0);
})();
