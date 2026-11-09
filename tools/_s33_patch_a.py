# -*- coding: utf-8 -*-
"""S33 补丁 A：config.c / ds5map.h / FreeRTOSConfig.h / CMakeLists.txt / ds5_usb_audio.c / audio.c / bt_hid_host.c"""
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _patchlib import P

SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'firmware', 'src')
os.chdir(SRC)

p = P('config.c')
p.rep("""    b->led_b             = 0xFF;
    b->tp_mode           = 0;     /* off */
    b->tp_mode_enabled_mask = 0x03;
    b->tp_mouse_sensitivity = 8;
}
""", """    b->led_b             = 0xFF;
}
""")
p.rep("""    if (b->ps_shortcut_enabled > 1)
        b->ps_shortcut_enabled = 0;
""", "")
p.rep("""    if (b->tp_mode > 4)
        b->tp_mode = 0;
    if (b->tp_mode_enabled_mask == 0 || b->tp_mode_enabled_mask > 0x1F)
        b->tp_mode_enabled_mask = 0x01; /* at least mode 0 enabled */
    if (b->tp_mouse_sensitivity == 0 || b->tp_mouse_sensitivity > 32)
        b->tp_mouse_sensitivity = 8;
""", "")
p.rex(r"""    /\* 不再有老布局的迁移：.*?\*/
    config_validate\(\);
    LOG_INF\("\[CFG\] Loaded: wake=%d led_off=%d poll=%d ps=%d\\n",
           cfg\.enable_wake, cfg\.disable_led,
           cfg\.polling_rate_mode, cfg\.ps_shortcut_enabled\);
    LOG_INF\("\[CFG\] tp_mode=%u mask=0x%02x sens=%u \(rlen=%u\)\\n",
            cfg\.tp_mode, cfg\.tp_mode_enabled_mask, cfg\.tp_mouse_sensitivity,
            \(unsigned\)rlen\);
""", """    /* 不再有老布局的迁移：结构体自带版本号（现 22 字节 / v6，S33 删掉原作者的四个字段）。
     * 老配置由下面的版本号检查拦掉，整份回落默认值 —— 不认识的就别猜。 */
    config_validate();
    LOG_INF("[CFG] Loaded: wake=%d led_off=%d poll=%d (rlen=%u)\\n",
           cfg.enable_wake, cfg.disable_led,
           cfg.polling_rate_mode, (unsigned)rlen);
""")
p.save()

p = P('ds5map.h')
p.rep("""    MAP_REG_DIAG_PREV = 0x16, /* 上一次开机的 DIAG 环快照（只读，316 B，布局同 0x14；S30） */
""", """    MAP_REG_DIAG_PREV = 0x16, /* 上一次开机的 DIAG 环快照（只读，316 B，布局同 0x14；S30） */
    MAP_REG_PERF      = 0x17, /* S33：性能统计（只读，96 B，布局见 perf.h）：音频丢块/编码耗时/各任务 CPU */
""")
p.rep("""     * （id 与 remap.h 的 REMAP_BTN_TP_F1_xxx / F2_xxx 一一对应） */
""", """     * （源位由 ds5map.c 的 src_bits() 直接从报文算；S33 起不再依赖已删除的 remap.c） */
""")
p.save()

p = P('FreeRTOSConfig.h')
p.rep("""#define configGENERATE_RUN_TIME_STATS           0
""", """/* S33：各任务 CPU 占比（perf.c 每秒采样 uxTaskGetSystemState）。计数器用 mtimer 微秒值。 */
#define configGENERATE_RUN_TIME_STATS           1
#ifndef __ASSEMBLER__
#include <stdint.h>
extern uint32_t perf_runtime_counter(void);
#endif
#define portCONFIGURE_TIMER_FOR_RUN_TIME_STATS()
#define portGET_RUN_TIME_COUNTER_VALUE()        perf_runtime_counter()
""")
p.save()

p = P('../CMakeLists.txt')
p.rep("""    src/remap.c
""", """    src/perf.c
""")
p.save()

p = P('ds5_usb_audio.c')
p.rep("""#include <string.h>
""", """#include <string.h>
#include "perf.h"   /* S33：PCM 块被覆盖（音频任务没来得及取走）计数 */
""")
p.rep("""            pcm_write_pos = 0;
            pcm_write_idx ^= 1;
            pcm_ready = true;
""", """            pcm_write_pos = 0;
            pcm_write_idx ^= 1;
            if (pcm_ready)
                perf_audio_overrun();   /* 上一块还没被 usb_audio_read 取走就被顶掉了 */
            pcm_ready = true;
""")
p.save()

p = P('audio.c')
p.rex(r'#include "diag.h"[^\n]*\n', """#include "diag.h"   /* S32：音频流起止与发送连败记进诊断环 */
#include "perf.h"   /* S33：每块处理耗时 / 编码最长耗时 / 发送失败数 / 看门狗击杀数 */
""")
p.rep("""    if (audio_task_handle) {
        vTaskSuspend(audio_task_handle);
    }

    encoding_in_progress = false;
""", """    if (audio_task_handle) {
        vTaskSuspend(audio_task_handle);
    }
    perf_audio_wdkill();

    encoding_in_progress = false;
""")
p.rep("""    if (ret) {
        fail_log_count++;
""", """    if (ret) {
        perf_audio_sendfail();
        fail_log_count++;
""")
p.rep("""                uint8_t ah_mode = config_get()->audio_haptic;
""", """                uint8_t ah_mode = config_get()->audio_haptic;
                uint64_t blk_t0 = bflb_mtimer_get_time_us();   /* S33 */
                uint32_t enc_max = 0;
""")
p.rep("""                        encoding_in_progress = false;

                        if (encoder_reset_pending) {
""", """                        encoding_in_progress = false;
                        {
                            uint32_t e_us = (uint32_t)(bflb_mtimer_get_time_us() - encode_start_us);
                            if (e_us > enc_max)
                                enc_max = e_us;
                        }

                        if (encoder_reset_pending) {
""")
p.rep("""                if (send_audio_report() != 0) {
""", """                int send_rc = send_audio_report();
                perf_audio_block((uint32_t)(bflb_mtimer_get_time_us() - blk_t0), enc_max);
                if (send_rc != 0) {
""")
p.save()

p = P('bt_hid_host.c')
p.rep("""static void set_state(enum bt_hid_host_state new_state)
{
    LOG_INF("[BT-FSM] %s -> %s\\n", state_name(hid_ctx.state), state_name(new_state));
    /* a1 低 8 位=旧状态，高 8 位=新状态（a2 传不到主机，打包进 a1） */
    diag_log(DIAG_EV_STATE, (uint32_t)hid_ctx.state | ((uint32_t)new_state << 8), 0);
""", """/* S33：连续主动 page 失败的次数。等待回连时每 ~9 s 一轮 IDLE→CONNECTING→(超时)→IDLE 会往
 * 25 格的 DIAG 环里写 3 条，75 s 就把掉线现场全冲掉。前 3 轮照记，之后只在第 20/40/… 轮
 * 记一次（ACL_UP 的 a1 高 16 位 = 累计失败次数）。任何一次连上就清零。 */
static uint16_t page_fail_streak;

static void set_state(enum bt_hid_host_state new_state)
{
    LOG_INF("[BT-FSM] %s -> %s\\n", state_name(hid_ctx.state), state_name(new_state));
    /* a1 低 8 位=旧状态，高 8 位=新状态（a2 传不到主机，打包进 a1） */
    bool page_cycle = (hid_ctx.state == BT_HID_STATE_IDLE && new_state == BT_HID_STATE_CONNECTING) ||
                      (hid_ctx.state == BT_HID_STATE_CONNECTING && new_state == BT_HID_STATE_IDLE);
    bool quiet = page_cycle && page_fail_streak > 3 && (page_fail_streak % 20) != 0;
    if (!quiet)
        diag_log(DIAG_EV_STATE, (uint32_t)hid_ctx.state | ((uint32_t)new_state << 8), 0);
""")
p.rep("""    if (err) {
        LOG_ERR("[BT] Connection failed: 0x%02x\\n", err);
        diag_log(DIAG_EV_ACL_UP, 0x8000u | err, 2);   /* 2=连接建立失败 */
""", """    if (err) {
        LOG_ERR("[BT] Connection failed: 0x%02x\\n", err);
        if (hid_ctx.outgoing && page_fail_streak < 0xFFFF)
            page_fail_streak++;                        /* S33：主动 page 又没连上 */
        if (page_fail_streak <= 3 || (page_fail_streak % 20) == 0)
            diag_log(DIAG_EV_ACL_UP, 0x8000u | err | ((uint32_t)page_fail_streak << 16), 2);   /* 2=连接建立失败 */
""")
p.rep("""        LOG_INF("[BT] Re-enabled page scan after failed connection\\n");
        set_state(BT_HID_STATE_IDLE);
        return;
    }
""", """        LOG_INF("[BT] Re-enabled page scan after failed connection\\n");
        set_state(BT_HID_STATE_IDLE);
        return;
    }
    page_fail_streak = 0;   /* S33：连上了（主动或被动），安静模式解除 */
""")
p.rep("""    if (conn != hid_ctx.conn) {
        LOG_INF("[BT] Old ACL teardown complete (unowned conn, reason 0x%02x, "
               "state=%d)\\n", reason, hid_ctx.state);
""", """    if (conn != hid_ctx.conn) {
        LOG_INF("[BT] Old ACL teardown complete (unowned conn, reason 0x%02x, "
               "state=%d)\\n", reason, hid_ctx.state);
        /* S33：force_disconnect 先把 hid_ctx.conn 置空，之后 HCI 断开完成只会走到这里，
         * 以前这条分支不记 DIAG，于是「3 s 无输入 → 强拆」之后的真实原因码永远看不到。
         * a1 位 16 = 1 标记"强拆后补记"，低 8 位仍是 HCI reason。 */
        diag_log(DIAG_EV_ACL_DOWN, (uint32_t)reason | ((uint32_t)bonded_count << 8) | 0x10000u, 0);
""")
p.save()
print('PATCH A DONE')
