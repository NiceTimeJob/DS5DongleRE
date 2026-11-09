# -*- coding: utf-8 -*-
"""一次性核对：网页日志页（webgui/js/page-log.js）和 Python 工具对同一份字节格式化出来的文字是否逐字一样。
读真机的 PERF / DIAG 原始字节 → Python 格式化 → 交给 node 跑 page-log.js 格式化 → 逐行比较。
用法：python -X utf8 tools/_weblog_parity.py"""
import ast
import json
import os
import subprocess
import sys

import diag

T = os.path.dirname(os.path.abspath(__file__))
JS = os.path.join(T, '..', 'webgui', 'js', 'page-log.js')

# watch_state.fmt_rec 单独拿出来（import watch_state 会新建日志文件）
src = open(os.path.join(T, 'watch_state.py'), encoding='utf-8').read()
ns = {'diag': diag}
for node in ast.parse(src).body:
    if isinstance(node, ast.FunctionDef) and node.name == 'fmt_rec':
        exec(compile(ast.Module([node], []), 'watch_state.py', 'exec'), ns)
fmt_rec = ns['fmt_rec']

h = diag.open_dev()
if h is None:
    sys.exit('接收器不在')
ver = None
try:
    h.send_feature_report(bytes([0xF8]) + bytes(63))
    r = bytes(h.get_feature_report(0xF8, 64))
    ver = (r[1:] if r[0] == 0xF8 else r).split(b'\0')[0].decode()
except Exception:
    pass
perf = None
for _ in range(5):
    a = diag.rpc_read(h, diag.REG_PERF, 0, 116)
    b = diag.rpc_read(h, diag.REG_PERF, 116, 44)
    if a and b:
        perf = bytes(a) + bytes(b)
        break
dbuf = diag.fetch_diag(h)
h.close()
if not perf or not dbuf:
    sys.exit('读不到 PERF / DIAG')

# 按固件版本截 PERF（多读的是残留），两边用同一份
size = {'R1.5': 152, 'R1.4': 148, 'R1.6': 160}.get((ver or '').split()[1] if ver and len(ver.split()) > 1 else '', 152)
perf = perf[:size]


def fake_rpc(_h, region, off, ln, tries=0):
    return perf[off:off + ln]


diag.rpc_read = fake_rpc
py_perf = diag.fmt_perf(diag.fetch_perf(None))
_, _, recs = diag.parse(dbuf)
py_recs = [fmt_rec(t, ev, a1) for (t, ev, a1, a2) in recs]

# 额外造几条覆盖各种事件码的记录
extra = [(1000, 28, 0x1b06), (1001, 2, 0x0701), (1002, 3, 0x80), (1003, 3, 0x00038004), (1004, 4, 0x10108),
         (1005, 18, 157324), (1006, 33, 3), (1007, 34, 4), (1008, 35, 2), (1009, 36, 0x014fff00), (1010, 37, 1254),
         (1011, 38, 0x00014000 | 0x7d00), (1012, 38, 0xFE011F40), (1013, 39, 0x00010012), (1014, 39, 0x00281400),
         (1015, 39, 0x0000FF14), (1016, 39, 0x00003F81), (1017, 39, 0x00007d38), (1018, 40, 0x014e004c),
         (1019, 40, 0x01FFFF4f), (1020, 41, 0x00FE0001), (1021, 41, 0x0D000001), (1022, 26, 2), (1023, 30, 1),
         (1024, 31, 1), (1025, 32, 2), (1026, 99, 0xdeadbeef), (1027, 2, 0x0009)]
py_extra = [fmt_rec(t, ev, a1) for (t, ev, a1) in extra]

node = r"""
const fs = require('fs');
global.window = {}; global.document = {}; global.location = { protocol: 'http:', hostname: '127.0.0.1' };
const src = fs.readFileSync(process.argv[1], "utf8");
eval(src + '\n;global.LogPage = LogPage;');
const inp = JSON.parse(fs.readFileSync(0, 'utf8'));
const hex2u8 = h => Uint8Array.from(h.match(/../g).map(x => parseInt(x, 16)));
const perf = LogPage.parsePerf(hex2u8(inp.perf));
const d = LogPage.parseDiag(hex2u8(inp.diag));
const out = {
  perf: LogPage.fmtPerf(perf),
  recs: d.recs.map(([t, ev, a1]) => LogPage.fmtRec(t, ev, a1)),
  extra: inp.extra.map(([t, ev, a1]) => LogPage.fmtRec(t, ev, a1)),
  size: LogPage.perfSizeFor(inp.ver),
};
process.stdout.write(JSON.stringify(out));
"""
p = subprocess.run(['node', '-e', node, JS], input=json.dumps({
    'perf': perf.hex(), 'diag': bytes(dbuf).hex(), 'extra': extra, 'ver': ver or ''}),
    capture_output=True, text=True, encoding='utf-8')
if p.returncode:
    sys.exit('node 失败：' + p.stderr)
js = json.loads(p.stdout)

bad = 0
print('固件版本:', ver, ' PERF', len(perf), 'B  JS 判定 PERF 大小:', js['size'])
for name, a, b in [('PERF', [py_perf], [js['perf']]), ('DIAG', py_recs, js['recs']), ('造的记录', py_extra, js['extra'])]:
    for x, y in zip(a, b):
        if x != y:
            bad += 1
            print('[不同] %s\n  py: %s\n  js: %s' % (name, x, y))
    if len(a) != len(b):
        bad += 1
        print('[条数不同] %s py=%d js=%d' % (name, len(a), len(b)))
    print('%s：%d 条比较完' % (name, len(a)))
print('PERF 一行（JS）:', js['perf'][:160], '...')
print('全部一致' if not bad else '有 %d 处不同' % bad)
