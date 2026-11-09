#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""复现网页 `protocol.js::readRegion()` 的截断 bug（2026-10-11 用户报的
「覆盖层三角→十字，固件生效但网页不显示」就是它）。

网页原来用 CHUNK=122 读区域：
  const n = Math.min(122, total - got);
  const d = await rpc(READ, region, off + got, [n]);
  got += Math.min(d.length, n);
  if (d.length < n) break;          ← 这里中招
但 0xFC 的 GET 响应最多只有 **121** 字节数据：
  描述符声明 127 字节报告 payload（不含报告号），固件又在 feature_resp_buf[0]
  塞了 0xFC 当"报告号"，于是 127 - 1(0xFC) - 5(RPC 头) = **121**。
于是每次要 122、只回 121 → `d.length < n` 成立 → **第一块就读完 break**，
整块 5850 字节的槽位区只有前 121 字节是真的，其余全 0 → 界面里除"主层前 4 个键"
以外全是"本体/默认"。

本脚本在真机上把两种读法各跑一遍，对比 layer1/△/单击 那个槽（偏移 1260）：
  用 122（旧网页）→ 0,0,0,0,0（所以网页不显示）
  用 121（修好后）→ 2,15,0,0,0（out=PAD, val=↑，固件里就是这么存的）

用法: python -X utf8 repro_web_readchunk.py

**注意**：这个脚本的偏移按 **R2.4 及以前**的 39 键布局（5850 B）算的；R2.5 起映射表是 41 键
（6150 B），层步长变了，脚本里的 TOTAL/NEEDLE_OFF 要跟着改才对应得上（当时那台设备的
layer1/△ 配置也早被版本守卫清掉了）。留档用。
"""
import sys

import diag

REG_SLOTS = 0x01
TOTAL = 5 * 39 * 6 * 5          # 5850
NEEDLE_OFF = (1 * 39 + 3) * 6 * 5   # layer1, btn3(△), form0(单击) → 1260


def web_read_region(h, region, total, chunk):
    """照抄网页 readRegion 的逻辑（含那句 `if (d.length < n) break`）。"""
    out = bytearray(total)
    got = 0
    chunks = 0
    while got < total:
        n = min(chunk, total - got)
        d = diag.rpc_read(h, region, got, n)
        chunks += 1
        if not d:
            break
        out[got:got + min(len(d), n)] = d[:n]
        got += min(len(d), n)
        if len(d) < n:
            break                       # ← 旧网页的行为
    return bytes(out), got, chunks


def main():
    h = diag.open_dev()
    if h is None:
        print('[!] 接收器不在')
        return 1
    for chunk in (122, 121):
        buf, got, chunks = web_read_region(h, REG_SLOTS, TOTAL, chunk)
        slot = buf[NEEDLE_OFF:NEEDLE_OFF + 5]
        nz = sum(1 for i in range(0, len(buf), 5) if buf[i:i + 5] != b'\0\0\0\0\0')
        print('chunk=%-4d 实际读回 %4d/%d 字节，发了 %d 块；非空槽位 %d 个' %
              (chunk, got, TOTAL, chunks, nz))
        print('           偏移 %d（layer1 △ 单击）= %s' % (NEEDLE_OFF, ' '.join('%02x' % b for b in slot)))
    h.close()
    print()
    print('结论：chunk=122 时只读了 121 字节就 break，网页看到的就是"什么都没有"；')
    print('      chunk=121 才对。修的是网页 protocol.js，不用动固件、不用重烧。')
    return 0


if __name__ == '__main__':
    sys.exit(main())
