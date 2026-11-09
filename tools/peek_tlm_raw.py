#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""看一眼固件遥测 0x10（TLM_RAW = 映射前的原始手柄报文），用来核对网页
`pad-art.js::buttonsFromReport()` 的字节假设：

  p[0..3] 摇杆回中时应该是 0x80（不是 0 —— 0 说明这块缓冲从没被写过）
  p[7] 低 4 位 = 十字键 hat，回中时应该是 8
  p[8] / p[9] = 键位字节

只读，不改设备里的任何东西。用法：
  D:\\Python\\python3.11.4\\python.exe .scratch\\peek_tlm_raw.py
"""
import sys
import time

import diag

REG_TLM_RAW = 0x10
REG_TLM_STATE = 0x12


def dump(tag, b, n=16):
    if b is None:
        print('%s: 读不出来' % tag)
        return None
    print('%s (%d B): %s' % (tag, len(b), ' '.join('%02x' % x for x in b[:n])))
    return b


def main():
    h = diag.open_dev()
    if h is None:
        print('[!] 接收器不在（或没有 0xFC 通道）')
        return 1
    try:
        for k in range(5):
            p = dump('0x10 TLM_RAW  #%d' % k, diag.rpc_read(h, REG_TLM_RAW, 0, 63))
            if p and len(p) >= 10:
                hat = p[7] & 0x0F
                at_rest = (p[0], p[1], p[2], p[3]) == (0x80, 0x80, 0x80, 0x80)
                print('    摇杆=%02x %02x %02x %02x (%s)  hat=%d  p8=%02x p9=%02x'
                      % (p[0], p[1], p[2], p[3],
                         '回中' if at_rest else '有偏移',
                         hat, p[8], p[9]))
                if not any(p[0:4]):
                    print('    [!] 四个摇杆字节全 0：固件还没处理过手柄报文'
                          '（或手柄没连）→ 网页会忽略这一帧')
                elif hat > 7:
                    print('    [ok] hat=%d（没按十字键），可用来验证"新按下"判定' % hat)
            time.sleep(0.2)
    finally:
        try:
            h.close()
        except Exception:
            pass
    return 0


if __name__ == '__main__':
    sys.exit(main())
