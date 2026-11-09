# -*- coding: utf-8 -*-
"""R1.6：「接收器当主」改成默认开 + 普通震动 / 灯 / 扳机收到就转发。

  ① bt_hid_host.c：s_role_req 默认 1；连上满 3 s 才申请（R1.5 实测是连接稳定后切的，别挤在连接建立那一下）。
     0xF6/0x11 照旧能关（不落盘，拔插恢复开）。
  ② main.c：删掉原作者「手柄喇叭有声音时每 24 ms 攒一批再转发」——包数不变，只是不再扣着；
     输出队列元素带上 ISR 收到的时刻，USB ISR 用 portYIELD_FROM_ISR 直接叫醒 bt 任务（同 ds5_usb_audio.c）。
  ③ PERF 152 → 160：输出报告数 / 转发延迟平均与最大 / 排队丢弃数；perf.c 加 sizeof 静态断言。
所有替换先做完再统一写盘。用法：python tools/_r16_patch.py"""
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _patchlib import P

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
SRC = os.path.join(ROOT, 'firmware', 'src')
files = []

# ======================= version.h =======================
p = P(os.path.join(SRC, 'version.h'))
p.rep('#define FW_VERSION      "R1.5"', '#define FW_VERSION      "R1.6"')
files.append(p)

# ======================= bt_hid_host.c / .h =======================
p = P(os.path.join(SRC, 'bt_hid_host.c'))
p.rep("""/* R1.5：「接收器当主」试验开关（0xF6 命令 0x11 运行时改，不落盘，默认关）和本次连接的进度 */
static volatile uint8_t  s_role_req;
static bool              s_rs_tried, s_lsto_done;
""", """/* R1.5：「接收器当主」开关（0xF6 命令 0x11 运行时改，不落盘）和本次连接的进度。
 * R1.6 起默认开：R1.5 实测手柄接受切换、不断线，当主后监督超时由我们定（10 s），帧率反而更高。
 * 连上满 ROLE_REQ_DELAY_MS 才申请——R1.5 是在连接稳定以后切的，别挤在连接建立那一下。 */
#define ROLE_REQ_DELAY_MS 3000u
static volatile uint8_t  s_role_req = 1;
static bool              s_rs_tried, s_lsto_done;
static uint32_t          s_conn_ms;      /* R1.6：这次连接做 link_params 的时刻 */
""")
p.rep("""    /* R1.5：「接收器当主」试验。开着、我们是从、这次连接还没试过 → 申请当主；
     * 当上主（不管是切换来的还是连上时就是）→ 监督超时写成 10 s，再记一条 LINK_PARAM。 */
    if (s_role_req && s_role == 0x01 && !s_rs_tried) {
""", """    /* R1.5：「接收器当主」。开着、我们是从、这次连接还没试过、连上满 3 s（R1.6）→ 申请当主；
     * 当上主（不管是切换来的还是连上时就是）→ 监督超时写成 10 s，再记一条 LINK_PARAM。 */
    if (s_role_req && s_role == 0x01 && !s_rs_tried &&
        (uint32_t)((uint32_t)(bflb_mtimer_get_time_us() / 1000ULL) - s_conn_ms) >= ROLE_REQ_DELAY_MS) {
""")
p.rep("""    s_rs_tried = false;                    /* R1.5：每次连上重新来 */
""", """    s_rs_tried = false;                    /* R1.5：每次连上重新来 */
    s_conn_ms = (uint32_t)(bflb_mtimer_get_time_us() / 1000ULL);
""")
files.append(p)

p = P(os.path.join(SRC, 'bt_hid_host.h'))
p.rep("""/* R1.5：「接收器当主」试验开关（0xF6 命令 0x11，ISR 里调安全：只置标志）。1 = 之后每次连上都申请当主 */
""", """/* R1.5：「接收器当主」开关（0xF6 命令 0x11，ISR 里调安全：只置标志）。1 = 每次连上都申请当主。
 * R1.6 起默认 1；发 0 关掉（不落盘，接收器拔插后恢复 1） */
""")
files.append(p)

p = P(os.path.join(SRC, 'usb_gamepad.c'))
p.rep("""            /* R1.5：「接收器当主」试验开关（不落盘）：[1] = 1 开 / 0 关。开着时 bt 任务下一次链路诊断就申请 */
""", """            /* R1.5：「接收器当主」开关（不落盘）：[1] = 1 开 / 0 关。开着时 bt 任务下一次链路诊断就申请。
             * R1.6 起默认开，这条命令主要用来关（拔插恢复开） */
""")
files.append(p)

# ======================= main.c =======================
p = P(os.path.join(SRC, 'main.c'))
p.rep("""#define USB_OUTPUT_BUF_SZ    64  /* Report ID (1) + SetStateData (up to 63 for DSE) */
""", """#define USB_OUTPUT_BUF_SZ    64  /* Report ID (1) + SetStateData (up to 63 for DSE) */

/* R1.6：输出报告（震动 / 灯 / 扳机）排队时带上 USB ISR 收到的时刻（µs 低 32 位），
 * bt 任务转发时量「USB 收到 → 交给蓝牙」的延迟进 PERF。 */
typedef struct {
    uint32_t t_us;
    uint8_t  data[USB_OUTPUT_BUF_SZ];
} usb_out_msg_t;
""")
p.rep("""    uint8_t msg[USB_OUTPUT_BUF_SZ];
    uint16_t copy = (len <= USB_OUTPUT_BUF_SZ) ? len : USB_OUTPUT_BUF_SZ;
    memcpy(msg, data, copy);
    if (copy < USB_OUTPUT_BUF_SZ)
        memset(msg + copy, 0, USB_OUTPUT_BUF_SZ - copy);
    if (xQueueIsQueueFullFromISR(output_queue)) {
        uint8_t dummy[USB_OUTPUT_BUF_SZ];
        xQueueReceiveFromISR(output_queue, dummy, NULL);
        out_drop_count++;
    }
    out_isr_ts_us = bflb_mtimer_get_time_us();
    xQueueSendToBackFromISR(output_queue, msg, NULL);
}
""", """    usb_out_msg_t msg;
    uint16_t copy = (len <= USB_OUTPUT_BUF_SZ) ? len : USB_OUTPUT_BUF_SZ;
    memcpy(msg.data, data, copy);
    if (copy < USB_OUTPUT_BUF_SZ)
        memset(msg.data + copy, 0, USB_OUTPUT_BUF_SZ - copy);
    bool dropped = false;
    if (xQueueIsQueueFullFromISR(output_queue)) {
        usb_out_msg_t dummy;
        xQueueReceiveFromISR(output_queue, &dummy, NULL);
        out_drop_count++;
        dropped = true;
    }
    out_isr_ts_us = bflb_mtimer_get_time_us();
    msg.t_us = (uint32_t)out_isr_ts_us;
    perf_out_rx(dropped);
    /* R1.6：直接叫醒 bt 任务（CPU 空着时不用等下一个 1 ms 节拍；同 ds5_usb_audio.c 的做法）。
     * 正在跑的是更高优先级的 usb / audio 任务时 woken 为假，什么都不变。 */
    BaseType_t woken = pdFALSE;
    xQueueSendToBackFromISR(output_queue, &msg, &woken);
    portYIELD_FROM_ISR(woken);
}
""")
p.rep("""    uint8_t usb_out[USB_OUTPUT_BUF_SZ];
    uint16_t idle_ticks = 0;
""", """    usb_out_msg_t usb_out;
    uint16_t idle_ticks = 0;
""")
p.rep("""            /* Pass-through output forwarding (matches DS5DongleRE):
             * Each USB output frame is independently forwarded to BT with
             * only config overlays applied — no flag accumulation or merging. */
            if (state_mgr_is_spk_active()) {
                int batch = 0;
                while (xQueueReceive(output_queue, usb_out, 0) == pdTRUE) {
                    uint8_t *payload = usb_out + 1;
                    apply_config_overlay(payload, DS5_USB_OUTPUT_PAYLOAD_LEN);
                    cache_output_state(payload);

                    uint8_t bt_out[DS5_BT_OUTPUT_REPORT_SIZE];
                    build_bt_output(payload, DS5_USB_OUTPUT_PAYLOAD_LEN,
                                    output_seq, bt_out);
                    output_seq = (output_seq + 1) & 0x0F;
                    bt_hid_host_send_output(bt_out, DS5_BT_OUTPUT_REPORT_SIZE);
                    batch++;
                }
                vTaskDelay(pdMS_TO_TICKS(24));
            } else if (xQueueReceive(output_queue, usb_out, pdMS_TO_TICKS(24)) == pdTRUE) {
                uint8_t *payload = usb_out + 1;
""", """            /* Pass-through output forwarding (matches DS5DongleRE):
             * Each USB output frame is independently forwarded to BT with
             * only config overlays applied — no flag accumulation or merging.
             * R1.6：手柄喇叭有声音时也收到就转发。原作者这时改成「每 24 ms 攒一批再发」，
             * 普通震动 / 灯 / 扳机因此平均晚 12 ms、最多 24 ms。发出去的包数一样（每份输出报告
             * 一个 0x31），只是不再扣着；蓝牙侧照旧一次只在飞一个（app_tx FIFO）。 */
            if (xQueueReceive(output_queue, &usb_out, pdMS_TO_TICKS(24)) == pdTRUE) {
                uint8_t *payload = usb_out.data + 1;
""")
p.rep("""                output_seq = (output_seq + 1) & 0x0F;
                bt_hid_host_send_output(bt_out, DS5_BT_OUTPUT_REPORT_SIZE);
next_output_iter:;
""", """                output_seq = (output_seq + 1) & 0x0F;
                bt_hid_host_send_output(bt_out, DS5_BT_OUTPUT_REPORT_SIZE);
                perf_out_fwd((uint32_t)bflb_mtimer_get_time_us() - usb_out.t_us);   /* R1.6 */
next_output_iter:;
""")
p.rep("""    output_queue = xQueueCreate(5, USB_OUTPUT_BUF_SZ);
""", """    output_queue = xQueueCreate(5, sizeof(usb_out_msg_t));   /* R1.6：元素带时间戳 */
""")
files.append(p)
assert 'state_mgr_is_spk_active()' not in p.s

# ======================= perf =======================
p = P(os.path.join(SRC, 'perf.h'))
p.rep('#include <stdint.h>\n', '#include <stdint.h>\n#include <stdbool.h>   /* R1.6：perf_out_rx(bool) */\n')
p.rep("""#define PERF_BLOB_SIZE  152   /* S36：96 → 116；R1.1：→ 136；R1.2：→ 140；R1.3：→ 144（链路诊断）；R1.4：→ 148；R1.5：→ 152 */
""", """#define PERF_BLOB_SIZE  160   /* S36：96 → 116；R1.1：→ 136；R1.2：→ 140；R1.3：→ 144（链路诊断）；R1.4：→ 148；R1.5：→ 152；R1.6：→ 160 */
""")
p.rep("""    uint16_t hci_evt;          /* 150 R1.5：连着时收到的普通 HCI 事件累计数（DIAG 只记前几条） */
} perf_blob_t;
""", """    uint16_t hci_evt;          /* 150 R1.5：连着时收到的普通 HCI 事件累计数（DIAG 只记前几条） */
    uint16_t out_win;          /* 152 R1.6：最近 1 s 电脑发来的输出报告数（震动 / 灯 / 扳机） */
    uint16_t out_lat_avg_us;   /* 154 R1.6：「USB 收到 → 交给蓝牙」平均延迟，µs */
    uint16_t out_lat_max_us;   /* 156 R1.6：最大延迟，µs（封顶 65535） */
    uint16_t out_drop;         /* 158 R1.6：开机以来排队满被丢掉的输出报告（封顶 65535） */
} perf_blob_t;
""")
p.rep("""void perf_hci_evt(void);                                                           /* R1.5 */
""", """void perf_hci_evt(void);                                                           /* R1.5 */
void perf_out_rx(bool dropped);       /* R1.6：USB ISR 里调（只加计数） */
void perf_out_fwd(uint32_t lat_us);   /* R1.6：bt 任务转发完一份输出报告后调 */
""")
files.append(p)

p = P(os.path.join(SRC, 'perf.c'))
p.rep("""void perf_hci_evt(void)
{
    g_perf.hci_evt++;
}
""", """void perf_hci_evt(void)
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
""")
p.rep("""        g_perf.gyro_win = (uint16_t)(gyro_n > 65535u ? 65535u : gyro_n);
    }
""", """        g_perf.gyro_win = (uint16_t)(gyro_n > 65535u ? 65535u : gyro_n);
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
""")
files.append(p)

# ======================= 主机工具 =======================
p = P(os.path.join(ROOT, 'tools', 'diag.py'))
p.rep("""PERF_SIZE = 152                         # S36：96 → 116；R1.1：→ 136；R1.2：→ 140；R1.3：→ 144（链路诊断）；R1.4：→ 148；R1.5：→ 152。一次 RPC 最多 122 B，分两段读
""", """PERF_SIZE = 160                         # S36：96 → 116；R1.1：→ 136；R1.2：→ 140；R1.3：→ 144（链路诊断）；R1.4：→ 148；R1.5：→ 152；R1.6：→ 160。一次 RPC 最多 122 B，分两段读
""")
p.rep("""        d.update(afh_lo=alo, afh_hi=ahi, hci_evt=nev)
    return d
""", """        d.update(afh_lo=alo, afh_hi=ahi, hci_evt=nev)
    if len(buf) >= 160:
        (ow, oavg, omax, odrop) = struct.unpack_from('<HHHH', buf, 152)
        d.update(out_win=ow, out_lat_avg_us=oavg, out_lat_max_us=omax, out_drop=odrop)
    return d
""")
p.rep("""        s += ' HCI事件%d' % d['hci_evt']
    return s
""", """        s += ' HCI事件%d' % d['hci_evt']
    if 'out_win' in d:
        s += ' | 输出报告 %d/s 转发延迟 均%.2f/最大%.2fms 排队丢%d' % (
            d['out_win'], d['out_lat_avg_us'] / 1000.0, d['out_lat_max_us'] / 1000.0, d['out_drop'])
    return s
""")
files.append(p)

p = P(os.path.join(ROOT, 'tools', 'link_tune.py'))
p.rep('''"""R1.5：「接收器当主」试验开关（0xF6 命令 0x11，不落盘：接收器断电即恢复默认「关」）。

用法：python -X utf8 tools/link_tune.py 1 [秒数]   打开：之后每次连上都申请当主（正连着的话 2 s 内就申请）
      python -X utf8 tools/link_tune.py 0 [秒数]   关闭（已经当上主的这次连接不会切回去，重连后恢复手柄当主）
''', '''"""「接收器当主」开关（0xF6 命令 0x11，不落盘）。R1.5 默认关；**R1.6 起默认开**（拔插恢复开）。

用法：python -X utf8 tools/link_tune.py 0 [秒数]   关闭（已经当上主的这次连接不会切回去，重连后恢复手柄当主）
      python -X utf8 tools/link_tune.py 1 [秒数]   打开：之后每次连上都申请当主（正连着的话几秒内就申请）
''')
files.append(p)

for f in files:
    f.save()

import ast
for t in ('diag.py', 'link_tune.py'):
    ast.parse(open(os.path.join(ROOT, 'tools', t), encoding='utf-8').read())
print('R1.6 PATCH DONE')
