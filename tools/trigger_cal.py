#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""边压边调扳机：压着扳机，用键盘把「起始位置」挪到"就是这儿有手感"的位置，然后保存。

为什么需要它：DS5 的 10 个行程档（效果里的 zone）和它上报的行程值（0~255）**不是严格一一对应**
—— 用户 2026-10-17 实测：把阻力设成档 8-9（上报值 204~255，名义 80~100%），手上在行程 ~50%
（上报值 ~124）就感到阻力了。所以"我想要 X% 有手感"只能按手感把档位拧过去，这个工具就是干这个的。

用法（**先关掉网页配置工具**：0xFC 一次只服务一个客户端）：
    python -X utf8 tools/trigger_cal.py --side l        # l = L2（左扳机）/ r = R2

按键：
    a / d        起始档 −1 / +1          A / D     起始档 −2 / +2
    ; / '        结束档 −1 / +1（只有「扳机」用得到）
    1..8         力度
    0            这一侧效果关（用来对比"完全没有效果"的手感）
    k            切换效果类型：阻力 → 扳机 → 自动扳机
    s            保存（COMMIT 落盘，之后网页/重启都还在）
    q            退出并**还原**（把进来时的配置写回，不落盘）
每一键都会：把新配置写进接收机（RAM，不落盘）+ 实时打印两侧行程值。
"""
import argparse
import sys
import time

try:
    import msvcrt                      # Windows：非阻塞读键
except ImportError:                    # pragma: no cover
    msvcrt = None

import diag

REG_S2T = 0x0B
REG_TLM_RAW = 0x10
LEN = 12
KIND_NAMES = ['阻力', '扳机', '自动扳机']
SIDES = {'l': (0, 'L2'), 'r': (1, 'R2')}      # 配置侧编号：bit0/低半 = 0 = 左（L2）


def zone(pos):
    return min((pos * 10 + 127) // 255, 9) if pos else None


def pack(s):
    kind = (s['kind_l'] & 3) | ((s['kind_r'] & 3) << 2)
    strength = (max(1, min(8, s['str_l']))) | (max(1, min(8, s['str_r'])) << 4)
    return bytes([s['thresh_l'] & 0xFF, s['thresh_r'] & 0xFF, max(1, min(63, s['hyst'])),
                  s['effect_mode'] & 3, strength, kind,
                  s['end_l'] & 0xFF, s['end_r'] & 0xFF,
                  s['freq_l'] & 0xFF, s['freq_r'] & 0xFF,
                  s['trig_mode'] & 3, s.get('flags', 0) & 3])   # [11] 行为位：原样保留，别清掉


def unpack(b):
    return {'thresh_l': b[0], 'thresh_r': b[1], 'hyst': b[2] or 8,
            'effect_mode': b[3] & 3,
            'str_l': (b[4] & 0x0F) or 6, 'str_r': (b[4] >> 4) or 6,
            'kind_l': b[5] & 3, 'kind_r': (b[5] >> 2) & 3,
            'end_l': b[6], 'end_r': b[7],
            'freq_l': b[8] or 10, 'freq_r': b[9] or 10,
            'trig_mode': b[10] & 3, 'flags': b[11] & 3}


def desc(s, w):
    nm = 'L2' if w == 0 else 'R2'
    th = s['thresh_l'] if w == 0 else s['thresh_r']
    on = (s['effect_mode'] >> w) & 1
    if not on or not th:
        return '%s：关' % nm
    kind = s['kind_l'] if w == 0 else s['kind_r']
    st = s['str_l'] if w == 0 else s['str_r']
    z = zone(th)
    if kind == 1:
        en_pos = s['end_l'] if w == 0 else s['end_r']
        en = zone(en_pos) if en_pos else min((z or 2) + 1, 8)
        return '%s：扳机 档 %d-%d 力度 %d' % (nm, min(max(z, 2), 7), en, st)
    if kind == 2:
        fq = s['freq_l'] if w == 0 else s['freq_r']
        return '%s：自动扳机 档 %d-9 力度 %d %dHz' % (nm, z, st, fq)
    return '%s：阻力 档 %d-9 力度 %d' % (nm, z, st)


def main():
    ap = argparse.ArgumentParser(description='边压边调扳机（起始/结束档、力度、效果类型）')
    ap.add_argument('--side', choices=['l', 'r'], default='l', help='调哪一侧（默认 L2）')
    ap.add_argument('--keep', action='store_true', help='退出时不还原（配合 s 保存用）')
    a = ap.parse_args()
    w, nm = SIDES[a.side]

    h = diag.open_dev()
    if h is None:
        print('[!] 接收器不在（或网页正占着 0xFC）')
        return 1
    cur = diag.rpc_read(h, REG_S2T, 0, LEN)
    if not cur or len(cur) < LEN:
        print('[!] 读不到 0x0b；先刷 R2.6 以上固件')
        return 1
    orig = bytes(cur)
    s = unpack(cur)

    def apply(commit=False):
        diag.rpc(h, 2, REG_S2T, 0, pack(s))                  # OP_WRITE
        if commit:
            diag.rpc(h, 3, 0xFF, 0, b'\x00')                 # OP_COMMIT
        time.sleep(0.05)

    def travel():
        r = diag.rpc_read(h, REG_TLM_RAW, 0, 8, tries=20)
        if not r:
            return None
        # TLM_RAW：0..3 摇杆，4 = L2，5 = R2（见 page-test.js 的 decode()）
        return r[4] if w == 0 else r[5]

    print('调 %s。按 a/d 挪起始档、1..8 力度、k 换类型、0 关效果、s 保存、q 退出还原。' % nm)
    print('  现在是：%s' % desc(s, w))
    if not (s['effect_mode'] >> w) & 1:
        s['kind_%s' % a.side] = 1
        s['effect_mode'] |= (1 << w)
        s['thresh_%s' % a.side] = s['thresh_%s' % a.side] or 128
        apply()
        print('  （这一侧本来关着，先按「扳机」打开看看手感）')

    last = 0.0
    try:
        while True:
            if msvcrt and msvcrt.kbhit():
                k = msvcrt.getwch()
                step = 0
                if k in ('a', 'A'):
                    step = -1 if k == 'a' else -2
                elif k in ('d', 'D'):
                    step = 1 if k == 'd' else 2
                elif k in (';', ':'):
                    step = -1
                elif k in ("'", '"'):
                    step = 1
                if step:
                    if k in (';', ':', "'", '"'):
                        e = s['end_%s' % a.side] or 128
                        s['end_%s' % a.side] = max(0, min(255, e + step * 8))
                    else:
                        t = s['thresh_%s' % a.side] or 0
                        s['thresh_%s' % a.side] = max(0, min(255, t + step * 8))
                    apply()
                    print('  → %s     （行程 %s）' % (desc(s, w), travel()))
                elif k in '12345678':
                    s['str_%s' % a.side] = int(k)
                    apply()
                    print('  → %s' % desc(s, w))
                elif k == '0':
                    s['effect_mode'] &= ~(1 << w)
                    apply()
                    print('  → %s（没有效果，用作对比）' % nm)
                elif k == 'k':
                    kk = ((s['kind_%s' % a.side]) + 1) % 3
                    s['kind_%s' % a.side] = kk
                    if not (s['effect_mode'] >> w) & 1:
                        s['effect_mode'] |= (1 << w)
                    apply()
                    print('  → %s' % desc(s, w))
                elif k == 's':
                    apply(commit=True)
                    print('  ★ 已保存（落盘）：%s' % desc(s, w))
                elif k in ('q', '\x1b'):
                    break
            now = time.time()
            if now - last > 0.5:
                last = now
                t = travel()
                sys.stdout.write('\r%s | %s 行程 %s     ' % (
                    desc(s, w), nm, ('%3d' % t) if t is not None else ' — '))
                sys.stdout.flush()
            time.sleep(0.02)
    except KeyboardInterrupt:
        pass

    if not a.keep:
        diag.rpc(h, 2, REG_S2T, 0, orig)
        diag.rpc(h, 3, 0xFF, 0, b'\x00')
        time.sleep(0.3)
        back = diag.rpc_read(h, REG_S2T, 0, LEN)
        print('\n已还原成进来时那份：%s' % ('逐字节一致 ✓' if bytes(back) == orig else '不一致 ✗'))
    else:
        print('\n（不还原：保留现在的值）')
    try:
        h.close()
    except Exception:
        pass
    return 0


if __name__ == '__main__':
    sys.exit(main())
