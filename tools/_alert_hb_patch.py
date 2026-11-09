# -*- coding: utf-8 -*-
"""2026-10-15 10:5x：用户整体覆盖同步工程后，watch_alert.py 里昨晚加的「心跳看到手柄离线」判定被冲掉，重打一遍；
顺带把夜测结论和「工程整体同步会冲掉本地改动」写进记忆。"""
import io
import os

TOOLS = 'E:/BL616_Project/DongleRE/tools'
MEM = 'C:/Users/yiqi/.claude/projects/E--BL616-Project-DongleRE/memory'


def rd(p):
    return io.open(p, encoding='utf-8', newline='').read()


def wr(p, s):
    io.open(p, 'w', encoding='utf-8', newline='').write(s)


def rep(p, old, new):
    s = rd(p)
    eol = '\r\n' if '\r\n' in s else '\n'
    old, new = old.replace('\n', eol), new.replace('\n', eol)
    assert s.count(old) == 1, (p, old[:60], s.count(old))
    wr(p, s.replace(old, new))


def app(p, text):
    s = rd(p)
    eol = '\r\n' if '\r\n' in s else '\n'
    if not s.endswith(eol):
        s += eol
    wr(p, s + text.replace('\n', eol))


# 1) watch_alert.py：心跳里的「手柄=离线」也算事件（昨晚 20:41 加过、5 个用例测过，被覆盖）
rep(os.path.join(TOOLS, 'watch_alert.py'),
    "        if mb and '心跳' in line:\n"
    "            last_boot = mb.group(1)\n"
    "            if '手柄=在线' in line:\n"
    "                try:\n",
    "        if mb and '心跳' in line:\n"
    "            last_boot = mb.group(1)\n"
    "            # 「手柄 在线/离线」只在转换时打一行，监控开始时已经在线的手柄不会再出现「在线」行，所以心跳也要看\n"
    "            on = '手柄=在线' in line\n"
    "            if pad_online and not on and not incident_at:\n"
    "                incident_at = time.time()\n"
    "                print('!!! 事件 %s : 心跳看到手柄离线：%s' % (time.strftime('%H:%M:%S'), line.strip()))\n"
    "                sys.stdout.flush()\n"
    "            pad_online = on\n"
    "            if on:\n"
    "                try:\n")

# 2) 记忆：夜测最终结论
rep(os.path.join(MEM, 'bt-link-master-role.md'),
    '四个 110 min 监控窗口（20:41、22:31、00:22、02:25）加 02:12–02:25 的日志扫描：20:30–04:15 累计 0 断链',
    '七个 110 min 监控窗口（20:41 → 09:47）+ 02:12–02:25 日志扫描 + 10:50 设备运行时间核对（DIAG t=51.95M ms ≈ 14.4 h 无复位）：'
    '**20:30 → 10:50 共 14 h 20 min 累计 0 断链**。原始记录器日志 state_20261014_203011.log 已被 10:47 的工程整体覆盖冲成 20:33 的快照，'
    '各窗口汇总只剩 Temp 里的告警输出文件')

# 3) 记忆：R1.7 ~ R1.13（家里那台机器 2026-10-14 22:46 ~ 10-15 03:19 做的，细节在 HANDOFF.md「2. 当前状态」与 CHANGELOG.md）
app(os.path.join(MEM, 'fw-version-scheme-r.md'),
    '\n**R1.7 ~ R1.13（2026-10-14 22:46 ~ 10-15 03:19，用户在家里那台机器上做的，10-15 10:47 整体覆盖同步到公司）**：'
    'R1.7 产品名改 `BL616-DongleRE`（不再带 LCT616）、删「静默 LED」、同偏移换 `pad_sleep_min` 手柄省电关机（config v8）；'
    'R1.8 音频触觉默认 1；R1.9 删 7 个音频参数（结构体 23→13 B，**config v9**）；R1.10 省电活动判定剔除 IMU；'
    'R1.11 修 config_load 长度下限 19 没改（R1.9/R1.10 配置存不住）；R1.12 `last_activity_ms` 未来值自愈守卫（DIAG 42 AUTO_OFF / 43 ACT_FIX）；'
    'R1.13 守卫只在未来量 >2000 ms 才记（降噪）。**最新 = R1.13**（774848 B，包在 `webgui/firmware/`，manifest version `BL616-DongleRE R1.13`，'
    '云端 donglere.megu17.xyz 已发），家里在用的接收器是 R1.12，公司这台还是 **R1.6 sdk232（config v7）**——现在的网页（v9）会因版本不匹配拦住写入。'
    'CHANGELOG.md 的 R* 段是面向玩家的发布说明（出包脚本自动提取），技术细节只写 HANDOFF。\n')

# 4) 记忆：工程整体同步会冲掉本地改动
wr(os.path.join(MEM, 'project-sync-overwrites-local.md'),
   '---\n'
   'name: project-sync-overwrites-local\n'
   'description: 用户在家/公司两台机器之间整体覆盖同步 DongleRE 工程目录（保留 mtime），会冲掉公司这边 tools/ 的本地改动和 tools/logs 的记录器日志；手上有多台接收器\n'
   'metadata:\n'
   '  type: project\n'
   '---\n'
   '\n'
   '2026-10-15 10:47 用户回公司后把整个 `E:\\BL616_Project\\DongleRE` 用家里的副本整体覆盖（文件 mtime 保留，所以 `find -mmin` 看不出来）。'
   '后果：① `tools/logs/state_20261014_203011.log` 从一夜的完整记录变回 20:33 的快照（夜测原始数据没了，只剩 Temp 里各窗口的告警汇总）；'
   '② 昨晚打进 `tools/watch_alert.py` 的「心跳看到手柄离线」判定被冲掉（已用 `tools/_alert_hb_patch.py` 重打）；'
   '③ 告警 watch_alert 因为日志文件被换、45 s 没新行，把记录器杀掉重开了两次（10:47:22、10:48:23）。\n'
   '\n'
   '接收器不止一台：公司这台（R1.6 sdk232，20:30 起连着 DualSense 夜测）、家里在用的那台（R1.12）、家里另一台 R1.6 sdk232 已拔；'
   '同名设备在网页授权框里分不清，只能看配置页「固件版本」行。\n'
   '\n'
   '**Why:** 用户的工作方式是两边整目录搬运，不走 git；公司这边做的工具改动、抓的日志如果没进家里的副本就会丢。\n'
   '\n'
   '**How to apply:** 用户说「更新了整个工程文件」后，先 `ls -lt` 看 tools/、firmware/src、HANDOFF.md、CHANGELOG.md、webgui/firmware/manifest.json，'
   '再检查 tools/watch_*.py 的本地改动还在不在、正在跑的记录器/告警跟的是哪个日志文件；重要的日志分析结果要及时写进 HANDOFF 或记忆，别只留在 logs/。'
   '见 [[dongle-test-workflow]]、[[fw-version-scheme-r]]。\n')

# 5) MEMORY.md 索引
rep(os.path.join(MEM, 'MEMORY.md'),
    '— 2026-10-14 用户决定；设备上现在是 R1.6 sdk232（938b8122，20:23 已烧录验证）= 默认当主 + 震动收到就转发',
    '— 2026-10-14 用户决定；最新 R1.13（10-15 03:19 家里编的，包在 webgui/firmware，云端已发，config v9）；公司这台接收器还是 R1.6 sdk232（config v7）')
rep(os.path.join(MEM, 'MEMORY.md'),
    'R1.6 起默认当主（link_tune.py 0 临时关）',
    'R1.6 起默认当主（link_tune.py 0 临时关）；R1.6 夜测 10-14 20:30 → 10-15 10:50 共 14 h 20 min 零掉线')
app(os.path.join(MEM, 'MEMORY.md'),
    '- [工程整体覆盖同步会冲掉本地改动与日志；接收器不止一台](project-sync-overwrites-local.md) — 2026-10-15 10:47 家里副本整体覆盖公司目录：夜测日志变 20:33 快照、watch_alert 改动丢；用户说「更新了整个工程」先查这些\n')
print('patched: watch_alert.py + 4 memory files')
