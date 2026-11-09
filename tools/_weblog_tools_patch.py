# -*- coding: utf-8 -*-
"""网页日志页（webgui/js/page-log.js）上线后，后台记录器 / 告警脚本的配合：
  watch_state.py：tools/logs/web_active 4 s 内刷新过（网页正在读接收器）就暂停，关掉句柄让路；
                  网页停了再接着记（先写一行 === 记录器继续 ===，再按重开的流程读一遍启动快照）。
  watch_alert.py：同时跟 state_*.log 和 web_*.log（谁最新跟谁，各自记住读到哪）；
                  「### OPEN」「=== 记录器继续」「=== 网页日志」后面那段是启动快照，不算实时事件；
                  只把带「停了约」的「输入帧率恢复」算停顿（刚连上的第一次读数不算）；
                  「!!! 接收器无应答」算事故。
用法：python tools/_weblog_tools_patch.py"""
import ast
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _patchlib import P

T = os.path.dirname(os.path.abspath(__file__))

p = P(os.path.join(T, 'watch_state.py'))
p.rep("""CRASH_MAGIC = 0xC0DE6116
""", """CRASH_MAGIC = 0xC0DE6116
WEB_MARK = os.path.join(LOGDIR, 'web_active')   # 网页日志页在读接收器时，serve.py 每秒刷新它


def web_active():
    \"\"\"网页日志页正在读接收器（4 s 内刷新过 web_active）→ 记录器让路：两边同时轮询会抢 0xFC 的响应寄存器。\"\"\"
    try:
        return time.time() - os.path.getmtime(WEB_MARK) < 4.0
    except OSError:
        return False
""")
p.rep("""    while True:
        t0 = time.time()
        present = pad_present()
""", """    web_paused = False
    while True:
        t0 = time.time()
        if web_active():
            if not web_paused:
                web_paused = True
                log('=== 网页日志在读接收器（tools/logs/web_*.log），记录器暂停，免得两边抢 0xFC 通道 ===')
                if h is not None:
                    try:
                        h.close()
                    except Exception:
                        pass
                    h = None
            time.sleep(INTERVAL)
            continue
        if web_paused:
            web_paused = False
            log('=== 记录器继续（网页日志停了）===')
            was_present = None
        present = pad_present()
""")
p.save()
ast.parse(p.s)

p = P(os.path.join(T, 'watch_alert.py'))
p.rep("""    'L2CAP_CTRL_DOWN', 'L2CAP_INTR_DOWN', 'CONNECTED ->',
)
""", """    'L2CAP_CTRL_DOWN', 'L2CAP_INTR_DOWN', 'CONNECTED ->', '!!! 接收器无应答',
)
# 这些行后面跟着的是（重新）连上时的启动快照（整个 DIAG 环），不是刚发生的事
DUMP_START = ('=== watch_state 启动', '=== 记录器继续', '=== 网页日志', '### OPEN')
""")
p.rep("""def newest():
    files = glob.glob(os.path.join(LOGDIR, 'state_*.log'))
    return max(files, key=os.path.getmtime) if files else None
""", """def newest():
    \"\"\"后台记录器（state_*.log）和网页日志页（web_*.log，serve.py 写）谁最新跟谁。\"\"\"
    files = (glob.glob(os.path.join(LOGDIR, 'state_*.log')) +
             glob.glob(os.path.join(LOGDIR, 'web_*.log')))
    return max(files, key=os.path.getmtime) if files else None
""")
p.rep("""    f = open(path, encoding='utf-8', errors='replace')
    f.seek(0, os.SEEK_END)
    start = time.time()
""", """    f = open(path, encoding='utf-8', errors='replace')
    f.seek(0, os.SEEK_END)
    offsets = {}         # 每个日志文件读到哪了（网页日志和记录器会轮流成为「最新」）
    start = time.time()
""")
p.rep("""            nw = newest()
            if nw and nw != path:
                path = nw
                f = open(path, encoding='utf-8', errors='replace')
                tail.append('--- 记录器换了日志文件: %s ---' % os.path.basename(path))
                last_line_t = now
""", """            nw = newest()
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
""")
p.rep("""        if '=== watch_state 启动' in line:
            in_dump = True
""", """        if any(k in line for k in DUMP_START):
            in_dump = True
""")
p.rep("""        if '输入帧率恢复' in line:
            stalls.append(line.strip())
""", """        if '输入帧率恢复' in line and '停了约' in line:   # 刚连上 / 刚开始读的那次「恢复」不算停顿
            stalls.append(line.strip())
""")
p.save()
ast.parse(p.s)
print('WEBLOG TOOLS PATCH DONE')
