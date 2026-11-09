#include "config.h"
#include <string.h>
#include "debug_log.h"

extern int bt_settings_set_bin(const char *key, const uint8_t *value,
                               size_t length);
extern int bt_settings_get_bin(const char *key, uint8_t *value,
                               size_t exp_len, size_t *real_len);

#define CONFIG_EF_KEY  "donglere_cfg"

static struct config_body cfg;

/* Single source of truth for "factory defaults". Previously a version mismatch
 * did memset(0xFF) and then only fixed a few fields, leaving e.g.
 * haptics_gain = NaN and polling_rate_mode = 3 behind（那两个字段 R1.9 已删）。 */
static void config_set_defaults(struct config_body *b)
{
    memset(b, 0, sizeof(*b));
    b->config_version    = CONFIG_VERSION;
    b->disable_led       = 0;     /* status LED stays on */
    b->polling_rate_mode = 1;     /* 500 Hz */
    b->enable_wake       = 0;
    b->enable_usb_sn     = 1;     /* DS5DongleRE always provides serial */
    b->usb_stealth       = 0;     /* USB visible at boot (non-stealth) */
    b->led_r             = 0xFF;
    b->led_g             = 0xFF;
    b->led_b             = 0xFF;
    b->audio_haptic      = 1;     /* R1.8：音频触觉默认「自动（游戏优先）」——游戏没发
                                   * 原生震动时把声音合成成震动，新芯片刷完即生效 */
    b->pad_sleep_min     = 0;     /* R1.7：手柄自动关机默认关 */
    /* R3.28：音频编码默认 = S37 那套「能出声就行」（48 kbps / 宽带 8k / 复杂度 0）。
     * 网页「恢复默认」也是写到这三个值；0 在这里有"没配过"的含义，所以默认值必须显式给。 */
    b->audio_kbps        = 48;
    b->audio_bw          = 3;
    b->audio_cx          = 0;
}

void config_validate(void)
{
    struct config_body *b = &cfg;

    if (b->config_version != CONFIG_VERSION) {
        LOG_WRN("[CFG] Version mismatch (%d vs %d), resetting to defaults\n",
               b->config_version, CONFIG_VERSION);
        config_set_defaults(b);
        /* fall through: the checks below are still cheap and keep the
         * defaults honest if they ever drift */
    }

    if (b->disable_led > 1)
        b->disable_led = 0;
    /* R1.1：USB 回报率固定 500 Hz（用户决定，网页不再提供修改）：主机写什么都当 1。
     * 字段留在结构体里，免得为它再升一次 CONFIG_VERSION、把基础配置又清一遍。 */
    b->polling_rate_mode = 1;
    if (b->enable_usb_sn > 1)
        b->enable_usb_sn = 0;
    if (b->disable_mic > 1)
        b->disable_mic = 0;
    if (b->disable_speaker > 1)
        b->disable_speaker = 0;
    if (b->enable_wake > 1)
        b->enable_wake = 1;
    if (b->usb_stealth > 1)
        b->usb_stealth = 0;
    /* Default LED color to white for configs migrated from older firmware
     * (memset(0) leaves these as 0,0,0 which would be black). */
    if (b->led_r == 0 && b->led_g == 0 && b->led_b == 0) {
        b->led_r = 0xFF;
        b->led_g = 0xFF;
        b->led_b = 0xFF;
    }
    if (b->audio_haptic > 2)
        b->audio_haptic = 0;
    /* R3.28：音频编码三参数——越界一律回默认（网页写进来的任何东西都不该让编码器读到离谱值） */
    if (b->audio_kbps < 8 || b->audio_kbps > 160)
        b->audio_kbps = 48;
    if (b->audio_bw > 5)
        b->audio_bw = 3;
    if (b->audio_cx > 10)
        b->audio_cx = 0;
    /* pad_sleep_min 不需要钳制：u8 的 0-255 分钟都是合法值。 */
}

void config_load(void)
{
    size_t rlen = 0;
    memset(&cfg, 0, sizeof(cfg));
    int err = bt_settings_get_bin(CONFIG_EF_KEY, (uint8_t *)&cfg,
                                  sizeof(cfg), &rlen);
    /* R1.11 修：长度下限必须是 sizeof(cfg)——R1.9 把结构体缩到 13 字节时漏改了
     * 原来写死的 19（23 字节时代的下限），存进去的 13 字节每次开机都被判成
     * "没有有效配置"而回落默认，表现为「保存不上、断电就掉」（2026-10-15 用户报，
     * persist_test.py 真机复现：写入+保存回读正常，软重启后全掉）。 */
    if (err != 0 || rlen < sizeof(cfg)) {
        LOG_INF("[CFG] No saved config, using defaults\n");
        config_set_defaults(&cfg);
    }
    /* 不再有老布局的迁移：结构体自带版本号（现 13 字节 / v9，R1.9 删了 7 个音频调节字段）。
     * 老配置由下面的版本号检查拦掉，整份回落默认值 —— 不认识的就别猜。 */
    config_validate();
    LOG_INF("[CFG] Loaded: wake=%d led_off=%d poll=%d (rlen=%u)\n",
           cfg.enable_wake, cfg.disable_led,
           cfg.polling_rate_mode, (unsigned)rlen);
}

bool config_save(void)
{
    int rc = bt_settings_set_bin(CONFIG_EF_KEY, (const uint8_t *)&cfg,
                                 sizeof(cfg));
    if (rc == 0) {
        LOG_INF("[CFG] Saved to flash\n");
        return true;
    }
    LOG_ERR("[CFG] Save failed: %d\n", rc);
    return false;
}

struct config_body *config_get(void)
{
    return &cfg;
}

void config_set(const uint8_t *data, uint16_t len)
{
    uint16_t copy_len = len < sizeof(cfg) ? len : sizeof(cfg);
    memcpy(&cfg, data, copy_len);
    config_validate();
    LOG_INF("[CFG] Updated from host (%d bytes)\n", copy_len);
}
