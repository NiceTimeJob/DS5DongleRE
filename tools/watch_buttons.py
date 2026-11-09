#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""盯住 0x10 TLM_RAW 的按键位，打印按下/松开 —— 这就是网页「捕获输入」和宏录制
读的那份数据（映射前的原始报文），所以它能单独证明"按键读得到"。

解码表和 web/js/pad-art.js 的 buttonsFromReport() 一一对应（别改一处忘另一处）：
  [7] 低 4 位 = 十字键 hat（0..7 = ↑↗→↘↓↙←↖，8..15 = 没按）
  [7] 高 4 位 = □(0) ✕(1) ○(2) △(3)
  [8] = L1(4) R1(5) L2(6) R2(7) 创建(8) 选项(9) L3(10) R3(11)
  [9] = PS(12) 触摸板(13) 静音(14) | 左Fn(31) 右Fn(32) 左背(33) 右背(34)

用法: python -X utf8 watch_buttons.py [秒数]      （默认 60 秒）
然后按手柄上的键，屏幕上会出 "按下 □ / 松开 □"。
"""
import sys
import time

import diag

NAMES = {0: '□', 1: '✕', 2: '○', 3: '△', 4: 'L1', 5: 'R1', 6: 'L2', 7: 'R2',
         8: '创建', 9: '选项', 10: 'L3', 11: 'R3', 12: 'PS', 13: '触摸板', 14: '静音',
         15: '↑', 16: '↓', 17: '←', 18: '→',
         31: '左Fn', 32: '右Fn', 33: '左背键', 34: '右背键'}
BITS = [(7, 0x10, 0), (7, 0x20, 1), (7, 0x40, 2), (7, 0x80, 3),
        (8, 0x01, 4), (8, 0x02, 5), (8, 0x04, 6), (8, 0x08, 7),
        (8, 0x10, 8), (8, 0x20, 9), (8, 0x40, 10), (8, 0x80, 11),
        (9, 0x01, 12), (9, 0x02, 13), (9, 0x04, 14),
        (9, 0x10, 31), (9, 0x20, 32), (9, 0x40, 33), (9, 0x80, 34)]
HAT = {0: [15], 1: [15, 18], 2: [18], 3: [16, 18], 4: [16], 5: [16, 17], 6: [17], 7: [17, 15]}


def decode(p):
    """-> 按下的 id 集合；p 是 63 字节原始报文（摇杆四字节全 0 = 还没收到过报文 → None）"""
    if not p or len(p) < 10:
        return None
    if not any(p[0:4]):
        return None
    ids = set()
    for o, m, i in BITS:
        if p[o] & m:
            ids.add(i)
    ids.update(HAT.get(p[7] & 0x0F, []))
    return ids


def main():
    secs = float(sys.argv[1]) if len(sys.argv) > 1 else 60.0
    h = diag.open_dev()
    if h is None:
        print('[!] 接收器不在（或没答 0xFC）')
        return 1
    print('盯 0x10 按键位 %.0f 秒…… 按手柄上的键试试' % secs)
    t0 = time.time()
    prev = None
    n = 0
    seen_input = False
    try:
        while time.time() - t0 < secs:
            p = diag.rpc_read(h, 0x10, 0, 63, tries=30)
            if p:
                ids = decode(p)
                if ids is not None:
                    seen_input = True
                    if prev is None:
                        prev = ids
                        print('  基准：现在按着 %s' % ('、'.join(NAMES.get(i, str(i)) for i in sorted(ids)) or '（无）'))
                    else:
                        for i in sorted(ids - prev):
                            print('%6.2fs  按下 %s (id=%d)   p7=%02x p8=%02x p9=%02x'
                                  % (time.time() - t0, NAMES.get(i, '?'), i, p[7], p[8], p[9]))
                        for i in sorted(prev - ids):
                            print('%6.2fs  松开 %s (id=%d)' % (time.time() - t0, NAMES.get(i, '?'), i))
                        prev = ids
                        n += 1
            time.sleep(0.02)
    finally:
        try:
            h.close()
        except Exception:
            pass
    if not seen_input:
        print('[!] 一头都没读到有效报文：手柄没连（0x10 还是全 0）')
        return 1
    print('结束：共 %d 次变化' % n)
    return 0


if __name__ == '__main__':
    sys.exit(main())
