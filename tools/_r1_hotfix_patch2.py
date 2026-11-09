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

# bt_hid_host.c 已由 _r1_hotfix_patch.py 打过（断言误把注释里的字样算进去，脚本中途退出）
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
