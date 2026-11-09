# -*- coding: utf-8 -*-
"""S33 补丁 C：主机工具 diag.py / watch_state.py（PERF 区 + ACL_UP/ACL_DOWN 新编码）"""
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _patchlib import P

TOOLS = os.path.dirname(os.path.abspath(__file__))
os.chdir(TOOLS)

p = P('diag.py')
p.rep("""REG_DIAG_PREV = 0x16                    # S30+: 上一次开机的 DIAG 快照（布局同 REG_DIAG）
""", """REG_DIAG_PREV = 0x16                    # S30+: 上一次开机的 DIAG 快照（布局同 REG_DIAG）
REG_PERF = 0x17                         # S33+: 性能统计 96 B（src/perf.h 的 perf_blob_t）
PERF_SIZE = 96
PERF_MAGIC = 0x46524550
""")
p.rep("""def fetch_crash(h):
""", """def fetch_perf(h):
    \"\"\"S33：读 96 B 性能块 → dict，固件不支持或 magic 不对返回 None。\"\"\"
    buf = rpc_read(h, REG_PERF, 0, PERF_SIZE)
    if not buf or len(buf) < PERF_SIZE:
        return None
    (magic, up, blocks, over, sfail, enc_max, blk_max, enc_win, blk_win,
     wd, ntask) = struct.unpack_from('<IIIIIHHHHHH', buf, 0)
    if magic != PERF_MAGIC:
        return None
    tasks = []
    for i in range(min(ntask, 8)):
        name = buf[32 + i * 8:32 + i * 8 + 6].split(b'\\0')[0].decode('ascii', 'replace')
        pm = struct.unpack_from('<H', buf, 32 + i * 8 + 6)[0]
        tasks.append((name, pm))
    return dict(uptime_s=up, blocks=blocks, overruns=over, send_fail=sfail,
                enc_max_us=enc_max, blk_max_us=blk_max, enc_win_us=enc_win,
                blk_win_us=blk_win, wd_kills=wd, tasks=tasks)


def fmt_perf(d):
    \"\"\"一行：音频块/丢块/发送失败/看门狗 + 编码耗时 + 各任务 CPU 千分比→百分比。\"\"\"
    cpu = ' '.join('%s=%.1f%%' % (n, pm / 10.0) for n, pm in d['tasks'])
    return ('音频块=%d 丢块=%d 发送失败=%d 看门狗杀=%d 编码max=%.1f/%.1fms 块max=%.1f/%.1fms | CPU %s'
            % (d['blocks'], d['overruns'], d['send_fail'], d['wd_kills'],
               d['enc_win_us'] / 1000.0, d['enc_max_us'] / 1000.0,
               d['blk_win_us'] / 1000.0, d['blk_max_us'] / 1000.0, cpu))


def fmt_acl_up(a1):
    \"\"\"ACL_UP 的 a1：成功 = HCI 句柄；失败 = 0x8000|err，S33 起高 16 位 = 连续主动 page 失败次数。\"\"\"
    if a1 & 0x8000:
        err = a1 & 0xFF
        streak = a1 >> 16
        s = '连接失败 err=0x%02x%s' % (err, ' (page 超时)' if err == 0x04 else '')
        if streak:
            s += ' 连续第 %d 次' % streak
        return s
    return '已连接 handle=0x%x' % a1


def fmt_acl_down(a1):
    \"\"\"ACL_DOWN 的 a1：低 8 位 HCI reason，位 8..15 bonded_count，位 16 = 强拆后补记（S33）。\"\"\"
    r = a1 & 0xFF
    name = {0x08: 'CONN_TIMEOUT 链路超时', 0x13: 'REMOTE_USER 对方主动',
            0x16: 'LOCAL_HOST 本机主动', 0x15: 'REMOTE_POWER_OFF 对方关机',
            0x05: 'AUTH_FAIL', 0x22: 'LMP 响应超时'}.get(r, '')
    s = 'hci_reason=0x%02x %s bonded=%d' % (r, name, (a1 >> 8) & 0xFF)
    if a1 & 0x10000:
        s += ' [强拆后补记]'
    return s


def fetch_crash(h):
""")
p.rep("""        elif ev in (3, 4, 23):
            extra = 'a1=0x%X a2=0x%X' % (a1, a2)
""", """        elif ev == 3:
            extra = fmt_acl_up(a1)
        elif ev == 4:
            extra = fmt_acl_down(a1)
        elif ev == 23:
            extra = 'a1=0x%X a2=0x%X' % (a1, a2)
""")
p.save()

p = P('watch_state.py')
p.rep("""    elif ev == 4:
        x = 'hci_reason=0x%02x bonded=%d' % (a1 & 0xFF, a1 >> 8)
""", """    elif ev == 3:
        x = diag.fmt_acl_up(a1)
    elif ev == 4:
        x = diag.fmt_acl_down(a1)
""")
# 每个周期读一次 PERF：丢块/发送失败/看门狗一变就记；心跳里带 CPU 表
p.rep("""        pr = diag.parse(diag.fetch_diag(h) or b'')
        if pr:
            head, boot, recs = pr
            if last_boot is not None and boot != last_boot:
""", """        pf = diag.fetch_perf(h)
        if pf:
            key = (pf['overruns'], pf['send_fail'], pf['wd_kills'])
            if perf_prev is not None and key != perf_prev:
                log('!!! 音频异常计数变化: 丢块 %d→%d 发送失败 %d→%d 看门狗杀 %d→%d (编码max %.1fms 块max %.1fms)' % (
                    perf_prev[0], key[0], perf_prev[1], key[1], perf_prev[2], key[2],
                    pf['enc_win_us'] / 1000.0, pf['blk_win_us'] / 1000.0))
            perf_prev = key
        pr = diag.parse(diag.fetch_diag(h) or b'')
        if pr:
            head, boot, recs = pr
            if last_boot is not None and boot != last_boot:
""")
p.rep("""                ('flags=0x%02x' % st[2]) if st else 'STATUS?', last_boot))
            next_hb = t0 + HEARTBEAT
""", """                ('flags=0x%02x' % st[2]) if st else 'STATUS?', last_boot))
            if pf:
                log('  性能 ' + diag.fmt_perf(pf))
            next_hb = t0 + HEARTBEAT
""")
p.save()
print('PATCH C DONE')
