#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""直接监听接收器的**鼠标 HID 接口**，把真实发出的光标位移打出来。

和 verify_fw.py 的 0xFC 通道完全无关（这是另一个 HID 接口的输入报告），所以
跟网页同时用也不会互相抢通道。

面板：接收器现在会暴露 键盘(0x01/0x06)、消费类(0x0C/0x01)、鼠标(0x01/0x02) ——
本脚本只开鼠标那一项（usage_page=0x0001, usage=0x0002）。

用法: python -X utf8 watch_mouse.py [秒数]     （默认 60 秒）
然后按你配的启用键（现在是 L2）再转动手柄；屏幕会打印 report id 和 dx/dy。
"""
import sys
import time

import hid

VID = 0x054C
RID_MOUSE = 3          # kbd_report_desc 里 Report ID 3 = 鼠标


def main():
    secs = float(sys.argv[1]) if len(sys.argv) > 1 else 60.0
    cand = [d for d in hid.enumerate(VID)
            if d['usage_page'] == 0x0001 and d['usage'] == 0x02]
    if not cand:
        print('[!] 没找到鼠标接口 —— 设备里跑的还是"键鼠接口被 gating 掉"的老固件？')
        return 1
    print('鼠标接口: %s' % cand[0]['path'].decode('utf-8', 'replace')[-40:])
    h = hid.device()
    h.open_path(cand[0]['path'])
    try:
        h.set_nonblocking(0)
    except Exception:
        pass
    print('监听 %.0f 秒…… 按住启用键(L2)再转手柄' % secs)
    t0 = time.time()
    n = 0
    moved = 0
    while time.time() - t0 < secs:
        try:
            d = h.read(64, 200)
        except Exception as e:
            print('  read 失败: %s' % e)
            break
        if not d:
            continue
        n += 1
        rid, payload = d[0], d[1:]
        # 布局跟固件一致：[buttons][dx][dy][wheel]
        btn = payload[0] if len(payload) > 0 else 0
        dx = payload[1] - 256 if len(payload) > 1 and payload[1] > 127 else (payload[1] if len(payload) > 1 else 0)
        dy = payload[2] - 256 if len(payload) > 2 and payload[2] > 127 else (payload[2] if len(payload) > 2 else 0)
        if dx or dy or btn:
            moved += 1
            if moved <= 20:
                print('%6.2fs  id=%d 左/右/中=%d  dx=%d dy=%d' % (time.time() - t0, rid, btn, dx, dy))
    h.close()
    print('共收到 %d 条输入报告，其中有位移/按键的 %d 条' % (n, moved))
    if n == 0:
        print('[i] 一条输入报告都没有 —— 鼠标报告没发出来（或启用键没按住）')
    elif moved == 0:
        print('[i] 有报告但位移恒为 0 —— 陀螺仪没在产生鼠标量')
    else:
        print('[ok] 鼠标通路是通的')
    return 0


if __name__ == '__main__':
    sys.exit(main())
