# -*- coding: utf-8 -*-
"""R2.0（2026-10-15，用户决定：这两天的内容算大更新，版本号从 R1.15 直接升 R2.0）。
代码改动只有一处遥测修正：日志页「每帧耗时」的陀螺仪分项以前恒为 0.0 µs / 0 次——
perf_gyro_cyc() 只包在 gyro_calibrating()（静置校准那 3 秒）里，平时每帧真正跑的
gyro_apply()（按住 L2 时陀螺仪叠加到右摇杆）没被计时，开销全算进「引擎」总数、拆不出来，
被误读成"陀螺仪不耗 CPU"（2026-10-15 用户按住 L2 看到右摇杆在动但陀螺仪栏仍 0.0）。
现在把 gyro_apply 也包进同一个计数器，并改掉那条过时注释。纯测量，不改任何行为。"""
import io

SRC = 'E:/BL616_Project/DongleRE/firmware/src/'
FW = 'E:/BL616_Project/DongleRE/firmware/'
MEM = 'C:/Users/yiqi/.claude/projects/E--BL616-Project-DongleRE/memory/'


def rd(p):
    return io.open(p, encoding='utf-8', newline='').read()


def wr(p, s):
    io.open(p, 'w', encoding='utf-8', newline='').write(s)


def rep(p, old, new, marker=None):
    s = rd(p)
    eol = '\r\n' if '\r\n' in s else '\n'
    if marker and marker in s:
        print('skip %-22s (already applied)' % p.split('/')[-1])
        return
    o, n = old.replace('\n', eol), new.replace('\n', eol)
    c = s.count(o)
    assert c == 1, (p, 'want 1 got %d' % c, old[:70])
    wr(p, s.replace(o, n))
    print('ok   %-22s' % p.split('/')[-1])


DM = SRC + 'ds5map.c'

# 1) 每帧 gyro_apply 包进陀螺仪计时
rep(DM,
    '    /* 陀螺仪在最后叠加（输出侧），鼠标增量经 g_gyro_dx/dy 交给 synth_mouse */\n'
    '    if (g_gyro_live)\n'
    '        gyro_apply(p);\n',
    '    /* 陀螺仪在最后叠加（输出侧），鼠标增量经 g_gyro_dx/dy 交给 synth_mouse。\n'
    '     * R2.0：这里才是平时每帧真正的陀螺仪开销，计进遥测的「陀螺仪」分项（它包含在「引擎」\n'
    '     * 总耗时里）。以前只有静置校准那 3 秒计时，日志页那栏平时恒为 0.0 µs / 0 次，\n'
    '     * 被误读成"陀螺仪不耗 CPU"。 */\n'
    '    if (g_gyro_live) {\n'
    '        uint32_t gc0 = perf_cyc();\n'
    '        gyro_apply(p);\n'
    '        perf_gyro_cyc(perf_cyc() - gc0);\n'
    '    }\n',
    marker='R2.0：这里才是平时每帧真正的陀螺仪开销')

# 2) 过时注释
rep(DM,
    '    /* R1.3：自动零偏校准已删除，平时陀螺仪没有任何每帧计算。只有静置校准的那 3 秒\n'
    '     * 每帧累加一次；',
    '    /* R1.3：自动零偏校准已删除。平时每帧的陀螺仪计算只有输出侧的 gyro_apply（按住\n'
    '     * 启用键时叠加到摇杆/鼠标，R2.0 起在那边单独计时）；这里只管静置校准的那 3 秒\n'
    '     * 每帧累加一次；',
    marker='R2.0 起在那边单独计时')

# 3) 版本号
rep(SRC + 'version.h', '#define FW_VERSION      "R1.15"', '#define FW_VERSION      "R2.0"', marker='"R2.0"')
rep(SRC + 'version.h',
    ' *   R1.1、R1.2 …  小版本（小数点递增）\n',
    ' *   R1.1、R1.2 …  小版本（小数点递增）\n'
    ' *   R2.0（2026-10-15）：用户把 10-14/10-15 两天的更新（R1 ~ R1.15）定为一次大更新，\n'
    ' *   大版本直接升到 2，之后小版本 R2.1、R2.2 …\n',
    marker='R2.0（2026-10-15）')

# 4) CHANGELOG（面向玩家；### 小节由出包脚本加标签，条目别自带「修复/新增」前缀）
rep(FW + 'CHANGELOG.md',
    '## R1.15 — 2026-10-15',
    '## R2.0 — 2026-10-15\n'
    '\n'
    '### Changed\n'
    '- 版本号升到 R2.0：把 10 月 14、15 两天的全部更新（全新按键映射引擎、接收器主动管理无线链路、手柄省电模式、「禁用 LED」真全灭、音频精简、刷写页「更新内容」等）收拢为一个大版本。功能与 R1.15 相同，再加下面这一条\n'
    '\n'
    '### Fixed\n'
    '- 日志页「每帧耗时」里的陀螺仪一栏一直显示 0：之前只统计了静置校准那几秒，平时按住启用键用陀螺仪瞄准时的真实开销没被算进去。现在能看到陀螺仪每帧实际用了多少微秒、生效了多少帧\n'
    '\n'
    '---\n'
    '\n'
    '## R1.15 — 2026-10-15',
    marker='## R2.0 — 2026-10-15')

# 5) HANDOFF：R2.0 置顶，R1.15 降为普通段
rep(FW + 'HANDOFF.md',
    '**最新固件：`R1.15`**（2026-10-15 公司编译。',
    '**最新固件：`R2.0`**（2026-10-15 公司编译；**用户决定：这两天的内容算大更新，版本号从 R1.15 直接升 R2.0**，'
    '之后小版本 R2.1、R2.2…）。= R1.15 + **陀螺仪遥测修正（纯测量）**：日志页「每帧耗时」的陀螺仪分项以前恒为 '
    '0.0 µs / 0 次——`perf_gyro_cyc()` 只包在 `ds5map.c` 的 `if (gyro_calibrating())`（静置校准那 3 秒）里，'
    '平时每帧真正跑的 `gyro_apply()`（`g_gyro_live`，按住启用键时叠加到右摇杆/鼠标）没被计时，开销全算进「引擎」'
    '总数拆不出来；2026-10-15 用户按住 L2 看到右摇杆在动但陀螺仪栏仍 0.0，由此发现。现在 `gyro_apply` 也包进同一个'
    '计数器（陀螺仪分项 ⊂ 引擎总耗时，main.c 的 c0..c1 区间覆盖整个映射处理），`gyro_win` 变成「本秒陀螺仪生效的帧数」。'
    '顺带改掉那条过时注释（"平时陀螺仪没有任何每帧计算"是 R1.3 的旧设计）。**R1.15 下的性能基线**（公司这台，'
    '听歌 + 震动，陀螺仪栏当时量不到）：空闲地板 19.1%、btble 29% / audio 21% / usb 17%，引擎 ~200 µs/帧'
    '（无音乐时 157 µs，涨幅是音频高负载下的缓存争用），最长单帧偶发 1.9 ms（周期 1.43 ms，被缓冲接住），'
    '整会话丢块 / 发送失败 / 排队丢全 0。**R2.0 待烧录 + 复测三件事叠满（音乐 + 震动 + 按住 L2 陀螺仪）**，'
    '那时陀螺仪分项才第一次有真数。补丁 `tools/_r20_patch.py`。网页无改动（page-log.js 原本就显示 gyro_dus/gyro_win）。）\n\n'
    '**R1.15**（2026-10-15 公司编译。',
    marker='**最新固件：`R2.0`**')

# 6) 记忆
mp = MEM + 'fw-version-scheme-r.md'
s = rd(mp)
if 'R2.0' in s:
    print('skip fw-version-scheme-r.md (already applied)')
else:
    eol = '\r\n' if '\r\n' in s else '\n'
    note = ('\n**R2.0（2026-10-15，用户决定）**：「这 2 天的内容算大更新了」→ 版本号从 R1.15 直接升 **R2.0**，'
            '之后小版本 R2.1、R2.2…。内容 = R1.15 + 陀螺仪遥测修正（`ds5map.c` 的每帧 `gyro_apply` 以前没计时，'
            '日志页陀螺仪栏恒 0.0/0 次是假读数；现在包进 `perf_gyro_cyc`，陀螺仪 ⊂ 引擎）。补丁 tools/_r20_patch.py。\n')
    if not s.endswith(eol):
        s += eol
    wr(mp, s + note.replace('\n', eol))
    print('ok   fw-version-scheme-r.md')

print('R2.0 patch done')
