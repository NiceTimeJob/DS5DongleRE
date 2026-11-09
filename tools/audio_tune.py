#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""S36：运行时调音频编码参数（不落盘，拔插即恢复默认 160 kbps / 200 B）。

用法：python -X utf8 tools/audio_tune.py <kbps> [bw] [mode] [cx] [秒数]
  kbps  8..160       码率；CBR 下每帧字节 = kbps * 1.25（160 → 200 B，64 → 80 B）
  bw    0..5         0 自动 / 1 窄带 4k / 2 中带 6k / 3 宽带 8k / 4 超宽带 12k / 5 全带 20k
  mode  0|1          0 = 补填充到 200 B（线上格式不变，安全）/ 1 = 按真实长度发（验证手柄认不认变长）
  cx    0..10        编码复杂度（现在固件默认 0）
  秒数  默认 8       设置后连续采样几秒 PERF，打印编码耗时与丢块/失败增量
例：python -X utf8 tools/audio_tune.py 64 3 0 0
"""
import struct
import sys
import time

import diag


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    kbps = int(sys.argv[1])
    bw = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    mode = int(sys.argv[3]) if len(sys.argv) > 3 else 0
    cx = int(sys.argv[4]) if len(sys.argv) > 4 else 0
    secs = int(sys.argv[5]) if len(sys.argv) > 5 else 8

    h = diag.open_dev()
    if not h:
        print('[!] 接收器不在（手柄接口没枚举或网页占着）')
        return 1
    # 0xF6 的描述符声明 63 字节：Windows 要求长度完全一致
    payload = bytes([0x10, kbps & 0xFF, (kbps >> 8) & 0xFF, bw, mode, cx]) + bytes(63 - 6)
    rc = h.send_feature_report(bytes([0xF6]) + payload)
    print('发送 0xF6/0x10 rc=%d : %d kbps bw=%d mode=%d cx=%d' % (rc, kbps, bw, mode, cx))
    prev = None
    for k in range(secs):
        time.sleep(1.0)
        pf = diag.fetch_perf(h)
        if not pf:
            print('  t+%ds PERF 读不到' % (k + 1))
            continue
        line = ('  t+%ds 编码窗口max=%.1fms 块窗口max=%.1fms 当前=%dkbps bw=%d mode=%d cx=%d'
                % (k + 1, pf['enc_win_us'] / 1000, pf['blk_win_us'] / 1000,
                   pf.get('tune_kbps', 0), pf.get('tune_bw', 0), pf.get('tune_mode', 0), pf.get('tune_cx', 0)))
        if prev:
            line += ('  | +块 %d +丢块 %d +发送失败 %d (池空 +%d l2cap +%d)'
                     % (pf['blocks'] - prev['blocks'], pf['overruns'] - prev['overruns'],
                        pf['send_fail'] - prev['send_fail'],
                        pf.get('fail_pool', 0) - prev.get('fail_pool', 0),
                        pf.get('fail_l2cap', 0) - prev.get('fail_l2cap', 0)))
        print(line)
        prev = pf
    if prev:
        print('  CPU ' + ' '.join('%s=%.1f%%' % (n, pm / 10.0) for n, pm in prev['tasks']))
    h.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
