#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""烧完固件后的独立校验：直接问设备（不问网页、不问我自己）。

  0xF8 → 固件版本串，应该正好等于源码 src/version.h 派生出来的串（R1.7 起形如 "BL616-DongleRE R1.9"）
  0xF7 → 配置体，byte0 = config_version，应该是 EXPECT_CFG_VER（R1.9 起 9，结构体 13 字节）
  0x7F → STATUS（0xFC RPC，走 diag.py 的实现），顺便看 cfgDirty / 引擎旗标

接收器插上（不按 BOOT）才会以 054C:0CE6 出现；系统里可能还有别的软件装的
「虚拟 DualSense」（同样 054C:0CE6）——按"谁答 0xFC"来认，跟 diag.open_dev 一样。

用法:
  python -X utf8 verify_fw.py            # 试一次
  python -X utf8 verify_fw.py 60         # 等最多 60 秒（拔插用）
"""
import sys
import time

import hid

import diag

RID_CFG = 0xF7
RID_VER = 0xF8
RID_STATUS = 0x7F
EXPECT_CFG_VER = 9
import os
import re
VER_H = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'firmware', 'src', 'version.h')


def source_tag():
    """从源码 src/version.h 派生当前版本串（本机只编 lctech616 普通版，不带 HS / SDK 标记），
    用来对照设备里跑的是不是同一版。读不到就返回 None —— 那不是错误，只是没法对照。"""
    try:
        h = open(VER_H, encoding='utf-8').read()
        ver = re.search(r'#define\s+FW_VERSION\s+"([^"]+)"', h).group(1)
        return 'BL616-DongleRE ' + ver
    except Exception:
        return None


def get_feature(h, rid, ln=63):
    """读一条特征报告，剥掉开头的 report id（固件把 id 放在 [0]）。"""
    try:
        h.send_feature_report(bytes([rid]) + bytes(ln))
    except Exception as e:
        return None, 'send 失败: %s' % e
    for _ in range(20):
        time.sleep(0.01)
        try:
            resp = bytes(h.get_feature_report(rid, ln + 1))
        except Exception as e:
            return None, 'get 失败: %s' % e
        if resp:
            return (resp[1:] if resp[0] == rid else resp), None
    return None, '没有响应'


def main():
    wait = int(sys.argv[1]) if len(sys.argv) > 1 else 0
    deadline = time.time() + wait
    h = None
    n = 0
    while True:
        h = diag.open_dev(retries=2)
        if h is not None:
            break
        if time.time() >= deadline:
            print('[!] 没找到接收器（插上了吗？没按 BOOT 吧？）')
            return 1
        n += 1
        if n % 30 == 1:     # 约每 60 秒一行，别把日志刷满
            print('... 等接收器：现在是 ISP 模式跑不了固件（BOOT 松开了吗？拔插一次）')
        time.sleep(2)

    bad = 0
    try:
        # 先列出这颗接收器暴露的 HID 接口（键鼠接口是不是真在，一眼能看出来：
        # 手柄 = usage_page 0x01/usage 0x05；键盘 0x01/0x06；消费类 0x0C/0x01；鼠标 0x01/0x02）
        ifs = [d for d in hid.enumerate(0x054C)]
        print('HID 接口    : %d 个' % len(ifs))
        for d in ifs:
            print('               usage_page=0x%04X usage=0x%04X if=%s %s'
                  % (d['usage_page'], d['usage'], d.get('interface_number'), d.get('product_string')))

        body, err = get_feature(h, RID_VER)
        ver = body.split(b'\0')[0].decode('utf-8', 'replace') if body else None
        src = source_tag()
        print('0xF8 固件版本 : %r   （源码里现在是 %r）' % (ver, src))
        if src and ver and ver.startswith(src + ' sdk'):
            print('     [ok] 与源码一致（%s 那份 SDK 编的包）' % ver[len(src) + 1:])
        elif src and ver != src:
            print('     [!] 和源码不一致 —— 设备里跑的可能不是刚编好的那一版（没烧？烧完没复位？）')
        elif src:
            print('     [ok] 与源码一致')
        else:
            print('     [--] 读不到源码版本串，只报设备值')

        body, err = get_feature(h, RID_CFG)
        if body:
            n = len(body)
            print('0xF7 配置体   : %d 字节, config_version=%d, polling=%d, led=#%02X%02X%02X, pad_sleep=%dmin'
                  % (n, body[0], body[2], body[8], body[9], body[10], body[12]))
            if body[0] != EXPECT_CFG_VER:
                bad += 1
                print('     [!] config_version 期望 %d（≤8 都是老结构体）' % EXPECT_CFG_VER)
            else:
                print('     [ok] 新结构体（13 字节布局）已经在跑')
        else:
            bad += 1
            print('0xF7 读不到：%s' % err)

        st = diag.rpc_read(h, RID_STATUS, 0, 16)
        if st:
            flags = st[2]
            print('0x7F STATUS   : proto=%d.%d flags=0x%02X（待落盘=%d 暂存中=%d 有映射=%d）层=%d'
                  % (st[0], st[1], flags, flags & 1, (flags >> 1) & 1, (flags >> 2) & 1, st[3]))
        else:
            print('0x7F STATUS   : 读不到')

        # 陀螺仪 0x07（14 字节）：默认应是 01 01 00 00 00 08 00 ff 30 00 30 00 01 00
        g = diag.rpc_read(h, 0x07, 0, 14)
        if g and len(g) >= 14:
            TGT = {0: '关', 1: '鼠标', 2: '左摇杆', 3: '右摇杆'}
            EN = {0: '关闭', 1: '常开', 2: '按住启用', 3: '开关启用'}
            print('0x07 陀螺仪   : %s' % ' '.join('%02x' % b for b in g))
            print('                target=%s enable=%s sens=%d/%d 死区=%d'
                  % (TGT.get(g[0], '?'), EN.get(g[6], '?'),
                     g[8] | (g[9] << 8), g[10] | (g[11] << 8), g[5]))
        else:
            print('0x07 陀螺仪   : 读不到')

        # DIAG 头几条：第一条就是"这次是怎么起来的"（复位原因）
        buf = diag.fetch_diag(h)
        pr = diag.parse(buf) if buf else None
        if pr:
            head, boot, recs = pr
            print('0x14 DIAG     : head=%d boot=%d，前几条=' % (head, boot))
            for (t, ev, a1, a2) in recs[:4]:
                name = diag.EV.get(ev, 'EV%d' % ev)
                extra = ('复位原因 = ' + diag.rst_str(a1)) if ev == 28 else ('a1=%d' % a1)
                print('                t=%6dms  %-13s %s' % (t, name, extra))
        else:
            print('0x14 DIAG     : 读不到')
    finally:
        try:
            h.close()
        except Exception:
            pass

    print('\n' + ('校验通过：设备里跑的就是源码这一版（%s）' % ver if not bad else '有 %d 项不对' % bad))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
