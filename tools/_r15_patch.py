# -*- coding: utf-8 -*-
"""R1.15（2026-10-15）：修「手柄省电模式」误关机。
R1.12 的「未来值守卫」没真正拦住——反汇编 R1.14 看到 volatile last_activity_ms 被读了三次，
守卫读第 1 次、判关机重读第 3 次，两读之间野写一下守卫就被绕过，直接误发关机命令（且不记 ACT_FIX）。
R1.14 实测：14:33:54 t=61s、14:46:50 t=837s 各误触发一次 AUTO_OFF a1=30，前面都没有 ACT_FIX。
改为一次性快照 act_ms，守卫与判关机共用同一快照：开机不足 N 分钟绝不可能被判成闲置 N 分钟。
只改变量名那几行，LOG 里含 \\n 的那行不碰（免得反斜杠被工具吃掉）。"""
import io

SRC = 'E:/BL616_Project/DongleRE/firmware/src/'
FW = 'E:/BL616_Project/DongleRE/firmware/'
MEM = 'C:/Users/yiqi/.claude/projects/E--BL616-Project-DongleRE/memory/'


def rd(p):
    return io.open(p, encoding='utf-8', newline='').read()


def wr(p, s):
    io.open(p, 'w', encoding='utf-8', newline='').write(s)


def rep(p, old, new, cnt=1):
    s = rd(p)
    eol = '\r\n' if '\r\n' in s else '\n'
    old2, new2 = old.replace('\n', eol), new.replace('\n', eol)
    n = s.count(old2)
    assert n == cnt, (p, 'want %d got %d' % (cnt, n), old[:60])
    wr(p, s.replace(old2, new2))
    print('ok  %-14s x%d' % (p.split('/')[-1], cnt))


MC = SRC + 'main.c'

# A1：在守卫前插入一次性快照，首个比较换 act_ms
rep(MC,
    '                    if (last_activity_ms > now_ms) {',
    '                    uint32_t act_ms = last_activity_ms;   /* R1.15：一次性快照，守卫和判关机共用，别让编译器把 volatile 重读成两三次 */\n'
    '                    if (act_ms > now_ms) {')

# A2/A3/A4：守卫体内三处引用换 act_ms（LOG 的 \\n 行不在其中）
rep(MC,
    '                        if ((uint32_t)(last_activity_ms - now_ms) > 2000U) {',
    '                        if ((uint32_t)(act_ms - now_ms) > 2000U) {')
rep(MC,
    '                            diag_log(DIAG_EV_ACT_FIX, last_activity_ms, 0);',
    '                            diag_log(DIAG_EV_ACT_FIX, act_ms, 0);')
rep(MC,
    '                                    (unsigned long)last_activity_ms, (unsigned long)now_ms);',
    '                                    (unsigned long)act_ms, (unsigned long)now_ms);')

# A5：守卫末尾同步 act_ms，判关机比较换 act_ms（这段唯一：以 fire-if 行结尾）
rep(MC,
    '                        last_activity_ms = now_ms;\n'
    '                    }\n'
    '                    if ((uint32_t)(now_ms - last_activity_ms) >= (uint32_t)mins * 60000U) {',
    '                        last_activity_ms = now_ms;\n'
    '                        act_ms = now_ms;\n'
    '                    }\n'
    '                    if ((uint32_t)(now_ms - act_ms) >= (uint32_t)mins * 60000U) {')

# B：注释补 R1.15 来龙去脉
rep(MC,
    '                     * 野写，那种才记 ACT_FIX 留物证，免得日志页平白亮红吓人。 */',
    '                     * 野写，那种才记 ACT_FIX 留物证，免得日志页平白亮红吓人。\n'
    '                     * R1.15：R1.12 的守卫其实没真正拦住——编出来的代码把 last_activity_ms 重读了\n'
    '                     * 三次（守卫读第 1 次、判关机重读第 3 次），两读之间野写就绕过守卫、直接误发关机。\n'
    '                     * 2026-10-15 R1.14 实测 14:33:54(t=61s)、14:46:50(t=837s) 各误关机一次、前面都\n'
    '                     * 没有 ACT_FIX = 守卫整轮没跑。改成下面的一次性快照 act_ms 彻底消除：开机不足 N\n'
    '                     * 分钟绝不可能被判成闲置 N 分钟。野写来源仍未定位（聚在网页工具连着时）。 */')

# 安全检查：这段逻辑里 last_activity_ms 现在只剩「写」(= now_ms)，比较/传参全是 act_ms
import re
blk = rd(MC)
i = blk.index('uint8_t mins = config_get()->pad_sleep_min;')
j = blk.index('usb_wake_power_off_controller();', i)
seg = blk[i:j]
assert seg.count('act_ms') == 5, seg.count('act_ms')          # 1 decl + 2 guard-cmp + 1 ACT_FIX arg + 1 fire-cmp
assert 'now_ms - last_activity_ms' not in seg                  # 比较不再直接读 volatile
print('main.c guard/fire now use act_ms snapshot (act_ms x%d in block)' % seg.count('act_ms'))

# version.h
rep(SRC + 'version.h', '#define FW_VERSION      "R1.14"', '#define FW_VERSION      "R1.15"')

# CHANGELOG（面向玩家，### Fixed → 出包自动加「修复：」，条目别自带前缀）
rep(FW + 'CHANGELOG.md',
    '## R1.14 — 2026-10-15',
    '## R1.15 — 2026-10-15\n'
    '\n'
    '### Fixed\n'
    '- 「手柄省电模式」偶尔刚连上没多久就把手柄关机：之前省电计时在后台受到干扰时会被误判成「已经闲置很久」，导致开机才几分钟手柄就被自动关机。现在计时改为一次性读取、判定全程用同一个值，没到你设定的时间绝不会误关机\n'
    '\n'
    '---\n'
    '\n'
    '## R1.14 — 2026-10-15')

# HANDOFF：R1.15 置顶、R1.14 降为普通段
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
    '**R1.14**（2026-10-15 公司机器编译。')

# 记忆
m = MEM + 'fw-version-scheme-r.md'
s = rd(m)
if 'R1.15' not in s:
    eol = '\r\n' if '\r\n' in s else '\n'
    note = ('\n**R1.15（2026-10-15 公司编译）**：修「手柄省电模式」误关机。R1.12 的未来值守卫被编译器对 volatile '
            '`last_activity_ms` 的三次重读绕过（守卫读第 1 次、判关机重读第 3 次），野写落在两读之间就直接误发关机、'
            '且不记 ACT_FIX；R1.14 实测开机 61 s / 14 min 各误关机一次。改为一次性快照 `act_ms`，守卫与判关机共用。'
            '补丁 tools/_r15_patch.py。\n')
    if not s.endswith(eol):
        s += eol
    wr(m, s + note.replace('\n', eol))
    print('ok  fw-version-scheme-r.md (R1.15 note)')

print('R1.15 patch done')
