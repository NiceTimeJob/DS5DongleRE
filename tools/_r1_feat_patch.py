# -*- coding: utf-8 -*-
"""R1 功能补丁（固件侧）：
  ① OUT_MEDIA 媒体键（Consumer Control 位图扩到 7 位：音量+/−、静音、播放/暂停、下一曲、上一曲、停止）
  ② 配置项 led_quiet_connected「静默 LED」：手柄连上就灭灯，等待/对频照常闪（CONFIG_VERSION 7，23 B）
  ③ 引擎定时器 1 ms → MAP_TICK_MS 5 ms
用法：python tools/_r1_feat_patch.py"""
import os
import re
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _patchlib import P

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
os.chdir(os.path.join(ROOT, 'firmware', 'src'))

# ---------------- ds5map.h ----------------
p = P('ds5map.h')
p.rep("""    OUT_STRAT = 7,   /* val = 战备表下标 (0..MAP_STRAT_COUNT-1)：把
                      *   g_cfg.strats[] 里的方向序列按十字键打出去
                      *   （《绝地潜兵 2》战备宏，不占 16 个宏名额） */
};
""", """    OUT_STRAT = 7,   /* val = 战备表下标 (0..MAP_STRAT_COUNT-1)：把
                      *   g_cfg.strats[] 里的方向序列按十字键打出去
                      *   （《绝地潜兵 2》战备宏，不占 16 个宏名额） */
    OUT_MEDIA = 8,   /* R1：媒体键。val = Consumer Control 位图的位号：
                      *   0 音量+  1 音量−  2 静音  3 播放/暂停  4 下一曲  5 上一曲  6 停止
                      *   按住期间位置 1，松开清 0（系统按"按下"沿触发） */
};
""")
p.rep("""#define MAP_TURBO_UNIT_MS    10
""", """#define MAP_TURBO_UNIT_MS    10
/* R1：引擎定时器周期。S36 起只在有宏/连发/长按在计时时运行，所以周期只影响那些时候的
 * 分辨率与开销：5 ms 对宏步进、连发（最小半周期 5 ms）、长按/双击窗口都够用，
 * 比 1 ms 少跑 80%。宏步间隔会按 5 ms 向上取整。 */
#define MAP_TICK_MS          5
""")
p.rep(""" *   44..47 reserved (清零)
""", """ *   44   u8  媒体键位图（R1，OUT_MEDIA 的 val 位号：bit0 音量+ … bit6 停止）
 *   45..47 reserved (清零)
""")
p.save()

# ---------------- ds5map.c ----------------
p = P('ds5map.c')
p.rep("""static uint8_t g_last_mouse_btns;
""", """static uint8_t g_last_mouse_btns;
static uint8_t g_last_media;         /* R1：最近一次发出去的 Consumer Control 位图（OUT_MEDIA） */
static bool    g_media_pending;      /* 想发但端点忙没发出去：下一帧必须再试（别走快速路径） */
""")
p.rep("""    if (usb_gamepad_kbd_ready())
        usb_gamepad_send_mouse_report(btns, (int8_t)dx, (int8_t)dy, wheel);
}
""", """    if (usb_gamepad_kbd_ready())
        usb_gamepad_send_mouse_report(btns, (int8_t)dx, (int8_t)dy, wheel);
}

/* R1：媒体键（Consumer Control，报告 ID 2）：把所有生效中的 OUT_MEDIA 动作并成 1 字节位图。
 * 和键盘报告不同：没发出去就不记为"已发"，下一帧重试——播放/暂停这种按一下的键丢一次就是丢了。 */
static void synth_media(void)
{
    uint8_t bits = 0;
    for (int i = 0; i < ACT_MAX; i++) {
        const act_t *a = &g_acts[i];
        if (!a->used || a->out != OUT_MEDIA || a->val > 7 || !act_on(a))
            continue;
        bits |= (uint8_t)(1u << a->val);
    }
    if (bits == g_last_media) {
        g_media_pending = false;
        return;
    }
    if (usb_gamepad_kbd_ready() && usb_gamepad_send_consumer_report(bits) == 0) {
        g_last_media = bits;
        g_media_pending = false;
    } else {
        g_media_pending = true;
    }
}
""")
p.rep("""    synth_kbd();

    /* S36：记下这一帧的输入与输出供下一帧复用；判定现在有没有时间相关的东西在跑 */
""", """    synth_kbd();
    synth_media();

    /* S36：记下这一帧的输入与输出供下一帧复用；判定现在有没有时间相关的东西在跑 */
""")
p.rep("""    g_fast_ok = !tick_needed() && !g_gyro_live && !g_tp_on && !g_tp_stick &&
                g_last_mouse_btns == 0 && g_wheel_pending == 0;
""", """    g_fast_ok = !tick_needed() && !g_gyro_live && !g_tp_on && !g_tp_stick &&
                g_last_mouse_btns == 0 && g_wheel_pending == 0 && !g_media_pending;
""")
p.rep("""                if (s->out == OUT_KBD)
                    g_has_kbd = true;
""", """                if (s->out == OUT_KBD || s->out == OUT_MEDIA)
                    g_has_kbd = true;          /* 媒体键走键鼠接口（报告 ID 2） */
""")
p.rep("""        if (s->out == OUT_KBD)
            g_has_kbd = true;
        if (s->out == OUT_MOUSE)
            g_has_mouse = true;
        if (s->out == OUT_MACRO) {
            g_has_kbd = true;
            g_has_mouse = true;
        }
    }
    g_tp_on = (g_cfg.tp.single != MAP_TP_NATIVE) ||
""", """        if (s->out == OUT_KBD || s->out == OUT_MEDIA)
            g_has_kbd = true;
        if (s->out == OUT_MOUSE)
            g_has_mouse = true;
        if (s->out == OUT_MACRO) {
            g_has_kbd = true;
            g_has_mouse = true;
        }
    }
    g_tp_on = (g_cfg.tp.single != MAP_TP_NATIVE) ||
""")
p.rep("""    memset(g_last_kbd, 0, sizeof(g_last_kbd));
    g_last_mouse_btns = 0;
    g_fast_ok = false;      /* S36 */
""", """    if (g_last_media && usb_gamepad_kbd_ready())
        usb_gamepad_send_consumer_report(0);     /* R1：松开媒体键 */
    g_last_media = 0;
    g_media_pending = false;
    memset(g_last_kbd, 0, sizeof(g_last_kbd));
    g_last_mouse_btns = 0;
    g_fast_ok = false;      /* S36 */
""")
p.rep("""    out[22] = g_tlm_wheel_seq;
""", """    out[22] = g_tlm_wheel_seq;
    out[44] = g_last_media;                      /* R1：媒体键位图 */
""")
p.rep("""        s_tick_timer = xTimerCreate("ds5map", pdMS_TO_TICKS(1), pdTRUE,
""", """        s_tick_timer = xTimerCreate("ds5map", pdMS_TO_TICKS(MAP_TICK_MS), pdTRUE,
""")
p.save()

# ---------------- usb_gamepad.c / .h ----------------
p = P('usb_gamepad.c')
p.rep("""    0x95, 0x03,       /*   Report Count (3) — 3 bits: VolUp, VolDn, Mute */
    0x09, 0xE9,       /*   Usage (Volume Increment) — bit 0 */
    0x09, 0xEA,       /*   Usage (Volume Decrement) — bit 1 */
    0x09, 0xE2,       /*   Usage (Mute)             — bit 2 */
    0x81, 0x02,       /*   Input (Data,Var,Abs) */
    0x95, 0x01,       /*   Report Count (1) */
    0x75, 0x05,       /*   Report Size (5) — padding */
    0x81, 0x01,       /*   Input (Const) */
""", """    0x95, 0x07,       /*   Report Count (7) — R1：7 位媒体键 */
    0x09, 0xE9,       /*   Usage (Volume Increment)    — bit 0 */
    0x09, 0xEA,       /*   Usage (Volume Decrement)    — bit 1 */
    0x09, 0xE2,       /*   Usage (Mute)                — bit 2 */
    0x09, 0xCD,       /*   Usage (Play/Pause)          — bit 3 */
    0x09, 0xB5,       /*   Usage (Scan Next Track)     — bit 4 */
    0x09, 0xB6,       /*   Usage (Scan Previous Track) — bit 5 */
    0x09, 0xB7,       /*   Usage (Stop)                — bit 6 */
    0x81, 0x02,       /*   Input (Data,Var,Abs) */
    0x95, 0x01,       /*   Report Count (1) */
    0x75, 0x01,       /*   Report Size (1) — padding */
    0x81, 0x01,       /*   Input (Const) */
""")
# 数组定长：必须按初始化列表的真实字节数改（S33/S34 的坑）
m = re.search(r'kbd_report_desc\[KBD_REPORT_DESC_SIZE\]\s*=\s*\{(.*?)\n\};', p.s, re.S)
body = re.sub(r'/\*.*?\*/', '', m.group(1), flags=re.S)
n = len(re.findall(r'0x[0-9A-Fa-f]{2}', body))
print('kbd_report_desc bytes =', n)
assert n == 146, n
p.rep("#define KBD_REPORT_DESC_SIZE 138  /* Keyboard + Consumer + Mouse */",
      "#define KBD_REPORT_DESC_SIZE 146  /* Keyboard + Consumer(7 位媒体键, R1) + Mouse；= 初始化列表字节数，改描述符必须同步改 */")
p.rep("""int usb_gamepad_send_mouse_report(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel)
{
""", """/* R1：媒体键（Consumer Control，报告 ID 2，1 字节位图：bit0 音量+ 1 音量− 2 静音 3 播放/暂停
 * 4 下一曲 5 上一曲 6 停止）。与键盘/鼠标共用同一个中断 IN 端点。 */
int usb_gamepad_send_consumer_report(uint8_t bits)
{
    if (!usb_configured || !kbd_registered || kbd_ep_busy)
        return -1;
    kbd_buf[0] = 0x02; /* Report ID 2 = Consumer Control */
    kbd_buf[1] = bits;
    kbd_ep_busy = true;
    kbd_ep_busy_since_us = bflb_mtimer_get_time_us();
    int ret = usbd_ep_start_write(0, USB_KBD_EP_IN, kbd_buf, 2);
    if (ret < 0)
        kbd_ep_busy = false;
    return ret;
}

int usb_gamepad_send_mouse_report(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel)
{
""")
p.save()

p = P('usb_gamepad.h')
p.rex(r"(int\s+usb_gamepad_send_mouse_report\([^\n]*\n)",
      r"\1int  usb_gamepad_send_consumer_report(uint8_t bits);   /* R1：媒体键位图（报告 ID 2） */\n")
p.save()

# ---------------- config.h / config.c ----------------
p = P('config.h')
p.rep("""    uint8_t audio_haptic;         /* 0=off, 1=auto(game priority), 2=force */
};
""", """    uint8_t audio_haptic;         /* 0=off, 1=auto(game priority), 2=force */
    uint8_t led_quiet_connected;  /* R1 bool：静默 LED——手柄连上就灭状态灯，等待/对频时照常闪 */
};
""")
p.rep("""#define CONFIG_VERSION  6
""", """/* R1（2026-10-14）：加 led_quiet_connected，结构体 22 → 23 字节。 */
#define CONFIG_VERSION  7
""")
p.save()
p = P('config.c')
p.rep("""    if (b->audio_haptic > 2)
        b->audio_haptic = 0;
""", """    if (b->audio_haptic > 2)
        b->audio_haptic = 0;
    if (b->led_quiet_connected > 1)
        b->led_quiet_connected = 0;
""")
p.rep("（现 22 字节 / v6，S33 删掉原作者的四个字段）", "（现 23 字节 / v7，R1 加「静默 LED」）")
p.save()

# ---------------- main.c ----------------
p = P('main.c')
p.rep("""    if (ds5_connected)
        p = LED_CONNECTED;
    else if (pairing_mode)
        p = LED_PAIRING;
""", """    if (ds5_connected)
        p = config_get()->led_quiet_connected ? LED_OFF : LED_CONNECTED;   /* R1：静默 LED，连上就灭 */
    else if (pairing_mode)
        p = LED_PAIRING;
""")
p.rep("""        {   /* S33：每 ~1 s 采一次各任务 CPU 占比，连同音频统计一起放进 MAP_REG_PERF */
""", """        {   /* R1：「静默 LED」开关实时生效：连着的时候翻转就立刻按新值刷新灯 */
            static bool prev_quiet;
            bool quiet = config_get()->led_quiet_connected != 0;
            if (quiet != prev_quiet) {
                prev_quiet = quiet;
                if (ds5_connected && !conn_led_off) {
                    if (battery_low)
                        led_status_set(LED_BLINK_BATTERY);
                    else
                        led_refresh_state();
                }
            }
        }

        {   /* S33：每 ~1 s 采一次各任务 CPU 占比，连同音频统计一起放进 MAP_REG_PERF */
""")
p.save()

# ---------------- 主机工具 ----------------
os.chdir(os.path.join(ROOT, 'tools'))
p = P('verify_fw.py')
p.rep("EXPECT_CFG_VER = 6", "EXPECT_CFG_VER = 7")
p.rep("print('     [ok] 新结构体（22 字节布局）已经在跑')", "print('     [ok] 新结构体（23 字节布局）已经在跑')")
p.save()
print('R1 FEATURE PATCH (firmware) DONE')
