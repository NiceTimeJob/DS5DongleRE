#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""HID 报告描述符体检（2026-10-16）

盯着 `firmware/src/usb_gamepad.c` 里那两个**定长数组**描述符，专治 S33/S34/S35 那类
"描述符和报告对不上 → Windows 给接口报 Code 10（无法启动）"的毛病：

  ① 初始化列表的字节数 == `HID_REPORT_DESC_SIZE_DS` / `KBD_REPORT_DESC_SIZE`
     （少了 → 尾部静默补 0x00，描述符就烂了；C 编译器不报错，S35 就是这么踩的）
  ② 整条描述符能一条条解析到底、不多不少正好用完，最后一项是 End Collection
  ③ 每个 Report ID 的输入位数（= 固件实际发的报告长度）
  ④ Consumer（报告 ID 2）的 16 个 usage 逐个对上（位号 = 配置里存的 val，
     写错 = 按下去发的是别的键；>0xFF 的 usage 必须用 0x0A 前缀，别写成 0x09）

    python tools/test_hid_desc.py
"""
import io
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, '..', 'firmware', 'src', 'usb_gamepad.c')

# 期望：报告 ID → (输入位数, 说明)
EXPECT_REPORTS = {1: (64, '键盘：修饰键 8 + 保留 8 + 键码 6*8'),
                  2: (16, '媒体/多功能：16 位位图（R2.2）'),
                  3: (32, '鼠标：按键 8 + X + Y + 滚轮')}
# 期望：报告 ID 2 的 usage（按下标 = 位号）
EXPECT_CONSUMER = [0xE9, 0xEA, 0xE2, 0xCD, 0xB5, 0xB6, 0xB7, 0xB3, 0xB4,
                   0x183, 0x192, 0x221, 0x223, 0x224, 0x225, 0x227]

fails = []


def check(name, cond, extra=''):
    print(('  ok   ' if cond else '  FAIL ') + name + ('  ' + str(extra) if extra else ''))
    if not cond:
        fails.append(name)


def grab(src, name):
    """从 C 源码里抠出一个数组的初始化字节，返回 (字节列表, 宏声明的长度或 None)"""
    m = re.search(r'static const uint8_t\s+' + name + r'\s*\[(\w+)\]\s*=\s*\{(.*?)\n\};',
                  src, re.S)
    if not m:
        raise SystemExit('源码里找不到数组 ' + name)
    macro = m.group(1)
    body = re.sub(r'/\*.*?\*/', '', m.group(2), flags=re.S)
    body = re.sub(r'//[^\n]*', '', body)
    vals = [int(v, 16) for v in re.findall(r'0x([0-9A-Fa-f]{2})', body)]
    mm = re.search(r'#define\s+' + macro + r'\s+(\d+)', src)
    return vals, (int(mm.group(1)) if mm else None), macro


def parse(data):
    """解析 HID 描述符；返回 (每个报告ID的输入位数, 每个报告ID的 usage 列表, 结束位置)

    usage 的归属按 HID 语义来：一个"报告字段" = 一串 usage + Report Size/Count + Input，
    所以 usage 先攒在 pending 里，遇到 Input 才归给当前 Report ID（这才对得上
    "位号 = 配置里的 val"）。集合表头的 usage（如 Consumer 集合的 Usage(0x01)、
    鼠标集合的 Usage(0x02)）后面跟的是集合开始 / 下一个 Report ID，会被丢掉，
    不会算进前一个报告里。"""
    i, rid, size, cnt = 0, None, 8, 1
    bits, usages, pending, ended = {}, {}, [], None
    while i < len(data):
        b = data[i]
        if b in (0xFE, 0xFF):                     # long item：跳过
            i += 2 + data[i + 1]
            continue
        tag, typ, sz = b & 0xF0, (b >> 2) & 3, b & 3
        ln = 4 if sz == 3 else sz
        if i + 1 + ln > len(data):
            raise ValueError('第 %d 字节处数据不够（描述符被截断了？）' % i)
        val = int.from_bytes(bytes(data[i + 1:i + 1 + ln]), 'little') if ln else 0
        if tag == 0x80 and typ == 1 and sz == 1:
            rid = val                              # Report ID
            pending = []                           # 表头 usage 到此为止
        elif tag == 0x70 and typ == 1:
            size = val                             # Report Size
        elif tag == 0x90 and typ == 1:
            cnt = val                              # Report Count
        elif tag == 0x80 and typ == 0:             # Input：一个字段结束
            bits[rid] = bits.get(rid, 0) + size * cnt
            if pending:
                usages.setdefault(rid, []).extend(pending)
                pending = []
        elif tag == 0x00 and typ == 2 and sz != 0:  # Usage（局部）
            pending.append(val)
        elif tag == 0xC0 and typ == 0:             # End Collection
            pending = []
            if i + 1 == len(data):
                ended = i + 1
        i += 1 + ln
    return bits, usages, ended


def main():
    src = io.open(SRC, encoding='utf-8').read()
    print('== HID 报告描述符体检  %s ==' % os.path.normpath(SRC))

    for name in ('kbd_report_desc', 'hid_report_desc_ds'):
        data, declared, macro = grab(src, name)
        check('%s 字节数 == %s' % (name, macro),
              declared is not None and len(data) == declared,
              '列表 %d / 宏 %s' % (len(data), declared))
        try:
            bits, usages, ended = parse(data)
        except ValueError as e:
            check('%s 能解析' % name, False, str(e))
            continue
        check('%s 不多不少正好解析完（末项是 End Collection）' % name,
              ended == len(data), '结束于 %s / 共 %d 字节' % (ended, len(data)))
        if name == 'kbd_report_desc':
            for rid, (want, why) in EXPECT_REPORTS.items():
                got = bits.get(rid)
                check('报告 %d 输入 %d 位（%s）' % (rid, want, why),
                      got == want, '实际 %s 位' % got)
            cons = usages.get(2, [])
            check('报告 2 的 usage 表 = 固件 MEDIA_NAMES 的顺序',
                  cons == EXPECT_CONSUMER,
                  '\n'.join('    位 %2d: 期望 0x%03X 实际 %s' % (i, w, hex(g) if g is not None else '缺')
                            for i, (w, g) in enumerate(zip(EXPECT_CONSUMER, cons + [None] * 16))
                            if w != g) or '')
            # 位号表也照一眼网页：两边对不上 = 下拉里选「快进」发出去是别的键
            pm = io.open(os.path.join(HERE, '..', 'webgui', 'js', 'page-map.js'),
                         encoding='utf-8').read()
            m = re.search(r'const MEDIA_NAMES = \[(.*?)\];', pm, re.S)
            names = re.findall(r"'([^']*)'", m.group(1)) if m else []
            check('网页 MEDIA_NAMES 项数 == 描述符里的 usage 数',
                  len(names) == len(EXPECT_CONSUMER),
                  '%d / %d' % (len(names), len(EXPECT_CONSUMER)))

    print('\n' + ('全部通过' if not fails else '有失败项：' + '、'.join(fails)))
    return 0 if not fails else 2


if __name__ == '__main__':
    sys.exit(main())
