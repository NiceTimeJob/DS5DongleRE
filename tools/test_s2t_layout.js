/* 两段式扳机（R2.5）的布局/一致性校验（只读，不改任何东西）：
 *   ① ds5map.h 的 MAP_BTN_COUNT / MAP_BTN_L2_S2 / MAP_BTN_R2_S2 ↔ pad-art.js 的 BTN 表
 *   ② map_s2t_t 的逐字段偏移 + 66 字节 ↔ page-map.js packRegions() 写出的字节位置
 *      （R3.25：整套扳机设置按层 —— [0..13] 主层、[14..25] 覆盖层两段位/分界点、[26..65] 覆盖层自适应扳机）
 *   ③ 新 region 号（S2T 0x0b / FLICK 0x0c / GEST 0x0d）在 ds5map.h、protocol.js、_gmfake.js 三处一致
 *   ④ 配置区大小：SLOTS = 5×41×6×5 = 6150（_gmfake.js 里是写死的，最容易忘）
 *   ⑤ 卡点字节的换算（阈值 → 硬件档位 → 11 字节）：把 ds5map.c 的算法镜像过来，
 *      拿社区实测的两组标准字节对拍（25 80 01 07 / 21 00 01 …）。
 * 用法：node DongleRE/tools/test_s2t_layout.js */
'use strict';
const fs = require('fs');
const p = require('path');
const SRC = p.join(__dirname, '..', 'firmware', 'src');
const JS = p.join(__dirname, '..', 'webgui', 'js');

const bad = [];
const ok = m => console.log('ok   ' + m);
const fail = m => { bad.push(m); console.log('FAIL ' + m); };

const read = f => fs.readFileSync(f, 'utf8');
const h = read(p.join(SRC, 'ds5map.h'));
const ds5C = read(p.join(SRC, 'ds5map.c'));
const mainC = read(p.join(SRC, 'main.c'));
const mapJs = read(p.join(JS, 'page-map.js'));
const protoJs = read(p.join(JS, 'protocol.js'));
const padJs = read(p.join(JS, 'pad-art.js'));
const fakeJs = read(p.join(__dirname, '..', 'webgui', '_gmfake.js'));

/* ---- ① 源键 id ---- */
const btnCount = +(h.match(/#define\s+MAP_BTN_COUNT\s+(\d+)/) || [])[1];
const l2s2 = +(h.match(/MAP_BTN_L2_S2\s*=\s*(\d+)/) || [])[1];
const r2s2 = +(h.match(/MAP_BTN_R2_S2\s*=\s*(\d+)/) || [])[1];
if (btnCount === 41 && l2s2 === 39 && r2s2 === 40) ok(`固件 MAP_BTN_COUNT=41，L2_S2=${l2s2} R2_S2=${r2s2}`);
else fail(`固件源键数量/编号不对：MAP_BTN_COUNT=${btnCount} L2_S2=${l2s2} R2_S2=${r2s2}`);

const padIds = Array.from(padJs.matchAll(/\{\s*id:\s*(\d+)\s*,\s*key:/g)).map(m => +m[1]);
if (padIds.length === btnCount && padIds.every((v, i) => v === i))
  ok(`pad-art.js 的 BTN 表 ${padIds.length} 项，id 0..${btnCount - 1} 连续`);
else fail(`pad-art.js BTN 表 id 不连续或数量不对：${padIds.length} 项（固件 ${btnCount}）`);
if (/id:\s*39\s*,\s*key:\s*'l2_s2'/.test(padJs) && /id:\s*40\s*,\s*key:\s*'r2_s2'/.test(padJs))
  ok('pad-art.js 里有 id 39/40（L2 二段 / R2 二段）');
else fail('pad-art.js 里没有 id 39/40 两个源键');
/* VK_FIRST 必须写死 35：写成 BTN_COUNT - VK_COUNT 会被 39/40 带偏（r59 修的那个坑） */
if (/const VK_FIRST = 35;/.test(mapJs)) ok('page-map.js 的 VK_FIRST 写死 35');
else fail('page-map.js 的 VK_FIRST 不是写死的 35（加键后 V1..V4 会被算错）');
if (/const isVkBtn = b => b >= VK_FIRST && b <= VK_LAST;/.test(mapJs))
  ok('isVkBtn 用 [VK_FIRST, VK_LAST] 区间（不是 b >= VK_FIRST）');
else fail('isVkBtn 没有限制上界：39/40 会被当成虚拟组合键');

/* ---- ⑥ 左右不能反（2026-10-17 真机抓到的 bug）----
 * 这个函数（ds5map_s2t_effect）的 `side` 用固件约定（0 = 右 / 1 = 左，和 "右在 [10..20]" 一致），
 * 而配置区用的是**配置约定**（bit0/低半 = 左）。以前拿 side 直接查配置 ⇒ 给 L2 配的效果装到了 R2 上。
 * 现在必须先把 side 翻译成配置侧的 w，再用 w 查一切；并且再也不许出现 `side ? s2->` 这种写法。 */
{
  const fn = ds5C.match(/bool ds5map_s2t_effect\(uint8_t side[\s\S]*?\n\}/);
  if (!fn) fail('在 ds5map.c 里找不到 ds5map_s2t_effect()');
  else {
    const body = fn[0];
    if (/uint8_t w\s*=\s*side \? 0 : 1;/.test(body)) ok('编码器把 side 翻译成配置侧编号 w（0=左 / 1=右）');
    else fail('编码器没有把 side 翻译成配置侧编号（左右会反）');
    if (/uint8_t bit\s*=\s*\(uint8_t\)\(1u << w\);/.test(body)) ok('效果位用配置侧编号算（bit0=左 / bit1=右）');
    else fail('效果位没有用配置侧编号算');
    if (/uint8_t th\s*=\s*w \? s2->thresh_r : s2->thresh_l;/.test(body)) ok('起始位置按配置侧编号取（w=1→右）');
    else fail('起始位置没按配置侧编号取');
    if (/side \? s2->/.test(body)) fail('编码器里还有 `side ? s2->…` 的旧写法（左右会反）');
    else ok('编码器里不再有 `side ? s2->…` 的旧写法');
    for (const [pat, what] of [[/w \? s2->end_r : s2->end_l/, '结束位置'], [/w \? s2->freq_r : s2->freq_l/, '频率']]) {
      if (pat.test(body)) ok(`${what}也按配置侧编号取`);
      else fail(`${what}没按配置侧编号取`);
    }
  }
  /* 遥测状态字节：位号必须跟配置区统一（bit0 = 左） */
  if (/if \(s2t_applied\[0\]\) st \|= 0x02;/.test(mainC) && /if \(s2t_applied\[1\]\) st \|= 0x01;/.test(mainC))
    ok('TLM_STATE[47] 的位号跟配置区统一（bit0=左，side0=右→bit1）');
  else fail('TLM_STATE[47] 的左右位号没跟配置区统一');
}

/* ---- ② map_s2t_t 布局 ---- */
const body = h.match(/(?:^|\n)typedef struct \{([^{}]*?)\}\s*map_s2t_t;/);
if (!body) { console.log('FAIL 在 ds5map.h 里找不到 map_s2t_t'); process.exit(1); }
/* 数组长度可能写成宏（t2_mode[MAP_SHIFT_LAYERS] / fx[MAP_SHIFT_LAYERS][S2T_FX_COUNT]）
 * —— 先把这几个宏换成数；二维数组按两维相乘算大小。 */
const MACROS = { MAP_SHIFT_LAYERS: 4, MAP_LAYERS: 5, MAP_BTN_COUNT: 41,
                 MAP_FORM_COUNT: 6, S2T_FX_COUNT: 10 };
const fields = [];
let off = 0;
for (const line of body[1].split('\n')) {
  const m = line.replace(/\/\*[\s\S]*?\*\//g, '')
    .match(/^\s*(uint8_t|uint16_t|uint32_t)\s+(\w+)(\[(\w+)\])?(\[(\w+)\])?\s*;/);
  if (!m) continue;
  const dim = tok => (tok ? (+tok || MACROS[tok] || 0) : 1);
  const n = dim(m[4]) * dim(m[6]);
  if (!n) fail(`map_s2t_t 里 ${m[2]} 的数组长度是未知宏 ${m[4] || ''}/${m[6] || ''}（补进 MACROS）`);
  const size = (m[1] === 'uint16_t' ? 2 : m[1] === 'uint32_t' ? 4 : 1) * n;
  fields.push({ name: n > 1 ? m[2] + '[0]' : m[2], off, size });
  off += size;
}
const S2T_SIZE = off;
/* R3.25：66 = 14（主层那一套）+ 4×3（覆盖层模式位/左右分界点）+ 4×10（覆盖层自适应扳机） */
const want = { thresh_l: 0, thresh_r: 1, hyst: 2, effect_mode: 3, strength: 4, kind: 5,
               end_l: 6, end_r: 7, freq_l: 8, freq_r: 9, trig_mode: 10, flags: 11,
               bound_l: 12, bound_r: 13,
               't2_mode[0]': 14, 't2_bnd_l[0]': 18, 't2_bnd_r[0]': 22, 'fx[0]': 26 };
if (S2T_SIZE === 66) ok('map_s2t_t = 66 字节（14 老布局 + 覆盖层 1..4 的 12 + 自适应扳机 40）');
else fail(`map_s2t_t = ${S2T_SIZE} 字节（网页 S2T_LEN 是 66）`);
for (const [k, o] of Object.entries(want)) {
  const f = fields.find(x => x.name === k);
  if (!f) fail(`map_s2t_t 里没有 ${k}`);
  else if (f.off !== o) fail(`${k}: 固件 off=${f.off} ≠ 约定 ${o}`);
}
{
  const fxf = fields.find(x => x.name === 'fx[0]');
  if (fxf && fxf.size === 40) ok('fx[覆盖层][列] = 4×10 = 40 字节（[26..65]）');
  else fail(`fx 的大小不是 40 字节：${fxf ? fxf.size : '找不到 fx'}`);
}
if (!bad.length) ok('map_s2t_t 逐字段偏移与网页约定一致（R3.25：整套扳机设置按层）');

/* 网页打包：字节位置必须一一对上 */
const pack = mapJs.match(/const s2t = new Uint8Array\(S2T_LEN\);([\s\S]*?)\n\n/);
if (!pack) fail('page-map.js 里找不到 s2t 打包代码');
else {
  const txt = pack[1];
  const pair = [[0, 'thresh_l'], [1, 'thresh_r'], [2, 'hyst'], [3, 'effect_mode'],
                [4, 'strength'], [5, 'kind'], [6, 'end_l'], [7, 'end_r'],
                [8, 'freq_l'], [9, 'freq_r'], [11, 'flags'],
                [12, 'bound_l'], [13, 'bound_r']];
  const miss = pair.filter(([i, k]) => !new RegExp(`s2t\\[${i}\\]\\s*=\\s*\\(?s2\\.${k}`).test(txt));
  if (miss.length) fail('网页打包缺字段：' + miss.map(x => x[1]).join('/'));
  else ok('page-map.js 打包 s2t[0..13] = 起始位置/迟滞/效果位/力度/类型/结束位置/频率/主层两段位/行为位/主层左右分界点');
  /* R3.25：覆盖层 1..4 那 12 字节走一个循环写 [14..25]（模式位 + 左右分界点） */
  const loopOk = /for \(let i = 0; i < SHIFT_LAYERS; i\+\+\)/.test(txt)
    && /s2t\[14 \+ i\]\s*=\s*\(t2m\[i\] \|\| 0\) & 3/.test(txt)
    && /s2t\[18 \+ i\]\s*=\s*\(t2l\[i\] \|\| 0\) & 0xFF/.test(txt)
    && /s2t\[22 \+ i\]\s*=\s*\(t2r\[i\] \|\| 0\) & 0xFF/.test(txt);
  if (loopOk) ok('page-map.js 打包 s2t[14..25] = 覆盖层 1..4 的模式位 / 左分界点 / 右分界点');
  else fail('page-map.js 没有按约定打包覆盖层那份（[14..25]）');
  /* R3.25 第二轮：覆盖层那套自适应扳机走 行×列 的双循环写 [26..65] */
  const fxLoopOk = /for \(let k = 0; k < FX_KEYS\.length; k\+\+\)/.test(txt)
    && /s2t\[26 \+ i \* FX_KEYS\.length \+ k\]\s*=\s*\(\(s2\['f_' \+ FX_KEYS\[k\]\] \|\| \[\]\)\[i\] \|\| 0\) & 0xFF/.test(txt);
  if (fxLoopOk) ok('page-map.js 打包 s2t[26..65] = 覆盖层 1..4 的自适应扳机（行主序，列 = FX_KEYS）');
  else fail('page-map.js 没有按约定打包覆盖层那套自适应扳机（[26..65]）');
}
if (new RegExp(`const S2T_LEN = ${S2T_SIZE};`).test(mapJs)) ok(`网页 S2T_LEN = ${S2T_SIZE}`);
else fail('网页 S2T_LEN 与固件 sizeof(map_s2t_t) 不一致');

/* ---- ⑧ 网页 FX_KEYS 顺序 ↔ 固件 S2T_FX_* 列号（错一位就会把"力度"写进"频率"）---- */
{
  const fxMacros = {};
  for (const m of h.matchAll(/#define\s+S2T_FX_(\w+)\s+(\d+)/g)) fxMacros[m[1]] = +m[2];
  const NAME2MACRO = { effect_mode: 'EFFECT', thresh_l: 'TH_L', thresh_r: 'TH_R',
                       end_l: 'END_L', end_r: 'END_R', strength: 'STR', kind: 'KIND',
                       freq_l: 'FREQ_L', freq_r: 'FREQ_R', flags: 'FLAGS' };
  const webFx = (mapJs.match(/const FX_KEYS = \[([\s\S]*?)\];/) || [])[1];
  const webKeys = webFx ? Array.from(webFx.matchAll(/'(\w+)'/g)).map(m => m[1]) : [];
  const orderOk = webKeys.length === 10
    && webKeys.every((k, i) => NAME2MACRO[k] && fxMacros[NAME2MACRO[k]] === i);
  if (orderOk && fxMacros.COUNT === 10)
    ok('网页 FX_KEYS 顺序 = 固件 S2T_FX_* 列号（逐项对上，0..9）');
  else
    fail('网页 FX_KEYS 与固件 S2T_FX_* 对不上：web=' + webKeys.join('/')
         + ' 固件=' + JSON.stringify(fxMacros));
}

/* [10] = 网页自己的「两段模式」位（bit0 左 / bit1 右）—— 固件只存不解释，
 * 所以加这个开关不用升 CFG_VER、不用重刷固件。 */
const tm = fields.find(x => x.name === 'trig_mode');
if (tm && tm.off === 10) ok('网页的「两段模式」位落在固件 trig_mode（偏移 10）');
else fail(`两段模式位应该在偏移 10（trig_mode），实际 ${tm ? tm.off : '找不到 trig_mode'}`);
if (/s2t\[10\]\s*=\s*\(s2\.trig_mode \|\| 0\) & 3;/.test(mapJs)) ok('page-map.js 把 s2t[10] 写成两段模式位（& 3）');
else fail('page-map.js 没有把 s2t[10] 写成两段模式位');

/* ---- ⑦ R3.25：模式位与分界点**按层**（主层在老偏移，覆盖层 1..4 在追加段；固件按当前激活层取）----
 * 语义：当前激活层（覆盖层栈顶，没有就主层）自己的开关/分界点说了算 —— 和按键映射同一套分层规矩。 */
{
  const activeOk = /uint8_t\s+t2_layer\s*=\s*top_layer\(\);/.test(ds5C)
    && /ds5map_t2_mode_layer\(t2_layer\)/.test(ds5C)
    && /ds5map_t2_bound_layer\(t2_layer, 0\)/.test(ds5C)
    && /ds5map_t2_bound_layer\(t2_layer, 1\)/.test(ds5C);
  if (activeOk) ok('固件判定二段时取**当前激活层**那一份（top_layer → t2_mode_layer / t2_bound_layer）');
  else fail('固件没按当前激活层取两段位/分界点（per-layer 会失效）');
  if (/t2_layer == g_fast_layer/.test(ds5C) && /g_fast_layer = t2_layer;/.test(ds5C))
    ok('快速路径把「当前层」也并进比较键（层变了必须重算，不拿上一层的缓存顶）');
  else fail('快速路径没比较当前层：激活覆盖层那一瞬间二段状态可能用旧层的缓存');
  if (/uint8_t ds5map_t2_mode_layer\(uint8_t layer\)/.test(ds5C)
      && /uint8_t ds5map_t2_bound_layer\(uint8_t layer, uint8_t w\)/.test(ds5C))
    ok('固件有按层取值的两个函数（ds5map_t2_mode_layer / ds5map_t2_bound_layer）');
  else fail('固件缺按层取值的函数');
  /* R3.25 第二轮：自适应扳机那一套也按层 —— 效果编码/兜底重发/两个行为位全取当前层 */
  const viewUses = (ds5C.match(/s2t_fx_view\(top_layer\(\)/g) || []).length;
  if (viewUses >= 4 && /static void s2t_fx_view\(uint8_t layer, map_s2t_t \*v\)/.test(ds5C))
    ok(`固件的自适应扳机参数取**当前激活层**那一套（s2t_fx_view(top_layer()) 用了 ${viewUses} 处）`);
  else fail(`固件还有函数在读整机那份效果参数（s2t_fx_view(top_layer()) 只有 ${viewUses} 处）——按层会失效`);
  if (/t2_layer != g_fx_layer/.test(ds5C) && /ds5map_s2t_request_resend_nf\(\);/.test(ds5C))
    ok('切层时请求重发一次扳机效果（桌面上没有主机输出帧也能立刻换手感）');
  else fail('切层没有请求重发（或者用了 forget 版）：切了层手感还是上一层的');
  /* R3.26：切层必须走「不 forget」那条 —— forget 会清掉 s2t_applied，
   * 新层没有效果时就跳过「撤下」分支，手柄上会粘着上一层的手感（用户实测过）。 */
  if (/ds5map_s2t_take_resend_nf\(\)/.test(mainC)
      && /ds5map_s2t_take_resend_nf\(\)\)\s*\r?\n\s*pad_resend_state\(false\);/.test(mainC))
    ok('main.c 把"切层重发"接成 pad_resend_state(false) —— 新层没效果时能真的撤下');
  else fail('切层重发接成了 forget 版：新层没有效果时撤不掉，手柄会粘着上一层的手感');
  /* R3.27：**配置改了**（网页保存 / COMMIT）也只许"强制重发"，不许清 s2t_applied ——
   * 清了之后 `if (s2t_applied[side])` 不成立 → 撤下（发 OFF）被跳过 → 手柄一直留着上一个效果
   * （用户实测：点「重置本层」后按 L2 还有扳机感）。s2t_forget() 只许留在"连上 primer"和"断开"两处。 */
  {
    const forgetCalls = (mainC.match(/^\s*s2t_forget\(\);/gm) || []).length;
    if (forgetCalls === 2)
      ok('s2t_forget() 只在「连上 primer」「断开」两处（配置改动一律不清它）');
    else
      fail(`s2t_forget() 有 ${forgetCalls} 处调用（只该有 2 处）—— 清它会让"关掉效果"撤不干净`);
    if (/if \(force\) s2t_force = true;/.test(mainC) && /if \(!s2t_force && s2t_applied\[side\]/.test(mainC))
      ok('配置改动走 s2t_force（强制重发一次）而不是 s2t_forget');
    else fail('配置改动那条路没有走 s2t_force');
  }
  if (/const S2T_LEN_OLD = 14;/.test(mapJs) && /state\.s2tLen = S2T_LEN_OLD;/.test(mapJs)
      && /const t2Layered = \(\) => state\.s2tLen !== S2T_LEN_OLD;/.test(mapJs))
    ok('网页兼容老固件：读不满 66 字节就退回 14 字节那版（s2tLen=14，一律按主层那份算、写也只写 14）');
  else fail('网页没有「老固件只有 14 字节」的退路（连老固件会整页 load 失败，或假装覆盖层也有一套）');
  if (/DS5MAP_CFG_VER\s+15/.test(ds5C) && /14 → 66 B/.test(ds5C))
    ok('DS5MAP_CFG_VER = 15（0x0b 长到 66 字节 → cfg.bin 整份回落默认，符合用户"开发期直接升版本号"）');
  else fail('DS5MAP_CFG_VER 没跟着升到 15（老 cfg.bin 大小对不上会静默回落，日志里说不清）');
}
/* [5] 必须**按字面**读：老配置留下来的 0 在新含义下是"阻力"，固件就是这么解释的；
 * 网页要是把 0 当"没设"默认成 1（扳机），面板显示就和手上的手感对不上（2026-10-17 查出的坑）。 */
if (/kind: s2t\[5\] & 0x0F,/.test(mapJs)) ok('page-map.js 按字面读效果类型（不拿 0 兜默认值）');
else fail('page-map.js 读 s2t[5] 时给 0 兜了默认值（会和固件的解释不一致）');
if (/const kind = \(old\.detent_kind \? 0 : 1\);/.test(mapJs))
  ok('applyData 迁移旧文件：断点式→扳机(1)、硬墙式→阻力(0)');
else fail('applyData 没有迁移旧文件的 detent_kind → kind');

/* ---- ③ region 号一致 ----
 * S2T 已经上线：三处都要有（固件 / 网页 protocol.js / 假接收机）。
 * FLICK、GEST 只是 R2.5 先把结构体落位（R2.6/R2.7 才上线），网页 protocol.js 里
 * 还**故意不加** REG 常量（加了也没人用）—— 这里只要求固件与假接收机对齐。 */
for (const [name, num, inWeb] of [['S2T', 0x0b, true], ['FLICK', 0x0c, false], ['GEST', 0x0d, false]]) {
  const hex = num.toString(16);
  const inH = new RegExp(`MAP_REG_${name}\\s*=\\s*0x0${hex}`, 'i').test(h);
  const inProto = new RegExp(`${name}:\\s*0x0${hex}\\b`, 'i').test(protoJs);
  const inFake = new RegExp(`0x0${hex}:\\s*\\d+`).test(fakeJs);
  const protoOk = inWeb ? inProto : true;
  if (inH && protoOk && inFake)
    ok(`region ${name} = 0x0${hex}（固件 / 假接收机${inWeb ? ' / protocol.js' : ''} 一致）`);
  else fail(`region ${name} = 0x0${hex} 不一致：h=${inH} protocol=${inProto}(需要=${inWeb}) fake=${inFake}`);
}
/* ---- ④ 配置区大小 ---- */
const slotSize = 5 * btnCount * 6 * 5;
if (new RegExp(`0x01:\\s*${slotSize}\\b`).test(fakeJs)) ok(`_gmfake.js SLOTS = ${slotSize}（与 5×${btnCount}×6×5 一致）`);
else fail(`_gmfake.js 的 SLOTS 大小不是 ${slotSize}`);
if (/0x0b:\s*66\b/.test(fakeJs)) ok('_gmfake.js S2T = 66（R3.25 起 0x0b 是 66 字节）');
else fail('_gmfake.js 的 S2T 大小不是 66');
if (/CFG_REGS = \[0x00, 0x01, 0x03, 0x07, 0x08, 0x09, 0x0a, 0x0b\]/.test(fakeJs))
  ok('_gmfake.js 的 CFG_REGS 把 0x0b 算进配置体（与网页哈希顺序一致）');
else fail('_gmfake.js 的 CFG_REGS 没把 0x0b 算进去');
if (/r\.meta, r\.slots, r\.lb, r\.gy, r\.tp, r\.vk, r\.strat, r\.s2t/.test(mapJs))
  ok('网页哈希顺序 = packRegions 顺序（…strats, s2t）');
else fail('网页哈希顺序里没有 r.s2t（哈希与 CFG_REGS 对不上）');

/* ---- ⑤ 效果字节：镜像 ds5map.c 的 ds5map_s2t_effect()，对拍社区实测值 ---- */
function zone(pos) {                          /* ds5map.c s2t_zone() */
  const z = Math.floor((pos * 10 + 127) / 255);
  return z > 9 ? 9 : z;
}
function strOf(v) { return v < 1 ? 6 : (v > 8 ? 8 : v); }
/* 阻力 FEEDBACK 0x21 / 自动扳机 VIBRATION 0x26：启用档位位图，每档 3bit 幅度（力度-1）。
 * 阻力认「结束位置」（不设 = 到 9 档）；自动扳机固定到 9 档（参考工具只给起始位置）。 */
function sweep(pos, strength, hi) {
  const b = new Array(11).fill(0), z = zone(pos), s = strOf(strength);
  const top = (hi === undefined || hi === null) ? 9 : hi;
  let zones = 0, amp = 0;
  for (let i = z; i <= top; i++) { zones |= 1 << i; amp |= ((s - 1) & 7) * Math.pow(2, 3 * i); }
  b[1] = zones & 0xFF; b[2] = (zones >>> 8) & 3;
  b[3] = amp & 0xFF; b[4] = (amp >>> 8) & 0xFF; b[5] = (amp >>> 16) & 0xFF; b[6] = (amp >>> 24) & 0xFF;
  return b;
}
function resist(pos, strength, endPos) {
  /* r67：阻力不再有结束位置（用户要求），`end` 在这一侧被忽略 —— 永远起始档..9 档 */
  const b = sweep(pos, strength, 9); b[0] = 0x21; return b;
}
function auto(pos, strength, freq) {
  /* 频率（R3.1，用户："原生是什么就写什么"）：配置里存的就是**原生那个字节**，不再折半。
   * 真游戏样本 1..180 都有（Spider-Man 180、TLOU1 1），原作者工具里显示的也是这个数本身。 */
  const b = sweep(pos, strength); b[0] = 0x26;
  b[9] = Math.max(1, (freq || 10));
  return b;
}
/* 扳机 WEAPON 0x25：起始~结束两档，硬约束 start 2..7 / end 3..8、end=start+1 */
function weapon(pos, endPos, strength) {
  const b = new Array(11).fill(0);
  let st = Math.min(Math.max(zone(pos), 2), 7);
  let en = endPos ? zone(endPos) : st + 1;
  if (en <= st) en = st + 1;
  if (en > 8) en = 8;
  if (en <= st) st = en - 1;
  b[0] = 0x25;
  b[1] = (1 << st) | (en < 8 ? (1 << en) : 0);
  if (en >= 8) b[2] = 1 << (en - 8);
  b[3] = strOf(strength) - 1;
  return b;
}
const hex = a => a.map(v => v.toString(16).padStart(2, '0')).join(' ');
/* 社区实测（ExtendInput / dualsense-controller）：Start 7 End 8 Force 8 = 25 80 01 07 … */
if (weapon(200, 0, 8).join(',') === [0x25, 0x80, 0x01, 0x07, 0, 0, 0, 0, 0, 0, 0].join(','))
  ok('扳机：起始 200（≈78%）+ 力度 8 → 25 80 01 07 …（与官方 Weapon 的 80% 卡点写法一致）');
else fail('扳机（Weapon）字节不对：' + hex(weapon(200, 0, 8)));
if (resist(200, 8).join(',') === [0x21, 0x00, 0x03, 0x00, 0x00, 0x00, 0x3f, 0, 0, 0, 0].join(','))
  ok('阻力：起始 200（档 8）+ 力度 8 → 21 00 03 … 3F（档 8/9 全开、幅度 7）');
else fail('阻力（Feedback）字节不对：' + hex(resist(200, 8)));
if (auto(200, 8, 30).join(',') === [0x26, 0x00, 0x03, 0x00, 0x00, 0x00, 0x3f, 0, 0, 30, 0].join(','))
  ok('自动扳机：起始 200 + 力度 8 + 频率 30 → 26 00 03 … 3F 00 00 1E（原生字节直接写，不折半）');
else fail('自动扳机（Vibration）字节不对：' + hex(auto(200, 8, 30)));
if (auto(200, 8, 0)[9] === 10) ok('自动扳机频率 0 → 默认 10');
else fail('自动扳机频率 0 没有回落到默认 10：' + auto(200, 8, 0)[9]);
/* 实测抓下来的官方效果对拍（来源：DS5Dongle-Studio 的 shared/effects/*.json，
 * 都是从真游戏里抓的 11 字节）——位置按 0..255 换算到对应档位：
 *   Returnal「Wall 3→7」= 25 88 00 03   TLOU1「Wall 3→5」= 25 28 00 04 */
if (weapon(89, 191, 4).join(',') === [0x25, 0x88, 0x00, 0x03, 0, 0, 0, 0, 0, 0, 0].join(','))
  ok('对拍真游戏抓的字节：Returnal 墙 3→7 = 25 88 00 03 ✓');
else fail('复现 Returnal 墙 3→7 失败：' + hex(weapon(89, 191, 4)));
if (weapon(89, 140, 5).join(',') === [0x25, 0x28, 0x00, 0x04, 0, 0, 0, 0, 0, 0, 0].join(','))
  ok('对拍真游戏抓的字节：TLOU1 墙 3→5 = 25 28 00 04 ✓');
else fail('复现 TLOU1 墙 3→5 失败：' + hex(weapon(89, 140, 5)));
/* bow_resistance「Resistance 2–7」= 21 FC 00 80 24 49 00 …：那是**区间阻力**（档 2..7）。
 * r67 按用户要求把「阻力」定成"起始档..9 档、没有结束位置"，所以这个区间**复现不了**了 ——
 * 这里改成"钉住它确实被忽略"（以后谁把结束位置加回阻力，这条会红）。 */
if (resist(51, 3, 178).join(',') === resist(51, 3).join(','))
  ok('阻力忽略「结束位置」（r67 用户要求：阻力只有起始位置）');
else fail('阻力又把结束位置算进去了：' + hex(resist(51, 3, 178)));
/* 不设结束位置 = 一路到底（老行为不变） */
if (resist(51, 3).join(',') === resist(51, 3, 0).join(','))
  ok('阻力不设结束位置时＝一路到底（行为与之前一致）');
else fail('阻力不设结束位置时行为变了');
/* 边界：极低/极高位置也要满足各效果的硬约束 */
for (const pos of [1, 40, 90, 140, 255]) {
  const b = weapon(pos, 0, 9);
  const zones = [];
  for (let i = 0; i < 8; i++) if (b[1] & (1 << i)) zones.push(i);
  if (b[2] & 1) zones.push(8);
  if (b[0] !== 0x25 || Math.min(...zones) < 2 || Math.max(...zones) > 8 || zones.length !== 2
      || Math.max(...zones) - Math.min(...zones) !== 1 || b[3] > 7)
    fail(`扳机约束被破坏（起始 ${pos}）：zones=${zones.join('/')} str=${b[3]}`);
  const r = resist(pos, 9);
  const rz = [];
  for (let i = 0; i < 8; i++) if (r[1] & (1 << i)) rz.push(i);
  if (r[2] & 1) rz.push(8);
  if (r[2] & 2) rz.push(9);
  const lo = zone(pos);
  if (r[0] !== 0x21 || Math.min(...rz) !== lo || Math.max(...rz) !== 9 || rz.length !== 10 - lo)
    fail(`阻力区段不对（起始 ${pos}）：zones=${rz.join('/')}（应该 ${lo}..9）`);
}
if (!bad.length) ok('三种效果在起始位置 1/40/90/140/255 下都满足硬件约束（扳机 start 2..7、阻力从起始档到 9）');

console.log(bad.length ? `\n${bad.length} 项失败` : '\n全部通过');
process.exit(bad.length ? 1 : 0);

/* R3.4：二段/虚拟组合键松手必须真的释放动作（用户实测的"R2 一直按着"） */
{
  const m = src.match(/MAP_BTN_IS_VK\(b\) \|\| MAP_BTN_IS_S2T\(b\)[\s\S]{0,700}?bind_release_btn\(\(uint8_t\)b\);[\s\S]{0,200}?act_release_btn\(\(uint8_t\)b\);/);
  if (m) ok('二段/虚拟组合键松手会 act_release_btn()（R3.4：修"输出一直卡在按下"）');
  else fail('松手沿没有 act_release_btn() —— 二段映射的输出又会卡住');
}

/* R3.5：迟滞不许把发动点抬高（用户实测"设 115 按到 100 就触发"的根因） */
{
  const bad = /uint16_t\s+hi\s*=\s*\(uint16_t\)thresh\s*\+\s*hyst/.test(src);
  const good = /\(uint16_t\)v\s*>=\s*\(uint16_t\)thresh/.test(src);
  if (!bad && good) ok('两段迟滞是标准做法：到分界点就发动（不再 thresh + hyst）');
  else fail('两段发动点又被抬高了（hi = thresh + hyst）或发动条件丢了');
}
