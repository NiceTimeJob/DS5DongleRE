#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""现场记录仪：抓「二段触发那一刻的原始行程值」。
判据：输出报文里的 R2 数字位（或 L2/R2 模拟量）从 0 变高，而原始 L2 还没到满行程
—— 那个瞬间的原始 L2 就是**固件实际用的分界点**。

用法（**关掉网页**）：python -X utf8 tools/watch_t2_fire.py 90
然后在这 90 秒内**慢慢把 L2 压到底、再松开**，重复两三次。
"""
import sys
import time

import diag

SECONDS = int(sys.argv[1]) if len(sys.argv) > 1 else 90


def main():
    h = diag.open_dev(retries=6)
    if h is None:
        print('[!] 读不到接收器')
        return 1
    print('记录 %d 秒：请慢慢把 L2 压到底再松开，重复两三次…' % SECONDS)
    t0 = time.time()
    prev_out = 0
    prev_raw = 0
    shots = 0
    peak = 0
    while time.time() - t0 < SECONDS:
        raw = diag.rpc_read(h, 0x10, 0, 8, tries=3)
        out = diag.rpc_read(h, 0x11, 0, 8, tries=3)
        if raw and out:
            l2, r2out = raw[4], out[5]
            peak = max(peak, l2)
            # 输出侧 R2 从"没按"变成"按下"的那一帧 = 二段刚触发
            if r2out > 8 and prev_out <= 8:
                shots += 1
                print('  第 %d 次触发：那一刻原始 L2 = %d   （输出 R2 = %d）' % (shots, l2, r2out))
            elif r2out <= 8 and prev_out > 8:
                print('  退出二段：那一刻原始 L2 = %d' % l2)
            prev_out = r2out
            prev_raw = l2
        time.sleep(0.01)
    print('本轮见过的原始 L2 最大 = %d' % peak)
    print('结论：上面每次"触发时的原始 L2"就是固件实际用的分界点 —— 和 dump_t2.py 显示的值对一下。')
    h.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
