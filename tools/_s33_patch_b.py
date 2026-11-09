# -*- coding: utf-8 -*-
"""S33 补丁 B：usb_gamepad.c / main.c / ds5map.c"""
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _patchlib import P

SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'firmware', 'src')
os.chdir(SRC)

# ---------------- usb_gamepad.c ----------------
p = P('usb_gamepad.c')
p.rep('#define FIRMWARE_VERSION "LCT616-DS5 3.18 S32"', '#define FIRMWARE_VERSION "LCT616-DS5 3.18 S33"')
p.rep('#include "remap.h"\n', '')
p.rep("""static volatile bool usb_remap_save_pending  = false;
static volatile uint8_t usb_remap_save_profile = 0;
static uint8_t remap_read_profile = 0;
""", "")
p.rep("""    0x85, 0xFB,       /*   Report ID (251) — button remap table */
    0x09, 0x3C,
    0x95, 0x7F,       /*   Report Count (127) — 31 entries * 4 + header */
    0xB1, 0x02,
""", "")
p.rep("""    return (report_id >= 0xF6 && report_id <= 0xF9) ||
           report_id == 0xFB || report_id == MAP_RPT_RPC;
#else
    return (report_id >= 0xF6 && report_id <= 0xF9) || report_id == 0xFB;
#endif
""", """    return (report_id >= 0xF6 && report_id <= 0xF9) || report_id == MAP_RPT_RPC;
#else
    return (report_id >= 0xF6 && report_id <= 0xF9);
#endif
""")
p.rep("""    if (usb_remap_save_pending) {
        usb_remap_save_pending = false;
        remap_save_profile(usb_remap_save_profile);
        LOG_INF("[USB] Remap profile %d saved to flash\\n", usb_remap_save_profile);
    }
""", "")
p.rex(r"""#if DS5MAP_GATING_ENABLE
    const bool has_kbd_targets = remap_has_kbd_targets\(\) \|\|
                                 ds5map_has_kbd_outputs\(\) \|\|
                                 ds5map_has_mouse_outputs\(\) \|\|
                                 config_get\(\)->ps_shortcut_enabled;
#else
    const bool has_kbd_targets = remap_has_kbd_targets\(\) \|\|
                                 config_get\(\)->ps_shortcut_enabled \|\|
                                 \(config_get\(\)->tp_mode == 2 \|\| config_get\(\)->tp_mode == 3\);
#endif
""", """    /* S33：原作者的 remap 档案 / PS 快捷键 / 触摸板鼠标模式已删，只看 ds5map 引擎有没有键鼠输出 */
    const bool has_kbd_targets = ds5map_has_kbd_outputs() ||
                                 ds5map_has_mouse_outputs();
""")
p.rex(r"""        \} else if \(report_id == 0xFB\) \{
            feature_resp_buf\[0\] = 0xFB;.*?                    remap_read_profile, \*len\);
""", "")
p.rex(r"""    /\* Button remap command \(0xFB SET\) \*/
    if \(report_type == 0x03 && report_id == 0xFB && payload_len > 0\) \{.*?        return;
    \}

""", "")
p.save()

# ---------------- main.c ----------------
p = P('main.c')
p.rep('#include "remap.h"\n', '')
p.rex(r'#include "diag.h"[^\n]*\n', """#include "diag.h"      /* S6：被拒的脏帧记进诊断环（RPC 可读） */
#include "perf.h"      /* S33：各任务 CPU 占比每秒采样 */
""")
p.rep("""#define USB_DEFERRED_DISC_US  (10ULL * 1000000ULL)
static volatile uint64_t usb_deferred_disc_us = 0;
""", """#define USB_DEFERRED_DISC_US  (10ULL * 1000000ULL)
static volatile uint64_t usb_deferred_disc_us = 0;
/* S33（用户决定）：掉线 10 s 后把 USB 软摘除的原设计关掉。摘除后主机什么都读不到，
 * 恰好把掉线现场藏起来（18:27 那次就是这样丢的）。要恢复原行为把它改成 1。 */
#define USB_DETACH_AFTER_DROP 0
static bool usb_detached = false;   /* USB 当前是否处于软摘除（隐身开机 / 掉线后摘除） */
""")
p.rep("""        if (was_connected && !bt_hid_host_is_switching() &&
            !usb_wake_host_suspended()) {
            usb_deferred_disc_us = bflb_mtimer_get_time_us();
            LOG_INF("[MAIN] USB disconnect deferred (10s timer started)\\n");
        }
""", """#if USB_DETACH_AFTER_DROP
        if (was_connected && !bt_hid_host_is_switching() &&
            !usb_wake_host_suspended()) {
            usb_deferred_disc_us = bflb_mtimer_get_time_us();
            LOG_INF("[MAIN] USB disconnect deferred (10s timer started)\\n");
        }
#endif
""")
p.rep("""        remap_on_disconnect();
""", "")
p.rex(r"""        if \(ever_connected\) \{
            if \(usb_deferred_disc_us\) \{.*?game already has its session\. \*/
            send_led_primer\(\);
        \}
""", """        if (usb_detached) {
            /* USB 处于软摘除（隐身开机 / 掉线后摘除）→ 现在接回去。
             * 让 Windows 先发它的蓝色初始化，再用我们的 primer 盖掉。 */
            diag_log(DIAG_EV_USB_CONN, ever_connected ? 3 : 4, 0);   /* 3=回连后软接回 4=首连隐身接回 */
            usb_soft_connect();
            usb_detached = false;
            stealth_primer_countdown = 5;
            LOG_INF("[MAIN] USB soft connect (was detached, ever_connected=%d)\\n",
                    (int)ever_connected);
        } else {
            /* USB 一直连着（S33 起掉线后不再摘除）：直接发 primer，游戏的会话还在。 */
            if (usb_deferred_disc_us) {
                usb_deferred_disc_us = 0;
                LOG_INF("[MAIN] Deferred disconnect cancelled\\n");
            }
            send_led_primer();
        }
""")
p.rep("""        audio_check_respawn();
""", """        audio_check_respawn();

        {   /* S33：每 ~1 s 采一次各任务 CPU 占比，连同音频统计一起放进 MAP_REG_PERF */
            static uint64_t perf_last_us;
            uint64_t perf_now = bflb_mtimer_get_time_us();
            if (perf_now - perf_last_us >= 1000000ULL) {
                perf_last_us = perf_now;
                perf_cpu_sample();
            }
        }
""")
p.rep("""        if (usb_deferred_disc_us && !ds5_connected &&
            (bflb_mtimer_get_time_us() - usb_deferred_disc_us) > USB_DEFERRED_DISC_US) {
            usb_deferred_disc_us = 0;
            diag_log(DIAG_EV_USB_DISC, 1, 0);   /* 1=10s 未重连，软摘除 */
            usb_soft_disconnect();
            LOG_INF("[MAIN] Deferred USB disconnect (no reconnect within 10s)\\n");
        }
""", """#if USB_DETACH_AFTER_DROP
        if (usb_deferred_disc_us && !ds5_connected &&
            (bflb_mtimer_get_time_us() - usb_deferred_disc_us) > USB_DEFERRED_DISC_US) {
            usb_deferred_disc_us = 0;
            diag_log(DIAG_EV_USB_DISC, 1, 0);   /* 1=10s 未重连，软摘除 */
            usb_soft_disconnect();
            usb_detached = true;
            LOG_INF("[MAIN] Deferred USB disconnect (no reconnect within 10s)\\n");
        }
#endif
""")
p.rep("""        diag_log(DIAG_EV_USB_DISC, 2, 0);   /* 2=开机隐身软摘除 */
        usb_soft_disconnect();
""", """        diag_log(DIAG_EV_USB_DISC, 2, 0);   /* 2=开机隐身软摘除 */
        usb_soft_disconnect();
        usb_detached = true;
""")
p.rex(r"""    /\* PS shortcut state \(short press → Win\+G, with debounce\) \*/
    bool     ps_debounced    = false;.*?    #define CC_VOL_DOWN  0x02   /\* bitmap bit1 = Volume Decrement \*/

""", """    /* S33：原作者的三套按键拦截（PS 短按→Win+G、Options+十字调音量、Create+十字切映射档）
     * 已整体删除：它们在 ds5map 引擎之前吃掉按键，又和引擎抢同一个 USB 键盘端点。
     * 现在手柄报文只经过 ds5map 引擎一处处理。 */

""")
p.rex(r"""            /\* Volume combo: Options\(≡\) \+ D-pad Up/Down → Consumer Control Vol\..*?(?=#if DS5MAP_ENGINE_ENABLE >= 2\n            /\* R3: 只加"每帧处理"这一步 \*/)""", "")
p.rep("""            remap_mouse_tick(raw_report + 2);
            remap_apply(raw_report + 2);
""", "")
p.rex(r"""                /\* PS key scheduled send \+ release \(runs unconditionally\) \*/
                \{.*?                    \} else if \(ps_was_pressed\) \{
                        ps_was_pressed = false;
                        ps_debounced = false;
                    \}
                \}
""", "")
p.rex(r"""        if \(!ds5_connected\) \{
            ps_debounced = false;.*?                ps_key_pending = false;
            \}
        \}

""", "")
p.rep("""    remap_init();
    remap_load();
""", "")
p.save()
for w in ('remap_', 'ps_key', 'vol_key', 'CC_VOL', 'config_ps_shortcut', 'ps_debounced'):
    n = p.s.count(w)
    if n:
        print('  main.c leftover %r x%d' % (w, n))

# ---------------- ds5map.c ----------------
p = P('ds5map.c')
p.rep('#include "remap.h"\n', '#include "perf.h"   /* S33：MAP_REG_PERF 只读视图 */\n')
p.rep("""    remap_get_src_bits(p, src);          /* fills ids 0..30 */
""", """    src_bits(p, src);                    /* ids 0..30：面键/十字键/触摸板手指分区 */
""")
p.rep("""static bool tp_finger(const uint8_t *tp, int16_t *x, int16_t *y)
{
    if (tp[0] & 0x80)
        return false;
    *x = (int16_t)(tp[1] | ((tp[2] & 0x0F) << 8));
    *y = (int16_t)(((tp[2] & 0xF0) >> 4) | (tp[3] << 4));
    return true;
}
""", """static bool tp_finger(const uint8_t *tp, int16_t *x, int16_t *y)
{
    if (tp[0] & 0x80)
        return false;
    *x = (int16_t)(tp[1] | ((tp[2] & 0x0F) << 8));
    *y = (int16_t)(((tp[2] & 0xF0) >> 4) | (tp[3] << 4));
    return true;
}

/* ---- 源位：报文 → src[0..30]（S33 起本文件自带，不再依赖已删除的 remap.c） ----
 *   p[7]: 十字键 hat[3:0]  □[4] ✕[5] ○[6] △[7]
 *   p[8]: L1[0] R1[1] L2[2] R2[3] Create[4] Options[5] L3[6] R3[7]
 *   p[9]: PS[0] 触摸板按下[1] 静音[2]（高 4 位是 DSE 四键，调用方另取）
 *   19..30 触摸板手指分区：手指 1 = 触点槽 0（先按下的那根）、手指 2 = 触点槽 1。
 *   TOUCH = 该手指在板上；上/下/左/右 = 相对板中心、超出死区、按主导轴判定；
 *   CLICK = 该手指在板上且板子被物理按下。
 *   原作者 remap.c 的版本是按左右半区并由他的 tp_mode 配置门控的，tp_mode 默认 0 时这
 *   12 个源位恒为 0 —— 网页上的「手指 1 / 手指 2」按键在 S33 之前其实从来没生效过。 */
static const struct { uint8_t off; uint8_t mask; } BTN_LOC[15] = {
    [MAP_BTN_SQUARE]   = { 7, 0x10 }, [MAP_BTN_CROSS]    = { 7, 0x20 },
    [MAP_BTN_CIRCLE]   = { 7, 0x40 }, [MAP_BTN_TRIANGLE] = { 7, 0x80 },
    [MAP_BTN_L1]       = { 8, 0x01 }, [MAP_BTN_R1]       = { 8, 0x02 },
    [MAP_BTN_L2]       = { 8, 0x04 }, [MAP_BTN_R2]       = { 8, 0x08 },
    [MAP_BTN_CREATE]   = { 8, 0x10 }, [MAP_BTN_OPTIONS]  = { 8, 0x20 },
    [MAP_BTN_L3]       = { 8, 0x40 }, [MAP_BTN_R3]       = { 8, 0x80 },
    [MAP_BTN_PS]       = { 9, 0x01 }, [MAP_BTN_TP_CLICK] = { 9, 0x02 },
    [MAP_BTN_MUTE]     = { 9, 0x04 },
};

static void src_finger_zone(const uint8_t *p, const uint8_t *slot, uint8_t *src, int base)
{
    int16_t x, y;
    for (int i = 0; i < 6; i++)
        src[base + i] = 0;
    if (!tp_finger(slot, &x, &y))
        return;
    src[base + 0] = 1;                                   /* TOUCH */
    src[base + 5] = (p[9] & 0x02) ? 1 : 0;               /* CLICK */
    int dx = (int)x - TP_CENTER_X, dy = (int)y - TP_CENTER_Y;
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    if (adx < TP_DEADZONE && ady < TP_DEADZONE)
        return;
    if (ady >= adx)
        src[base + (dy < 0 ? 1 : 2)] = 1;                /* UP / DOWN */
    else
        src[base + (dx < 0 ? 3 : 4)] = 1;                /* LEFT / RIGHT */
}

static void src_bits(const uint8_t *p, uint8_t *src)
{
    for (int i = 0; i < 15; i++)
        src[i] = (p[BTN_LOC[i].off] & BTN_LOC[i].mask) ? 1 : 0;
    uint8_t hat = p[7] & 0x0F;
    src[MAP_BTN_DPAD_UP]    = (hat == 0 || hat == 1 || hat == 7) ? 1 : 0;
    src[MAP_BTN_DPAD_DOWN]  = (hat == 3 || hat == 4 || hat == 5) ? 1 : 0;
    src[MAP_BTN_DPAD_LEFT]  = (hat == 5 || hat == 6 || hat == 7) ? 1 : 0;
    src[MAP_BTN_DPAD_RIGHT] = (hat == 1 || hat == 2 || hat == 3) ? 1 : 0;
    src_finger_zone(p, &p[32], src, MAP_BTN_TP_F1_TOUCH);
    src_finger_zone(p, &p[36], src, MAP_BTN_TP_F2_TOUCH);
}
""")
p.rep("""    case MAP_REG_VER:       return MAP_VER_SIZE;
""", """    case MAP_REG_VER:       return MAP_VER_SIZE;
    case MAP_REG_PERF:      return PERF_BLOB_SIZE;   /* S33 */
""")
p.rep("""    case MAP_REG_TLM_OUT:   return g_tlm_out;
    default: return NULL;
""", """    case MAP_REG_TLM_OUT:   return g_tlm_out;
    case MAP_REG_PERF:      return perf_blob();       /* S33：纯 RAM 视图，ISR 安全 */
    default: return NULL;
""")
p.save()
print('PATCH B DONE')
