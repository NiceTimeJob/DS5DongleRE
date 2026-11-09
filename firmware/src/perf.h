#ifndef DONGLERE_PERF_H_
#define DONGLERE_PERF_H_

/*
 * S33：运行时性能统计（MAP_REG_PERF = 0x17，只读，96 B）。
 *
 * 起因：用户怀疑「宏 / 映射 / 覆盖层引擎」把音频挤粗糙了。引擎在 usb 任务（优先级 6）里
 * 每帧跑一次（实测 750 帧/s），音频任务优先级 5，而 audio.c 自己的注释说一帧 10 ms 的
 * Opus 编码要 5–7 ms、一块 21.3 ms 要编两帧——余量很薄。这里把判断需要的数字直接读出来：
 *   - 音频：块数、USB 侧 PCM 块被覆盖次数（= 丢块）、发送失败数、看门狗击杀数、
 *           编码/整块的最长耗时（开机以来 + 最近 1 s 窗口）
 *   - CPU：各任务最近 1 s 的占比（千分比），含 FreeRTOS 定时器任务与空闲任务
 * 全部是 RAM 里的计数，读取是纯拷贝，USB ISR 安全。
 */

#include <stdint.h>
#include <stdbool.h>   /* R1.6：perf_out_rx(bool) */

#define PERF_MAGIC      0x46524550u   /* 'PERF' */
#define PERF_TASK_MAX   8
#define PERF_BLOB_SIZE  160   /* S36：96 → 116；R1.1：→ 136；R1.2：→ 140；R1.3：→ 144（链路诊断）；R1.4：→ 148；R1.5：→ 152；R1.6：→ 160 */

typedef struct __attribute__((packed)) {
    uint32_t magic;            /* 0  PERF_MAGIC */
    uint32_t uptime_s;         /* 4  开机秒数（最近一次采样时） */
    uint32_t audio_blocks;     /* 8  音频块总数（每块 1024 采样 = 21.3 ms） */
    uint32_t audio_overruns;   /* 12 PCM 块还没取走就被下一块顶掉（丢块） */
    uint32_t audio_send_fail;  /* 16 0x39 发送失败总数 */
    uint16_t enc_max_us;       /* 20 单帧 Opus 编码最长耗时（开机以来） */
    uint16_t blk_max_us;       /* 22 一块处理（含发送）最长耗时（开机以来） */
    uint16_t enc_win_us;       /* 24 最近一个 1 s 窗口内的编码最长耗时 */
    uint16_t blk_win_us;       /* 26 最近一个 1 s 窗口内的整块最长耗时 */
    uint16_t wd_kills;         /* 28 编码看门狗（>15 ms）杀过音频任务几次 */
    uint16_t ntask;            /* 30 下面 task[] 有效条数 */
    struct __attribute__((packed)) {
        char     name[6];      /* 任务名前 5 字节 + '\0' */
        uint16_t permille;     /* 最近 1 s 的 CPU 占比，千分之一 */
    } task[PERF_TASK_MAX];     /* 32 .. 95 */
    /* ---- S36 ---- */
    uint32_t fail_pool;        /* 96  发送失败：hid_tx_pool 空（bt_hid_host_send_output 返回 -2） */
    uint32_t fail_l2cap;       /* 100 发送失败：bt_l2cap_chan_send 返回错误 */
    uint32_t fail_noconn;      /* 104 发送失败：当时没连接（-1） */
    int16_t  last_err;         /* 108 最近一次 l2cap 错误码 */
    uint16_t tune_kbps;        /* 110 当前编码码率 kbps */
    uint8_t  tune_bw;          /* 112 带宽码 */
    uint8_t  tune_mode;        /* 113 0 填充到 200 B / 1 真实长度 */
    uint8_t  tune_cx;          /* 114 复杂度 */
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
    uint16_t fast_win;         /* 136 R1.2：其中走快速路径的帧数（输入没变、复用上一帧结果） */
    uint16_t gyro_win;         /* 138 R1.2：陀螺仪融合跑了几次（R1.3 起只有静置校准时才跑） */
    int8_t   rssi;             /* 140 R1.3：最近一次 Read_RSSI（BR/EDR 是相对黄金区间的 dB，0 = 在区间内） */
    uint8_t  link_quality;     /* 141 R1.3：Read_Link_Quality 0..255（厂商定义，越大越好） */
    uint8_t  afh_used;         /* 142 R1.3：AFH 在用信道数 0..79（被干扰砍掉的越多越少） */
    uint8_t  afh_mode;         /* 143 R1.3：AFH 模式 0 关 / 1 开 */
    uint8_t  role;             /* 144 R1.4：0 主（Central）/ 1 从 / 0xFF 没读到 */
    uint8_t  rsv145;
    uint16_t lsto;             /* 146 R1.4：链路监督超时，slot（0.625 ms）；0 = 没读到 */
    uint8_t  afh_lo;           /* 148 R1.5：被 AFH 排除的最低信道号（2402+n MHz），0xFF = 一个没排除 */
    uint8_t  afh_hi;           /* 149 R1.5：被排除的最高信道号 */
    uint16_t hci_evt;          /* 150 R1.5：连着时收到的普通 HCI 事件累计数（DIAG 只记前几条） */
    uint16_t out_win;          /* 152 R1.6：最近 1 s 电脑发来的输出报告数（震动 / 灯 / 扳机） */
    uint16_t out_lat_avg_us;   /* 154 R1.6：「USB 收到 → 交给蓝牙」平均延迟，µs */
    uint16_t out_lat_max_us;   /* 156 R1.6：最大延迟，µs（封顶 65535） */
    uint16_t out_drop;         /* 158 R1.6：开机以来排队满被丢掉的输出报告（封顶 65535） */
} perf_blob_t;

/* R1.1：读 CPU 周期计数器（M 模式 mcycle，低 32 位；只用来算差值） */
static inline uint32_t perf_cyc(void)
{
    uint32_t c;
    __asm__ volatile("csrr %0, mcycle" : "=r"(c));
    return c;
}

/* 音频侧（audio.c / ds5_usb_audio.c 调用） */
void perf_audio_block(uint32_t blk_us, uint32_t enc_max_us);   /* 音频任务：一块处理完 */
void perf_audio_overrun(void);                                 /* USB ISR：丢块 */
void perf_audio_sendfail(int err);                             /* 音频任务：0x39 发送失败（err 见 fail_* 注释） */
void perf_audio_tune(uint16_t kbps, uint8_t bw, uint8_t mode, uint8_t cx);   /* S36：记录当前编码参数 */
void perf_audio_wdkill(void);                                  /* 定时器任务：看门狗击杀 */

/* R1.1：每帧分段计时（usb 任务 / 映射引擎调用；参数都是 mcycle 周期数） */
void perf_frame_rx(uint32_t rx_cyc);                    /* 每收到一帧 */
void perf_frame(uint32_t eng_cyc, uint32_t usb_cyc);    /* 每处理一帧 */
void perf_gyro_cyc(uint32_t cyc);                       /* 陀螺仪融合一次 */
void perf_frame_fast(void);                             /* R1.2：这一帧走了快速路径 */
void perf_link(int8_t rssi, uint8_t quality, uint8_t afh_used, uint8_t afh_mode);   /* R1.3 */
void perf_link_param(uint8_t role, uint16_t lsto);                                 /* R1.4 */
void perf_afh_range(uint8_t lo, uint8_t hi);                                       /* R1.5 */
void perf_hci_evt(void);                                                           /* R1.5 */
void perf_out_rx(bool dropped);       /* R1.6：USB ISR 里调（只加计数） */
void perf_out_fwd(uint32_t lat_us);   /* R1.6：bt 任务转发完一份输出报告后调 */

/* 任务上下文、≥1 s 调一次：采样各任务 CPU 占比，顺带把 1 s 窗口的最大值归档。 */
void perf_cpu_sample(void);

/* 给 RPC 读的只读视图（PERF_BLOB_SIZE 字节） */
const uint8_t *perf_blob(void);

/* FreeRTOS 运行时统计的计数器（portGET_RUN_TIME_COUNTER_VALUE） */
uint32_t perf_runtime_counter(void);

#endif /* DONGLERE_PERF_H_ */
