# -*- coding: utf-8 -*-
"""S36 补丁：引擎定时器按需运行 + 每帧快速路径 + 音频编码参数运行时可调 + 发送失败分类 + DIAG 限流。
用法：python tools/_s36_patch.py   （在任意目录运行均可）"""
import os
import re
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _patchlib import P

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
os.chdir(os.path.join(ROOT, 'firmware', 'src'))

# ======================= ds5map.c =======================
p = P('ds5map.c')

# 状态变量
p.rep("""static bool g_managed[MAP_BTN_COUNT];
""", """static bool g_managed[MAP_BTN_COUNT];

/* S36：每帧快速路径。上一帧完整路径算出"没有任何时间相关的东西在跑"且这一帧的按键位 +
 * 触摸板触点与上一帧完全一样 → 直接套用上一帧的映射结果，跳过整条处理链。
 * g_force_full 由 RPC 写入 / 定时器回调 / 断开置位，逼下一帧走完整路径重新评估。 */
static bool          g_fast_ok;
static volatile bool g_force_full = true;
static uint8_t       g_fast_in[11];     /* p[7..9] + p[32..39] */
static uint8_t       g_fast_out[3];     /* 映射后的 p[7..9] */
static uint8_t       g_fast_l2, g_fast_r2;   /* 0 不改 / 1 置 0xFF / 2 置 0x00 */
""")

# 定时器声明挪到回调前面（回调里要引用它）
m = re.search(r"\nstatic TimerHandle_t s_tick_timer[^\n]*\n", p.s)
if not m:
    sys.exit('ds5map.c: s_tick_timer declaration not found')
decl = m.group(0)
p.s = p.s.replace(decl, "\n", 1)

p.rep("""static void tick_timer_cb(TimerHandle_t t)
{
    (void)t;
    MAP_LOCK();
    tick_internal();
    MAP_UNLOCK();
}
""", decl.strip('\n') + """

/* S36：1 ms 定时器改按需运行。只有宏 / 战备序列在放、动作表非空、或有映射键按着（长按与
 * 连发要计时）时才让它跑；都没有就在回调里把自己停掉，下一帧看到有需要再启动。
 * 空闲时省掉每毫秒一次的抢锁 + 两次任务切换（S35 实测定时器任务占 CPU 10%）。 */
static bool tick_needed(void)
{
    if (g_mp.active || g_sp.active)
        return true;
    for (int i = 0; i < ACT_MAX; i++)
        if (g_acts[i].used)
            return true;
    for (int b = 0; b < MAP_BTN_V1; b++)
        if (g_managed[b] && g_btn[b].raw)
            return true;
    return false;
}

/* 任务上下文调用。usb 任务每个走完整路径的帧都调一次，不只在按下沿：定时器自己停掉和
 * 这里启动之间有个很小的竞争窗，每帧都看一眼就能在 1~2 ms 内补上。 */
static void tick_arm_if_needed(void)
{
    if (s_tick_timer && tick_needed() && xTimerIsTimerActive(s_tick_timer) == pdFALSE)
        xTimerStart(s_tick_timer, 0);
}

static void tick_timer_cb(TimerHandle_t t)
{
    (void)t;
    MAP_LOCK();
    tick_internal();
    if (!tick_needed())
        xTimerStop(s_tick_timer, 0);
    MAP_UNLOCK();
}
""")

# tick_internal 末尾：改了状态就逼下一帧走完整路径
p.rep("""    macro_tick(now);
    strat_tick(now);
}
""", """    macro_tick(now);
    strat_tick(now);
    g_force_full = true;   /* S36：定时器可能改了动作/宏/层状态 */
}
""")

# 快速路径入口：放在原有"什么都没配"捷径之后
p.rep("""        memcpy(g_tlm_out, p, INPUT_LEN);
        g_tlm_seq++;
        MAP_UNLOCK();
        return;
    }

    if (gyro_track) {
""", """        memcpy(g_tlm_out, p, INPUT_LEN);
        g_tlm_seq++;
        MAP_UNLOCK();
        return;
    }

    /* S36：输入没变就复用上一帧的结果。手柄 600~770 帧/s 里绝大多数帧只有摇杆 / 陀螺仪在变，
     * 按键位和触摸板触点与上一帧完全一样，映射结果也必然一样。 */
    if (g_fast_ok && !g_force_full && !gyro_track && !g_tp_on &&
        memcmp(&p[7], g_fast_in, 3) == 0 && memcmp(&p[32], g_fast_in + 3, 8) == 0) {
        p[7] = g_fast_out[0];
        p[8] = g_fast_out[1];
        p[9] = g_fast_out[2];
        if (g_fast_l2)
            p[4] = (g_fast_l2 == 1) ? 0xFF : 0x00;
        if (g_fast_r2)
            p[5] = (g_fast_r2 == 1) ? 0xFF : 0x00;
        memcpy(g_tlm_out, p, INPUT_LEN);
        g_tlm_seq++;
        MAP_UNLOCK();
        return;
    }
    g_force_full = false;
    uint8_t fast_in[11];
    memcpy(fast_in, &p[7], 3);
    memcpy(fast_in + 3, &p[32], 8);

    if (gyro_track) {
""")

# L2/R2 覆盖记号
p.rep("""    if (l2_raw != l2_out)
        p[4] = l2_out ? 0xFF : 0x00;
    if (r2_raw != r2_out)
        p[5] = r2_out ? 0xFF : 0x00;
""", """    if (l2_raw != l2_out)
        p[4] = l2_out ? 0xFF : 0x00;
    if (r2_raw != r2_out)
        p[5] = r2_out ? 0xFF : 0x00;
    g_fast_l2 = (l2_raw != l2_out) ? (l2_out ? 1 : 2) : 0;   /* S36 */
    g_fast_r2 = (r2_raw != r2_out) ? (r2_out ? 1 : 2) : 0;
""")

# 完整路径收尾：记下输入/输出，判定是否可走快速路径，按需启动定时器
p.rep("""    synth_mouse(mdx, mdy, g_wheel_pending);
    g_wheel_pending = 0;
    synth_kbd();

    MAP_UNLOCK();
}
""", """    synth_mouse(mdx, mdy, g_wheel_pending);
    g_wheel_pending = 0;
    synth_kbd();

    /* S36：记下这一帧的输入与输出供下一帧复用；判定现在有没有时间相关的东西在跑 */
    memcpy(g_fast_in, fast_in, sizeof(g_fast_in));
    g_fast_out[0] = p[7];
    g_fast_out[1] = p[8];
    g_fast_out[2] = p[9];
    g_fast_ok = !tick_needed() && !g_gyro_live && !g_tp_on && !g_tp_stick &&
                g_last_mouse_btns == 0 && g_wheel_pending == 0;
    tick_arm_if_needed();

    MAP_UNLOCK();
}
""")

# RPC 写 / 配置派生 / 断开 → 逼完整路径
p.rep("""    if (op == 0)                 /* NOP: keep current response buffer */
        return;
""", """    if (op == 0)                 /* NOP: keep current response buffer */
        return;
    if (op != MAP_OP_READ)
        g_force_full = true;     /* S36：配置 / 动作可能变了，下一帧别走快速路径 */
""")
p.rep("""static void cfg_derive(void)
{
    g_any_managed = false;
""", """static void cfg_derive(void)
{
    g_force_full = true;   /* S36 */
    g_any_managed = false;
""")
p.rep("""    memset(g_last_kbd, 0, sizeof(g_last_kbd));
    g_last_mouse_btns = 0;

    MAP_UNLOCK();
}
""", """    memset(g_last_kbd, 0, sizeof(g_last_kbd));
    g_last_mouse_btns = 0;
    g_fast_ok = false;      /* S36 */
    g_force_full = true;

    MAP_UNLOCK();
}
""")
p.save()

# ======================= audio.c / audio.h =======================
p = P('audio.c')
p.rep("""static uint8_t audio_seq;   /* sequence counter for 0x39 audio report */
""", """static uint8_t audio_seq;   /* sequence counter for 0x39 audio report */

/* S36：编码参数运行时可调（0xF6 命令 0x10，不落盘）。默认与之前完全一样：160 kbps / 200 B。
 * 思路：喇叭就那样，码率从 160 降到几十 kbps 听不出差别，但编码器要搜索的脉冲数少了
 * 大半，编码时间（S35 实测一帧 10 ms 要 11~16 ms）会明显缩短。
 *   tune_mode 0 = 编码产物用 opus_packet_pad() 补成合法的 200 B 带填充包，线上格式不变；
 *   tune_mode 1 = 按真实长度发（长度字段 < 200，两帧紧挨着放），验证手柄认不认变长。 */
static volatile uint16_t tune_kbps_req = 160;
static volatile uint8_t  tune_bw_req, tune_mode_req, tune_cx_req;
static volatile bool     tune_pending;
static uint16_t opus_frame_bytes = OPUS_OUT_SIZE;   /* CBR 下每帧产物字节数 = kbps * 1.25 */
static uint8_t  tune_mode;
static uint16_t slot_len[2] = { OPUS_OUT_SIZE, OPUS_OUT_SIZE };

void audio_set_tuning(uint16_t kbps, uint8_t bw, uint8_t mode, uint8_t complexity)
{
    tune_kbps_req = kbps;
    tune_bw_req   = bw;
    tune_mode_req = mode;
    tune_cx_req   = complexity;
    tune_pending  = true;   /* 可能来自 USB ISR：由 audio_task 在两次编码之间应用 */
}
""")
p.rep("""    bool speaker_enabled = !config_get()->disable_speaker;
    if (speaker_enabled) {
        pkt[140] = plug_headset ? DS5_AUDIO_TAG_HEADSET : DS5_AUDIO_TAG_SPEAKER;
        pkt[141] = OPUS_OUT_SIZE;
        memcpy(pkt + 142, opus_slots[0], OPUS_OUT_SIZE);
        memcpy(pkt + 142 + OPUS_OUT_SIZE, opus_slots[1], OPUS_OUT_SIZE);
    }
""", """    bool speaker_enabled = !config_get()->disable_speaker;
    if (speaker_enabled) {
        /* S36：两帧用同一个长度字段；万一两帧长度不同（只在改参数那一瞬），短的补填充到长的 */
        uint16_t n = slot_len[0] > slot_len[1] ? slot_len[0] : slot_len[1];
        if (n > OPUS_OUT_SIZE)
            n = OPUS_OUT_SIZE;
        for (int i = 0; i < 2; i++)
            if (slot_len[i] < n && opus_packet_pad(opus_slots[i], slot_len[i], n) != OPUS_OK)
                memset(opus_slots[i] + slot_len[i], 0, n - slot_len[i]);
        pkt[140] = plug_headset ? DS5_AUDIO_TAG_HEADSET : DS5_AUDIO_TAG_SPEAKER;
        pkt[141] = (uint8_t)n;
        memcpy(pkt + 142, opus_slots[0], n);
        memcpy(pkt + 142 + n, opus_slots[1], n);
    }
""")
p.rep("""    if (ret) {
        perf_audio_sendfail();
        fail_log_count++;
""", """    if (ret) {
        perf_audio_sendfail(ret);   /* S36：按原因分类（-2 缓冲池空 / -1 没连接 / 其它 = l2cap 错误码） */
        fail_log_count++;
""")
p.rep("""            if (a_active && a_read && a_bt)
            {
                bool speaker_on = !config_get()->disable_speaker;
""", """            if (a_active && a_read && a_bt)
            {
                if (tune_pending) {
                    /* S36：在两次编码之间应用新参数 */
                    tune_pending = false;
                    static const int bw_tab[6] = {
                        OPUS_AUTO, OPUS_BANDWIDTH_NARROWBAND, OPUS_BANDWIDTH_MEDIUMBAND,
                        OPUS_BANDWIDTH_WIDEBAND, OPUS_BANDWIDTH_SUPERWIDEBAND, OPUS_BANDWIDTH_FULLBAND };
                    uint16_t kbps = tune_kbps_req;
                    if (kbps < 8)   kbps = 8;
                    if (kbps > 160) kbps = 160;          /* 10 ms 一帧最多 200 B */
                    uint8_t bw = tune_bw_req < 6 ? tune_bw_req : 0;
                    uint8_t cx = tune_cx_req > 10 ? 10 : tune_cx_req;
                    opus_encoder_ctl(encoder, OPUS_SET_BITRATE((opus_int32)kbps * 1000));
                    opus_encoder_ctl(encoder, OPUS_SET_BANDWIDTH(bw_tab[bw]));
                    opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(cx));
                    opus_frame_bytes = (uint16_t)((kbps * 125u) / 100u);
                    if (opus_frame_bytes > OPUS_OUT_SIZE)
                        opus_frame_bytes = OPUS_OUT_SIZE;
                    tune_mode = tune_mode_req ? 1 : 0;
                    perf_audio_tune(kbps, bw, tune_mode, cx);
                    LOG_INF("[AUDIO] tune: %u kbps bw=%u mode=%u cx=%u -> %u B/frame\\n",
                            kbps, bw, tune_mode, cx, opus_frame_bytes);
                }
                bool speaker_on = !config_get()->disable_speaker;
""")
p.rep("""                        if (encoded <= 0) {
                            LOG_ERR("[AUDIO] Opus encode error: %d\\n", encoded);
                            memset(opus_slots[slot], 0, OPUS_OUT_SIZE);
                        } else if (encoded < OPUS_OUT_SIZE) {
                            memset(opus_slots[slot] + encoded, 0,
                                   OPUS_OUT_SIZE - encoded);
                        }
                    } else {
                        memset(opus_slots[slot], 0, OPUS_OUT_SIZE);
                    }
""", """                        if (encoded <= 0) {
                            LOG_ERR("[AUDIO] Opus encode error: %d\\n", encoded);
                            memset(opus_slots[slot], 0, OPUS_OUT_SIZE);
                            slot_len[slot] = OPUS_OUT_SIZE;
                        } else {
                            /* S36：目标帧长 = 填充模式 200 B / 真实长度模式 opus_frame_bytes。
                             * 短了用 opus_packet_pad() 补成合法的带填充包（解码器忽略填充）。
                             * 不能直接补零：CELT 按包长分配比特，补零会解出噪声。 */
                            uint16_t want = tune_mode ? opus_frame_bytes : OPUS_OUT_SIZE;
                            if (want < (uint16_t)encoded)
                                want = (uint16_t)encoded;
                            if ((uint16_t)encoded < want &&
                                opus_packet_pad(opus_slots[slot], encoded, want) != OPUS_OK) {
                                memset(opus_slots[slot] + encoded, 0, OPUS_OUT_SIZE - encoded);
                                want = OPUS_OUT_SIZE;
                            }
                            slot_len[slot] = want;
                        }
                    } else {
                        memset(opus_slots[slot], 0, OPUS_OUT_SIZE);
                        slot_len[slot] = OPUS_OUT_SIZE;
                    }
""")
p.rep("""                        /* S32：第 4 次和第 10 次各记一条，看"险情"多久出现一次 */
                        if (send_fail_streak == 4 || send_fail_streak == 10)
                            diag_log(DIAG_EV_AUDIO, 3, send_fail_streak);
""", """                        /* S32：第 4 次和第 10 次各记一条；S36：限流 30 s 一条——S35 实测放视频时
                         * 它每 250 ms 一条，25 格的环全是它，掉线定位会失效 */
                        if (send_fail_streak == 4 || send_fail_streak == 10) {
                            static uint64_t last_diag_us;
                            uint64_t nowu = bflb_mtimer_get_time_us();
                            if (nowu - last_diag_us >= 30000000ULL) {
                                last_diag_us = nowu;
                                diag_log(DIAG_EV_AUDIO, 3, send_fail_streak);
                            }
                        }
""")
p.save()

p = P('audio.h')
p.rep("""/**
 * Check if the audio task was killed by the encode watchdog and respawn it.
""", """/* S36：运行时调编码参数（0xF6 命令 0x10）。kbps 8..160；bw 0 自动 / 1 NB / 2 MB / 3 WB / 4 SWB / 5 FB；
 * mode 0 = 补填充到 200 B（线上格式不变）/ 1 = 按真实长度发；complexity 0..10。ISR 可调，audio_task 应用。 */
void audio_set_tuning(uint16_t kbps, uint8_t bw, uint8_t mode, uint8_t complexity);

/**
 * Check if the audio task was killed by the encode watchdog and respawn it.
""")
p.save()

# ======================= bt_hid_host.c =======================
p = P('bt_hid_host.c')
p.rep("""            LOG_WRN("[HID] send_output direct: pool empty (len=%d)\\n", (int)len);
            return -1;
""", """            LOG_WRN("[HID] send_output direct: pool empty (len=%d)\\n", (int)len);
            return -2;   /* S36：和"没连接"(-1) 区分开，audio.c 按原因计数 */
""")
p.save()

# ======================= perf.h / perf.c =======================
p = P('perf.h')
p.rep("""#define PERF_BLOB_SIZE  96
""", """#define PERF_BLOB_SIZE  116   /* S36：96 → 116，尾部加发送失败分类与当前编码参数 */
""")
p.rep("""    } task[PERF_TASK_MAX];     /* 32 .. 95 */
} perf_blob_t;
""", """    } task[PERF_TASK_MAX];     /* 32 .. 95 */
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
} perf_blob_t;
""")
p.rep("""void perf_audio_sendfail(void);                                /* 音频任务：0x39 发送失败 */
""", """void perf_audio_sendfail(int err);                             /* 音频任务：0x39 发送失败（err 见 fail_* 注释） */
void perf_audio_tune(uint16_t kbps, uint8_t bw, uint8_t mode, uint8_t cx);   /* S36：记录当前编码参数 */
""")
p.save()

p = P('perf.c')
p.rep("""static perf_blob_t g_perf = { .magic = PERF_MAGIC };
""", """static perf_blob_t g_perf = { .magic = PERF_MAGIC, .tune_kbps = 160 };
""")
p.rep("""void perf_audio_sendfail(void)
{
    g_perf.audio_send_fail++;
}
""", """void perf_audio_sendfail(int err)
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
""")
p.save()

# ======================= usb_gamepad.c =======================
p = P('usb_gamepad.c')
p.rep('#define FIRMWARE_VERSION "LCT616-DS5 3.18 S35"', '#define FIRMWARE_VERSION "LCT616-DS5 3.18 S36"')
p.rep("""        } else if (cmd == 0x03) {
            usb_reset_pending = true;
        }
        return;
    }
""", """        } else if (cmd == 0x03) {
            usb_reset_pending = true;
        } else if (cmd == 0x10 && payload_len >= 6) {
            /* S36：音频编码参数运行时调（不落盘）：[1..2] kbps LE，[3] 带宽码 0 自动/1 NB/2 MB/3 WB/4 SWB/5 FB，
             * [4] 模式 0 补填充到 200 B / 1 按真实长度发，[5] 复杂度 0..10。audio_task 在两次编码之间应用。 */
            extern void audio_set_tuning(uint16_t kbps, uint8_t bw, uint8_t mode, uint8_t complexity);
            audio_set_tuning((uint16_t)(payload[1] | (payload[2] << 8)), payload[3], payload[4], payload[5]);
            LOG_INF("[USB] CMD 0x10: audio tune\\n");
        }
        return;
    }
""")
p.save()

# ======================= 主机侧 diag.py =======================
os.chdir(os.path.join(ROOT, 'tools'))
p = P('diag.py')
p.rep("""PERF_SIZE = 96
""", """PERF_SIZE = 116                         # S36：96 → 116（尾部多了发送失败分类 + 当前编码参数）
""")
p.rep("""    buf = rpc_read(h, REG_PERF, 0, PERF_SIZE)
    if not buf or len(buf) < PERF_SIZE:
        return None
""", """    buf = rpc_read(h, REG_PERF, 0, PERF_SIZE)
    if not buf or len(buf) < 96:          # S35 固件只有 96 B，也能解析
        return None
""")
p.rep("""    return dict(uptime_s=up, blocks=blocks, overruns=over, send_fail=sfail,
                enc_max_us=enc_max, blk_max_us=blk_max, enc_win_us=enc_win,
                blk_win_us=blk_win, wd_kills=wd, tasks=tasks)
""", """    d = dict(uptime_s=up, blocks=blocks, overruns=over, send_fail=sfail,
             enc_max_us=enc_max, blk_max_us=blk_max, enc_win_us=enc_win,
             blk_win_us=blk_win, wd_kills=wd, tasks=tasks)
    if len(buf) >= 116:
        (fp, fl, fn, le, kbps, bw, mode, cx) = struct.unpack_from('<IIIhHBBB', buf, 96)
        d.update(fail_pool=fp, fail_l2cap=fl, fail_noconn=fn, last_err=le,
                 tune_kbps=kbps, tune_bw=bw, tune_mode=mode, tune_cx=cx)
    return d
""")
p.rep("""    cpu = ' '.join('%s=%.1f%%' % (n, pm / 10.0) for n, pm in d['tasks'])
    return ('音频块=%d 丢块=%d 发送失败=%d 看门狗触发=%d 编码max=%.1f/%.1fms 块max=%.1f/%.1fms | CPU %s'
            % (d['blocks'], d['overruns'], d['send_fail'], d['wd_kills'],
               d['enc_win_us'] / 1000.0, d['enc_max_us'] / 1000.0,
               d['blk_win_us'] / 1000.0, d['blk_max_us'] / 1000.0, cpu))
""", """    cpu = ' '.join('%s=%.1f%%' % (n, pm / 10.0) for n, pm in d['tasks'])
    s = ('音频块=%d 丢块=%d 发送失败=%d 看门狗触发=%d 编码max=%.1f/%.1fms 块max=%.1f/%.1fms | CPU %s'
         % (d['blocks'], d['overruns'], d['send_fail'], d['wd_kills'],
            d['enc_win_us'] / 1000.0, d['enc_max_us'] / 1000.0,
            d['blk_win_us'] / 1000.0, d['blk_max_us'] / 1000.0, cpu))
    if 'fail_pool' in d:
        s += (' | 失败分类 池空=%d l2cap=%d 未连=%d 最近错=%d | 编码 %dkbps bw=%d mode=%d cx=%d'
              % (d['fail_pool'], d['fail_l2cap'], d['fail_noconn'], d['last_err'],
                 d['tune_kbps'], d['tune_bw'], d['tune_mode'], d['tune_cx']))
    return s
""")
p.save()
print('PATCH S36 DONE')
