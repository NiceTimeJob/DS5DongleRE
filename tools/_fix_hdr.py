# -*- coding: utf-8 -*-
"""一次性修复：R1 功能补丁用 rex() 写坏了 usb_gamepad.h 的一行声明（rex 的 new 是字面量，
反向引用 \\1 和 \\n 被原样写进了文件）。顺手给 _patchlib.rex 加断言防再犯。"""
import io
import os

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')

p = os.path.join(ROOT, 'firmware', 'src', 'usb_gamepad.h')
s = io.open(p, encoding='utf-8', newline='').read()
nl = '\r\n' if '\r\n' in s else '\n'
lines = s.split(nl)
bad = [i for i, l in enumerate(lines)
       if 'usb_gamepad_send_consumer_report(uint8_t bits)' in l and 'usb_gamepad_kbd_ready' in l]
assert len(bad) == 1, bad
i = bad[0]
lines[i:i + 1] = [
    'int  usb_gamepad_send_mouse_report(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel);',
    'int  usb_gamepad_send_consumer_report(uint8_t bits);   /* R1：媒体键位图（报告 ID 2） */',
    'bool usb_gamepad_kbd_ready(void);',
]
io.open(p, 'w', encoding='utf-8', newline='').write(nl.join(lines))
print('usb_gamepad.h fixed, lines %d..%d' % (i + 1, i + 3))

q = os.path.join(ROOT, 'tools', '_patchlib.py')
t = io.open(q, encoding='utf-8', newline='').read()
old = "    def rex(self, pat, new, cnt=1):\n        m = re.findall(pat, self.s, re.S)"
new = ("    def rex(self, pat, new, cnt=1):\n"
       "        # new 是字面量（lambda 替换，不做模板处理）：不能写反向引用 / 转义（如 \\1、\\n）\n"
       "        assert '\\\\1' not in new and '\\\\2' not in new, 'rex(): new 是字面量，不支持反向引用'\n"
       "        m = re.findall(pat, self.s, re.S)")
if old in t:
    t = t.replace(old, new)
    io.open(q, 'w', encoding='utf-8', newline='').write(t)
    print('_patchlib.rex guarded')
else:
    print('_patchlib.rex already guarded or anchor missing')
