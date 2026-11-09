#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""探针：0x0d（体感手势区，分界点暂存在偏移 24/25）到底能不能读写。
对照组：0x0b（扳机区，网页保存时一直在用、确定通）。"""
import sys

import diag

OPS = {1: 'READ', 2: 'WRITE', 3: 'COMMIT'}


def rpc_write(h, region, off, data):
    """op=2 写；帧格式 [op region off_lo off_hi len data...]，响应 [状态 op region off_lo off_hi ...]"""
    import time as _t
    tx = bytearray(diag.RPT_LEN)
    tx[0] = 2
    tx[1] = region
    tx[2] = off & 0xFF
    tx[3] = (off >> 8) & 0xFF
    tx[4] = len(data)
    tx[5:5 + len(data)] = data
    if h.send_feature_report(bytes([diag.RPC_ID]) + bytes(tx)) < 0:
        return None
    for _ in range(40):
        _t.sleep(0.003)
        resp = bytes(h.get_feature_report(diag.RPC_ID, diag.RPT_LEN + 1))
        body = resp[1:] if resp and resp[0] == diag.RPC_ID else resp
        if not body or body[0] == 2:
            continue
        if len(body) >= 5 and body[1] == 2 and body[2] == region and (body[3] | (body[4] << 8)) == off:
            return body[0]
    return None


def rpc_commit(h):
    import time as _t
    tx = bytearray(diag.RPT_LEN)
    tx[0] = 3
    tx[1] = 0xFF
    h.send_feature_report(bytes([diag.RPC_ID]) + bytes(tx))
    _t.sleep(0.4)


def probe(h, region, off, name):
    rd = diag.rpc_read(h, region, off, 4, tries=40)
    print('  %-16s 0x%02x off %-3d 读 = %s'
          % (name, region, off, (' '.join('%02x' % b for b in rd) if rd else '**失败/回 ERR**')))
    return rd


def main():
    h = diag.open_dev(retries=6)
    if h is None:
        print('[!] 读不到接收器')
        return 1
    print('对照（确定通的区）：')
    probe(h, 0x0B, 0, 'S2T 扳机区')
    print('待查：')
    g = probe(h, 0x0D, 0, 'GEST 手势区')
    probe(h, 0x0D, 24, 'GEST 偏移 24')
    probe(h, 0x0C, 0, 'FLICK 区')
    # 往 GEST 偏移 24 写一个探针值（0xA5），再读回来；最后尽力还原
    old = (g or [])[24:25] if g and len(g) > 24 else None
    print('写 0xa5 → 偏移 24：状态 %s' % rpc_write(h, 0x0D, 24, b'\xa5'))
    back = diag.rpc_read(h, 0x0D, 24, 1, tries=40)
    print('  读回 = %s' % (' '.join('%02x' % b for b in back) if back else '**失败**'))
    if back and back[0] == 0xA5:
        print('  ⇒ **这个区可写**：那问题在网页那条写入路径上（不是固件）')
    else:
        print('  ⇒ **这个区写不进去**：固件侧没这个区的写入口（或被尺寸/偏移拒掉）')
    # 顺手验证 COMMIT 是否把它落盘（拔插后还在 = 落盘成功）
    rpc_commit(h)
    print('  已 COMMIT（这个探针留在 0xa5，你下次 dump 会看到它 —— 用来验证"落盘"这条路通不通）')
    h.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
