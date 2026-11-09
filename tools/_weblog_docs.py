# -*- coding: utf-8 -*-
"""网页日志页的文档：CHANGELOG（R1.6 段加网页）/ HANDOFF（R1.6 段后面）/ 记忆。"""
import io
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _patchlib import P

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')

p = P(os.path.join(ROOT, 'firmware', 'CHANGELOG.md'))
p.rep("""### Added
- 性能统计加输出报告：每秒多少份、「USB 收到 → 交给蓝牙」的平均 / 最大延迟、排队丢弃数
""", """### Added
- 性能统计加输出报告：每秒多少份、「USB 收到 → 交给蓝牙」的平均 / 最大延迟、排队丢弃数
- 网页新增「日志」页（固件刷写旁边）：实时状态卡片（手柄 / 帧率小图 / 蓝牙链路 / 音频 / 震动输出 / 每帧耗时 / CPU）+ 事件日志（和后台记录器同格式，可只看事件、导出 .txt）。接收器连着就在后台每秒读一次；用本机 serve.bat 打开时同步写进 tools/logs/web_*.log，后台记录器自动让路
""")
p.save()

p = P(os.path.join(ROOT, 'firmware', 'HANDOFF.md'))
p.rep("""- 包：`_archive/R1.6_sdk232_938b8122`（787184 B）。

### R1.5""", """- 包：`_archive/R1.6_sdk232_938b8122`（787184 B）。
- **网页「日志」页**（用户：「刷写固件旁边加个日志功能 把这些都动态写上去」，r37）：`webgui/js/page-log.js`。接收器连着就每秒读 0xF9 + PERF（按 0xF8 版本定长度 `perfSizeFor`：多读的字节是上一条响应的残留，不能拿来解析）+ DIAG 16 B 表头（head/boot 变了才读整个环），每 10 s 心跳 + 性能；启动 / 重连读版本、CRASH、DIAG、DIAG_PREV。**文字格式和 watch_state.py 逐字一致**（`tools/_weblog_parity.py` 拿真机字节比过：PERF、25 条真 DIAG、28 条造的各类事件全部一致），watch_alert 的判据直接适用。`protocol.js` 断开事件带 `reason`（unplug / manual / noresp）：拔掉记「### GONE」、无应答记「!!! 接收器无应答」、手动断开只记一行。`serve.py` 加 `POST /__log?s=<会话>[&a=1]` → `tools/logs/web_<会话>.log`，带 a=1（正在读且上一拍成功）就刷新 `tools/logs/web_active`；`watch_state.py` 看到它 4 s 内刷新过就关句柄暂停（`=== 网页日志在读接收器 …`），网页停了写 `=== 记录器继续` 再按重开流程读快照；`watch_alert.py` 同时跟 state_* 和 web_*（谁最新跟谁、各自记偏移），`### OPEN` / `=== 记录器继续` / `=== 网页日志` 后面是快照不算事件，「输入帧率恢复」只有带「停了约」才算停顿（修掉之前把记录器重启算成停顿的误计），「!!! 接收器无应答」算事故。离线自测：`webgui/_logtest.html`（iframe + 假 DS5，headless Edge `--dump-dom`）。**用户要用新的 serve.py**：旧服务器窗口开着的话要关掉重开 serve.bat，否则页面状态栏会提示没写到磁盘。

### R1.5""")
p.save()

mem = os.path.join(os.path.expanduser('~'), '.claude', 'projects', 'E--BL616-Project-DongleRE', 'memory')
path = os.path.join(mem, 'dongle-test-workflow.md')
s = io.open(path, encoding='utf-8').read()
old = '6. 网页：让用户双击 `webgui\\serve.bat`（http://127.0.0.1:8080，别用 localhost）。会话里后台起的服务 2 小时会被系统停掉。网页和记录器同时读接收器会互相抢。'
new = ('6. 网页：让用户双击 `webgui\\serve.bat`（http://127.0.0.1:8080，别用 localhost）。会话里后台起的服务 2 小时会被系统停掉。'
       '2026-10-14 起网页有「日志」页：网页连着接收器就每秒读一次，经 serve.py 写 `tools/logs/web_*.log` 并刷新 `web_active`，'
       '记录器 watch_state.py 自动暂停让路（要重启过的新版记录器才有这逻辑），watch_alert 同时跟 state_* / web_*。'
       '改了 watch_state.py 要重启记录器进程才生效。')
assert s.count(old) == 1
io.open(path, 'w', encoding='utf-8').write(s.replace(old, new))
print('WEBLOG DOCS DONE')
