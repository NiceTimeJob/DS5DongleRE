# -*- coding: utf-8 -*-
"""S37 补丁：
  ① 引擎"真正在计时"判定（按住映射键也能走快速路径、定时器也能停）
  ② 音频：真单声道编码器（照 3.19 "Encoder reinit %dch"）+ 默认 48 kbps 宽带（补填充保持 200 B 线上格式）
  ③ 连上后关 page/inquiry scan（照 3.20a/3.21 "scan OFF (ACL active)"），断开后既有路径重开；DIAG_EV_SCAN_SET
  ④ 残留清理：ds5map_tp_cycle()、usb_gamepad_send_consumer_report()、注释
用法：python tools/_s37_patch.py"""
import os
import re
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _patchlib import P

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
os.chdir(os.path.join(ROOT, 'firmware', 'src'))

# ======================= ds5map.c / ds5map.h =======================
p = P('ds5map.c')
p.rep("""static bool tick_needed(void)
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
""", """/* S37：只算"真正在计时"的东西。按着一个映射键本身不算（它的动作在松开沿由帧路径收尾），
 * 只有长按还没触发、动作带连发或定时结束、原生连发在跑、宏 / 战备在放，才需要每毫秒跑。
 * 这样游戏里按住被映射的扳机 / 奔跑键时，快速路径照样生效、定时器照样停着。
 * 双击 / 三击的时间窗不靠它：按下沿那边按时间戳判定，过期自然落到"重新计 1 次"。 */
static bool tick_needed(void)
{
    if (g_mp.active || g_sp.active)
        return true;
    for (int i = 0; i < ACT_MAX; i++) {
        const act_t *a = &g_acts[i];
        if (a->used && ((a->flags & MAP_FLAG_TURBO) || a->end_ms != 0))
            return true;
    }
    for (int b = 0; b < MAP_BTN_V1; b++) {
        const btn_rt_t *st = &g_btn[b];
        if (!g_managed[b] || !st->raw)
            continue;
        if (!st->hold_fired || st->nturbo_run)
            return true;
    }
    return false;
}
""")
p.rex(r"uint8_t ds5map_tp_cycle\(void\)\n\{.*?\n\}\n", "")
p.save()

p = P('ds5map.h')
p.rep("""uint8_t  ds5map_tp_cycle(void);        /* Create+触摸板：切单指目标，返回新值 */
""", "")
p.save()

# ======================= audio.c =======================
p = P('audio.c')
p.rep("""static int encoder_force_channels;
""", """static int enc_channels;   /* S37：编码器本身按 1/2 声道初始化（3.19 的 "Encoder reinit %dch"），不再 FORCE_CHANNELS */
""")
p.rep("""static volatile uint16_t tune_kbps_req = 160;
static volatile uint8_t  tune_bw_req, tune_mode_req, tune_cx_req;
static volatile bool     tune_pending;
static uint16_t opus_frame_bytes = OPUS_OUT_SIZE;   /* CBR 下每帧产物字节数 = kbps * 1.25 */
static uint8_t  tune_mode;
""", """/* S37 默认值（用户：「音频能出声就行，不需要很好的音质」）：48 kbps、宽带 8 kHz、补填充到 200 B。
 * 160 → 48 kbps 编码器要搜索的脉冲数少了七成，喇叭上听不出差别；线上包格式与之前完全一样。
 * 运行时 `tools/audio_tune.py 160 0 0 0` 可立刻回到老参数（不用重烧）。 */
#define AUDIO_DEF_KBPS  48
#define AUDIO_DEF_BW    3
#define AUDIO_DEF_CX    0
static volatile uint16_t tune_kbps_req = AUDIO_DEF_KBPS;
static volatile uint8_t  tune_bw_req = AUDIO_DEF_BW, tune_mode_req, tune_cx_req = AUDIO_DEF_CX;
static volatile bool     tune_pending;
static uint16_t cur_kbps = AUDIO_DEF_KBPS;
static uint8_t  cur_bw = AUDIO_DEF_BW, cur_cx = AUDIO_DEF_CX;
static uint16_t opus_frame_bytes = OPUS_OUT_SIZE;   /* CBR 下每帧产物字节数 = kbps * 1.25 */
static uint8_t  tune_mode;
""")
# 重采样：单声道编码器直接要单声道 PCM
p.rep("""static void resample_512_480(const int16_t *in, int16_t *out)
{
""", """static void resample_512_480(const int16_t *in, int16_t *out, bool mono)
{
""")
p.rep("""        out[i * 2]     = (int16_t)l;
        out[i * 2 + 1] = (int16_t)r;
    }
}
""", """        if (mono) {
            out[i] = (int16_t)((l + r) >> 1);      /* S37：真单声道编码器要单声道 PCM */
        } else {
            out[i * 2]     = (int16_t)l;
            out[i * 2 + 1] = (int16_t)r;
        }
    }
}
""")
# 参数应用 + 按声道数重建编码器
p.rep("""int audio_init(void)
{
    resamp_sinc_init();
""", """/* S36/S37：把当前编码参数套到编码器上（init、重建、运行时调参都走这里） */
static void apply_tuning(void)
{
    static const int bw_tab[6] = {
        OPUS_AUTO, OPUS_BANDWIDTH_NARROWBAND, OPUS_BANDWIDTH_MEDIUMBAND,
        OPUS_BANDWIDTH_WIDEBAND, OPUS_BANDWIDTH_SUPERWIDEBAND, OPUS_BANDWIDTH_FULLBAND };
    opus_encoder_ctl(encoder, OPUS_SET_BITRATE((opus_int32)cur_kbps * 1000));
    opus_encoder_ctl(encoder, OPUS_SET_BANDWIDTH(bw_tab[cur_bw < 6 ? cur_bw : 0]));
    opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(cur_cx));
    opus_frame_bytes = (uint16_t)((cur_kbps * 125u) / 100u);
    if (opus_frame_bytes > OPUS_OUT_SIZE)
        opus_frame_bytes = OPUS_OUT_SIZE;
    perf_audio_tune(cur_kbps, cur_bw, tune_mode, cur_cx);
}

/* S37：按声道数重新初始化编码器并套上当前参数。立体声编码器 + FORCE_CHANNELS(1) 仍要做
 * 立体声预处理与下混；真正的单声道编码器省掉这些（3.19 的 "[AUDIO] Encoder reinit %dch"）。 */
static int encoder_setup(int channels)
{
    int err = opus_encoder_init(encoder, 48000, channels, OPUS_APPLICATION_RESTRICTED_CELT);
    if (err != OPUS_OK) {
        LOG_ERR("[AUDIO] Encoder reinit %dch failed: %d\\n", channels, err);
        return -1;
    }
    enc_channels = channels;
    opus_encoder_ctl(encoder, OPUS_SET_EXPERT_FRAME_DURATION(OPUS_FRAMESIZE_10_MS));
    opus_encoder_ctl(encoder, OPUS_SET_VBR(0));
    apply_tuning();
    LOG_INF("[AUDIO] Encoder reinit %dch\\n", channels);
    return 0;
}

int audio_init(void)
{
    resamp_sinc_init();
""")
p.rep("""    encoder = (OpusEncoder *)encoder_mem;
    err = opus_encoder_init(encoder, 48000, 2,
                            OPUS_APPLICATION_RESTRICTED_CELT);
    if (err != OPUS_OK) {
        LOG_ERR("[AUDIO] Opus encoder init failed: %d\\n", err);
        encoder = NULL;
        return -1;
    }

    opus_encoder_ctl(encoder, OPUS_SET_EXPERT_FRAME_DURATION(OPUS_FRAMESIZE_10_MS));
    opus_encoder_ctl(encoder, OPUS_SET_BITRATE(200 * 8 * 100));
    opus_encoder_ctl(encoder, OPUS_SET_VBR(0));
    opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(0));
    opus_encoder_ctl(encoder, OPUS_SET_FORCE_CHANNELS(1));
    encoder_force_channels = 1;
""", """    encoder = (OpusEncoder *)encoder_mem;
    if (encoder_setup(1) != 0) {          /* S37：默认真单声道；插耳机时重建成立体声 */
        encoder = NULL;
        return -1;
    }
""")
p.rep("""                if (tune_pending) {
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
                int target_channels = plug_headset ? 2 : 1;

                if (target_channels != encoder_force_channels) {
                    int ctl_err = opus_encoder_ctl(
                        encoder, OPUS_SET_FORCE_CHANNELS(target_channels));
                    if (ctl_err == OPUS_OK) {
                        encoder_force_channels = target_channels;
                    } else {
                        LOG_ERR("[AUDIO] Opus force %s failed: %d\\n",
                                target_channels == 1 ? "mono" : "stereo",
                                ctl_err);
                    }
                }
""", """                if (tune_pending) {
                    /* S36：在两次编码之间应用新参数 */
                    tune_pending = false;
                    uint16_t kbps = tune_kbps_req;
                    if (kbps < 8)   kbps = 8;
                    if (kbps > 160) kbps = 160;          /* 10 ms 一帧最多 200 B */
                    cur_kbps = kbps;
                    cur_bw = tune_bw_req < 6 ? tune_bw_req : 0;
                    cur_cx = tune_cx_req > 10 ? 10 : tune_cx_req;
                    tune_mode = tune_mode_req ? 1 : 0;
                    apply_tuning();
                    LOG_INF("[AUDIO] tune: %u kbps bw=%u mode=%u cx=%u -> %u B/frame\\n",
                            cur_kbps, cur_bw, tune_mode, cur_cx, opus_frame_bytes);
                }
                bool speaker_on = !config_get()->disable_speaker;
                int target_channels = plug_headset ? 2 : 1;
                if (target_channels != enc_channels)
                    encoder_setup(target_channels);     /* S37：真·单声道 / 立体声编码器 */
""")
p.rep("""                        resample_512_480(slot_pcm, spk_resamp);
""", """                        resample_512_480(slot_pcm, spk_resamp, enc_channels == 1);
""")
p.save()

# ======================= diag.h =======================
p = P('diag.h')
p.rep("""    DIAG_EV_AUDIO,        /* S32：arg1 = 1 音频流开始 / 2 结束 / 3 发送连续失败近阈值 / 4 连败超限强制断开；arg2 = 连败次数 */
    DIAG_EV_MAX
""", """    DIAG_EV_AUDIO,        /* S32：arg1 = 1 音频流开始 / 2 结束 / 3 发送连续失败近阈值 / 4 连败超限强制断开；arg2 = 连败次数 */
    DIAG_EV_SCAN_SET,     /* S37：arg1 = 1 连上后关 page/inquiry scan / 2 断开后重开 */
    DIAG_EV_MAX
""")
p.save()

# ======================= bt_hid_host.c =======================
p = P('bt_hid_host.c')
p.rep("""    LOG_INF("[BT] Re-enabled connectable+discoverable, ready for new connection\\n");
""", """    LOG_INF("[BT] Re-enabled connectable+discoverable, ready for new connection\\n");
    diag_log(DIAG_EV_SCAN_SET, 2, 0);   /* S37 */
""")
p.save()

# ======================= main.c =======================
p = P('main.c')
p.rep("""        {   /* S33：每 ~1 s 采一次各任务 CPU 占比，连同音频统计一起放进 MAP_REG_PERF */
""", """        {   /* S37（照上游 3.20a/3.21 的 "scan OFF (ACL active)"）：连着的时候把 page/inquiry scan
             * 关掉，射频不再分时间去扫描；断开后 conn_disconnected / force_disconnect 照旧重开。
             * S35 实测放视频时 0x39 有约三分之一发送失败，扫描抢射频是嫌疑之一。
             * 每 10 s 再按一次：主机休眠唤醒那条路会把 scan 重开（radio_wake），连着时也压回去。 */
            static bool scan_off_done;
            static uint64_t scan_off_us;
            uint64_t sn = bflb_mtimer_get_time_us();
            if (bt_hid_host_get_state() == BT_HID_STATE_CONNECTED) {
                if (!scan_off_done) {
                    scan_off_done = true;
                    scan_off_us = sn;
                    bt_hid_host_radio_idle();
                    diag_log(DIAG_EV_SCAN_SET, 1, 0);
                } else if (sn - scan_off_us >= 10000000ULL) {
                    scan_off_us = sn;
                    bt_hid_host_radio_idle();
                }
            } else {
                scan_off_done = false;
            }
        }

        {   /* S33：每 ~1 s 采一次各任务 CPU 占比，连同音频统计一起放进 MAP_REG_PERF */
""")
p.save()

# ======================= usb_gamepad.c / .h =======================
p = P('usb_gamepad.c')
p.rep('#define FIRMWARE_VERSION "LCT616-DS5 3.18 S36"', '#define FIRMWARE_VERSION "LCT616-DS5 3.18 S37"')
p.rex(r"int usb_gamepad_send_consumer_report\(uint16_t bits\)\n\{.*?\n\}\n\n", "")
p.rep("    /* ---- Interface 4: Keyboard (PS shortcut / remap / wake key) ---- */",
      "    /* ---- Interface 4: Keyboard + Consumer + Mouse（ds5map 引擎的键鼠输出 / PS 唤醒键）---- */")
p.save()
p = P('usb_gamepad.h')
p.rep("""int  usb_gamepad_send_consumer_report(uint16_t usage);
""", "")
p.save()

# ======================= 主机侧 =======================
os.chdir(os.path.join(ROOT, 'tools'))
p = P('diag.py')
p.rep("""    33: 'DISC_REQ', 34: 'AUDIO',                                                # S32
""", """    33: 'DISC_REQ', 34: 'AUDIO',                                                # S32
    35: 'SCAN_SET',                                                             # S37
""")
p.save()
p = P('watch_state.py')
p.rep("""    elif ev == 34:
        x = {1: '扬声器流开始', 2: '扬声器流结束',
""", """    elif ev == 35:
        x = '连上后关 page/inquiry scan（射频专心传数据）' if a1 == 1 else '断开后重开 scan'
    elif ev == 34:
        x = {1: '扬声器流开始', 2: '扬声器流结束',
""")
p.save()

os.chdir(os.path.join(ROOT, 'webgui', 'js'))
p = P('pad-art.js')
p.rep("固件 remap.c TP_DEADZONE=120", "固件 ds5map.c TP_DEADZONE=120")
p.rep("布局与固件 ds5_protocol.h / remap.c 的 remap_get_src_bits() 一一对应", "布局与固件 ds5_protocol.h / ds5map.c 的 src_bits() 一一对应")
p.save()
p = P('macros.js')
p.rep("与固件 remap.c / ds5_protocol.h 一一对应", "与固件 ds5map.c src_bits() / ds5_protocol.h 一一对应")
p.save()
p = P('page-test.js')
p.rep("与固件 remap.c 的 BTN_LOC / get_src_bit", "与固件 ds5map.c 的 BTN_LOC / src_bits()")
p.save()
print('PATCH S37 DONE')
