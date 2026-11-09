#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""网页刷写包体检：manifest.json ↔ 磁盘文件 ↔ 固件里的版本串 ↔ 分区表，四处必须对得上。

刷写页（webgui/js/page-flash.js）完全靠 manifest.json 干活：地址、大小、sha256、版本串。
任何一处对不上都可能刷坏、或者出现"明明旧版却说已是最新"。这个脚本把这条链逐环钉住：

  ① manifest 能解析，product / version / date / notes / files 都在
  ② version 与固件源码 version.h 的 "BL616-DongleRE <FW_VERSION>" 一致（允许 " HS" / " sdkXXX" 后缀）
  ③ version 与 app bin 里**真正嵌进去的字符串**逐字相等（刷写页拿它跟设备的 0xF8 比"已是最新"）
  ④ files[] 每个文件都在磁盘上、大小与 sha256 与 manifest 一致
  ⑤ 三个地址与 make_fw_pack.py 的 LAYOUT、以及 lctech616 分区表（Boot2 0 / pt 0xE000 / FW 0x10000）
     三处一致；app 必须装得进 FW 分区
  ⑥ flash_para 若在：文件在、大小/sha 对，且 [13] != 0xFF（官方流程会跳过、不该打包）
  ⑦ notes 里有当前 FW_VERSION 那一段（刷写页「更新内容」显示的就是它）
  ⑧ webflash/firmware（出包目录）与 webgui/firmware（发布目录）逐文件一致
  ⑨ build_out 里的 app 与 manifest 里的 app 是同一个（出包之后没有再编过）

用法：python -X utf8 tools/test_fw_pack.py
"""
import hashlib
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FW_SRC = os.path.join(ROOT, 'firmware')
PUB = os.path.join(ROOT, 'webgui', 'firmware')          # 发布目录（刷写页读这里）
OUT = os.path.join(ROOT, 'webflash', 'firmware')        # 出包目录（make_fw_pack.py 的 --out）
PACKER = os.path.join(ROOT, 'webflash', 'make_fw_pack.py')
TOML = os.path.join(FW_SRC, 'firmware', 'lctech616', 'partition_cfg_4M.toml')
BUILD_OUT = os.path.join(FW_SRC, 'build', 'build_out')

bad = []
def ok(m):   print('  ok   %s' % m)
def fail(m): bad.append(m); print('  FAIL %s' % m)

def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 16), b''):
            h.update(chunk)
    return h.hexdigest()

print('== 网页刷写包体检  %s ==' % PUB)

# ---- ① manifest ----
mpath = os.path.join(PUB, 'manifest.json')
if not os.path.isfile(mpath):
    print('  FAIL 没有 manifest.json（先跑 webflash/make_fw_pack.py）')
    sys.exit(1)
with open(mpath, encoding='utf-8') as f:
    man = json.load(f)
need = ['product', 'version', 'date', 'notes', 'files']
miss = [k for k in need if k not in man]
if not miss:
    ok('manifest 五个字段都在（product / version / date / notes / files）')
else:
    fail('manifest 缺字段：%s' % '/'.join(miss))
    sys.exit(1)
print('       版本 %s，打包日期 %s，文件 %d 个，更新内容 %d 段'
      % (man['version'], man['date'], len(man['files']), len(man['notes'])))

# ---- ② version.h ----
ver_h = open(os.path.join(FW_SRC, 'src', 'version.h'), encoding='utf-8').read()
fwver = re.search(r'#define\s+FW_VERSION\s+"([^"]+)"', ver_h).group(1)
prod  = re.search(r'#define\s+FW_PRODUCT\s+"([^"]+)"', ver_h)
want = 'BL616-DongleRE ' + fwver          # 本机只编 lctech616 普通版
if man['version'] == want or man['version'].startswith(want + ' '):
    ok('manifest version == version.h 派生串（%s）' % want)
else:
    fail('manifest version = %r，version.h 派生的应该是 %r（只允许后面多 " HS"/" sdkXXX"）'
         % (man['version'], want))

# ---- ③ app bin 里嵌进去的版本串 ----
app = next((f for f in man['files'] if f['name'].startswith('ds5donglere_')), None)
if not app:
    fail('manifest 里没有 app（ds5donglere_*.bin）')
    sys.exit(1)
app_path = os.path.join(PUB, app['name'])
blob = open(app_path, 'rb').read()
found = set(m.group(0).decode('ascii') for m in
            re.finditer(rb'BL616-DongleRE R[0-9]+(\.[0-9]+)?( HS)?( sdk[0-9]+)?', blob))
if man['version'] in found:
    ok('app bin 里嵌的版本串就是 %r（0xF8 会照这个答）' % man['version'])
else:
    fail('app bin 里的版本串是 %s，manifest 却写 %r —— 刷写页会永远说"可刷写更新"'
         % ('/'.join(sorted(found)) or '（一个都没找到）', man['version']))

# ---- ④ 文件与哈希 ----
sizes = {f['name']: (f['size'], f['sha256']) for f in man['files']}
for name, (sz, sha) in sizes.items():
    p = os.path.join(PUB, name)
    if not os.path.isfile(p):
        fail('manifest 里的 %s 在磁盘上没有' % name); continue
    real = os.path.getsize(p)
    if real != sz:
        fail('%s 大小对不上：manifest %d / 磁盘 %d' % (name, sz, real)); continue
    rsha = sha256(p)
    if rsha != sha:
        fail('%s sha256 对不上：manifest %s / 磁盘 %s' % (name, sha[:12], rsha[:12])); continue
if not bad:
    ok('%d 个文件的大小与 sha256 全部与 manifest 一致' % len(sizes))

# ---- ⑤ 地址：LAYOUT / 分区表 / 实际文件 三处一致 ----
packer = open(PACKER, encoding='utf-8').read()
layout = re.findall(r"\('([^']+)',\s*(0x[0-9A-Fa-f]+),\s*'([^']+)'\)", packer)
lay = {arc: int(addr, 16) for _pat, addr, arc in layout}
if len(lay) == 3:
    ok('make_fw_pack.py 的 LAYOUT 解析到 3 项：%s'
       % ', '.join('%s@0x%X' % (k, v) for k, v in lay.items()))
else:
    fail('make_fw_pack.py 的 LAYOUT 解析不出 3 项（改了格式就同步改这个测试）')
toml = open(TOML, encoding='utf-8').read()
# 注意：toml 里的 0 是裸写的（`address0 = 0`），不是 0x0 —— 正则必须两种都收，
# 否则非贪婪扫描会一路跳到下一个带 0x 的地址（曾经因此把 Boot2 读成 0x10000）
ADDR = r'(0x[0-9A-Fa-f]+|\d+)'
def pt_addr(name):
    m = re.search(r'name\s*=\s*"%s"[\s\S]*?address0\s*=\s*' % name + ADDR, toml)
    return int(m.group(1), 0) if m else None
def pt_size(name):
    m = re.search(r'name\s*=\s*"%s"[\s\S]*?size0\s*=\s*' % name + ADDR, toml)
    return int(m.group(1), 0) if m else None
pts = re.search(r'\[pt_table\][\s\S]*?address0\s*=\s*' + ADDR, toml)
pt_addr_pt = int(pts.group(1), 0) if pts else None
expect = {
    'boot2':     (pt_addr('Boot2'), layout and lay.get('boot2_bl616_isp_release.bin')),
    'partition': (pt_addr_pt,      lay.get('partition.bin')),
    'app':       (pt_addr('FW'),   lay.get('ds5donglere_bl618_bl616.bin')),
}
for key, (from_toml, from_packer) in expect.items():
    if from_toml is None or from_packer is None:
        fail('%s 的地址取不到（toml=%s packer=%s）' % (key, from_toml, from_packer)); continue
    if from_toml != from_packer:
        fail('%s 地址：分区表 0x%X ≠ make_fw_pack.py 0x%X' % (key, from_toml, from_packer)); continue
    ok('%s 地址 0x%06X（分区表 == make_fw_pack.py）' % (key, from_toml))
for f in man['files']:
    key = ('boot2' if f['name'].startswith('boot2') else
           'partition' if f['name'].startswith('partition') else 'app')
    if f['addr'] != expect[key][0]:
        fail('%s 在 manifest 里的地址 0x%X ≠ 应为 0x%X' % (f['name'], f['addr'], expect[key][0]))
fw_size = pt_size('FW')
if app['size'] > fw_size:
    fail('app %d 字节装不进 FW 分区（0x%X）' % (app['size'], fw_size))
else:
    ok('app %d 字节，FW 分区 0x%X（够放）' % (app['size'], fw_size))

# ---- ⑥ flash_para ----
fp = man.get('flash_para')
if fp:
    p = os.path.join(PUB, fp['name'])
    if not os.path.isfile(p):
        fail('manifest 有 flash_para，但磁盘上没有 %s' % fp['name'])
    else:
        real = os.path.getsize(p); rsha = sha256(p)
        data = open(p, 'rb').read()
        if real != fp['size'] or rsha != fp['sha256']:
            fail('flash_para 大小/sha 对不上')
        elif data[13:14] == b'\xff':
            fail('flash_para.bin[13] == 0xFF —— 官方流程会跳过它，不该打进包里')
        else:
            ok('flash_para.bin %d 字节、sha 对，[13]=0x%02X（会真的发出去）' % (real, data[13]))
else:
    print('  [i] 包里没有 flash_para（不致命，但刷写会少一次扩展 set_para）')

# ---- ⑦ notes 要有当前版本 ----
notes_vers = [n.get('version') for n in man['notes']]
if fwver in notes_vers:
    idx = notes_vers.index(fwver)
    n = man['notes'][idx]
    ok('更新内容里有 %s（%s，%d 条）%s' % (fwver, n.get('date'), len(n.get('items', [])),
                                        '，且排在第一' if idx == 0 else '（不在第一条：%s）' % notes_vers[:3]))
else:
    fail('更新内容里没有 %s 那一段（CHANGELOG.md 少了版本小节，或 NOTES_SINCE 之后没被提取）' % fwver)

# ---- ⑧ 两个目录一致 ----
if os.path.isdir(OUT):
    diff = []
    for name in sorted(set(os.listdir(PUB)) | set(os.listdir(OUT))):
        a, b = os.path.join(PUB, name), os.path.join(OUT, name)
        if not (os.path.isfile(a) and os.path.isfile(b)):
            diff.append(name + '(只在一处)'); continue
        if sha256(a) != sha256(b):
            diff.append(name + '(内容不同)')
    if diff:
        fail('webgui/firmware 与 webflash/firmware 不一致：%s（重跑 make_fw_pack.py）' % ', '.join(diff))
    else:
        ok('webgui/firmware 与 webflash/firmware 逐文件一致（%d 个）' % len(os.listdir(PUB)))
else:
    print('  [i] 没有 webflash/firmware 目录，跳过一致性对比')

# ---- ⑨ build_out 的 app 与包里的 app 同一个 ----
bo = os.path.join(BUILD_OUT, 'ds5donglere_bl618_bl616.bin')
if os.path.isfile(bo):
    if sha256(bo) == app['sha256']:
        ok('build_out 里的 app 与包里的 app 是同一个（出包之后没再编过）')
    else:
        fail('build_out 的 app（%s）与包里的（%s）不是同一个 —— 出包之后又编过，重跑 make_fw_pack.py'
             % (sha256(bo)[:12], app['sha256'][:12]))
else:
    print('  [i] 没有 build_out（清过 build），跳过"包是不是最新编出来的"这一条')

print('\n' + ('全部通过' if not bad else '%d 项失败' % len(bad)))
sys.exit(1 if bad else 0)
