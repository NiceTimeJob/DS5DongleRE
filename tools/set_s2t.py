#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""命令行读写接收机的「扳机」配置区（0x0b），= 网页右栏「扳机逻辑」页的等价物。

什么时候用它：跑 tools/watch_s2t.py 看实时行程时必须先关掉网页（0xFC 一次只给一个客户端），
那时想改效果/位置就用这个脚本；平时在网页上调更直观。

区布局（= 固件 src/ds5map.h 的 map_s2t_t，**R3.25 起 66 字节、整套按层**）：
  [0] 左起始位置 0..255（0 = 这一侧关）  [1] 右起始位置
  [2] 迟滞 1..63（R3.8 起判定只用纯阈值，这一格留着不读）
  [3] 效果开关位（bit0 左 / bit1 右）—— 关掉就是「关闭」，扳机全是普通扳机
  [4] 力度：低 4 位左 / 高 4 位右（**硬件只有 8 档**，1..8）
  [5] 效果类型：低 2 位左 / bit2-3 右（0 阻力 / 1 扳机 / 2 自动扳机）
  [6] 左结束位置 0..255（0 = 未设 → 起始档+1；只对「扳机」有效）  [7] 右结束位置
  [8] 左频率 Hz（0 = 默认 10；只对「自动扳机」有效；发给手柄时按硬件 2 Hz/格折半）  [9] 右频率
  [10] **主层（层 0）**的「两段模式」位（bit0 左 / bit1 右）—— 固件只当配置原样存、按层取用
  [11] 行为位（bit0 忽略游戏原生效果 / bit1 压穿释放一段）—— **本脚本原样保留**
  [12][13] **主层**的两段分界点（左/右），0 = 没设过 → 回落手感起始位置
  [14..25] 覆盖层 1..4 的两段位 + 左右分界点
  [26..65] 覆盖层 1..4 各自那套「自适应扳机」（行主序 4×10，列 = 效果位/起始左右/结束左右/
           力度/类型/频率左右/行为位）—— **本脚本不碰**（只读写前 12 字节）

⚠ R3.25 起整套扳机设置**按层**：这个脚本改/显示的只是**主层**那一份；
  覆盖层激活时固件用的是覆盖层自己的设置（网页上按层设）。

用法：
  python -X utf8 set_s2t.py                                  # 只读打印（不改）
  python -X utf8 set_s2t.py --l-fx weapon --l-start 200 --l-end 220 --l-str 8 --mode left
  python -X utf8 set_s2t.py --r-fx auto --r-start 150 --r-str 7 --r-freq 40
  python -X utf8 set_s2t.py --l-fx resist --l-start 120 --r-fx none
  python -X utf8 set_s2t.py --restore        # 全关（＝出厂默认 00 00 08 00 66 05 …）
改完会 COMMIT（落盘），然后回读逐字节对比。**只写 0x0b 这一个区**，不碰映射/陀螺仪/宏。
"""
import argparse
import sys
import time

import diag

REG_S2T = 0x0B
OP_WRITE, OP_COMMIT = 2, 3
LEN = 12

DEF = {'thresh_l': 0, 'thresh_r': 0, 'hyst': 8, 'effect_mode': 0,
       'str_l': 6, 'str_r': 6, 'kind_l': 1, 'kind_r': 1,
       'end_l': 0, 'end_r': 0, 'freq_l': 10, 'freq_r': 10, 'trig_mode': 0}
FX = {'none': None, 'resist': 0, 'weapon': 1, 'auto': 2}
FX_NAMES = {0: '阻力(Feedback 0x21)', 1: '扳机(Weapon 0x25)', 2: '自动扳机(Vibration 0x26)'}
MODE_BITS = {'none': 0, 'left': 1, 'right': 2, 'both': 3}


def zone(pos):
    """镜像固件 ds5map.c 的 s2t_zone()：起始/结束位置 0..255 → 硬件档位 0..9。"""
    return min((pos * 10 + 127) // 255, 9) if pos else None


def eff_zone(start, end, kind):
    """这一侧的效果**实际**落在哪几档（lo, hi, 原始起始档, 是否被夹）——
    镜像 ds5map_s2t_effect()：阻力 = 起始档..结束档（不设=到 9 档）；自动扳机 = 起始档..9；
    扳机 = 起始~结束档（硬件 start 2..7 / end ≤8）。"""
    z = zone(start)
    if z is None:
        return None
    if kind == 0:
        hi = zone(end) if end else 9
        if hi < z:
            hi = z
        return (z, hi, z, False)
    if kind == 2:
        return (z, 9, z, False)
    st = min(max(z, 2), 7)
    en = zone(end) if end else st + 1
    if en <= st:
        en = st + 1
    if en > 8:
        en = 8
    if en <= st:
        st = en - 1
    return (st, en, z, st != z)


def side_txt(s, side):
    th = s['thresh_l'] if side == 0 else s['thresh_r']
    nm = '左' if side == 0 else '右'
    if not th:
        return '%s：关' % nm
    on = (s['effect_mode'] >> side) & 1
    kind = s['kind_l'] if side == 0 else s['kind_r']
    st = s['str_l'] if side == 0 else s['str_r']
    if not on:
        return '%s：%d（档 %d）效果关闭' % (nm, th, zone(th))
    lo, hi, z, clamped = eff_zone(th, s['end_l'] if side == 0 else s['end_r'], kind)
    rng = '档 %d' % lo if lo == hi else '档 %d-%d' % (lo, hi)
    extra = '力度 %d' % st
    if kind == 2:
        extra += '，%d Hz' % (s['freq_l'] if side == 0 else s['freq_r'])
    note = '（起始档 %d 被硬件约束夹到 %d）' % (z, lo) if clamped else ''
    return '%s：%s 起始 %d（%s＝%d~%d%%），%s%s' % (
        nm, FX_NAMES.get(kind, kind), th, rng, lo * 10, (hi + 1) * 10, extra, note)


def show(tag, s):
    raw = pack(s)
    print('%s: %s' % (tag, ' '.join('%02x' % x for x in raw)))
    print('   %s' % side_txt(s, 0))
    print('   %s' % side_txt(s, 1))
    print('   迟滞 %d   两段模式：左=%s 右=%s' % (
        s['hyst'], '两段' if s['trig_mode'] & 1 else '普通',
        '两段' if s['trig_mode'] & 2 else '普通'))


def unpack(b):
    return {'thresh_l': b[0], 'thresh_r': b[1], 'hyst': b[2] or 8,
            'effect_mode': b[3] & 3,
            'str_l': (b[4] & 0x0F) or 6, 'str_r': (b[4] >> 4) or 6,
            'kind_l': b[5] & 3, 'kind_r': (b[5] >> 2) & 3,
            'end_l': b[6], 'end_r': b[7],
            'freq_l': b[8] or 30, 'freq_r': b[9] or 30,
            'trig_mode': b[10] & 3, 'flags': b[11] & 3}


def pack(s):
    kind = (s['kind_l'] & 3) | ((s['kind_r'] & 3) << 2)
    strength = (max(1, min(8, s['str_l']))) | (max(1, min(8, s['str_r'])) << 4)
    return bytes([s['thresh_l'] & 0xFF, s['thresh_r'] & 0xFF, max(1, min(63, s['hyst'])),
                  s['effect_mode'] & 3, strength, kind,
                  s['end_l'] & 0xFF, s['end_r'] & 0xFF,
                  s['freq_l'] & 0xFF, s['freq_r'] & 0xFF,
                  s['trig_mode'] & 3, s.get('flags', 0) & 3])   # [11] 行为位：原样保留，别清掉


def apply_args(cur, a):
    """把命令行给的字段盖到现值上；给了 fx 就连效果开关位一起改（none = 关掉这一侧）。"""
    s = dict(cur)
    if a.restore:
        s = dict(DEF)
        s['freq_l'] = s['freq_r'] = 30
        return s
    for side, tag in ((0, 'l'), (1, 'r')):
        fx = getattr(a, '%s_fx' % tag)
        if fx is not None:
            v = FX[fx]
            if v is None:                                   # none = 关闭这一侧的效果
                s['effect_mode'] &= ~(1 << side)
            else:
                s['effect_mode'] |= (1 << side)
                s['kind_%s' % tag] = v
        st = getattr(a, '%s_start' % tag)
        if st is not None:
            s['thresh_%s' % tag] = max(0, min(255, st))
            if not s['thresh_%s' % tag]:
                s['effect_mode'] &= ~(1 << side)             # 起始位置 0 = 这一侧关（固件同语义）
        en = getattr(a, '%s_end' % tag)
        if en is not None:
            s['end_%s' % tag] = max(0, min(255, en))
        sv = getattr(a, '%s_str' % tag)
        if sv is not None:
            s['str_%s' % tag] = max(1, min(8, sv))
        fq = getattr(a, '%s_freq' % tag)
        if fq is not None:
            s['freq_%s' % tag] = max(2, min(254, fq))
    if a.hyst is not None:
        s['hyst'] = max(1, min(63, a.hyst))
    if a.mode is not None:
        s['trig_mode'] = MODE_BITS[a.mode]
    return s


def rpc(h, op, reg, off, data):
    return diag.rpc(h, op, reg, off, data) if hasattr(diag, 'rpc') else None


def main():
    ap = argparse.ArgumentParser(description='读写接收机 0x0b（扳机效果）配置区')
    ap.add_argument('--l-fx', '--l-fx-', dest='l_fx', choices=list(FX), help='左：none/resist/weapon/auto')
    ap.add_argument('--r-fx', dest='r_fx', choices=list(FX), help='右：none/resist/weapon/auto')
    ap.add_argument('--l-start', '--l', dest='l_start', type=int, help='左起始位置 0..255（0=关）')
    ap.add_argument('--r-start', '--r', dest='r_start', type=int, help='右起始位置 0..255（0=关）')
    ap.add_argument('--l-end', dest='l_end', type=int, help='左结束位置 0..255（只对「扳机」）')
    ap.add_argument('--r-end', dest='r_end', type=int, help='右结束位置 0..255')
    ap.add_argument('--l-str', '--strength', dest='l_str', type=int, help='左力度 1..8（硬件 8 档）')
    ap.add_argument('--r-str', dest='r_str', type=int, help='右力度 1..8')
    ap.add_argument('--l-freq', dest='l_freq', type=int, help='左频率 2..254 Hz（只对「自动扳机」；硬件 2 Hz/格）')
    ap.add_argument('--r-freq', dest='r_freq', type=int, help='右频率 2..254 Hz')
    ap.add_argument('--hyst', type=int, help='迟滞 1..63')
    ap.add_argument('--mode', choices=list(MODE_BITS), help='哪一侧是「两段模式」（网页右栏那个开关）')
    ap.add_argument('--restore', action='store_true', help='恢复默认（两侧关、效果关、普通模式）')
    a = ap.parse_args()

    h = diag.open_dev()
    if h is None:
        print('[!] 接收器不在（或没答 0xFC）')
        return 1
    try:
        cur = diag.rpc_read(h, REG_S2T, 0, LEN)
        if not cur or len(cur) < LEN:
            print('[!] 读不到 0x0b —— 这台固件的扳机区不对（R2.5 起才有）')
            return 1
        now = unpack(cur)
        show('当前', now)

        want = apply_args(now, a)
        payload = pack(want)
        if payload == bytes(cur):
            print('\n（没有要改的）')
            return 0
        for side, tag in ((0, 'l'), (1, 'r')):
            if (want['trig_mode'] >> side) & 1 and not want['thresh_%s' % tag]:
                print('[i] %s侧是「两段模式」但起始位置 0 —— 固件不会触发二段'
                      '（网页切模式时会自动给 200）' % ('左' if side == 0 else '右'))
        print('\n要写入: ' + ' '.join('%02x' % x for x in payload))
        res = diag.rpc(h, OP_WRITE, REG_S2T, 0, payload)
        if not res or res[0] != 0:
            print('[!] 写 0x0b 失败：%r' % (res,))
            return 1
        res = diag.rpc(h, OP_COMMIT, 0xFF, 0, b'\x00')
        if not res or res[0] != 0:
            print('[!] COMMIT 失败：%r' % (res,))
            return 1
        time.sleep(0.4)

        after = diag.rpc_read(h, REG_S2T, 0, LEN)
        show('\n写后回读', unpack(after))
        ok = bytes(after) == payload
        print('\n回读%s' % ('逐字节一致 ✓（已落盘）' if ok else '不一致 ✗'))
        stt = diag.rpc_read(h, 0x7F, 0, 16)
        if stt:
            print('STATUS flags=0x%02x（待落盘=%d）' % (stt[2], stt[2] & 1))
        if ok and (payload[0] or payload[1]):
            print('现在可以跑 tools/watch_s2t.py 看行程与效果落点了（记得先断开网页）')
        return 0 if ok else 1
    finally:
        try:
            h.close()
        except Exception:
            pass


if __name__ == '__main__':
    sys.exit(main())
