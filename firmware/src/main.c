#include "board.h"
#include "bflb_mtimer.h"
#include "bflb_gpio.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

#include "bt_hid_host.h"
#include "ds5_protocol.h"
#include "usb_gamepad.h"
#include "ds5_usb_audio.h"
#include "audio.h"
#include "usb_wake.h"
#include "led_status.h"
#include "state_mgr.h"
#include "config.h"
#include "dse.h"

/* R2：只打开引擎"初始化"这一步（其余开关保持关闭）。
 * 必须在 include ds5map.h 之前定义，否则会被头文件兜底值挡住。 */
#ifndef DS5MAP_ENGINE_ENABLE
#define DS5MAP_ENGINE_ENABLE 2
#endif

#include "ds5map.h"
#include "diag.h"      /* S6：被拒的脏帧记进诊断环（RPC 可读） */
#include "perf.h"      /* S33：各任务 CPU 占比每秒采样 */

#include "bl616_glb.h"
#include "btble_lib_api.h"
#include "rfparam_adapter.h"
#include "hci_driver.h"
#include "bflb_mtd.h"
#include "easyflash.h"
#include "board_config.h"
#include "debug_log.h"
#include <string.h>

#define STACK_WORDS(bytes) \
    (((bytes) + sizeof(StackType_t) - 1) / sizeof(StackType_t))

#define BT_TASK_STACK_SIZE    STACK_WORDS(1024*24)
#define BT_TASK_PRIORITY      (configMAX_PRIORITIES - 3)
#define USB_TASK_STACK_SIZE   STACK_WORDS(1024*12)
#define USB_TASK_PRIORITY     (configMAX_PRIORITIES - 1)
#define AUDIO_TASK_STACK_SIZE STACK_WORDS(1024*32)
#define AUDIO_TASK_PRIORITY   (configMAX_PRIORITIES - 2)
#define MIC_TASK_STACK_SIZE   STACK_WORDS(1024*12)
#define MIC_TASK_PRIORITY     (configMAX_PRIORITIES - 4)
#define LED_TASK_STACK_SIZE   STACK_WORDS(1024*2)
#define LED_TASK_PRIORITY     (tskIDLE_PRIORITY + 1)
#define BOOT_HOLD_TICKS      30   /* 30 x 100ms = 3 seconds */
#define BOOT_CLICK_WINDOW    5    /* 500ms window between clicks */
#define RECONNECT_TIMEOUT_MS 2000
#define HANDSHAKE_TIMEOUT_US  (10ULL * 1000000ULL)
#define CONNECTING_TIMEOUT_TICKS pdMS_TO_TICKS(8000)
#define L2CAP_FALLBACK_TIMEOUT_TICKS pdMS_TO_TICKS(3000)
/* S29: actively page the bonded controller every ~8s while disconnected
 * (unit = main-loop idle ticks, ~100ms each). Passive page scan alone can't
 * recover a controller that never noticed our reboot (power blip shorter
 * than its link supervision timeout): it thinks the old link is alive, so
 * it neither pages us nor answers inquiry — deadlock until PS is pressed. */
#define PAGE_RETRY_TICKS 80
/* S34（用户决定，2026-10-12 晚）：「你就不要主动呼唤了，不然一直连不上，要让手柄去主动连接才对」。
 * 主动 page 的 5 s 里接收器收不到手柄按 PS 发来的回连；用户宁可自己按 PS。改成 1 可恢复 S29 的主动呼叫。 */
#define ACTIVE_PAGE_RECONNECT 0
/* S31（照上游 3.21）：等待连接期间每 ~10 s 读一次 HCI Scan_Enable 对账；
 * SCANNING 卡超过 30 s 强制结束（inquiry 完成回调丢了就会永远卡住）。 */
#define HB_TICKS 100
#define SCAN_STUCK_US (30ULL * 1000000ULL)

#define INPUT_QUEUE_ITEM_SZ  DS5_BT_INPUT_REPORT_SIZE
#define USB_OUTPUT_BUF_SZ    64  /* Report ID (1) + SetStateData (up to 63 for DSE) */

/* R1.6：输出报告（震动 / 灯 / 扳机）排队时带上 USB ISR 收到的时刻（µs 低 32 位），
 * bt 任务转发时量「USB 收到 → 交给蓝牙」的延迟进 PERF。 */
typedef struct {
    uint32_t t_us;
    uint8_t  data[USB_OUTPUT_BUF_SZ];
} usb_out_msg_t;

static QueueHandle_t input_queue;
static QueueHandle_t output_queue;
static volatile bool ds5_connected = false;
static volatile bool ever_connected = false;
static volatile bool battery_low = false;
static volatile bool bt_init_done = false;
/* Pairing mode: entered by holding the BOOT button 3 s. Persists (with a
 * ~3 Hz LED blink) until a controller pairs and connects, or power-off. */
static volatile bool pairing_mode = false;

/* Single source of truth for the status LED.
 *   connected       → solid
 *   pairing mode    → ~3 Hz blink (stays until paired or power-off)
 *   otherwise       → ~1 Hz blink (waiting for controller)
 * R1.14（用户要求）：「禁用 LED」开着时除对频快闪外一律灭——不再是亮 1 分钟才灭，
 * 电量告警也不闪。板上另一颗红灯是电源灯、硬件直连，固件管不到。
 * Also registers the resting pattern so one-shot event flashes return to it. */
static void led_refresh_state(void)
{
    enum led_pattern p = LED_WAITING;

    if (ds5_connected)
        p = LED_CONNECTED;
    else if (pairing_mode)
        p = LED_PAIRING;

    if (config_led_disabled() && !pairing_mode)
        p = LED_OFF;

    led_status_set_default(p);
    led_status_set(p);
}

static volatile uint8_t cached_battery_level = 0xFF;
static volatile uint8_t cached_battery_state = 0;

static volatile uint64_t handshake_start_us;
static volatile TickType_t connecting_start_tick = 0;
static volatile bool prev_led_disabled = false;
static volatile bool scan_after_disconnect = false;
/* Deferred USB soft-disconnect: wait N seconds after BT disconnect before
 * pulling USB, so quick controller switching preserves Windows audio session. */
#define USB_DEFERRED_DISC_US  (10ULL * 1000000ULL)
static volatile uint64_t usb_deferred_disc_us = 0;
/* S33（用户决定）：掉线 10 s 后把 USB 软摘除的原设计关掉。摘除后主机什么都读不到，
 * 恰好把掉线现场藏起来（18:27 那次就是这样丢的）。要恢复原行为把它改成 1。 */
#define USB_DETACH_AFTER_DROP 0
static bool usb_detached = false;   /* USB 当前是否处于软摘除（隐身开机 / 掉线后摘除） */
/* Deferred primer re-send: after CONNECTED, wait ~300ms then re-send
 * volume to ensure the controller has fully initialized its HID channel. */
static volatile uint64_t primer_resend_us = 0;
#define PRIMER_RESEND_DELAY_US  (300ULL * 1000ULL)
/* Stealth mode: frames of Windows USB output to wait before sending primer */
static volatile uint8_t stealth_primer_countdown = 0;
static uint8_t output_seq = 0;  /* sequence counter for 0x31 BT output */
static bool first_input_logged = false;
static uint32_t usb_fwd_count = 0;
/* R2.4：蓝牙断开（BT_HID_STATE_IDLE）后的「全部松开」交给 usb_task 做，on_hid_state 只置这个标志。
 * on_hid_state 跑在蓝牙协议栈的线程里；以前它在那边直接发一帧回中，同一时刻 usb_task 可能正拿着
 * 断开前最后一帧真数据过引擎、交给 USB —— 按着的键要么盖掉回中帧，要么排在它后面发出去，
 * 电脑就停在"还按着"的状态。 */
static volatile bool disc_neutral_req = false;

/* R1.7：手柄自动省电关机 —— 看「用户操作」，不看「有没有报告」：
 * DS5 输入报告每秒几百帧，没人碰也照发，所以必须按值判断。 */
#define PAD_STICK_ACTIVE   16   /* 摇杆离中 |v-0x80| 阈值；以下算漂移 */
#define PAD_TRIG_ACTIVE     8   /* 扳机按下深度阈值（持续压住 = 持续活动：赛车游戏的油门） */
/* R1.10：体感（陀螺/加速度）从活动判定里剔除。原来有 PAD_IMU_DELTA=800 帧间阈值，
 * 理论静置噪声够不着，但**震动马达**（原生震动、尤其 R1.8 起默认开的音频触觉，增益还
 * 固定最大）会把 IMU 摇出远超阈值的尖峰——只要手柄在震，计时器永远被重置，自动关机
 * 永远不触发（用户设 1 分钟实测不关机，2026-10-15）。马达震和手拿着晃在 IMU 上无法
 * 区分，只能整个不算；纯体感零按键玩 N 分钟的场景被认为不存在。 */

static volatile uint32_t last_activity_ms;  /* usb_task 写、bt_task 读；32 位读写原子，回绕用无符号减法免疫 */
static uint8_t  act_prev_btn[3]   = {0x08, 0, 0};              /* payload[7..9]；idle = 十字键松开 */
static uint8_t  act_prev_touch[8] = {0x80,0,0,0, 0x80,0,0,0};  /* 每指 4 字节，首字节 bit7 = 没摸 */

static void pad_sleep_reset(void)   /* 每次连上：计时起算点 + 基准帧 */
{
    last_activity_ms = (uint32_t)(bflb_mtimer_get_time_us() / 1000ULL);
    act_prev_btn[0] = 0x08; act_prev_btn[1] = 0; act_prev_btn[2] = 0;
    act_prev_touch[0] = 0x80; act_prev_touch[4] = 0x80;
}

static volatile uint64_t out_isr_ts_us = 0;   /* timestamp set in USB ISR */
static volatile uint32_t out_drop_count = 0;   /* queue-full drops */

/* R1.9：apply_config_overlay 已删（trigger_reduce / speaker_gain / lock_volume /
 * 音量回写都不再是配置项）——输出帧原样透传，不再叠加任何配置。 */

/* Build a standard 0x31 BT output report (78 bytes) for ongoing output.
 * set_state_data: 47 bytes of SetStateData
 * seq: sequence counter
 * out: 78-byte output buffer */
static void build_bt_output(const uint8_t *set_state_data, uint16_t len,
                            uint8_t seq, uint8_t *out)
{
    memset(out, 0, DS5_BT_OUTPUT_REPORT_SIZE);
    out[0] = DS5_BT_OUTPUT_REPORT_ID;
    out[1] = seq << 4;
    out[2] = DS5_BT_OUTPUT_TAG;
    if (len > DS5_USB_OUTPUT_PAYLOAD_LEN)
        len = DS5_USB_OUTPUT_PAYLOAD_LEN;
    memcpy(out + 3, set_state_data, len);

    uint32_t crc = ds5_crc32(DS5_BT_OUTPUT_CRC_SEED, out,
                             DS5_BT_OUTPUT_REPORT_SIZE - 4);
    ds5_write_le32(&out[DS5_BT_OUTPUT_REPORT_SIZE - 4], crc);
}

/* Build an extended 0x32 BT output report (142 bytes).
 * Matches DS5DongleRE exactly: pkt[1]=0x10 hardcoded (PS5 native BT format). */
static void build_bt_output_ext(const uint8_t *set_state_data, uint16_t len,
                                uint8_t *out)
{
    memset(out, 0, DS5_BT_OUTPUT_EXT_SIZE);
    out[0] = DS5_BT_OUTPUT_REPORT_ID_EXT;
    out[1] = 0x10;  /* hardcoded — matches PS5 native BT format */
    out[2] = DS5_BT_OUTPUT_EXT_TAG;
    out[3] = DS5_BT_OUTPUT_EXT_PAYLOAD;
    if (len > DS5_BT_OUTPUT_EXT_PAYLOAD)
        len = DS5_BT_OUTPUT_EXT_PAYLOAD;
    memcpy(out + 4, set_state_data, len);

    uint32_t crc = ds5_crc32(DS5_BT_OUTPUT_CRC_SEED, out,
                             DS5_BT_OUTPUT_EXT_SIZE - 4);
    ds5_write_le32(&out[DS5_BT_OUTPUT_EXT_SIZE - 4], crc);
}

/* Default SetStateData sent as LED primer on first connect / reconnect.
 * Matches DS5DongleRE's update_state() primer exactly.
 *
 * !!!  DO NOT CHANGE THE FOLLOWING VALUES WITHOUT CAREFUL TESTING  !!!
 *
 * [0] flags0 = 0x80 (AllowAudioControl ONLY)
 *   — Adding more flags here causes the controller to misinterpret the
 *     primer as a real game output. LED will be overridden on first connect.
 *
 * [1] flags1 = 0x04 (AllowLedColor ONLY)
 *   — Must include AllowLedColor so the controller accepts our LED color.
 *     Do NOT add AllowRumbleEmulation or other flags; they break LED init.
 *
 * [38] = 0x03 (AllowLightBrightnessChange | AllowColorLightFadeAnimation)
 *   — Required so byte[41] and byte[42] are processed by the controller.
 *
 * [41] LightFadeAnimation = 0x02 (FadeOut)
 *   !!!  MUST BE FadeOut (2). Do NOT change to Nothing (0) or FadeIn (1).  !!!
 *   — Verified by extensive testing (v4.4d vs v4.5d): changing this to 0
 *     silently breaks LED on ALL subsequent game sessions. The controller
 *     ignores our color and keeps whatever the OS sent last (usually blue).
 *     This value matches DS5DongleRE reference firmware exactly.
 *
 * [42] LightBrightness = 0x00 (Bright)
 *   — Must be Bright (0). DS5DongleRE uses the same value.
 *
 * [44..46] = {0xFF, 0xFF, 0xFF} — initial color: white
 *   — Safe to change the color. Changing the animation/brightness is NOT. */
static const uint8_t init_set_state[DS5_USB_OUTPUT_PAYLOAD_LEN] = {
    /* [0] flags0: AllowAudioControl only — DO NOT ADD MORE FLAGS */
    0x80,
    /* [1] flags1: AllowLedColor only — DO NOT ADD MORE FLAGS */
    0x04,
    /* [2..9] all zeros (no rumble, no volumes, no mute) */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* [10..20] RightTriggerFFB (11 bytes) */ 0,0,0,0,0,0,0,0,0,0,0,
    /* [21..31] LeftTriggerFFB (11 bytes) */  0,0,0,0,0,0,0,0,0,0,0,
    /* [32..37] zeros (Timestamp, MotorPowerLevel, AudioControl2) */ 0,0,0,0,0,0,
    /* [38] AllowLightBrightnessChange | AllowColorLightFadeAnimation — required */
    0x03,
    /* [39] HapticLowPassFilter */ 0x00,
    /* [40] UNK */  0x00,
    /* [41] LightFadeAnimation: FadeOut=2 — MUST BE 2, see note above */
    0x02,
    /* [42] LightBrightness: Bright=0 — MUST BE 0, see note above */
    0x00,
    /* [43] PlayerIndicators: none */
    0x00,
    /* [44] LedRed   — color only, safe to change */
    0xFF,
    /* [45] LedGreen — color only, safe to change */
    0xFF,
    /* [46] LedBlue  — color only, safe to change */
    0xFF,
};

/* ---- BT HID callbacks ---- */

/* Persistent cache of the last trigger configuration the game sent.
 * The DualSense controller is stateful (keeps trigger config until changed),
 * but on reconnection its state is lost. We replay this cache after primer
 * so the controller immediately resumes adaptive triggers. */
static uint8_t cached_trigger_flags = 0;          /* bits 2,3 of flags0 */
static uint8_t cached_trigger_r[11];              /* bytes 10-20: RightTriggerFFB */
static uint8_t cached_trigger_l[11];              /* bytes 21-31: LeftTriggerFFB */

/* ---- R2.6 扳机效果注入（关/阻力/扳机/自动扳机）----
 * 字节由 ds5map_s2t_effect() 按配置算；这里负责「什么时候下发」。
 * 效果是**状态式**的（手柄记住效果直到被改写），所以只在「这一侧从没有我们的效果变成有」
 * 的那一帧写一次并置上 flags0 的对应位；之后一个字都不动 —— 每帧重发会把刚压过去的
 * 扳机墙重新装上，玩家会感到压穿了又被顶住。
 * 游戏优先：这一侧最近 1 秒收到过游戏下发的扳机效果就整段让位（连字节都不碰，游戏那份
 * 照原样过），游戏停手满 1 秒后我们再补回去。 */
#define S2T_GAME_HOLD_MS 1000

/* R2.6b：**扳机效果必须走 0x32 扩展报文**。
 * 主转发路径每帧发的是 0x31（78 字节）—— 那个报文手柄会处理震动/灯/音频，但**自适应扳机块
 * 被忽略**（2026-10-17 用户实测：网页上设了阻力，按扳机完全没手感；"卡点从来没摸到过"也说得通）。
 * 连上时那次 primer 用的是 0x32（142 字节，tag 0x90，"PS5 native BT format"，见
 * build_bt_output_ext 的注释），所以这里改成：**只要这一帧的扳机块变了**（游戏换了效果 or
 * 我们装了/撤了自己的效果），在正常 0x31 之后再补发一个同内容的 0x32。
 * 只在"变了"的那一帧补，所以不会持续翻倍蓝牙流量。 */
static bool    out_trigger_dirty = false;

/* R2.8：自动扳机（VIBRATION）会衰减，要按真游戏的节奏重发（捕获样本 duration_ms 12~51 ms）。
 * 50 ms 足够跟上"机枪感"，又不会每帧都发；别的模式是状态式的，发一次就不动。 */
#define S2T_AUTO_RESEND_MS 50
static uint32_t s2t_sent_ms[2];         /* 上一次把我们的效果写进报文的时间 */

static uint32_t game_trig_ms[2];        /* [0] 右 / [1] 左：游戏最后一次下发扳机效果的时刻 */
static uint8_t  s2t_applied[2];         /* 我们的卡点此刻是不是已经装在手柄上 */
static uint8_t  s2t_last[2][11];        /* 已经下发过的那份字节（配置改了要重新下发） */
/* R3.27：配置改了 → **强制重发一次**，但**不许清 s2t_applied**。
 * 以前这里用 s2t_forget()，结果"把效果关掉"时 `if (s2t_applied[side])` 不成立 →
 * 撤下（发 OFF）那一步被跳过 → 手柄上一直留着上一个效果
 * （用户实测：点「重置本层」参数全关了，按 L2 还有扳机感）。 */
static bool     s2t_force;

static void s2t_forget(void)            /* 手柄断开/重连：那边已经没有我们的卡点了 */
{
    s2t_applied[0] = s2t_applied[1] = 0;
}

static void s2t_apply_to_payload(uint8_t *payload)
{
    uint32_t now = (uint32_t)(bflb_mtimer_get_time_us() / 1000ULL);

    for (int side = 0; side < 2; side++) {
        uint8_t off = side ? 21 : 10;
        uint8_t bit = side ? 0x08 : 0x04;          /* flags0：bit2 右 / bit3 左 */
        uint8_t buf[11];
        bool game = (uint32_t)(now - game_trig_ms[side]) < S2T_GAME_HOLD_MS;
        if (ds5map_s2t_ignore_game())
            game = false;              /* R3.0：网页上没勾「原生游戏优先」→ 忽略游戏的效果，一直用我们的 */
        bool want = ds5map_s2t_effect((uint8_t)side, buf) && !game;

        if (!want) {
            /* 让位 / 撤下：之前是我们装的就还回游戏那份（游戏从没发过就发官方 OFF） */
            if (s2t_applied[side]) {
                if (cached_trigger_flags & bit)
                    memcpy(payload + off, side ? cached_trigger_l : cached_trigger_r, 11);
                else {
                    memset(payload + off, 0, 11);
                    payload[off] = DS5_TRIGGER_OFF;
                }
                payload[0] |= bit;
                s2t_applied[side] = 0;
                memset(s2t_last[side], 0, 11);
                out_trigger_dirty = true;              /* 撤下也要走 0x32 才生效 */
            }
            continue;
        }
        if (!s2t_force && s2t_applied[side] && memcmp(s2t_last[side], buf, 11) == 0) {
            /* 已经装好了：状态式效果这一帧别碰；但**自动扳机会衰减** → 按真游戏的节奏重发 */
            if (!(ds5map_s2t_needs_refresh((uint8_t)side) &&
                  (uint32_t)(now - s2t_sent_ms[side]) >= S2T_AUTO_RESEND_MS))
                continue;
        }
        memcpy(payload + off, buf, 11);
        memcpy(s2t_last[side], buf, 11);
        payload[0] |= bit;
        s2t_applied[side] = 1;
        s2t_sent_ms[side] = now;
        out_trigger_dirty = true;                  /* 装上了：让转发路径补一个 0x32 */
    }

    s2t_force = false;               /* R3.27：这一趟已经"强制重发"过了 */

    /* 诊断：把「扳机效果现在什么状态」摆进遥测（TLM_STATE[47]，见 ds5map.h 的布局注释），
     * 这样网页测试页 / watch_s2t.py 能回答"我明明配了效果，为什么手上没感觉"：
     *   我们装上了（bit0/1）还是让位给游戏了（bit2/3 游戏在管）。每帧覆盖写。
     * **位号跟配置区统一：bit0/bit2 = 左(L2)、bit1/bit3 = 右(R2)** —— 这个函数里的 side
     * 是「0 = 右 / 1 = 左」（跟 off/bit 一致），所以下面按 side 反向摆位。 */
    {
        uint8_t st = 0;
        if (s2t_applied[0]) st |= 0x02;                                   /* side0 = 右 → bit1 */
        if (s2t_applied[1]) st |= 0x01;                                   /* side1 = 左 → bit0 */
        if ((uint32_t)(now - game_trig_ms[0]) < S2T_GAME_HOLD_MS) st |= 0x08;   /* 右在管 → bit3 */
        if ((uint32_t)(now - game_trig_ms[1]) < S2T_GAME_HOLD_MS) st |= 0x04;   /* 左在管 → bit2 */
        ds5map_s2t_set_state(st);
    }
}

/* Persistent cache of the last LED color the game set.
 * Same principle: controller loses LED state on BT disconnect,
 * games don't re-send color after USB replug. */
static bool    cached_led_valid = false;
static uint8_t cached_led_rgb[3];                 /* bytes 44-46: R, G, B */
static uint8_t cached_player_ind = 0;             /* byte 43: PlayerIndicators */
static bool    cached_player_valid = false;
static bool    game_was_active_at_disconnect = false;  /* snapshot at BT disconnect */

/* Called from the output forwarding path to snapshot game state
 * whenever the game sends a report with relevant flags set. */
static void cache_output_state(const uint8_t *set_state_data)
{
    uint8_t f0 = set_state_data[0];
    uint8_t f1 = set_state_data[1];

    /* Cache trigger data (accumulate flags independently — game may send
     * left and right in separate frames) */
    if (f0 & 0x04) {
        if (memcmp(cached_trigger_r, set_state_data + 10, 11) != 0)
            out_trigger_dirty = true;          /* 游戏换效果了 → 这一帧要补 0x32 */
        cached_trigger_flags |= 0x04;
        memcpy(cached_trigger_r, set_state_data + 10, 11);
        /* R2.5：只有游戏真的发了一个**效果**才算是「游戏在管这一侧」。很多游戏每帧都带
         * 扳机块但里面是 OFF（0x00/0x05，＝不设效果），那种不该把我们的卡点压掉
         * （R2.1 的扳机振动当年也是这条规矩）。 */
        if (cached_trigger_r[0] != 0x00 && cached_trigger_r[0] != DS5_TRIGGER_OFF)
            game_trig_ms[0] = (uint32_t)(bflb_mtimer_get_time_us() / 1000ULL);
    }
    if (f0 & 0x08) {
        if (memcmp(cached_trigger_l, set_state_data + 21, 11) != 0)
            out_trigger_dirty = true;
        cached_trigger_flags |= 0x08;
        memcpy(cached_trigger_l, set_state_data + 21, 11);
        if (cached_trigger_l[0] != 0x00 && cached_trigger_l[0] != DS5_TRIGGER_OFF)
            game_trig_ms[1] = (uint32_t)(bflb_mtimer_get_time_us() / 1000ULL);
    }

    /* Cache LED color */
    if (f1 & 0x04) {
        cached_led_rgb[0] = set_state_data[44];
        cached_led_rgb[1] = set_state_data[45];
        cached_led_rgb[2] = set_state_data[46];
        cached_led_valid = true;
    }

    /* Cache player indicators */
    if (f1 & 0x10) {
        cached_player_ind = set_state_data[43];
        cached_player_valid = true;
    }
}

/* Send the 0x32 LED primer — shared by primer_cb and reconnect handler */
static void send_led_primer(void)
{
    state_mgr_init(init_set_state, DS5_USB_OUTPUT_PAYLOAD_LEN);
    struct config_body *cfg = config_get();
    uint8_t merged[DS5_USB_OUTPUT_PAYLOAD_LEN];
    state_mgr_get(merged, DS5_USB_OUTPUT_PAYLOAD_LEN);

    /* R1.9：音量不再是配置项，固定拉满——连上就把喇叭/耳机音量设到 127
     * （不注入的话手柄会保持默认低音量）；之后主机的音量修改照常透传。 */
    merged[0] |= 0x30;
    merged[4] = 127;
    merged[5] = 127;
    LOG_INF("[PRIMER] vol=max led=#%02X%02X%02X\n",
            cfg->led_r, cfg->led_g, cfg->led_b);

    merged[44] = cfg->led_r;
    merged[45] = cfg->led_g;
    merged[46] = cfg->led_b;
    state_mgr_set_led_color(cfg->led_r, cfg->led_g, cfg->led_b);
    state_mgr_clear_flags();

    /* Only replay trigger cache — games don't re-send trigger data after
     * USB re-enumeration. LED and player indicators are NOT replayed because
     * USB replug causes the host to re-send them; caching would cause stale
     * game colors to persist after game exit. */
    if (cached_trigger_flags) {
        merged[0] |= cached_trigger_flags;
        if (cached_trigger_flags & 0x04)
            memcpy(merged + 10, cached_trigger_r, 11);
        if (cached_trigger_flags & 0x08)
            memcpy(merged + 10 + 11, cached_trigger_l, 11);
    }

    /* R2.5：连上/重连时手柄的扳机效果已经全丢，我们的卡点要重新装一次（另外游戏一帧
     * 输出报告都不发时也靠这一次 —— 没有 PC 帧就没有别的地方能注入）。 */
    s2t_forget();
    s2t_apply_to_payload(merged);
    out_trigger_dirty = false;      /* R2.6b：primer 本身就是 0x32，不用再补一个 */

    uint8_t buf[DS5_BT_OUTPUT_EXT_SIZE];
    build_bt_output_ext(merged, DS5_USB_OUTPUT_PAYLOAD_LEN, buf);
    bt_hid_host_send_output(buf, DS5_BT_OUTPUT_EXT_SIZE);
}

/* R2.9：把"当前该发给手柄的状态"重发一遍（0x32 扩展报文）——**不动灯/音量**，
 * 内容 = 主机最后一次的输出状态 + 缓存里游戏的扳机效果 + 我们自己的扳机效果。
 * 用途：网页改完扳机参数立刻生效（不用断电上电）；每秒兜底一次防"被游戏顶掉/手柄重连"。 */
/* R3.13：forget=true 用于「配置改了立刻生效」（强制重发）；
 * forget=false 用于「撤下效果」。
 * R3.27：**两种都不许清 s2t_applied** —— 清了之后"把效果关掉"就走不到撤下分支，
 * 手柄会一直留着上一个效果（用户实测：点「重置本层」后按 L2 还有扳机感）。
 * 现在 forget=true 只是让下一趟跳过"字节没变就别发"的优化（s2t_force）。 */
static void pad_resend_state(bool force)
{
    uint8_t merged[DS5_USB_OUTPUT_PAYLOAD_LEN];
    state_mgr_get(merged, DS5_USB_OUTPUT_PAYLOAD_LEN);
    if (cached_trigger_flags) {
        merged[0] |= cached_trigger_flags;
        if (cached_trigger_flags & 0x04)
            memcpy(merged + 10, cached_trigger_r, 11);
        if (cached_trigger_flags & 0x08)
            memcpy(merged + 21, cached_trigger_l, 11);
    }
    if (force) s2t_force = true;      /* 配置改了：即使字节一样也重发一次（但保留 s2t_applied） */
    s2t_apply_to_payload(merged);
    out_trigger_dirty = false;        /* 这次就是 0x32，不用再补 */

    uint8_t buf[DS5_BT_OUTPUT_EXT_SIZE];
    build_bt_output_ext(merged, DS5_USB_OUTPUT_PAYLOAD_LEN, buf);
    bt_hid_host_send_output(buf, DS5_BT_OUTPUT_EXT_SIZE);
}

/* Called from l2cap_intr_connected (earliest possible moment, pre-HANDSHAKE).
 * Sends the 0x32 primer to the controller immediately when L2CAP opens. */
static void on_hid_primer(void)
{
    send_led_primer();
    LOG_INF("[MAIN] Primer sent from l2cap_intr_connected\n");
}

static void on_hid_input(const uint8_t *data, uint16_t len)
{
    if (len < 3) return;

    /* data[0] = report ID (0x31), data[1] = flags */
    if ((data[1] >> 1) & 1) {
        /* Bit1 set: mic Opus frame at data[3], length = len - 3 */
        if (len > 3)
            audio_mic_feed(data + 3, (uint16_t)(len - 3));
        return;
    }

    /* S6: 报告 ID 必须是 0x31（DS5 蓝牙输入报告）。
     *
     * 之前这里只查了"长度够"和"不是麦克风帧"，于是任何 >=78 字节的非输入报告
     * 都会被当作手柄输入塞进队列 —— 实测约 13% 的帧是这种"脏帧"：
     *     正常帧 7f 81 80 80 00 00 01 08
     *     脏 帧 6f c8 00 00 00 00 00 f0   （摇杆乱值、右摇杆清零）
     * 它们会污染摇杆/按键输出，也可能让主机侧看到跳变。
     *
     * 被拒的帧记进诊断环（arg1 = 实际报告 ID，arg2 = 标志字节），
     * 这样以后能从设备里读出"到底拒掉了什么"，而不是靠猜。 */
    if (data[0] != 0x31) {
        diag_log(DIAG_EV_INTR_RECV, data[0], 0x3100u | data[1]);
        return;
    }

    if (len >= DS5_BT_INPUT_REPORT_SIZE) {
        if (!first_input_logged) {
            first_input_logged = true;
            LOG_INF("[MAIN] First input report received (%d bytes)\n", len);
        }
        uint8_t report[DS5_BT_INPUT_REPORT_SIZE];
        memcpy(report, data, DS5_BT_INPUT_REPORT_SIZE);
        xQueueOverwrite(input_queue, report);
    }
}

static void on_hid_state(enum bt_hid_host_state state)
{
    switch (state) {
    case BT_HID_STATE_IDLE: {
        handshake_start_us = 0;
        connecting_start_tick = 0;
        stealth_primer_countdown = 0;
        primer_resend_us = 0;
        bool was_connected = ds5_connected;
        if (was_connected) {
            uint64_t since_out = bflb_mtimer_get_time_us() - out_isr_ts_us;
            game_was_active_at_disconnect = (out_isr_ts_us != 0) &&
                                            (since_out < 5000000ULL);
        }
        if (ds5_connected) {
            ds5_connected = false;
            battery_low = false;
            cached_battery_level = 0xFF;
            cached_battery_state = 0;
        }
        /* R2.4：回中帧和引擎复位都挪到 usb_task（见 disc_neutral_req 上方注释）。
         * 以前这里发的回中帧是整帧清零再摆正摇杆：两个触点的 bit7 = 0，电脑会以为两根手指
         * 按在触摸板 (0,0)，摇杆也摆在 0x7F 而不是 0x80。 */
        disc_neutral_req = true;
        ds5_connected = false;
        usb_fwd_count = 0;
        first_input_logged = false;
        usb_wake_on_bt_disconnect();
#if USB_DETACH_AFTER_DROP
        if (was_connected && !bt_hid_host_is_switching() &&
            !usb_wake_host_suspended()) {
            usb_deferred_disc_us = bflb_mtimer_get_time_us();
            LOG_INF("[MAIN] USB disconnect deferred (10s timer started)\n");
        }
#endif
        dse_reset();
        audio_reset();
        s2t_forget();     /* R2.5：手柄那边已经没有我们的卡点了（重连时 primer 会补） */
        /* S1 的 ds5map_on_bt_disconnect()（断开时清掉引擎的累积状态）R2.4 起在 usb_task 里调 */
        led_refresh_state();
        if (scan_after_disconnect) {
            scan_after_disconnect = false;
            LOG_INF("[MAIN] State: IDLE — button-triggered rescan\n");
            bt_hid_host_scan_start(0);
        } else {
            LOG_INF("[MAIN] State: IDLE\n");
        }
        break;
    }

    case BT_HID_STATE_SCANNING:
        led_refresh_state();
        LOG_INF("[MAIN] State: SCANNING\n");
        break;

    case BT_HID_STATE_CONNECTING:
    case BT_HID_STATE_SDP_QUERY:
        connecting_start_tick = xTaskGetTickCount();
        led_refresh_state();
        LOG_INF("[MAIN] State: CONNECTING (%d)\n", state);
        break;

    case BT_HID_STATE_L2CAP_CONTROL:
    case BT_HID_STATE_L2CAP_INTERRUPT:
        led_refresh_state();
        LOG_INF("[MAIN] State: CONNECTING (%d)\n", state);
        break;

    case BT_HID_STATE_HANDSHAKE: {
        connecting_start_tick = 0;
        /* primer_cb already fired from l2cap_intr_connected (earliest possible).
         * Re-send here in case the state machine order differs on some reconnects.
         * Also update audio active state. */
        send_led_primer();
        if (usb_audio_is_active())
            state_mgr_set_spk_active(true);
        handshake_start_us = bflb_mtimer_get_time_us();
        LOG_INF("[MAIN] State: HANDSHAKE (0x32 sent)\n");
        break;
    }

    case BT_HID_STATE_CONNECTED: {
        connecting_start_tick = 0;
        /* 接收机在 PC 上**永远**是普通 DS5（PID 054C:0CE6 + DS 报告描述符）。
         * "手柄模式"（DS5 / DSE / 自动识别）与随之而来的"型号变了就复位重新
         * 枚举"整套已按用户要求删除（2026-10-11，S15）：那个复位就是
         * 「手柄刚连上、接收机自己重启、随即掉线」的原因，DS5/DSE 都中招。
         * DSE 多出来的 4 个键在内部当映射/宏的输入源用，PC 侧不区别对待。 */

        handshake_start_us = 0;

        if (usb_detached) {
            /* USB 处于软摘除（隐身开机 / 掉线后摘除）→ 现在接回去。
             * 让 Windows 先发它的蓝色初始化，再用我们的 primer 盖掉。 */
            diag_log(DIAG_EV_USB_CONN, ever_connected ? 3 : 4, 0);   /* 3=回连后软接回 4=首连隐身接回 */
            usb_soft_connect();
            usb_detached = false;
            stealth_primer_countdown = 5;
            LOG_INF("[MAIN] USB soft connect (was detached, ever_connected=%d)\n",
                    (int)ever_connected);
        } else {
            /* USB 一直连着（S33 起掉线后不再摘除）：直接发 primer，游戏的会话还在。 */
            if (usb_deferred_disc_us) {
                usb_deferred_disc_us = 0;
                LOG_INF("[MAIN] Deferred disconnect cancelled\n");
            }
            send_led_primer();
        }

        ds5_connected = true;
        ever_connected = true;
        battery_low = false;
        primer_resend_us = bflb_mtimer_get_time_us();
        usb_wake_on_bt_connect();
        pad_sleep_reset();   /* R1.7：自动省电计时从每次连接重新起算 */
        pairing_mode = false;
        led_refresh_state();
        LOG_INF("[MAIN] State: CONNECTED\n");
        break;
    }

    default:
        LOG_INF("[MAIN] State: %d\n", state);
        break;
    }
}


/* ---- USB output callback (host → device) ----
 * Runs in USB interrupt context — only copy raw bytes, let bt_task
 * do the heavy lifting (CRC, BT report construction). */

static void on_usb_output(const uint8_t *data, uint16_t len)
{
    if (!ds5_connected || len < 2)
        return;

    if (data[0] != DS5_USB_REPORT_ID_OUTPUT)
        return;

    usb_out_msg_t msg;
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

/* ---- FreeRTOS tasks ---- */

static struct bflb_device_s *btn_gpio;

static void boot_button_init(void)
{
    btn_gpio = bflb_device_get_by_name("gpio");
    /* GPIO2 is the bootstrap pin — the board's external pull-down keeps
     * it LOW for normal boot; pressing the BOOT button pulls it HIGH.
     * Do NOT add an internal pull-up (conflicts with external circuit). */
    bflb_gpio_init(btn_gpio, BOOT_BUTTON_PIN,
                   GPIO_INPUT | GPIO_FLOAT | GPIO_SMT_EN);
    LOG_INF("[BTN] GPIO%d init, raw=%d\n", BOOT_BUTTON_PIN,
           (int)bflb_gpio_read(btn_gpio, BOOT_BUTTON_PIN));
}

static bool boot_button_pressed(void)
{
    /* Active HIGH: BOOT button connects GPIO2 to VCC (HIGH = pressed) */
    return bflb_gpio_read(btn_gpio, BOOT_BUTTON_PIN) != 0;
}

static void bt_task(void *arg)
{
    (void)arg;
    LOG_INF("[MAIN] Board: %s\n", BOARD_NAME);

    LOG_INF("[BT_TASK] btble_controller_init...\n");
    btble_controller_init(configMAX_PRIORITIES - 1);
    LOG_INF("[BT_TASK] hci_driver_init...\n");
    hci_driver_init();
    LOG_INF("[BT_TASK] bt_hid_host_init...\n");

    struct bt_hid_host_config cfg = {
        .input_cb   = on_hid_input,
        .state_cb   = on_hid_state,
        .primer_cb  = on_hid_primer,
        .target_vid = DS5_VID,
        .target_pid = DS5_PID,
    };

    int err = bt_hid_host_init(&cfg);
    if (err) {
        LOG_ERR("[MAIN] BT init failed: %d\n", err);
        vTaskDelete(NULL);
        return;
    }

    /* BT init complete — re-enable USB peripheral clock that BT may have disabled */
    GLB_Set_USB_CLK_From_WIFIPLL(1);
    bt_init_done = true;
    LOG_INF("[BT_TASK] BT init done, USB clock re-enabled, signaling usb_task\n");

    LOG_INF("[BT_TASK] led -> WAITING (1Hz blink)\n");
    led_refresh_state();
    LOG_INF("[BT_TASK] led OK\n");

    LOG_INF("[BT_TASK] try_reconnect...\n");
    if (bt_hid_host_try_reconnect() == 0) {
        /* Passive reconnect: page scan is enabled, wait for the
         * controller to page us and create L2CAP channels. */
        LOG_INF("[MAIN] Waiting for controller to reconnect...\n");
        TickType_t start = xTaskGetTickCount();
        while (!ds5_connected &&
               (xTaskGetTickCount() - start) < pdMS_TO_TICKS(RECONNECT_TIMEOUT_MS)) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (!ds5_connected) {
            LOG_WRN("[MAIN] Reconnect timed out, starting scan\n");
            bt_hid_host_scan_start(0);
        }
    } else {
        LOG_INF("[MAIN] No bonded device, starting scan\n");
        bt_hid_host_scan_start(0);
    }
    LOG_INF("[BT_TASK] Entering main loop\n");

    usb_out_msg_t usb_out;
    uint16_t idle_ticks = 0;
    uint16_t loop_count = 0;
    uint64_t disconnecting_since = 0;
    uint64_t stale_conn_since = 0;
    uint16_t scan_retry_ticks = 0;   /* counts idle ticks since last scan */
    uint8_t  scan_fail_count = 0;    /* consecutive scans with 0 results */
    uint16_t page_retry_ticks = 0;   /* S29: idle ticks since last active page */
    uint16_t hb_ticks = 0;           /* S31: idle ticks since last scan-enable check */
    uint64_t scanning_since = 0;     /* S31: when SCANNING was first observed (0 = not scanning) */
#define MAX_SCAN_RETRIES 2           /* stop active inquiry after N failures;
                                      * bonded controllers reconnect via page scan
                                      * (like original DS5DongleRE) */

    /*
     * Button gesture FSM (polled at 10 Hz, independent of loop speed).
     *   single click  → switch to next bonded controller
     *   double click  → disconnect + fresh scan (search new controller)
     *   hold >= 3s    → clear all bonds + blacklist + reset
     */
    enum { BTN_IDLE, BTN_PRESSING, BTN_HELD, BTN_WAIT } btn_fsm = BTN_IDLE;
    uint8_t  btn_press_samples = 0;
    uint8_t  btn_wait_samples  = 0;
    uint8_t  btn_click_count   = 0;
    uint64_t btn_last_poll_us  = 0;
    uint8_t  btn_debug_counter = 0;

    prev_led_disabled = config_led_disabled();

    for (;;) {
        audio_check_respawn();

        {   /* S37（照上游 3.20a/3.21 的 "scan OFF (ACL active)"）：连着的时候把 page/inquiry scan
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
            static uint64_t perf_last_us;
            uint64_t perf_now = bflb_mtimer_get_time_us();
            if (perf_now - perf_last_us >= 1000000ULL) {
                perf_last_us = perf_now;
                perf_cpu_sample();
            }
        }

#if DS5MAP_ENGINE_ENABLE >= 1
        /* S3: 引擎的落盘入口（cfg_save 的唯一调用者）。
         * **必须放在任务上下文**：cfg_save() 会写 LittleFS/flash，
         * 放在 USB 热路径上会导致状态机异常（曾出现"上电 2-3 秒蓝灯常亮"）。
         * ds5map_deferred() 内部先做无锁快判（g_pend_active），
         * 没有待落盘内容时只是读一个 volatile 布尔就返回。 */
        ds5map_deferred();
#endif

        loop_count++;
        if ((loop_count % 50) == 1) {
            LOG_DBG("[BT_TASK] loop #%u state=%d conn=%d\n",
                   loop_count, bt_hid_host_get_state(), ds5_connected);
        }

        /* ---- BOOT button gesture FSM (10 Hz cadence) ----
         * Placed BEFORE any continue statements so the button is
         * always responsive regardless of BT state machine activity. */
        uint64_t btn_now = bflb_mtimer_get_time_us();
        if (btn_now - btn_last_poll_us >= 100000ULL) {
            btn_last_poll_us = btn_now;
            bool pressed = boot_button_pressed();

            if (++btn_debug_counter >= 50) {
                btn_debug_counter = 0;
                LOG_DBG("[BTN] GPIO%d raw=%d fsm=%d\n",
                       BOOT_BUTTON_PIN,
                       (int)bflb_gpio_read(btn_gpio, BOOT_BUTTON_PIN),
                       btn_fsm);
            }

            switch (btn_fsm) {
            case BTN_IDLE:
                if (pressed) {
                    btn_fsm = BTN_PRESSING;
                    btn_press_samples = 1;
                    LOG_DBG("[BTN] IO2 pressed (raw=%d)\n",
                           (int)bflb_gpio_read(btn_gpio, BOOT_BUTTON_PIN));
                }
                break;

            case BTN_PRESSING:
                if (pressed) {
                    btn_press_samples++;
                    if (btn_press_samples >= BOOT_HOLD_TICKS) {
                        btn_click_count = 0;
                        btn_fsm = BTN_HELD;
                        LOG_INF("[BTN] IO2 held 3s — entering pairing mode\n");
                        /* Pairing mode: drop bonds + current link, then keep
                         * scanning until a controller pairs (or power-off).
                         * Deliberately does NOT reset on release — the old
                         * behaviour rebooted the DongleRE, which threw away the
                         * pairing state and looked like a random dropout. */
                        pairing_mode = true;
                        scan_fail_count = 0;
                        led_refresh_state();
                        bt_hid_host_clear_bonds();
                        bt_hid_host_scan_start(0);
                        idle_ticks = 0;
                    } else if (btn_press_samples == 3) {
                        LOG_DBG("[BTN] IO2 holding... (%d/%d)\n",
                               btn_press_samples, BOOT_HOLD_TICKS);
                    }
                } else {
                    btn_click_count++;
                    btn_fsm = BTN_WAIT;
                    btn_wait_samples = 0;
                    LOG_DBG("[BTN] IO2 released (click #%d)\n", btn_click_count);
                }
                break;

            case BTN_HELD:
                if (!pressed) {
                    btn_fsm = BTN_IDLE;
                    btn_press_samples = 0;
                    diag_log(DIAG_EV_BTN, 2, 0);   /* S32：长按 3 s = 清绑定 + 对频模式 */
                    LOG_INF("[BTN] IO2 released — pairing mode active\n");
                }
                break;

            case BTN_WAIT:
                if (pressed) {
                    btn_fsm = BTN_PRESSING;
                    btn_press_samples = 1;
                } else if (++btn_wait_samples >= BOOT_CLICK_WINDOW) {
                    uint8_t clicks = btn_click_count;
                    btn_click_count = 0;
                    btn_fsm = BTN_IDLE;
                    btn_press_samples = 0;

                    if (clicks == 1) {
                        diag_log(DIAG_EV_BTN, 1, 1);   /* S32：以前按键事件根本没人记，DIAG_EV_BTN 是死枚举 */
                        if (bt_hid_host_get_bonded_count() > 1) {
                            int next = bt_hid_host_switch_next();
                            LOG_INF("[BTN] Single click — switching to controller %d\n", next);
                        } else {
                            LOG_INF("[BTN] Single click — only 1 bonded, fresh scan\n");
                            scan_fail_count = 0;
                            if (ds5_connected) {
                                scan_after_disconnect = true;
                                bt_hid_host_disconnect_site(BT_DISC_SITE_MAIN_BTN_1CLICK);
                            } else {
                                bt_hid_host_scan_start(0);
                            }
                        }
                        idle_ticks = 0;
                    } else if (clicks >= 2) {
                        diag_log(DIAG_EV_BTN, 1, clicks);   /* S32 */
                        LOG_INF("[BTN] Double click — disconnect + fresh scan\n");
                        scan_fail_count = 0;
                        if (ds5_connected) {
                            scan_after_disconnect = true;
                            bt_hid_host_disconnect_site(BT_DISC_SITE_MAIN_BTN_2CLICK);
                        } else {
                            bt_hid_host_scan_start(0);
                        }
                        idle_ticks = 0;
                    }
                }
                break;
            }
        }

        bt_hid_host_persist_if_dirty();

        if (usb_wake_radio_wake_pending()) {
            bt_hid_host_radio_wake();
            LOG_INF("[MAIN] Radio wake — BT scan re-enabled after standby\n");
        }

        /* R1.3：链路诊断（RSSI / 链路质量 / AFH 在用信道数）每 ~2 s 读一次，进 PERF；
         * 原来只读 RSSI、按循环计数大约 12 s 一次。 */
        {
            static uint64_t link_stat_us;
            static bool     link_param_done;   /* R1.4：每次连上做一次角色 / 监督超时 */
            uint64_t ln = bflb_mtimer_get_time_us();
            if (!ds5_connected) {
                link_param_done = false;
            } else if (ln - link_stat_us >= 2000000ULL) {
                link_stat_us = ln;
                if (!link_param_done) {
                    link_param_done = true;
                    bt_hid_host_link_params();
                }
                bt_hid_host_read_link_stats();
            }
        }

        /* Fallback: if DISCONNECTING for >1.5 seconds, the remote is dead.
         * Force cleanup to return to IDLE. */
        if (bt_hid_host_get_state() == BT_HID_STATE_DISCONNECTING) {
            if (disconnecting_since == 0)
                disconnecting_since = bflb_mtimer_get_time_us();
            else if ((bflb_mtimer_get_time_us() - disconnecting_since)
                     > 1500000ULL) {
                LOG_WRN("[BT_TASK] DISCONNECTING stuck >1.5s, forcing cleanup\n");
                bt_hid_host_force_disconnect();
                disconnecting_since = 0;
            }
        } else {
            disconnecting_since = 0;
        }

        /* Deferred USB soft-disconnect: if BT disconnected and no reconnection
         * within the timeout, pull USB so Windows removes the phantom device. */
#if USB_DETACH_AFTER_DROP
        if (usb_deferred_disc_us && !ds5_connected &&
            (bflb_mtimer_get_time_us() - usb_deferred_disc_us) > USB_DEFERRED_DISC_US) {
            usb_deferred_disc_us = 0;
            diag_log(DIAG_EV_USB_DISC, 1, 0);   /* 1=10s 未重连，软摘除 */
            usb_soft_disconnect();
            usb_detached = true;
            LOG_INF("[MAIN] Deferred USB disconnect (no reconnect within 10s)\n");
        }
#endif

        if (bt_hid_host_poll_scan_early()) {
            idle_ticks = 0;
            continue;
        }

        if (bt_hid_host_poll_connect()) {
            idle_ticks = 0;
            continue;
        }

        if (bt_hid_host_poll_security()) {
            idle_ticks = 0;
            continue;
        }

        if (bt_hid_host_poll_sdp()) {
            idle_ticks = 0;
            continue;
        }

        bt_hid_host_poll_security_watchdog();
        bt_hid_host_poll_incoming_l2cap_fallback();

        /* React to disable_led config toggle in real-time.
         * R1.14：开 = 立刻全灭（对频快闪除外），关 = 恢复状态灯；电量告警只在没禁用时闪。 */
        bool cur_led_disabled = config_led_disabled();
        if (cur_led_disabled != prev_led_disabled) {
            prev_led_disabled = cur_led_disabled;
            if (!cur_led_disabled && ds5_connected && battery_low)
                led_status_set(LED_BLINK_BATTERY);
            else
                led_refresh_state();
            LOG_DBG("[LED-DBG] disable_led=%d conn=%d\n", cur_led_disabled, ds5_connected);
        }

        if (ds5_connected) {
            {   /* R1.7：N 分钟没用户操作 → 发 DualSense 关机命令（手柄立即静音关机，同 PS5）。
                 * 只发命令、**不动射频**（绝不能学挂起路径调 bt_hid_host_radio_idle()）：
                 * 手柄关机后 ACL 自然掉，conn_disconnected 重开 page scan，按 PS 秒回连。 */
                uint8_t mins = config_get()->pad_sleep_min;
                if (mins != 0 && !usb_wake_host_suspended()) {
                    uint32_t now_ms = (uint32_t)(bflb_mtimer_get_time_us() / 1000ULL);
                    /* R1.12：last_activity_ms 出现"未来值"绝不合法——时钟单调，三个写入点
                     * 写的都是当时的 now，只可能 ≤ now。2026-10-15 实机：用户设 30 分钟，
                     * 连上 2~8 秒就被踢（AUTO_OFF a1=30），当时开机才 10.5 分钟，数学上
                     * 唯一解释是这个变量被野写成了大数/未来值，无符号减法回绕出 ≥30 分钟
                     * 的假闲置（R1.11 前保存不生效，此路径从未真正跑过，所以刚暴露）。
                     * 拉回现在，本轮不判——宁可晚点关，绝不能误踢。
                     * R1.13：领先 1~2ms 的不是野写，是竞态——bt_task 采完 now 到比较之间，
                     * usb_task 的帧活动写入恰好落在下一毫秒（450~700 帧/s 下必然偶发），
                     * 同一轮就会被拉回，毫无危害；只有领先量大到不可能是竞态（>2s）才像
                     * 野写，那种才记 ACT_FIX 留物证，免得日志页平白亮红吓人。
                     * R1.15：R1.12 的守卫其实没真正拦住——编出来的代码把 last_activity_ms 重读了
                     * 三次（守卫读第 1 次、判关机重读第 3 次），两读之间野写就绕过守卫、直接误发关机。
                     * 2026-10-15 R1.14 实测 14:33:54(t=61s)、14:46:50(t=837s) 各误关机一次、前面都
                     * 没有 ACT_FIX = 守卫整轮没跑。改成下面的一次性快照 act_ms 彻底消除：开机不足 N
                     * 分钟绝不可能被判成闲置 N 分钟。野写来源仍未定位（聚在网页工具连着时）。 */
                    uint32_t act_ms = last_activity_ms;   /* R1.15：一次性快照，守卫和判关机共用，别让编译器把 volatile 重读成两三次 */
                    if (act_ms > now_ms) {
                        if ((uint32_t)(act_ms - now_ms) > 2000U) {
                            diag_log(DIAG_EV_ACT_FIX, act_ms, 0);
                            LOG_WRN("[MAIN] Auto sleep: activity ts far in future (0x%08lx > %lu), healed\n",
                                    (unsigned long)act_ms, (unsigned long)now_ms);
                        }
                        last_activity_ms = now_ms;
                        act_ms = now_ms;
                    }
                    if ((uint32_t)(now_ms - act_ms) >= (uint32_t)mins * 60000U) {
                        last_activity_ms = now_ms;  /* 命令若没送到，N 分钟后自然重试 */
                        diag_log(DIAG_EV_AUTO_OFF, mins, 0);
                        LOG_INF("[MAIN] Auto sleep: %u min idle -> controller power off\n", mins);
                        usb_wake_power_off_controller();
                    }
                }
            }

            /* Push UAC volume/mute changes to controller immediately */
            if (state_mgr_vol_dirty()) {
                state_mgr_vol_ack();
                uint8_t merged[DS5_USB_OUTPUT_PAYLOAD_LEN];
                state_mgr_get(merged, DS5_USB_OUTPUT_PAYLOAD_LEN);
                uint8_t bt_out[DS5_BT_OUTPUT_REPORT_SIZE];
                build_bt_output(merged, DS5_USB_OUTPUT_PAYLOAD_LEN,
                                output_seq, bt_out);
                output_seq = (output_seq + 1) & 0x0F;
                bt_hid_host_send_output(bt_out, DS5_BT_OUTPUT_REPORT_SIZE);
                state_mgr_clear_flags();
            }

            /* Pass-through output forwarding (matches DS5DongleRE):
             * Each USB output frame is independently forwarded to BT with
             * only config overlays applied — no flag accumulation or merging.
             * R1.6：手柄喇叭有声音时也收到就转发。原作者这时改成「每 24 ms 攒一批再发」，
             * 普通震动 / 灯 / 扳机因此平均晚 12 ms、最多 24 ms。发出去的包数一样（每份输出报告
             * 一个 0x31），只是不再扣着；蓝牙侧照旧一次只在飞一个（app_tx FIFO）。 */
            /* R2.9：**配置改了就立刻重发一次手柄状态**（用户报"改参数要断电上电才生效"）。
             * 我们的效果原本只在 ①有 PC 输出报文经过 或 ②连上时的 primer 才注入；在桌面上调参数
             * 时根本没有输出报文 → 注入没跑过 → 只有断电上电（走 primer）才生效。这里在 bt 循环里
             * 兜住：网页一 COMMIT 扳机区就置标志，循环每 ≤24 ms 看一次，立刻发一份 0x32。
             * 另外每 1 秒兜底重发一次（覆盖"手柄重连/效果被游戏顶掉/自动扳机衰减"这些情况）。 */
            if (ds5map_s2t_take_resend())
                pad_resend_state(true);
            /* R3.25b：**切层**请求（配置没变，只是"此刻用哪一套"变了）——不能 forget：
             * 新层可能"没有效果"，那要走撤下（发 OFF）分支，而撤下依赖 s2t_applied。
             * 用户实测：主层开 L2 阻力、覆盖层 1 关掉，切过去手上还有阻力。 */
            if (ds5map_s2t_take_resend_nf())
                pad_resend_state(false);
            /* R3.13：**撤下效果也要能送出去** —— out_trigger_dirty 以前只在「有主机输出帧」的
             * 分支里被消费 ✗，桌面上主机不发帧时撤下的 0x32 永远发不出去，手柄就一直粘着上一个
             * 效果（用户实测：选了关闭，手上还有感觉）。这里在没有主机帧时自己重发一次；
             * **不能 forget**（忘了就跳过「撤下」分支，OFF 反而发不出去 ✗）。 */
            if (out_trigger_dirty)
                pad_resend_state(false);
            {
                static uint32_t last_ensure_ms;
                uint32_t tnow = (uint32_t)(bflb_mtimer_get_time_us() / 1000ULL);
                if (ds5map_s2t_any_effect() && (uint32_t)(tnow - last_ensure_ms) >= 1000U) {
                    last_ensure_ms = tnow;
                    pad_resend_state(false);
                }
            }

            if (xQueueReceive(output_queue, &usb_out, pdMS_TO_TICKS(24)) == pdTRUE) {
                uint8_t *payload = usb_out.data + 1;

                /* Stealth mode: drain Windows' blue init frames without forwarding
                 * to BT, then fire primer so controller receives purple BEFORE
                 * any Windows output (same ordering as normal mode). */
                if (stealth_primer_countdown > 0) {
                    cache_output_state(payload);
                    stealth_primer_countdown--;
                    if (stealth_primer_countdown == 0) {
                        LOG_INF("[MAIN] Stealth: primer fired, forwarding starts\n");
                        send_led_primer();
                        primer_resend_us = bflb_mtimer_get_time_us();
                    } else {
                        LOG_INF("[MAIN] Stealth: holding BT forward, countdown=%d\n",
                                (int)stealth_primer_countdown);
                    }
                    goto next_output_iter;
                }

                cache_output_state(payload);
                s2t_apply_to_payload(payload);   /* R2.5：两段式扳机的卡点（装了就不重复动） */

                uint8_t bt_out[DS5_BT_OUTPUT_REPORT_SIZE];
                build_bt_output(payload, DS5_USB_OUTPUT_PAYLOAD_LEN,
                                output_seq, bt_out);                output_seq = (output_seq + 1) & 0x0F;
                bt_hid_host_send_output(bt_out, DS5_BT_OUTPUT_REPORT_SIZE);
                perf_out_fwd((uint32_t)bflb_mtimer_get_time_us() - usb_out.t_us);   /* R1.6 */

                /* R2.6b：扳机块变了就再补一个 0x32 扩展报文 —— 0x31 里手柄**不认**自适应扳机
                 * （2026-10-17 用户实测没手感），primer 那条 0x32 才是有效的。只在"变了"的帧补。 */
                if (out_trigger_dirty) {
                    out_trigger_dirty = false;
                    uint8_t bt_ext[DS5_BT_OUTPUT_EXT_SIZE];
                    build_bt_output_ext(payload, DS5_USB_OUTPUT_PAYLOAD_LEN, bt_ext);
                    bt_hid_host_send_output(bt_ext, DS5_BT_OUTPUT_EXT_SIZE);
                }
next_output_iter:;
            }

            idle_ticks = 0;
        } else {
            vTaskDelay(pdMS_TO_TICKS(100));
            idle_ticks++;

            /* CONNECTING timeout: L2CAP channels didn't complete in time */
            {
                static TickType_t l2cap_timeout_start = 0;
                enum bt_hid_host_state s = bt_hid_host_get_state();
                if ((s == BT_HID_STATE_CONNECTING ||
                     s == BT_HID_STATE_SDP_QUERY) &&
                    connecting_start_tick > 0) {
                    TickType_t elapsed = xTaskGetTickCount() - connecting_start_tick;
                    if (elapsed > CONNECTING_TIMEOUT_TICKS) {
                        LOG_WRN("[MAIN] Connecting timeout (%d) — disconnecting\n", s);
                        connecting_start_tick = 0;
                        l2cap_timeout_start = 0;
                        bt_hid_host_disconnect_site(BT_DISC_SITE_MAIN_CONN_TIMEOUT);
                    }
                } else if (s == BT_HID_STATE_L2CAP_CONTROL ||
                           s == BT_HID_STATE_L2CAP_INTERRUPT) {
                    if (l2cap_timeout_start == 0)
                        l2cap_timeout_start = xTaskGetTickCount();
                    TickType_t elapsed = xTaskGetTickCount() - l2cap_timeout_start;
                    if (elapsed > L2CAP_FALLBACK_TIMEOUT_TICKS) {
                        LOG_WRN("[MAIN] Connecting timeout (%d) — disconnecting\n", s);
                        connecting_start_tick = 0;
                        l2cap_timeout_start = 0;
                        bt_hid_host_disconnect_site(BT_DISC_SITE_MAIN_L2CAP_TIMEOUT);
                    }
                } else {
                    l2cap_timeout_start = 0;
                }
            }

            if (bt_hid_host_get_state() == BT_HID_STATE_HANDSHAKE) {
                bt_hid_host_handshake_tick();
                if (handshake_start_us > 0 &&
                    (bflb_mtimer_get_time_us() - handshake_start_us) >
                    HANDSHAKE_TIMEOUT_US) {
                    LOG_WRN("[MAIN] Handshake timeout — disconnecting\n");
                    handshake_start_us = 0;
                    bt_hid_host_disconnect_site(BT_DISC_SITE_MAIN_HS_TIMEOUT);
                }
            }

            {
                enum bt_hid_host_state s = bt_hid_host_get_state();
                if (s != BT_HID_STATE_IDLE) {
                    stale_conn_since = 0;
                    scan_retry_ticks = 0;
                    page_retry_ticks = 0;
                    if (s != BT_HID_STATE_SCANNING)
                        scan_fail_count = 0;
                }
                if (s == BT_HID_STATE_SCANNING) {
                    /* S31（照 3.21 的「SCANNING stuck 30s」）：inquiry 完成回调若丢了，
                     * 状态机会永远停在 SCANNING，再也回不到等待连接。30 s 兜底强制结束，
                     * 之后原有的重试/主动 page 逻辑照常接管。 */
                    uint64_t t_now = bflb_mtimer_get_time_us();
                    if (scanning_since == 0) {
                        scanning_since = t_now;
                    } else if (t_now - scanning_since > SCAN_STUCK_US) {
                        puts("[MAIN] SCANNING stuck 30s, forcing stop");
                        diag_log(DIAG_EV_SCAN_STUCK,
                                 (uint32_t)((t_now - scanning_since) / 1000ULL), 0);
                        bt_hid_host_scan_stop();
                        scanning_since = 0;
                    }
                } else {
                    scanning_since = 0;
                }
            }

            if (bt_hid_host_get_state() == BT_HID_STATE_IDLE &&
                idle_ticks >= 20 && !usb_wake_host_suspended()) {
                if (bt_hid_host_has_pending_conn()) {
                    if (stale_conn_since == 0)
                        stale_conn_since = bflb_mtimer_get_time_us();
                    else if ((bflb_mtimer_get_time_us() - stale_conn_since)
                             > 15ULL * 1000000ULL) {
                        LOG_WRN("[MAIN] conn_disconnected never fired, "
                               "dropping stale conn\n");
                        bt_hid_host_drop_stale_conn();
                        stale_conn_since = 0;
                    }
                    scan_retry_ticks = 0;
                    page_retry_ticks = 0;
                } else {
                    stale_conn_since = 0;
                    scan_retry_ticks += idle_ticks;
                    uint16_t retry_interval = ever_connected ? 600 : 300;
                    if (scan_retry_ticks >= retry_interval) {
                        scan_retry_ticks = 0;
                        if (scan_fail_count < MAX_SCAN_RETRIES) {
                            scan_fail_count++;
                            bt_hid_host_scan_start(0);
                        }
                    }
                    /* S29: actively page the bonded controller every ~8s.
                     * Skipped in pairing mode (bonds were just cleared and we
                     * are scanning for a NEW controller — paging the old MAC
                     * would fight the inquiry). poll_connect suppresses the
                     * inquiry retry while a page is in flight, and HCI can't
                     * do both anyway. */
#if ACTIVE_PAGE_RECONNECT
                    page_retry_ticks += idle_ticks;
                    if (page_retry_ticks >= PAGE_RETRY_TICKS) {
                        page_retry_ticks = 0;
                        if (!pairing_mode)
                            bt_hid_host_page_bonded();
                    }
#else
                    /* S34：只做 page scan 被动等手柄来连（原作者 3.18 的做法）；
                     * S31 的心跳仍每 ~10 s 确认 page scan 没被关掉。 */
                    (void)page_retry_ticks;
#endif
                    /* S31（照 3.21 的 [BT-HB]）：等待连接期间每 ~10 s 读一次 HCI Scan_Enable，
                     * page scan（bit1）若被悄悄关掉就立刻补开，并记进 DIAG 让主机看得见。 */
                    hb_ticks += idle_ticks;
                    if (hb_ticks >= HB_TICKS) {
                        hb_ticks = 0;
                        int se = bt_hid_host_read_scan_enable();
                        if (se >= 0 && (se & 0x02) == 0) {
                            diag_log(DIAG_EV_HB_SCAN_FIX, (uint32_t)se, 0);
                            bt_hid_host_radio_wake();
                        }
                    }
                }
                idle_ticks = 0;
            }
        }
    }
}

/* 接收机在 PC 上**永远以普通 DS5 的身份出现**（2026-10-11，用户决定）。
 *
 * 理由：DSE 多出来的那几个键（左Fn/右Fn/左背键/右背键）在接收机内部就被当作
 * 映射/宏的**输入源**用掉了，PC 侧本来也不需要认识它们。既然两种手柄在 PC 上
 * 的体验完全一样，就没必要按手柄类型切换 USB 描述符 —— 那样连 DSE 时还会
 * 触发一次 USB 重新枚举（主机侧表现为断开重连），反而添乱。
 *
 * S15 起这套"手柄模式"选择整个删掉了：不再有 should_use_dse()、没有 DSE 报告
 * 描述符、没有"型号变了就复位"，USB 描述符（PID 054C:0CE6 + DS 描述符）是唯一
 * 一份、开机就是它。 */

/* R1.4：输入停顿时交给电脑的「全部松开」帧。p 是 63 字节 USB 负载（布局同 Linux
 * hid-playstation.c 的 dualsense_input_report）：摇杆回中、扳机 0、按键全松（十字键 8 =
 * 不按）、陀螺仪角速度清零、两个触点抬起（bit7 = 没摸）。加速度、电量、时间戳保留上一帧的
 * —— 整帧清零会让电脑看到电量 0、重力消失。 */
static void ds5_input_neutralize(uint8_t *p)
{
    p[0] = p[1] = p[2] = p[3] = 0x80;
    p[4] = p[5] = 0;
    p[7] = DS5_DPAD_NONE;
    p[8] = 0;
    p[9] = 0;
    memset(&p[15], 0, 6);
    p[32] |= 0x80;
    p[36] |= 0x80;
}

/* R1.7：每帧喂一次，判断「用户有没有在碰手柄」，给自动省电关机计时。
 * p = raw_report + 2（63 字节 USB 负载，布局同 ds5_input_neutralize）。
 * R1.10：不判体感——马达震动会把 IMU 摇得比手晃还凶（见 PAD_TRIG_ACTIVE 上方注释）。 */
static void pad_activity_check(const uint8_t *p)
{
    bool active = false;

    /* 按键/十字键（含 PS）：任何变化 */
    if (p[7] != act_prev_btn[0] || p[8] != act_prev_btn[1] || p[9] != act_prev_btn[2])
        active = true;
    act_prev_btn[0] = p[7]; act_prev_btn[1] = p[8]; act_prev_btn[2] = p[9];

    /* 摇杆：离中即活动 —— 持续偏转持续算，漂移（<阈值）不算 */
    for (int i = 0; i < 4; i++) {
        int d = (int)p[i] - 0x80;
        if (d > PAD_STICK_ACTIVE || d < -PAD_STICK_ACTIVE) { active = true; break; }
    }

    /* 扳机：按过阈值即活动（持续压住也算，赛车游戏的油门） */
    if (p[4] > PAD_TRIG_ACTIVE || p[5] > PAD_TRIG_ACTIVE)
        active = true;

    /* 触摸：接触状态变化，或按着时坐标动（抬起后坐标无意义，只比首字节） */
    for (int f = 0; f < 2; f++) {
        const uint8_t *cur = &p[32 + f*4];
        uint8_t *old = &act_prev_touch[f*4];
        int n = (cur[0] & 0x80) ? 1 : 4;
        for (int i = 0; i < n; i++)
            if (cur[i] != old[i]) { active = true; break; }
        memcpy(old, cur, 4);
    }

    if (active)
        last_activity_ms = (uint32_t)(bflb_mtimer_get_time_us() / 1000ULL);
}

static void usb_task(void *arg)
{
    (void)arg;

    /* Wait for BT init to complete — it reconfigures clocks and kills USB */
    LOG_INF("[USB_TASK] Waiting for BT init to complete...\n");
    while (!bt_init_done) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    LOG_INF("[USB_TASK] BT init done, starting USB init\n");

    usb_gamepad_set_polling_rate(config_polling_mode());
    usb_gamepad_init(on_usb_output);
    usb_gamepad_set_suspend_hooks(usb_wake_on_suspend,
                                  usb_wake_on_resume,
                                  usb_wake_on_configured);

    if (config_usb_stealth()) {
        vTaskDelay(pdMS_TO_TICKS(150));
        diag_log(DIAG_EV_USB_DISC, 2, 0);   /* 2=开机隐身软摘除 */
        usb_soft_disconnect();
        usb_detached = true;
    }

    uint8_t raw_report[DS5_BT_INPUT_REPORT_SIZE];
    uint64_t last_input_us    = bflb_mtimer_get_time_us();
    uint32_t usb_check_cnt = 0;

    /* R1.1：按 USB 回报率处理。手柄每秒发 ~700 帧，回报率固定 500 Hz：映射引擎和
     * USB 转发只对"最新的一帧"做，两次之间至少隔 PROC_INTERVAL_US。中间来的帧只更新
     * pend_raw，以前它们每帧都过一遍引擎、结果又被下一帧覆盖，约三成算力白花。
     * 门槛取 1.8 ms 而不是 2 ms：任务按 1 ms 节拍醒来，留一点余量才不会掉到 333 Hz。 */
#define PROC_INTERVAL_US  1800ULL
    uint8_t  pend_raw[DS5_BT_INPUT_REPORT_SIZE];
    bool     pend_have = false;
    bool     pend_valid = false;    /* R2.4：pend_raw 里是不是收到过的真帧（开机后第一帧之前不是） */
    uint64_t last_proc_us = 0;

/* R1.4：3 s → 10 s。2026-10-14 17:06 实抓：手柄停发约 4.45 s 后自己恢复了（我们 3 s 强拆后
 * 1.4 s 又收到它的输入报告），是这个看门狗把能自愈的链路砍掉了。停顿期间先给电脑发一帧
 * 「全部松开」（STALL_NEUTRAL_US），真死链最多 10 s 后照旧强拆、重开 page scan。 */
#define CONN_WATCHDOG_US (10ULL * 1000000ULL)
#define STALL_NEUTRAL_US (500000ULL)

    /* S33：原作者的三套按键拦截（PS 短按→Win+G、Options+十字调音量、Create+十字切映射档）
     * 已整体删除：它们在 ds5map 引擎之前吃掉按键，又和引擎抢同一个 USB 键盘端点。
     * 现在手柄报文只经过 ds5map 引擎一处处理。 */

    for (;;) {
        /* Periodic USB status check (every ~10 seconds) */
        if (++usb_check_cnt >= 10000) {
            usb_check_cnt = 0;
            LOG_DBG("[USB-MON] UNPLUG=%d GLINT=%d FORCE_FS=%d "
                   "DEV_CTL=0x%08lx GLB_INT=0x%08lx\n",
                   (int)(*(volatile uint32_t *)(USB_BASE + 0x114) & 1),
                   (int)((*(volatile uint32_t *)(USB_BASE + 0x100) >> 2) & 1),
                   (int)((*(volatile uint32_t *)(USB_BASE + 0x100) >> 9) & 1),
                   (unsigned long)*(volatile uint32_t *)(USB_BASE + 0x100),
                   (unsigned long)*(volatile uint32_t *)(USB_BASE + 0x0C4));
        }
        usb_gamepad_process_deferred();

        if (primer_resend_us && ds5_connected &&
            stealth_primer_countdown == 0 &&
            (bflb_mtimer_get_time_us() - primer_resend_us) > PRIMER_RESEND_DELAY_US) {
            primer_resend_us = 0;
            send_led_primer();
        }

        /* Connection watchdog: if connected and we've received at least one
         * report, but then nothing for CONN_WATCHDOG_US (R1.4: 10 s) → link is dead.
         * Use force_disconnect to bypass HCI ACK (dead controller can't ACK). */
        /* R1.3：输入停顿 ≥1 s 记一条（带当时的链路诊断），恢复时再记停了多久；
         * 停顿最后变成断开（看门狗 / 断链，usb_fwd_count 被清零）就不记恢复。 */
        {
            static bool     stall_logged;
            static uint32_t stall_gap_ms;
            uint64_t gap = bflb_mtimer_get_time_us() - last_input_us;
            bool live = ds5_connected && usb_fwd_count > 0;
            if (!stall_logged) {
                if (live && gap > 1000000ULL) {
                    stall_logged = true;
                    stall_gap_ms = (uint32_t)(gap / 1000ULL);
                    diag_log(DIAG_EV_STALL, bt_hid_host_link_stats_packed(), 0);
                }
            } else if (!live) {
                stall_logged = false;
            } else if (gap < 100000ULL) {
                stall_logged = false;
                diag_log(DIAG_EV_STALL_END, stall_gap_ms, 0);
            } else if (gap / 1000ULL > stall_gap_ms) {
                stall_gap_ms = (uint32_t)(gap / 1000ULL);
            }
        }

        /* R1.4：停顿超过 STALL_NEUTRAL_US 就把最后一帧改成「全部松开」，过一遍引擎再交给 USB。
         * 不这么做的话 USB 端点一直重发停顿前那一帧：按着的键和扳机一直按着、陀螺仪转视角
         * 一直在转、映射出去的键盘键也一直按着。手柄回来后下一帧照常覆盖；每次停顿只发一次。 */
        {
            static bool neutral_sent;
            uint64_t nn = bflb_mtimer_get_time_us();
            if (nn - last_input_us < STALL_NEUTRAL_US) {
                neutral_sent = false;
            } else if (!neutral_sent && ds5_connected && usb_fwd_count > 0) {
                neutral_sent = true;
                pend_have = false;
                ds5_input_neutralize(pend_raw + 2);
#if DS5MAP_ENGINE_ENABLE >= 2
                ds5map_process(pend_raw + 2, (uint32_t)(nn / 1000ULL));
#endif
                usb_gamepad_send_raw_input(pend_raw + 2);
            }
        }

        if (ds5_connected && usb_fwd_count > 0 &&
            (bflb_mtimer_get_time_us() - last_input_us) > CONN_WATCHDOG_US) {
            LOG_WRN("[MAIN] Watchdog: no input for %us, force disconnect\n",
                    (unsigned)(CONN_WATCHDOG_US / 1000000ULL));
            diag_log(DIAG_EV_WD_CONN, usb_fwd_count, 0);
            bt_hid_host_force_disconnect();
            last_input_us = bflb_mtimer_get_time_us();
            usb_fwd_count = 0;
        }

        TickType_t wait = pend_have ? 1 : pdMS_TO_TICKS(10);
        if (xQueueReceive(input_queue, raw_report, wait) == pdTRUE) {
            uint32_t rx_c0 = perf_cyc();
            last_input_us = bflb_mtimer_get_time_us();
            usb_fwd_count++;
            if (usb_fwd_count == 1)
                LOG_DBG("[USB-FWD] First BT→USB input forwarded\n");
            else if ((usb_fwd_count % 500) == 0)
                LOG_DBG("[USB-FWD] %lu reports forwarded\n",
                       (unsigned long)usb_fwd_count);

            /* R1.1：映射引擎和 USB 转发挪到下面按回报率做；这里只留每帧都必须看的事 */
            memcpy(pend_raw, raw_report, sizeof(pend_raw));
            pend_have = true;
            pend_valid = true;

            /* 主机挂起时也要能看到按键（PS 唤醒），用原始报文、每帧都看 */
            usb_wake_on_bt_input(raw_report + 2, DS5_USB_INPUT_PAYLOAD_LEN);
            pad_activity_check(raw_report + 2);   /* R1.7：自动省电关机的活动判定 */

            /* Headset plug detection for audio tag switching (0x93 vs 0x96) */
            audio_set_headset((raw_report[2 + DS5_HEADSET_BYTE] & 1) != 0);

            /* Mic mute button → toggle MuteLight on controller.
             * Controller toggles status_bit only when a game has activated
             * the mic — this is normal PS5 behavior. */
            {
                static uint8_t prev_mic_bit = 0xFF;
                uint8_t cur_mic_bit = (raw_report[2 + DS5_HEADSET_BYTE] >> 2) & 1;
                if (prev_mic_bit != 0xFF && cur_mic_bit != prev_mic_bit) {
                    uint8_t mute_state[DS5_USB_OUTPUT_PAYLOAD_LEN];
                    memset(mute_state, 0, sizeof(mute_state));
                    mute_state[1] = 0x01;  /* flags1: AllowMuteLight */
                    mute_state[8] = cur_mic_bit ? 0x01 : 0x00;
                    uint8_t pkt[DS5_BT_OUTPUT_EXT_SIZE];
                    build_bt_output_ext(mute_state, DS5_USB_OUTPUT_PAYLOAD_LEN, pkt);
                    bt_hid_host_send_output(pkt, DS5_BT_OUTPUT_EXT_SIZE);
                }
                prev_mic_bit = cur_mic_bit;
            }

            if (ds5_connected) {
                /* Battery monitoring: <=10% red blink */
                uint8_t batt = raw_report[2 + DS5_BATT_BYTE_OFFSET];
                uint8_t pct  = batt & DS5_BATT_LEVEL_MASK;
                uint8_t st   = (batt >> DS5_BATT_STATE_SHIFT) & 0x0F;
                cached_battery_level = (pct > 10) ? 100 : pct * 10;
                cached_battery_state = st;
                bool discharging = (st == DS5_BATT_STATE_DISCHARGE);
                bool is_low  = discharging && (pct <= DS5_BATT_LOW_THRESHOLD);

                if (is_low && !battery_low) {
                    battery_low = true;
                    if (!config_led_disabled())   /* R1.14：禁用 LED 时电量告警也不闪 */
                        led_status_set(LED_BLINK_BATTERY);
                    LOG_WRN("[MAIN] Battery critical (%d%%)\n", pct * 10);
                } else if (!is_low && battery_low) {
                    battery_low = false;
                    led_refresh_state();
                }

            }
            perf_frame_rx(perf_cyc() - rx_c0);
        }

        /* R2.4：断开后的「全部松开」（标志由 on_hid_state 置）。放在下面的 500 Hz 处理之前：
         * 先扔掉断开前还没处理的最后一帧（pend_raw / 队列里那帧），再把最后一帧的拷贝改成
         * 回中帧（ds5_input_neutralize：摇杆 0x80、扳机 0、按键全松、陀螺仪清零、触点抬起），
         * 直接交给 USB —— 不过引擎：手柄已经没了，引擎的结果没有意义，「常开」陀螺仪还会叠出
         * 零偏。手柄端点会一直重发最后挂上的这一帧，所以电脑看到的最终状态一定是回中。
         * 最后复位引擎：键盘、媒体、鼠标的「全部松开」由它进队发送，保证送到。
         * 标志刚好在上面收帧之后才置上的话，这一轮下面会照常发出一帧旧数据，下一轮这里就盖掉。 */
        if (disc_neutral_req) {
            disc_neutral_req = false;
            pend_have = false;
            xQueueReset(input_queue);
            if (pend_valid) {
                ds5_input_neutralize(pend_raw + 2);
                usb_gamepad_send_raw_input(pend_raw + 2);
            }
#if DS5MAP_ENGINE_ENABLE >= 1
            ds5map_on_bt_disconnect();
#endif
        }

        /* R1.1：按回报率（固定 500 Hz）对最新一帧做映射 + 交给 USB。
         * 不能改成"端点空了才处理"：try_send_pending 在端点完成中断里立刻用上一帧
         * 重新挂上，端点几乎永远是忙的。 */
        if (pend_have) {
            uint64_t pn = bflb_mtimer_get_time_us();
            if (pn - last_proc_us >= PROC_INTERVAL_US) {
                pend_have = false;
                last_proc_us = pn;
                uint32_t c0 = perf_cyc();
#if DS5MAP_ENGINE_ENABLE >= 2
                ds5map_process(pend_raw + 2, (uint32_t)(pn / 1000ULL));
#endif
                uint32_t c1 = perf_cyc();
                usb_gamepad_send_raw_input(pend_raw + 2);
                perf_frame(c1 - c0, perf_cyc() - c1);
            }
        }

        usb_wake_task();
        dse_task();
    }
}

static void led_task(void *arg)
{
    (void)arg;
    for (;;) {
        led_status_tick();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

uint8_t get_battery_level(void)  { return cached_battery_level; }
uint8_t get_battery_state(void)  { return cached_battery_state; }

int main(void)
{
    board_init();

    printf("\n=== DS5DongleRE BL618 ===\n");
    LOG_INF("DualSense Wireless Bridge\n");

    /* Flash speed diagnostic */
    {
        volatile uint32_t *glb_sf_cfg0 = (volatile uint32_t *)0x20000170;
        uint32_t sf_val  = *glb_sf_cfg0;
        uint8_t clk_sel  = (sf_val >> 12) & 0x3;
        uint8_t clk_sel2 = (sf_val >> 14) & 0x3;
        uint8_t clk_div  = (sf_val >> 8) & 0x7;
        unsigned mhz = 0;
        const char *src = "?";
        if (clk_sel == 0) {
            if (clk_sel2 == 1)      { src = "XTAL";   mhz = 40; }
            else if (clk_sel2 == 0) { src = "PLL120"; mhz = 120; }
            else if (clk_sel2 == 2) { src = "PLL96";  mhz = 96; }
        } else if (clk_sel == 1) { src = "PLL80";  mhz = 80; }
        else if (clk_sel == 2)   { src = "BCLK";   mhz = 0; }
        else if (clk_sel == 3)   { src = "PLL160"; mhz = 160; }
        if (clk_div > 0 && mhz > 0) mhz /= (clk_div + 1);
        LOG_INF("[B] Flash: %s/%u = %uMHz\n", src, clk_div, mhz);
    }

    /* Configure Exchange Memory for BT controller (required for BR/EDR) */
    {
        extern uint8_t __LD_CONFIG_EM_SEL;
        volatile uint32_t em_size = (uint32_t)&__LD_CONFIG_EM_SEL;
        if (em_size == 0)
            GLB_Set_EM_Sel(GLB_WRAM160KB_EM0KB);
        else if (em_size == 32 * 1024)
            GLB_Set_EM_Sel(GLB_WRAM128KB_EM32KB);
        else
            GLB_Set_EM_Sel(GLB_WRAM96KB_EM64KB);
        LOG_INF("[B] EM: %lu KB\n", em_size / 1024);
    }

    bflb_mtd_init();
    easyflash_init();
    if (rfparam_init(0, NULL, 0) != 0)
        LOG_ERR("[B] RF init FAIL\n");
    bflb_mtimer_delay_ms(3000);

    config_load();
#if DS5MAP_ENGINE_ENABLE >= 1
    ds5map_init();   /* R2: 只加这一步 */
#endif
    led_status_init();
    led_refresh_state();
    boot_button_init();
    usb_wake_init();

    usb_audio_early_init();
    bool audio_ok = (audio_init() == 0);
    if (audio_ok) audio_apply_config();      /* R3.28：按配置体里存的音频挡位 */
    LOG_INF("[B] audio: %s\n", audio_ok ? "OK" : "FAIL");

    input_queue = xQueueCreate(1, INPUT_QUEUE_ITEM_SZ);
    /* Depth-1: USB ISR always delivers the latest report to usb_task.
     * The 10-deep app_tx_fifo in bt_hid_host.c provides the actual buffering
     * and CAN_SEND_NOW flow control (one BT packet in-flight at a time). */
    output_queue = xQueueCreate(5, sizeof(usb_out_msg_t));   /* R1.6：元素带时间戳 */
    if (!input_queue || !output_queue) {
        LOG_ERR("[B] queue alloc FAIL\n");
        while (1) {}
    }

    xTaskCreate(bt_task,    "bt",    BT_TASK_STACK_SIZE,
                NULL, BT_TASK_PRIORITY, NULL);
    xTaskCreate(usb_task,  "usb",   USB_TASK_STACK_SIZE,
                NULL, USB_TASK_PRIORITY, NULL);
    if (audio_ok) {
        xTaskCreate(audio_task, "audio", AUDIO_TASK_STACK_SIZE,
                    NULL, AUDIO_TASK_PRIORITY, NULL);  /* handle stored in audio_task() */
        xTaskCreate(audio_mic_task, "mic", MIC_TASK_STACK_SIZE,
                    NULL, MIC_TASK_PRIORITY, NULL);
    }
    xTaskCreate(led_task,  "led",   LED_TASK_STACK_SIZE,
                NULL, LED_TASK_PRIORITY, NULL);
    LOG_INF("[B] init done, starting scheduler\n");
    vTaskStartScheduler();

    while (1) {}
    return 0;
}

void vApplicationIdleHook(void)
{
    __asm volatile("wfi");
}
