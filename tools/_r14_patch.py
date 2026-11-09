# -*- coding: utf-8 -*-
"""R1.4：输入停顿不再 3 s 就拆链。

2026-10-14 17:06 实抓：手柄停发约 4.45 s 后自己恢复了（我们 3 s 强拆后 1.4 s 又收到它的
0x31 输入报告），是 3 s 无输入看门狗把能自愈的链路砍掉了。
  ① 看门狗 3 s → 10 s；
  ② 停顿 0.5 s 先给电脑发一帧"全部松开"（过引擎：映射出去的键盘键/鼠标键也松开）；
  ③ 连上后读一次角色 + 链路监督超时（16:34 那次 0x08 正好在最后一帧后 5.0 s ⇒ LSTO 5 s），
     我们是主就把 LSTO 设成 10 s；结果记 DIAG_EV_LINK_PARAM，角色/LSTO 每 2 s 进 PERF。
所有替换先做完再统一写盘，任何锚点不对整份不落盘。
用法：python tools/_r14_patch.py"""
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _patchlib import P

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
SRC = os.path.join(ROOT, 'firmware', 'src')
files = []

# ======================= version.h =======================
p = P(os.path.join(SRC, 'version.h'))
p.rep('#define FW_VERSION      "R1.3"', '#define FW_VERSION      "R1.4"')
files.append(p)

# ======================= main.c =======================
p = P(os.path.join(SRC, 'main.c'))
p.rep("""#define CONN_WATCHDOG_US (3ULL * 1000000ULL)  /* 3 seconds without input → force disconnect */
""", """/* R1.4：3 s → 10 s。2026-10-14 17:06 实抓：手柄停发约 4.45 s 后自己恢复了（我们 3 s 强拆后
 * 1.4 s 又收到它的输入报告），是这个看门狗把能自愈的链路砍掉了。停顿期间先给电脑发一帧
 * 「全部松开」（STALL_NEUTRAL_US），真死链最多 10 s 后照旧强拆、重开 page scan。 */
#define CONN_WATCHDOG_US (10ULL * 1000000ULL)
#define STALL_NEUTRAL_US (500000ULL)
""")
p.rep("""        /* Connection watchdog: if connected and we've received at least one
         * report, but then nothing for 3s → controller likely powered off.
         * Use force_disconnect to bypass HCI ACK (dead controller can't ACK). */
""", """        /* Connection watchdog: if connected and we've received at least one
         * report, but then nothing for CONN_WATCHDOG_US (R1.4: 10 s) → link is dead.
         * Use force_disconnect to bypass HCI ACK (dead controller can't ACK). */
""")
p.rep("""        if (ds5_connected && usb_fwd_count > 0 &&
            (bflb_mtimer_get_time_us() - last_input_us) > CONN_WATCHDOG_US) {
            LOG_WRN("[MAIN] Watchdog: no input for 3s, force disconnect\\n");
""", """        /* R1.4：停顿超过 STALL_NEUTRAL_US 就把最后一帧改成「全部松开」，过一遍引擎再交给 USB。
         * 不这么做的话 USB 端点一直重发停顿前那一帧：按着的键和扳机一直按着、陀螺仪转视角
         * 一直在转、映射出去的键盘键也一直按着。手柄回来后下一帧照常覆盖；每次停顿只发一次。 */
        {
            static bool neutral_sent;
            uint64_t nn = bflb_mtimer_get_time_us();
            if (nn - last_input_us < STALL_NEUTRAL_US) {
                neutral_sent = false;
            } else if (!neutral_sent && ds5_connected && usb_fwd_count > 0) {
                neutral_sent = true;
                pend_have = false;
                ds5_input_neutralize(pend_raw + 2);
#if DS5MAP_ENGINE_ENABLE >= 2
                ds5map_process(pend_raw + 2, (uint32_t)(nn / 1000ULL));
#endif
                usb_gamepad_send_raw_input(pend_raw + 2);
            }
        }

        if (ds5_connected && usb_fwd_count > 0 &&
            (bflb_mtimer_get_time_us() - last_input_us) > CONN_WATCHDOG_US) {
            LOG_WRN("[MAIN] Watchdog: no input for %us, force disconnect\\n",
                    (unsigned)(CONN_WATCHDOG_US / 1000000ULL));
""")
p.rep("""static void usb_task(void *arg)
{
    (void)arg;
""", """/* R1.4：输入停顿时交给电脑的「全部松开」帧。p 是 63 字节 USB 负载（布局同 Linux
 * hid-playstation.c 的 dualsense_input_report）：摇杆回中、扳机 0、按键全松（十字键 8 =
 * 不按）、陀螺仪角速度清零、两个触点抬起（bit7 = 没摸）。加速度、电量、时间戳保留上一帧的
 * —— 整帧清零会让电脑看到电量 0、重力消失。 */
static void ds5_input_neutralize(uint8_t *p)
{
    p[0] = p[1] = p[2] = p[3] = 0x80;
    p[4] = p[5] = 0;
    p[7] = DS5_DPAD_NONE;
    p[8] = 0;
    p[9] = 0;
    memset(&p[15], 0, 6);
    p[32] |= 0x80;
    p[36] |= 0x80;
}

static void usb_task(void *arg)
{
    (void)arg;
""")
p.rep("""        {
            static uint64_t link_stat_us;
            uint64_t ln = bflb_mtimer_get_time_us();
            if (ds5_connected && ln - link_stat_us >= 2000000ULL) {
                link_stat_us = ln;
                bt_hid_host_read_link_stats();
            }
        }
""", """        {
            static uint64_t link_stat_us;
            static bool     link_param_done;   /* R1.4：每次连上做一次角色 / 监督超时 */
            uint64_t ln = bflb_mtimer_get_time_us();
            if (!ds5_connected) {
                link_param_done = false;
            } else if (ln - link_stat_us >= 2000000ULL) {
                link_stat_us = ln;
                if (!link_param_done) {
                    link_param_done = true;
                    bt_hid_host_link_params();
                }
                bt_hid_host_read_link_stats();
            }
        }
""")
files.append(p)

# ======================= bt_hid_host.c / .h =======================
p = P(os.path.join(SRC, 'bt_hid_host.c'))
p.rep("""static volatile uint8_t s_link_quality, s_afh_used, s_afh_mode;
""", """static volatile uint8_t s_link_quality, s_afh_used, s_afh_mode;
static volatile uint8_t  s_role = 0xFF;   /* R1.4：0 主（Central）/ 1 从 / 0xFF 没读到 */
static volatile uint16_t s_lsto;          /* R1.4：链路监督超时，slot（0.625 ms） */
""")
p.rep("""    perf_link(bt_hid_host_get_cached_rssi(), s_link_quality, s_afh_used, s_afh_mode);
}
""", """    /* R1.4：status, handle(2), role */
    if (hci_status_read(BT_OP(BT_OGF_LINK_POLICY, 0x0009), handle, r, sizeof(r)) >= 4 && r[0] == 0)
        s_role = r[3];
    /* R1.4：status, handle(2), link_supervision_timeout(2) */
    if (hci_status_read(BT_OP(BT_OGF_BASEBAND, 0x0036), handle, r, sizeof(r)) >= 5 && r[0] == 0)
        s_lsto = (uint16_t)(r[3] | (r[4] << 8));
    perf_link(bt_hid_host_get_cached_rssi(), s_link_quality, s_afh_used, s_afh_mode);
    perf_link_param(s_role, s_lsto);
}

/* R1.4：连上后做一次。16:34 那次 ACL_DOWN 0x08 恰好在最后一帧之后 5.0 s ⇒ 这条链路的监督超时
 * 是 5 s（S31 起我们不再写 0x0037，那 5 s 是谁定的要看角色）。我们是主（Central）就设成 10 s
 * （= 无输入看门狗），让 3~10 s 的射频停顿能自愈；是从就只能听手柄的。
 * 不设得更长：新控制器库拆一条死链要等监督超时到点，旧 ACL 没拆完手柄就回不来。
 * DIAG_EV_LINK_PARAM a1 = 连上时的监督超时(slot) | 角色<<16 | 写结果<<24
 *（写结果 0 = 成功 / 0xFE = 没写（不是主或没读到）/ 其它 = 失败的 errno）。 */
#define LINK_LSTO_SLOTS 16000u   /* 10 s / 0.625 ms */

void bt_hid_host_link_params(void)
{
    struct bt_conn *c = hid_ctx.conn;
    if (!c || hid_ctx.state != BT_HID_STATE_CONNECTED)
        return;
    uint16_t handle = c->handle;
    uint8_t  r[8];
    uint8_t  role = 0xFF;
    uint16_t lsto = 0;
    uint8_t  wr = 0xFE;

    if (hci_status_read(BT_OP(BT_OGF_LINK_POLICY, 0x0009), handle, r, sizeof(r)) >= 4 && r[0] == 0)
        role = r[3];
    if (hci_status_read(BT_OP(BT_OGF_BASEBAND, 0x0036), handle, r, sizeof(r)) >= 5 && r[0] == 0)
        lsto = (uint16_t)(r[3] | (r[4] << 8));

    if (role == 0x00 && lsto != LINK_LSTO_SLOTS) {
        struct net_buf *buf = bt_hci_cmd_create(BT_OP(BT_OGF_BASEBAND, 0x0037), 4);
        wr = 0xFF;
        if (buf) {
            net_buf_add_le16(buf, handle);
            net_buf_add_le16(buf, LINK_LSTO_SLOTS);
            struct net_buf *rsp = NULL;
            int err = bt_hci_cmd_send_sync(BT_OP(BT_OGF_BASEBAND, 0x0037), buf, &rsp);
            if (rsp)
                net_buf_unref(rsp);
            wr = err ? (uint8_t)(err < 0 ? -err : err) : 0;
        }
    }
    diag_log(DIAG_EV_LINK_PARAM, (uint32_t)lsto | ((uint32_t)role << 16) | ((uint32_t)wr << 24), 0);
    LOG_INF("[BT] link params: role=%u lsto=%u slots write=%u\\n",
            (unsigned)role, (unsigned)lsto, (unsigned)wr);
}
""")
files.append(p)

p = P(os.path.join(SRC, 'bt_hid_host.h'))
p.rep("""uint32_t bt_hid_host_link_stats_packed(void);
""", """uint32_t bt_hid_host_link_stats_packed(void);
/* R1.4：连上后调一次（task 上下文）：读角色 + 链路监督超时，是主就把监督超时设成 10 s，记 DIAG */
void bt_hid_host_link_params(void);
""")
files.append(p)

# ======================= perf =======================
p = P(os.path.join(SRC, 'perf.h'))
p.rep("""#define PERF_BLOB_SIZE  144   /* S36：96 → 116；R1.1：→ 136；R1.2：→ 140；R1.3：→ 144（链路诊断） */
""", """#define PERF_BLOB_SIZE  148   /* S36：96 → 116；R1.1：→ 136；R1.2：→ 140；R1.3：→ 144（链路诊断）；R1.4：→ 148 */
""")
p.rep("""    uint8_t  afh_mode;         /* 143 R1.3：AFH 模式 0 关 / 1 开 */
} perf_blob_t;
""", """    uint8_t  afh_mode;         /* 143 R1.3：AFH 模式 0 关 / 1 开 */
    uint8_t  role;             /* 144 R1.4：0 主（Central）/ 1 从 / 0xFF 没读到 */
    uint8_t  rsv145;
    uint16_t lsto;             /* 146 R1.4：链路监督超时，slot（0.625 ms）；0 = 没读到 */
} perf_blob_t;
""")
p.rep("""void perf_link(int8_t rssi, uint8_t quality, uint8_t afh_used, uint8_t afh_mode);   /* R1.3 */
""", """void perf_link(int8_t rssi, uint8_t quality, uint8_t afh_used, uint8_t afh_mode);   /* R1.3 */
void perf_link_param(uint8_t role, uint16_t lsto);                                 /* R1.4 */
""")
files.append(p)

p = P(os.path.join(SRC, 'perf.c'))
p.rep("""    g_perf.afh_mode = afh_mode;
}
""", """    g_perf.afh_mode = afh_mode;
}

void perf_link_param(uint8_t role, uint16_t lsto)
{
    g_perf.role = role;
    g_perf.lsto = lsto;
}
""")
files.append(p)

# ======================= diag.h =======================
p = P(os.path.join(SRC, 'diag.h'))
p.rep("""    DIAG_EV_STALL_END,    /* R1.3：停顿后输入恢复；arg1 = 停了多少 ms */
""", """    DIAG_EV_STALL_END,    /* R1.3：停顿后输入恢复；arg1 = 停了多少 ms */
    DIAG_EV_LINK_PARAM,   /* R1.4：连上后一次；arg1 = 监督超时(slot) | 角色<<16 | 写 10 s 的结果<<24 */
""")
files.append(p)

# ======================= 主机工具 =======================
p = P(os.path.join(ROOT, 'tools', 'diag.py'))
p.rep("""    36: 'STALL', 37: 'STALL_END',                                               # R1.3
""", """    36: 'STALL', 37: 'STALL_END',                                               # R1.3
    38: 'LINK_PARAM',                                                           # R1.4
""")
p.rep("""PERF_SIZE = 144                         # S36：96 → 116；R1.1：→ 136；R1.2：→ 140；R1.3：→ 144（链路诊断）。一次 RPC 最多 122 B，分两段读
""", """PERF_SIZE = 148                         # S36：96 → 116；R1.1：→ 136；R1.2：→ 140；R1.3：→ 144（链路诊断）；R1.4：→ 148。一次 RPC 最多 122 B，分两段读
""")
p.rep("""        d.update(rssi=rs, link_quality=lq, afh_used=au, afh_mode=am)
    return d
""", """        d.update(rssi=rs, link_quality=lq, afh_used=au, afh_mode=am)
    if len(buf) >= 148:
        (role, _rsv, lsto) = struct.unpack_from('<BBH', buf, 144)
        d.update(role=role, lsto=lsto)
    return d
""")
p.rep("""                                                  '' if d['afh_mode'] else '(AFH关)')
    return s
""", """                                                  '' if d['afh_mode'] else '(AFH关)')
    if 'lsto' in d:
        s += ' 角色%s 监督超时%s' % ({0: '主', 1: '从'}.get(d['role'], '?') if d['lsto'] else '?',
                                  ('%.2fs' % (d['lsto'] * 0.000625)) if d['lsto'] else '?')
    return s


def fmt_link_param(a1):
    \"\"\"R1.4 LINK_PARAM 的 a1：低 16 位 = 连上时的监督超时（slot），位 16..23 角色，位 24..31 写 10 s 的结果。\"\"\"
    lsto, role, wr = a1 & 0xFFFF, (a1 >> 16) & 0xFF, (a1 >> 24) & 0xFF
    rs = {0: '主(Central)', 1: '从(Peripheral)'}.get(role, '没读到')
    ws = {0: '已改成 10 s', 0xFE: '没改（不是主 / 没读到）'}.get(wr, '改失败 errno=%d' % wr)
    return '角色=%s 连上时监督超时=%s，%s' % (rs, ('%.2f s' % (lsto * 0.000625)) if lsto else '没读到', ws)
""")
p.rep("""        elif ev in (11, 12, 13, 14, 25, 26, 27):
            extra = 'a1=0x%X a2=0x%X' % (a1, a2)
""", """        elif ev in (11, 12, 13, 14, 25, 26, 27):
            extra = 'a1=0x%X a2=0x%X' % (a1, a2)
        elif ev == 38:
            extra = fmt_link_param(a1)
""")
files.append(p)

p = P(os.path.join(ROOT, 'tools', 'watch_state.py'))
p.rep("""        x = '3s 无输入看门狗, 此前转发 %d 帧' % a1
""", """        x = '无输入看门狗（R1.4 起 10 s）, 此前转发 %d 帧' % a1
""")
p.rep("""    elif ev == 37:
        x = '输入恢复，停了 %d ms' % a1
""", """    elif ev == 37:
        x = '输入恢复，停了 %d ms' % a1
    elif ev == 38:
        x = diag.fmt_link_param(a1)
""")
files.append(p)

for f in files:
    f.save()

main_c = files[1].s
for w in ('ds5_input_neutralize(pend_raw + 2)', 'STALL_NEUTRAL_US', 'bt_hid_host_link_params()'):
    assert w in main_c, w
import ast
for t in ('diag.py', 'watch_state.py'):
    ast.parse(open(os.path.join(ROOT, 'tools', t), encoding='utf-8').read())
print('R1.4 PATCH DONE')
