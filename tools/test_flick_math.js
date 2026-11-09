'use strict';
/* R2.7 Flick Stick 的**算法镜像单测**：把 firmware/src/ds5map.c 的 flick_apply() 抄成 JS，
 * 用合成的摇杆轨迹钉住几个最容易写错的点：
 *   ① 入沿重定基线（拨进激活圈的那一帧不能跳变）
 *   ② 角度增量积分 + 一帧发不完的余量要留到下帧（不丢步）
 *   ③ wrap 到最短角差（170° → -170° 是 +20°，不是 -340°）
 *   ④ 迟滞（进 r_on 才算接管，掉到 r_off 以下才放手）
 *   ⑤ 接管期间右摇杆写回 0x80（不让游戏双重输入）；没接管时一个字节都不碰
 *   ⑥ 正反转对称
 * 本机没有原生 C 编译器（只有 RISC-V 那套，编了跑不了），所以和 test_s2t_layout.js 一样
 * 用"镜像 + 断言"钉行为；**真机手感仍然只能靠用户的手**（见 HANDOFF §⑯）。
 * 用法：node tools/test_flick_math.js */

const bad = [];
const ok = m => console.log('ok   ' + m);
const fail = m => { bad.push(m); console.log('FAIL ' + m); };

const PI = Math.PI;
const wrapPi = d => {                    /* 镜像 C 的两个 while 循环 */
  while (d > 3.14159265) d -= 6.28318531;
  while (d < -3.14159265) d += 6.28318531;
  return d;
};

/* 镜像 flick_apply()：st 就是固件里那几个 static；默认配置 = cfg_defaults() */
function makeFlick(cfg) {
  const fl = Object.assign({ r_on: 60, r_off: 50, sens: 1024, y_mode: 0 }, cfg || {});
  const st = { engaged: false, theta: 0, acc: 0, dx: 0, dy: 0 };
  const step = (rx, ry, p) => {
    p = p || [0x80, 0x80, rx, ry];
    st.dx = st.dy = 0;
    const dx = rx - 128, dy = ry - 128;
    const r2 = dx * dx + dy * dy;
    const ron = fl.r_on / 100, roff = fl.r_off / 100;
    if (!st.engaged) {
      if (r2 >= ron * ron * 128 * 128) { st.engaged = true; st.theta = Math.atan2(dy, dx); st.acc = 0; }
    } else if (r2 <= roff * roff * 128 * 128) {
      st.engaged = false;
    }
    if (!st.engaged) return p;
    const th = Math.atan2(dy, dx);
    const dth = wrapPi(th - st.theta);
    st.theta = th;
    const want = st.acc + Math.trunc(dth * fl.sens * 0.5);
    let out = want;
    if (out > 127) out = 127; else if (out < -127) out = -127;
    st.acc = want - out;
    st.dx = out;
    if (fl.y_mode) {
      let vy = Math.trunc((ry - 128) * fl.sens / 512);
      if (fl.y_mode === 2) vy = -vy;
      if (vy > 127) vy = 127; else if (vy < -127) vy = -127;
      st.dy = vy;
    }
    p[2] = 0x80;                         /* 接管：右摇杆回中 */
    p[3] = 0x80;
    return p;
  };
  return { st, step };
}

/* 摇杆坐标：角度 deg、半径 pct（0..100+） */
const stick = (deg, pct) => {
  const r = pct / 100 * 128;
  const a = deg * PI / 180;
  return [Math.round(128 + r * Math.cos(a)), Math.round(128 + r * Math.sin(a))];
};
/* 同位置连打若干帧，把余量冲干净，返回总增量 */
function flush(f, rx, ry, n) {
  let sum = 0;
  for (let i = 0; i < (n || 12); i++) { f.step(rx, ry); sum += f.st.dx; }
  return sum;
}

/* ---- ① 入沿重定基线：拨进激活圈的第一帧 dx 必须是 0 ---- */
{
  const f = makeFlick();
  const [x, y] = stick(90, 90);
  f.step(x, y);
  if (f.st.engaged && f.st.dx === 0) ok('① 入沿重定基线：进圈第一帧 dx = 0（不跳变）');
  else fail(`① 进圈第一帧就跳变了：engaged=${f.st.engaged} dx=${f.st.dx}`);
}

/* ---- ② 角度增量积分 + 余量不丢：转 90° 的总量 = trunc(π/2 × sens/2) ---- */
{
  const f = makeFlick();
  const [x0, y0] = stick(0, 90);
  f.step(x0, y0);                        /* 先接管（基线 = 0°） */
  const [x1, y1] = stick(90, 90);
  const first = f.step(x1, y1).length ? f.st.dx : 0;
  const total = first + flush(f, x1, y1);
  const want = Math.trunc(PI / 2 * 1024 * 0.5);          /* 804 */
  if (first === 127) ok('② 单帧上限 127 生效（大角度分几帧发）');
  else fail(`② 单帧没被夹到 127：${first}`);
  if (Math.abs(total - want) <= 1) ok(`② 转 90° 的总量对：${total}（应 ${want}，余量没丢）`);
  else fail(`② 转 90° 的总量不对：${total}（应 ${want}）`);
}

/* ---- ③ wrap：170° → -170° 是 +20°，不是 -340° ---- */
{
  const f = makeFlick();
  const [xa, ya] = stick(170, 90);
  f.step(xa, ya);
  const [xb, yb] = stick(-170, 90);
  const first = f.step(xb, yb).length ? f.st.dx : 0;
  const total = first + flush(f, xb, yb);
  const want = Math.trunc(20 * PI / 180 * 1024 * 0.5);   /* 178 */
  if (total > 0 && Math.abs(total - want) <= 1)
    ok(`③ 跨 ±180° 走最短角差：总量 +${total}（应 +${want}，不是 -340°）`);
  else fail(`③ 跨 ±180° 的角差算错：${total}（应 +${want}）`);
}

/* ---- ④ 迟滞：进 r_on=60% 才接管，掉到 r_off=50% 以下才放手 ---- */
{
  const f = makeFlick();
  const [x1, y1] = stick(0, 55);          /* 55% < r_on：不该接管 */
  f.step(x1, y1);
  const engagedAt55 = f.st.engaged;
  const [x2, y2] = stick(0, 65);          /* 65% > r_on：接管 */
  f.step(x2, y2);
  const engagedAt65 = f.st.engaged;
  const [x3, y3] = stick(0, 55);          /* 掉到 55%：还在 r_off(50) 以上 → 仍接管 */
  f.step(x3, y3);
  const stillAt55 = f.st.engaged;
  const [x4, y4] = stick(0, 45);          /* 45% < r_off：放手 */
  f.step(x4, y4);
  const releasedAt45 = f.st.engaged;
  if (!engagedAt55 && engagedAt65 && stillAt55 && !releasedAt45)
    ok('④ 迟滞对：55% 不接管 → 65% 接管 → 55% 仍接管 → 45% 放手');
  else fail(`④ 迟滞不对：55=${engagedAt55} 65=${engagedAt65} 55=${stillAt55} 45=${releasedAt45}`);
}

/* ---- ⑤ 接管期间摇杆写回 0x80；没接管时一个字节都不碰 ---- */
{
  const f = makeFlick();
  const off = [0x80, 0x80, 200, 150];      /* 没接管（半径 ~57% 且是第一次进） */
  const [x0, y0] = stick(0, 55);
  const p0 = [0x80, 0x80, x0, y0];
  f.step(x0, y0, p0);
  const untouched = (p0[2] === x0 && p0[3] === y0);
  const [x1, y1] = stick(45, 90);
  const p1 = [0x80, 0x80, x1, y1];
  f.step(x1, y1, p1);
  const neutral = (p1[2] === 0x80 && p1[3] === 0x80);
  if (untouched && neutral) ok('⑤ 接管时右摇杆写回 0x80；没接管时不动它');
  else fail(`⑤ 摇杆处理不对：未接管保持=${untouched} 接管回中=${neutral}（off=${off.length}）`);
}

/* ---- ⑥ 正反转对称 ---- */
{
  const f = makeFlick();
  const [x0, y0] = stick(0, 90);
  f.step(x0, y0);
  const [xp, yp] = stick(60, 90);
  const pos = f.step(xp, yp).length ? f.st.dx + flush(f, xp, yp) : 0;
  const g = makeFlick();
  g.step(x0, y0);
  const [xm, ym] = stick(-60, 90);
  const neg = g.step(xm, ym).length ? g.st.dx + flush(g, xm, ym) : 0;
  if (pos > 0 && neg < 0 && Math.abs(pos + neg) <= 1)
    ok(`⑥ 正反转对称：+${pos} / ${neg}（幅度相等、方向相反）`);
  else fail(`⑥ 正反转不对称：+${pos} / ${neg}`);
}

/* ---- ⑦ 灵敏度按 sens 线性：sens 翻倍 → 总量翻倍 ---- */
{
  const one = makeFlick({ sens: 512 });
  const two = makeFlick({ sens: 1024 });
  const [x0, y0] = stick(0, 90);
  one.step(x0, y0); two.step(x0, y0);
  const [x1, y1] = stick(90, 90);
  one.step(x1, y1); two.step(x1, y1);
  const a = one.st.dx + flush(one, x1, y1);
  const b = two.st.dx + flush(two, x1, y1);
  if (Math.abs(b - 2 * a) <= 2) ok(`⑦ 灵敏度线性：sens 512→${a}，1024→${b}（约 2 倍）`);
  else fail(`⑦ 灵敏度不线性：512→${a}，1024→${b}`);
}

/* ---- ⑧ 配置文件里的默认值要与固件 cfg_defaults() 一致 ---- */
{
  const fs = require('fs');
  const p = require('path');
  const src = fs.readFileSync(p.join(__dirname, '..', 'firmware', 'src', 'ds5map.c'), 'utf8');
  const want = [/g_cfg\.flick\.r_on = 60;/, /g_cfg\.flick\.r_off = 50;/,
                /g_cfg\.flick\.sens = 1024;/, /g_cfg\.flick\.enable_mode = MAP_GYRO_EN_OFF;/];
  const miss = want.filter(re => !re.test(src));
  if (!miss.length) ok('⑧ 固件 cfg_defaults() 里 flick 的默认值 = 本测试用的那套（半径 60/50、sens 1024、默认关）');
  else fail('⑧ 固件里 flick 的默认值变了，测试用的默认值要跟着改：' + miss.join(' '));
}

console.log(bad.length ? `\n${bad.length} 项失败` : '\n全部通过');
process.exit(bad.length ? 1 : 0);
