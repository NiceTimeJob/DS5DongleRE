#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""一次把「两段扳机」相关的全部实数读出来（省得再来回问）：
  ① 0x0b 的 14 字节：两段模式位 / 分界点 / 手感参数；
  ② **二段派生键（39=L2二段 / 40=R2二段）在 6 个形态里的映射**（层 0 主层）——
     这是"压到底也不触发 R2"最可能的原因：映射被清空了（每次切回普通模式都会清）。

用法（**先关掉网页**）：python -X utf8 tools/dump_t2_full.py
"""
import sys

import diag

REG_S2T, REG_SLOTS = 0x0B, 0x01
LAYERS, BTNS, FORMS = 5, 41, 6
SLOT = 5
FORMS_CN = ['单击', '长按', '双击', '三击', '按下瞬间', '松开瞬间']
OUT_SELF, OUT_NONE = 0, 1          # 0 = 原键（默认，等于没配）


def main():
    h = diag.open_dev(retries=6)
    if h is None:
        print('[!] 读不到接收器（网页还开着？）')
        return 1
    s2t = diag.rpc_read(h, REG_S2T, 0, 14, tries=40)
    if not s2t or len(s2t) < 14:
        print('[!] 0x0b 读不到（固件要 R3.6 才有 14 字节）')
        return 1
    ver = diag.rpc_read(h, 0x15, 0, 40, tries=60)
    if ver:
        tag = bytes(ver[4:20]).split(b'\x00')[0].decode('ascii', 'ignore')
        vs = bytes(ver[20:36]).split(b'\x00')[0].decode('ascii', 'ignore')
        print('★ 设备里跑的固件版本 = %s   build=%s' % (vs or '?', tag or '?'))
        print('  （要验 R3.6 的行为，这里必须是 R3.6 或更高）')
    print('0x0b = %s' % ' '.join('%02x' % b for b in s2t))
    for w, nm in ((0, 'L2'), (1, 'R2')):
        two = (s2t[10] >> w) & 1
        th = s2t[0] if w == 0 else s2t[1]
        bnd = s2t[12] if w == 0 else s2t[13]
        print('%s：两段模式位=%d  分界点=%d%s  手感起始=%d' %
              (nm, two, bnd, '（0=没设过→回落手感起始）' if not bnd else '', th))

    print('\n二段派生键（主层）的映射：')
    for btn, nm in ((39, 'L2 二段'), (40, 'R2 二段')):
        off = (0 * BTNS + btn) * FORMS * SLOT
        raw = diag.rpc_read(h, REG_SLOTS, off, FORMS * SLOT, tries=60)
        if not raw:
            print('  %s：读不到' % nm)
            continue
        got = []
        for f in range(FORMS):
            s = raw[f * SLOT:(f + 1) * SLOT]
            if s[0] not in (OUT_SELF, OUT_NONE):
                got.append('%s→out=%d val=%d mod=%d flags=%d' % (FORMS_CN[f], s[0], s[1], s[2], s[3]))
        print('  %s：%s' % (nm, '，'.join(got) if got else '**空的**（＝没配，压到底也不会触发任何东西）'))
    h.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
