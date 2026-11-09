#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""打印设备里和「两段分界 / 扳机手感」有关的**实数**，用来对齐"界面显示 vs 设备实际"。

用法（**先关掉网页配置工具**：0xFC 一次只服务一个客户端）：
    python -X utf8 tools/dump_t2.py

R3.25 起**整套扳机设置按层**，所以这个脚本也按层打：
  1) `0x0b` 区的字节（主层：[0..13]；覆盖层 1..4：[14..25] 两段位/分界点、[26..65] 自适应扳机）；
  2) 每一层那两个分界点的**实际生效值**（按固件 ds5map_t2_bound_layer() 的规则：
     0 = 从没设过 → 回落到手感起始位置）；
  3) 每一层那套「自适应扳机」（效果/起始/结束/力度/类型/频率/行为位）；
  4) 提醒：固件判定/算效果时取的是**当前激活层**那一份。

于是：**分界点界面写 110、但压到 51 就触发** 这类问题，看一眼就知道是
"这一层分界点还是 0/旧值"，还是"分界点写进去了、固件用的却是别的数"。
"""
import sys

import diag

REG_S2T = 0x0B
KINDS = ['阻力', '扳机', '自动扳机']
LAYERS = ['主层（默认层）', '覆盖层 1', '覆盖层 2', '覆盖层 3', '覆盖层 4']
LEN = 66          # R3.25 起 66 字节（老固件 14 字节，读不满就按能读到的部分打）
FX_PER_LAYER = 10  # 覆盖层那套自适应扳机的列数（= 固件 S2T_FX_COUNT）


def mode_of(b, layer):
    """层 layer 的「两段模式」位（层 0 用 [10]，覆盖层用 [14..17]）。"""
    return (b[10] if layer == 0 else (b[14 + layer - 1] if len(b) > 14 + layer - 1 else 0)) & 3


def bound_of(b, layer, w):
    """层 layer、w（0=左 1=右）的分界点字节；0 = 没设过。"""
    if layer == 0:
        return b[12 + w] if len(b) > 13 else 0
    i = (18 if w == 0 else 22) + (layer - 1)     # 覆盖层 1..4：[18..21] 左 / [22..25] 右
    return b[i] if len(b) > i else 0


def fx_of(b, layer, k):
    """覆盖层 layer（1..4）那套自适应扳机的第 k 列（列号见固件 S2T_FX_*）；读不到 = 0。"""
    off = 26 + (layer - 1) * FX_PER_LAYER + k
    return b[off] if len(b) > off else 0


def main():
    h = diag.open_dev(retries=6)
    if h is None:
        print('[!] 读不到接收器（网页还开着？拔插一下再试）')
        return 1
    b = diag.rpc_read(h, REG_S2T, 0, LEN, tries=60)
    h.close()
    if not b or len(b) < 14:
        print('[!] 0x0b 读不到（读超时或固件太老：R2.5 起才有这个区）—— 再跑一次，或先关掉网页）')
        return 1
    b = list(b)
    has_t2 = len(b) >= 26
    has_fx = len(b) >= 66
    print('0x0b = %s  （%d 字节：%s）'
          % (' '.join('%02x' % x for x in b), len(b),
             'R3.25 起：整套按层' if has_fx else
             ('按层但只有两段位/分界点' if has_t2 else 'R3.24 及以前的老布局，只有一份（按主层看）')))
    if not has_fx:
        print('    ⚠ 这台固件还没有「自适应扳机按层」的字节（R3.25 才有）：'
              '覆盖层那份按主层算、改哪层都一样。')

    for w, name in ((0, '左 L2'), (1, '右 R2')):
        th = b[0] if w == 0 else b[1]
        en = b[6] if w == 0 else b[7]
        kind = (b[5] & 3) if w == 0 else ((b[5] >> 2) & 3)
        strn = (b[4] & 0x0F) if w == 0 else ((b[4] >> 4) or 6)
        freq = b[8] if w == 0 else b[9]
        on = (b[3] >> w) & 1
        print('%s：主层手感[%s 起始 %d 结束 %d 力度 %d 频率 %d]'
              % (name, KINDS[kind] if on else '关闭', th, en, strn, freq))

    print('两段模式 / 分界点（每层一套；0 = 没设过 → 回落到上面那侧的起始位置）：')
    for layer in range(5):
        if layer > 0 and not has_t2:
            continue
        for w, side in ((0, '左'), (1, '右')):
            m = (mode_of(b, layer) >> w) & 1
            raw = bound_of(b, layer, w)
            eff = raw if raw else (b[0] if w == 0 else b[1])
            print('  %-10s %s：模式=%s  分界点字节=%3d %s → 实际生效 = %d（档 %d）'
                  % (LAYERS[layer], side, '两段' if m else '普通', raw,
                     '(没设过，回落手感起始位置)' if not raw else '', eff,
                     min((eff * 10 + 127) // 255, 9)))

    if has_fx:
        print('自适应扳机（每层一套；主层那套见上面两行）：')
        for layer in range(1, 5):
            eff = fx_of(b, layer, 0)
            on = []
            if eff & 1:
                on.append('左开')
            if eff & 2:
                on.append('右开')
            kd = fx_of(b, layer, 6)
            kl, kr = kd & 3, (kd >> 2) & 3
            st = fx_of(b, layer, 5)
            print('  %-10s 效果=%s 起始 L%d/R%d 结束 L%d/R%d 力度 L%d/R%d 类型 L%s/R%s 频率 L%d/R%d 行为位=%d'
                  % (LAYERS[layer], '/'.join(on) or '关闭',
                     fx_of(b, layer, 1), fx_of(b, layer, 2),
                     fx_of(b, layer, 3), fx_of(b, layer, 4),
                     (st & 0x0F) or 6, (st >> 4) or 6,
                     KINDS[kl] if kl <= 2 else '?', KINDS[kr] if kr <= 2 else '?',
                     fx_of(b, layer, 7) or 10, fx_of(b, layer, 8) or 10, fx_of(b, layer, 9)))

    print('行为位 flags(主层) = %d（bit0 忽略游戏原生效果 / bit1 压穿释放一段）  迟滞 = %d（不参与判定）'
          % (b[11] & 3, b[2] or 8))
    print('固件判定/算效果时取的是**当前激活层**那一份：没激活覆盖层 → 主层；覆盖层激活期间 → 覆盖层。')
    return 0


if __name__ == '__main__':
    sys.exit(main())
