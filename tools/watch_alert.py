#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""跟着 watch_state.py 的最新日志看，出事就退出（给后台监控用）。

用法：python -X utf8 tools/watch_alert.py [分钟数，默认 110]
  - 出现断链 / 复位 / USB 消失 / 看门狗 / 帧率掉 0 等事件：再等 20 s 收尾（复位后的
    BACK、上一轮 DIAG 快照都在这 20 s 里），打印事件前后的日志并退出。
  - 到时间都没事：打印这段时间的统计（帧率、电量、音频发送失败、最后一行性能）并退出。
  - 记录器 45 s 没写一行（它每 10 s 至少一行心跳）= 卡死了（2026-10-14 13:03 实际发生过：
    hidapi 的特征报告读写在 Windows 上没有超时，偶尔一卡到底）→ 杀掉重开，接着看新日志。
记录器重启会换日志文件，这里自动跟到最新的那个。
"""
import glob
import os
import re
import subprocess
import sys
import time

TOOLS = os.path.dirname(os.path.abspath(__file__))
LOGDIR = os.path.join(TOOLS, 'logs')
MINUTES = float(sys.argv[1]) if len(sys.argv) > 1 else 110.0
STALL_S = 45
PY = sys.executable

INCIDENT = (
    '### GONE', 'WD_CONN', 'FORCE_DISC', 'ACL_DOWN', 'DISC_REQ', 'RST_REASON',
    'CRASH 区有效', 'SCAN_STUCK', 'HB_SCAN_FIX', '!!! 启动计数',
    'L2CAP_CTRL_DOWN', 'L2CAP_INTR_DOWN', 'CONNECTED ->', '!!! 接收器无应答',
)
# 这些行后面跟着的是（重新）连上时的启动快照（整个 DIAG 环），不是刚发生的事
DUMP_START = ('=== watch_state 启动', '=== 记录器继续', '=== 网页日志', '### OPEN')


def newest():
    """后台记录器（state_*.log）和网页日志页（web_*.log，serve.py 写）谁最新跟谁。"""
    files = (glob.glob(os.path.join(LOGDIR, 'state_*.log')) +
             glob.glob(os.path.join(LOGDIR, 'web_*.log')))
    return max(files, key=os.path.getmtime) if files else None


def restart_recorder():
    kill = ("Get-CimInstance Win32_Process -Filter \"Name='python.exe'\" | "
            "Where-Object { $_.CommandLine -match 'watch_state' } | "
            "ForEach-Object { Stop-Process -Id $_.ProcessId -Force }")
    subprocess.run(['powershell', '-NoProfile', '-Command', kill],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1.0)
    start = ("Start-Process -FilePath '%s' -ArgumentList '-X','utf8','%s' "
             "-WorkingDirectory '%s' -WindowStyle Minimized"
             % (PY, os.path.join(TOOLS, 'watch_state.py').replace('\\', '/'), TOOLS.replace('\\', '/')))
    subprocess.run(['powershell', '-NoProfile', '-Command', start],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def main():
    path = newest()
    if not path:
        print('没有记录器日志')
        return 2
    f = open(path, encoding='utf-8', errors='replace')
    f.seek(0, os.SEEK_END)
    offsets = {}         # 每个日志文件读到哪了（网页日志和记录器会轮流成为「最新」）
    start = time.time()
    deadline = start + MINUTES * 60
    last_line_t = time.time()
    restarts = 0
    tail = []
    rates = []
    batt = []
    audio_lines = 0
    stalls = []          # 输入停顿（不算事故，单独记）
    in_dump = False      # 记录器刚启动时把 DIAG 环整个打一遍，那段不是实时事件
    pad_online = None    # 上一次看到的手柄在线状态（只有 在线→离线 才算事故）
    low_rate = 0         # 手柄在线但心跳帧率 < 300 的次数
    last_boot = None     # 心跳里的启动计数
    last_perf = ''
    print('监控开始 %s，日志 %s，最长 %.0f 分钟' % (time.strftime('%H:%M:%S'), os.path.basename(path), MINUTES))
    sys.stdout.flush()
    incident_at = None
    while True:
        line = f.readline()
        if not line:
            now = time.time()
            if incident_at and now - incident_at > 20:
                break
            if not incident_at and now > deadline:
                break
            nw = newest()
            if nw and nw != path:
                offsets[path] = f.tell()
                path = nw
                f = open(path, encoding='utf-8', errors='replace')
                if path in offsets:
                    f.seek(offsets[path])
                elif os.path.getctime(path) < start:
                    f.seek(0, os.SEEK_END)       # 监控开始前就有的文件：只看之后新写的
                tail.append('--- 换到日志文件: %s ---' % os.path.basename(path))
                last_line_t = now
            elif now - last_line_t > STALL_S:
                restarts += 1
                msg = '--- %s 记录器 %d s 没动静，判定卡死，重启（第 %d 次）---' % (
                    time.strftime('%H:%M:%S'), STALL_S, restarts)
                tail.append(msg)
                print(msg)
                sys.stdout.flush()
                restart_recorder()
                last_line_t = time.time()
            time.sleep(1.0)
            continue
        last_line_t = time.time()
        line = line.rstrip('\n')
        tail.append(line)
        if len(tail) > 80:
            tail.pop(0)
        if '心跳' in line and '帧率=' in line:
            try:
                rates.append(int(line.split('帧率=')[1].split('/s')[0]))
            except ValueError:
                pass
            if '手柄=在线 ' in line:
                try:
                    batt.append(line.split('手柄=在线 ')[1].split('%')[0])
                except IndexError:
                    pass
        elif '性能 ' in line:
            last_perf = line
        mb = re.search(r'boot=(\d+)', line)
        if any(k in line for k in DUMP_START):
            in_dump = True
        if in_dump and ('心跳' in line or '输入帧率' in line or 'DIAG t=' in line):
            in_dump = False
        if in_dump:
            if 'DIAG head=' in line and mb and last_boot is not None and mb.group(1) != last_boot:
                if not incident_at:
                    incident_at = time.time()
                    print('!!! 事件 %s : 启动计数 %s -> %s，接收器重启过' % (
                        time.strftime('%H:%M:%S'), last_boot, mb.group(1)))
                    sys.stdout.flush()
            if mb:
                last_boot = mb.group(1)
            continue
        if mb and '心跳' in line:
            last_boot = mb.group(1)
            # 「手柄 在线/离线」只在转换时打一行，监控开始时已经在线的手柄不会再出现「在线」行，所以心跳也要看
            on = '手柄=在线' in line
            if pad_online and not on and not incident_at:
                incident_at = time.time()
                print('!!! 事件 %s : 心跳看到手柄离线：%s' % (time.strftime('%H:%M:%S'), line.strip()))
                sys.stdout.flush()
            pad_online = on
            if on:
                try:
                    if int(line.split('帧率=')[1].split('/s')[0]) < 300:
                        low_rate += 1
                except (ValueError, IndexError):
                    pass
        if '手柄 在线' in line:
            pad_online = True
        elif '手柄 离线' in line:
            if pad_online and not incident_at:
                incident_at = time.time()
                print('!!! 事件 %s : %s' % (time.strftime('%H:%M:%S'), line.strip()))
                sys.stdout.flush()
            pad_online = False
        if '音频异常计数变化' in line:
            audio_lines += 1
        if '输入帧率恢复' in line and '停了约' in line:   # 刚连上 / 刚开始读的那次「恢复」不算停顿
            stalls.append(line.strip())
        if not incident_at and any(k in line for k in INCIDENT):
            incident_at = time.time()
            print('!!! 事件 %s : %s' % (time.strftime('%H:%M:%S'), line.strip()))
            sys.stdout.flush()

    mins = (time.time() - start) / 60.0
    if incident_at:
        print('=== 出事了（监控 %.1f 分钟后，期间记录器重启 %d 次，此前输入停顿 %d 次）。事件前后的日志：===' % (mins, restarts, len(stalls)))
        for l in tail:
            print(l)
        return 0
    print('=== %.0f 分钟内没有断链/复位/USB 消失（记录器重启 %d 次）===' % (mins, restarts))
    if rates:
        live = [r for r in rates if r > 0]
        print('心跳 %d 次，帧率 最低 %d / 平均 %d / 最高 %d（为 0 的心跳 %d 次）' % (
            len(rates), min(rates), sum(rates) // len(rates), max(rates), len(rates) - len(live)))
    if batt:
        print('电量 %s%% -> %s%%' % (batt[0], batt[-1]))
    print('音频发送失败的提示行 %d 条（10 s 限流）' % audio_lines)
    print('输入停顿（链路短暂没数据、自己恢复）%d 次' % len(stalls))
    print('手柄在线但帧率 < 300/s 的心跳 %d 次（正常 600~780）' % low_rate)
    for s in stalls[-10:]:
        print('  ' + s)
    if last_perf:
        print('最后一行性能：' + last_perf.strip())
    return 0


if __name__ == '__main__':
    sys.exit(main())
