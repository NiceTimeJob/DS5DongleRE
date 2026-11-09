# -*- coding: utf-8 -*-
"""记录器 / 监控器：输入停顿也要记清楚（2026-10-14 15:37 实测：两次 1~2 s 的停顿，
记录器只记了「恢复」没记「掉到 0」，因为掉之前那 0.5 s 窗口的帧率只剩几十、不满足 >50）。
  watch_state.py：只要上一周期 ≥1 帧/s、这一周期 <1 就记「掉到 0」，恢复时带上停了多久。
  watch_alert.py：停顿不算事故（不退出），单独计数，结束时报告。"""
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _patchlib import P

T = os.path.dirname(os.path.abspath(__file__))

p = P(os.path.join(T, 'watch_state.py'))
p.rep("""            if rate is not None and rate > 50 and new_rate < 1:
                log('!!! 输入帧率掉到 0 (上一周期 %.0f 帧/s)' % rate)
            if (rate is None or rate < 1) and new_rate > 50:
                log('输入帧率恢复 %.0f 帧/s' % new_rate)
            rate = new_rate
""", """            if rate is not None and rate >= 1 and new_rate < 1:
                stall_t = seq_time        # 上一次还看到帧在涨的时刻
                log('!!! 输入帧率掉到 0 (上一周期 %.0f 帧/s)' % rate)
            if (rate is None or rate < 1) and new_rate > 50:
                if stall_t:
                    log('输入帧率恢复 %.0f 帧/s（停了约 %.1f s）' % (new_rate, t0 - stall_t))
                else:
                    log('输入帧率恢复 %.0f 帧/s' % new_rate)
                stall_t = None
            rate = new_rate
""")
p.rep("    perf_prev = None   # S33：(丢块, 发送失败, 看门狗触发) 上一周期的值\n",
      "    perf_prev = None   # S33：(丢块, 发送失败, 看门狗触发) 上一周期的值\n    stall_t = None     # 输入掉到 0 时记下的时刻（算停了多久）\n")
p.save()

p = P(os.path.join(T, 'watch_alert.py'))
p.rep("""    '帧率掉到 0', 'CRASH 区有效', 'SCAN_STUCK', 'HB_SCAN_FIX', '!!! 启动计数',
""", """    'CRASH 区有效', 'SCAN_STUCK', 'HB_SCAN_FIX', '!!! 启动计数',
""")
p.rep("""    audio_lines = 0
    last_perf = ''
""", """    audio_lines = 0
    stalls = []          # 输入停顿（不算事故，单独记）
    last_perf = ''
""")
p.rep("""        if '音频异常计数变化' in line:
            audio_lines += 1
""", """        if '音频异常计数变化' in line:
            audio_lines += 1
        if '输入帧率恢复' in line:
            stalls.append(line.strip())
""")
p.rep("""    print('音频发送失败的提示行 %d 条（10 s 限流）' % audio_lines)
""", """    print('音频发送失败的提示行 %d 条（10 s 限流）' % audio_lines)
    print('输入停顿（链路短暂没数据、自己恢复）%d 次' % len(stalls))
    for s in stalls[-10:]:
        print('  ' + s)
""")
p.rep("""        print('=== 出事了（监控 %.1f 分钟后，期间记录器重启 %d 次）。事件前后的日志：===' % (mins, restarts))
""", """        print('=== 出事了（监控 %.1f 分钟后，期间记录器重启 %d 次，此前输入停顿 %d 次）。事件前后的日志：===' % (mins, restarts, len(stalls)))
""")
p.save()
import ast
for fn in ('watch_state.py', 'watch_alert.py'):
    ast.parse(open(os.path.join(T, fn), encoding='utf-8').read())
print('RECORDER PATCH DONE')
