/* 校验 web/js/pad-art.js 的 buttonsFromReport()：
 *   ① 逐位对照固件 ds5_protocol.h 的按键位定义
 *   ② 与 page-test.js 里那份 BIT/HAT 表逐条比对（两份必须完全一致）
 * 只读文件，不改任何东西。用法：node DongleRE/tools/test_buttons_from_report.js */
'use strict';
const fs = require('fs');
const WEB = require('path').join(__dirname, '..', 'webgui', 'js') + '/';

const PadArt = eval(fs.readFileSync(WEB + 'pad-art.js', 'utf8') + '\nPadArt;');

/* ---- ② 从 page-test.js 抠出 BIT / HAT 表 ---- */
const test = fs.readFileSync(WEB + 'page-test.js', 'utf8');
const bitTxt = test.match(/const BIT = \[([\s\S]*?)\];/)[1];
const hatTxt = test.match(/const HAT = (\{[^;]*\});/)[1];
const BIT2 = eval('[' + bitTxt + ']');
const HAT2 = eval('(' + hatTxt + ')');

let bad = 0;
const eq = (name, got, want) => {
  const g = [...got].sort((a, b) => a - b).join(',');
  const w = [...want].sort((a, b) => a - b).join(',');
  if (g !== w) { bad++; console.log(`FAIL ${name}: got [${g}] want [${w}]`); }
  else console.log(`ok   ${name} → [${g}]`);
};

/* ---- ① 已知报文 ---- */
const mk = (b7, b8, b9) => {
  const p = new Uint8Array(63);
  p[0] = p[1] = p[2] = p[3] = 0x80;   /* 摇杆回中 */
  p[7] = b7; p[8] = b8; p[9] = b9;
  return p;
};
eq('回中（hat=8）', PadArt.buttonsFromReport(mk(0x08, 0, 0)), []);
eq('□', PadArt.buttonsFromReport(mk(0x18, 0, 0)), [0]);
eq('□+○', PadArt.buttonsFromReport(mk(0x58, 0, 0)), [0, 2]);
eq('△+×', PadArt.buttonsFromReport(mk(0xa8 >> 0, 0, 0)), [1, 3]);
eq('十字↑', PadArt.buttonsFromReport(mk(0x00, 0, 0)), [15]);
eq('十字↘', PadArt.buttonsFromReport(mk(0x03, 0, 0)), [16, 18]);
eq('十字↙', PadArt.buttonsFromReport(mk(0x05, 0, 0)), [16, 17]);
eq('十字←', PadArt.buttonsFromReport(mk(0x06, 0, 0)), [17]);
eq('L1+R2', PadArt.buttonsFromReport(mk(0x08, 0x09, 0)), [4, 7]);
eq('创建+选项+L3+R3', PadArt.buttonsFromReport(mk(0x08, 0xf0, 0)), [8, 9, 10, 11]);
eq('PS+静音', PadArt.buttonsFromReport(mk(0x08, 0, 0x05)), [12, 14]);
eq('PS+触摸板+静音+bit3', PadArt.buttonsFromReport(mk(0x08, 0, 0x0f)), [12, 13, 14]);
eq('DSE 四键', PadArt.buttonsFromReport(mk(0x08, 0, 0xf0)), [31, 32, 33, 34]);
eq('□ + 十字→ + L1 + PS', PadArt.buttonsFromReport(mk(0x12, 0x01, 0x01)), [0, 4, 12, 18]);

/* 缓冲区从没被写过 / 报文太短 → null */
const zero = new Uint8Array(63);
if (PadArt.buttonsFromReport(zero) !== null) { bad++; console.log('FAIL 全 0 缓冲应返回 null'); }
else console.log('ok   全 0 缓冲 → null');
const short = new Uint8Array(9);
short.fill(0x80);
if (PadArt.buttonsFromReport(short) !== null) { bad++; console.log('FAIL 9 字节应返回 null'); }
else console.log('ok   9 字节 → null');

/* DataView（WebHID 输入端报文那条路） */
const dv = new DataView(mk(0xa8, 0x00, 0x00).buffer);
eq('DataView: ×+△', PadArt.buttonsFromReport(dv), [1, 3]);

/* ---- ② 两份表逐条比对 ---- */
const mine = PadArt.buttonsFromReport.toString();   /* 只用来确认函数存在 */
if (!mine) { bad++; console.log('FAIL buttonsFromReport 不存在'); }
const pairs = (tbl) => tbl.map(([o, m, id]) => `${o}:${m}:${id}`).join('|');
/* pad-art.js 的表没有导出，用一次"全 1 报文"反推：每个 bit 单独测一遍 */
const oneBit = (o, m) => {
  const p = new Uint8Array(63);
  p[0] = 0x80;
  p[o] = m;
  return PadArt.buttonsFromReport(p);
};
let agree = 0;
for (const [o, m, id] of BIT2) {
  const got = oneBit(o, m);
  if (!got || !got.has(id)) { bad++; console.log(`FAIL page-test 对齐 ${o}/${m.toString(16)} 期望 id=${id}`); }
  else agree++;
}
console.log(`ok   与 page-test.js 的 BIT 表一致：${agree}/${BIT2.length} 条`);
/* HAT 表：0..7 逐个比对（注意 id 是按键 id，两表都应该是同样的集合） */
for (let h = 0; h < 8; h++) {
  const p = new Uint8Array(63);
  p[0] = 0x80; p[7] = h;
  eq(`hat=${h}`, PadArt.buttonsFromReport(p), HAT2[h]);
}
/* hat 8..15 = 没按 */
for (let h = 8; h < 16; h++) {
  const p = new Uint8Array(63);
  p[0] = 0x80; p[7] = h;
  eq(`hat=${h}（空）`, PadArt.buttonsFromReport(p), []);
}

console.log(bad ? `\n${bad} 项不一致` : '\n全部通过');
process.exit(bad ? 1 : 0);
