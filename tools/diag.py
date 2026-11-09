#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""DIAG 环日志的正确解析与读取（**唯一权威版本**，其它脚本一律 import 本模块）。

固件侧布局（src/diag.c / diag.h）：
  头 16 B : u32 magic('DIAG'=0x44494147) | u16 ver | u16 head | u32 boot | u32 t0_ms
  每条 **12 B**: u32 t_ms | u8 ev | u8 rsv[3] | u32 a1
               ^0        ^4      ^5..7       ^8

**步长是 12，不是 16** —— 2026-10-11 在 S15 上直接读原始字节核实的：
  `47 41 49 44 0100 0200 …` 头之后，rec0 = `t=3604ms ev=28(复位原因) a1=0(上电)`、
  rec1 = `ev=1(BOOT) a1=boot计数`，按 12 切正好；按 16 切出来是乱的。
  原因：固件里 `diag_rec_t` 结构体**确实有 a2**（`u32 t_ms|u8 ev|u8 rsv[3]|u32 a1|u32 a2`
  = 16 B，diag.c 里那句注释"恰好 12 B"是算错了），但 `diag_read_at()` / `diag_read_rec()`
  只按 `DIAG_REC_SZ`(12) 往上拷 —— **a2 根本没传到主机**，所以线上记录就是 12 B。
  总大小 = 16 + 12 × 25 = **316 B**（`DIAG_SLOTS` = 25）。

之前这份脚本写的是 16/22/368：读最后一块时 off 越界（真机只有 316 B）→ 固件回 ERR
→ 整次读取作废，报"DIAG 读不出来"。别再用那个组合了。

**a2 拿不到**（上面的拷贝长度问题）。要看 `DIAG_EV_STATE` 的"旧状态→新状态"这种
双参数事件，得先改固件：`DIAG_REC_SZ` 12→16、`DIAG_SIZE` 316→416，再同步本文件。
本轮（S15）没动固件，先把主机侧修对。
"""
import struct
import time

import hid

VID = 0x054C
RPC_ID = 0xFC
RPT_LEN = 127
OP_READ = 1
REG_DIAG = 0x14
REG_CRASH = 0x13
REG_STATUS = 0x7F
REG_DIAG_PREV = 0x16                    # S30+: 上一次开机的 DIAG 快照（布局同 REG_DIAG）
REG_PERF = 0x17                         # S33+: 性能统计 96 B（src/perf.h 的 perf_blob_t）
PERF_SIZE = 160                         # S36：96 → 116；R1.1：→ 136；R1.2：→ 140；R1.3：→ 144（链路诊断）；R1.4：→ 148；R1.5：→ 152；R1.6：→ 160。一次 RPC 最多 122 B，分两段读
PERF_MAGIC = 0x46524550

HDR = 16
REC = 12                                # 线上记录是 12 B（a2 没传，见文件头说明）
SLOTS = 25
DIAG_SIZE = HDR + REC * SLOTS           # 16 + 300 = 316 B

EV = {
    0: 'NONE', 1: 'BOOT', 2: 'STATE', 3: 'ACL_UP', 4: 'ACL_DOWN',
    5: 'SECURITY', 6: 'L2CAP_CTRL_UP', 7: 'L2CAP_CTRL_DOWN',
    8: 'L2CAP_INTR_UP', 9: 'L2CAP_INTR_DOWN', 10: 'SDP_DONE',
    11: 'HANDSHAKE', 12: 'HANDSHAKE_OK', 13: 'INPUT_FIRST', 14: 'INPUT',
    15: 'INTR_RECV', 16: 'WD_SEC_OK', 17: 'WD_SEC_FAIL', 18: 'WD_CONN',
    19: 'SCAN', 20: 'PAIRMODE', 21: 'BTN', 22: 'L2CAP_FALLBACK',
    23: 'FORCE_DISC', 24: 'RESET', 25: 'USB_INIT', 26: 'USB_DISC',
    27: 'USB_CONN', 28: 'RST_REASON',
    29: 'SCAN_STUCK', 30: 'HB_SCAN_FIX', 31: 'USB_SUSPEND', 32: 'USB_RESUME',   # S31
    33: 'DISC_REQ', 34: 'AUDIO',                                                # S32
    35: 'SCAN_SET',                                                             # S37
    36: 'STALL', 37: 'STALL_END',                                               # R1.3
    38: 'LINK_PARAM',                                                           # R1.4
    39: 'HCI_EVT', 40: 'AFH', 41: 'ROLE_SW',                                    # R1.5
    42: 'AUTO_OFF',                                                             # R1.7
    43: 'ACT_FIX',                                                              # R1.12：活动计时未来值自愈，a1=脏值物证
}

# S32：本机主动断开的来源编号（DIAG_EV_DISC_REQ 的 a1）
DISC_SITE = {
    1: '板载键单击(仅一个绑定→断开重扫)', 2: '板载键双击(断开+重扫)',
    3: 'CONNECTING 超时 8s', 4: 'L2CAP 超时 3s', 5: '握手超时 10s',
    6: '音频发送连败 50 次（R1 起已停用）', 7: '板载键单击切换手柄', 8: '长按清绑定',
}

# S30 起 RST_REASON 的 a1：低 8 位 = 原因，位 8..15 = HBN 原始复位事件位（bit4 = BOD）。
# S30 之前固件从没调过 bl_sys_rstinfo_init()，a1 恒为 0，那时的"上电"没有信息量。
RST = {0: '上电(保持区丢失=真断电)', 1: '来历不明的热复位(RAM 保留, 无人标记)', 2: '致命异常(EXCEPTION)',
       3: '软件故障钩子(栈溢出/内存耗尽/断言)', 4: '软复位(固件主动)', 5: 'HBN 唤醒',
       6: '掉电检测(BOD)'}


def rst_str(a1):
    """RST_REASON 的 a1 → 可读字符串（兼容 S30 之前只有原因码的格式）。"""
    r = a1 & 0xFF
    evt = (a1 >> 8) & 0xFF
    s = RST.get(r, '未知(%d)' % r)
    if evt:
        s += ' hbn_evt=0x%02x%s' % (evt, ' [BOD]' if evt & 0x10 else '')
    return s

STATE = {0: 'IDLE', 1: 'SCANNING', 2: 'CONNECTING', 3: 'SDP_QUERY',
         4: 'L2CAP_CTRL', 5: 'L2CAP_INTR', 6: 'HANDSHAKE', 7: 'CONNECTED',
         8: 'DISCONNECTING'}

# 曾经拿 DIAG_EV_DSE_MODE 当"通用标记"事件（a1 = 标记值）打过一批启动定位标记，
# MARK 表和对应的解析分支随该枚举一起删除（S15）：固件里已经没有那种调用，
# 留着只会把 RST_REASON（现在编号 28）误读成标记。


def open_dev(retries=5):
    """打开接收器的 Gamepad 口，并验证 0xFC 真的有人应答。

    系统里可能有 reWASD/GeniTech 的虚拟 DualSense（`0x0CE6&REV_0100`，单 HID 口、
    没有 0xFC），所以不能按 PID 猜 —— 逐个口试读 STATUS，谁答应用谁。
    """
    for _ in range(retries):
        cands = [d for d in hid.enumerate(VID)
                 if d['usage_page'] == 0x01 and d['usage'] == 0x05
                 and d['product_id'] in (0x0DF2, 0x0CE6)]

        def score(d):
            p = d['path'].decode('utf-8', 'replace').lower()
            if 'mi_03' in p:
                return 0
            if 'rev_0100' in p:
                return 9
            return 5
        cands.sort(key=score)
        for d in cands:
            try:
                h = hid.device()
                h.open_path(d['path'])
            except Exception:
                continue
            if rpc_read(h, REG_STATUS, 0, 16) is not None:
                return h
            try:
                h.close()
            except Exception:
                pass
        time.sleep(0.4)
    return None


def rpc_read(h, region, off, ln, tries=90):
    tx = bytearray(RPT_LEN)
    tx[0] = OP_READ
    tx[1] = region
    tx[2] = off & 0xFF
    tx[3] = (off >> 8) & 0xFF
    tx[4] = ln
    try:
        if h.send_feature_report(bytes([RPC_ID]) + bytes(tx)) < 0:
            return None
    except Exception:
        return None
    for _ in range(tries):
        time.sleep(0.003)
        try:
            resp = bytes(h.get_feature_report(RPC_ID, RPT_LEN + 1))
        except Exception:
            return None
        body = resp[1:] if resp and resp[0] == RPC_ID else resp
        if not body:
            continue
        if body[0] == 2:          # PENDING：固件还在准备
            continue
        # 标签校验：op / region / off 必须与【本次】请求一致。
        # 不做这一步就会接受上一轮留下的陈旧响应 —— 网页端 protocol.js 的
        # tagMatch 正是干这个的。此前就因为缺这一步读到过乱码、
        # 进而误判设备状态（甚至误覆盖过一个槽位）。
        if (len(body) < 5
                or body[1] != OP_READ
                or body[2] != region
                or (body[3] | (body[4] << 8)) != off):
            continue
        if body[0] != 0:
            return None
        return body[5:5 + ln]
    return None


def rpc(h, op, region, off, data, tries=90):
    """通用 RPC 调用（op: 1=READ, 2=WRITE, 3=COMMIT, 4=ERASE, 5=ACTION）。

    成功返回**响应体**（bytes，`body[0] == 0` 表示 OK），失败/超时返回 None ——
    `set_s2t.py` / `trigger_cal.py` 就是按这个约定写的（`if not res or res[0] != 0`）。
    2026-10-17 补：这两个工具一直在调 `diag.rpc()`，但函数随协议重构消失了 → 一跑就
    AttributeError。补齐后两边恢复可用。
    """
    data = bytes(data or b'')
    tx = bytearray(RPT_LEN)
    tx[0] = op
    tx[1] = region
    tx[2] = off & 0xFF
    tx[3] = (off >> 8) & 0xFF
    tx[4] = len(data)                       # READ 时这里是"要读多少"，WRITE 时是"带了多少"
    for i, b in enumerate(data[:max(0, RPT_LEN - 5)]):
        tx[5 + i] = b
    try:
        if h.send_feature_report(bytes([RPC_ID]) + bytes(tx)) < 0:
            return None
    except Exception:
        return None
    for _ in range(tries):
        time.sleep(0.003)
        try:
            resp = bytes(h.get_feature_report(RPC_ID, RPT_LEN + 1))
        except Exception:
            return None
        body = resp[1:] if resp and resp[0] == RPC_ID else resp
        if not body:
            continue
        if body[0] == 2:                    # PENDING
            continue
        if (len(body) < 5
                or body[1] != op
                or body[2] != region
                or (body[3] | (body[4] << 8)) != off):
            continue                        # 陈旧响应（标签对不上），继续等本次的
        return body
    return None


def rpc_read_units(h, region, total, unit, base=0):
    """按 unit 的整数倍分块读整个区域，返回完整字节串。

    **必须按 unit 的倍数分块**：早期脚本用 122 字节分块（122 不是 5 的倍数），
    块边界会让块内按 unit 步长的解析整体错位，读出来的"槽位"全是假的。
    """
    out = bytearray()
    while len(out) < total:
        n = min(120, total - len(out))
        n -= n % unit                     # 只取 unit 的整数倍
        if n == 0:
            break
        d = rpc_read(h, region, base + len(out), n)
        if not d:
            break
        out += d
    return bytes(out)


def fetch_diag(h, region=REG_DIAG):
    """分块读整块 DIAG（316 B，固件按 off/dlen 支持分块）。region=REG_DIAG_PREV 读上一次开机的快照。"""
    buf = b''
    off = 0
    while off < DIAG_SIZE:
        n = min(122, DIAG_SIZE - off)
        d = rpc_read(h, region, off, n)
        if d is None:
            return None
        if not d:
            break
        buf += d
        off += len(d)
    return buf


def fetch_diag_prev(h):
    """上一次开机的 DIAG 快照（S30+）。返回 None = 固件不支持，或上次是冷启动（表头 magic 无效）。"""
    buf = fetch_diag(h, REG_DIAG_PREV)
    return buf if buf and parse(buf) else None


def fetch_perf(h):
    """S33：读 96 B 性能块 → dict，固件不支持或 magic 不对返回 None。"""
    buf = rpc_read(h, REG_PERF, 0, 116)
    if not buf or len(buf) < 96:          # S35 固件只有 96 B，也能解析
        return None
    if len(buf) >= 116:
        tail = rpc_read(h, REG_PERF, 116, PERF_SIZE - 116)   # R1.1 起才有；老固件回 ERR → None
        if tail:
            buf = bytes(buf) + bytes(tail)
    (magic, up, blocks, over, sfail, enc_max, blk_max, enc_win, blk_win,
     wd, ntask) = struct.unpack_from('<IIIIIHHHHHH', buf, 0)
    if magic != PERF_MAGIC:
        return None
    tasks = []
    for i in range(min(ntask, 8)):
        name = buf[32 + i * 8:32 + i * 8 + 6].split(b'\0')[0].decode('ascii', 'replace')
        pm = struct.unpack_from('<H', buf, 32 + i * 8 + 6)[0]
        tasks.append((name, pm))
    d = dict(uptime_s=up, blocks=blocks, overruns=over, send_fail=sfail,
             enc_max_us=enc_max, blk_max_us=blk_max, enc_win_us=enc_win,
             blk_win_us=blk_win, wd_kills=wd, tasks=tasks)
    if len(buf) >= 116:
        (fp, fl, fn, le, kbps, bw, mode, cx) = struct.unpack_from('<IIIhHBBB', buf, 96)
        d.update(fail_pool=fp, fail_l2cap=fl, fail_noconn=fn, last_err=le,
                 tune_kbps=kbps, tune_bw=bw, tune_mode=mode, tune_cx=cx)
    if len(buf) >= 136:
        (rx, pr, eng, gyro, usb, rxd, pmax, mhz) = struct.unpack_from('<IIHHHHHH', buf, 116)
        d.update(rx_win=rx, proc_win=pr, eng_dus=eng, gyro_dus=gyro, usb_dus=usb,
                 rx_dus=rxd, proc_max_us=pmax, cpu_mhz=mhz)
    if len(buf) >= 140:
        (fw, gw) = struct.unpack_from('<HH', buf, 136)
        d.update(fast_win=fw, gyro_win=gw)
    if len(buf) >= 144:
        (rs, lq, au, am) = struct.unpack_from('<bBBB', buf, 140)
        d.update(rssi=rs, link_quality=lq, afh_used=au, afh_mode=am)
    if len(buf) >= 148:
        (role, _rsv, lsto) = struct.unpack_from('<BBH', buf, 144)
        d.update(role=role, lsto=lsto)
    if len(buf) >= 152:
        (alo, ahi, nev) = struct.unpack_from('<BBH', buf, 148)
        d.update(afh_lo=alo, afh_hi=ahi, hci_evt=nev)
    if len(buf) >= 160:
        (ow, oavg, omax, odrop) = struct.unpack_from('<HHHH', buf, 152)
        d.update(out_win=ow, out_lat_avg_us=oavg, out_lat_max_us=omax, out_drop=odrop)
    return d


def fmt_perf(d):
    """一行：音频块/丢块/发送失败/看门狗 + 编码耗时 + 各任务 CPU 千分比→百分比。"""
    cpu = ' '.join('%s=%.1f%%' % (n, pm / 10.0) for n, pm in d['tasks'])
    s = ('音频块=%d 丢块=%d 发送失败=%d 看门狗触发=%d 编码max=%.1f/%.1fms 块max=%.1f/%.1fms | CPU %s'
         % (d['blocks'], d['overruns'], d['send_fail'], d['wd_kills'],
            d['enc_win_us'] / 1000.0, d['enc_max_us'] / 1000.0,
            d['blk_win_us'] / 1000.0, d['blk_max_us'] / 1000.0, cpu))
    if 'fail_pool' in d:
        s += (' | 失败分类 池空=%d l2cap=%d 未连=%d 最近错=%d | 编码 %dkbps bw=%d mode=%d cx=%d'
              % (d['fail_pool'], d['fail_l2cap'], d['fail_noconn'], d['last_err'],
                 d['tune_kbps'], d['tune_bw'], d['tune_mode'], d['tune_cx']))
    if 'rx_win' in d:
        if d['cpu_mhz']:
            s += (' | 帧 收%d/处理%d 每帧: 引擎%.1fus(陀螺仪%.1f) USB%.1fus 收帧%.1fus 最长%dus @%dMHz'
                  % (d['rx_win'], d['proc_win'], d['eng_dus'] / 10.0, d['gyro_dus'] / 10.0,
                     d['usb_dus'] / 10.0, d['rx_dus'] / 10.0, d['proc_max_us'], d['cpu_mhz']))
        else:
            s += ' | 帧 收%d/处理%d（周期计数器没在跑，分段耗时不可用）' % (d['rx_win'], d['proc_win'])
    if 'fast_win' in d:
        s += ' 快路径%d 陀螺仪%d次' % (d['fast_win'], d['gyro_win'])
    if 'link_quality' in d:
        s += ' | 链路 RSSI%d 质量%d AFH%d/79%s' % (d['rssi'], d['link_quality'], d['afh_used'],
                                                  '' if d['afh_mode'] else '(AFH关)')
    if 'lsto' in d:
        s += ' 角色%s 监督超时%s' % ({0: '主', 1: '从'}.get(d['role'], '?') if d['lsto'] else '?',
                                  ('%.2fs' % (d['lsto'] * 0.000625)) if d['lsto'] else '?')
    if 'afh_lo' in d:
        if d['afh_lo'] != 0xFF:
            s += ' 排除%d~%d(%d~%dMHz)' % (d['afh_lo'], d['afh_hi'], 2402 + d['afh_lo'], 2402 + d['afh_hi'])
        s += ' HCI事件%d' % d['hci_evt']
    if 'out_win' in d:
        s += ' | 输出报告 %d/s 转发延迟 均%.2f/最大%.2fms 排队丢%d' % (
            d['out_win'], d['out_lat_avg_us'] / 1000.0, d['out_lat_max_us'] / 1000.0, d['out_drop'])
    return s


HCI_EVT_NAME = {0x08: '加密变化', 0x10: '控制器硬件错误!', 0x11: 'Flush', 0x12: '角色变化', 0x14: '模式变化',
                0x1A: '缓冲溢出', 0x1B: '最大时隙变化', 0x1D: '包类型变化', 0x1E: 'QoS 违约', 0x30: '密钥刷新',
                0x38: '监督超时变化', 0xFF: '厂商事件'}


def fmt_hci_evt(a1):
    """R1.5 HCI_EVT 的 a1：低 8 位事件码，其上 3 字节按事件挑（见固件 bt_hci_evt_tap）。"""
    ev, b0, b1, b2 = a1 & 0xFF, (a1 >> 8) & 0xFF, (a1 >> 16) & 0xFF, (a1 >> 24) & 0xFF
    name = HCI_EVT_NAME.get(ev, '事件 0x%02X' % ev)
    if ev == 0x12:
        x = 'status=0x%02x 现在是%s' % (b0, {0: '主', 1: '从'}.get(b1, '?'))
    elif ev == 0x14:
        x = 'status=0x%02x 模式=%s 间隔=%s' % (b0, {0: '活跃', 1: 'hold', 2: 'sniff', 3: 'park'}.get(b1, b1),
                                             ('%d slot' % b2) if b2 != 0xFF else '>255 slot')
    elif ev == 0x1B:
        x = '最大时隙=%d' % b0
    elif ev == 0x1D:
        x = 'status=0x%02x 包类型=0x%04x' % (b0, b1 | (b2 << 8))
    elif ev == 0x38:
        x = '%.2f s' % ((b0 | (b1 << 8)) * 0.000625)
    elif ev == 0x08:
        x = 'status=0x%02x 加密=%d' % (b0, b1)
    else:
        x = '%02x %02x %02x' % (b0, b1, b2)
    return '%s %s' % (name, x)


def fmt_afh(a1):
    """R1.5 AFH 的 a1：在用数 | 被排除最低信道<<8 | 最高<<16 | 模式<<24。信道 n = 2402+n MHz。"""
    n, lo, hi, mode = a1 & 0xFF, (a1 >> 8) & 0xFF, (a1 >> 16) & 0xFF, (a1 >> 24) & 0xFF
    if lo == 0xFF:
        return 'AFH 在用 %d/79（全开）%s' % (n, '' if mode else ' AFH关')
    return 'AFH 在用 %d/79，排除的信道在 %d~%d（%d~%d MHz）%s' % (n, lo, hi, 2402 + lo, 2402 + hi,
                                                           '' if mode else ' AFH关')


def fmt_role_sw(a1):
    """R1.5 ROLE_SW 的 a1：原链路策略 | 写策略结果<<16 | 切换命令结果<<24。"""
    pol, ep, es = a1 & 0xFFFF, (a1 >> 16) & 0xFF, (a1 >> 24) & 0xFF
    res = lambda e: {0: '成功', 0xFE: '不需要', 0xFF: '没发出去'}.get(e, '失败 errno=%d' % e)
    return '申请当主：原链路策略=%s 写策略=%s 切换命令=%s（结果看后面的「角色变化」）' % (
        ('0x%04x' % pol) if pol != 0xFFFF else '没读到', res(ep), res(es))


def fmt_link_param(a1):
    """R1.4 LINK_PARAM 的 a1：低 16 位 = 连上时的监督超时（slot），位 16..23 角色，位 24..31 写 10 s 的结果。"""
    lsto, role, wr = a1 & 0xFFFF, (a1 >> 16) & 0xFF, (a1 >> 24) & 0xFF
    rs = {0: '主(Central)', 1: '从(Peripheral)'}.get(role, '没读到')
    ws = {0: '已改成 10 s', 0xFE: '没改（不是主 / 没读到）'}.get(wr, '改失败 errno=%d' % wr)
    return '角色=%s 连上时监督超时=%s，%s' % (rs, ('%.2f s' % (lsto * 0.000625)) if lsto else '没读到', ws)


def fmt_acl_up(a1):
    """ACL_UP 的 a1：成功 = HCI 句柄；失败 = 0x8000|err，S33 起高 16 位 = 连续主动 page 失败次数。"""
    if a1 & 0x8000:
        err = a1 & 0xFF
        streak = a1 >> 16
        s = '连接失败 err=0x%02x%s' % (err, ' (page 超时)' if err == 0x04 else '')
        if streak:
            s += ' 连续第 %d 次' % streak
        return s
    return '已连接 handle=0x%x' % a1


def fmt_acl_down(a1):
    """ACL_DOWN 的 a1：低 8 位 HCI reason，位 8..15 bonded_count，位 16 = 强拆后补记（S33）。"""
    r = a1 & 0xFF
    name = {0x08: 'CONN_TIMEOUT 链路超时', 0x13: 'REMOTE_USER 对方主动',
            0x16: 'LOCAL_HOST 本机主动', 0x15: 'REMOTE_POWER_OFF 对方关机',
            0x05: 'AUTH_FAIL', 0x22: 'LMP 响应超时'}.get(r, '')
    s = 'hci_reason=0x%02x %s bonded=%d' % (r, name, (a1 >> 8) & 0xFF)
    if a1 & 0x10000:
        s += ' [强拆后补记]'
    return s


def fetch_crash(h):
    cr = rpc_read(h, REG_CRASH, 0, 28)
    if not cr or len(cr) < 28:
        return None
    return struct.unpack('<7I', cr[:28])


def parse(buf):
    """-> (head, boot, [(t_ms, ev, a1, a2), ...])，按写入顺序返回最近 25 条。

    a2 恒为 0：固件只把记录的 12 字节（到 a1 为止）拷给主机，a2 没传过来。
    """
    if not buf or len(buf) < HDR:
        return None
    magic, ver, head, boot, t0 = struct.unpack_from('<IHHII', buf, 0)
    if magic != 0x44494147:
        return None
    recs = []
    for n in range(SLOTS):
        o = HDR + n * REC
        if o + REC > len(buf):        # 固件版本更老/块没读全时别越界
            break
        t_ms, ev, r0, r1, r2, a1 = struct.unpack_from('<IBBBBI', buf, o)
        if ev == 0:                   # 空槽（DIAG_EV_NONE）
            continue
        recs.append((t_ms, ev, a1, 0))
    recs.sort(key=lambda r: r[0])
    return head, boot, recs


def fmt(recs):
    out = []
    for t_ms, ev, a1, a2 in recs:
        name = EV.get(ev, 'EV%d' % ev)
        extra = ''
        if ev == 28:
            extra = '复位原因 = %s' % rst_str(a1)
        elif ev == 2:
            # 固件把**新旧状态打包进 a1**：a1 = 旧 | (新 << 8)，a2 恒 0
            #（`bt_hid_host.c` 的 diag_log；diag.h 里"arg1=旧状态, arg2=新状态"的注释是旧的，
            # 而且 a2 根本传不到主机）。watch_state.py:72 与 page-log.js:123 一直是这么解的，
            # 只有这里以前按 a1/a2 解 —— 于是 "IDLE -> SCANNING" 被打成 "256 -> IDLE"。
            extra = '%s -> %s' % (STATE.get(a1 & 0xFF, hex(a1 & 0xFF)),
                                  STATE.get((a1 >> 8) & 0xFF, hex((a1 >> 8) & 0xFF)))
        elif ev == 3:
            extra = fmt_acl_up(a1)
        elif ev == 4:
            extra = fmt_acl_down(a1)
        elif ev == 23:
            extra = 'a1=0x%X a2=0x%X' % (a1, a2)
        elif ev in (11, 12, 13, 14, 25, 26, 27):
            extra = 'a1=0x%X a2=0x%X' % (a1, a2)
        elif ev == 38:
            extra = fmt_link_param(a1)
        elif ev == 39:
            extra = fmt_hci_evt(a1)
        elif ev == 40:
            extra = fmt_afh(a1)
        elif ev == 41:
            extra = fmt_role_sw(a1)
        elif ev == 42:
            extra = '手柄省电关机：%d 分钟没碰手柄，发关机命令' % a1
        elif ev == 43:
            extra = '活动计时抓到未来值 0x%08X（RAM 野写物证），已自愈、未误关机' % a1
        out.append('  t=%7d ms  %-13s %s' % (t_ms, name, extra))
    return '\n'.join(out)


def main():
    import sys
    loop = int(sys.argv[1]) if len(sys.argv) > 1 else 1
    for k in range(loop):
        h = open_dev()
        if h is None:
            print('[!] 接收器不在')
            return
        cr = fetch_crash(h)
        if cr:
            magic, mcause, mepc, mtval, mstatus, count, boot = cr
            if magic == 0xC0DE6116:
                print('CRASH: mcause=0x%08x mepc=0x%08x mtval=0x%08x count=%d boot=%d'
                      % (mcause, mepc, mtval, count, boot))
            else:
                print('CRASH: 无有效记录 (magic=0x%08x)' % magic)
        buf = fetch_diag(h)
        h.close()
        parsed = parse(buf) if buf else None
        if not parsed:
            print('[!] DIAG 读不出来')
            return
        head, boot, recs = parsed
        print('=== DIAG head=%d boot=%d，最近 %d 条 ===' % (head, boot, len(recs)))
        print(fmt(recs))
        if k + 1 < loop:
            time.sleep(2)


if __name__ == '__main__':
    main()
