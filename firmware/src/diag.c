/*
 * 诊断事件环形日志 —— 见 diag.h 的说明。
 *
 * 放在 .noinit_data 段（软复位/看门狗复位不丢，冷启动时由 diag_init 清空），
 * 因为最想抓的现场恰恰是「崩了/被复位了」那一次。
 */

#include "diag.h"

#include <string.h>

#include "bflb_mtimer.h"
#include "bl616_sys.h"
#include "bl616_hbn.h"   /* S30: HBN_Get_Reset_Event，原始复位事件位（bit4 = BOD） */

/* 头 16 B：magic / ver / 写游标 / boot 计数 / 起始时间戳 / 保留 */
#define DIAG_MAGIC 0x44494147u   /* 'DIAG' */
#define DIAG_VER   1u

/* 头和记录都显式 packed：布局必须和主机侧解析代码逐字节一致，
 * 不能依赖编译器对齐（曾经因为 ev+rsv 的填充导致主机按 12/16 两种步长
 * 都能"解出东西"，浪费了很久）。 */
typedef struct {
    uint32_t magic;
    uint16_t ver;
    uint16_t head;      /* 已写入的记录条数（取模看 slot） */
    uint32_t boot;      /* 第几次启动 */
    uint32_t t0_ms;     /* 本次启动的时间戳基准（诊断用） */
} __attribute__((packed)) diag_hdr_t;       /* 恰好 16 B */

typedef struct {
    uint32_t t_ms;
    uint8_t  ev;
    uint8_t  rsv[3];
    uint32_t a1;
    uint32_t a2;
} __attribute__((packed)) diag_rec_t;      /* 恰好 12 B */

/* 必须落在 .noinit_data（bl616_common.ld.in 里 *(.noinit_data*)）——
 * 否则被复位清空，正好丢掉最想抓的那一次现场。 */
__attribute__((section(".noinit_data"))) static diag_hdr_t s_hdr;
__attribute__((section(".noinit_data"))) static diag_rec_t s_rec[DIAG_SLOTS];
/* S30：上一次开机的整块快照（同样 noinit）。diag_init() 清环之前先拷到这里，
 * 主机通过 MAP_REG_DIAG_PREV(0x16) 读回"复位前最后 25 条"。 */
__attribute__((section(".noinit_data"))) static diag_hdr_t s_prev_hdr;
__attribute__((section(".noinit_data"))) static diag_rec_t s_prev_rec[DIAG_SLOTS];

/* 只有 USB ISR 侧读、任务侧写，8 位游标天然原子，不需要加锁。 */
static uint8_t s_next;      /* 下一条要写的 slot */
static uint32_t s_last_input_ms;

static uint32_t now_ms(void)
{
    return (uint32_t)(bflb_mtimer_get_time_us() / 1000ULL);
}

/* 诊断模块总开关。
 *
 * 2026-10-10 夜：原始固件 v3.21 在同一块板/同一个口上连续 70+ 秒零重启，
 * 而我们这份每 5–10 秒复位一次 → 问题在本工程的某个改动里。为做 A/B，
 * 这里把整块诊断功能关掉（diag_log 直接 return、RPC 读区返回空），
 * 其余代码一字不动。验证完再打开。
 *
 * 0 = 关闭（A/B 对照用）；1 = 打开 */
#define DIAG_ENABLE 1

void diag_init(uint32_t boot_count)
{
#if !DIAG_ENABLE
    (void)boot_count;
    s_hdr.magic = 0;      /* 让主机侧一眼看出诊断是关的 */
    return;
#else
    /* S30：清环之前先把上一次开机的日志整块留下（magic 完好 = RAM 活过了这次复位）。
     * 以前这里直接 memset，等于每次开机都把最想看的"复位前最后 25 条"亲手擦掉。 */
    if (s_hdr.magic == DIAG_MAGIC) {
        memcpy(&s_prev_hdr, &s_hdr, sizeof(s_hdr));
        memcpy(s_prev_rec, s_rec, sizeof(s_rec));
    } else {
        s_prev_hdr.magic = 0;      /* 冷启动：没有可信的上一轮 */
    }
    memset(s_rec, 0, sizeof(s_rec));
    s_next = 0;
    s_last_input_ms = 0;
    s_hdr.magic = DIAG_MAGIC;
    s_hdr.ver   = DIAG_VER;
    s_hdr.head  = 0;
    s_hdr.boot  = boot_count;
    s_hdr.t0_ms = now_ms();
    /* 复位原因放在第一条。S30 之前 bl_sys_rstinfo_init() 从没被调过，这里恒为 0「上电」，
     * 毫无信息量。现在：先抓 HBN 原始复位事件位（bit4 = BOD 掉电检测），再让 SDK 解析
     * 保持区（HBN RAM 0x20010400，热复位不丢、真掉电丢）。结果含义：
     *   0 上电   = 保持区校验失败，RAM 真丢了（长时间断电）
     *   1 热复位 = 保持区完好但没人标记原因（SDK 预存的"看门狗"哨兵）—— 来历不明的热复位
     *   2 异常   = SDK interrupt.c 的 exception_entry 标的
     *   3 钩子   = fault_hooks.c（栈溢出 / malloc 失败 / 断言）标的
     *   4 软复位 = 固件主动复位（主机命令 0xF6/0x03）
     *   6 BOD    = 掉电检测复位
     * a1 = 原因 | (原始事件位 << 8)。 */
    uint8_t rst_evt = 0;
    HBN_Get_Reset_Event(&rst_evt);
    bl_sys_rstinfo_init();
    diag_log(DIAG_EV_RST_REASON, (uint32_t)bl_sys_rstinfo_get() | ((uint32_t)rst_evt << 8), 0);
    diag_log(DIAG_EV_BOOT, boot_count, 0);
#endif /* DIAG_ENABLE */
}

void diag_log(uint8_t ev, uint32_t arg1, uint32_t arg2)
{
#if !DIAG_ENABLE
    (void)ev; (void)arg1; (void)arg2;
    return;
#else
    /* 输入报告是最频繁的事件（500 Hz）：只在「超过 1 秒没有输入」之后，
     * 或者第一帧时记一条，免得把环形缓冲冲干净。 */
    uint32_t t = now_ms();
    if (ev == DIAG_EV_INPUT) {
        if (s_last_input_ms && (uint32_t)(t - s_last_input_ms) < 1000)
            return;
        s_last_input_ms = t ? t : 1;
    }

    diag_rec_t *r = &s_rec[s_next];
    r->t_ms = t;
    r->ev   = ev;
    r->rsv[0] = r->rsv[1] = r->rsv[2] = 0;
    r->a1   = arg1;
    r->a2   = arg2;

    s_next = (uint8_t)((s_next + 1) % DIAG_SLOTS);

    /* head 只在 magic 正确时自增，避免未初始化 RAM 里的垃圾头被当成有效日志 */
    if (s_hdr.magic == DIAG_MAGIC)
        s_hdr.head++;
#endif /* DIAG_ENABLE */
}

void diag_read(uint8_t *out)
{
    memcpy(out, &s_hdr, sizeof(s_hdr));
    memcpy(out + sizeof(s_hdr), s_rec, sizeof(s_rec));
}

/* 下面三个是给 USB ISR 用的：只拷调用方真正要的那几字节。
 * 不要在 ISR 里调 diag_read()，它要 316 字节的目的缓冲 —— 中断栈只有 4 KB。 */
void diag_read_hdr(void *out16)
{
    memcpy(out16, &s_hdr, sizeof(s_hdr));
}

void diag_read_rec(int idx, void *out12)
{
    if (idx < 0 || idx >= DIAG_SLOTS) {
        memset(out12, 0, DIAG_REC_SZ);
        return;
    }
    memcpy(out12, &s_rec[idx], DIAG_REC_SZ);
}

/* 按 offset 从「一份表头 + 一组记录」里拷一段；当前环和上一轮快照共用这一个实现。 */
static void copy_at(const diag_hdr_t *hdr, const diag_rec_t *rec,
                    uint8_t *out, uint16_t off, uint16_t n)
{
    while (n) {
        uint16_t done = 0;
        if (off < sizeof(*hdr)) {
            uint16_t k = (uint16_t)(sizeof(*hdr) - off);
            if (k > n) k = n;
            memcpy(out, (const uint8_t *)hdr + off, k);
            done = k;
        } else {
            uint16_t rel = (uint16_t)(off - sizeof(*hdr));
            int idx = rel / DIAG_REC_SZ;
            uint16_t bo = (uint16_t)(rel % DIAG_REC_SZ);
            if (idx >= DIAG_SLOTS) {
                memset(out, 0, n);
                return;
            }
            uint16_t k = (uint16_t)(DIAG_REC_SZ - bo);
            if (k > n) k = n;
            memcpy(out, (const uint8_t *)&rec[idx] + bo, k);
            done = k;
        }
        out += done;
        off = (uint16_t)(off + done);
        n = (uint16_t)(n - done);
    }
}

void diag_read_at(uint8_t *out, uint16_t off, uint16_t n)
{
    copy_at(&s_hdr, s_rec, out, off, n);
}

void diag_read_prev_at(uint8_t *out, uint16_t off, uint16_t n)
{
    copy_at(&s_prev_hdr, s_prev_rec, out, off, n);
}
