# -*- coding: utf-8 -*-
"""读接收机里的「配置身份牌」（R2.4 起，RPC 区 0x18，40 字节）——只读，不改设备。

  0   name[32]   配置名 UTF-8，'\\0' 结尾；全 0 = 从没写过
  32  u32 saved_at  这份内容在电脑上保存的时刻（Unix 秒）
  36  u32 hash      网页 packRegions() 的 FNV-1a

用法：python -X utf8 tools/read_cfgid.py
网页开着也能读（单次 RPC，撞上网页的轮询会自动重试；网页那边要连续 3 次失败才会记日志）。
老固件（R2.3 及以前）没有这个区，会读失败。
"""
import struct
import sys
import time

import diag

REG_CFG_ID = 0x18


def main():
    h = diag.open_dev()
    if h is None:
        print('[!] 接收器不在（或没有 0xFC 通道）')
        return 1
    try:
        b = None
        for _ in range(5):
            b = diag.rpc_read(h, REG_CFG_ID, 0, 40)
            if b is not None and len(b) >= 40:
                break
            time.sleep(0.15)
        if b is None or len(b) < 40:
            print('[!] 读不出 0x18：固件没有这个区（R2.3 及以前），或者一直撞上网页的轮询')
            return 2
        raw = bytes(b[:40])
        name_b = raw[:32].split(b'\0', 1)[0]
        saved_at, h32 = struct.unpack_from('<II', raw, 32)
        print('原始 40 字节：' + raw.hex(' '))
        if not name_b and not saved_at and not h32:
            print('身份牌：空的（从没写过配置名）')
        else:
            print('配置名：%r（%d 字节）' % (name_b.decode('utf-8', 'replace'), len(name_b)))
            print('保存时间：%s（%d）' % (time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(saved_at))
                                       if saved_at else '无', saved_at))
            print('哈希：%08x' % h32)
        return 0
    finally:
        try:
            h.close()
        except Exception:
            pass


if __name__ == '__main__':
    sys.exit(main())
