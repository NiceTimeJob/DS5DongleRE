# -*- coding: utf-8 -*-
"""游戏配置接口（webgui/serve.py 的 /__game/*）冒烟测试 —— 不经过浏览器，直接打 HTTP。

用法：先跑着本机服务器，然后  python tools/test_game_cfg.py [端口]
      （r57 起建议另起一个指到临时目录的服务器再测，别碰真配置目录：
        set USERPROFILE=%TEMP%/gcfg_t && python webgui/serve.py 8094，然后测 8094）

覆盖：列表 / 保存（新建 + 同名覆盖）/ 读取 / 改名（配置名 + 文件名）/ 删除 /
      目录穿越拦截 / 空名字、非法内容被拒。测试文件用 `_t-` 前缀，跑完自己删掉，
      并在最后确认配置目录里没留下任何测试残留。

历史：r48 那版叫 test_global_cfg.py，测的是已经删掉的 /__cfg/*（「全局配置」页）。
r50/r51 换成 /__game/*（按键映射页左上「游戏配置」）后那个文件就一直是红的，2026-10-16
按现在的接口重写。
"""
import io
import json
import os
import sys
import urllib.error
import urllib.parse
import urllib.request

PORT = sys.argv[1] if len(sys.argv) > 1 else '8080'
BASE = 'http://127.0.0.1:%s' % PORT
PREFIX = '_t-'                      # 测试文件名前缀：别和用户的「配置1.json」撞
ok_all = True


def call(path, body=None):
    url = BASE + path
    if body is None:
        req = urllib.request.Request(url)
    else:
        req = urllib.request.Request(url, data=json.dumps(body).encode('utf-8'),
                                     headers={'Content-Type': 'application/json'})
    try:
        with urllib.request.urlopen(req, timeout=8) as r:
            return r.status, json.loads(r.read().decode('utf-8'))
    except urllib.error.HTTPError as e:
        try:
            return e.code, json.loads(e.read().decode('utf-8'))
        except Exception:
            return e.code, {}
    except Exception as e:
        return 0, {'err': str(e)}


def check(name, cond, extra=''):
    global ok_all
    print(('  ok   ' if cond else '  FAIL ') + name + (('  ' + str(extra)) if extra else ''))
    if not cond:
        ok_all = False


# 一份「像真配置」的载荷：app/ver/name/hash + 页面真正会存的那几块
def payload(name, hash_):
    return {
        'app': 'ds5donglere-game', 'ver': 1, 'name': name, 'hash': hash_,
        'meta': {'hold': 300},
        'slots': [], 'binds': [], 'gyro': {}, 'tp': {}, 'vk': {}, 'strats': [],
    }


print('== 游戏配置接口冒烟测试  %s ==' % BASE)
st, r = call('/__game/list')
check('list 可用', st == 200 and r.get('ok'), r.get('dir', r))
if st != 200 or not r.get('ok'):
    print('\n服务器没在跑 / 不是 webgui/serve.py（普通 http.server 没有 /__game 接口）')
    sys.exit(1)
CFGDIR = r.get('dir', '')
before = [p['file'] for p in r.get('profiles', [])]

# ---------- 保存：新建 ----------
st, r = call('/__game/save', {'name': PREFIX + '甲', 'data': payload(PREFIX + '甲', 'aaaa1111')})
f1 = r.get('file', '')
check('新建（文件名跟配置名走）', st == 200 and r.get('ok') and f1 == PREFIX + '甲.json', f1)
check('新建时 created=true', r.get('created') is True, r.get('created'))
check('回显 hash', r.get('hash') == 'aaaa1111', r.get('hash'))
# r57（方案 B）：网页要把 saved_at 原样写进接收机的身份牌，所以 /save 要回给它
check('回显 saved_at（YYYY-MM-DD HH:MM:SS）',
      len(str(r.get('saved_at') or '')) == 19 and str(r.get('saved_at'))[4] == '-', r.get('saved_at'))

# ---------- 保存：同名 = 覆盖同一份（用户定的规矩：名字就是身份） ----------
st, r = call('/__game/save', {'name': PREFIX + '甲', 'data': payload(PREFIX + '甲', 'bbbb2222')})
check('同名再存 = 覆盖同一份（不新建、不报错）',
      r.get('ok') and r.get('file') == f1 and r.get('created') is False, r)

st, r = call('/__game/get?f=' + urllib.parse.quote(f1))
check('读回内容（name/saved_at/hash 都落了盘）',
      st == 200 and r.get('name') == PREFIX + '甲' and r.get('hash') == 'bbbb2222'
      and bool(r.get('saved_at')), r.get('saved_at'))

st, r = call('/__game/list')
names = [p['file'] for p in r.get('profiles', [])]
check('列表里有它、且只多这一份', names.count(f1) == 1 and len(names) == len(before) + 1,
      [n for n in names if n not in before])

# ---------- 改名（配置名 + 文件名一起改） ----------
f2 = PREFIX + '乙.json'
st, r = call('/__game/rename', {'file': f1, 'name': PREFIX + '乙', 'newFile': f2})
check('改名 + 改文件名', st == 200 and r.get('ok') and r.get('file') == f2
      and r.get('name') == PREFIX + '乙', r)

st, r = call('/__game/get?f=' + urllib.parse.quote(f2))
check('改的是同一个文件的内容（hash 没丢）', r.get('hash') == 'bbbb2222', r.get('name'))
st, r = call('/__game/get?f=' + urllib.parse.quote(f1))
check('旧文件名已经不在', st == 404 or not r.get('name'), st)

# 只改配置名（不给 newFile）＝ 文件不动
st, r = call('/__game/rename', {'file': f2, 'name': PREFIX + '丙'})
check('只改配置名、文件不动', r.get('ok') and r.get('file') == f2
      and r.get('name') == PREFIX + '丙', r)

# ---------- 删除 ----------
st, r = call('/__game/delete', {'file': f2})
check('删除', r.get('ok'), r)
st, r = call('/__game/list')
check('删完列表回到原样', [p['file'] for p in r.get('profiles', [])] == before, r.get('profiles'))

# ---------- 拒绝不合法输入 ----------
st, r = call('/__game/save', {'name': '', 'data': payload('x', '')})
check('空配置名被拒', not r.get('ok'), r.get('err'))
st, r = call('/__game/save', {'name': PREFIX + '丁', 'data': 'not-a-dict'})
check('配置内容不是对象被拒', not r.get('ok'), r.get('err'))
# 点名一个不合法文件名：不报错，回落成「文件名跟配置名走」（网页只会传列表里的名字，
# 走到这条说明请求是手写的；别静默写到 a.txt 去就行）
st, r = call('/__game/save', {'name': PREFIX + '戊', 'data': payload('x', ''), 'file': 'a.txt'})
check('点名非法文件名 → 回落成按配置名', r.get('ok') and r.get('file') == PREFIX + '戊.json',
      r.get('file'))
call('/__game/delete', {'file': PREFIX + '戊.json'})

for bad in ['../serve.py', '..\\x.json', 'sub/x.json', 'C:\\Windows\\x.json', 'x.txt', '.state.json']:
    st, r = call('/__game/get?f=' + urllib.parse.quote(bad))
    check('目录穿越拦截 %-18s' % bad, st == 404 or not r.get('ok'), st)
    st, r = call('/__game/delete', {'file': bad})
    check('  删除也拦 %-18s' % bad, not r.get('ok'), r.get('err'))
    st, r = call('/__game/rename', {'file': bad, 'name': 'x'})
    check('  改名也拦 %-18s' % bad, not r.get('ok'), r.get('err'))

# ---------- 收尾：确认没留下测试残留 ----------
st, r = call('/__game/list')
left = [p['file'] for p in r.get('profiles', []) if p['file'].startswith(PREFIX)]
check('跑完自己清干净（没留下测试文件）', left == [], left)
if CFGDIR:
    print('  配置目录：%s' % CFGDIR)

print('\n%s' % ('全部通过' if ok_all else '有失败项，见上面的 FAIL'))
sys.exit(0 if ok_all else 1)
