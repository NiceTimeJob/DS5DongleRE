#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""常驻状态记录器：每 0.5 s 轮询接收器，把状态按时间戳追加到 tools/logs/state_<时间>.log。

记录内容：接收器从 USB 消失/回来（GONE/BACK）；回来时读版本、CRASH 区、DIAG 表头
（启动计数 +1 = 热复位、跳变 = 冷启动）和整个 DIAG 环；输入帧率（突然掉 0 立刻记）；
手柄在线/电量变化；DIAG 新事件；每 10 s 一行心跳。
用法: python -X utf8 watch_state.py [周期秒]   停止: Ctrl+C 或关窗口
注意: 运行期间别开网页配置工具（0xFC 通道同一时刻只能一个客户端）。
"""
import os
import sys
import time
import datetime

import hid
import diag

INTERVAL = float(sys.argv[1]) if len(sys.argv) > 1 else 0.5
HEARTBEAT = 10.0
HERE = os.path.dirname(os.path.abspath(__file__))
LOGDIR = os.path.join(HERE, 'logs')
os.makedirs(LOGDIR, exist_ok=True)
LOGPATH = os.path.join(LOGDIR, 'state_%s.log' % datetime.datetime.now().strftime('%Y%m%d_%H%M%S'))
LOGF = open(LOGPATH, 'a', encoding='utf-8')
NUL = bytes(1)
CRASH_MAGIC = 0xC0DE6116
WEB_MARK = os.path.join(LOGDIR, 'web_active')   # 网页日志页在读接收器时，serve.py 每秒刷新它


def web_active():
    """网页日志页正在读接收器（4 s 内刷新过 web_active）→ 记录器让路：两边同时轮询会抢 0xFC 的响应寄存器。"""
    try:
        return time.time() - os.path.getmtime(WEB_MARK) < 4.0
    except OSError:
        return False


def log(line):
    s = '%s  %s' % (datetime.datetime.now().strftime('%H:%M:%S.%f')[:-3], line)
    print(s, flush=True)
    print(s, file=LOGF, flush=True)


AUDIO_FLAG = {0: '无', 1: '麦克风', 2: '扬声器', 3: '扬声器+麦克风'}


def get_feature(h, rid, ln=63):
    try:
        h.send_feature_report(bytes([rid]) + bytes(ln))
        time.sleep(0.005)
        r = bytes(h.get_feature_report(rid, ln + 1))
        return r[1:] if r and r[0] == rid else r
    except Exception:
        return None


def pad_present():
    for d in hid.enumerate(diag.VID):
        p = d['path'].decode('utf-8', 'replace').lower()
        if (d['usage_page'] == 0x01 and d['usage'] == 0x05
                and d['product_id'] in (0x0CE6, 0x0DF2) and 'rev_0100' not in p):
            return True
    return False


def fmt_rec(t, ev, a1):
    name = diag.EV.get(ev, 'EV%d' % ev)
    if ev == 28:
        x = '复位原因=' + diag.rst_str(a1)
    elif ev == 2:
        x = '%s -> %s' % (diag.STATE.get(a1 & 0xFF, hex(a1 & 0xFF)), diag.STATE.get(a1 >> 8, hex(a1 >> 8)))
    elif ev == 3:
        x = diag.fmt_acl_up(a1)
    elif ev == 4:
        x = diag.fmt_acl_down(a1)
    elif ev == 18:
        x = '无输入看门狗（R1.4 起 10 s）, 此前转发 %d 帧' % a1
    elif ev == 26:
        x = 'USB 软摘除 调用点=%d' % a1
    elif ev == 29:
        x = 'SCANNING 卡死 %d ms, 已强制结束' % a1
    elif ev == 30:
        x = '心跳发现 page scan 没开 (scan_enable=0x%02x), 已补开' % a1
    elif ev == 31:
        x = '主机 USB 挂起' if a1 == 1 else '挂起去抖到期: 手柄关机 + 射频静默'
    elif ev == 32:
        x = '主机 USB 恢复' if a1 == 1 else 'USB 重新配置'
    elif ev == 33:
        x = '本机主动断开! 来源=%s' % diag.DISC_SITE.get(a1, '?%d' % a1)
    elif ev == 36:
        x = '输入停了 >=1 s：RSSI=%d 链路质量=%d AFH 信道=%d/79' % (
            (a1 & 0xFF) - 256 if (a1 & 0x80) else (a1 & 0xFF), (a1 >> 8) & 0xFF, (a1 >> 16) & 0xFF)
    elif ev == 37:
        x = '输入恢复，停了 %d ms' % a1
    elif ev == 38:
        x = diag.fmt_link_param(a1)
    elif ev == 39:
        x = diag.fmt_hci_evt(a1)
    elif ev == 40:
        x = diag.fmt_afh(a1)
    elif ev == 41:
        x = diag.fmt_role_sw(a1)
    elif ev == 35:
        x = '连上后关 page/inquiry scan（射频专心传数据）' if a1 == 1 else '断开后重开 scan'
    elif ev == 34:
        x = {1: '扬声器流开始', 2: '扬声器流结束',
             3: '音频发送连续失败(第 4/10 次时各记一条)',
             4: '音频发送连败超限（R1 起只丢音频、不断链）'}.get(a1, 'a1=%d' % a1)
    else:
        x = 'a1=0x%x' % a1
    return 't=%8d ms  %-14s %s' % (t, name, x)


def read_seq(h):
    d = diag.rpc_read(h, 0x12, 0, 4, tries=30)
    return (d[0] | (d[1] << 8)) if d and len(d) >= 2 else None


def dump_on_open(h, last_boot):
    v = get_feature(h, 0xF8)
    log('  版本 0xF8 = %s' % (v.split(NUL)[0].decode('utf-8', 'replace') if v else '?'))
    cr = diag.fetch_crash(h)
    if cr and cr[0] == CRASH_MAGIC:
        log('  !!! CRASH 区有效: mcause=0x%08x mepc=0x%08x mtval=0x%08x mstatus=0x%08x count=%d boot=%d' % cr[1:])
    elif cr:
        log('  CRASH 区无记录 (magic=0x%08x)' % cr[0])
    pr = diag.parse(diag.fetch_diag(h) or b'')
    if not pr:
        log('  DIAG 读不到')
        return None, 0
    head, boot, recs = pr
    if last_boot is None:
        kind = '首次读取'
    elif boot == last_boot:
        kind = '启动计数未变'
    elif boot - last_boot == 1:
        kind = '+1 => 热复位, RAM 保留 (不是长时间断电)'
    else:
        kind = '跳变 %d => 冷启动 / RAM 丢失' % (boot - last_boot)
    log('  DIAG head=%d boot=%d  %s' % (head, boot, kind))
    for (t, ev, a1, a2) in recs:
        log('    ' + fmt_rec(t, ev, a1))
    prev = diag.fetch_diag_prev(h) if hasattr(diag, 'fetch_diag_prev') else None
    pp = diag.parse(prev) if prev else None
    if pp:
        ph, pb, precs = pp
        log('  上一次开机的 DIAG 快照 (复位前最后 %d 条, 那次 boot=%d, 共写过 %d 条):' % (len(precs), pb, ph))
        for (t, ev, a1, a2) in precs:
            log('    PREV ' + fmt_rec(t, ev, a1))
    else:
        log('  上一次开机的 DIAG 快照: 无 (冷启动 / 固件不支持)')
    return boot, (max(r[0] for r in recs) if recs else 0)


def main():
    log('=== watch_state 启动, 周期 %.2fs, 日志 %s ===' % (INTERVAL, LOGPATH))
    log('=== 网页「日志」页开着并在读接收器时，记录器会自动暂停让路 ===')
    h = None
    last_boot = None
    last_t = 0
    seq_prev = None
    seq_time = 0.0
    rate = None
    pad = None
    perf_prev = None   # S33：(丢块, 发送失败, 看门狗触发) 上一周期的值
    stall_t = None     # 输入掉到 0 时记下的时刻（算停了多久）
    perf_log_t = 0     # 音频异常计数变化最多 10 s 记一行（放音频时每半秒都在变）
    gone_at = None
    was_present = None
    fails = 0
    next_hb = 0.0
    web_paused = False
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
        if present != was_present:
            if present:
                log('### BACK  接收器回到 USB%s' % ((' (消失了 %.1fs)' % (t0 - gone_at)) if gone_at else ''))
            elif was_present is not None:
                gone_at = t0
                log('### GONE  接收器从 USB 消失  (最后帧率=%s 手柄=%s)' % (rate, pad))
            was_present = present
        if not present:
            if h is not None:
                try:
                    h.close()
                except Exception:
                    pass
                h = None
            time.sleep(INTERVAL)
            continue
        if h is None:
            h = diag.open_dev(retries=1)
            if h is None:
                time.sleep(INTERVAL)
                continue
            log('### OPEN  0xFC 通道可用')
            b, last_t = dump_on_open(h, last_boot)
            if b is not None:
                last_boot = b
            seq_prev, rate, fails = None, None, 0
            next_hb = t0 + HEARTBEAT
        seq = read_seq(h)
        if seq is None:
            fails += 1
            if fails == 3:
                log('!!! 连续 3 次 RPC 无应答 (设备还在 USB 上)')
            if fails >= 10:
                log('!!! RPC 持续无应答, 重开设备')
                try:
                    h.close()
                except Exception:
                    pass
                h = None
            time.sleep(INTERVAL)
            continue
        if fails >= 3:
            log('RPC 恢复应答')
        fails = 0
        if seq_prev is not None and t0 > seq_time:
            new_rate = ((seq - seq_prev) & 0xFFFF) / (t0 - seq_time)
            if rate is not None and rate >= 1 and new_rate < 1:
                stall_t = seq_time        # 上一次还看到帧在涨的时刻
                log('!!! 输入帧率掉到 0 (上一周期 %.0f 帧/s)' % rate)
            if (rate is None or rate < 1) and new_rate > 50:
                if stall_t:
                    log('输入帧率恢复 %.0f 帧/s（停了约 %.1f s）' % (new_rate, t0 - stall_t))
                else:
                    log('输入帧率恢复 %.0f 帧/s' % new_rate)
                stall_t = None
            rate = new_rate
        seq_prev, seq_time = seq, t0
        f9 = get_feature(h, 0xF9)
        if f9 and len(f9) >= 13:
            # 0xF9 去掉报告 ID 后：[0] RSSI(int8, 1=未知) [1] 音频标志(0x80 | 扬声器 0x02 | 麦克风 0x01)
            # [11] 电量 %（手柄断开时固件置 0xFF）[12] 电池状态半字节（0 放电 / 1 充电 / 2 满）
            # 2026-10-12 修：旧版把 [1] 当"电量字节"、拿 "[1]!=0xFF" 当"在线"，于是手柄没连也
            # 永远显示"在线 0x80"；日志里出现过的 0x82 其实是"扬声器流在跑"，不是电量。
            lvl, bst, af = f9[11], f9[12], f9[1] & 0x03
            rssi = f9[0] - 256 if f9[0] > 127 else f9[0]
            ps = (lvl != 0xFF, lvl, bst, af, rssi)
            if ps[:4] != (pad[:4] if pad else None):
                log('手柄 %s  电量=%s 电池状态=%d 音频流=%s RSSI=%s' % (
                    '在线' if ps[0] else '离线',
                    ('%d%%' % lvl) if lvl != 0xFF else '?', bst,
                    AUDIO_FLAG.get(af, '?'), rssi if rssi != 1 else '?'))
                pad = ps
        pf = diag.fetch_perf(h)
        if pf:
            key = (pf['overruns'], pf['send_fail'], pf['wd_kills'])
            if perf_prev is not None and key != perf_prev and t0 - perf_log_t >= 10:
                perf_log_t = t0
                log('!!! 音频异常计数变化: 丢块 %d→%d 发送失败 %d→%d 看门狗触发 %d→%d (编码max %.1fms 块max %.1fms)' % (
                    perf_prev[0], key[0], perf_prev[1], key[1], perf_prev[2], key[2],
                    pf['enc_win_us'] / 1000.0, pf['blk_win_us'] / 1000.0))
            perf_prev = key
        pr = diag.parse(diag.fetch_diag(h) or b'')
        if pr:
            head, boot, recs = pr
            if last_boot is not None and boot != last_boot:
                log('!!! 启动计数 %d -> %d 但 USB 没掉' % (last_boot, boot))
                last_boot, last_t = boot, 0
            new = [r for r in recs if r[0] > last_t]
            for (t, ev, a1, a2) in new:
                log('  DIAG ' + fmt_rec(t, ev, a1))
            if new:
                last_t = max(r[0] for r in new)
        if t0 >= next_hb:
            st = diag.rpc_read(h, 0x7F, 0, 16, tries=30)
            log('心跳  帧率=%s  手柄=%s  音频流=%s  %s  boot=%s' % (
                ('%.0f/s' % rate) if rate is not None else '?',
                ('在线 %d%%' % pad[1]) if pad and pad[0] else '离线',
                AUDIO_FLAG.get(pad[3], '?') if pad else '?',
                ('flags=0x%02x' % st[2]) if st else 'STATUS?', last_boot))
            if pf:
                log('  性能 ' + diag.fmt_perf(pf))
            next_hb = t0 + HEARTBEAT
        el = time.time() - t0
        if el < INTERVAL:
            time.sleep(INTERVAL - el)


if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        log('=== watch_state 停止 ===')
