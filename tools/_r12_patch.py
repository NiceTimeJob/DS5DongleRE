# -*- coding: utf-8 -*-
"""R1.2：陀螺仪没生效时走快速路径（优化清单第 2 项）。
  ① ds5map_process 的快速路径不再要求"陀螺仪关着"：陀螺仪开着但这一帧没生效（按住/开关模式
     没触发）也能复用上一帧的映射结果；融合降到 1/4 频率，只为维持零偏校准。按下启用键
     → 按键位变了 → 完整路径；生效期间 g_fast_ok 为假 → 每帧都完整计算，瞄准手感不变。
  ② tick_needed() 修一个会被①放大的旧漏洞：「原键 + 开关 + 连发」锁存后松开键也在翻转，
     以前只看按着的键，快速路径会把它冻住。
  ③ PERF 136 → 140 B：每秒走快速路径的帧数、陀螺仪融合次数。
用法：python tools/_r12_patch.py"""
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _patchlib import P

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
SRC = os.path.join(ROOT, 'firmware', 'src')

p = P(os.path.join(SRC, 'version.h'))
p.rep('#define FW_VERSION      "R1.1"', '#define FW_VERSION      "R1.2"')
p.save()

# ---------------- ds5map.c ----------------
p = P(os.path.join(SRC, 'ds5map.c'))
p.rep("""static uint8_t       g_fast_l2, g_fast_r2;   /* 0 不改 / 1 置 0xFF / 2 置 0x00 */
""", """static uint8_t       g_fast_l2, g_fast_r2;   /* 0 不改 / 1 置 0xFF / 2 置 0x00 */
/* R1.2：陀螺仪开着但没生效时，快速路径里融合只跑 1/GYRO_IDLE_DECIM 帧（引擎 ~500 Hz → ~125 Hz），
 * 只为维持零偏校准（零偏以分钟计变化，采样率低四倍测出来一样）。 */
#define GYRO_IDLE_DECIM 4
static uint8_t       g_gyro_decim;
""")
p.rep("""    for (int b = 0; b < MAP_BTN_V1; b++) {
        const btn_rt_t *st = &g_btn[b];
        if (!g_managed[b] || !st->raw)
            continue;
        if (!st->hold_fired || st->nturbo_run)
            return true;
    }
    return false;
}
""", """    for (int b = 0; b < MAP_BTN_V1; b++) {
        const btn_rt_t *st = &g_btn[b];
        if (!g_managed[b])
            continue;
        /* R1.2：原生连发在跑就要计时。「原键 + 开关 + 连发」锁存后松开了键也在翻转，
         * 以前只看按着的键 → 快速路径会把输出冻住（R1.2 起陀螺仪用户也走快速路径，会碰上）。 */
        if (st->nturbo_run)
            return true;
        if (st->raw && !st->hold_fired)
            return true;
    }
    return false;
}
""")
p.rep("""    /* S36：输入没变就复用上一帧的结果。手柄 600~770 帧/s 里绝大多数帧只有摇杆 / 陀螺仪在变，
     * 按键位和触摸板触点与上一帧完全一样，映射结果也必然一样。 */
    if (g_fast_ok && !g_force_full && !gyro_track && !g_tp_on &&
        memcmp(&p[7], g_fast_in, 3) == 0 && memcmp(&p[32], g_fast_in + 3, 8) == 0) {
        p[7] = g_fast_out[0];
""", """    /* S36：输入没变就复用上一帧的结果。手柄 600~770 帧/s 里绝大多数帧只有摇杆 / 陀螺仪在变，
     * 按键位和触摸板触点与上一帧完全一样，映射结果也必然一样。
     * R1.2：陀螺仪开着但这一帧没生效也走这里。启用键（按住/开关）的状态只由按键位决定，
     * 按键位没变它就没变；g_fast_ok 本身要求上一帧 !g_gyro_live（「常开」模式永远走完整路径）。 */
    if (g_fast_ok && !g_force_full && !g_tp_on &&
        memcmp(&p[7], g_fast_in, 3) == 0 && memcmp(&p[32], g_fast_in + 3, 8) == 0) {
        if (gyro_track && ++g_gyro_decim >= GYRO_IDLE_DECIM) {
            g_gyro_decim = 0;
            uint32_t gc0 = perf_cyc();
            gyro_update(p, now);
            tlm_gyro_pack(g_tlm_gyro);
            perf_gyro_cyc(perf_cyc() - gc0);
        }
        perf_frame_fast();
        p[7] = g_fast_out[0];
""")
p.save()
assert p.s.count('perf_frame_fast();') == 1

# ---------------- perf.h / perf.c ----------------
p = P(os.path.join(SRC, 'perf.h'))
p.rep("""#define PERF_BLOB_SIZE  136   /* S36：96 → 116（发送失败分类、编码参数）；R1.1：→ 136（每帧分段计时） */
""", """#define PERF_BLOB_SIZE  140   /* S36：96 → 116（发送失败分类、编码参数）；R1.1：→ 136（每帧分段计时）；R1.2：→ 140 */
""")
p.rep("""    uint16_t cpu_mhz;          /* 134 实测 CPU 主频；0 = 周期计数器没在跑，上面几项无效 */
} perf_blob_t;
""", """    uint16_t cpu_mhz;          /* 134 实测 CPU 主频；0 = 周期计数器没在跑，上面几项无效 */
    uint16_t fast_win;         /* 136 R1.2：其中走快速路径的帧数（输入没变、复用上一帧结果） */
    uint16_t gyro_win;         /* 138 R1.2：陀螺仪融合跑了几次 */
} perf_blob_t;
""")
p.rep("""void perf_gyro_cyc(uint32_t cyc);                       /* 陀螺仪融合一次 */
""", """void perf_gyro_cyc(uint32_t cyc);                       /* 陀螺仪融合一次 */
void perf_frame_fast(void);                             /* R1.2：这一帧走了快速路径 */
""")
p.save()

p = P(os.path.join(SRC, 'perf.c'))
p.rep("""static volatile uint32_t s_rx_n, s_proc_n, s_gyro_n, s_proc_max_cyc;
""", """static volatile uint32_t s_rx_n, s_proc_n, s_gyro_n, s_proc_max_cyc, s_fast_n;
""")
p.rep("""void perf_gyro_cyc(uint32_t cyc)
{
""", """void perf_frame_fast(void)
{
    s_fast_n++;
}

void perf_gyro_cyc(uint32_t cyc)
{
""")
p.rep("""        uint32_t rx_n = s_rx_n, proc_n = s_proc_n, gyro_n = s_gyro_n, pmax = s_proc_max_cyc;
""", """        uint32_t rx_n = s_rx_n, proc_n = s_proc_n, gyro_n = s_gyro_n, pmax = s_proc_max_cyc;
        uint32_t fast_n = s_fast_n;
        s_fast_n = 0;
""")
p.rep("""        g_perf.proc_max_us = mhz ? (uint16_t)((pmax / mhz) > 65535u ? 65535u : (pmax / mhz)) : 0;
""", """        g_perf.proc_max_us = mhz ? (uint16_t)((pmax / mhz) > 65535u ? 65535u : (pmax / mhz)) : 0;
        g_perf.fast_win = (uint16_t)(fast_n > 65535u ? 65535u : fast_n);
        g_perf.gyro_win = (uint16_t)(gyro_n > 65535u ? 65535u : gyro_n);
""")
p.save()

# ---------------- 主机 diag.py ----------------
p = P(os.path.join(ROOT, 'tools', 'diag.py'))
p.rep("""PERF_SIZE = 136                         # S36：96 → 116；R1.1：→ 136（每帧分段计时）。一次 RPC 最多 122 B，分两段读
""", """PERF_SIZE = 140                         # S36：96 → 116；R1.1：→ 136（每帧分段计时）；R1.2：→ 140。一次 RPC 最多 122 B，分两段读
""")
p.rep("""        d.update(rx_win=rx, proc_win=pr, eng_dus=eng, gyro_dus=gyro, usb_dus=usb,
                 rx_dus=rxd, proc_max_us=pmax, cpu_mhz=mhz)
    return d
""", """        d.update(rx_win=rx, proc_win=pr, eng_dus=eng, gyro_dus=gyro, usb_dus=usb,
                 rx_dus=rxd, proc_max_us=pmax, cpu_mhz=mhz)
    if len(buf) >= 140:
        (fw, gw) = struct.unpack_from('<HH', buf, 136)
        d.update(fast_win=fw, gyro_win=gw)
    return d
""")
p.rep("""            s += ' | 帧 收%d/处理%d（周期计数器没在跑，分段耗时不可用）' % (d['rx_win'], d['proc_win'])
    return s
""", """            s += ' | 帧 收%d/处理%d（周期计数器没在跑，分段耗时不可用）' % (d['rx_win'], d['proc_win'])
    if 'fast_win' in d:
        s += ' 快路径%d 陀螺仪%d次' % (d['fast_win'], d['gyro_win'])
    return s
""")
p.save()
print('R1.2 PATCH DONE')
