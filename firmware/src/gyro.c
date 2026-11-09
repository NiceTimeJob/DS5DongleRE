/*
 * gyro.c — 手柄体感：本体轴两轴角速度取值 + 静置校准（零偏）
 *
 * R1.3（2026-10-14，用户要求「彻底去掉自动校准，静置校准存到配置文件里，就和按键映射一样」）：
 *   - 删掉自动零偏校准（Stillness 检测 + 灵敏度修正）。R1.2 实测它跑一次 130~140 µs，
 *     是映射引擎里最贵的一项；用户的用法（映射到右摇杆、按住 L2 才生效）里零偏换算成
 *     摇杆偏移不到 1%。
 *   - 零偏只来自「静置校准」：手柄放平别动 3 秒取平均；期间任一轴晃动超过阈值、或者帧数
 *     太少就判失败、不采用（GYRO_FLAG_CAL_FAILED）。成功后由 ds5map.c 存进 LittleFS 的
 *     gcal.bin（和按键映射的 cfg.bin 同一个文件系统），开机读回，断开重连不丢。
 *   - 不在校准时没有任何每帧计算：gyro_read_hv() 直接从报文取值、扣零偏、过死区。
 *
 * 跟随重力（player space）2026-10-11 就按用户要求删除了。
 *
 * 历史：被删掉的自动校准部分曾移植自 JibbSmart/GamepadMotionHelpers v10
 * （MIT License, Copyright (c) 2020-2023 Julian "Jibb" Smart）；R1.3 起相关代码已全部移除。
 */

#include "gyro.h"

#include <string.h>

#define GYRO_MANUAL_CAL_MS    3000
#define GYRO_CAL_MIN_SAMPLES  50       /* 3 s 里至少这么多帧才算数（引擎 ~500 Hz，正常 ~1500） */
#define GYRO_CAL_MAX_SPREAD   4.0f     /* 校准期间任一轴 最大−最小 超过这么多 度/秒 ⇒ 手柄动了 */
#define GYRO_BIAS_MAX         20.0f    /* 零偏绝对值上限（度/秒）；超过就是读坏了 */

/* 本体轴的方向符号 —— **定死在固件里**（2026-10-11 用户实测：DS5 的 yaw/pitch 原始符号
 * 跟他"转手柄 → 视角跟手"的直觉相反，两条轴都要翻一次）。网页的「反向」勾选框在这之上再翻一次。
 * 只影响映射通路：0x10/0x11 遥测里显示的仍是手柄原始值，方便对账。 */
#define GYRO_BASE_SIGN_H (-1)
#define GYRO_BASE_SIGN_V (-1)

static gyro_cfg_t s_cfg;

static float         s_bias[3];      /* 度/秒 */
static bool          s_have_bias;
static bool          s_bias_new;     /* 刚校准出新零偏，等 ds5map 取走落盘 */

static volatile bool s_cal_req;      /* gyro_cal_start() 可能在 USB ISR 里置位 */
static bool          s_manual;
static bool          s_cal_failed;
static uint32_t      s_manual_start;
static uint32_t      s_now_ms;
static int           s_manual_n;
static float         s_manual_sum[3];
static float         s_manual_min[3];
static float         s_manual_max[3];

/* 角速度 p[15]/p[17]/p[19] = pitch/yaw/roll，16 LSB 每度/秒 */
static int32_t gyro_raw_lsb(const uint8_t *p, uint8_t axis)
{
    const uint8_t *q;
    switch (axis) {
    case GYRO_AXIS_YAW:  q = &p[17]; break;
    case GYRO_AXIS_ROLL: q = &p[19]; break;
    default:             q = &p[15]; break;   /* GYRO_AXIS_PITCH */
    }
    return (int32_t)(int16_t)((uint16_t)q[0] | ((uint16_t)q[1] << 8));
}

void gyro_reset(void)
{
    /* 断开时只取消进行中的校准；零偏来自 flash，跟这次连接无关，保留 */
    s_cal_req = false;
    s_manual = false;
    s_manual_n = 0;
}

void gyro_apply_cfg(const gyro_cfg_t *cfg)
{
    s_cfg = *cfg;
    s_cfg.dir = GYRO_DIR_LOCAL;
    if (s_cfg.axis_h > GYRO_AXIS_ROLL)       s_cfg.axis_h = GYRO_AXIS_YAW;
    if (s_cfg.axis_v > GYRO_AXIS_ROLL)       s_cfg.axis_v = GYRO_AXIS_PITCH;
    s_cfg.inv_h = s_cfg.inv_h ? 1 : 0;
    s_cfg.inv_v = s_cfg.inv_v ? 1 : 0;
}

void gyro_cal_start(void)  { s_cal_req = true; }

void gyro_cal_cancel(void)
{
    s_cal_req = false;
    s_manual = false;
}

bool gyro_calibrating(void)
{
    return s_cal_req || s_manual;
}

void gyro_set_bias(const float bias[3])
{
    for (int i = 0; i < 3; i++)
        s_bias[i] = bias[i];
    s_have_bias = true;
}

bool gyro_take_new_bias(float out[3])
{
    if (!s_bias_new)
        return false;
    s_bias_new = false;
    for (int i = 0; i < 3; i++)
        out[i] = s_bias[i];
    return true;
}

/* 只在静置校准期间被调用：累加 3 s，结束时判定是否采用 */
void gyro_update(const uint8_t *p, uint32_t now_ms)
{
    if (s_cal_req) {
        s_cal_req = false;
        s_manual = true;
        s_cal_failed = false;
        s_manual_start = now_ms;
        s_manual_n = 0;
        for (int i = 0; i < 3; i++) {
            s_manual_sum[i] = 0.f;
            s_manual_min[i] = 1e9f;
            s_manual_max[i] = -1e9f;
        }
    }
    s_now_ms = now_ms;
    if (!s_manual)
        return;

    for (int i = 0; i < 3; i++) {
        const float g = (float)gyro_raw_lsb(p, (uint8_t)i) / GYRO_LSB_PER_DEG_S;
        s_manual_sum[i] += g;
        if (g < s_manual_min[i]) s_manual_min[i] = g;
        if (g > s_manual_max[i]) s_manual_max[i] = g;
    }
    s_manual_n++;

    if ((uint32_t)(now_ms - s_manual_start) < GYRO_MANUAL_CAL_MS)
        return;

    s_manual = false;
    float nb[3];
    bool ok = s_manual_n >= GYRO_CAL_MIN_SAMPLES;
    for (int i = 0; i < 3 && ok; i++) {
        nb[i] = s_manual_sum[i] / (float)s_manual_n;
        if (s_manual_max[i] - s_manual_min[i] > GYRO_CAL_MAX_SPREAD)
            ok = false;                       /* 期间手柄动了 */
        if (!(nb[i] > -GYRO_BIAS_MAX && nb[i] < GYRO_BIAS_MAX))
            ok = false;                       /* 读坏了（也挡 NaN） */
    }
    if (!ok) {
        s_cal_failed = true;
        return;
    }
    for (int i = 0; i < 3; i++)
        s_bias[i] = nb[i];
    s_have_bias = true;
    s_bias_new = true;
}

bool gyro_read_hv(const uint8_t *p, int32_t *h_raw, int32_t *v_raw)
{
    const float dz = (float)s_cfg.deadzone;
    float out_h, out_v;

    /* 本体轴：水平/垂直各自取配置指定的那条轴的角速度，扣零偏，不做姿态变换。 */
    out_h = (float)GYRO_BASE_SIGN_H *
            ((float)gyro_raw_lsb(p, s_cfg.axis_h) / GYRO_LSB_PER_DEG_S - s_bias[s_cfg.axis_h]);
    out_v = (float)GYRO_BASE_SIGN_V *
            ((float)gyro_raw_lsb(p, s_cfg.axis_v) / GYRO_LSB_PER_DEG_S - s_bias[s_cfg.axis_v]);

    if (s_cfg.inv_h) out_h = -out_h;
    if (s_cfg.inv_v) out_v = -out_v;

    /* 减去式死区：边界连续，微调不会「黏住再猛跳」 */
    if (out_h > dz)       out_h -= dz;
    else if (out_h < -dz) out_h += dz;
    else                  out_h = 0.f;
    if (out_v > dz)       out_v -= dz;
    else if (out_v < -dz) out_v += dz;
    else                  out_v = 0.f;

    *h_raw = (int32_t)(out_h * GYRO_LSB_PER_DEG_S + (out_h < 0.f ? -0.5f : 0.5f));
    *v_raw = (int32_t)(out_v * GYRO_LSB_PER_DEG_S + (out_v < 0.f ? -0.5f : 0.5f));
    return true;
}

void gyro_get_status(gyro_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->dir = GYRO_DIR_LOCAL;
    out->flags = (uint8_t)(((s_manual || s_cal_req) ? GYRO_FLAG_MANUAL_CAL : 0) |
                           (s_have_bias ? GYRO_FLAG_CONFIDENT : 0) |
                           (s_cal_failed ? GYRO_FLAG_CAL_FAILED : 0));
    out->confidence = s_have_bias ? 255 : 0;
    if (s_manual) {
        uint32_t prog = (uint32_t)(s_now_ms - s_manual_start) * 100u / GYRO_MANUAL_CAL_MS;
        out->cal_prog = (uint8_t)(prog > 100u ? 100u : prog);
    } else if (s_cal_req) {
        out->cal_prog = 0;
    } else {
        out->cal_prog = 255;
    }
    for (int i = 0; i < 3; i++)
        out->bias[i] = s_bias[i];
}
