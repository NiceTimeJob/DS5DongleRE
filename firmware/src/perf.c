#include "perf.h"
#include "FreeRTOS.h"
#include "task.h"
#include "bflb_mtimer.h"
#include <string.h>
#include <stdbool.h>

static perf_blob_t g_perf = { .magic = PERF_MAGIC, .tune_kbps = 160 };

/* 1 s 窗口内的最大值，采样时搬进 g_perf.*_win_us 再清零 */
static volatile uint32_t s_enc_win, s_blk_win;

/* R1.1：每帧分段计时的累加器（usb 任务写、bt 任务每秒取走清零；取的时候进临界区） */
static volatile uint32_t s_rx_n, s_proc_n, s_gyro_n, s_proc_max_cyc, s_fast_n;
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

void perf_frame_fast(void)
{
    s_fast_n++;
}

void perf_link(int8_t rssi, uint8_t quality, uint8_t afh_used, uint8_t afh_mode)
{
    g_perf.rssi = rssi;
    g_perf.link_quality = quality;
    g_perf.afh_used = afh_used;
    g_perf.afh_mode = afh_mode;
}

void perf_link_param(uint8_t role, uint16_t lsto)
{
    g_perf.role = role;
    g_perf.lsto = lsto;
}

void perf_afh_range(uint8_t lo, uint8_t hi)
{
    g_perf.afh_lo = lo;
    g_perf.afh_hi = hi;
}

void perf_hci_evt(void)
{
    g_perf.hci_evt++;
}

/* R1.6：输出报告（震动 / 灯 / 扳机）。s_out_n / s_out_drop 在 USB ISR 里加，采样时进临界区取；
 * 延迟累加只在 bt 任务里写、也只在 bt 任务（perf_cpu_sample）里读。 */
static volatile uint32_t s_out_n, s_out_drop;
static uint32_t s_out_fwd_n, s_out_lat_sum, s_out_lat_max;

_Static_assert(sizeof(perf_blob_t) == PERF_BLOB_SIZE, "perf_blob_t 和 PERF_BLOB_SIZE 对不上");

void perf_out_rx(bool dropped)
{
    s_out_n++;
    if (dropped)
        s_out_drop++;
}

void perf_out_fwd(uint32_t lat_us)
{
    s_out_fwd_n++;
    s_out_lat_sum += lat_us;
    if (lat_us > s_out_lat_max)
        s_out_lat_max = lat_us;
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
    return (uint32_t)bflb_mtimer_get_time_us();
}

void perf_audio_block(uint32_t blk_us, uint32_t enc_max_us)
{
    g_perf.audio_blocks++;
    if (enc_max_us > g_perf.enc_max_us)
        g_perf.enc_max_us = (enc_max_us > 0xFFFF) ? 0xFFFF : (uint16_t)enc_max_us;
    if (blk_us > g_perf.blk_max_us)
        g_perf.blk_max_us = (blk_us > 0xFFFF) ? 0xFFFF : (uint16_t)blk_us;
    if (enc_max_us > s_enc_win)
        s_enc_win = enc_max_us;
    if (blk_us > s_blk_win)
        s_blk_win = blk_us;
}

void perf_audio_overrun(void)
{
    g_perf.audio_overruns++;
}

void perf_audio_sendfail(int err)
{
    g_perf.audio_send_fail++;
    if (err == -2)
        g_perf.fail_pool++;
    else if (err == -1)
        g_perf.fail_noconn++;
    else {
        g_perf.fail_l2cap++;
        g_perf.last_err = (int16_t)err;
    }
}

void perf_audio_tune(uint16_t kbps, uint8_t bw, uint8_t mode, uint8_t cx)
{
    g_perf.tune_kbps = kbps;
    g_perf.tune_bw = bw;
    g_perf.tune_mode = mode;
    g_perf.tune_cx = cx;
}

void perf_audio_wdkill(void)
{
    g_perf.wd_kills++;
}

/* uxTaskGetSystemState 返回的是开机以来的累计运行时间，占比要用两次采样的差。
 * 用任务句柄配对上一次的值（表顺序不保证稳定）。 */
#define PERF_TASK_SLOTS 12
static TaskStatus_t   s_status[PERF_TASK_SLOTS];
static TaskHandle_t   s_prev_handle[PERF_TASK_SLOTS];
static uint32_t       s_prev_run[PERF_TASK_SLOTS];
static uint32_t       s_prev_total;
static bool           s_primed;

void perf_cpu_sample(void)
{
    uint32_t total = 0;
    UBaseType_t n = uxTaskGetSystemState(s_status, PERF_TASK_SLOTS, &total);
    uint32_t dtotal = total - s_prev_total;

    /* 先把音频的 1 s 窗口归档 */
    uint32_t ew = s_enc_win, bw = s_blk_win;
    s_enc_win = 0;
    s_blk_win = 0;
    g_perf.enc_win_us = (ew > 0xFFFF) ? 0xFFFF : (uint16_t)ew;
    g_perf.blk_win_us = (bw > 0xFFFF) ? 0xFFFF : (uint16_t)bw;
    g_perf.uptime_s = (uint32_t)(bflb_mtimer_get_time_us() / 1000000ULL);

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
        uint32_t fast_n = s_fast_n;
        s_fast_n = 0;
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
        g_perf.fast_win = (uint16_t)(fast_n > 65535u ? 65535u : fast_n);
        g_perf.gyro_win = (uint16_t)(gyro_n > 65535u ? 65535u : gyro_n);
    }

    /* R1.6：输出报告的 1 s 窗口 */
    {
        taskENTER_CRITICAL();
        uint32_t on = s_out_n, od = s_out_drop;
        s_out_n = 0;
        taskEXIT_CRITICAL();
        uint32_t fn = s_out_fwd_n, ls = s_out_lat_sum, lm = s_out_lat_max;
        s_out_fwd_n = 0;
        s_out_lat_sum = 0;
        s_out_lat_max = 0;
        uint32_t avg = fn ? ls / fn : 0;
        g_perf.out_win = (uint16_t)(on > 65535u ? 65535u : on);
        g_perf.out_lat_avg_us = (uint16_t)(avg > 65535u ? 65535u : avg);
        g_perf.out_lat_max_us = (uint16_t)(lm > 65535u ? 65535u : lm);
        g_perf.out_drop = (uint16_t)(od > 65535u ? 65535u : od);
    }

    uint16_t out = 0;
    if (s_primed && dtotal != 0) {
        /* 按运行时间差从大到小挑前 PERF_TASK_MAX 个（n 很小，直接选择排序） */
        uint32_t delta[PERF_TASK_SLOTS];
        for (UBaseType_t i = 0; i < n; i++) {
            uint32_t prev = 0;
            for (int k = 0; k < PERF_TASK_SLOTS; k++) {
                if (s_prev_handle[k] == s_status[i].xHandle) {
                    prev = s_prev_run[k];
                    break;
                }
            }
            delta[i] = (uint32_t)s_status[i].ulRunTimeCounter - prev;
        }
        bool used[PERF_TASK_SLOTS] = { 0 };
        for (; out < PERF_TASK_MAX && out < n; out++) {
            int best = -1;
            for (UBaseType_t i = 0; i < n; i++)
                if (!used[i] && (best < 0 || delta[i] > delta[best]))
                    best = (int)i;
            if (best < 0)
                break;
            used[best] = true;
            uint64_t pm = ((uint64_t)delta[best] * 1000u) / dtotal;
            g_perf.task[out].permille = (pm > 1000) ? 1000 : (uint16_t)pm;
            memset(g_perf.task[out].name, 0, sizeof(g_perf.task[out].name));
            strncpy(g_perf.task[out].name, s_status[best].pcTaskName,
                    sizeof(g_perf.task[out].name) - 1);
        }
    }
    g_perf.ntask = out;

    /* 记住这一轮，给下一轮做差 */
    memset(s_prev_handle, 0, sizeof(s_prev_handle));
    for (UBaseType_t i = 0; i < n && i < PERF_TASK_SLOTS; i++) {
        s_prev_handle[i] = s_status[i].xHandle;
        s_prev_run[i] = (uint32_t)s_status[i].ulRunTimeCounter;
    }
    s_prev_total = total;
    s_primed = true;
}

const uint8_t *perf_blob(void)
{
    return (const uint8_t *)&g_perf;
}
