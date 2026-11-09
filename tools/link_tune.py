#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""「接收器当主」开关（0xF6 命令 0x11，不落盘）。R1.5 默认关；**R1.6 起默认开**（拔插恢复开）。

用法：python -X utf8 tools/link_tune.py 0 [秒数]   关闭（已经当上主的这次连接不会切回去，重连后恢复手柄当主）
      python -X utf8 tools/link_tune.py 1 [秒数]   打开：之后每次连上都申请当主（正连着的话几秒内就申请）
设置后每秒采一次 PERF（收到的帧率 / 角色 / 监督超时 / HCI 事件数），并打印新出现的 DIAG 事件，
切换结果（角色变化、监督超时改没改成）都在里面。记录器 watch_state.py 开着也能用。
"""
import sys
import time

import diag


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in ('0', '1'):
        print(__doc__)
        return 2
    on = int(sys.argv[1])
    secs = int(sys.argv[2]) if len(sys.argv) > 2 else 10

    h = diag.open_dev()
    if not h:
        print('[!] 接收器不在（手柄接口没枚举或网页占着）')
        return 1
    pr = diag.parse(diag.fetch_diag(h) or b'')
    last_t = max((r[0] for r in pr[2]), default=0) if pr else 0
    # 0xF6 的描述符声明 63 字节：Windows 要求长度完全一致
    rc = h.send_feature_report(bytes([0xF6]) + bytes([0x11, on]) + bytes(63 - 2))
    print('发送 0xF6/0x11 rc=%d：接收器当主 = %s' % (rc, '开' if on else '关'))
    for k in range(secs):
        time.sleep(1.0)
        d = diag.fetch_perf(h)
        if d and 'lsto' in d:
            print('  %2ds  收%d/s  角色=%s  监督超时=%s  HCI事件=%s' % (
                k + 1, d['rx_win'], {0: '主', 1: '从'}.get(d['role'], '?'),
                ('%.2fs' % (d['lsto'] * 0.000625)) if d['lsto'] else '?', d.get('hci_evt', '?')))
        pr = diag.parse(diag.fetch_diag(h) or b'')
        if pr:
            new = [r for r in pr[2] if r[0] > last_t]
            if new:
                print(diag.fmt(new))
                last_t = max(r[0] for r in new)
    h.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
