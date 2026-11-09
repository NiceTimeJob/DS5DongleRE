# -*- coding: utf-8 -*-
"""R1.15 补丁的剩余部分（main.c 已在 _r15_patch.py 里改好；那次因为一个计数断言写错值而
在 version.h 之前中断）。这里只做 version.h / CHANGELOG / HANDOFF / 记忆，幂等。"""
import io

SRC = 'E:/BL616_Project/DongleRE/firmware/src/'
FW = 'E:/BL616_Project/DongleRE/firmware/'
MEM = 'C:/Users/yiqi/.claude/projects/E--BL616-Project-DongleRE/memory/'


def rd(p):
    return io.open(p, encoding='utf-8', newline='').read()


def wr(p, s):
    io.open(p, 'w', encoding='utf-8', newline='').write(s)


def rep(p, old, new, done_marker):
    s = rd(p)
    eol = '\r\n' if '\r\n' in s else '\n'
    if done_marker in s:
        print('skip %-16s (already applied)' % p.split('/')[-1])
        return
    old2, new2 = old.replace('\n', eol), new.replace('\n', eol)
    n = s.count(old2)
    assert n == 1, (p, 'want 1 got %d' % n, old[:60])
    wr(p, s.replace(old2, new2))
    print('ok   %-16s' % p.split('/')[-1])


rep(SRC + 'version.h',
    '#define FW_VERSION      "R1.14"',
    '#define FW_VERSION      "R1.15"',
    '"R1.15"')

rep(FW + 'CHANGELOG.md',
    '## R1.14 — 2026-10-15',
    '## R1.15 — 2026-10-15\n'
    '\n'
    '### Fixed\n'
    '- 「手柄省电模式」偶尔刚连上没多久就把手柄关机：之前省电计时在后台受到干扰时会被误判成「已经闲置很久」，导致开机才几分钟手柄就被自动关机。现在计时改为一次性读取、判定全程用同一个值，没到你设定的时间绝不会误关机\n'
    '\n'
    '---\n'
    '\n'
    '## R1.14 — 2026-10-15',
    '## R1.15 — 2026-10-15')

rep(FW + 'HANDOFF.md',
    '**最新固件：`R1.14`**（2026-10-15 公司机器编译。',
    '**最新固件：`R1.15`**（2026-10-15 公司编译。= R1.14 + **修「手柄省电模式」误关机**：R1.11/R1.12 那个 '
    '`last_activity_ms` 被野写成未来值导致「开机没多久就把手柄关机」的 bug，**R1.12 的守卫其实没真正拦住**——'
    '反汇编 R1.14 看到编出来的代码把 volatile `last_activity_ms` 读了三次（守卫读第 1 次 `a002ea4e`、判关机重读第 3 次 '
    '`a002ea90`），两读之间一旦被野写，守卫（只在未来值时跑、还跑在前面）就被绕过，直接发关机命令且不记 ACT_FIX。'
    'R1.14 实测物证（`log/DongleRE_日志_20261015_143050.txt`）：14:33:54 **t=61023 ms**（开机 61 秒）、'
    '14:46:50 **t=837619 ms**（开机 ~14 分钟）各误触发一次 AUTO_OFF a1=30，前面都没有 ACT_FIX = 守卫整轮没跑，'
    '30 分钟计时数学上根本不可能到期。R1.15 改为**一次性快照** `act_ms = last_activity_ms`，守卫与判关机共用同一快照'
    '（补丁 `tools/_r15_patch.py`）：未来值要么当场拉回、要么本轮根本看不到，开机不足 N 分钟绝不可能被判成闲置 N 分钟；'
    '野写来源仍未定位（时间上聚在网页工具 HID 连接期间，快照让它无法再造成误关机，物证照记 ACT_FIX）。'
    '**同一份日志 14:31–14:33 的三次 USB 消失是另一回事**：RST_REASON=BOD/真断电、GONE/BACK 5~6 s、RAM 丢失 = 供电跌落，'
    '与本修复无关，属 [[random-reset-incident-2026-10-12]] 那条线。**待烧录 + 实测**。）\n\n'
    '**R1.14**（2026-10-15 公司机器编译。',
    '**最新固件：`R1.15`**')

# 记忆
mp = MEM + 'fw-version-scheme-r.md'
s = rd(mp)
if 'R1.15' in s:
    print('skip fw-version-scheme-r.md (already applied)')
else:
    eol = '\r\n' if '\r\n' in s else '\n'
    note = ('\n**R1.15（2026-10-15 公司编译）**：修「手柄省电模式」误关机。R1.12 的未来值守卫被编译器对 volatile '
            '`last_activity_ms` 的三次重读绕过（守卫读第 1 次、判关机重读第 3 次），野写落在两读之间就直接误发关机、'
            '且不记 ACT_FIX；R1.14 实测开机 61 s / ~14 min 各误关机一次。改为一次性快照 `act_ms`，守卫与判关机共用。'
            '补丁 tools/_r15_patch.py。\n')
    if not s.endswith(eol):
        s += eol
    wr(mp, s + note.replace('\n', eol))
    print('ok   fw-version-scheme-r.md')

print('R1.15 rest done')
