/* protocol.js 分块读写的回归测试（不需要真设备）：
 * 假设备完全照固件行为回包 —— 0xFC 的 GET 响应 = [0xFC][5 字节 RPC 头][数据 ≤121]
 *   ① readRegion(0x01, 0, 6150) 必须完整拿回 6150 字节（R2.5 起映射表 41 键；
 *      当年坏掉时是 5850 —— 数字只影响分块数，验证的是分块逻辑）
 *   ② 老逻辑（请求 122 + `d.length < n 就 break`）在同一台假设备上只能拿回 121 字节
 *      —— 作为反面对照，证明修的是真问题
 *   ③ writeRegion 应该按 122 发（请求缓冲整 127 字节都是数据）
 *   ④ 小区域（META 92 字节）读写照旧
 * 用法：node DongleRE/tools/test_protocol_chunk.js */
'use strict';
const fs = require('fs');
const vm = require('vm');
const PROTO = require('path').join(__dirname, '..', 'webgui', 'js', 'protocol.js');

/* ---- 假固件：区域 0x01 = 6150 字节的规律数据（R2.5 的真实大小），响应数据上限 121 ---- */
const REGION_LEN = 6150;
const RESP_DATA_CAP = 121;
const region = new Uint8Array(REGION_LEN);
for (let i = 0; i < REGION_LEN; i++) region[i] = (i * 7 + 3) & 0xFF;

const stats = { reads: 0, writes: [], maxWriteData: 0 };
let lastResp = null;

const dev = {
  opened: true,
  productName: 'fake-dongle',
  collections: [{ featureReports: [{ reportId: 0xFC }] }],
  async open() {},
  async close() {},
  addEventListener() {}, removeEventListener() {},
  async sendFeatureReport(id, buf) {
    const op = buf[0], reg = buf[1], off = buf[2] | (buf[3] << 8), len = buf[4];
    const hdr = new Uint8Array(6);
    hdr[0] = 0xFC; hdr[1] = 0;
    hdr[2] = op; hdr[3] = reg; hdr[4] = off & 0xFF; hdr[5] = (off >> 8) & 0xFF;
    if (op === 1) {                       /* READ */
      stats.reads++;
      const d = region.subarray(off, off + Math.min(len, RESP_DATA_CAP));  /* 从区域取，且只回 121 */
      lastResp = new Uint8Array(6 + d.length);
      lastResp.set(hdr, 0); lastResp.set(d, 6);
    } else if (op === 2) {                /* WRITE */
      stats.writes.push({ off, len });
      stats.maxWriteData = Math.max(stats.maxWriteData, len);
      region.set(buf.subarray(5, 5 + len), off);
      lastResp = hdr;
    } else {                              /* COMMIT 之类 */
      lastResp = hdr;
    }
  },
  async receiveFeatureReport() {
    return new DataView(lastResp.buffer, lastResp.byteOffset, lastResp.byteLength);
  },
};

const sandbox = {
  console, setTimeout, clearTimeout, setInterval, clearInterval,
  Promise, Math, JSON, Date, Set, Map, Uint8Array, DataView, ArrayBuffer,
  /* r34 起 protocol.js 会挂 navigator.hid 的 disconnect 监听，桩里必须带 addEventListener */
  navigator: { hid: { getDevices: async () => [dev], requestDevice: async () => [dev],
                      addEventListener: () => {} } },
};
sandbox.window = sandbox;
const ctx = vm.createContext(sandbox);
vm.runInContext(fs.readFileSync(PROTO, 'utf8'), ctx, { filename: 'protocol.js' });

let bad = 0;
const check = (name, ok, extra) => {
  if (ok) console.log('ok   ' + name);
  else { bad++; console.log('FAIL ' + name + (extra ? ' -> ' + extra : '')); }
};

(async () => {
  const DS5 = vm.runInContext('DS5', ctx);
  const dev2 = await DS5.reconnect();
  check('连上假设备', !!dev2);

  const expect = i => (i * 7 + 3) & 0xFF;

  /* ① 新逻辑：完整读回 */
  stats.reads = 0;
  const got = await DS5.readRegion(0x01, 0, REGION_LEN);
  let mism = 0;
  for (let i = 0; i < REGION_LEN; i++) if (got[i] !== expect(i)) { mism++; if (mism < 4) console.log('     首处不符 @' + i); }
  check(`① readRegion 完整读回 ${REGION_LEN} 字节（发了 ${stats.reads} 块）`, mism === 0, mism + ' 字节不符');
  check('① 偏移 1260（layer1 △ 单击）不是 0', got[1260] !== 0, 'got[1260]=' + got[1260]);

  /* ② 反面对照：老逻辑（122 + 短了就 break） */
  const old = new Uint8Array(REGION_LEN);
  let g = 0;
  while (g < REGION_LEN) {
    const n = Math.min(122, REGION_LEN - g);
    const d = await DS5.rpc(DS5.OP.READ, 0x01, g, new Uint8Array([n]));
    old.set(d.subarray(0, n), g);
    g += Math.min(d.length, n);
    if (d.length < n) break;                 /* ← 老代码 */
  }
  check('② 老逻辑只能拿到 121 字节就"读完了"', g === 121, '实际 ' + g);
  check('② 老逻辑下偏移 1260 是 0（＝网页看不到那条映射）', old[1260] === 0, 'old[1260]=' + old[1260]);

  /* ③ 写：按 122 分块 */
  stats.writes.length = 0;
  const payload = new Uint8Array(REGION_LEN);
  for (let i = 0; i < payload.length; i++) payload[i] = (i * 3) & 0xFF;
  await DS5.writeRegion(0x01, 0, payload);
  check(`③ 6150 字节写入分成 ${stats.writes.length} 块，最大数据 ${stats.maxWriteData} 字节（应为 122）`,
        stats.writes.length === 51 && stats.maxWriteData === 122);
  let wmism = 0;
  for (let i = 0; i < REGION_LEN; i++) if (region[i] !== ((i * 3) & 0xFF)) wmism++;
  check('③ 写进去的字节和发出去的一致', wmism === 0, wmism + ' 字节不符');

  /* ④ 小区域照旧 */
  const meta = await DS5.readRegion(0x01, 0, 92);
  check('④ 92 字节小区域读取正常', meta.length === 92 && meta[10] === ((10 * 3) & 0xFF));

  console.log(bad ? `\n${bad} 项失败` : '\n全部通过');
  process.exit(bad ? 1 : 0);
})();
