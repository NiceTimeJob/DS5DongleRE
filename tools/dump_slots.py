#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把设备里的映射槽位（0x01 SLOTS，R2.5 起 6150 B，R2.5 之前 5850 B）全dump出来，只打印非默认项。

槽位布局与固件 ds5map.h / 网页 page-map.js 必须完全一致：
  每个槽 5 字节 {out, val, mod, flags, param}
  idx = (layer*41 + btn)*6 + form      → 偏移 = idx*5   （R2.5 起 41 键；之前是 39）
  form: 0=单击 1=长按 2=双击 3=三击 4=按下 5=松开
  out : 0=本体 1=无 2=手柄键 3=键盘 4=鼠标 5=宏 6=振动 7=战备

用法: python -X utf8 dump_slots.py [layer]
  layer 省略 = 全部 5 层
输出用 ASCII 名字（控制台是 GBK，中文会花）。
"""
import sys

import diag

REG_SLOTS = 0x01
BTNS = ['SQR', 'CROSS', 'CIRC', 'TRI', 'L1', 'R1', 'L2', 'R2', 'CREATE', 'OPTIONS',
        'L3', 'R3', 'PS', 'TP', 'MUTE', 'UP', 'DOWN', 'LEFT', 'RIGHT']
BTNS += ['TPF1_T', 'TPF1_U', 'TPF1_D', 'TPF1_L', 'TPF1_R', 'TPF1_C',
         'TPF2_T', 'TPF2_U', 'TPF2_D', 'TPF2_L', 'TPF2_R', 'TPF2_C']
BTNS += ['FN_L', 'FN_R', 'BACK_L', 'BACK_R', 'V1', 'V2', 'V3', 'V4',
         'L2_S2', 'R2_S2']      # R2.5：两段式扳机的两个派生源键
FORMS = ['tap', 'hold', 'dtap', 'ttap', 'press', 'release']
OUTS = {0: 'SELF', 1: 'NONE', 2: 'PAD', 3: 'KBD', 4: 'MOUSE', 5: 'MACRO',
        6: 'VIB', 7: 'STRAT'}

NB = 41          # map_btn 数量（R2.5 起 41：含 V1..V4 与 L2/R2 二段）
NF = 6


def name_btn(i):
    return BTNS[i] if i < len(BTNS) else 'BTN%d' % i


def main():
    want = int(sys.argv[1]) if len(sys.argv) > 1 else None
    h = diag.open_dev()
    if h is None:
        print('[!] 接收器不在')
        return 1
    total = 5 * NB * NF * 5
    raw = diag.rpc_read_units(h, REG_SLOTS, total, 5)
    h.close()
    print('SLOTS: 读到 %d/%d 字节（应能被 5 整除）' % (len(raw), total))
    if len(raw) < total:
        print('[!] 没读全，下面的结论不可靠')
    n = 0
    for layer in range(5):
        if want is not None and layer != want:
            continue
        for btn in range(NB):
            for form in range(NF):
                idx = (layer * NB + btn) * NF + form
                o = idx * 5
                if o + 5 > len(raw):
                    break
                out, val, mod, flags, param = raw[o:o + 5]
                if (out, val, mod, flags, param) == (0, 0, 0, 0, 0):
                    continue       # 默认＝本体，不打印
                tgt = ''
                if out == 2:
                    tgt = ' -> %s(%d)' % (name_btn(val), val)
                elif out == 3:
                    tgt = ' -> keycode %d mod=0x%02x' % (val, mod)
                elif out == 4:
                    tgt = ' -> mouse %d' % val
                elif out == 5:
                    tgt = ' -> macro %d' % val
                elif out == 7:
                    tgt = ' -> strat %d' % val
                print('layer=%d  %-8s %-7s  out=%-5s val=%-3d flags=0x%02x param=%d%s'
                      % (layer, name_btn(btn), FORMS[form], OUTS.get(out, out),
                         val, flags, param, tgt))
                n += 1
    print('共 %d 个非默认槽位' % n)
    if n == 0:
        print('（一个都没有 → 设备里的映射是空的！）')
    return 0


if __name__ == '__main__':
    sys.exit(main())
