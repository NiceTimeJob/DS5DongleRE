/*
 * gyro.h — 手柄体感：本体轴两轴角速度取值 + 静置校准（零偏）
 *
 * R1.3（2026-10-14）起没有自动零偏校准了：零偏只来自「静置校准」，由 ds5map.c 存进
 * LittleFS 的 gcal.bin，开机读回（gyro_set_bias）。不在校准时没有任何每帧计算。
 * 跟随重力（player space）早在 2026-10-11 就删了。
 * 这个头文件只暴露接口，不含任何 SDK 依赖，方便在主机上单独编出来跑仿真。
 */
#ifndef DS5MAP_GYRO_H_
#define DS5MAP_GYRO_H_

#include <stdint.h>
#include <stdbool.h>

/* ------------------------------------------------------------------ */
/* 体感方向                                                             */
/* ------------------------------------------------------------------ */
/* 只剩本体轴这一个值：GYRO_DIR_FOLLOW_GRAVITY 已按用户要求删除（2026-10-11）。 */
enum {
    /* 本体轴：水平/垂直轴各自直接取报文里的某条轴。手柄姿态一变，方向就跟着歪。 */
    GYRO_DIR_LOCAL = 1,
};

/* 本体轴编号，跟 map_gyro_axis 对齐 */
enum {
    GYRO_AXIS_PITCH = 0,
    GYRO_AXIS_YAW   = 1,
    GYRO_AXIS_ROLL  = 2,
};

typedef struct {
    uint8_t dir;       /* 保留字段：恒按 GYRO_DIR_LOCAL 处理 */
    uint8_t axis_h;    /* GYRO_AXIS_*：水平轴 */
    uint8_t axis_v;    /* GYRO_AXIS_*：垂直轴（默认俯仰） */
    uint8_t inv_h;
    uint8_t inv_v;
    uint8_t deadzone;  /* 死区，单位度/秒；减去式，不是硬截断 */
} gyro_cfg_t;

/* 字段一个都不能删：TLM_STATE 第 48 字节起按这个布局打包（ds5map.c tlm_gyro_pack）。 */
typedef struct {
    uint8_t dir;        /* 恒为 GYRO_DIR_LOCAL */
    uint8_t flags;      /* GYRO_FLAG_* */
    uint8_t confidence; /* R1.3 起：255 = 已有零偏（校准过 / 开机读回），0 = 没有 */
    uint8_t cal_prog;   /* 静置校准进度 0..100；255 = 没在校准 */
    float   grav[3];    /* 重力方向；已不再估计，恒为 0（字段保留给遥测） */
    float   bias[3];    /* 当前零偏，度/秒 */
} gyro_status_t;

#define GYRO_FLAG_GRAVITY_IN_USE 0x01   /* 恒 0 */
#define GYRO_FLAG_MANUAL_CAL     0x02   /* 静置校准进行中 */
#define GYRO_FLAG_CONFIDENT      0x04   /* R1.3：已有零偏（校准过，或开机从 gcal.bin 读回） */
#define GYRO_FLAG_GRAVITY_BAD    0x08   /* 恒 0 */
#define GYRO_FLAG_CAL_FAILED     0x10   /* R1.3：上一次静置校准失败（期间手柄动了 / 帧太少），没采用 */

/* 报文里角速度/加速度原始值 → 物理量的换算（Linux hid-playstation.c） */
#define GYRO_LSB_PER_DEG_S 16.0f    /* DS_GYRO_RES_PER_DEG_S 1024 */
#define GYRO_LSB_PER_G     8192.0f  /* DS_ACC_RES_PER_G 8192 */

/* 断开时调用：只取消正在进行的校准，存下来的零偏保留 */
void gyro_reset(void);
void gyro_apply_cfg(const gyro_cfg_t *cfg);
/* 只在静置校准期间需要调用（gyro_calibrating() 为真时每帧一次）；p 是映射前的 63 字节负载 */
void gyro_update(const uint8_t *p, uint32_t now_ms);
/* 本体轴两条轴的角速度，单位 raw LSB（16 = 1 度/秒），已扣零偏/死区、已反向 */
bool gyro_read_hv(const uint8_t *p, int32_t *h_raw, int32_t *v_raw);
/* 遥测用状态 */
void gyro_get_status(gyro_status_t *out);

/* 静置校准：ISR 里调用安全（只置标志位，真正的活儿在 gyro_update 里） */
void gyro_cal_start(void);
void gyro_cal_cancel(void);
bool gyro_calibrating(void);

/* R1.3：零偏持久化。开机从 flash 读回后 gyro_set_bias；校准成功后 gyro_take_new_bias 取出去落盘 */
void gyro_set_bias(const float bias[3]);
bool gyro_take_new_bias(float out[3]);

#endif /* DS5MAP_GYRO_H_ */
