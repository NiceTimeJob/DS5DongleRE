# -*- coding: utf-8 -*-
"""watch_alert：
  ① 「手柄 离线」只在 在线→离线 的那一下才算事故（记录器在音频标志变化时也会打一行「手柄 离线 … 音频流=扬声器」，
     15:54:57 就被这行误报了）。
  ② 帧率偏低单独计数：手柄在线、心跳帧率 < 300 的次数（正常 600~780；15:55 起只有 120~140），结束时报告。"""
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _patchlib import P

T = os.path.dirname(os.path.abspath(__file__))
p = P(os.path.join(T, 'watch_alert.py'))
p.rep("""    'L2CAP_CTRL_DOWN', 'L2CAP_INTR_DOWN', 'CONNECTED ->', '手柄 离线',
)
""", """    'L2CAP_CTRL_DOWN', 'L2CAP_INTR_DOWN', 'CONNECTED ->',
)
""")
p.rep("""    in_dump = False      # 记录器刚启动时把 DIAG 环整个打一遍，那段不是实时事件
""", """    in_dump = False      # 记录器刚启动时把 DIAG 环整个打一遍，那段不是实时事件
    pad_online = None    # 上一次看到的手柄在线状态（只有 在线→离线 才算事故）
    low_rate = 0         # 手柄在线但心跳帧率 < 300 的次数
""")
p.rep("""        if mb and '心跳' in line:
            last_boot = mb.group(1)
""", """        if mb and '心跳' in line:
            last_boot = mb.group(1)
            if '手柄=在线' in line:
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
""")
p.rep("""    print('输入停顿（链路短暂没数据、自己恢复）%d 次' % len(stalls))
""", """    print('输入停顿（链路短暂没数据、自己恢复）%d 次' % len(stalls))
    print('手柄在线但帧率 < 300/s 的心跳 %d 次（正常 600~780）' % low_rate)
""")
p.save()
import ast
ast.parse(open(os.path.join(T, 'watch_alert.py'), encoding='utf-8').read())
print('WATCH_ALERT OFFLINE/LOWRATE PATCH DONE')
