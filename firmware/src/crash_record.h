#pragma once
/*
 * 上次异常 / 故障现场（28 B），主机通过 MAP_REG_CRASH(0x13) 读回。
 *
 * 变量本体定义在 SDK drivers/soc/bl616/std/startup/interrupt.c 的 .noinit_data 里
 * （软复位不丢），布局必须与那边的 crash_record_t 逐字节一致（主机按 <7I 解）。
 *
 * mcause 的含义：
 *   真实 CSR 值        —— SDK exception_entry 填的 CPU 异常现场（mepc/mtval/mstatus 同为 CSR）
 *   0xDEAD0001         —— FreeRTOS 栈溢出钩子（mepc = 任务句柄，mtval = 任务名前 4 字节）
 *   0xDEAD0002         —— FreeRTOS malloc 失败钩子（mepc = 调用返回地址）
 *   0xDEAD0003         —— configASSERT / 蓝牙栈 bt_assert（mepc = 调用返回地址）
 * 见 fault_hooks.c。
 */
#include <stdint.h>

typedef struct {
    uint32_t magic;   /* CRASH_RECORD_MAGIC = 记录有效 */
    uint32_t mcause;
    uint32_t mepc;
    uint32_t mtval;
    uint32_t mstatus;
    uint32_t count;   /* 累计异常/故障次数（magic 有效时累加） */
    uint32_t boot;    /* 这份记录是第几次启动后的（= g_boot_count） */
} crash_record_t;

#define CRASH_RECORD_MAGIC 0xC0DE6116u

extern crash_record_t g_crash_record;
extern uint32_t g_boot_count;   /* noinit 启动计数：热复位 +1，真掉电后随机 */
