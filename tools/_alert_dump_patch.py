# -*- coding: utf-8 -*-
"""watch_alert：记录器重启后的"开机转储"不算事故。
15:41 那次是记录器卡死被重启，新记录器把 DIAG 环从头打了一遍（里面有开机时的 RST_REASON），
监控器把它当成了"接收器重启"。现在：
  - 从 "=== watch_state 启动" 到第一条实时行（心跳 / 输入帧率 / DIAG t=）之间是转储，跳过事故匹配；
  - 转储里的 "DIAG head=… boot=N" 和此前心跳里的 boot=N 比，变了才算接收器真的重启过。"""
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _patchlib import P

T = os.path.dirname(os.path.abspath(__file__))
p = P(os.path.join(T, 'watch_alert.py'))
p.rep("""import glob
import os
""", """import glob
import os
import re
""")
p.rep("""    stalls = []          # 输入停顿（不算事故，单独记）
""", """    stalls = []          # 输入停顿（不算事故，单独记）
    in_dump = False      # 记录器刚启动时把 DIAG 环整个打一遍，那段不是实时事件
    last_boot = None     # 心跳里的启动计数
""")
p.rep("""        if '音频异常计数变化' in line:
            audio_lines += 1
""", """        mb = re.search(r'boot=(\\d+)', line)
        if '=== watch_state 启动' in line:
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
        if '音频异常计数变化' in line:
            audio_lines += 1
""")
p.save()
import ast
ast.parse(open(os.path.join(T, 'watch_alert.py'), encoding='utf-8').read())
print('WATCH_ALERT DUMP PATCH DONE')
