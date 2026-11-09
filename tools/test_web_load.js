/* 冒烟测试：把 web/js 下 11 个脚本按 index.html 的顺序在一个假的 DOM 里跑一遍，
 * 只检查"加载期"有没有报错（语法、TDZ、引用了不存在的全局、IIFE 里写错的初始化）。
 * 不做真实渲染，也不碰设备。用法：node DongleRE/tools/test_web_load.js */
'use strict';
const fs = require('fs');
const vm = require('vm');
const WEB = require('path').join(__dirname, '..', 'webgui', 'js') + '/';
const ORDER = ['protocol.js', 'stratagems.js', 'macros.js', 'pad-art.js',
               'page-map.js', 'page-config.js', 'page-test.js',
               'flash-blisp.js', 'flash-flasher.js', 'page-flash.js', 'app.js'];

/* ---- 最小 DOM 桩：够脚本在加载期用就行 ---- */
const noop = () => {};
const mkEl = () => new Proxy({
  style: {}, dataset: {}, classList: { add: noop, remove: noop, toggle: noop, contains: () => false },
  children: [], value: '', textContent: '', innerHTML: '', checked: false,
  addEventListener: noop, removeEventListener: noop, appendChild: noop, removeChild: noop,
  querySelector: () => mkEl(), querySelectorAll: () => [], closest: () => null,
  setAttribute: noop, getAttribute: () => null, remove: noop, click: noop,
}, {
  get(t, k) { return k in t ? t[k] : (typeof k === 'string' ? mkEl() : undefined); },
});

const document = {
  getElementById: () => null, querySelector: () => null, querySelectorAll: () => [],
  createElement: () => mkEl(), createElementNS: () => mkEl(),
  addEventListener: noop, removeEventListener: noop,
  body: mkEl(), documentElement: mkEl(), hidden: false,
};

const sandbox = {
  console, setTimeout, clearTimeout, setInterval, clearInterval,
  performance: { now: () => Date.now() },
  Date, Math, JSON, Promise, Set, Map, Uint8Array, Uint16Array, Int8Array, DataView,
  ArrayBuffer, TextEncoder, TextDecoder, Blob: class {}, URL: { createObjectURL: () => '', revokeObjectURL: noop },
  fetch: () => Promise.reject(new Error('no net')),
  document,
  navigator: { hid: undefined, getGamepads: undefined, userAgent: 'node', clipboard: {} },
  localStorage: { getItem: () => null, setItem: noop, removeItem: noop },
  location: { protocol: 'http:', href: 'http://127.0.0.1:8080/', search: '' },
  XMLHttpRequest: class {},
};
sandbox.window = sandbox;
sandbox.globalThis = sandbox;
sandbox.window.addEventListener = noop;
sandbox.window.removeEventListener = noop;
sandbox.window.localStorage = sandbox.localStorage;

const ctx = vm.createContext(sandbox);
let bad = 0;
for (const f of ORDER) {
  try {
    vm.runInContext(fs.readFileSync(WEB + f, 'utf8'), ctx, { filename: f });
    console.log('ok   ' + f);
  } catch (e) {
    bad++;
    console.log('FAIL ' + f + ' → ' + e.message);
  }
}

/* 加载完之后，把这次改动的新接口点一遍名 */
const checks = [
  ['PadArt', 'object'],
  ['MapPage', 'object'],
  ['Macros', 'object'],
  ['TestPage', 'object'],
  ['ConfigPage', 'object'],
];
for (const [name, kind] of checks) {
  const got = vm.runInContext(`typeof ${name}`, ctx);
  if (got !== kind) { bad++; console.log(`FAIL 全局 ${name} 类型 ${got} ≠ ${kind}`); }
  else console.log(`ok   全局 ${name} = ${got}`);
}
const fn = vm.runInContext('typeof PadArt.buttonsFromReport', ctx);
if (fn !== 'function') { bad++; console.log('FAIL PadArt.buttonsFromReport 不是函数'); }
const padMapGone = vm.runInContext('typeof Macros.PAD_MAP', ctx);
if (padMapGone !== 'undefined') { bad++; console.log('FAIL Macros.PAD_MAP 应该已删除'); }
else console.log('ok   Macros.PAD_MAP 已删除');

console.log(bad ? `\n${bad} 项失败` : '\n全部通过');
process.exit(bad ? 1 : 0);
