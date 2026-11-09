# -*- coding: utf-8 -*-
"""R1 热修（2026-10-14 中午，用户贴出 10:53 的日志后）：
  ① 拆链竞态导致的空指针复位：hid_ctx.conn 由 conn_take() 在临界区里原子摘取，
     只有摘到的人才 unref；force_disconnect 不可重入。
  ② 音频发送连败 50 次不再拆链（只记一条、退避），链路死活只由 3 s 无输入看门狗判定。
  ③ 复位前把 D-cache 刷回 RAM（异常处理、故障钩子、主机命令复位）：10:53 那次 RST_REASON=EXCEPTION
     却读不到 CRASH 现场，就是现场还在 D-cache 里、复位后 RAM 里还是旧值。
用法：python tools/_r1_hotfix_patch.py"""
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _patchlib import P

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
SRC = os.path.join(ROOT, 'firmware', 'src')

# ======================= bt_hid_host.c =======================
p = P(os.path.join(SRC, 'bt_hid_host.c'))

p.rep("""static void conn_disconnected(struct bt_conn *conn, uint8_t reason)
{
""", """/* R1：拆链路径的竞态修复（2026-10-14 10:53:41 实抓整机复位，RST_REASON=EXCEPTION）。
 * 链路先静默断掉 → 音频任务的优雅断开、usb 任务的 3 s 无输入看门狗、bt 任务的
 * "DISCONNECTING 卡 1.5 s" 三条路前后脚都来拆同一条链；后两条进 force_disconnect，
 * 都卡在 bt_conn_disconnect 的 HCI 同步命令上。断开完成事件一到，BT 线程的
 * conn_disconnected 把 hid_ctx.conn unref 并置空，卡住的 force_disconnect 醒来再
 * bt_conn_unref(hid_ctx.conn) —— 读到 NULL → 空指针异常 → 复位（USB 也跟着掉）。
 * 现在：谁拆链谁先在临界区里把指针摘走，只有摘到的人才 unref；expect 非空时只摘这一条。 */
static struct bt_conn *conn_take(struct bt_conn *expect)
{
    struct bt_conn *c = NULL;
    taskENTER_CRITICAL();
    if (hid_ctx.conn && (expect == NULL || hid_ctx.conn == expect)) {
        c = hid_ctx.conn;
        hid_ctx.conn = NULL;
    }
    taskEXIT_CRITICAL();
    return c;
}

static void conn_disconnected(struct bt_conn *conn, uint8_t reason)
{
""")
p.rep("""    if (conn != hid_ctx.conn) {
        LOG_INF("[BT] Old ACL teardown complete (unowned conn, reason 0x%02x, "
""", """    struct bt_conn *mine = conn_take(conn);   /* R1：原子认领；没认领到 = 别人已经拆过 */
    if (!mine) {
        LOG_INF("[BT] Old ACL teardown complete (unowned conn, reason 0x%02x, "
""")
p.rep("""    diag_log(DIAG_EV_ACL_DOWN, (uint32_t)reason | ((uint32_t)bonded_count << 8), 0);

    bt_conn_unref(hid_ctx.conn);
    hid_ctx.conn = NULL;
""", """    diag_log(DIAG_EV_ACL_DOWN, (uint32_t)reason | ((uint32_t)bonded_count << 8), 0);

    bt_conn_unref(mine);
""")

# bt_hid_host_disconnect：只读一次指针
p.rep("""    if (hid_ctx.conn) {
        set_state(BT_HID_STATE_DISCONNECTING);
        bt_conn_disconnect(hid_ctx.conn,
                           BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    }
""", """    struct bt_conn *c = hid_ctx.conn;   /* R1：只读一次，别在检查和使用之间被别的线程置空 */
    if (c) {
        set_state(BT_HID_STATE_DISCONNECTING);
        bt_conn_disconnect(c, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    }
""")

# L2CAP 断开回调：只读一次指针
p.rep("""    diag_log(DIAG_EV_L2CAP_CTRL_DOWN, hid_ctx.intr_connected, 0);
    if (!hid_ctx.intr_connected && hid_ctx.conn) {
        set_state(BT_HID_STATE_DISCONNECTING);
        bt_conn_disconnect(hid_ctx.conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    }
""", """    diag_log(DIAG_EV_L2CAP_CTRL_DOWN, hid_ctx.intr_connected, 0);
    struct bt_conn *c = hid_ctx.conn;   /* R1：只读一次 */
    if (!hid_ctx.intr_connected && c) {
        set_state(BT_HID_STATE_DISCONNECTING);
        bt_conn_disconnect(c, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    }
""")
p.rep("""    diag_log(DIAG_EV_L2CAP_INTR_DOWN, hid_ctx.ctrl_connected, 0);
    if (!hid_ctx.ctrl_connected && hid_ctx.conn) {
        set_state(BT_HID_STATE_DISCONNECTING);
        bt_conn_disconnect(hid_ctx.conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    }
""", """    diag_log(DIAG_EV_L2CAP_INTR_DOWN, hid_ctx.ctrl_connected, 0);
    struct bt_conn *c = hid_ctx.conn;   /* R1：只读一次 */
    if (!hid_ctx.ctrl_connected && c) {
        set_state(BT_HID_STATE_DISCONNECTING);
        bt_conn_disconnect(c, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    }
""")

# force_disconnect：不可重入 + 先摘指针再做阻塞调用
p.rep("""void bt_hid_host_force_disconnect(void)
{
    LOG_WRN("[BT] Force disconnect — HID cleanup, conn=%p\\n", hid_ctx.conn);
    diag_log(DIAG_EV_FORCE_DISC, hid_ctx.state, 0);
""", """static volatile bool s_force_busy;   /* R1：force_disconnect 正在执行（可能卡在 HCI 同步命令上） */

void bt_hid_host_force_disconnect(void)
{
    bool busy;
    taskENTER_CRITICAL();
    busy = s_force_busy;
    s_force_busy = true;
    taskEXIT_CRITICAL();
    if (busy) {
        /* 另一个任务正在拆（10:53 就是 usb 任务看门狗和 bt 任务兜底同时进来）：别重复拆 */
        diag_log(DIAG_EV_FORCE_DISC, (uint32_t)hid_ctx.state | 0x100u, 0);
        return;
    }

    LOG_WRN("[BT] Force disconnect — HID cleanup, conn=%p\\n", hid_ctx.conn);
    diag_log(DIAG_EV_FORCE_DISC, hid_ctx.state, 0);
""")
p.rep("""    if (hid_ctx.conn) {
        LOG_INF("[BT] Sending HCI Disconnect for ACL cleanup\\n");
        bt_conn_disconnect(hid_ctx.conn, 0x13);
        bt_conn_unref(hid_ctx.conn);
        hid_ctx.conn = NULL;
    }
""", """    {
        struct bt_conn *c = conn_take(NULL);   /* R1：先摘指针，再做会阻塞的 HCI 调用 */
        if (c) {
            LOG_INF("[BT] Sending HCI Disconnect for ACL cleanup\\n");
            bt_conn_disconnect(c, 0x13);
            bt_conn_unref(c);
        }
    }
""")
p.rep("""        LOG_INF("[BT] Fallback: outgoing connect pending (waiting for ACL teardown)\\n");
    }
}
""", """        LOG_INF("[BT] Fallback: outgoing connect pending (waiting for ACL teardown)\\n");
    }
    s_force_busy = false;
}
""")

# drop_stale_conn
p.rep("""void bt_hid_host_drop_stale_conn(void)
{
    if (!hid_ctx.conn)
        return;

    LOG_WRN("[BT] conn_disconnected never arrived — force cleanup\\n");

    bt_conn_unref(hid_ctx.conn);
    hid_ctx.conn = NULL;
""", """void bt_hid_host_drop_stale_conn(void)
{
    struct bt_conn *c = conn_take(NULL);   /* R1：原子摘取 */
    if (!c)
        return;

    LOG_WRN("[BT] conn_disconnected never arrived — force cleanup\\n");

    bt_conn_unref(c);
""")

# clear_bonds
p.rep("""    if (hid_ctx.conn) {
        diag_log(DIAG_EV_DISC_REQ, BT_DISC_SITE_CLEAR_BONDS, (uint32_t)hid_ctx.state);   /* S32 */
        bt_conn_disconnect(hid_ctx.conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        bt_conn_unref(hid_ctx.conn);
        hid_ctx.conn = NULL;
    }
""", """    {
        struct bt_conn *c = conn_take(NULL);   /* R1：原子摘取，再做阻塞的 HCI 调用 */
        if (c) {
            diag_log(DIAG_EV_DISC_REQ, BT_DISC_SITE_CLEAR_BONDS, (uint32_t)hid_ctx.state);   /* S32 */
            bt_conn_disconnect(c, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
            bt_conn_unref(c);
        }
    }
""")
p.save()
for w in ('bt_conn_unref(hid_ctx.conn)',):
    n = p.s.count(w)
    print('  bt_hid_host.c remaining %r x%d' % (w, n))
    assert n == 0

p = P(os.path.join(SRC, 'bt_hid_host.h'))
p.rep("""    BT_DISC_SITE_AUDIO_SENDFAIL     = 6,  /* audio.c 音频连续 50 次发送失败 */
""", """    BT_DISC_SITE_AUDIO_SENDFAIL     = 6,  /* audio.c 音频连续 50 次发送失败（R1 起不再拆链，编号保留） */
""")
p.save()

# ======================= audio.c =======================
p = P(os.path.join(SRC, 'audio.c'))
p.rep("""                    if (send_fail_streak >= AUDIO_SEND_FAIL_MAX) {
                        LOG_ERR("[AUDIO] %u consecutive send failures, "
                                "forcing disconnect\\n",
                                (unsigned)send_fail_streak);
                        diag_log(DIAG_EV_AUDIO, 4, send_fail_streak);   /* S32：连败超限，要断链了 */
                        send_fail_streak = 0;
                        bt_hid_host_disconnect_site(BT_DISC_SITE_AUDIO_SENDFAIL);
                    } else if (send_fail_streak > 3) {
""", """                    if (send_fail_streak >= AUDIO_SEND_FAIL_MAX) {
                        /* R1：不再因为音频发不出去就拆链。2026-10-14 10:53 实抓：链路先静默断掉
                         * （手柄不再发输入、发送缓冲池耗尽），音频连败 50 次只是后果；它再去拆链，
                         * 和 3 s 无输入看门狗、DISCONNECTING 兜底撞在一起触发了空指针复位。
                         * 链路死活只由 usb 任务的 3 s 无输入看门狗判定；这里只记一条（30 s 限流）、退避。 */
                        LOG_ERR("[AUDIO] %u consecutive send failures, dropping audio\\n",
                                (unsigned)send_fail_streak);
                        {
                            static uint64_t last_max_diag_us;
                            uint64_t nowm = bflb_mtimer_get_time_us();
                            if (nowm - last_max_diag_us >= 30000000ULL) {
                                last_max_diag_us = nowm;
                                diag_log(DIAG_EV_AUDIO, 4, send_fail_streak);
                            }
                        }
                        send_fail_streak = 0;
                        vTaskDelay(pdMS_TO_TICKS(200));
                    } else if (send_fail_streak > 3) {
""")
p.save()

# ======================= fault_hooks.c / usb_gamepad.c：复位前刷 D-cache =======================
p = P(os.path.join(SRC, 'fault_hooks.c'))
p.rep("""#include "bflb_irq.h"
""", """#include "bflb_irq.h"
#include "bflb_l1c.h"   /* R1：复位前把 D-cache 刷回 RAM */
""")
p.rep("""    bl_sys_rstinfo_set(BL_RST_SOFTWARE_WATCHDOG);
    (void)bflb_irq_save();
    GLB_SW_System_Reset();
""", """    bl_sys_rstinfo_set(BL_RST_SOFTWARE_WATCHDOG);
    (void)bflb_irq_save();
    bflb_l1c_dcache_clean_all();   /* R1：现场还在 D-cache 里，不刷回 RAM 复位后就没了 */
    GLB_SW_System_Reset();
""")
p.save()

p = P(os.path.join(SRC, 'usb_gamepad.c'))
p.rex(r'#include "bl616_sys.h"[^\n]*\n', '#include "bl616_sys.h"   /* S30: bl_sys_rstinfo_set */\n#include "bflb_l1c.h"     /* R1：复位前把 D-cache 刷回 RAM */\n')
p.rep("""        vTaskDelay(pdMS_TO_TICKS(100));
        GLB_SW_System_Reset();
""", """        vTaskDelay(pdMS_TO_TICKS(100));
        bflb_l1c_dcache_clean_all();   /* R1：DIAG 环在 D-cache 里，复位前刷回 RAM */
        GLB_SW_System_Reset();
""")
p.save()

# ======================= SDK interrupt.c（现用 SDK + 2.3.32 worktree） =======================
for sdk in (r'E:/BL616_Project/ds5dongle-bl618-opensource-3.18/bouffalo_sdk',
            r'E:/BL616_Project/sdk_v2.3.32'):
    f = sdk + '/drivers/soc/bl616/std/startup/interrupt.c'
    if not os.path.exists(f):
        print('skip (missing):', f)
        continue
    p = P(f)
    if 'bflb_l1c_dcache_clean_all' in p.s:
        print('already patched:', f)
        continue
    p.rep("""    bl_sys_rstinfo_set(BL_RST_FATAL_EXCEPTION);
    __disable_irq();
    GLB_SW_System_Reset();
""", """    bl_sys_rstinfo_set(BL_RST_FATAL_EXCEPTION);
    __disable_irq();
    {
        /* DongleRE R1：现场与 DIAG 环还在 D-cache 里（0x62xx_xxxx 是可缓存别名），
         * 不刷回 RAM 的话复位后读到的是旧值（2026-10-14 实抓：RST_REASON=EXCEPTION 但 CRASH 区空）。 */
        extern void bflb_l1c_dcache_clean_all(void);
        bflb_l1c_dcache_clean_all();
    }
    GLB_SW_System_Reset();
""")
    p.save()

# ======================= 主机工具标签 =======================
TOOLS = os.path.join(ROOT, 'tools')
p = P(os.path.join(TOOLS, 'diag.py'))
p.rep("6: '音频发送连败 50 次'", "6: '音频发送连败 50 次（R1 起已停用）'")
p.save()
p = P(os.path.join(TOOLS, 'watch_state.py'))
p.rep("4: '音频发送连败超限 -> 固件强制断链'", "4: '音频发送连败超限（R1 起只丢音频、不断链）'")
p.save()
print('R1 HOTFIX PATCH DONE')
