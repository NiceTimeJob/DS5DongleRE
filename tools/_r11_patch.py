# -*- coding: utf-8 -*-
"""R1.1：USB 回报率固定 500 Hz + 映射引擎按回报率处理 + 每帧分段计时。
  ① config_validate 强制 polling_rate_mode = 1（原作者标的 500 Hz 档）；网页去掉「USB 回报率」选项。
  ② usb_task：每收到一帧只做必须每帧看的事（看门狗时间戳、唤醒、耳机/静音键/电量），
     映射引擎 + USB 转发按 ≥1.8 ms 一次处理最新那帧（≈500 Hz；手柄 ~700 帧/s，多出来的帧以前全被白算）。
     注意：不能按"端点空了才处理"来做——try_send_pending 在端点完成中断里立刻用上一帧重新挂上，
     端点几乎永远是忙的，那样会退化成 20 ms 兜底。
  ③ 每帧分段计时（mcycle 周期数，每秒换算成 0.1 µs）：收帧开销、引擎、其中陀螺仪、USB 转发、最长一次、CPU 主频，
     放进 PERF 尾部（116 → 136 B）。
用法：python tools/_r11_patch.py"""
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _patchlib import P

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
SRC = os.path.join(ROOT, 'firmware', 'src')

# ---------------- version.h ----------------
p = P(os.path.join(SRC, 'version.h'))
p.rep('#define FW_VERSION      "R1"', '#define FW_VERSION      "R1.1"')
p.save()

# ---------------- config.c：回报率固定 ----------------
p = P(os.path.join(SRC, 'config.c'))
p.rep("""    if (b->polling_rate_mode > 2)
        b->polling_rate_mode = 2;
""", """    /* R1.1：USB 回报率固定 500 Hz（用户决定，网页不再提供修改）：主机写什么都当 1。
     * 字段留在结构体里，免得为它再升一次 CONFIG_VERSION、把基础配置又清一遍。 */
    b->polling_rate_mode = 1;
""")
p.save()

# ---------------- perf.h ----------------
p = P(os.path.join(SRC, 'perf.h'))
p.rep("""#define PERF_BLOB_SIZE  116   /* S36：96 → 116，尾部加发送失败分类与当前编码参数 */
""", """#define PERF_BLOB_SIZE  136   /* S36：96 → 116（发送失败分类、编码参数）；R1.1：→ 136（每帧分段计时） */
""")
p.rep("""    uint8_t  tune_cx;          /* 114 复杂度 */
    uint8_t  rsv;              /* 115 */
} perf_blob_t;
""", """    uint8_t  tune_cx;          /* 114 复杂度 */
    uint8_t  rsv;              /* 115 */
    /* ---- R1.1：每帧分段计时（最近 1 s 窗口；时间单位 0.1 µs，由 mcycle 周期数按实测主频换算） ---- */
    uint32_t rx_win;           /* 116 收到的手柄帧数 */
    uint32_t proc_win;         /* 120 过了映射引擎 + 交给 USB 的帧数（≈500） */
    uint16_t eng_dus;          /* 124 映射引擎平均每帧 */
    uint16_t gyro_dus;         /* 126 其中陀螺仪融合（每次调用的平均） */
    uint16_t usb_dus;          /* 128 交给 USB 的平均耗时 */
    uint16_t rx_dus;           /* 130 每收一帧的固定开销（唤醒/耳机/静音键/电量） */
    uint16_t proc_max_us;      /* 132 引擎 + USB 最长一次（µs） */
    uint16_t cpu_mhz;          /* 134 实测 CPU 主频；0 = 周期计数器没在跑，上面几项无效 */
} perf_blob_t;

/* R1.1：读 CPU 周期计数器（M 模式 mcycle，低 32 位；只用来算差值） */
static inline uint32_t perf_cyc(void)
{
    uint32_t c;
    __asm__ volatile("csrr %0, mcycle" : "=r"(c));
    return c;
}
""")
p.rep("""void perf_audio_wdkill(void);                                  /* 定时器任务：看门狗击杀 */
""", """void perf_audio_wdkill(void);                                  /* 定时器任务：看门狗击杀 */

/* R1.1：每帧分段计时（usb 任务 / 映射引擎调用；参数都是 mcycle 周期数） */
void perf_frame_rx(uint32_t rx_cyc);                    /* 每收到一帧 */
void perf_frame(uint32_t eng_cyc, uint32_t usb_cyc);    /* 每处理一帧 */
void perf_gyro_cyc(uint32_t cyc);                       /* 陀螺仪融合一次 */
""")
p.rep("/* 给 RPC 读的只读视图（96 B） */", "/* 给 RPC 读的只读视图（PERF_BLOB_SIZE 字节） */")
p.save()

# ---------------- perf.c ----------------
p = P(os.path.join(SRC, 'perf.c'))
p.rep("""uint32_t perf_runtime_counter(void)
{
""", """/* R1.1：每帧分段计时的累加器（usb 任务写、bt 任务每秒取走清零；取的时候进临界区） */
static volatile uint32_t s_rx_n, s_proc_n, s_gyro_n, s_proc_max_cyc;
static volatile uint64_t s_rx_cyc, s_eng_cyc, s_gyro_cyc, s_usb_cyc;
static uint32_t s_last_mcycle;
static uint64_t s_last_us;

void perf_frame_rx(uint32_t rx_cyc)
{
    s_rx_n++;
    s_rx_cyc += rx_cyc;
}

void perf_frame(uint32_t eng_cyc, uint32_t usb_cyc)
{
    s_proc_n++;
    s_eng_cyc += eng_cyc;
    s_usb_cyc += usb_cyc;
    uint32_t t = eng_cyc + usb_cyc;
    if (t > s_proc_max_cyc)
        s_proc_max_cyc = t;
}

void perf_gyro_cyc(uint32_t cyc)
{
    s_gyro_n++;
    s_gyro_cyc += cyc;
}

/* 周期数 → 平均每次多少个 0.1 µs */
static uint16_t to_dus(uint64_t cyc, uint32_t n, uint32_t mhz)
{
    if (!mhz || !n)
        return 0;
    uint64_t v = cyc * 10u / mhz / n;
    return v > 65535u ? 65535u : (uint16_t)v;
}

uint32_t perf_runtime_counter(void)
{
""")
p.rep("""    g_perf.uptime_s = (uint32_t)(bflb_mtimer_get_time_us() / 1000000ULL);

    uint16_t out = 0;
""", """    g_perf.uptime_s = (uint32_t)(bflb_mtimer_get_time_us() / 1000000ULL);

    /* R1.1：每帧分段计时的 1 s 窗口。主频用 mcycle 与 µs 计时器对出来（不写死） */
    {
        uint32_t mc = perf_cyc();
        uint64_t us = bflb_mtimer_get_time_us();
        uint32_t mhz = 0;
        if (s_last_us && us > s_last_us)
            mhz = (uint32_t)(mc - s_last_mcycle) / (uint32_t)(us - s_last_us);
        s_last_mcycle = mc;
        s_last_us = us;

        taskENTER_CRITICAL();
        uint32_t rx_n = s_rx_n, proc_n = s_proc_n, gyro_n = s_gyro_n, pmax = s_proc_max_cyc;
        uint64_t rx_c = s_rx_cyc, eng_c = s_eng_cyc, gyro_c = s_gyro_cyc, usb_c = s_usb_cyc;
        s_rx_n = 0; s_proc_n = 0; s_gyro_n = 0; s_proc_max_cyc = 0;
        s_rx_cyc = 0; s_eng_cyc = 0; s_gyro_cyc = 0; s_usb_cyc = 0;
        taskEXIT_CRITICAL();

        g_perf.rx_win = rx_n;
        g_perf.proc_win = proc_n;
        g_perf.cpu_mhz = (uint16_t)mhz;
        g_perf.rx_dus = to_dus(rx_c, rx_n, mhz);
        g_perf.eng_dus = to_dus(eng_c, proc_n, mhz);
        g_perf.gyro_dus = to_dus(gyro_c, gyro_n, mhz);
        g_perf.usb_dus = to_dus(usb_c, proc_n, mhz);
        g_perf.proc_max_us = mhz ? (uint16_t)((pmax / mhz) > 65535u ? 65535u : (pmax / mhz)) : 0;
    }

    uint16_t out = 0;
""")
p.save()

# ---------------- ds5map.c：陀螺仪计时 ----------------
p = P(os.path.join(SRC, 'ds5map.c'))
p.rep("""    if (gyro_track) {
        gyro_update(p, now);
        tlm_gyro_pack(g_tlm_gyro);   /* 任务侧打包，ISR 的 build_tlm_state 只搬缓存 */
    }
""", """    if (gyro_track) {
        uint32_t gc0 = perf_cyc();   /* R1.1：量陀螺仪这一段到底多贵 */
        gyro_update(p, now);
        tlm_gyro_pack(g_tlm_gyro);   /* 任务侧打包，ISR 的 build_tlm_state 只搬缓存 */
        perf_gyro_cyc(perf_cyc() - gc0);
    }
""")
p.save()

# ---------------- main.c：按回报率处理 + 计时 ----------------
p = P(os.path.join(SRC, 'main.c'))
p.rep("""    uint8_t raw_report[DS5_BT_INPUT_REPORT_SIZE];
    uint64_t last_input_us    = bflb_mtimer_get_time_us();
    uint32_t usb_check_cnt = 0;
""", """    uint8_t raw_report[DS5_BT_INPUT_REPORT_SIZE];
    uint64_t last_input_us    = bflb_mtimer_get_time_us();
    uint32_t usb_check_cnt = 0;

    /* R1.1：按 USB 回报率处理。手柄每秒发 ~700 帧，回报率固定 500 Hz：映射引擎和
     * USB 转发只对"最新的一帧"做，两次之间至少隔 PROC_INTERVAL_US。中间来的帧只更新
     * pend_raw，以前它们每帧都过一遍引擎、结果又被下一帧覆盖，约三成算力白花。
     * 门槛取 1.8 ms 而不是 2 ms：任务按 1 ms 节拍醒来，留一点余量才不会掉到 333 Hz。 */
#define PROC_INTERVAL_US  1800ULL
    uint8_t  pend_raw[DS5_BT_INPUT_REPORT_SIZE];
    bool     pend_have = false;
    uint64_t last_proc_us = 0;
""")
p.rex(r"        TickType_t wait = pdMS_TO_TICKS\(10\);\n        if \(xQueueReceive\(input_queue, raw_report, wait\) == pdTRUE\) \{\n.*?(?=        usb_wake_task\(\);\n        dse_task\(\);)",
"""        TickType_t wait = pend_have ? 1 : pdMS_TO_TICKS(10);
        if (xQueueReceive(input_queue, raw_report, wait) == pdTRUE) {
            uint32_t rx_c0 = perf_cyc();
            last_input_us = bflb_mtimer_get_time_us();
            usb_fwd_count++;
            if (usb_fwd_count == 1)
                LOG_DBG("[USB-FWD] First BT→USB input forwarded\\n");
            else if ((usb_fwd_count % 500) == 0)
                LOG_DBG("[USB-FWD] %lu reports forwarded\\n",
                       (unsigned long)usb_fwd_count);

            /* R1.1：映射引擎和 USB 转发挪到下面按回报率做；这里只留每帧都必须看的事 */
            memcpy(pend_raw, raw_report, sizeof(pend_raw));
            pend_have = true;

            /* 主机挂起时也要能看到按键（PS 唤醒），用原始报文、每帧都看 */
            usb_wake_on_bt_input(raw_report + 2, DS5_USB_INPUT_PAYLOAD_LEN);

            /* Headset plug detection for audio tag switching (0x93 vs 0x96) */
            audio_set_headset((raw_report[2 + DS5_HEADSET_BYTE] & 1) != 0);

            /* Mic mute button → toggle MuteLight on controller.
             * Controller toggles status_bit only when a game has activated
             * the mic — this is normal PS5 behavior. */
            {
                static uint8_t prev_mic_bit = 0xFF;
                uint8_t cur_mic_bit = (raw_report[2 + DS5_HEADSET_BYTE] >> 2) & 1;
                if (prev_mic_bit != 0xFF && cur_mic_bit != prev_mic_bit) {
                    uint8_t mute_state[DS5_USB_OUTPUT_PAYLOAD_LEN];
                    memset(mute_state, 0, sizeof(mute_state));
                    mute_state[1] = 0x01;  /* flags1: AllowMuteLight */
                    mute_state[8] = cur_mic_bit ? 0x01 : 0x00;
                    uint8_t pkt[DS5_BT_OUTPUT_EXT_SIZE];
                    build_bt_output_ext(mute_state, DS5_USB_OUTPUT_PAYLOAD_LEN, pkt);
                    bt_hid_host_send_output(pkt, DS5_BT_OUTPUT_EXT_SIZE);
                }
                prev_mic_bit = cur_mic_bit;
            }

            if (ds5_connected) {
                /* Battery monitoring: <=10% red blink */
                uint8_t batt = raw_report[2 + DS5_BATT_BYTE_OFFSET];
                uint8_t pct  = batt & DS5_BATT_LEVEL_MASK;
                uint8_t st   = (batt >> DS5_BATT_STATE_SHIFT) & 0x0F;
                cached_battery_level = (pct > 10) ? 100 : pct * 10;
                cached_battery_state = st;
                bool discharging = (st == DS5_BATT_STATE_DISCHARGE);
                bool is_low  = discharging && (pct <= DS5_BATT_LOW_THRESHOLD);

                if (is_low && !battery_low) {
                    battery_low = true;
                    led_status_set(LED_BLINK_BATTERY);
                    LOG_WRN("[MAIN] Battery critical (%d%%)\\n", pct * 10);
                } else if (!is_low && battery_low) {
                    battery_low = false;
                    if (config_led_disabled() && conn_led_off)
                        led_status_set(LED_OFF);
                    else
                        led_refresh_state();
                }

            }
            perf_frame_rx(perf_cyc() - rx_c0);
        }

        /* R1.1：按回报率（固定 500 Hz）对最新一帧做映射 + 交给 USB。
         * 不能改成"端点空了才处理"：try_send_pending 在端点完成中断里立刻用上一帧
         * 重新挂上，端点几乎永远是忙的。 */
        if (pend_have) {
            uint64_t pn = bflb_mtimer_get_time_us();
            if (pn - last_proc_us >= PROC_INTERVAL_US) {
                pend_have = false;
                last_proc_us = pn;
                uint32_t c0 = perf_cyc();
#if DS5MAP_ENGINE_ENABLE >= 2
                ds5map_process(pend_raw + 2, (uint32_t)(pn / 1000ULL));
#endif
                uint32_t c1 = perf_cyc();
                usb_gamepad_send_raw_input(pend_raw + 2);
                perf_frame(c1 - c0, perf_cyc() - c1);
            }
        }

""")
p.save()
assert p.s.count('ds5map_process(') == 1, p.s.count('ds5map_process(')

# ---------------- 网页：去掉「USB 回报率」 ----------------
p = P(os.path.join(ROOT, 'webgui', 'js', 'page-config.js'))
p.rep("""    { k: 'polling_rate_mode', t: 'sel', n: 'USB 回报率', opts: [['0', '250 Hz'], ['1', '500 Hz'], ['2', '实时（跟随手柄）']] },
""", "")
p.save()

# ---------------- 主机工具：PERF 136 B，两段读 ----------------
p = P(os.path.join(ROOT, 'tools', 'diag.py'))
p.rep("""PERF_SIZE = 116                         # S36：96 → 116（尾部多了发送失败分类 + 当前编码参数）
""", """PERF_SIZE = 136                         # S36：96 → 116；R1.1：→ 136（每帧分段计时）。一次 RPC 最多 122 B，分两段读
""")
p.rep("""    buf = rpc_read(h, REG_PERF, 0, PERF_SIZE)
    if not buf or len(buf) < 96:          # S35 固件只有 96 B，也能解析
        return None
""", """    buf = rpc_read(h, REG_PERF, 0, 116)
    if not buf or len(buf) < 96:          # S35 固件只有 96 B，也能解析
        return None
    if len(buf) >= 116:
        tail = rpc_read(h, REG_PERF, 116, PERF_SIZE - 116)   # R1.1 起才有；老固件回 ERR → None
        if tail:
            buf = bytes(buf) + bytes(tail)
""")
p.rep("""        d.update(fail_pool=fp, fail_l2cap=fl, fail_noconn=fn, last_err=le,
                 tune_kbps=kbps, tune_bw=bw, tune_mode=mode, tune_cx=cx)
    return d
""", """        d.update(fail_pool=fp, fail_l2cap=fl, fail_noconn=fn, last_err=le,
                 tune_kbps=kbps, tune_bw=bw, tune_mode=mode, tune_cx=cx)
    if len(buf) >= 136:
        (rx, pr, eng, gyro, usb, rxd, pmax, mhz) = struct.unpack_from('<IIHHHHHH', buf, 116)
        d.update(rx_win=rx, proc_win=pr, eng_dus=eng, gyro_dus=gyro, usb_dus=usb,
                 rx_dus=rxd, proc_max_us=pmax, cpu_mhz=mhz)
    return d
""")
p.rep("""                 d['tune_kbps'], d['tune_bw'], d['tune_mode'], d['tune_cx']))
    return s
""", """                 d['tune_kbps'], d['tune_bw'], d['tune_mode'], d['tune_cx']))
    if 'rx_win' in d:
        if d['cpu_mhz']:
            s += (' | 帧 收%d/处理%d 每帧: 引擎%.1fus(陀螺仪%.1f) USB%.1fus 收帧%.1fus 最长%dus @%dMHz'
                  % (d['rx_win'], d['proc_win'], d['eng_dus'] / 10.0, d['gyro_dus'] / 10.0,
                     d['usb_dus'] / 10.0, d['rx_dus'] / 10.0, d['proc_max_us'], d['cpu_mhz']))
        else:
            s += ' | 帧 收%d/处理%d（周期计数器没在跑，分段耗时不可用）' % (d['rx_win'], d['proc_win'])
    return s
""")
p.save()
print('R1.1 PATCH DONE')
