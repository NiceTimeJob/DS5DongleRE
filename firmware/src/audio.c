#include "audio.h"
#include "ds5_usb_audio.h"
#include "bt_hid_host.h"
#include "ds5_protocol.h"
#include "config.h"

#include "FreeRTOS.h"
#include "semphr.h"
#include "queue.h"
#include "timers.h"
#include "bflb_mtimer.h"

#include "opus.h"
#include <string.h>
#include "debug_log.h"
#include <math.h>
#include "diag.h"   /* S32：音频流起止与发送连败记进诊断环 */
#include "perf.h"   /* S33：每块处理耗时 / 编码最长耗时 / 发送失败数 / 看门狗击杀数 */


#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---- Constants matching DS5DongleRE audio.cpp ---- */
#define OPUS_FRAME_SAMPLES  480     /* 10ms at 48kHz */
#define OPUS_OUT_SIZE       200     /* CBR output size */
#define HAPTIC_BUF_SIZE     64      /* 32 stereo int8 pairs */
#define HAPTIC_DECIMATE     16      /* 48kHz / 3kHz */
#define ACCUM_SAMPLES       USB_AUDIO_ACCUM_SAMPLES  /* 1024 = 2 Opus frames */
/* Half-block size: the 1024-sample block is split into two 512-sample halves,
 * each resampled 512→480 and encoded as one Opus frame. */
#define HALF_ACCUM          (ACCUM_SAMPLES / 2)      /* 512 */

#define MIC_OPUS_SIZE       71      /* Opus encoded mic frame from DualSense */
#define MIC_CHANNELS        1       /* Controller sends mono mic */
#define MIC_QUEUE_DEPTH     4

/* Polyphase sinc resampler: 512→480 = 16:15 ratio
 * Matches DS5DongleRE's WDL sinc resampler for anti-alias filtering.
 * gcd(512,480) = 32 → 15 unique phases, 8 taps per phase. */
#define SINC_HALF_TAPS  4
#define SINC_TAPS       (2 * SINC_HALF_TAPS)
#define RESAMP_PHASES   15

static uint8_t audio_seq;   /* sequence counter for 0x39 audio report */

/* S36：编码参数运行时可调（0xF6 命令 0x10，不落盘）。默认与之前完全一样：160 kbps / 200 B。
 * 思路：喇叭就那样，码率从 160 降到几十 kbps 听不出差别，但编码器要搜索的脉冲数少了
 * 大半，编码时间（S35 实测一帧 10 ms 要 11~16 ms）会明显缩短。
 *   tune_mode 0 = 编码产物用 opus_packet_pad() 补成合法的 200 B 带填充包，线上格式不变；
 *   tune_mode 1 = 按真实长度发（长度字段 < 200，两帧紧挨着放），验证手柄认不认变长。 */
/* S37 默认值（用户：「音频能出声就行，不需要很好的音质」）：48 kbps、宽带 8 kHz、补填充到 200 B。
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
static uint16_t slot_len[2] = { OPUS_OUT_SIZE, OPUS_OUT_SIZE };

void audio_set_tuning(uint16_t kbps, uint8_t bw, uint8_t mode, uint8_t complexity)
{
    tune_kbps_req = kbps;
    tune_bw_req   = bw;
    tune_mode_req = mode;
    tune_cx_req   = complexity;
    tune_pending  = true;   /* 可能来自 USB ISR：由 audio_task 在两次编码之间应用 */
}

/* R3.28：把配置体里存的音频编码参数（音频挡位）应用上来。
 * 开机读配置后调一次；网页「写入接收器」改配置后也调一次 —— 这样挡位是**设备记住**的，
 * 断电拔插不用再等网页重发。0/越界已经在 config_validate() 里归一过了。 */
void audio_apply_config(void)
{
    const struct config_body *b = config_get();
    uint16_t kbps = b->audio_kbps ? b->audio_kbps : AUDIO_DEF_KBPS;
    uint8_t bw = b->audio_bw <= 5 ? b->audio_bw : AUDIO_DEF_BW;
    uint8_t cx = b->audio_cx <= 10 ? b->audio_cx : AUDIO_DEF_CX;
    audio_set_tuning(kbps, bw, 0, cx);
    LOG_INF("[AUDIO] cfg: %u kbps bw=%u cx=%u\n", (unsigned)kbps, (unsigned)bw, (unsigned)cx);
}

/* Static buffers for Opus encoder/decoder — avoids 20+KB heap allocation.
 * Upper bounds verified at init via opus_encoder_get_size() / opus_decoder_get_size(). */
#define OPUS_ENC_MAX_SIZE 36864
#define OPUS_DEC_MAX_SIZE 24576
static __attribute__((aligned(8))) uint8_t encoder_mem[OPUS_ENC_MAX_SIZE];
static __attribute__((aligned(8))) uint8_t decoder_mem[OPUS_DEC_MAX_SIZE];
static OpusEncoder  *encoder;
static OpusDecoder  *decoder;
static uint8_t  packet_counter;
static volatile bool plug_headset;
static volatile bool mic_enabled;   /* host opened mic interface AND config allows */
static volatile bool mic_status_pending;  /* deferred: send 0x32 to controller */
static int enc_channels;   /* S37：编码器本身按 1/2 声道初始化（3.19 的 "Encoder reinit %dch"），不再 FORCE_CHANNELS */

static QueueHandle_t mic_queue;

static int16_t  pcm_block[ACCUM_SAMPLES * USB_AUDIO_CHANNELS];

static int16_t  spk_resamp[OPUS_FRAME_SAMPLES * 2];
static bool     mic_first_frame;
static volatile bool encoding_in_progress;
static volatile bool encoder_reset_pending;
static volatile bool audio_task_killed;
static volatile uint64_t encode_start_us;
static TaskHandle_t audio_task_handle;
static TimerHandle_t encode_watchdog;

/* Double-frame buffers for 0x39 report (2x haptics + 2x opus per packet) */
static uint8_t  opus_slots[2][OPUS_OUT_SIZE];
static int8_t   haptic_slots[2][HAPTIC_BUF_SIZE];

/* Pre-computed polyphase sinc filter in q15 fixed-point.
 * Init uses float for precision; hot path uses int32 MAC only.
 * Coefficients per phase are normalized to sum = 32768 (1.0 in q15),
 * so max accumulator value = 32768 * 32768 = 1,073,741,824 < INT32_MAX. */
static int16_t sinc_q15[RESAMP_PHASES][SINC_TAPS];

static void resamp_sinc_init(void)
{
    const float cutoff = 480.0f / 512.0f;

    for (int p = 0; p < RESAMP_PHASES; p++) {
        float frac = (float)p / (float)RESAMP_PHASES;
        float coeffs_f[SINC_TAPS];
        float sum = 0.0f;

        for (int t = 0; t < SINC_TAPS; t++) {
            float x = (float)(t - (SINC_HALF_TAPS - 1)) - frac;
            float sx = x * cutoff;
            float s;
            if (fabsf(sx) < 1e-6f)
                s = cutoff;
            else
                s = cutoff * sinf((float)M_PI * sx) / ((float)M_PI * sx);

            float wn = ((float)t + 0.5f) / (float)SINC_TAPS;
            float w = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * wn));

            coeffs_f[t] = s * w;
            sum += coeffs_f[t];
        }

        if (fabsf(sum) > 1e-6f) {
            for (int t = 0; t < SINC_TAPS; t++)
                coeffs_f[t] /= sum;
        }

        /* Float -> q15, then adjust largest tap so sum == 32768 exactly */
        int32_t qsum = 0;
        int max_idx = 0;
        float max_val = 0.0f;
        for (int t = 0; t < SINC_TAPS; t++) {
            int32_t q = (int32_t)(coeffs_f[t] * 32768.0f +
                                  (coeffs_f[t] >= 0 ? 0.5f : -0.5f));
            if (q > 32767) q = 32767;
            if (q < -32768) q = -32768;
            sinc_q15[p][t] = (int16_t)q;
            qsum += q;
            if (fabsf(coeffs_f[t]) > max_val) {
                max_val = fabsf(coeffs_f[t]);
                max_idx = t;
            }
        }
        int32_t fix = 32768 - qsum;
        int32_t corrected = sinc_q15[p][max_idx] + fix;
        if (corrected > 32767) corrected = 32767;
        if (corrected < -32768) corrected = -32768;
        sinc_q15[p][max_idx] = (int16_t)corrected;
    }
}

/* ---- Polyphase sinc resample 512 -> 480 (4ch USB PCM -> stereo, q15) ---- */
static void resample_512_480(const int16_t *in, int16_t *out, bool mono)
{
    for (int i = 0; i < OPUS_FRAME_SAMPLES; i++) {
        uint32_t src_pos = (uint32_t)i * 16;
        int center = (int)(src_pos / RESAMP_PHASES);
        int phase  = (int)(src_pos % RESAMP_PHASES);

        const int16_t *c = sinc_q15[phase];
        int32_t sum_l = (1 << 14);
        int32_t sum_r = (1 << 14);

        if (center >= SINC_HALF_TAPS - 1 &&
            center <= HALF_ACCUM - SINC_HALF_TAPS - 1) {
            const int16_t *s = in +
                (center - (SINC_HALF_TAPS - 1)) * USB_AUDIO_CHANNELS;
#define RESAMP_TAP(t) do { \
                int16_t coeff = c[(t)]; \
                sum_l += (int32_t)s[(t) * USB_AUDIO_CHANNELS] * coeff; \
                sum_r += (int32_t)s[(t) * USB_AUDIO_CHANNELS + 1] * coeff; \
            } while (0)
            RESAMP_TAP(0);
            RESAMP_TAP(1);
            RESAMP_TAP(2);
            RESAMP_TAP(3);
            RESAMP_TAP(4);
            RESAMP_TAP(5);
            RESAMP_TAP(6);
            RESAMP_TAP(7);
#undef RESAMP_TAP
        } else {
            for (int t = 0; t < SINC_TAPS; t++) {
                int idx = center + t - (SINC_HALF_TAPS - 1);
                if (idx < 0) idx = 0;
                if (idx >= HALF_ACCUM) idx = HALF_ACCUM - 1;

                int16_t coeff = c[t];
                sum_l += (int32_t)in[idx * USB_AUDIO_CHANNELS] * coeff;
                sum_r += (int32_t)in[idx * USB_AUDIO_CHANNELS + 1] * coeff;
            }
        }

        int32_t l = sum_l >> 15;
        int32_t r = sum_r >> 15;
        if (l > 32767) l = 32767; if (l < -32768) l = -32768;
        if (r > 32767) r = 32767; if (r < -32768) r = -32768;

        if (mono) {
            out[i] = (int16_t)((l + r) >> 1);      /* S37：真单声道编码器要单声道 PCM */
        } else {
            out[i * 2]     = (int16_t)l;
            out[i * 2 + 1] = (int16_t)r;
        }
    }
}

/* ---- Haptics: 16:1 point-sample decimation (stereo int16 → stereo int8) ----
 * No low-pass filter — matches wired DS5 behavior (internal haptics also
 * run at 3 kHz without filtering). */
/* R1.9：触觉强度不再是配置项，固定最大（2.0）→ 8.8 定点 512。
 * v*512>>16 一步完成 2 倍增益和 int16→int8 的量程映射。 */
#define HAPTIC_GAIN_FP  512

static void decimate_haptics(const int16_t *in, int8_t *out, uint32_t in_samples)
{
    uint32_t out_pairs = in_samples / HAPTIC_DECIMATE;
    if (out_pairs > HAPTIC_BUF_SIZE / 2)
        out_pairs = HAPTIC_BUF_SIZE / 2;

    for (uint32_t i = 0; i < out_pairs; i++) {
        uint32_t idx = (i * HAPTIC_DECIMATE) * USB_AUDIO_CHANNELS;
        int32_t val_l = in[idx + 2];
        int32_t val_r = in[idx + 3];
        val_l = (val_l * HAPTIC_GAIN_FP) >> 16;
        val_r = (val_r * HAPTIC_GAIN_FP) >> 16;
        if (val_l > 127) val_l = 127;
        if (val_l < -128) val_l = -128;
        if (val_r > 127) val_r = 127;
        if (val_r < -128) val_r = -128;
        out[i * 2]     = (int8_t)val_l;
        out[i * 2 + 1] = (int8_t)val_r;
    }
}

/* ---- Audio-to-Haptic: decimate audio ch0/ch1 waveform to haptic format ----
 * Same 16:1 point-sample as decimate_haptics, but reads ch0/ch1 instead of
 * ch2/ch3.  DualSense LRA needs an oscillating waveform, not DC envelope. */
static void audio_to_haptic(const int16_t *in, int8_t *out, uint32_t in_samples)
{
    uint32_t out_pairs = in_samples / HAPTIC_DECIMATE;
    if (out_pairs > HAPTIC_BUF_SIZE / 2)
        out_pairs = HAPTIC_BUF_SIZE / 2;

    for (uint32_t i = 0; i < out_pairs; i++) {
        uint32_t idx = (i * HAPTIC_DECIMATE) * USB_AUDIO_CHANNELS;
        int32_t val_l = in[idx];       /* ch0 */
        int32_t val_r = in[idx + 1];   /* ch1 */
        val_l = (val_l * HAPTIC_GAIN_FP) >> 16;
        val_r = (val_r * HAPTIC_GAIN_FP) >> 16;
        if (val_l > 127) val_l = 127;
        if (val_l < -128) val_l = -128;
        if (val_r > 127) val_r = 127;
        if (val_r < -128) val_r = -128;
        out[i * 2]     = (int8_t)val_l;
        out[i * 2 + 1] = (int8_t)val_r;
    }
}

/* Detect whether ch2/ch3 carry real HD rumble or just noise/dither.
 * Threshold avoids false positives from OS-injected low-level dither. */
#define HAPTIC_SILENCE_THRESH  256

static bool haptic_channel_silent(const int16_t *in, uint32_t in_samples)
{
    int32_t peak = 0;
    for (uint32_t i = 0; i < in_samples; i += HAPTIC_DECIMATE) {
        uint32_t idx = i * USB_AUDIO_CHANNELS;
        int32_t v2 = in[idx + 2];
        int32_t v3 = in[idx + 3];
        if (v2 < 0) v2 = -v2;
        if (v3 < 0) v3 = -v3;
        if (v2 > peak) peak = v2;
        if (v3 > peak) peak = v3;
    }
    return peak < HAPTIC_SILENCE_THRESH;
}

/* ---- Build and send BT report 0x39 (547 bytes, double-frame) ---- */
#define AUDIO_SEND_FAIL_MAX  50   /* ~1s @ 21ms/frame → force disconnect */

static int send_audio_report(void)
{
    static uint8_t pkt[DS5_BT_AUDIO_REPORT_SIZE];
    memset(pkt, 0, sizeof(pkt));

    pkt[0] = DS5_BT_AUDIO_REPORT_ID;
    pkt[1] = (audio_seq & 0x0F) << 4;
    audio_seq = (audio_seq + 1) & 0x0F;

    /* Audio control header (tag 0x91, 6 fields) */
    pkt[2] = DS5_AUDIO_TAG_HEADER;
    pkt[3] = 6;
    pkt[4] = mic_enabled ? 0x7F : 0x7E;

    /* R1.9：音频缓冲深度不再是配置项，固定 64 下发给手柄 */
    pkt[5] = 64;
    pkt[6] = 64;
    pkt[7] = 64;
    pkt[8] = 64;
    packet_counter += 2;
    pkt[9] = packet_counter;

    /* Haptics (tag 0xD2, 2x 64-byte blocks) */
    pkt[10] = DS5_AUDIO_TAG_HAPTICS;
    pkt[11] = DS5_AUDIO_SAMPLE_SIZE;
    memcpy(pkt + 12, haptic_slots[0], HAPTIC_BUF_SIZE);
    memcpy(pkt + 12 + HAPTIC_BUF_SIZE, haptic_slots[1], HAPTIC_BUF_SIZE);

    /* Speaker Opus (tag 0xD3/0xD6, 2x 200-byte blocks) */
    bool speaker_enabled = !config_get()->disable_speaker;
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

    /* CRC32 */
    uint32_t crc = ds5_crc32(DS5_BT_OUTPUT_CRC_SEED, pkt,
                             DS5_BT_AUDIO_REPORT_SIZE - 4);
    ds5_write_le32(&pkt[DS5_BT_AUDIO_REPORT_SIZE - 4], crc);

    static uint32_t fail_log_count = 0;
    int ret = bt_hid_host_send_output(pkt, DS5_BT_AUDIO_REPORT_SIZE);
    if (ret) {
        perf_audio_sendfail(ret);   /* S36：按原因分类（-2 缓冲池空 / -1 没连接 / 其它 = l2cap 错误码） */
        fail_log_count++;
        if (fail_log_count <= 3 || (fail_log_count % 100) == 0)
            LOG_ERR("[AUDIO] BT send failed: %d (x%lu)\n",
                    ret, (unsigned long)fail_log_count);
    } else {
        fail_log_count = 0;
    }
    return ret;
}

/* ---- Public API ---- */

#define STACK_WORDS(bytes) \
    (((bytes) + sizeof(StackType_t) - 1) / sizeof(StackType_t))
#define AUDIO_TASK_STACK_SIZE STACK_WORDS(1024*32)
#define AUDIO_TASK_PRIORITY   (configMAX_PRIORITIES - 2)

#define ENCODE_TIMEOUT_US  15000  /* 15ms — normal encode takes ~5-7ms */
#define WATCHDOG_PERIOD_MS 20

static void encode_watchdog_cb(TimerHandle_t timer)
{
    (void)timer;
    if (!encoding_in_progress) return;

    uint64_t elapsed = bflb_mtimer_get_time_us() - encode_start_us;
    if (elapsed < ENCODE_TIMEOUT_US) return;

    if (audio_task_handle) {
        vTaskSuspend(audio_task_handle);
    }
    perf_audio_wdkill();

    encoding_in_progress = false;
    encoder_reset_pending = true;
    audio_task_killed = true;
}

bool audio_check_respawn(void)
{
    if (!audio_task_killed) return false;
    audio_task_killed = false;

    if (audio_task_handle) {
        vTaskDelete(audio_task_handle);
        audio_task_handle = NULL;
    }

    xTaskCreate(audio_task, "audio", AUDIO_TASK_STACK_SIZE,
                NULL, AUDIO_TASK_PRIORITY, NULL);
    LOG_ERR("[WD-OPUS] respawned\n");
    return true;
}

/* S36/S37：把当前编码参数套到编码器上（init、重建、运行时调参都走这里） */
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
        LOG_ERR("[AUDIO] Encoder reinit %dch failed: %d\n", channels, err);
        return -1;
    }
    enc_channels = channels;
    opus_encoder_ctl(encoder, OPUS_SET_EXPERT_FRAME_DURATION(OPUS_FRAMESIZE_10_MS));
    opus_encoder_ctl(encoder, OPUS_SET_VBR(0));
    apply_tuning();
    LOG_INF("[AUDIO] Encoder reinit %dch\n", channels);
    return 0;
}

int audio_init(void)
{
    resamp_sinc_init();

    int err;
    int enc_size = opus_encoder_get_size(2);
    int dec_size = opus_decoder_get_size(MIC_CHANNELS);

    if (enc_size > OPUS_ENC_MAX_SIZE) {
        LOG_ERR("[AUDIO] Opus encoder needs %d bytes, buffer is %d\n",
                enc_size, OPUS_ENC_MAX_SIZE);
        return -1;
    }
    if (dec_size > OPUS_DEC_MAX_SIZE) {
        LOG_ERR("[AUDIO] Opus decoder needs %d bytes, buffer is %d\n",
                dec_size, OPUS_DEC_MAX_SIZE);
        return -1;
    }

    encoder = (OpusEncoder *)encoder_mem;
    if (encoder_setup(1) != 0) {          /* S37：默认真单声道；插耳机时重建成立体声 */
        encoder = NULL;
        return -1;
    }

    decoder = (OpusDecoder *)decoder_mem;
    err = opus_decoder_init(decoder, 48000, MIC_CHANNELS);
    if (err != OPUS_OK) {
        LOG_ERR("[AUDIO] Opus decoder init failed: %d\n", err);
        decoder = NULL;
    }

    mic_queue = xQueueCreate(MIC_QUEUE_DEPTH, MIC_OPUS_SIZE);
    if (!mic_queue) {
        LOG_ERR("[AUDIO] mic_queue create failed\n");
    }

    if (!decoder || !mic_queue) {
        LOG_ERR("[AUDIO] Mic path unavailable (decoder=%p queue=%p)\n",
               (void *)decoder, (void *)mic_queue);
    }

    packet_counter = 0;
    plug_headset = false;
    mic_enabled = false;
    mic_status_pending = false;
    mic_first_frame = false;
    memset(opus_slots, 0, sizeof(opus_slots));
    memset(haptic_slots, 0, sizeof(haptic_slots));

    encode_watchdog = xTimerCreate("wd-opus", pdMS_TO_TICKS(WATCHDOG_PERIOD_MS),
                                   pdTRUE, NULL, encode_watchdog_cb);
    if (encode_watchdog)
        xTimerStart(encode_watchdog, 0);

    LOG_INF("[AUDIO] Opus static init (enc=%d/%d dec=%d/%d mic_q=%p wd=%p)\n",
           enc_size, OPUS_ENC_MAX_SIZE, dec_size, OPUS_DEC_MAX_SIZE,
           (void *)mic_queue, (void *)encode_watchdog);
    return 0;
}

/* ---- Send a 0x32 status report to toggle controller mic streaming ---- */
static void send_mic_status(void)
{
    if (bt_hid_host_get_state() != BT_HID_STATE_CONNECTED)
        return;

    static uint8_t mic_seq = 0;
    uint8_t pkt[DS5_BT_OUTPUT_EXT_SIZE];
    memset(pkt, 0, sizeof(pkt));

    pkt[0] = DS5_BT_OUTPUT_REPORT_ID_EXT;
    pkt[1] = (mic_seq & 0x0F) << 4;
    mic_seq = (mic_seq + 1) & 0x0F;

    pkt[2] = DS5_AUDIO_TAG_HEADER;
    pkt[3] = 1;
    pkt[4] = mic_enabled ? 0x03 : 0x02;

    uint32_t crc = ds5_crc32(DS5_BT_OUTPUT_CRC_SEED, pkt,
                             DS5_BT_OUTPUT_EXT_SIZE - 4);
    ds5_write_le32(&pkt[DS5_BT_OUTPUT_EXT_SIZE - 4], crc);

    bt_hid_host_send_output(pkt, DS5_BT_OUTPUT_EXT_SIZE);
}

__attribute__((section(".tcm_code")))
void audio_task(void *arg)
{
    (void)arg;

    audio_task_handle = xTaskGetCurrentTaskHandle();

    if (encoder_reset_pending) {
        encoder_reset_pending = false;
        opus_encoder_ctl(encoder, OPUS_RESET_STATE);
        LOG_ERR("[WD-OPUS] encoder reset\n");
    }

    SemaphoreHandle_t sem = (SemaphoreHandle_t)usb_audio_get_semaphore();
    LOG_INF("[AUDIO] Task started\n");

    uint32_t send_fail_streak = 0;

    for (;;) {
        if (xSemaphoreTake(sem, pdMS_TO_TICKS(25)) == pdTRUE) {
            bool a_active = usb_audio_is_active();
            bool a_read   = a_active ? usb_audio_read(pcm_block) : false;
            bool a_bt     = bt_hid_host_get_state() == BT_HID_STATE_CONNECTED;

            /* S32：音频流的起止各记一条——它是"本机主动断链"的候选来源之一，
             * 不记的话无法判断某次断开时音频到底在不在跑。 */
            {
                static bool stream_was_active = false;
                if (a_active != stream_was_active) {
                    stream_was_active = a_active;
                    diag_log(DIAG_EV_AUDIO, a_active ? 1 : 2, 0);
                }
            }

            if (a_active && a_read && a_bt)
            {
                if (tune_pending) {
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
                    LOG_INF("[AUDIO] tune: %u kbps bw=%u mode=%u cx=%u -> %u B/frame\n",
                            cur_kbps, cur_bw, tune_mode, cur_cx, opus_frame_bytes);
                }
                bool speaker_on = !config_get()->disable_speaker;
                int target_channels = plug_headset ? 2 : 1;
                if (target_channels != enc_channels)
                    encoder_setup(target_channels);     /* S37：真·单声道 / 立体声编码器 */

                uint8_t ah_mode = config_get()->audio_haptic;
                uint64_t blk_t0 = bflb_mtimer_get_time_us();   /* S33 */
                uint32_t enc_max = 0;

                for (int slot = 0; slot < 2; slot++) {
                    const uint32_t base = (uint32_t)slot * HALF_ACCUM;
                    const int16_t *slot_pcm =
                        &pcm_block[base * USB_AUDIO_CHANNELS];

                    if (ah_mode == 2) {
                        audio_to_haptic(slot_pcm, haptic_slots[slot], HALF_ACCUM);
                    } else if (ah_mode == 1 &&
                               haptic_channel_silent(slot_pcm, HALF_ACCUM)) {
                        audio_to_haptic(slot_pcm, haptic_slots[slot], HALF_ACCUM);
                    } else {
                        decimate_haptics(slot_pcm, haptic_slots[slot], HALF_ACCUM);
                    }

                    if (speaker_on) {
                        resample_512_480(slot_pcm, spk_resamp, enc_channels == 1);
                        encode_start_us = bflb_mtimer_get_time_us();
                        encoding_in_progress = true;
                        int encoded = opus_encode(encoder, spk_resamp, OPUS_FRAME_SAMPLES,
                                                  opus_slots[slot], OPUS_OUT_SIZE);
                        encoding_in_progress = false;
                        {
                            uint32_t e_us = (uint32_t)(bflb_mtimer_get_time_us() - encode_start_us);
                            if (e_us > enc_max)
                                enc_max = e_us;
                        }

                        if (encoder_reset_pending) {
                            encoder_reset_pending = false;
                            opus_encoder_ctl(encoder, OPUS_RESET_STATE);
                        }
                        if (encoded <= 0) {
                            LOG_ERR("[AUDIO] Opus encode error: %d\n", encoded);
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
                }

                int send_rc = send_audio_report();
                perf_audio_block((uint32_t)(bflb_mtimer_get_time_us() - blk_t0), enc_max);
                if (send_rc != 0) {
                    send_fail_streak++;
                    if (send_fail_streak >= AUDIO_SEND_FAIL_MAX) {
                        /* R1：不再因为音频发不出去就拆链。2026-10-14 10:53 实抓：链路先静默断掉
                         * （手柄不再发输入、发送缓冲池耗尽），音频连败 50 次只是后果；它再去拆链，
                         * 和 3 s 无输入看门狗、DISCONNECTING 兜底撞在一起触发了空指针复位。
                         * 链路死活只由 usb 任务的 3 s 无输入看门狗判定；这里只记一条（30 s 限流）、退避。 */
                        LOG_ERR("[AUDIO] %u consecutive send failures, dropping audio\n",
                                (unsigned)send_fail_streak);
                        {
                            static uint64_t last_max_diag_us;
                            uint64_t nowm = bflb_mtimer_get_time_us();
                            if (nowm - last_max_diag_us >= 30000000ULL) {
                                last_max_diag_us = nowm;
                                diag_log(DIAG_EV_AUDIO, 4, send_fail_streak);
                            }
                        }
                        send_fail_streak = 0;
                        vTaskDelay(pdMS_TO_TICKS(200));
                    } else if (send_fail_streak > 3) {
                        /* S32：第 4 次和第 10 次各记一条；S36：限流 30 s 一条——S35 实测放视频时
                         * 它每 250 ms 一条，25 格的环全是它，掉线定位会失效 */
                        if (send_fail_streak == 4 || send_fail_streak == 10) {
                            static uint64_t last_diag_us;
                            uint64_t nowu = bflb_mtimer_get_time_us();
                            if (nowu - last_diag_us >= 30000000ULL) {
                                last_diag_us = nowu;
                                diag_log(DIAG_EV_AUDIO, 3, send_fail_streak);
                            }
                        }
                        vTaskDelay(pdMS_TO_TICKS(20));
                    } else {
                        vTaskDelay(1);
                    }
                } else {
                    send_fail_streak = 0;
                }
            }
        }

        if (mic_status_pending &&
            bt_hid_host_get_state() == BT_HID_STATE_CONNECTED) {
            mic_status_pending = false;
            LOG_INF("[AUDIO] Mic %s\n", mic_enabled ? "enabled" : "disabled");
            send_mic_status();
        }
    }
}

void audio_set_headset(bool plugged)
{
    if (plugged != plug_headset)
        LOG_INF("[AUDIO] Headset %s\n", plugged ? "plugged" : "unplugged");
    plug_headset = plugged;
}

void audio_reset(void)
{
    packet_counter = 0;
    plug_headset = false;
    mic_enabled = false;
    mic_status_pending = false;
    mic_first_frame = false;
    memset(opus_slots, 0, sizeof(opus_slots));
    memset(haptic_slots, 0, sizeof(haptic_slots));
    if (encoding_in_progress) {
        encoder_reset_pending = true;
    } else if (encoder) {
        opus_encoder_ctl(encoder, OPUS_RESET_STATE);
    }
    if (decoder)
        opus_decoder_ctl(decoder, OPUS_RESET_STATE);
    if (mic_queue)
        xQueueReset(mic_queue);
    usb_audio_mic_stop();
}

void audio_reset_encoder(void)
{
    memset(opus_slots, 0, sizeof(opus_slots));
    memset(haptic_slots, 0, sizeof(haptic_slots));
    if (encoding_in_progress) {
        encoder_reset_pending = true;
    } else if (encoder) {
        opus_encoder_ctl(encoder, OPUS_RESET_STATE);
    }
}

void audio_mic_feed(const uint8_t *opus_data, uint16_t len)
{
    if (!mic_enabled || !mic_queue) return;
    if (len < MIC_OPUS_SIZE) return;

    uint8_t frame[MIC_OPUS_SIZE];
    memcpy(frame, opus_data, MIC_OPUS_SIZE);
    if (xQueueSend(mic_queue, frame, 0) != pdTRUE) {
        uint8_t discard[MIC_OPUS_SIZE];
        xQueueReceive(mic_queue, discard, 0);
        xQueueSend(mic_queue, frame, 0);
    }
}

void audio_set_mic_active(bool active)
{
    mic_enabled = active && !config_get()->disable_mic;
    mic_status_pending = true;  /* deferred to audio_task (called from USB ISR) */
}

bool audio_mic_active(void)
{
    return mic_enabled;
}

/* ---- Mic decode task: runs independently at lower priority than audio_task ----
 * Blocks on mic_queue so it doesn't burn CPU when mic is inactive.
 * Decodes one Opus frame per wakeup → writes to USB mic ring buffer.
 * Keeps audio_task cycle at ~21ms regardless of mic decoding cost. */
__attribute__((section(".tcm_code")))
void audio_mic_task(void *arg)
{
    (void)arg;
    static uint8_t  mic_opus_buf[MIC_OPUS_SIZE];
    static int16_t  mic_mono[OPUS_FRAME_SAMPLES];
    static int16_t  mic_stereo[OPUS_FRAME_SAMPLES * 2];

    for (;;) {
        if (!mic_queue) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (xQueueReceive(mic_queue, mic_opus_buf, portMAX_DELAY) != pdTRUE)
            continue;

        if (!decoder || !mic_enabled)
            continue;

        int decoded = opus_decode(decoder, mic_opus_buf, MIC_OPUS_SIZE,
                                  mic_mono, OPUS_FRAME_SAMPLES, 0);
        if (decoded <= 0) {
            LOG_ERR("[MIC] Opus decode error: %d\n", decoded);
            continue;
        }
        if (!mic_first_frame) {
            mic_first_frame = true;
            LOG_INF("[AUDIO] First mic frame decoded (%d samples)\n", decoded);
        }

        /* Pack mono → stereo via uint32: one 32-bit write per sample (LE). */
        uint32_t *out32 = (uint32_t *)mic_stereo;
        for (int i = 0; i < decoded; i++) {
            uint16_t s = (uint16_t)mic_mono[i];
            out32[i] = (uint32_t)s | ((uint32_t)s << 16);
        }
        usb_audio_mic_write(mic_stereo, (uint32_t)decoded);
    }
}
