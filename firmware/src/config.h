#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Configuration body — 主机侧用 0xF6/0xF7 特征报告读写（网页 page-config.js 的
 * OFF/SIZE 必须与本结构逐字节对齐；改布局就改 CONFIG_VERSION，老配置一律回落默认值）。
 *
 * 布局最初照抄 DS5DongleRE 的 Config_body，但 S15 起不再兼容它的布局了：
 * controller_mode（手柄模式：DS5/DSE/自动）和 dse_detected（自动模式记上次是不是
 * DSE）两个字段随"永远以普通 DS5 出现"的改动一起删除 —— 固件里已经没人读、
 * 也没人写。R1.9 又删了 7 个音频调节字段（见结构体下方历史注记）。
 */
struct __attribute__((packed)) config_body {
    uint8_t config_version;
    uint8_t disable_led;          /* bool */
    uint8_t polling_rate_mode;    /* 固定 1（500Hz）；字段仅占位，网页不提供修改 */
    uint8_t enable_usb_sn;        /* bool */
    uint8_t disable_mic;          /* bool       — disable mic passthrough */
    uint8_t disable_speaker;      /* bool       — disable Opus speaker encoding */
    uint8_t enable_wake;          /* bool */
    uint8_t usb_stealth;          /* bool — hide USB until BT controller connects */
    uint8_t led_r;                /* [0-255] custom LED red   (0xFF = default white) */
    uint8_t led_g;                /* [0-255] custom LED green (0xFF = default white) */
    uint8_t led_b;                /* [0-255] custom LED blue  (0xFF = default white) */
    uint8_t audio_haptic;         /* 0=off, 1=auto(game priority), 2=force */
    uint8_t pad_sleep_min;        /* R1.7：手柄自动省电关机（分钟），0=关；到点发 DualSense 关机命令，按 PS 回连 */
    /* R3.28（2026-10-17 用户要求：音频挡位要**断电记忆**）：Opus 编码三参数进配置体。
     * 0 = 用固件默认（kbps 48 / bw 3 宽带 / cx 0），合法值域见 config_validate()。 */
    uint8_t audio_kbps;           /* 8..160 kbps；0 = 默认 48 */
    uint8_t audio_bw;             /* 0 自动 / 1 窄带 / 2 中带 / 3 宽带 / 4 超宽带 / 5 全带 */
    uint8_t audio_cx;             /* 0..10 编码复杂度 */
};

/* S33（2026-10-12）：原作者的 ps_shortcut_enabled / tp_mode / tp_mode_enabled_mask /
 * tp_mouse_sensitivity 四个字段随他的快捷键与触摸板模式一起删除，结构体 26 → 22 字节。 */
/* R1（2026-10-14）：加 led_quiet_connected，结构体 22 → 23 字节；
 * R1.7：「静默 LED」整个删除（和「禁用 LED」重复），同偏移换成 pad_sleep_min，仍 23 字节。
 * R1.9（2026-10-15，用户决定）：删 haptics_gain / speaker_volume / headset_volume /
 * speaker_gain / audio_buffer_length / trigger_reduce / lock_volume 共 7 个音频调节字段
 * ——不再让用户调，固件里固定：触觉增益 2.0（audio.c）、音量 127/127（primer 注入，
 * 之后主机音量修改照常透传）、缓冲深度 64（audio.c）；增益/扳机削减的注入逻辑随删。
 * 结构体 23 → 13 字节，CONFIG_VERSION 8 → 9（基础配置再回落一次默认值）。
 * R2.3（2026-10-16，用户决定）：R2.1 的 trig_vib / trig_vib_shield（扳机振动派生）
 * 整段删除（体验不好，用户说后面自己再研究）—— 结构体回到 13 字节、
 * CONFIG_VERSION 回到 9，所以**设备上 R2.0 的旧配置继续有效、不用重设**。
 * R3.28（2026-10-17）：加 audio_kbps / audio_bw / audio_cx 三个字段（用户要求音频挡位断电记忆），
 * 结构体 13 → 16 字节，CONFIG_VERSION 9 → 10（基础配置回落一次默认值）。
 */
#define CONFIG_VERSION  10

void config_load(void);
bool config_save(void);
void config_validate(void);

struct config_body *config_get(void);
void config_set(const uint8_t *data, uint16_t len);

/* Convenience accessors for hot-path fields */
static inline bool config_wake_enabled(void)       { return config_get()->enable_wake; }
static inline bool config_led_disabled(void)        { return config_get()->disable_led; }
static inline uint8_t config_polling_mode(void)     { return config_get()->polling_rate_mode; }
static inline bool config_speaker_disabled(void)    { return config_get()->disable_speaker; }
static inline bool config_mic_disabled(void)         { return config_get()->disable_mic; }
static inline bool config_usb_stealth(void)         { return config_get()->usb_stealth; }

#endif /* CONFIG_H */
