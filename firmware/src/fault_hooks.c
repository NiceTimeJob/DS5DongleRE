/*
 * S30：把 FreeRTOS / 蓝牙栈的三条"死路"改成「记现场 → 标记原因 → 软复位」。
 *
 * SDK 自带的弱定义（components/os/freertos/freertos_port.c）是 printf + abort()，
 * abort() 最终落到 newlib 的 _exit() 死循环；板上没有硬件看门狗，于是设备就此挂死、
 * 不会自己回来，主机也读不到任何现场。这里用强定义覆盖它们：
 *   - 现场写进 SDK interrupt.c 里那块 noinit 的 g_crash_record（主机 MAP_REG_CRASH 0x13 读回），
 *     用 mcause = 0xDEAD000x 和真实 CPU 异常区分开；
 *   - 复位原因标成 BL_RST_SOFTWARE_WATCHDOG，DIAG 第一条显示「软件故障钩子」；
 *   - GLB_SW_System_Reset() 不清 RAM，现场和 DIAG 环都能活过去。
 * 蓝牙 host 栈的 bt_assert() 走的也是 vAssertCalled()，一并覆盖到。
 */
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "crash_record.h"
#include "bl616_sys.h"
#include "bl616_glb.h"
#include "bflb_irq.h"
#include "bflb_l1c.h"   /* R1：复位前把 D-cache 刷回 RAM */

#define HOOK_STACK_OVERFLOW 0xDEAD0001u
#define HOOK_MALLOC_FAILED  0xDEAD0002u
#define HOOK_ASSERT         0xDEAD0003u

static void fault_record_and_reset(uint32_t code, uint32_t pc, uint32_t extra)
{
    uint32_t n = (g_crash_record.magic == CRASH_RECORD_MAGIC) ? (g_crash_record.count + 1u) : 1u;
    g_crash_record.magic   = CRASH_RECORD_MAGIC;
    g_crash_record.mcause  = code;
    g_crash_record.mepc    = pc;
    g_crash_record.mtval   = extra;
    g_crash_record.mstatus = 0;
    g_crash_record.count   = n;
    g_crash_record.boot    = g_boot_count;
    bl_sys_rstinfo_set(BL_RST_SOFTWARE_WATCHDOG);
    (void)bflb_irq_save();
    bflb_l1c_dcache_clean_all();   /* R1：现场还在 D-cache 里，不刷回 RAM 复位后就没了 */
    GLB_SW_System_Reset();
    while (1) {
    }
}

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    char b[4] = {0, 0, 0, 0};
    uint32_t name4 = 0;
    if (pcTaskName)
        strncpy(b, pcTaskName, 4);       /* 任务名前 4 字节塞进 mtval，主机按 ASCII 看 */
    memcpy(&name4, b, 4);
    printf("[FAULT] stack overflow in task %s", pcTaskName ? pcTaskName : "?");
    puts("");
    fault_record_and_reset(HOOK_STACK_OVERFLOW, (uint32_t)(uintptr_t)xTask, name4);
}

void vApplicationMallocFailedHook(void)
{
    puts("[FAULT] malloc failed");
    fault_record_and_reset(HOOK_MALLOC_FAILED, (uint32_t)(uintptr_t)__builtin_return_address(0), 0);
}

void vAssertCalled(void);
void vAssertCalled(void)
{
    puts("[FAULT] assert");
    fault_record_and_reset(HOOK_ASSERT, (uint32_t)(uintptr_t)__builtin_return_address(0), 0);
}
