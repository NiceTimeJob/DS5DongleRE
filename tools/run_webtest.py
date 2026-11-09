# -*- coding: utf-8 -*-
"""跑网页离线自测（headless Edge --dump-dom），打印 <pre id="out"> 里的结果。
自己起一个 serve.py，配置目录和日志目录都指到临时目录 —— 怎么测都碰不到真配置、真日志。

用法：python -X utf8 run_webtest.py <页面> [端口] [虚拟时间预算 ms]
  页面   _gmtest.html / _smoke.html / _logtest.html …
  端口   默认 8093：自己起一个 webgui/serve.py，不碰用户开着的 8080
"""
import os
import re
import subprocess
import sys
import tempfile
import time
import urllib.request
import html
import io

ROOT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'webgui')
EDGE = r'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe'
page = sys.argv[1]
port = sys.argv[2] if len(sys.argv) > 2 else '8093'
budget = sys.argv[3] if len(sys.argv) > 3 else '120000'

# 测试服务器和真东西隔开：配置目录（~/DongleRE/configs）和日志目录（tools/logs）都指到临时目录。
# 页面里的日志页一连上（假）接收机就往 /__log 写，2026-10-16 就这样往 tools/logs 里留过一堆假会话。
sandbox = tempfile.mkdtemp(prefix='webtest_')
env = dict(os.environ, USERPROFILE=sandbox, HOME=sandbox,
           DONGLERE_LOGDIR=os.path.join(sandbox, 'logs'))
os.makedirs(env['DONGLERE_LOGDIR'], exist_ok=True)
srv = subprocess.Popen([sys.executable, '-X', 'utf8', os.path.join(ROOT, 'serve.py'), port],
                       cwd=ROOT, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
try:
    for _ in range(50):
        try:
            urllib.request.urlopen('http://127.0.0.1:%s/__ver' % port, timeout=1).read()
            break
        except Exception:
            time.sleep(0.2)
    prof = tempfile.mkdtemp(prefix='edge_wt_')
    # 直接调 msedge.exe 拿不到 --dump-dom 的输出（它把活交给另一个进程就返回了），
    # 要走 PowerShell 管道（HANDOFF r48 轮记的坑）
    dump = os.path.join(prof, 'dom.html')
    # 管道默认按控制台代码页（GBK）解 Edge 的输出，中文会乱、连 </pre> 的 < 都被吃掉 —— 先切 UTF-8
    cmd = ("[Console]::OutputEncoding = [System.Text.Encoding]::UTF8; "
           "& '%s' --headless=new --disable-gpu --no-first-run --user-data-dir='%s' "
           "--virtual-time-budget=%s --dump-dom 'http://127.0.0.1:%s/%s' 2>$null "
           "| Out-File -Encoding utf8 '%s'") % (EDGE, prof, budget, port, page, dump)
    subprocess.run(['powershell', '-NoProfile', '-Command', cmd], timeout=600)
    dom = io.open(dump, encoding='utf-8-sig', errors='replace').read() if os.path.exists(dump) else ''
    m = re.search(r'<pre id="out"[^>]*>(.*?)</pre>', dom, re.S)
    if not m:
        print('没拿到 <pre id="out">（%d 字节 DOM）' % len(dom))
        print(dom[:2000])
        sys.exit(2)
    print(html.unescape(m.group(1)))
finally:
    srv.terminate()
