#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把设备里的陀螺仪配置（0x07 区，14 字节）写成**当前固件的默认值**，并回读确认。

固件默认（src/ds5map.c::cfg_defaults()；2026-10-11 用户指定改成他那套，S16 起生效）：
   target      = 3  **右摇杆**（原来是 1 鼠标）
   axis_h      = 1  偏摆 yaw
   axis_v      = 0  俯仰 pitch
   inv_h/inv_v = 0  不反向
   deadzone    = 8
   enable_mode = 1  **常开**（原来是 0 关闭）
   enable_btn  = 0xFF 未绑定
   sens_h/v    = 132 / 140       （Q4，132 = 8.25×；原来是 48 = 3.0×）
   gravity     = 1  本体轴（固件强制）
   rsv2        = 0
即 14 字节： 03 01 00 00 00 08 01 ff 84 00 8c 00 01 00

**为什么还要单独写一次**：设备 LittleFS 里已经存着一份配置（而且 DS5MAP_CFG_VER
没变，版本检查不会拦它），固件启动时读的是那份，**默认值只在"没有配置/版本不符"时
才生效**。所以改完固件默认值之后，要么用本脚本把这份配置也写成同一套，要么把
DS5MAP_CFG_VER +1 让旧配置整体作废（代价：映射/宏一起清空）。

只写 0x07 这一个区 + COMMIT(0xFF)，**不碰** 槽位/宏/其他设置。
用法: python -X utf8 set_gyro_default.py
"""
import sys
import time

import diag

REG_GYRO = 0x07
OP_WRITE = 2
OP_COMMIT = 3
RPT_LEN = 127
RPC_ID = 0xFC

DEFAULT = bytes([0x03, 0x01, 0x00, 0x00, 0x00, 0x08, 0x01, 0xFF,
                 0x84, 0x00, 0x8C, 0x00, 0x01, 0x00])

TGT = {0: '关', 1: '鼠标', 2: '左摇杆', 3: '右摇杆'}
EN = {0: '关闭', 1: '常开', 2: '按住按键启用', 3: '开关按键启用'}
AX = {0: '俯仰 pitch', 1: '偏摆 yaw', 2: '翻滚 roll'}


def rpc(h, op, region, off, data=b'', tries=90):
    """发一条 RPC（写/提交），等标签匹配的最终响应；返回 (status, body) 或 None。"""
    tx = bytearray(RPT_LEN)
    tx[0] = op
    tx[1] = region
    tx[2] = off & 0xFF
    tx[3] = (off >> 8) & 0xFF
    tx[4] = len(data)
    tx[5:5 + len(data)] = data
    if h.send_feature_report(bytes([RPC_ID]) + bytes(tx)) < 0:
        return None
    for _ in range(tries):
        time.sleep(0.003)
        try:
            resp = bytes(h.get_feature_report(RPC_ID, RPT_LEN + 1))
        except Exception:
            return None
        body = resp[1:] if resp and resp[0] == RPC_ID else resp
        if not body or body[0] == 2:          # 空 / PENDING
            continue
        if (len(body) < 5 or body[1] != op or body[2] != region
                or (body[3] | (body[4] << 8)) != off):
            continue                          # 陈旧响应，继续等
        return body[0], body[5:]
    return None


def show(tag, g):
    print('%s: %s' % (tag, ' '.join('%02x' % b for b in g)))
    print('   target=%-6s enable_mode=%-12s enable_btn=0x%02x'
          % (TGT.get(g[0], '?'), EN.get(g[6], '?'), g[7]))
    print('   水平=%s 垂直=%s  inv=%d/%d  死区=%d  灵敏度=%d/%d  gravity=%d'
          % (AX.get(g[1], '?'), AX.get(g[2], '?'), g[3], g[4], g[5],
             g[8] | (g[9] << 8), g[10] | (g[11] << 8), g[12]))


def main():
    h = diag.open_dev()
    if h is None:
        print('[!] 接收器不在')
        return 1
    try:
        before = diag.rpc_read(h, REG_GYRO, 0, 14)
        if not before or len(before) < 14:
            print('[!] 读不到 0x07')
            return 1
        show('改之前', before)
        if bytes(before) == DEFAULT:
            print('已经就是默认值，不用改')
            return 0

        print('\n写入默认值 + COMMIT …')
        r = rpc(h, OP_WRITE, REG_GYRO, 0, DEFAULT)
        if not r or r[0] != 0:
            print('[!] 写 0x07 失败：%r' % (r,))
            return 1
        r = rpc(h, OP_COMMIT, 0xFF, 0, b'\x00')
        if not r or r[0] != 0:
            print('[!] COMMIT 失败：%r' % (r,))
            return 1
        time.sleep(0.3)

        after = diag.rpc_read(h, REG_GYRO, 0, 14)
        show('\n改之后', after)
        ok = bytes(after) == DEFAULT
        print('\n回读%s' % ('与默认值一致 ✓' if ok else '与默认值不一致 ✗'))

        st = diag.rpc_read(h, 0x7F, 0, 16)
        if st:
            print('STATUS flags=0x%02x（待落盘=%d 暂存中=%d）'
                  % (st[2], st[2] & 1, (st[2] >> 1) & 1))
        ts = diag.rpc_read(h, 0x12, 0, 64)
        if ts and len(ts) >= 30:
            print('TLM_STATE[29]=0x%02x → 生效中=%d 目标=%s（写入后应变成"不生效/鼠标"）'
                  % (ts[29], ts[29] & 1, TGT.get((ts[29] >> 1) & 3, '?')))
        return 0 if ok else 1
    finally:
        try:
            h.close()
        except Exception:
            pass


if __name__ == '__main__':
    sys.exit(main())
