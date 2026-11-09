# -*- coding: utf-8 -*-
"""断连回中实测（R2.4）：盯着电脑这一侧，看手柄断开后是不是「全部回中」。

同时记两样东西，只在变化时打印一行：
  ① 接收机手柄接口（054C:0CE6 if=3）发给电脑的输入报文：摇杆、扳机、按键、触点；
  ② Windows 眼里按着的键盘键和鼠标键（GetAsyncKeyState，所有键盘/鼠标合起来看）。
结束时给结论：手柄报文是不是回中帧、有没有还按着的键。

用法：python -X utf8 tools/disc_watch.py [秒数，默认 60]
测试时别碰真键盘、真鼠标（会被算进去）。手柄上按住键 / 扳机 / 映射出来的键盘键鼠标键，
然后直接关手柄（长按 PS 约 10 秒）或拿远到断开，等几秒再看结论。
"""
import ctypes
import sys
import time

import hid

SECS = float(sys.argv[1]) if len(sys.argv) > 1 else 60.0
user32 = ctypes.WinDLL('user32')
user32.GetAsyncKeyState.restype = ctypes.c_short

VK_NAME = {0x01: '鼠标左键', 0x02: '鼠标右键', 0x04: '鼠标中键', 0x05: '鼠标侧键1', 0x06: '鼠标侧键2',
           0x08: 'Backspace', 0x09: 'Tab', 0x0D: 'Enter', 0x10: 'Shift', 0x11: 'Ctrl', 0x12: 'Alt',
           0x14: 'CapsLock', 0x1B: 'Esc', 0x20: '空格', 0x25: '←', 0x26: '↑', 0x27: '→', 0x28: '↓',
           0x5B: '左Win', 0x5C: '右Win', 0xA0: '左Shift', 0xA1: '右Shift', 0xA2: '左Ctrl',
           0xA3: '右Ctrl', 0xA4: '左Alt', 0xA5: '右Alt', 0xAD: '静音', 0xAE: '音量-', 0xAF: '音量+',
           0xB0: '下一曲', 0xB1: '上一曲', 0xB2: '停止', 0xB3: '播放/暂停'}
# 通用的 Shift/Ctrl/Alt（0x10~0x12）和左右分开的那几个同时会亮，只看左右分开的
SKIP = {0x10, 0x11, 0x12}


def vk_name(vk):
    if vk in VK_NAME:
        return VK_NAME[vk]
    if 0x30 <= vk <= 0x39 or 0x41 <= vk <= 0x5A:
        return chr(vk)
    if 0x70 <= vk <= 0x87:
        return 'F%d' % (vk - 0x6F)
    return 'VK_%02X' % vk


def keys_down():
    return tuple(vk for vk in range(1, 0xFF)
                 if vk not in SKIP and (user32.GetAsyncKeyState(vk) & 0x8000))


def pad_state(p):
    return (p[0], p[1], p[2], p[3], p[4], p[5], p[7], p[8], p[9], p[32], p[36])


def pad_text(s):
    lx, ly, rx, ry, l2, r2, b7, b8, b9, t0, t1 = s
    hat = b7 & 0x0F
    hat_s = '不按' if hat == 8 else '↑↗→↘↓↙←↖'[hat] if hat < 8 else '?%d' % hat
    touch = []
    for i, t in enumerate((t0, t1)):
        touch.append('抬起' if t & 0x80 else '按着')
    return ('L(%3d,%3d) R(%3d,%3d) L2=%3d R2=%3d 十字=%s 键=%02X/%02X/%02X 触点=%s/%s'
            % (lx, ly, rx, ry, l2, r2, hat_s, b7 & 0xF0, b8, b9, touch[0], touch[1]))


def pad_problems(s):
    lx, ly, rx, ry, l2, r2, b7, b8, b9, t0, t1 = s
    bad = []
    for name, v in (('LX', lx), ('LY', ly), ('RX', rx), ('RY', ry)):
        if abs(v - 0x80) > 2:
            bad.append('%s=%d（没回中）' % (name, v))
    if l2 or r2:
        bad.append('扳机 L2=%d R2=%d' % (l2, r2))
    if (b7 & 0x0F) != 8:
        bad.append('十字键还按着（hat=%d）' % (b7 & 0x0F))
    if b7 & 0xF0 or b8 or b9:
        bad.append('按键位 %02X/%02X/%02X' % (b7 & 0xF0, b8, b9))
    if not (t0 & 0x80) or not (t1 & 0x80):
        bad.append('触摸板触点没抬起')
    return bad


def main():
    path = None
    for d in hid.enumerate(0x054C, 0x0CE6):
        if d.get('interface_number') == 3 and d.get('usage_page') == 1 and d.get('usage') == 5:
            path = d['path']
    h = None
    if path:
        h = hid.device()
        h.open_path(path)
        h.set_nonblocking(1)
    else:
        print('找不到接收机的手柄接口（054C:0CE6 if=3），只看键盘鼠标')

    print('开始盯 %.0f 秒（Ctrl+C 提前结束）' % SECS)
    t0 = time.time()
    last_pad = last_keys = None
    last_rpt_t = None
    n_rpt = 0
    try:
        while time.time() - t0 < SECS:
            now = time.time() - t0
            if h:
                while True:
                    r = h.read(128)
                    if not r:
                        break
                    if r[0] != 0x01 or len(r) < 38:
                        continue
                    n_rpt += 1
                    last_rpt_t = now
                    s = pad_state(bytes(r[1:]))
                    if s != last_pad:
                        print('%7.2fs 手柄报文 %s' % (now, pad_text(s)))
                        last_pad = s
            k = keys_down()
            if k != last_keys:
                print('%7.2fs 按着的键：%s' % (now, '、'.join(vk_name(v) for v in k) or '（无）'))
                last_keys = k
            time.sleep(0.005)
    except KeyboardInterrupt:
        pass

    print()
    print('==== 结论（共收到手柄报文 %d 条，最后一条在 %.2fs）====' % (
        n_rpt, last_rpt_t if last_rpt_t is not None else -1))
    ok = True
    if last_pad is not None:
        bad = pad_problems(last_pad)
        if bad:
            ok = False
            print('手柄报文最后停在：' + pad_text(last_pad))
            print('  没回中：' + '；'.join(bad))
        else:
            print('手柄报文最后是回中帧 ✓')
    k = keys_down()
    if k:
        ok = False
        print('Windows 里还按着：' + '、'.join(vk_name(v) for v in k))
    else:
        print('键盘鼠标没有按着的键 ✓')
    print('全部回中 ✓' if ok else '有东西没松开 ✗')
    if h:
        h.close()


if __name__ == '__main__':
    main()
