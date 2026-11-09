/* 校验网页的高级配置字节布局和固件结构体是否逐字节一致：
 *   ① src/config.h 的 config_body 字段偏移 ↔ web/js/page-config.js 的 OFF
 *   ② SIZE == sizeof(struct)  ③ CFG_VERSION == CONFIG_VERSION
 *   ④ FIELDS 里出现的每个键都在 OFF 里（否则界面上是个永远读不到值的控件）
 * 只读，不改任何东西。用法：node DongleRE/tools/test_config_layout.js */
'use strict';
const fs = require('fs');
const FW = require('path').join(__dirname, '..', 'firmware', 'src', 'config.h');
const WEB = require('path').join(__dirname, '..', 'webgui', 'js', 'page-config.js');

const bad = [];
const ok = m => console.log('ok   ' + m);
const fail = m => { bad.push(m); console.log('FAIL ' + m); };

/* ---- 固件结构体 ---- */
const h = fs.readFileSync(FW, 'utf8');
const body = h.match(/struct __attribute__\(\(packed\)\) config_body \{([\s\S]*?)\n\};/);
if (!body) { console.log('FAIL 在 config.h 里找不到 config_body'); process.exit(1); }
const fields = [];
let off = 0;
for (const line of body[1].split('\n')) {
  const m = line.replace(/\/\*[\s\S]*?\*\//g, '').match(/^\s*(uint8_t|float)\s+(\w+)\s*;/);
  if (!m) continue;
  const size = m[1] === 'float' ? 4 : 1;
  fields.push({ name: m[2], off, size });
  off += size;
}
const SIZE = off;
const ver = +(h.match(/#define\s+CONFIG_VERSION\s+(\d+)/) || [])[1];
if (!ver) { console.log('FAIL 找不到 CONFIG_VERSION'); process.exit(1); }
ok(`固件 config_body = ${SIZE} 字节，${fields.length} 个字段，CONFIG_VERSION=${ver}`);

/* ---- 网页 OFF / SIZE / CFG_VERSION ---- */
const js = fs.readFileSync(WEB, 'utf8');
const offBlock = js.match(/const OFF = \{([\s\S]*?)\n  \};/);
if (!offBlock) { console.log('FAIL 在 page-config.js 里找不到 OFF'); process.exit(1); }
const webOff = {};
for (const m of offBlock[1].matchAll(/(\w+)\s*:\s*(\d+)/g)) webOff[m[1]] = +m[2];
const webSize = +(js.match(/const SIZE = (\d+)/) || [])[1];
const webVer = +(js.match(/const CFG_VERSION = (\d+)/) || [])[1];

/* ① 固件字段 → OFF 偏移 */
for (const f of fields) {
  if (!(f.name in webOff)) {
    /* config_version 之外的字段网页必须登记，否则保存时会被写成 0 */
    fail(`固件字段 ${f.name}（off ${f.off}）在网页 OFF 里没有登记`);
  } else if (webOff[f.name] !== f.off) {
    fail(`${f.name}: 固件 off=${f.off} ≠ 网页 off=${webOff[f.name]}`);
  }
}
/* 反向：OFF 里的键必须真的存在于结构体 */
for (const k of Object.keys(webOff)) {
  if (!fields.some(f => f.name === k)) fail(`网页 OFF 里的 ${k} 在固件结构体里不存在`);
}
if (!bad.length) ok(`OFF 与结构体逐字段一致（${Object.keys(webOff).length} 项）`);

/* ② ③ 大小和版本 */
if (webSize === SIZE) ok(`SIZE = ${webSize} == sizeof(config_body)`);
else fail(`SIZE = ${webSize} ≠ sizeof(config_body) = ${SIZE}`);
if (webVer === ver) ok(`CFG_VERSION = ${webVer} == CONFIG_VERSION`);
else fail(`CFG_VERSION = ${webVer} ≠ CONFIG_VERSION = ${ver}`);

/* ④ FIELDS 的键都在 OFF 里（t:'color' 是合成控件 led_rgb → led_r/g/b，不用登记） */
const fieldBlock = js.match(/const FIELDS = \[([\s\S]*?)\n  \];/);
const items = [...fieldBlock[1].matchAll(/\{\s*k:\s*'(\w+)',\s*t:\s*'(\w+)'/g)].map(m => ({ k: m[1], t: m[2] }));
const missing = items.filter(i => i.t !== 'color' && !(i.k in webOff)).map(i => i.k);
if (!missing.length) ok(`FIELDS 的 ${items.length} 个控件键都在 OFF 里（合成控件已排除）`);
else fail(`FIELDS 里这些键不在 OFF 里：${missing.join(', ')}`);

/* 顺手把映射打出来，方便和 config.h 对照 */
console.log('\n字段 → 偏移：' + fields.map(f => `${f.name}@${f.off}`).join('  '));
console.log(bad.length ? `\n${bad.length} 项不一致` : '\n全部通过');
process.exit(bad.length ? 1 : 0);
