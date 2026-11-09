#include "ds5map.h"
#include "gyro.h"
#include "diag.h"
#include "macro_store.h"
#include "perf.h"   /* S33：MAP_REG_PERF 只读视图 */
#include "usb_gamepad.h"
#include "debug_log.h"
#include "lfs.h"
#include "bflb_mtimer.h"
#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"
#include "semphr.h"
#include <string.h>
#include <math.h>   /* R2.7：Flick Stick 用 atan2f（不声明会按 int 返回，拿到垃圾值） */

/* 引擎开关兜底：真实取值由 main.c 定义（-D 可覆盖）。
 * ds5map.h 故意不定义它，避免挡住 main.c 的设置。 */
#ifndef DS5MAP_ENGINE_ENABLE
#define DS5MAP_ENGINE_ENABLE 0
#endif

/* ==================================================================== */
/* Config blob (LittleFS "cfg.bin")                                     */
/* ==================================================================== */

#define DS5MAP_CFG_MAGIC 0x44354D43u /* "CM5D" */
/* v10: 删掉摇杆曲线区（map_stick_curves_t，1024 B）。按用户要求**不做旧版本迁移**：
 * 版本号不匹配就整份回落到 cfg_defaults()（见 ds5map_load）。
 * v11（S17）：陀螺仪默认值改成「常开 + 右摇杆 + 灵敏度 132/140」。**故意升版本号**，
 * 让设备里那份 v10 配置整体作废 —— 否则存着的配置会盖住新默认值，用户看到"改了没用"。
 * 用户规则：不考虑兼容老版本，映射/宏一起作废是预期行为，别为了保它们去绕。
 * v12（S18）：陀螺仪默认再加「水平/垂直都反向」（用户实测方向才对）。同样直接升版本号。
 * v13（S19 未烧成、并入 S20）：方向符号改到 `gyro.c` 里定死（`GYRO_BASE_SIGN_*`），cfg 的
 * inv 默认回到 0 —— 用户要的是"**不勾反向也是正确方向**"。
 * v13 同时定：**死区默认 0**、灵敏度默认 1024（64×，用户说"64× 才是常规灵敏度"）。
 * v14（R2.5）：**两段式扳机**。MAP_BTN_COUNT 39 → 41（新增 L2/R2 二段两个源键），
 * SLOTS 区 5850 → 6150 B；同时把 FLICK(0x0C) / GEST(0x0D) 两个 region 与结构体一次落位
 * （默认全关），后面两个功能上线时不再动配置版本号。老配置整份回落默认（用户规则）。
 * v15（R3.25）：**整套扳机设置改成按层**（用户 2026-10-17 要求）——
 * `map_s2t_t` 14 → 66 B（追加覆盖层 1..4 的两段位/左右分界点 + 那一套「自适应扳机」参数），
 * 于是 `ds5map_body_t`（cfg.bin 的整块）大小也变了 → 老 cfg.bin 读不出来，**整份回落默认**。
 * 刷这一版之前先在网页上按一次「保存」，把当前参数存成一份游戏配置；刷完在下拉里选回那份
 * （老文件的 s2t 标量只有主层那一份，会原样落到主层，覆盖层那份默认「普通模式 + 效果关闭」）。 */
#define DS5MAP_CFG_VER   15
#define DS5MAP_CFG_FILE  "cfg.bin"

#define DEF_HOLD_MS  300
#define DEF_DTAP_MS  150
#define DEF_TTAP_MS  300
#define DEF_PULSE_MS 40

#define ACT_MAX   32
#define INPUT_LEN 63

typedef struct {
    map_timing_t     timing;
    char             layer_names[MAP_LAYERS][16];
    map_slot_t       slots[MAP_LAYERS][MAP_BTN_COUNT][MAP_FORM_COUNT];
    map_layer_bind_t binds[MAP_SHIFT_LAYERS];
    map_gyro_t       gyro;                  /* 全局一份，所有层共用 */
    map_tp_t         tp;                    /* 全局，不分层 */
    map_vk_t         vk[MAP_VK_COUNT];
    map_strat_t      strats[MAP_STRAT_COUNT];
    map_s2t_t        s2t;                   /* R2.5 两段式扳机：手感整机一份，模式位/分界点按层（R3.25） */
    map_flick_t      flick;                 /* R2.5 落位，R2.6 上线（默认关） */
    map_gest_t       gest;                  /* R2.5 落位，R2.7 上线（默认关） */
} ds5map_body_t;

/* 历史版本（v3 / v6 / v7 / v8）的磁盘布局类型与迁移逻辑已按用户要求删除
 * （2026-10-11，不需要兼容老配置）：读到旧版本号整份用 cfg_defaults()。 */

static ds5map_body_t g_cfg;

/* Derived caches */
static bool g_any_managed;
static bool g_any_mapping;
static bool g_has_kbd;
static bool g_has_mouse;
static bool g_managed[MAP_BTN_COUNT];

/* S36：每帧快速路径。上一帧完整路径算出"没有任何时间相关的东西在跑"且这一帧的按键位 +
 * 触摸板触点与上一帧完全一样 → 直接套用上一帧的映射结果，跳过整条处理链。
 * g_force_full 由 RPC 写入 / 定时器回调 / 断开置位，逼下一帧走完整路径重新评估。 */
static bool          g_fast_ok;
static volatile bool g_force_full = true;
static uint8_t       g_fast_in[11];     /* p[7..9] + p[32..39] */
static uint8_t       g_fast_out[3];     /* 映射后的 p[7..9] */
static uint8_t       g_fast_l2, g_fast_r2;   /* 0 不改 / 1 置 0xFF / 2 置 0x00 */

/* Staged-but-unflushed config flag */
static volatile bool g_cfg_dirty;

/* R1.3：静置校准出来的新零偏，等 deferred（bt 任务）写进 gcal.bin */
static float         g_gcal_bias[3];
static volatile bool g_gcal_dirty;
static map_cfg_id_t g_cfg_id;          /* R2.4：配置身份牌（MAP_REG_CFG_ID），存 cfgid.bin */

/* ==================================================================== */
/* Runtime state                                                        */
/* ==================================================================== */

typedef struct {
    uint8_t  raw;
    uint8_t  taps;           /* presses in the current sequence (0 = idle, 1..3) */
    uint8_t  hold_fired;
    uint8_t  nturbo_run;     /* 原生连发：这一轮按住是否已启动 */
    uint8_t  nturbo_state;   /* 原生连发：当前相位 */
    uint8_t  ntoggle_on;     /* 原生开关：锁存状态（按一下开，再按一下关） */
    uint32_t press_ms;
    uint32_t release_ms;     /* last release edge */
    uint32_t seq_ms;         /* press time of tap #1 of the current sequence */
    uint32_t nturbo_next;    /* 原生连发：下一次翻转时刻 */
} btn_rt_t;

static btn_rt_t g_btn[MAP_BTN_COUNT];

typedef struct {
    uint8_t  used;
    uint8_t  btn;
    uint8_t  form;
    uint8_t  out;
    uint8_t  val;
    uint8_t  mod;
    uint8_t  flags;
    uint8_t  param;
    uint8_t  latched;
    uint8_t  turbo_state;
    uint32_t end_ms;         /* 0 = until source release */
    uint32_t turbo_next;
} act_t;

static act_t g_acts[ACT_MAX];

static uint8_t g_layer_mask;                     /* bits 1..4 */
static uint8_t g_stack[MAP_SHIFT_LAYERS];        /* layer ids, [0] most recent */
static uint8_t g_bind_hold_act;                  /* bitmask over binds[] */

static uint8_t g_last_kbd[8];
static uint8_t g_last_mouse_btns;
static uint16_t g_last_media;        /* R1 / R2.2：最近一次发出去的 Consumer Control 位图（OUT_MEDIA，16 位） */
static bool    g_media_pending;      /* 想发但端点忙没发出去：下一帧必须再试（别走快速路径） */

/* Macro player (single instance; playback buffer) */
static uint8_t g_macro_buf[MAP_MACRO_MAX_BYTES];
static map_macro_hdr_t *g_mph;
static map_macro_step_t *g_mps;

typedef struct {
    uint8_t  active;
    uint8_t  trigger;
    uint8_t  src_btn;
    uint8_t  id;
    uint16_t step;
    uint16_t repeat_left;
    uint32_t next_ms;
    uint32_t pause_until;
} macro_player_t;

static macro_player_t g_mp;

static uint64_t g_macro_pad;       /* 64-bit: one bit per map_btn id */
static uint8_t  g_macro_kbd[6];
static uint8_t  g_macro_mouse;
static int8_t   g_wheel_pending;   /* accumulated wheel clicks, flushed by process() */

/* 战备宏播放器：按顺序把方向序列敲出去。每个方向「按下 → 松开」交替，
 * 中间必须真的松开一次——像「补给」的 ↓↓↑↑ 这种连续同向的码，
 * 不松开的话游戏只会看到一次按键。松开的间隔也顺带把两个相邻方向分开。 */
#define STRAT_PRESS_MS 30
#define STRAT_GAP_MS   30

typedef struct {
    uint8_t  active;
    uint8_t  step;                     /* 正在打第几个方向 */
    uint8_t  down;                     /* 1 = 这个方向的键正按着 */
    uint8_t  len;
    uint8_t  seq[MAP_STRAT_MAX_LEN];
    uint32_t next_ms;
} strat_player_t;

static strat_player_t g_sp;
static uint8_t g_strat_pad;        /* 4 bit：bit0..3 = 上下左右（对齐 MAP_BTN_DPAD_UP..） */

/* Telemetry */
static uint8_t g_tlm_raw[INPUT_LEN];
static uint8_t g_tlm_out[INPUT_LEN];
static volatile uint16_t g_tlm_seq;
static volatile uint8_t g_tlm_wheel;      /* 最后一次滚轮增量（i8） */
static volatile uint8_t g_tlm_wheel_seq;  /* 滚轮事件计数 */
static uint8_t g_tlm_vk_mask;             /* 这一帧按下的虚拟组合键位（bit0..3 = V1..V4） */
/* 鼠标增量累计：主机 20Hz 轮询，而报文是 500Hz —— 单帧增量落在两次读取
 * 中间就永远看不见了，所以固件这边累加，读一次清一次。 */
static int32_t g_tlm_acc[6];              /* 触摸板 xy / 陀螺仪 xy / 实发 xy */

/* 陀螺仪姿态遥测的 16 字节缓存（dir/flags/conf/cal_prog + grav Q14×3 + bias Q8×3）。
 * build_tlm_state() 是在 USB ISR 里跑的，而 gyro_get_status()/f_to_i16() 里有
 * 浮点运算 —— 这颗芯片的 ISR 里碰 FPU 就是硬 fault（整机卡死/复位），
 * 所以打包只在任务侧做，ISR 只 memcpy 这份缓存。 */
static uint8_t g_tlm_gyro[16];
static void tlm_gyro_pack(uint8_t *out);   /* 只能在任务上下文调用（有浮点）！*/

/* 上次异常 / 故障现场：定义在 SDK interrupt.c 的 noinit RAM 里，布局见 crash_record.h；
 * exception_entry 与 fault_hooks.c 填写，这里通过 MAP_REG_CRASH 读回。 */
#include "crash_record.h"

/* 累加到位就饱和（int16 能表示的范围内），页面关着不读也不会把计数器撑爆 */
static void tlm_acc_add(int i, int v)
{
    int32_t n = g_tlm_acc[i] + v;
    if (n > 32767) n = 32767; else if (n < -32768) n = -32768;
    g_tlm_acc[i] = n;
}

/* RPC */
static uint8_t g_resp[MAP_RPC_REPORT_LEN];

/* RPC 工作缓冲（2026-10-10 修）。
 *
 * ds5map_rpc_request() 运行在 **USB ISR 上下文**，原先在栈上开了
 *   MAP_TLM_STATE_SIZE + MAP_RPC_CHUNK_MAX(122) + MAP_META_SIZE(92) 等缓冲。
 * 中断栈只有几 KB，一次处理就可能溢出 -> 反复重启（只有编进 RPC 分支才复现）。
 * 改成静态缓冲：RPC 处理本就不重入（ISR 内串行），且都在 MAP_LOCK 保护下。 */
static uint8_t g_rpc_work[MAP_RPC_CHUNK_MAX];
static uint8_t g_rpc_meta[MAP_META_SIZE];
static uint8_t g_rpc_status[MAP_STATUS_SIZE];
static uint8_t g_rpc_tlm[MAP_TLM_STATE_SIZE];
static volatile uint16_t g_resp_len;

/* ===== 固件版本标识（只读，供主机确认"设备里跑的是哪一版"）===== */
#define MAP_VER_SIZE 40
static uint8_t g_ver_blob[MAP_VER_SIZE];
static bool    g_ver_ready;

#ifndef DS5MAP_BUILD_TAG
#define DS5MAP_BUILD_TAG "unknown"
#endif

void ds5map_set_build_tag(const char *fwver)
{
    memset(g_ver_blob, 0, sizeof(g_ver_blob));
    g_ver_blob[0] = 'V'; g_ver_blob[1] = 'E'; g_ver_blob[2] = 'R'; g_ver_blob[3] = '@';
    strncpy((char *)g_ver_blob + 4, DS5MAP_BUILD_TAG, 16);
    if (fwver)
        strncpy((char *)g_ver_blob + 20, fwver, 16);
    /* 开关状态快照：非 0 表示该功能已打开 */
    g_ver_blob[36] = (uint8_t)DS5MAP_ENGINE_ENABLE;
    g_ver_ready = true;
}
static uint8_t g_pend_req[MAP_RPC_REPORT_LEN];
static uint16_t g_pend_len;
static volatile bool g_pend_active;

static SemaphoreHandle_t s_lock;

/* Runtime state is touched from the BT input task, the USB task (deferred
 * work) and the 1 ms timer task; serialize with a recursive mutex. The USB
 * ISR (RPC handler) only writes RAM config staging / response buffers. */
#define MAP_LOCK()   do { if (s_lock) xSemaphoreTakeRecursive(s_lock, portMAX_DELAY); } while (0)
#define MAP_UNLOCK() do { if (s_lock) xSemaphoreGiveRecursive(s_lock); } while (0)

/* ==================================================================== */
/* Helpers                                                              */
/* ==================================================================== */

/* 可分段累加：crc16_ccitt(a||b) == crc16_update(crc16_update(0,a), b)。
 * v7 配置的陀螺仪块位置变了，得拆成几段分别读到最终位置，CRC 只能跟着分段算。 */
static uint16_t crc16_update(uint16_t crc, const uint8_t *d, uint32_t n)
{
    while (n--) {
        crc ^= (uint16_t)(*d++) << 8;
        for (int i = 0; i < 8; i++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021)
                                 : (uint16_t)(crc << 1);
    }
    return crc;
}

static uint16_t crc16_ccitt(const uint8_t *d, uint32_t n)
{
    return crc16_update(0, d, n);
}

static inline uint32_t now_ms(void)
{
    return (uint32_t)(bflb_mtimer_get_time_us() / 1000U);
}

static inline uint32_t pulse_ms(void)
{
    return g_cfg.timing.tap_pulse_ms ? g_cfg.timing.tap_pulse_ms : DEF_PULSE_MS;
}

/* 栈只进不出（layer_deactivate 只清 g_layer_mask，不动 g_stack），所以「栈顶」
 * 还得拿 mask 过一遍：否则层键一松，那层仍霸着 top_layer()，陀螺仪会一直去读
 * 一份早就退场的设置。用 mask 过滤后的语义和 slot_lookup() 的走法一致。 */
static uint8_t top_layer(void)
{
    for (int i = 0; i < MAP_SHIFT_LAYERS; i++) {
        uint8_t l = g_stack[i];
        if (l != 0xFF && (g_layer_mask & (uint8_t)(1u << l)))
            return l;
    }
    return MAP_LAYER_ID_MAIN;
}

/* The TAP slot decides what the button *becomes* in this layer:
 *   本体 (OUT_SELF, default) -> the button's own native output
 *   无   (OUT_NONE)         -> nothing at all
 *   anything else           -> that action INSTEAD of the button itself
 * Unmapped buttons have no slot at all, so they pass straight through. */
static const map_slot_t *slot_lookup(uint8_t btn, uint8_t form);

static bool native_on(uint8_t btn)
{
    const map_slot_t *s = slot_lookup(btn, FORM_TAP);
    if (!s || s->out == OUT_SELF)
        return true;
    /* 「保留原键输出」（MAP_FLAG_PASS_NATIVE）：这个键被改成别的动作时，原键**照样**进输出掩码，
     * 于是电脑那边同时收到「原键 + 这个动作」＝叠加而不是替换（用户在网页上勾选）。 */
    return (s->flags & MAP_FLAG_PASS_NATIVE) != 0;
}

static void layer_stack_push(uint8_t l)
{
    int pos = MAP_SHIFT_LAYERS - 1;
    for (int i = 0; i < MAP_SHIFT_LAYERS; i++) {
        if (g_stack[i] == l) { pos = i; break; }
    }
    for (int i = pos; i > 0; i--)
        g_stack[i] = g_stack[i - 1];
    g_stack[0] = l;
}

static void layer_activate(uint8_t l)
{
    g_layer_mask |= (uint8_t)(1u << l);
    layer_stack_push(l);
}

static void layer_deactivate(uint8_t l)
{
    g_layer_mask &= (uint8_t)~(1u << l);
}

static void bind_fire(uint8_t btn, uint8_t form)
{
    for (int i = 0; i < MAP_SHIFT_LAYERS; i++) {
        const map_layer_bind_t *bd = &g_cfg.binds[i];
        if (bd->src_btn != btn || bd->src_form != form)
            continue;
        uint8_t l = (uint8_t)(i + 1);
        if (bd->mode == MAP_LAYER_MODE_HOLD) {
            g_bind_hold_act |= (uint8_t)(1u << i);
            layer_activate(l);
        } else {
            if (g_layer_mask & (uint8_t)(1u << l))
                layer_deactivate(l);
            else
                layer_activate(l);
        }
    }
}

static void bind_release_btn(uint8_t btn)
{
    for (int i = 0; i < MAP_SHIFT_LAYERS; i++) {
        if (!(g_bind_hold_act & (uint8_t)(1u << i)))
            continue;
        if (g_cfg.binds[i].src_btn == btn) {
            g_bind_hold_act &= (uint8_t)~(1u << i);
            layer_deactivate((uint8_t)(i + 1));
        }
    }
}

static const map_slot_t *slot_lookup(uint8_t btn, uint8_t form)
{
    for (int i = 0; i < MAP_SHIFT_LAYERS; i++) {
        uint8_t l = g_stack[i];
        if (l == 0xFF || !(g_layer_mask & (uint8_t)(1u << l)))
            continue;
        const map_slot_t *s = &g_cfg.slots[l][btn][form];
        if (s->out != OUT_SELF)
            return s;
    }
    const map_slot_t *s = &g_cfg.slots[MAP_LAYER_ID_MAIN][btn][form];
    return (s->out != OUT_SELF) ? s : NULL;
}

/* ==================================================================== */
/* Action table                                                         */
/* ==================================================================== */

static void acts_clear(void)
{
    memset(g_acts, 0, sizeof(g_acts));
}

static void act_release_btn(uint8_t btn)
{
    for (int i = 0; i < ACT_MAX; i++) {
        act_t *a = &g_acts[i];
        if (!a->used || a->btn != btn)
            continue;
        if ((a->flags & MAP_FLAG_TOGGLE) || a->latched)
            continue;                     /* latched outputs survive */
        if (a->end_ms == 0)               /* until-release entry */
            a->used = 0;
    }
}

static uint32_t turbo_period_ms(uint8_t param)
{
    uint32_t p = param ? (uint32_t)param * MAP_TURBO_UNIT_MS
                       : MAP_TURBO_DEFAULT_MS;
    if (p < 20)
        p = 20;
    return p;
}

static void act_add(uint8_t btn, uint8_t form, const map_slot_t *s, uint32_t now)
{
    if (s->flags & MAP_FLAG_TOGGLE) {
        /* second activation latches off */
        for (int i = 0; i < ACT_MAX; i++) {
            act_t *a = &g_acts[i];
            if (a->used && a->btn == btn && a->form == form && a->latched) {
                a->used = 0;
                return;
            }
        }
    }

    act_t *a = NULL;
    for (int i = 0; i < ACT_MAX; i++) {
        if (!g_acts[i].used) { a = &g_acts[i]; break; }
    }
    if (!a)
        return;

    memset(a, 0, sizeof(*a));
    a->used = 1;
    a->btn = btn;
    a->form = form;
    a->out = s->out;
    a->val = s->val;
    a->mod = s->mod;
    a->flags = s->flags;
    a->param = s->param;
    a->latched = (s->flags & MAP_FLAG_TOGGLE) ? 1 : 0;

    if (s->flags & MAP_FLAG_TURBO) {
        a->end_ms = 0;                     /* runs until source release */
        a->turbo_state = 1;
        a->turbo_next = now + turbo_period_ms(a->param) / 2;
    } else if (form == FORM_RELEASE) {
        a->end_ms = now + pulse_ms();      /* released already — short blip */
    } else {
        a->end_ms = 0;                     /* press/hold/tap/dtap/ttap: while held */
    }
}

static bool act_on(const act_t *a)
{
    /* 连发优先于锁存：开关 + 连发＝按一下开始自动连发，再按一下停。
     * （tick 对 latched 的连发一直翻转相位，这里必须放行，
     *  否则「开关 + 连发」会被锁存短路成常开。） */
    if (a->flags & MAP_FLAG_TURBO)
        return a->turbo_state != 0;
    return true;
}

/* ==================================================================== */
/* Output synthesis                                                     */
/* ==================================================================== */

static void synth_kbd(void)
{
    uint8_t rep[8];
    int slot = 2;

    memset(rep, 0, sizeof(rep));
    for (int i = 0; i < ACT_MAX; i++) {
        const act_t *a = &g_acts[i];
        if (!a->used || a->out != OUT_KBD || !act_on(a))
            continue;
        rep[0] |= a->mod;
        if (slot < 8)
            rep[slot++] = a->val;
    }
    for (int k = 0; k < 6; k++) {
        uint8_t kc = g_macro_kbd[k];
        if (!kc)
            continue;
        if (kc >= 0xE0 && kc <= 0xE7) {          /* macro-recorded modifier */
            rep[0] |= (uint8_t)(1u << (kc - 0xE0));
            continue;
        }
        if (slot < 8)
            rep[slot++] = kc;
    }

    if (memcmp(rep, g_last_kbd, 8) == 0)
        return;
    memcpy(g_last_kbd, rep, 8);
    /* R2.4：进队发送，端点忙也不会丢（以前忙就丢、却照样记成已发 → 电脑上的键卡住） */
    usb_gamepad_send_kbd_report(rep, 8);
}

static void synth_mouse(int dx, int dy, int8_t wheel)
{
    uint8_t btns = 0;
    for (int i = 0; i < ACT_MAX; i++) {
        const act_t *a = &g_acts[i];
        if (!a->used || a->out != OUT_MOUSE || a->val > 2 || !act_on(a))
            continue;
        btns |= (uint8_t)(1u << a->val);
    }
    btns |= g_macro_mouse;

    if (dx > 127) dx = 127; else if (dx < -127) dx = -127;
    if (dy > 127) dy = 127; else if (dy < -127) dy = -127;

    if (btns == g_last_mouse_btns && wheel == 0 && dx == 0 && dy == 0)
        return;
    g_last_mouse_btns = btns;
    if (wheel) {
        g_tlm_wheel = (uint8_t)wheel;
        g_tlm_wheel_seq++;
    }
    usb_gamepad_send_mouse_report(btns, (int8_t)dx, (int8_t)dy, wheel);   /* R2.4：进队 */
}

/* R1 / R2.2：媒体与多功能键（Consumer Control，报告 ID 2）：把所有生效中的 OUT_MEDIA 动作
 * 并成 16 位位图（R2.2 起 2 字节）。
 * 和键盘报告不同：没发出去就不记为"已发"，下一帧重试——播放/暂停这种按一下的键丢一次就是丢了。 */
static void synth_media(void)
{
    uint16_t bits = 0;
    for (int i = 0; i < ACT_MAX; i++) {
        const act_t *a = &g_acts[i];
        if (!a->used || a->out != OUT_MEDIA || a->val > 15 || !act_on(a))
            continue;
        bits |= (uint16_t)(1u << a->val);
    }
    if (bits == g_last_media) {
        g_media_pending = false;
        return;
    }
    if (usb_gamepad_send_consumer_report(bits) == 0) {
        g_last_media = bits;
        g_media_pending = false;
    } else {
        g_media_pending = true;
    }
}

/* ==================================================================== */
/* Gyro → mouse / stick                                                 */
/* ==================================================================== */
/* 角速度轴按 kernel hid-playstation 的标定顺序落在报文里：
 * p[15]=pitch（俯仰）p[17]=yaw（偏摆）p[19]=roll（翻滚），16 LSB 每度/秒。
 * 姿态估计（重力方向、零偏、player space）全在 gyro.c 里，这里只管把
 * gyro_read_hv() 给的两条角速度（raw LSB）乘灵敏度、累加、写进输出。
 * 缩放固定点：鼠标 >>15、摇杆 >>13（摇杆量程小，同灵敏度下放大 4 倍），
 * sens 是 Q4（16 = 1.0×）。亚像素余量留在累加器里，慢转不会丢步。 */
#define GYRO_SHIFT_MOUSE 15
#define GYRO_SHIFT_STICK 13

static int32_t g_gyro_acc[2];
static int     g_gyro_dx, g_gyro_dy;
static bool    g_gyro_on;          /* 选了目标且启用模式不是「关闭」——快速路径用它兜底 */
/* ---------- R2.7 Flick Stick 的运行时状态（配置在 g_cfg.flick） ---------- */
static bool    g_flick_on;         /* 配了启用模式（＝会用）—— 快速路径/捷径用它兜底 */
static bool    g_flick_live;       /* 本帧真的接管右摇杆 */
static bool    g_flick_engaged;    /* 迟滞：摇杆现在在激活圈里 */
static bool    g_flick_btn_raw;    /* 启用键上一帧状态（开关模式判按下沿） */
static bool    g_flick_latch;      /* 开关模式的锁存 */
static float   g_flick_theta;      /* 上一帧角度（入沿重定基线） */
static int32_t g_flick_acc;        /* 一帧发不完的鼠标计数留到下一帧 */
static int     g_flick_dx, g_flick_dy;
/* R3.0：L2/R2 的「一段被二段压住」抑制状态（release_s1 开关打开时用） */
static bool    g_s1_suppressed[2];
static volatile uint8_t g_gyro_probe;  /* 测试页读遥测 / 发校准命令时置位：下一帧刷新陀螺仪遥测 */
static bool    g_gyro_live;        /* 本帧是否真的叠加（按住/开关模式逐帧判定） */
static bool    g_gyro_latch;       /* 开关模式的锁存状态 */
static uint8_t g_gyro_btn_raw;     /* 上一帧启用键的状态（开关模式判边沿） */

/* R2.5：两段式扳机。g_s2t_latch = 两颗扳机「二段已按下」的锁存（bit0 左 / bit1 右），
 * 纯阈值判定要它；g_s2t_used = 真有槽位引用这两个源键（没人用就整个跳过，连快速路径的
 * 比较都省掉）；g_fast_s2 = 上一帧完整路径算出来的二段状态位。
 * R3.25：模式位/分界点按层了 —— 二段状态还取决于**当前激活层**，所以把那一层
 * （g_fast_layer）也并进快速路径的比较键：层变了就得重算，不能拿上一层的缓存顶。 */
static uint8_t g_s2t_latch;
static bool    g_s2t_used;
static uint8_t g_fast_s2;
static uint8_t g_fast_layer;
/* R3.25（第二轮）：上一帧用的「扳机设置属于哪一层」。层一变就请求重发一次扳机效果，
 * 否则桌面上没有主机输出帧时，切了层手感还是上一层的（main.c 的 bt 循环取这个请求）。 */
static uint8_t g_fx_layer;

static int32_t gyro_step(int idx, int32_t raw, uint16_t sens, int shift)
{
    int32_t want = g_gyro_acc[idx] + raw * (int32_t)sens;
    int32_t out = want >> shift;
    g_gyro_acc[idx] = want - (out << shift);
    return out;
}

/* 配置 → 陀螺仪模块。cfg 一动就得同步，不然改了轴/灵敏度要等下次重启才生效。 */
static void gyro_sync_cfg(void)
{
    gyro_cfg_t gc;
    gc.dir      = GYRO_DIR_LOCAL;    /* 只剩本体轴：跟随重力已删除（2026-10-11） */
    gc.axis_h   = g_cfg.gyro.axis_h;
    gc.axis_v   = g_cfg.gyro.axis_v;
    gc.inv_h    = g_cfg.gyro.inv_h;
    gc.inv_v    = g_cfg.gyro.inv_v;
    gc.deadzone = g_cfg.gyro.deadzone;
    gyro_apply_cfg(&gc);
}

/* 在映射完的报文上叠加陀螺仪动作；鼠标的增量放到 g_gyro_dx/dy 里等 synth_mouse */
static void gyro_apply(uint8_t *p)
{
    const map_gyro_t *gy = &g_cfg.gyro;

    g_gyro_dx = 0;
    g_gyro_dy = 0;
    if (gy->target == MAP_GYRO_OFF || gy->target > MAP_GYRO_RSTICK)
        return;

    int32_t h, v;
    if (!gyro_read_hv(p, &h, &v))
        return;

    if (gy->target == MAP_GYRO_MOUSE) {
        int32_t dx = gyro_step(0, h, gy->sens_h, GYRO_SHIFT_MOUSE);
        int32_t dy = gyro_step(1, v, gy->sens_v, GYRO_SHIFT_MOUSE);
        /* 单帧输出钳位（2026-10-11 修）。
         * 摇杆分支一直有 ±127 钳位，鼠标分支却没有 —— 于是融合状态跳变时
         * （刚连上、重力还没收敛、或传感器抖动）单帧输出会冲到几百
         * （实测 453 / -203，而正常鼠标增量是 ±1~20），指针瞬间乱飞。
         * 鼠标报告本身就只有 ±127 的范围，这里按同样上限截断最稳妥。 */
        if (dx >  127) dx =  127; else if (dx < -127) dx = -127;
        if (dy >  127) dy =  127; else if (dy < -127) dy = -127;
        g_gyro_dx = (int)dx;
        g_gyro_dy = (int)dy;
        return;
    }

    /* 摇杆：直接叠加到输出摇杆上（以 128 为中心，超界截断） */
    g_gyro_acc[0] = 0;
    g_gyro_acc[1] = 0;
    int32_t dh = gyro_step(0, h, gy->sens_h, GYRO_SHIFT_STICK);
    int32_t dv = gyro_step(1, v, gy->sens_v, GYRO_SHIFT_STICK);
    if (dh > 127) dh = 127; else if (dh < -127) dh = -127;
    if (dv > 127) dv = 127; else if (dv < -127) dv = -127;

    int ix = (gy->target == MAP_GYRO_LSTICK) ? 0 : 2;
    int iy = ix + 1;
    int32_t x = (int32_t)p[ix] + dh;
    int32_t y = (int32_t)p[iy] + dv;
    p[ix] = (uint8_t)(x < 0 ? 0 : (x > 255 ? 255 : x));
    p[iy] = (uint8_t)(y < 0 ? 0 : (y > 255 ? 255 : y));
}

/* ==================================================================== */
/* Touchpad — single finger move + two finger pinch                     */
/* ==================================================================== */
/* 手指坐标是报文里的 12 位绝对值，板子 1920×1080、原点左上（见 remap.c）。
 * 按下（MAP_BTN_TP_CLICK / 手指1-触摸 / 手指2-触摸）走普通按键映射表，
 * 这里只管「移动」：单指一个目标，双指距离变化＝开合脉冲动作。 */
#define TP_CENTER_X        960
#define TP_CENTER_Y        540
#define TP_DEADZONE        120      /* 十字按键的死区（板单位） */
#define TP_PINCH_DEF_PX    150      /* 双指距离变化多少板单位触发一次 */
#define TP_PINCH_MIN_PX    40
#define TP_PINCH_MAX_PX    600
/* 灵敏度（sens）鼠标和摇杆共用一根滑条，但两条路的换算基准不同：
 *   鼠标：滑动增量 × sens/16 —— 板子动 16 像素、指针走 sens 像素；
 *   摇杆：中心到板边 = 128 × sens/8 —— sens=8（默认）时整块板正好等于满量程，
 *         和「没有灵敏度」的旧行为完全一致，往右拉就是放大推杆量。
 * 取 8 而不是 16 是为了让默认值保持原样，改了老配置的手感会变。 */
#define TP_MOUSE_SENS_DEF  8
#define TP_MOUSE_SENS_MAX  32
#define TP_STICK_SENS_BASE 8

/* 双指开合是脉冲动作，借两个不存在的按钮号当载体（不查按键状态机） */
#define VBTN_PINCH_OUT     MAP_BTN_COUNT
#define VBTN_PINCH_IN      (MAP_BTN_COUNT + 1)

static void fire_slot(uint8_t btn, uint8_t form, const map_slot_t *s, uint32_t now);

static bool    g_tp_on;             /* 单指目标/双指动作至少配了一个 */
static uint8_t g_tp_mask;           /* 单指目标（十字按键）产生的按键位 */
static int     g_tp_dx, g_tp_dy;    /* 鼠标增量，交给 synth_mouse 一起发 */
static int16_t g_tp_sx, g_tp_sy;    /* 摇杆目标的轴值，曲线之后覆盖输出 */
static bool    g_tp_stick;          /* 本帧要不要覆盖目标摇杆 */
static int16_t g_tp_last_x = -1, g_tp_last_y = -1;   /* 单指鼠标上一帧坐标 */
static int16_t g_tp_pinch_dist;     /* 双指距离（曼哈顿），-1 = 双指不在 */
static int     g_tp_pinch_acc;      /* 距离变化累加，过阈值触发一次 */
static uint8_t g_tp_finger;         /* 触摸板状态（给 TLM_STATE 看）：bit0 单指, bit1 双指 */
static uint8_t g_tp_pinch_seq[2];   /* 张开/捏合触发计数，主机比对变化就知道刚触发过 */

/* 触点：[0] bit7 = 无手指，[1] x 低 8 位，[2] x 高 4 位 | y 低 4 位，[3] y 高 8 位 */
static bool tp_finger(const uint8_t *tp, int16_t *x, int16_t *y)
{
    if (tp[0] & 0x80)
        return false;
    *x = (int16_t)(tp[1] | ((tp[2] & 0x0F) << 8));
    *y = (int16_t)(((tp[2] & 0xF0) >> 4) | (tp[3] << 4));
    return true;
}

/* ---- 源位：报文 → src[0..30]（S33 起本文件自带，不再依赖已删除的 remap.c） ----
 *   p[7]: 十字键 hat[3:0]  □[4] ✕[5] ○[6] △[7]
 *   p[8]: L1[0] R1[1] L2[2] R2[3] Create[4] Options[5] L3[6] R3[7]
 *   p[9]: PS[0] 触摸板按下[1] 静音[2]（高 4 位是 DSE 四键，调用方另取）
 *   19..30 触摸板手指分区：手指 1 = 触点槽 0（先按下的那根）、手指 2 = 触点槽 1。
 *   TOUCH = 该手指在板上；上/下/左/右 = 相对板中心、超出死区、按主导轴判定；
 *   CLICK = 该手指在板上且板子被物理按下。
 *   原作者 remap.c 的版本是按左右半区并由他的 tp_mode 配置门控的，tp_mode 默认 0 时这
 *   12 个源位恒为 0 —— 网页上的「手指 1 / 手指 2」按键在 S33 之前其实从来没生效过。 */
static const struct { uint8_t off; uint8_t mask; } BTN_LOC[15] = {
    [MAP_BTN_SQUARE]   = { 7, 0x10 }, [MAP_BTN_CROSS]    = { 7, 0x20 },
    [MAP_BTN_CIRCLE]   = { 7, 0x40 }, [MAP_BTN_TRIANGLE] = { 7, 0x80 },
    [MAP_BTN_L1]       = { 8, 0x01 }, [MAP_BTN_R1]       = { 8, 0x02 },
    [MAP_BTN_L2]       = { 8, 0x04 }, [MAP_BTN_R2]       = { 8, 0x08 },
    [MAP_BTN_CREATE]   = { 8, 0x10 }, [MAP_BTN_OPTIONS]  = { 8, 0x20 },
    [MAP_BTN_L3]       = { 8, 0x40 }, [MAP_BTN_R3]       = { 8, 0x80 },
    [MAP_BTN_PS]       = { 9, 0x01 }, [MAP_BTN_TP_CLICK] = { 9, 0x02 },
    [MAP_BTN_MUTE]     = { 9, 0x04 },
};

static void src_finger_zone(const uint8_t *p, const uint8_t *slot, uint8_t *src, int base)
{
    int16_t x, y;
    for (int i = 0; i < 6; i++)
        src[base + i] = 0;
    if (!tp_finger(slot, &x, &y))
        return;
    src[base + 0] = 1;                                   /* TOUCH */
    src[base + 5] = (p[9] & 0x02) ? 1 : 0;               /* CLICK */
    int dx = (int)x - TP_CENTER_X, dy = (int)y - TP_CENTER_Y;
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    if (adx < TP_DEADZONE && ady < TP_DEADZONE)
        return;
    if (ady >= adx)
        src[base + (dy < 0 ? 1 : 2)] = 1;                /* UP / DOWN */
    else
        src[base + (dx < 0 ? 3 : 4)] = 1;                /* LEFT / RIGHT */
}

static void src_bits(const uint8_t *p, uint8_t *src)
{
    for (int i = 0; i < 15; i++)
        src[i] = (p[BTN_LOC[i].off] & BTN_LOC[i].mask) ? 1 : 0;
    uint8_t hat = p[7] & 0x0F;
    src[MAP_BTN_DPAD_UP]    = (hat == 0 || hat == 1 || hat == 7) ? 1 : 0;
    src[MAP_BTN_DPAD_DOWN]  = (hat == 3 || hat == 4 || hat == 5) ? 1 : 0;
    src[MAP_BTN_DPAD_LEFT]  = (hat == 5 || hat == 6 || hat == 7) ? 1 : 0;
    src[MAP_BTN_DPAD_RIGHT] = (hat == 1 || hat == 2 || hat == 3) ? 1 : 0;
    src_finger_zone(p, &p[32], src, MAP_BTN_TP_F1_TOUCH);
    src_finger_zone(p, &p[36], src, MAP_BTN_TP_F2_TOUCH);
}

/* g_tp_mask 是 4 位小掩码（bit0..3 = 十字键 上/下/左/右），
 * 因为 id 15..18 的位放不进 uint8_t —— 并进总 mask 时左移 MAP_BTN_DPAD_UP。 */
#define TP_MASK_SHIFT  MAP_BTN_DPAD_UP

static inline uint8_t tp_bit(uint8_t btn)
{
    return (uint8_t)(1u << (btn - MAP_BTN_DPAD_UP));
}

/* 每帧解析触摸板。十字按键的位进 g_tp_mask（要并进按键 mask），
 * 摇杆值/鼠标增量留给 tp_apply_stick() 和 synth_mouse()。 */
static void tp_update(const uint8_t *p, uint32_t now)
{
    const map_tp_t *tp = &g_cfg.tp;
    int16_t x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    bool on1 = tp_finger(&p[32], &x1, &y1);
    bool on2 = tp_finger(&p[36], &x2, &y2);

    g_tp_mask = 0;
    g_tp_dx = 0;
    g_tp_dy = 0;
    g_tp_stick = false;
    g_tp_finger = (uint8_t)((on1 && !on2 ? 1 : 0) | (on1 && on2 ? 2 : 0));

    /* 双指：距离（曼哈顿，够用且不用开方）变化累计到阈值就脉冲一次 */
    if (on1 && on2) {
        int16_t ddx = (int16_t)(x1 - x2);
        int16_t ddy = (int16_t)(y1 - y2);
        if (ddx < 0) ddx = (int16_t)-ddx;
        if (ddy < 0) ddy = (int16_t)-ddy;
        int16_t d = (int16_t)(ddx + ddy);
        int16_t step = (int16_t)(tp->pinch_px ? tp->pinch_px : TP_PINCH_DEF_PX);

        if (g_tp_pinch_dist >= 0) {
            g_tp_pinch_acc += (int)d - (int)g_tp_pinch_dist;
            /* 手指张得很快时一帧可能跨过好几步，一次全打出去 */
            while (g_tp_pinch_acc >= step) {
                g_tp_pinch_acc -= step;
                g_tp_pinch_seq[0]++;
                fire_slot(VBTN_PINCH_OUT, FORM_RELEASE, &tp->pinch_out, now);
            }
            while (g_tp_pinch_acc <= -step) {
                g_tp_pinch_acc += step;
                g_tp_pinch_seq[1]++;
                fire_slot(VBTN_PINCH_IN, FORM_RELEASE, &tp->pinch_in, now);
            }
        } else {
            g_tp_pinch_acc = 0;              /* 刚变成双指：这帧只记基准 */
        }
        g_tp_pinch_dist = d;
    } else {
        g_tp_pinch_dist = -1;
        g_tp_pinch_acc = 0;
    }

    /* 单指目标只在「恰好一根手指」时生效——双指是开合手势 */
    if (!on1 || on2) {
        g_tp_last_x = -1;
        g_tp_last_y = -1;
        return;
    }

    switch (tp->single) {
    case MAP_TP_MOUSE: {
        if (g_tp_last_x >= 0) {
            int sens = tp->sens ? tp->sens : TP_MOUSE_SENS_DEF;
            /* 余量天然留在坐标差里（下一帧还是从实际坐标算），不会丢 */
            g_tp_dx = ((int)(x1 - g_tp_last_x) * sens) / 16;
            g_tp_dy = ((int)(y1 - g_tp_last_y) * sens) / 16;
        }
        g_tp_last_x = x1;
        g_tp_last_y = y1;
        break;
    }
    case MAP_TP_LSTICK:
    case MAP_TP_RSTICK: {
        int sens = tp->sens ? tp->sens : TP_MOUSE_SENS_DEF;
        int32_t ax = 128 + ((int32_t)(x1 - TP_CENTER_X) * 128 * sens)
                          / (TP_CENTER_X * TP_STICK_SENS_BASE);
        int32_t ay = 128 + ((int32_t)(y1 - TP_CENTER_Y) * 128 * sens)
                          / (TP_CENTER_Y * TP_STICK_SENS_BASE);
        if (ax < 0) ax = 0; else if (ax > 255) ax = 255;
        if (ay < 0) ay = 0; else if (ay > 255) ay = 255;
        g_tp_sx = (int16_t)ax;
        g_tp_sy = (int16_t)ay;
        g_tp_stick = true;
        break;
    }
    case MAP_TP_DPAD: {
        int dx = (int)x1 - TP_CENTER_X;
        int dy = (int)y1 - TP_CENTER_Y;
        int adx = dx < 0 ? -dx : dx;
        int ady = dy < 0 ? -dy : dy;
        if (ady >= TP_DEADZONE)
            g_tp_mask |= tp_bit(dy < 0 ? MAP_BTN_DPAD_UP : MAP_BTN_DPAD_DOWN);
        if (adx >= TP_DEADZONE)
            g_tp_mask |= tp_bit(dx < 0 ? MAP_BTN_DPAD_LEFT : MAP_BTN_DPAD_RIGHT);
        break;
    }
    default:
        break;                               /* 原生：什么都不做 */
    }
}

/* 摇杆目标要在曲线之后覆盖输出（曲线是给物理摇杆用的），没手指就不动 */
static void tp_apply_stick(uint8_t *p)
{
    if (!g_tp_stick)
        return;
    if (g_cfg.tp.single == MAP_TP_LSTICK) {
        p[0] = (uint8_t)g_tp_sx;
        p[1] = (uint8_t)g_tp_sy;
    } else {
        p[2] = (uint8_t)g_tp_sx;
        p[3] = (uint8_t)g_tp_sy;
    }
}

/* ==================================================================== */
/* Macro subsystem                                                      */
/* ==================================================================== */

static void macro_clear_outputs(void)
{
    g_macro_pad = 0;
    memset(g_macro_kbd, 0, sizeof(g_macro_kbd));
    g_macro_mouse = 0;
}

static void macro_pad_set(uint8_t id, uint8_t edge)
{
    if (id >= MAP_BTN_COUNT)
        return;
    if (edge)
        g_macro_pad |= (uint64_t)1U << id;
    else
        g_macro_pad &= ~((uint64_t)1U << id);
}

static void macro_kbd_set(uint8_t key, uint8_t edge)
{
    if (!key)
        return;
    if (edge) {
        for (int i = 0; i < 6; i++) {
            if (g_macro_kbd[i] == 0) { g_macro_kbd[i] = key; return; }
        }
    } else {
        for (int i = 0; i < 6; i++) {
            if (g_macro_kbd[i] == key) { g_macro_kbd[i] = 0; return; }
        }
    }
}

static void macro_mouse_set(uint8_t v, uint8_t edge)
{
    if (v <= 2) {
        if (edge)
            g_macro_mouse |= (uint8_t)(1u << v);
        else
            g_macro_mouse &= (uint8_t)~(1u << v);
    } else if (v == 3 || v == 4) {
        if (edge) {
            if (v == 3) {
                if (g_wheel_pending < 127) g_wheel_pending++;
            } else {
                if (g_wheel_pending > -127) g_wheel_pending--;
            }
        }
    }
}

static void macro_apply_step(uint16_t idx)
{
    const map_macro_step_t *st = &g_mps[idx];
    switch (st->act) {
    case MACRO_ACT_KEY:   macro_kbd_set(st->key, st->edge); break;
    case MACRO_ACT_MOUSE: macro_mouse_set(st->key, st->edge); break;
    case MACRO_ACT_PAD:   macro_pad_set(st->key, st->edge); break;
    default: break;
    }
}

static void macro_stop(void)
{
    g_mp.active = 0;
    g_mp.id = 0xFF;
    macro_clear_outputs();
}

static bool macro_load(uint8_t id)
{
    if (id >= MAP_MACRO_MAX_COUNT)
        return false;

    uint32_t size = 0;
    if (!macro_store_find(id, &size) ||
        size < sizeof(map_macro_hdr_t) + 2 || size > MAP_MACRO_MAX_BYTES)
        return false;

    uint16_t got = 0;
    if (macro_store_read(id, 0, g_macro_buf, (uint16_t)size, &got) != 0 ||
        got != size)
        return false;

    map_macro_hdr_t *h = (map_macro_hdr_t *)g_macro_buf;
    if (h->magic != MAP_MACRO_MAGIC || h->step_count > MAP_MACRO_MAX_STEPS)
        return false;
    if (sizeof(map_macro_hdr_t) +
        (uint32_t)h->step_count * sizeof(map_macro_step_t) + 2 > size)
        return false;

    uint16_t crc = crc16_ccitt(g_macro_buf, size - 2);
    uint16_t stored = (uint16_t)(g_macro_buf[size - 2] | (g_macro_buf[size - 1] << 8));
    if (crc != stored) {
        LOG_WRN("[MAP] macro %u CRC mismatch\n", id);
        return false;
    }

    g_mph = h;
    g_mps = (map_macro_step_t *)(g_macro_buf + sizeof(map_macro_hdr_t));
    return true;
}

static void macro_trigger(uint8_t id, uint8_t src_btn, uint32_t now)
{
    if (g_mp.active)
        return;                            /* single player instance */
    if (!macro_load(id))
        return;

    g_mp.active = 1;
    g_mp.id = id;
    g_mp.src_btn = src_btn;
    g_mp.trigger = g_mph->trigger;
    g_mp.repeat_left = g_mph->repeat ? g_mph->repeat : 1;
    g_mp.pause_until = 0;
    g_mp.next_ms = 0;

    macro_clear_outputs();

    if (g_mph->step_count == 0) {
        macro_stop();
        return;
    }
    macro_apply_step(0);
    g_mp.step = 1;
    g_mp.next_ms = now + (g_mps[0].dt_ms ? g_mps[0].dt_ms : 1);
    LOG_INF("[MAP] macro %u start (btn %u, trig %u)\n", id, src_btn, g_mp.trigger);
}

static void macro_tick(uint32_t now)
{
    if (!g_mp.active)
        return;

    if (g_mp.trigger == MAP_MACRO_TRIG_HOLD &&
        !g_btn[g_mp.src_btn].raw) {
        macro_stop();
        return;
    }

    if (g_mp.pause_until) {
        if (now < g_mp.pause_until)
            return;
        g_mp.pause_until = 0;
        g_mp.step = 0;
        g_mp.next_ms = 0;
    }

    int guard = 0;
    while (g_mp.active && g_mp.next_ms && now >= g_mp.next_ms && guard++ < 64) {
        if (g_mp.step < g_mph->step_count) {
            uint16_t dt = g_mps[g_mp.step].dt_ms;
            macro_apply_step(g_mp.step);
            g_mp.step++;
            g_mp.next_ms = now + (dt ? dt : 1);
        } else {
            macro_clear_outputs();
            if (g_mp.trigger == MAP_MACRO_TRIG_HOLD) {
                g_mp.pause_until = now + g_mph->repeat_pause_ms + 1;
                g_mp.next_ms = 0;
            } else if (g_mp.repeat_left > 1) {
                g_mp.repeat_left--;
                g_mp.pause_until = now + g_mph->repeat_pause_ms + 1;
                g_mp.next_ms = 0;
            } else {
                macro_stop();
            }
        }
    }
}

/* ==================================================================== */
/* 战备宏（《绝地潜兵 2》方向序列）                                        */
/* ==================================================================== */

static void strat_stop(void)
{
    g_sp.active = 0;
    g_sp.down = 0;
    g_sp.next_ms = 0;
    g_strat_pad = 0;
}

/* 把一条序列整个放出去。已经有一条在放就忽略——跟宏一样是单实例，
 * 不然双击/三击连着挂两条战备会撞在一起。 */
static void strat_trigger(uint8_t idx, uint32_t now)
{
    if (g_sp.active || idx >= MAP_STRAT_COUNT)
        return;

    const map_strat_t *s = &g_cfg.strats[idx];
    if (s->len == 0 || s->len > MAP_STRAT_MAX_LEN)
        return;
    for (int i = 0; i < s->len; i++) {
        if (s->dir[i] > 3)
            return;                        /* 坏数据整条不放，别乱按十字键 */
        g_sp.seq[i] = s->dir[i];
    }

    g_sp.len = s->len;
    g_sp.step = 0;
    g_sp.down = 1;
    g_sp.active = 1;
    g_strat_pad = (uint8_t)(1u << g_sp.seq[0]);
    g_sp.next_ms = now + STRAT_PRESS_MS;
}

static void strat_tick(uint32_t now)
{
    if (!g_sp.active || !g_sp.next_ms || now < g_sp.next_ms)
        return;

    if (g_sp.down) {                       /* 先松开 */
        g_strat_pad = 0;
        g_sp.down = 0;
        g_sp.next_ms = now + STRAT_GAP_MS;
        return;
    }

    g_sp.step++;                           /* 再打下一个方向 */
    if (g_sp.step >= g_sp.len) {
        strat_stop();
        return;
    }
    g_strat_pad = (uint8_t)(1u << g_sp.seq[g_sp.step]);
    g_sp.down = 1;
    g_sp.next_ms = now + STRAT_PRESS_MS;
}

/* ==================================================================== */
/* Gesture engine                                                       */
/* ==================================================================== */

/* 把一个槽位当成动作打出去。btn 可以是虚拟号（触摸板双指开合用 35/36），
 * 这种不查槽位表、也不参与按键状态机的过期判定。 */
static void fire_slot(uint8_t btn, uint8_t form, const map_slot_t *s, uint32_t now)
{
    if (!s || s->out == OUT_NONE || s->out == OUT_SELF)
        return;

    if (s->out == OUT_MACRO) {
        macro_trigger(s->val, btn, now);
        return;
    }
    if (s->out == OUT_STRAT) {
        strat_trigger(s->val, now);
        return;
    }
    if (s->out == OUT_VIB)
        return;                            /* reserved */
    if (s->out == OUT_MOUSE && s->val >= 3) {
        if (s->val == 3) {
            if (g_wheel_pending < 127) g_wheel_pending++;
        } else {
            if (g_wheel_pending > -127) g_wheel_pending--;
        }
        return;
    }
    act_add(btn, form, s, now);
}

static void fire_action(uint8_t btn, uint8_t form, uint32_t now)
{
    const map_slot_t *s = slot_lookup(btn, form);

    /* layer binds fire on the gesture itself, mapped output or not */
    bind_fire(btn, form);

    fire_slot(btn, form, s, now);
}

static void tick_internal(void)
{
    uint32_t now = now_ms();
    uint32_t hold_ms = g_cfg.timing.hold_ms;
    uint32_t dtap_ms = g_cfg.timing.dtap_ms;
    uint32_t ttap_ms = g_cfg.timing.ttap_ms;

    /* a tap sequence expires silently — no output depends on the window.
     * 虚拟组合键（MAP_BTN_IS_VK）不做长按，所以跳过它们；两段式扳机键（39/40）
     * 是普通源键，照常参与长按判定。 */
    for (int b = 0; b < MAP_BTN_COUNT; b++) {
        if (MAP_BTN_IS_VK(b) || !g_managed[b])
            continue;
        btn_rt_t *st = &g_btn[b];

        if (!st->raw && st->taps) {
            uint32_t win = (st->taps >= 2) ? ttap_ms : dtap_ms;
            if ((now - st->release_ms) >= win)
                st->taps = 0;
        }

        if (st->raw && !st->hold_fired && (now - st->press_ms) >= hold_ms) {
            st->hold_fired = 1;
            fire_action((uint8_t)b, FORM_HOLD, now);
        }
    }

    /* action table */
    for (int i = 0; i < ACT_MAX; i++) {
        act_t *a = &g_acts[i];
        if (!a->used)
            continue;

        if (a->flags & MAP_FLAG_TURBO) {
            if (a->latched) {
                /* latched turbo: keep toggling */
            } else if (!g_btn[a->btn].raw) {
                a->used = 0;
                continue;
            }
            if (now >= a->turbo_next) {
                a->turbo_state ^= 1;
                a->turbo_next = now + turbo_period_ms(a->param) / 2;
            }
        } else if (a->latched) {
            /* stays until re-fire / disconnect */
        } else if (a->end_ms && now >= a->end_ms) {
            a->used = 0;
            continue;
        } else if (a->end_ms == 0 && (a->btn >= MAP_BTN_COUNT || !g_btn[a->btn].raw)) {
            a->used = 0;
            continue;
        }
    }

    macro_tick(now);
    strat_tick(now);
    g_force_full = true;   /* S36：定时器可能改了动作/宏/层状态 */
}

static TimerHandle_t s_tick_timer;

/* S36：1 ms 定时器改按需运行。只有宏 / 战备序列在放、动作表非空、或有映射键按着（长按与
 * 连发要计时）时才让它跑；都没有就在回调里把自己停掉，下一帧看到有需要再启动。
 * 空闲时省掉每毫秒一次的抢锁 + 两次任务切换（S35 实测定时器任务占 CPU 10%）。 */
/* S37：只算"真正在计时"的东西。按着一个映射键本身不算（它的动作在松开沿由帧路径收尾），
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
    for (int b = 0; b < MAP_BTN_COUNT; b++) {
        const btn_rt_t *st = &g_btn[b];
        if (MAP_BTN_IS_VK(b) || !g_managed[b])
            continue;
        /* R1.2：原生连发在跑就要计时。「原键 + 开关 + 连发」锁存后松开了键也在翻转，
         * 以前只看按着的键 → 快速路径会把输出冻住（R1.2 起陀螺仪用户也走快速路径，会碰上）。 */
        if (st->nturbo_run)
            return true;
        if (st->raw && !st->hold_fired)
            return true;
    }
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

/* Rebuild p[7..9] from a 64-bit button mask (bit n = map_btn id n) */
static void payload_rebuild_buttons(uint8_t *p, uint64_t mask)
{
#define MBIT(id) ((mask >> (id)) & 1u)
    uint8_t du = (uint8_t)MBIT(MAP_BTN_DPAD_UP);
    uint8_t dd = (uint8_t)MBIT(MAP_BTN_DPAD_DOWN);
    uint8_t dl = (uint8_t)MBIT(MAP_BTN_DPAD_LEFT);
    uint8_t dr = (uint8_t)MBIT(MAP_BTN_DPAD_RIGHT);
    uint8_t hat;
    if      (du && dr) hat = 1;
    else if (dr && dd) hat = 3;
    else if (dd && dl) hat = 5;
    else if (dl && du) hat = 7;
    else if (du)       hat = 0;
    else if (dr)       hat = 2;
    else if (dd)       hat = 4;
    else if (dl)       hat = 6;
    else               hat = 8;

    p[7] = hat
         | (MBIT(MAP_BTN_SQUARE)   ? 0x10 : 0)
         | (MBIT(MAP_BTN_CROSS)    ? 0x20 : 0)
         | (MBIT(MAP_BTN_CIRCLE)   ? 0x40 : 0)
         | (MBIT(MAP_BTN_TRIANGLE) ? 0x80 : 0);

    p[8] = (MBIT(MAP_BTN_L1)      ? 0x01 : 0)
         | (MBIT(MAP_BTN_R1)      ? 0x02 : 0)
         | (MBIT(MAP_BTN_L2)      ? 0x04 : 0)
         | (MBIT(MAP_BTN_R2)      ? 0x08 : 0)
         | (MBIT(MAP_BTN_CREATE)  ? 0x10 : 0)
         | (MBIT(MAP_BTN_OPTIONS) ? 0x20 : 0)
         | (MBIT(MAP_BTN_L3)      ? 0x40 : 0)
         | (MBIT(MAP_BTN_R3)      ? 0x80 : 0);

    /* bit 3 belongs to Sony's vendor field and has no known meaning — keep it */
    p[9] = (uint8_t)((p[9] & 0x08)
         | (MBIT(MAP_BTN_PS)       ? 0x01 : 0)
         | (MBIT(MAP_BTN_TP_CLICK) ? 0x02 : 0)
         | (MBIT(MAP_BTN_MUTE)     ? 0x04 : 0)
         | (MBIT(MAP_BTN_FN_L)     ? 0x10 : 0)
         | (MBIT(MAP_BTN_FN_R)     ? 0x20 : 0)
         | (MBIT(MAP_BTN_BACK_L)   ? 0x40 : 0)
         | (MBIT(MAP_BTN_BACK_R)   ? 0x80 : 0));
#undef MBIT
}

/* R2.5：两段式扳机的边沿判定。side 0 = 左（p[4]）/ 1 = 右（p[5]）。
 * 行程 ≥ thresh + hyst 算按下、≤ thresh − hyst 算松开，中间保持原状（迟滞）；
 * thresh = 0 表示这一侧没开二段（永远返回 0）。 */
static uint8_t s2t_edge(uint8_t side, uint8_t v, uint8_t thresh)
{
    if (!thresh)
        return 0;
    uint8_t  bit  = (uint8_t)(1u << side);
    /* R3.8（用户要求"简单粗暴"）：**纯阈值** —— 到分界点就切二段，低于分界点就回一段。
     * 迟滞（`g_cfg.s2t.hyst`）不再参与判定：对普通用户不可解释，而且它的偏移量会让人
     * 觉得"设 200 却要压到 228"。字节保留在配置里（免得又一次配置重置），但不再读。 */
    if ((uint16_t)v >= (uint16_t)thresh)
        g_s2t_latch |= bit;
    else
        g_s2t_latch &= (uint8_t)~bit;
    return (g_s2t_latch & bit) ? 1 : 0;
}

/* R2.6：起始位置 0..255 → 硬件行程档位 0..9（每档 ≈ 10% 行程）；力度/效果类型从打包字节里按侧取
 * （力度硬件 3 bit = 1..8 档，网页那根 0~255 的滑条只是外衣）。 */
static uint8_t s2t_zone(uint8_t thresh)
{
    /* R3.11：回到**线性**换算（档 × 25.5）—— 2026-10-17 试过一版"逐档实测表"，
     * 但实测精度不够（用户："起始改成 120，实际 150 才起效"✗），已弃用。 */
    uint32_t z = ((uint32_t)thresh * 10u + 127u) / 255u;
    return (uint8_t)(z > 9 ? 9 : z);
}

static uint8_t s2t_str(const map_s2t_t *s2, uint8_t side)
{
    uint8_t v = side ? (uint8_t)(s2->strength >> 4) : (uint8_t)(s2->strength & 0x0F);
    if (v < 1) return 6;
    return (uint8_t)(v > 8 ? 8 : v);
}

static uint8_t s2t_kind(const map_s2t_t *s2, uint8_t side)
{
    uint8_t k = side ? (uint8_t)((s2->kind >> 2) & 0x03) : (uint8_t)(s2->kind & 0x03);
    return (uint8_t)(k > MAP_S2T_KIND_AUTO ? MAP_S2T_KIND_WEAPON : k);
}

static void s2t_set_str(map_s2t_t *s2, uint8_t side, uint8_t v)
{
    if (v < 1) v = 6;
    if (v > 8) v = 8;
    if (side) s2->strength = (uint8_t)((s2->strength & 0x0F) | (uint8_t)(v << 4));
    else      s2->strength = (uint8_t)((s2->strength & 0xF0) | v);
}

/* R3.25：「两段模式」位与「分界点」按层取（layer 0 = 主层，1..4 = 覆盖层）。
 *   · 主层用 0x0b 区原来的 [10] / [12] / [13]（老偏移一个字节都没动，老网页/老工具照旧读写它）；
 *   · 覆盖层 1..4 用追加段 [14..17] / [18..21] / [22..25]。
 * 传进来的 layer 超范围按主层算（不越界）。 */
uint8_t ds5map_t2_mode_layer(uint8_t layer)
{
    const map_s2t_t *s2 = &g_cfg.s2t;
    if (layer == MAP_LAYER_ID_MAIN || layer >= MAP_LAYERS)
        return (uint8_t)(s2->trig_mode & 0x03);
    return (uint8_t)(s2->t2_mode[layer - 1] & 0x03);
}

uint8_t ds5map_t2_bound_layer(uint8_t layer, uint8_t w)
{
    const map_s2t_t *s2 = &g_cfg.s2t;
    uint8_t v;
    if (layer == MAP_LAYER_ID_MAIN || layer >= MAP_LAYERS) {
        v = w ? s2->bound_r : s2->bound_l;
    } else {
        uint8_t i = (uint8_t)(layer - 1);
        v = w ? s2->t2_bnd_r[i] : s2->t2_bnd_l[i];
    }
    if (!v)                                   /* 0 = 没设过 → 回落到手感起始位置 */
        v = w ? s2->thresh_r : s2->thresh_l;
    return v;
}

/* R3.25（第二轮）：**整套扳机设置按层** —— 把某一层那一套「自适应扳机」参数装进一个临时
 * `map_s2t_t`，好让下面那些只认 `const map_s2t_t *` 的老代码（s2t_str/s2t_kind/编码器）
 * 原样复用，布局只在结构体注释里写一遍。
 *   · 层 0 / 越界：整份就是主层（老字段本来就是它），直接拷贝；
 *   · 覆盖层 1..4：把 [0..9] 那一片换成 fx[行]（列号见 S2T_FX_*）。
 * 每帧每侧调一次，66 字节的结构体拷贝 —— 可以忽略。 */
static void s2t_fx_view(uint8_t layer, map_s2t_t *v)
{
    const map_s2t_t *s = &g_cfg.s2t;
    *v = *s;
    if (layer == MAP_LAYER_ID_MAIN || layer >= MAP_LAYERS)
        return;
    const uint8_t *f = s->fx[layer - 1];
    v->effect_mode = f[S2T_FX_EFFECT];
    v->thresh_l    = f[S2T_FX_TH_L];
    v->thresh_r    = f[S2T_FX_TH_R];
    v->end_l       = f[S2T_FX_END_L];
    v->end_r       = f[S2T_FX_END_R];
    v->strength    = f[S2T_FX_STR];
    v->kind        = f[S2T_FX_KIND];
    v->freq_l      = f[S2T_FX_FREQ_L];
    v->freq_r      = f[S2T_FX_FREQ_R];
    v->flags       = f[S2T_FX_FLAGS];
}

/* ==================================================================== */
/* R2.7 Flick Stick：右摇杆拨向哪边人物就转向哪边                            */
/* ==================================================================== */
/* 算法是**角度增量积分**（免校准，和 Steam 的 flick stick 一个套路）：
 *   · 摇杆半径过 `r_on` 才算"接管"（迟滞：掉到 `r_off` 以下才退出，r_off < r_on）；
 *   · 接管的那一帧记下当前角度当基线（**入沿重定基线**，所以拨进去的瞬间不会跳变）；
 *   · 之后每帧算最短角差 `dθ = wrap_pi(θ - θ_prev)`，按 `counts = dθ × sens/2` 给鼠标增量
 *     （sens 是 Q4：16 = 1.0×；默认 1024 ⇒ 180° ≈ 1608 个计数）；
 *   · 一帧发不完的计数留在 `g_flick_acc` 里下帧接着发，不丢步；
 *   · 接管期间**把右摇杆写回 0x80**：不让游戏同时收到"摇杆推到底"和鼠标转向（双重输入会乱飘）；
 *   · y_mode 0 = 俯仰不动（默认）/ 1 = 按杆的纵向位置给俯仰 / 2 = 反号。
 * 精细瞄准交给陀螺仪：两路鼠标增量在下面 synth_mouse 那里相加。
 * 注意：这里能碰浮点（跑在任务上下文）；**绝不能挪进 build_tlm_state（USB ISR）**。 */
static void flick_apply(uint8_t rx, uint8_t ry, uint8_t *p)
{
    const map_flick_t *fl = &g_cfg.flick;
    float dx = (float)rx - 128.f, dy = (float)ry - 128.f;
    float r2 = dx * dx + dy * dy;
    float ron = (float)fl->r_on / 100.f, roff = (float)fl->r_off / 100.f;

    g_flick_dx = g_flick_dy = 0;

    /* 迟滞：进圈才算接管，掉出外圈才放手（用半径平方比，省一次开方） */
    if (!g_flick_engaged) {
        if (r2 >= ron * ron * 128.f * 128.f) {
            g_flick_engaged = true;
            g_flick_theta = atan2f(dy, dx);       /* 入沿重定基线：不跳变 */
            g_flick_acc = 0;
        }
    } else if (r2 <= roff * roff * 128.f * 128.f) {
        g_flick_engaged = false;
    }
    if (!g_flick_engaged)
        return;                                   /* 没在圈里：右摇杆照原样给游戏（这是精细瞄准的时候） */

    float th = atan2f(dy, dx);
    float dth = th - g_flick_theta;
    while (dth >  3.14159265f) dth -= 6.28318531f;   /* wrap 到 -π..π（最短角差） */
    while (dth < -3.14159265f) dth += 6.28318531f;
    g_flick_theta = th;

    int32_t want = g_flick_acc + (int32_t)(dth * (float)fl->sens * 0.5f);
    int32_t out = want;
    if (out > 127) out = 127; else if (out < -127) out = -127;
    g_flick_acc = want - out;                     /* 发不完的留到下一帧 */
    g_flick_dx = (int)out;

    if (fl->y_mode) {
        int32_t vy = ((int32_t)ry - 128) * (int32_t)fl->sens / 512;
        if (fl->y_mode == 2) vy = -vy;
        if (vy > 127) vy = 127; else if (vy < -127) vy = -127;
        g_flick_dy = (int)vy;
    }

    p[2] = 0x80;                                  /* 接管：右摇杆回中，别让游戏双重输入 */
    p[3] = 0x80;
}

static void flick_reset(void)
{
    g_flick_live = false;
    g_flick_engaged = false;
    g_flick_latch = false;
    g_flick_btn_raw = false;
    g_flick_acc = 0;
    g_flick_dx = g_flick_dy = 0;
}

void ds5map_process(uint8_t *p, uint32_t now)
{
    MAP_LOCK();

    memcpy(g_tlm_raw, p, INPUT_LEN);

    /* R1.3：自动零偏校准已删除。平时每帧的陀螺仪计算只有输出侧的 gyro_apply（按住
     * 启用键时叠加到摇杆/鼠标，R2.0 起在那边单独计时）；这里只管静置校准的那 3 秒
     * 每帧累加一次；校准成功就把新零偏交给 deferred 落盘（gcal.bin）。
     * 测试页轮询遥测时只刷新一次打包好的状态（tlm_gyro_pack 有浮点，必须在任务里做）。 */
    if (gyro_calibrating()) {
        uint32_t gc0 = perf_cyc();
        gyro_update(p, now);
        float nb[3];
        if (gyro_take_new_bias(nb)) {
            memcpy(g_gcal_bias, nb, sizeof(g_gcal_bias));
            g_gcal_dirty = true;
        }
        tlm_gyro_pack(g_tlm_gyro);
        perf_gyro_cyc(perf_cyc() - gc0);
        g_gyro_probe = 0;
    } else if (g_gyro_probe) {
        g_gyro_probe = 0;
        tlm_gyro_pack(g_tlm_gyro);
    }

    /* 触摸板/陀螺仪开着时不能走捷径：鼠标增量、按键位都在下面合成。
     * g_last_mouse_btns != 0 也一样——刚退出鼠标模式时还得再跑一帧把键松开。
     * （摇杆曲线已删除，捷径条件不再看曲线是否恒等。） */
    if (!g_any_managed && !g_mp.active && !g_sp.active &&
        !g_gyro_on && !g_flick_on && !g_tp_on && g_last_mouse_btns == 0) {
        /* 走捷径说明陀螺仪/flick/触摸板/虚拟键都没配，遥测里也照实清零 */
        g_tlm_vk_mask = 0;
        g_gyro_live = false;
        g_flick_live = false;
        g_flick_engaged = false;
        g_flick_dx = g_flick_dy = 0;
        g_tp_stick = false;
        g_tp_finger = 0;
        memcpy(g_tlm_out, p, INPUT_LEN);
        g_tlm_seq++;
        MAP_UNLOCK();
        return;
    }

    /* S36：输入没变就复用上一帧的结果。手柄 600~770 帧/s 里绝大多数帧只有摇杆 / 陀螺仪在变，
     * 按键位和触摸板触点与上一帧完全一样，映射结果也必然一样。
     * R1.2：陀螺仪开着但这一帧没生效也走这里。启用键（按住/开关）的状态只由按键位决定，
     * 按键位没变它就没变；g_fast_ok 本身要求上一帧 !g_gyro_live（「常开」模式永远走完整路径）。 */
    /* R2.5：两段式扳机的源位只依赖扳机模拟值 p[4]/p[5]，必须在快速路径判定之前算出来，
     * 才能像按键位一样让「二段状态没变」的帧复用上一帧结果。阈值两侧都 0（默认）时不
     * 计算；只有真被槽位引用了（g_s2t_used）才把它并进快速路径的比较键。 */
    uint8_t s2 = 0;
    /* R3.3b（用户报「切回普通模式后 R2 一直按着」）：**模式位要真的管用** —— 二段派生键（39/40）
     * 只在这一侧处于「两段模式」时才算数。否则切回普通模式后，设备里残留的二段映射照旧会被触发
     * （界面那边是把二段映射清掉的，但只要有一次没保存/没清干净，普通模式就会莫名其妙多按一个键）。*/
    /* R3.25：模式位和分界点**按层**了 —— 用的是**当前激活层**（覆盖层栈顶；没覆盖层就是主层）
     * 那一份，和按键映射同一套分层语义。覆盖层没开两段，那段行程就只是一段（不派生 39/40）。*/
    uint8_t t2_layer = top_layer();
    uint8_t t2_mode  = ds5map_t2_mode_layer(t2_layer);
    uint8_t t2_bl    = ds5map_t2_bound_layer(t2_layer, 0);
    uint8_t t2_br    = ds5map_t2_bound_layer(t2_layer, 1);
    /* R3.25（第二轮）：自适应扳机那套参数也按层了 —— 层一变就请 main.c 把新效果重发一次。
     * 桌面上没有主机输出帧时，这是唯一能让"切层手感立刻跟着变"的路径。
     * **必须用 _nf 版**：新层可能"没有效果"，那要走撤下（发 OFF）分支，而撤下依赖
     * s2t_applied —— forget 版会把它清掉，效果就撤不掉（见 ds5map_s2t_request_resend_nf）。 */
    if (t2_layer != g_fx_layer) {
        g_fx_layer = t2_layer;
        ds5map_s2t_request_resend_nf();
    }
    if ((t2_mode & 0x01) && t2_bl)
        s2 |= (uint8_t)(s2t_edge(0, p[4], t2_bl));
    if ((t2_mode & 0x02) && t2_br)
        s2 |= (uint8_t)(s2t_edge(1, p[5], t2_br) << 1);

    if (g_fast_ok && !g_force_full && !g_tp_on &&
        (!g_s2t_used || (s2 == g_fast_s2 && t2_layer == g_fast_layer)) &&
        memcmp(&p[7], g_fast_in, 3) == 0 && memcmp(&p[32], g_fast_in + 3, 8) == 0) {
        perf_frame_fast();
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

    uint8_t src[MAP_BTN_COUNT];
    src_bits(p, src);                    /* ids 0..30：面键/十字键/触摸板手指分区 */
    src[MAP_BTN_FN_L]   = (p[9] & 0x10) ? 1 : 0;
    src[MAP_BTN_FN_R]   = (p[9] & 0x20) ? 1 : 0;
    src[MAP_BTN_BACK_L] = (p[9] & 0x40) ? 1 : 0;
    src[MAP_BTN_BACK_R] = (p[9] & 0x80) ? 1 : 0;

    /* 两段式扳机（R2.5）：两个由扳机行程派生的源键，有真实的按下/松开态，
     * 走普通源键那条路（六种形态、全部输出类型都白捡）。 */
    src[MAP_BTN_L2_S2] = (uint8_t)(s2 & 1u);
    src[MAP_BTN_R2_S2] = (uint8_t)((s2 >> 1) & 1u);

    /* 虚拟组合键：两个源键都按住才算按下（源键自己照常输出）。源键必须
     * 是两个不同的物理键，cfg_derive() 已经把不合法的写成 0xFF。 */
    g_tlm_vk_mask = 0;
    for (int v = 0; v < MAP_VK_COUNT; v++) {
        uint8_t a = g_cfg.vk[v].a;
        uint8_t c = g_cfg.vk[v].b;
        src[MAP_BTN_V1 + v] = (MAP_BTN_IS_SRC(a) && MAP_BTN_IS_SRC(c) &&
                               src[a] && src[c]) ? 1 : 0;
        if (src[MAP_BTN_V1 + v])
            g_tlm_vk_mask |= (uint8_t)(1u << v);
    }

    /* 陀螺仪启用模式：常开直接放行，按住/开关看启用键（用源位而不是映射后的输出，
     * 这样启用键自己还能映射成别的东西）。开关模式判按下沿翻转锁存。 */
    const map_gyro_t *gy = &g_cfg.gyro;
    g_gyro_live = g_gyro_on && gy->target != MAP_GYRO_OFF &&
                  gy->enable_mode != MAP_GYRO_EN_OFF;
    if (g_gyro_live && gy->enable_btn < MAP_BTN_COUNT &&
        gy->enable_mode != MAP_GYRO_EN_ALWAYS) {
        uint8_t raw = src[gy->enable_btn] ? 1 : 0;
        if (gy->enable_mode == MAP_GYRO_EN_HOLD) {
            g_gyro_live = raw != 0;
        } else {
            if (raw && !g_gyro_btn_raw)
                g_gyro_latch = !g_gyro_latch;
            g_gyro_live = g_gyro_latch;
        }
        g_gyro_btn_raw = raw;
    } else {
        g_gyro_latch = false;
        g_gyro_btn_raw = 0;
    }
    if (!g_gyro_live)
        g_gyro_dx = g_gyro_dy = 0;   /* 没生效就别把上一帧的增量再发一次 */

    /* R2.7 Flick Stick：先把**物理**右摇杆抄下来（下面 tp_apply_stick / gyro_apply 都会改
     * p[2]/p[3]），门控和陀螺仪一模一样（常开 / 按住 / 开关），只是它永远吐鼠标增量。 */
    uint8_t fl_rx = p[2], fl_ry = p[3];
    const map_flick_t *fk = &g_cfg.flick;
    g_flick_live = g_flick_on;
    if (g_flick_live && fk->enable_mode != MAP_GYRO_EN_ALWAYS) {
        uint8_t raw = (fk->enable_btn < MAP_BTN_COUNT) ? (src[fk->enable_btn] ? 1 : 0) : 0;
        if (fk->enable_mode == MAP_GYRO_EN_HOLD) {
            g_flick_live = raw != 0;
        } else {
            if (raw && !g_flick_btn_raw)
                g_flick_latch = !g_flick_latch;
            g_flick_live = g_flick_latch;
        }
        g_flick_btn_raw = raw;
    } else {
        g_flick_latch = false;
        g_flick_btn_raw = false;
    }
    if (!g_flick_live) {
        g_flick_engaged = false;
        g_flick_acc = 0;
        g_flick_dx = g_flick_dy = 0;   /* 没生效就别把上一帧的增量再发一次 */
    }

    if (g_tp_on)
        tp_update(p, now);
    else {
        g_tp_mask = 0;
        g_tp_dx = g_tp_dy = 0;   /* 关掉触摸板后别把上一帧的增量一直发下去 */
    }

    /* L2/R2 analog bytes must follow the digital mask: a game reads the
     * analog value even when the digital bit is suppressed. */
    uint8_t l2_raw = src[MAP_BTN_L2];
    uint8_t r2_raw = src[MAP_BTN_R2];

    for (int b = 0; b < MAP_BTN_COUNT; b++) {
        btn_rt_t *st = &g_btn[b];
        uint8_t r = src[b] ? 1 : 0;

        /* R3.0「压穿时一段仍然有效」没勾（= release_s1）时：二段（派生键 39/40）一按下，
         * 就把这一侧的物理扳机（L2/R2）已经发出去的东西松开，并在二段按着期间**跳过**它的映射
         * ——这样「轻按=瞄准、压穿=开火」不会变成"瞄准一直按着"。
         * 注意：模拟量（p[4]/p[5]）不动，游戏那边照样看得到行程；二段松开后**需要松手再按一次**
         * 才会重新触发一段（不做"自动恢复"——那要么误触发长按、要么状态机更绕，见界面上的说明）。 */
        if (b == MAP_BTN_L2 || b == MAP_BTN_R2) {
            uint8_t db = (b == MAP_BTN_L2) ? MAP_BTN_L2_S2 : MAP_BTN_R2_S2;
            if (ds5map_s2t_release_s1() && src[db]) {
                if (!g_s1_suppressed[b == MAP_BTN_L2 ? 0 : 1]) {
                    g_s1_suppressed[b == MAP_BTN_L2 ? 0 : 1] = true;
                    bind_release_btn((uint8_t)b);
                }
                st->raw = r;
                continue;                      /* 二段按着：一段这一帧不参与映射 */
            }
            g_s1_suppressed[b == MAP_BTN_L2 ? 0 : 1] = false;
        }

        if (MAP_BTN_IS_VK(b) || MAP_BTN_IS_S2T(b)) {
            /* 虚拟组合键 / 扳机二段派生键：**只有单击一种形态**（用户 2026-10-17 定的：二段只要
             * 一个单击输出），按下沿打一次就完了，不进双击/三击/长按那套判定窗口；
             * 它们也不是报文里的键，没有「本体」输出。 */
            if (r && !st->raw) {
                fire_action((uint8_t)b, FORM_TAP, now);
            } else if (!r && st->raw) {
                /* ★ R3.4 修（用户实测：L2 二段→R2，松手后 R2 一直按着）：
                 * fire_action() 建的是**按住到松手**的动作，松手时必须调 act_release_btn()
                 * 把这条动作丢掉，否则它输出的那一位（这里是 R2）就永远停在"按下"。
                 * 以前这里只调 bind_release_btn()（那个函数只管切层 binds），所以卡死；
                 * 切回普通模式也救不回来 —— 卡住的是**运行态**，不是配置。 */
                bind_release_btn((uint8_t)b);      /* 仍保留：万一它还兼着切层的活 */
                act_release_btn((uint8_t)b);
            }
            st->raw = r;
            continue;
        }

        if (!g_managed[b]) {
            st->raw = r;
            continue;
        }

        if (r && !st->raw) {
            /* press edge — count it into the current tap sequence */
            if (st->taps == 0) {
                st->taps = 1;
                st->seq_ms = now;
            } else if (st->taps == 1 &&
                       (now - st->release_ms) <= g_cfg.timing.dtap_ms) {
                st->taps = 2;
            } else if (st->taps >= 2 &&
                       (now - st->seq_ms) <= g_cfg.timing.ttap_ms) {
                st->taps = 3;
            } else {
                st->taps = 1;
                st->seq_ms = now;
            }
            st->press_ms = now;
            st->hold_fired = 0;

            fire_action((uint8_t)b, FORM_PRESS, now);
            fire_action((uint8_t)b, FORM_TAP, now);      /* always, immediately */
            if (st->taps == 2)
                fire_action((uint8_t)b, FORM_DTAP, now);
            else if (st->taps >= 3)
                fire_action((uint8_t)b, FORM_TTAP, now);

            /* 「原键 + 开关模式」：单击槽是原键又勾了开关时，按下沿翻转锁存。
             * 覆盖层把这个键接管时（slot_lookup 非 NULL）不翻，锁存原样保留。 */
            const map_slot_t *mts = &g_cfg.slots[MAP_LAYER_ID_MAIN][b][FORM_TAP];
            if (!slot_lookup((uint8_t)b, FORM_TAP) &&
                (mts->flags & MAP_FLAG_TOGGLE))
                st->ntoggle_on ^= 1;
        } else if (!r && st->raw) {
            /* release edge */
            fire_action((uint8_t)b, FORM_RELEASE, now);
            bind_release_btn((uint8_t)b);
            act_release_btn((uint8_t)b);
            st->release_ms = now;
            st->hold_fired = 0;
        }
        st->raw = r;
    }

    /* output mask */
    uint64_t mask = g_macro_pad | ((uint64_t)g_tp_mask << TP_MASK_SHIFT);
    /* 战备宏敲的十字键：4 bit 正好和 MAP_BTN_DPAD_UP..RIGHT 连续对齐 */
    mask |= (uint64_t)g_strat_pad << MAP_BTN_DPAD_UP;
    for (int i = 0; i < ACT_MAX; i++) {
        const act_t *a = &g_acts[i];
        if (a->used && a->out == OUT_PAD && a->val < MAP_BTN_COUNT && act_on(a))
            mask |= (uint64_t)1U << a->val;
    }
    for (int b = 0; b < MAP_BTN_COUNT; b++) {
        bool on;
        btn_rt_t *st = &g_btn[b];
        bool derived = MAP_BTN_IS_S2T(b);     /* R2.5：派生键没有本体输出（见 ds5map.h） */
        if (MAP_BTN_IS_VK(b))
            continue;                        /* 虚拟键不进原生输出掩码 */
        if (derived)
            on = false;                      /* 两段式扳机键：动作由动作表发，原生位没人认 */
        else if (g_managed[b])
            on = (st->raw != 0) && native_on((uint8_t)b);
        else
            on = (src[b] != 0);

        /* 「原键 + 连发/开关」：单击槽是原键又勾了这两个位时，原生输出按
         * 下面的规则合成。slot_lookup 对原键返回 NULL，所以这里直接看主层
         * 单击槽的 flags。R2/L2 的模拟量字节在下面跟随掩码位，所以扳机的
         * 连发/锁存也是真生效。
         *   连发＝按 50% 占空比翻转（与动作连发同一节奏）；
         *   开关＝按下沿翻转锁存（翻转在按下沿做），锁存期间输出保持；
         *   两个都勾＝按一下开始自动连发，再按一下停。
         * 派生键（39/40）没有原生输出，所以这两个位对它们不生效 —— 不然定时器
         * 会为了一个永远没人看的掩码位一直跑着。 */
        const map_slot_t *ms = &g_cfg.slots[MAP_LAYER_ID_MAIN][b][FORM_TAP];
        bool n_slot = !derived && (slot_lookup((uint8_t)b, FORM_TAP) == NULL);
        bool n_tgl = n_slot && (ms->flags & MAP_FLAG_TOGGLE);
        bool n_tur = n_slot && (ms->flags & MAP_FLAG_TURBO);
        if (n_tgl)
            on = st->ntoggle_on != 0;
        if (n_tur && on) {
            uint32_t half = turbo_period_ms(ms->param) / 2;
            if (!st->nturbo_run) {
                st->nturbo_run = 1;
                st->nturbo_state = 1;
                st->nturbo_next = now + half;
            } else if (now >= st->nturbo_next) {
                st->nturbo_state ^= 1;
                st->nturbo_next = now + half;
            }
            on = st->nturbo_state != 0;
        } else {
            st->nturbo_run = 0;
        }
        if (on)
            mask |= (uint64_t)1U << b;
    }

    payload_rebuild_buttons(p, mask);

    /* R3.7（用户实测：没勾「压穿时一段仍然有效」，输出里一段还是按着）：
     * 抑制以前只跳过了**映射**，但扳机本身是**原生透传**（mask 里那位一直在），
     * 所以数字位照样按下、模拟量也不归零。这里在输出阶段把这一侧的物理扳机摘掉 ——
     * 数字位和模拟量一起松开，勾上时则完全不动（走原样透传）。 */
    /* R3.12：**同时清报文里的数字位** —— payload_rebuild_buttons() 在本函数前面就把
     * p[8] 写好了（L2 = 0x04 / R2 = 0x08，见 BTN_LOC 上方的布局注释），只清 mask 不够 ✗，
     * 否则模拟量已是 0、报文位却还按着（测试页手柄图就会继续亮着）。 */
    if (g_s1_suppressed[0]) { mask &= ~(1u << MAP_BTN_L2); p[8] &= (uint8_t)~0x04; }
    if (g_s1_suppressed[1]) { mask &= ~(1u << MAP_BTN_R2); p[8] &= (uint8_t)~0x08; }
    uint8_t l2_out = (mask >> MAP_BTN_L2) & 1u;
    uint8_t r2_out = (mask >> MAP_BTN_R2) & 1u;
    if (l2_raw != l2_out)
        p[4] = l2_out ? 0xFF : 0x00;
    if (r2_raw != r2_out)
        p[5] = r2_out ? 0xFF : 0x00;
    g_fast_l2 = (l2_raw != l2_out) ? (l2_out ? 1 : 2) : 0;   /* S36 */
    g_fast_r2 = (r2_raw != r2_out) ? (r2_out ? 1 : 2) : 0;

    /* 摇杆响应曲线已按用户要求【彻底移除】（2026-10-11，整块连数据一起删）。
     * 原逻辑：if (!g_curve_identity) p[i] = g_cfg.curves.lut[i][p[i]];
     * 隐患：只要表里有一个字节不对，"整条轴"就会被按错误的表映射。实测 LX 的表
     * 只剩前 121 项（分块写入被 cfg_derive 中途覆盖留下的残值），lut[127] 变成 0，
     * 于是"摇杆居中 → 输出最左"，表现出来就像摇杆坏了。
     * 现在摇杆值原样通过，配置里也没有曲线区了（MAP_REG_CURVES 已摘除）。 */

    /* 触摸板摇杆目标覆盖物理摇杆，然后陀螺仪再往上叠 */
    if (g_tp_on)
        tp_apply_stick(p);

    /* 陀螺仪在最后叠加（输出侧），鼠标增量经 g_gyro_dx/dy 交给 synth_mouse。
     * R2.0：这里才是平时每帧真正的陀螺仪开销，计进遥测的「陀螺仪」分项（它包含在「引擎」
     * 总耗时里）。以前只有静置校准那 3 秒计时，日志页那栏平时恒为 0.0 µs / 0 次，
     * 被误读成"陀螺仪不耗 CPU"。 */
    if (g_gyro_live) {
        uint32_t gc0 = perf_cyc();
        gyro_apply(p);
        perf_gyro_cyc(perf_cyc() - gc0);
    }

    /* R2.7 Flick Stick 放在陀螺仪**之后**：它要把右摇杆写回 0x80，得是最后一个动摇杆的。
     * （开销计进"引擎"总耗时；没单独占一个遥测分项，遥测分项就那几栏。） */
    if (g_flick_live)
        flick_apply(fl_rx, fl_ry, p);

    memcpy(g_tlm_out, p, INPUT_LEN);
    g_tlm_seq++;

    int mdx = g_gyro_dx + g_tp_dx + g_flick_dx;
    int mdy = g_gyro_dy + g_tp_dy + g_flick_dy;
    if (mdx > 127) mdx = 127; else if (mdx < -127) mdx = -127;
    if (mdy > 127) mdy = 127; else if (mdy < -127) mdy = -127;
    tlm_acc_add(0, g_tp_dx);
    tlm_acc_add(1, g_tp_dy);
    tlm_acc_add(2, g_gyro_dx);
    tlm_acc_add(3, g_gyro_dy);
    tlm_acc_add(4, mdx);
    tlm_acc_add(5, mdy);

    synth_mouse(mdx, mdy, g_wheel_pending);
    g_wheel_pending = 0;
    synth_kbd();
    synth_media();

    /* S36：记下这一帧的输入与输出供下一帧复用；判定现在有没有时间相关的东西在跑 */
    memcpy(g_fast_in, fast_in, sizeof(g_fast_in));
    g_fast_s2 = s2;                        /* R2.5：二段状态也是快速路径的比较键 */
    g_fast_layer = t2_layer;               /* R3.25：算这个二段状态时用的是哪一层 */
    g_fast_out[0] = p[7];
    g_fast_out[1] = p[8];
    g_fast_out[2] = p[9];
    g_fast_ok = !tick_needed() && !g_gyro_live && !g_flick_live && !g_tp_on && !g_tp_stick &&
                g_last_mouse_btns == 0 && g_wheel_pending == 0 && !g_media_pending;
    tick_arm_if_needed();

    MAP_UNLOCK();
}

/* R2.4：遥测缓冲里「没有手柄」的样子 = 回中帧：摇杆 0x80、扳机 0、十字键 8（不按）、
 * 两个触点抬起（bit7 = 没摸）。以前开机和断开后是全 0，按 DS5 格式解码就是「十字键↑、
 * 两根摇杆推到左上、两根手指按在触摸板 (0,0)」，测试页会照着画出来。 */
static void tlm_fill_neutral(void)
{
    memset(g_tlm_raw, 0, INPUT_LEN);
    g_tlm_raw[0] = g_tlm_raw[1] = g_tlm_raw[2] = g_tlm_raw[3] = 0x80;
    g_tlm_raw[7] = 0x08;
    g_tlm_raw[32] = g_tlm_raw[36] = 0x80;
    memcpy(g_tlm_out, g_tlm_raw, INPUT_LEN);
    g_tlm_seq++;
}

void ds5map_on_bt_disconnect(void)
{
    MAP_LOCK();

    g_wheel_pending = 0;
    g_gyro_acc[0] = g_gyro_acc[1] = 0;
    g_gyro_dx = g_gyro_dy = 0;
    g_gyro_live = false;
    g_gyro_latch = false;
    g_gyro_btn_raw = 0;
    g_s2t_latch = 0;       /* R2.5：断开后二段源键回到「松开」，别留一个假的按住 */
    g_fast_s2 = 0;
    g_fast_layer = MAP_LAYER_ID_MAIN;   /* R3.25：层也跟着回主层 */
    flick_reset();         /* R2.7：断开后 flick 的接管/基线/余量全清，别留半个朝向 */
    gyro_reset();          /* R1.3：只取消进行中的静置校准；存下来的零偏保留 */
    g_tp_mask = 0;
    g_tp_dx = g_tp_dy = 0;
    g_tp_stick = false;
    g_tp_finger = 0;
    g_tp_last_x = g_tp_last_y = -1;
    g_tp_pinch_dist = -1;
    g_tp_pinch_acc = 0;
    g_tlm_vk_mask = 0;
    memset(g_tlm_acc, 0, sizeof(g_tlm_acc));
    acts_clear();
    macro_stop();
    strat_stop();
    memset(g_btn, 0, sizeof(g_btn));
    memset(g_macro_kbd, 0, sizeof(g_macro_kbd));
    g_macro_pad = 0;
    g_macro_mouse = 0;
    g_layer_mask = 0;
    g_bind_hold_act = 0;
    memset(g_stack, 0xFF, sizeof(g_stack));

    /* R2.4：键盘、媒体、鼠标的「全部松开」进队发送，一份都不会被挤掉。以前三份共用一个
     * 端点、谁赶上端点忙谁就被跳过（键盘那份一发，端点就忙了，鼠标/媒体那两份必丢），
     * 下面却照样把 g_last_* 清零、以后不再补 → 电脑上的键一直按着、自动连发。 */
    usb_gamepad_kbd_release_all();
    tlm_fill_neutral();
    g_last_media = 0;
    g_media_pending = false;
    memset(g_last_kbd, 0, sizeof(g_last_kbd));
    g_last_mouse_btns = 0;
    g_fast_ok = false;      /* S36 */
    g_force_full = true;

    MAP_UNLOCK();
}

/* ==================================================================== */
/* Config load / save                                                   */
/* ==================================================================== */

static void cfg_defaults(void)
{
    static const char *names[MAP_LAYERS] = {
        "默认层", "覆盖层 1", "覆盖层 2", "覆盖层 3", "覆盖层 4"
    };
    memset(&g_cfg, 0, sizeof(g_cfg));
    g_cfg.timing.hold_ms = DEF_HOLD_MS;
    g_cfg.timing.dtap_ms = DEF_DTAP_MS;
    g_cfg.timing.ttap_ms = DEF_TTAP_MS;
    g_cfg.timing.tap_pulse_ms = DEF_PULSE_MS;
    for (int i = 0; i < MAP_LAYERS; i++) {
        strncpy(g_cfg.layer_names[i], names[i], 15);
        g_cfg.layer_names[i][15] = '\0';
    }
    for (int i = 0; i < MAP_SHIFT_LAYERS; i++)
        g_cfg.binds[i].src_btn = 0xFF;
    /* 摇杆曲线已删除：配置里没有这一块了 */
    /* 陀螺仪默认（2026-10-11 用户逐条定下来的，配置回落默认值后也还是这套手感）：
     *   目标 = 右摇杆、启用 = 常开、水平/垂直 = 不勾反向（方向符号定死在 gyro.c）
     *   水平轴 = 偏摆 yaw、垂直轴 = 俯仰 pitch
     *   **死区 0**（用户要求；静止噪声会直接进摇杆，真有漂移就往上加一点）
     *   **灵敏度 1024 = 64×**（用户："64 倍才是比较常规的灵敏度"；网页滑条上限放到 128×）
     * 换算：每帧叠加到摇杆 = 角速度(°/s) × sens ÷ 512（Q4，16＝1.00×）。 */
    g_cfg.gyro.target = MAP_GYRO_RSTICK;
    g_cfg.gyro.gravity = MAP_GYRO_GRAV_LOCAL;   /* 只有本体轴这一种模式 */
    g_cfg.gyro.axis_h = MAP_GYRO_AXIS_YAW;
    g_cfg.gyro.axis_v = MAP_GYRO_AXIS_PITCH;
    /* 方向符号已经定死在 gyro.c 的 GYRO_BASE_SIGN_*（用户要"不勾反向也正确"），
     * 所以这里默认**不勾反向**；勾上＝在那之上再翻一次。 */
    g_cfg.gyro.inv_h = 0;
    g_cfg.gyro.inv_v = 0;
    g_cfg.gyro.deadzone = 0;
    g_cfg.gyro.enable_mode = MAP_GYRO_EN_ALWAYS;
    g_cfg.gyro.enable_btn = 0xFF;
    g_cfg.gyro.sens_h = 1024;
    g_cfg.gyro.sens_v = 1024;
    /* 触摸板默认原生透传；双指开合没配动作 */
    g_cfg.tp.single = MAP_TP_NATIVE;
    g_cfg.tp.pinch_out.out = OUT_NONE;
    g_cfg.tp.pinch_in.out = OUT_NONE;
    g_cfg.tp.sens = TP_MOUSE_SENS_DEF;
    g_cfg.tp.pinch_px = TP_PINCH_DEF_PX;
    /* 虚拟组合键默认没配源键；战备表默认全是空槽（len 0＝memset 已经给了） */
    for (int i = 0; i < MAP_VK_COUNT; i++)
        g_cfg.vk[i].a = g_cfg.vk[i].b = 0xFF;

    /* R2.5 两段式扳机：默认**关**（两侧阈值 0）、段落感也不造 —— 用户没开之前行为一字不变 */
    g_cfg.s2t.thresh_l = 0;
    g_cfg.s2t.thresh_r = 0;
    g_cfg.s2t.hyst = 8;
    g_cfg.s2t.effect_mode = 0;      /* 默认不造效果（＝关闭） */
    g_cfg.s2t.strength = 0x66;      /* 左 6 / 右 6（低 4 位左、高 4 位右；硬件 1..8 档） */
    g_cfg.s2t.kind = 0x05;          /* 左「扳机」/ 右「扳机」（低 2 位左、bit2-3 右） */
    g_cfg.s2t.end_l = 0;            /* 0 = 没设 → 起始档+1（只对「扳机」有效；0..255 行程值） */
    g_cfg.s2t.end_r = 0;
    g_cfg.s2t.freq_l = 0;           /* 0 = 默认 10 Hz（只对「自动扳机」有效；存的是 Hz） */
    g_cfg.s2t.freq_r = 0;
    g_cfg.s2t.trig_mode = 0;        /* 主层的「两段模式」位（网页写，固件按层取用） */
    g_cfg.s2t.flags = 0;            /* R3.0：游戏优先 + 压穿时一段仍然有效（两个默认都是"是"） */
    g_cfg.s2t.bound_l = 0;          /* R3.6：主层分界点 0 = 没设过 → 跟着手感起始位置（开箱默认） */
    g_cfg.s2t.bound_r = 0;
    /* R3.25：覆盖层 1..4 那一整套（两段位/分界点 t2_*，自适应扳机参数 fx[][]）都在结构体末尾，
     * 上面的 memset(&g_cfg,0,…) 已经把它们清零 —— 覆盖层默认「普通模式 + 分界点没设过 + 效果关闭」。 */

    /* R2.5 落位、R2.6/R2.7 才上线的两块：默认全关（零填充本来也就是关） */
    g_cfg.flick.enable_mode = MAP_GYRO_EN_OFF;
    g_cfg.flick.enable_btn = 0xFF;
    g_cfg.flick.r_on = 60;
    g_cfg.flick.r_off = 50;
    g_cfg.flick.y_mode = 0;
    g_cfg.flick.sens = 1024;
    g_cfg.gest.src_btn = 0xFF;
    g_cfg.gest.kind_mask = 0x0F;
    g_cfg.gest.thresh = 300;
    for (int i = 0; i < 4; i++)
        g_cfg.gest.act[i].out = OUT_NONE;
}

static void cfg_derive(void)
{
    g_force_full = true;   /* S36 */
    g_any_managed = false;
    g_any_mapping = false;
    g_has_kbd = false;
    g_has_mouse = false;
    memset(g_managed, 0, sizeof(g_managed));

    for (int l = 0; l < MAP_LAYERS; l++) {
        for (int b = 0; b < MAP_BTN_COUNT; b++) {
            for (int f = 0; f < MAP_FORM_COUNT; f++) {
                const map_slot_t *s = &g_cfg.slots[l][b][f];
                if (s->out == OUT_SELF) {
                    /* 「原键 + 连发/开关」：原生输出也要按占空比翻转 / 锁存，
                     * 这个键必须走完整映射路径（否则快速透传会让这两个位被
                     * 无视）。只认主层单击槽——覆盖层的 OUT_SELF 槽按既有
                     * 语义＝本层没配置。 */
                    if (l == MAP_LAYER_ID_MAIN && f == FORM_TAP &&
                        (s->flags & (MAP_FLAG_TURBO | MAP_FLAG_TOGGLE))) {
                        g_managed[b] = true;
                        g_any_managed = true;
                    }
                    continue;
                }
                g_any_mapping = true;
                g_managed[b] = true;
                g_any_managed = true;
                if (s->out == OUT_KBD || s->out == OUT_MEDIA)
                    g_has_kbd = true;          /* 媒体键走键鼠接口（报告 ID 2） */
                if (s->out == OUT_MOUSE)
                    g_has_mouse = true;
                if (s->out == OUT_MACRO) {
                    /* a macro may drive keyboard and mouse */
                    g_has_kbd = true;
                    g_has_mouse = true;
                }
            }
        }
    }
    for (int i = 0; i < MAP_SHIFT_LAYERS; i++) {
        uint8_t b = g_cfg.binds[i].src_btn;
        if (b < MAP_BTN_COUNT) {
            g_managed[b] = true;
            g_any_managed = true;
        }
    }

    /* 摇杆曲线健全性检查（2026-10-10 修）。
     *
     * 症状：手柄能连、按键正常，但两个摇杆恒为 0（原始报文 p[0..3]=80 81 80 81
     * 是好的，发出去却成了 00 00 00 00）。
     * 根因：media 分区 cfg.bin 的曲线字段损坏/未写入时是**全 0**，而这里只判断
     * "是否恒等映射"，于是 g_curve_identity=false → 每帧都走完整映射路径 →
     * 执行 p[0]=lut[0][p[0]] 把摇杆查成 0；同时每帧重写整个报文，扰乱了蓝牙/USB
     * 数据流，表现成"手柄连不上"。
     *
     * 修法：把明显损坏的曲线（整轴全 0）恢复成恒等映射。 */
    /* 已删除（2026-10-11，摇杆曲线彻底移除）：这里原来做两件事 ——
     * ① 把损坏的（整轴全 0）LUT 恢复成恒等映射；
     * ② 判断 4 条轴是否恒等，缓存到 g_curve_identity 给 ds5map_process 走捷径。
     * 曲线数据本身已从配置结构里删掉，两段都没有意义了。 */

    /* clamp timings */
    if (g_cfg.timing.hold_ms < 50) g_cfg.timing.hold_ms = DEF_HOLD_MS;
    if (g_cfg.timing.dtap_ms < 50) g_cfg.timing.dtap_ms = DEF_DTAP_MS;
    if (g_cfg.timing.ttap_ms < 50) g_cfg.timing.ttap_ms = DEF_TTAP_MS;
    if (g_cfg.timing.tap_pulse_ms < 10) g_cfg.timing.tap_pulse_ms = DEF_PULSE_MS;

    /* gyro: 清洗全局那一份。g_gyro_on 决定要不要挂鼠标接口、要不要每帧喂融合
     * （没启用时靠 g_gyro_probe 让测试页把融合叫醒）。 */
    g_gyro_on = false;
    {
        map_gyro_t *gy = &g_cfg.gyro;
        if (gy->target > MAP_GYRO_RSTICK) gy->target = MAP_GYRO_MOUSE;
        /* 「跟随重力」已按用户要求**彻底删除**（2026-10-11）：那套要用加速度
         * 反算重力方向再做 player-space 解算，代码量最大也最容易出问题。
         * gravity 字节还留在 map_gyro_t 里（网页的 14 字节区固定），但无论主机写
         * 什么，这里都强制成本体轴：陀螺仪直接取三条轴的角速度。 */
        gy->gravity = MAP_GYRO_GRAV_LOCAL;
        if (gy->axis_h > MAP_GYRO_AXIS_ROLL) gy->axis_h = MAP_GYRO_AXIS_YAW;
        if (gy->axis_v > MAP_GYRO_AXIS_ROLL) gy->axis_v = MAP_GYRO_AXIS_PITCH;
        if (gy->sens_h == 0) gy->sens_h = 48;
        if (gy->sens_v == 0) gy->sens_v = 48;
        if (gy->enable_mode > MAP_GYRO_EN_TOGGLE) gy->enable_mode = MAP_GYRO_EN_OFF;
        if (gy->enable_btn >= MAP_BTN_COUNT) gy->enable_btn = 0xFF;
        /* 按住/开关却没绑键 ＝ 永远开不了，直接当关闭 */
        if (gy->enable_mode != MAP_GYRO_EN_ALWAYS &&
            gy->enable_mode != MAP_GYRO_EN_OFF &&
            gy->enable_btn == 0xFF)
            gy->enable_mode = MAP_GYRO_EN_OFF;
        if (gy->enable_mode != MAP_GYRO_EN_OFF && gy->target != MAP_GYRO_OFF) {
            g_gyro_on = true;
            if (gy->target == MAP_GYRO_MOUSE)
                g_has_mouse = true;
        }
    }
    gyro_sync_cfg();

    /* R2.7 Flick Stick：配了启用模式（而且启用键有效、清洗已经保证）就算"会用" ——
     * 快速路径/捷径靠它兜底；它永远吐鼠标增量，所以只要有它就得把鼠标接口算上。 */
    g_flick_on = (g_cfg.flick.enable_mode != MAP_GYRO_EN_OFF);
    if (g_flick_on)
        g_has_mouse = true;

    /* touchpad: sanitize + interfaces */
    if (g_cfg.tp.single > MAP_TP_DPAD) g_cfg.tp.single = MAP_TP_NATIVE;
    if (g_cfg.tp.sens > TP_MOUSE_SENS_MAX) g_cfg.tp.sens = TP_MOUSE_SENS_DEF;
    if (g_cfg.tp.pinch_px < TP_PINCH_MIN_PX || g_cfg.tp.pinch_px > TP_PINCH_MAX_PX)
        g_cfg.tp.pinch_px = TP_PINCH_DEF_PX;
    for (int i = 0; i < 2; i++) {
        map_slot_t *s = i ? &g_cfg.tp.pinch_in : &g_cfg.tp.pinch_out;
        if (s->out == OUT_SELF)
            s->out = OUT_NONE;               /* 开合动作没有「本体」可言 */
        /* 脉冲动作：开关/连发语义不适用，去掉免得动作表不按脉冲过期 */
        s->flags &= (uint8_t)~(MAP_FLAG_TOGGLE | MAP_FLAG_TURBO);
        if (s->out == OUT_NONE)
            continue;
        g_any_mapping = true;
        if (s->out == OUT_KBD || s->out == OUT_MEDIA)
            g_has_kbd = true;
        if (s->out == OUT_MOUSE)
            g_has_mouse = true;
        if (s->out == OUT_MACRO) {
            g_has_kbd = true;
            g_has_mouse = true;
        }
    }
    g_tp_on = (g_cfg.tp.single != MAP_TP_NATIVE) ||
              (g_cfg.tp.pinch_out.out > OUT_NONE) ||
              (g_cfg.tp.pinch_in.out > OUT_NONE);
    if (g_tp_on && g_cfg.tp.single == MAP_TP_MOUSE)
        g_has_mouse = true;

    /* 虚拟组合键：源键必须是两个不同的物理键（不能拿虚拟键当源，否则会
     * 互相套娃）。不合法的直接清成 0xFF＝未配置。 */
    for (int v = 0; v < MAP_VK_COUNT; v++) {
        uint8_t a = g_cfg.vk[v].a;
        uint8_t b = g_cfg.vk[v].b;
        if (!MAP_BTN_IS_SRC(a) || !MAP_BTN_IS_SRC(b) || a == b) {
            g_cfg.vk[v].a = 0xFF;
            g_cfg.vk[v].b = 0xFF;
        }
    }

    /* 战备序列：长度和方向都夹一遍，坏数据当空槽 */
    for (int i = 0; i < MAP_STRAT_COUNT; i++) {
        map_strat_t *s = &g_cfg.strats[i];
        if (s->len > MAP_STRAT_MAX_LEN) {
            s->len = 0;
            continue;
        }
        for (int d = 0; d < s->len; d++) {
            if (s->dir[d] > 3) { s->len = 0; break; }
        }
    }

    /* R2.6 扳机效果：起始位置 0 = 这一侧关；力度硬件只有 8 档；结束位置/频率 0 = 用默认；
     * 这一侧没开（起始位置 0）就没有效果可言，效果位跟着清（网页怎么填都不生效）。 */
    {
        map_s2t_t *s2 = &g_cfg.s2t;
        if (s2->hyst == 0 || s2->hyst > 63) s2->hyst = 8;
        s2->effect_mode &= 0x03;
        s2t_set_str(s2, 0, (uint8_t)(s2->strength & 0x0F));      /* 左力度 1..8 */
        s2t_set_str(s2, 1, (uint8_t)(s2->strength >> 4));        /* 右力度 1..8 */
        s2->kind = (uint8_t)(s2t_kind(s2, 0) | (uint8_t)(s2t_kind(s2, 1) << 2));
        if (s2->kind > 0x0F) s2->kind &= 0x0F;
        s2->trig_mode &= 0x03;                  /* 主层那份 */
        for (int i = 0; i < MAP_SHIFT_LAYERS; i++)
            s2->t2_mode[i] &= 0x03;             /* R3.25：覆盖层 1..4 那份也只认低两位 */
        /* R3.25（第二轮）：覆盖层那套「自适应扳机」参数也过一遍取值清洗 —— 别让脏字节在
         * 配置里躺着（网页/手改的配置写进来的任何东西都不该让编码器读到离谱的值）。 */
        for (int i = 0; i < MAP_SHIFT_LAYERS; i++) {
            uint8_t *f = s2->fx[i];
            uint8_t sl, sr, kl, kr;
            f[S2T_FX_EFFECT] &= 0x03;
            f[S2T_FX_FLAGS]  &= 0x03;
            sl = (uint8_t)(f[S2T_FX_STR] & 0x0F);          /* 力度：低半左 / 高半右 */
            sr = (uint8_t)((f[S2T_FX_STR] >> 4) & 0x0F);
            if (sl > 8) sl = 8;                            /* 0 = 未设（读的时候取默认 6） */
            if (sr > 8) sr = 8;
            f[S2T_FX_STR] = (uint8_t)(sl | (uint8_t)(sr << 4));
            kl = (uint8_t)(f[S2T_FX_KIND] & 0x03);         /* 类型：低 2 位左 / bit2-3 右 */
            kr = (uint8_t)((f[S2T_FX_KIND] >> 2) & 0x03);
            if (kl > MAP_S2T_KIND_AUTO) kl = MAP_S2T_KIND_WEAPON;   /* 和 s2t_kind() 的兜底一致 */
            if (kr > MAP_S2T_KIND_AUTO) kr = MAP_S2T_KIND_WEAPON;
            f[S2T_FX_KIND] = (uint8_t)(kl | (uint8_t)(kr << 2));
        }
        s2->flags &= 0x03;             /* R3.0：只认这两个行为位 */
        /* 两个派生源键只有被槽位/层绑定真正引用时才需要算（上面的槽位扫描已经置好
         * g_managed 与 g_any_managed）；没人用就把锁存清掉，别留一个假的「按住」。 */
        g_s2t_used = g_managed[MAP_BTN_L2_S2] || g_managed[MAP_BTN_R2_S2];
        if (!g_s2t_used) {
            g_s2t_latch = 0;
            g_fast_s2 = 0;
        }
    }

    /* R2.5 落位、R2.6/R2.7 上线的两块：先做取值清洗，别让脏字节在配置里躺着
     *（网页/老配置写进来的任何东西都不该让以后上线的逻辑读到离谱的值）。 */
    {
        map_flick_t *fl = &g_cfg.flick;
        if (fl->enable_mode > MAP_GYRO_EN_TOGGLE) fl->enable_mode = MAP_GYRO_EN_OFF;
        if (fl->enable_btn >= MAP_BTN_COUNT) fl->enable_btn = 0xFF;
        if (fl->enable_mode != MAP_GYRO_EN_ALWAYS && fl->enable_mode != MAP_GYRO_EN_OFF &&
            fl->enable_btn == 0xFF)
            fl->enable_mode = MAP_GYRO_EN_OFF;
        if (fl->r_on < 5) fl->r_on = 60;
        if (fl->r_on > 100) fl->r_on = 100;
        if (fl->r_off >= fl->r_on)
            fl->r_off = (uint8_t)(fl->r_on > 10 ? fl->r_on - 10 : 0);
        if (fl->y_mode > 2) fl->y_mode = 0;
        if (fl->sens == 0) fl->sens = 1024;
    }
    {
        map_gest_t *gs = &g_cfg.gest;
        if (gs->src_btn >= MAP_BTN_COUNT) gs->src_btn = 0xFF;
        gs->kind_mask &= 0x0F;
        if (gs->thresh < 20) gs->thresh = 300;
        if (gs->thresh > 2000) gs->thresh = 2000;
        for (int i = 0; i < 4; i++) {
            map_slot_t *s = &gs->act[i];
            if (s->out == OUT_SELF) s->out = OUT_NONE;   /* 脉冲动作没有「本体」可言 */
            s->flags &= (uint8_t)~(MAP_FLAG_TOGGLE | MAP_FLAG_TURBO);
        }
    }
}

/* Read n body bytes + the trailing crc16 and report whether both are sane. */
static bool cfg_read_checked(lfs_t *fs, lfs_file_t *f, void *body, size_t n)
{
    lfs_ssize_t rd = lfs_file_read(fs, f, body, n);
    uint8_t cb[2] = { 0, 0 };
    lfs_file_read(fs, f, cb, 2);
    uint16_t stored = (uint16_t)(cb[0] | (cb[1] << 8));
    return rd == (lfs_ssize_t)n &&
           crc16_ccitt((const uint8_t *)body, (uint32_t)n) == stored;
}

/* 已删除（2026-10-11，用户要求不再兼容老配置）：cfg_read_v8_gyro()、
 * cfg_fix_layer_names()、cfg_upgrade_v3()/cfg_upgrade_v6() 以及
 * ds5map_body_v3_t / ds5map_body_v6_t 全部移除。读到版本号不匹配的旧配置时，
 * ds5map_load() 什么都不读，直接沿用 cfg_defaults() 的内容。 */

void ds5map_load(void)
{
    cfg_defaults();

    lfs_t *fs = macro_store_lfs();
    if (fs) {
        lfs_file_t f;
        uint8_t hdr[8];
        if (lfs_file_open(fs, &f, DS5MAP_CFG_FILE, LFS_O_RDONLY) == 0) {
            lfs_ssize_t rd = lfs_file_read(fs, &f, hdr, sizeof(hdr));
            uint32_t magic = (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8) |
                             ((uint32_t)hdr[2] << 16) | ((uint32_t)hdr[3] << 24);
            uint16_t ver = (uint16_t)(hdr[4] | (hdr[5] << 8));
            if (rd == (lfs_ssize_t)sizeof(hdr) && magic == DS5MAP_CFG_MAGIC) {
                if (ver == DS5MAP_CFG_VER) {
                    /* Read straight into g_cfg. ds5map_init() is called from
                     * main(), which runs on the 4 KB boot stack (__StackTop in
                     * the linker script) — a local ds5map_body_t underflows it
                     * into the tail of .bss. */
                    if (cfg_read_checked(fs, &f, &g_cfg, sizeof(g_cfg))) {
                        LOG_INF("[MAP] config loaded (%u bytes)\n", (unsigned)sizeof(g_cfg));
                    } else {
                        LOG_WRN("[MAP] config CRC mismatch, using defaults\n");
                        cfg_defaults();
                    }
                } else {
                    /* 不兼容的旧版本（2026-10-11 起不再做任何迁移）。
                     * 安全性：这里**一个字节都不读**，g_cfg 保持上面 cfg_defaults()
                     * 填好的内容 → 不越界、不循环，直接从头开始。 */
                    LOG_WRN("[MAP] config version %u != %u, using defaults\n",
                            (unsigned)ver, (unsigned)DS5MAP_CFG_VER);
                }
            }
            lfs_file_close(fs, &f);
        } else {
            LOG_INF("[MAP] no config file, using defaults\n");
        }
    }

    cfg_derive();
    g_cfg_dirty = false;
    ds5map_on_bt_disconnect();  /* reset runtime state */
}

/* R1.3：静置校准出来的零偏单独存一个小文件，和按键映射的 cfg.bin 在同一个 LittleFS 上。
 * 不塞进 cfg.bin：那样要升 DS5MAP_CFG_VER，用户的映射会被整份清回默认。 */
#define GCAL_FILE   "gcal.bin"
#define GCAL_MAGIC  0x4C414347u   /* "GCAL" */
#define GCAL_VER    1

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t ver;
    uint16_t rsv;
    float    bias[3];      /* 度/秒 */
    uint16_t crc;          /* crc16_ccitt(bias) */
} gcal_file_t;

static void gcal_save(void)
{
    lfs_t *fs = macro_store_lfs();
    if (!fs)
        return;
    gcal_file_t g;
    memset(&g, 0, sizeof(g));
    g.magic = GCAL_MAGIC;
    g.ver = GCAL_VER;
    memcpy(g.bias, g_gcal_bias, sizeof(g.bias));
    g.crc = crc16_ccitt((const uint8_t *)g.bias, sizeof(g.bias));

    lfs_file_t f;
    if (lfs_file_open(fs, &f, GCAL_FILE, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC) != 0) {
        LOG_ERR("[GYRO] gcal save: open failed\n");
        return;
    }
    bool ok = lfs_file_write(fs, &f, &g, sizeof(g)) == (lfs_ssize_t)sizeof(g);
    if (lfs_file_close(fs, &f) != 0)
        ok = false;
    LOG_INF("[GYRO] gcal %s\n", ok ? "saved" : "save FAILED");
}

static void gcal_load(void)
{
    lfs_t *fs = macro_store_lfs();
    if (!fs)
        return;
    lfs_file_t f;
    if (lfs_file_open(fs, &f, GCAL_FILE, LFS_O_RDONLY) != 0)
        return;                                /* 还没做过静置校准 */
    gcal_file_t g;
    lfs_ssize_t rd = lfs_file_read(fs, &f, &g, sizeof(g));
    lfs_file_close(fs, &f);
    if (rd != (lfs_ssize_t)sizeof(g) || g.magic != GCAL_MAGIC || g.ver != GCAL_VER ||
        g.crc != crc16_ccitt((const uint8_t *)g.bias, sizeof(g.bias)))
        return;
    for (int i = 0; i < 3; i++)
        if (!(g.bias[i] > -20.f && g.bias[i] < 20.f))
            return;                            /* 也挡 NaN */
    gyro_set_bias(g.bias);
    LOG_INF("[GYRO] gcal loaded\n");
}

/* R2.4：配置身份牌（方案 B）也单独存一个小文件，理由同 gcal.bin：塞进 cfg.bin 要升
 * DS5MAP_CFG_VER，用户的映射会被整份清掉。只在网页 COMMIT(0x18) 时写。 */
#define CFGID_FILE  "cfgid.bin"
#define CFGID_MAGIC 0x44494643u   /* "CFID" */
#define CFGID_VER   1

typedef struct __attribute__((packed)) {
    uint32_t     magic;
    uint16_t     ver;
    uint16_t     rsv;
    map_cfg_id_t id;
    uint16_t     crc;          /* crc16_ccitt(id) */
} cfgid_file_t;

static bool cfgid_save(void)
{
    lfs_t *fs = macro_store_lfs();
    if (!fs)
        return false;
    cfgid_file_t c;
    memset(&c, 0, sizeof(c));
    c.magic = CFGID_MAGIC;
    c.ver = CFGID_VER;
    c.id = g_cfg_id;
    c.crc = crc16_ccitt((const uint8_t *)&c.id, sizeof(c.id));

    lfs_file_t f;
    if (lfs_file_open(fs, &f, CFGID_FILE, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC) != 0) {
        LOG_ERR("[MAP] cfgid save: open failed\n");
        return false;
    }
    bool ok = lfs_file_write(fs, &f, &c, sizeof(c)) == (lfs_ssize_t)sizeof(c);
    if (lfs_file_close(fs, &f) != 0)
        ok = false;
    LOG_INF("[MAP] cfgid %s\n", ok ? "saved" : "save FAILED");
    return ok;
}

static void cfgid_load(void)
{
    memset(&g_cfg_id, 0, sizeof(g_cfg_id));
    lfs_t *fs = macro_store_lfs();
    if (!fs)
        return;
    lfs_file_t f;
    if (lfs_file_open(fs, &f, CFGID_FILE, LFS_O_RDONLY) != 0)
        return;                                /* 还没写过身份牌 */
    cfgid_file_t c;
    lfs_ssize_t rd = lfs_file_read(fs, &f, &c, sizeof(c));
    lfs_file_close(fs, &f);
    if (rd != (lfs_ssize_t)sizeof(c) || c.magic != CFGID_MAGIC || c.ver != CFGID_VER ||
        c.crc != crc16_ccitt((const uint8_t *)&c.id, sizeof(c.id)))
        return;
    c.id.name[sizeof(c.id.name) - 1] = 0;
    g_cfg_id = c.id;
}

static bool cfg_save(void)
{
    lfs_t *fs = macro_store_lfs();
    if (!fs)
        return false;

    lfs_file_t f;
    if (lfs_file_open(fs, &f, DS5MAP_CFG_FILE,
                      LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC) != 0)
        return false;

    uint8_t hdr[8];
    hdr[0] = (uint8_t)(DS5MAP_CFG_MAGIC & 0xFF);
    hdr[1] = (uint8_t)((DS5MAP_CFG_MAGIC >> 8) & 0xFF);
    hdr[2] = (uint8_t)((DS5MAP_CFG_MAGIC >> 16) & 0xFF);
    hdr[3] = (uint8_t)((DS5MAP_CFG_MAGIC >> 24) & 0xFF);
    hdr[4] = (uint8_t)(DS5MAP_CFG_VER & 0xFF);
    hdr[5] = (uint8_t)((DS5MAP_CFG_VER >> 8) & 0xFF);
    hdr[6] = 0;
    hdr[7] = 0;

    uint16_t crc = crc16_ccitt((const uint8_t *)&g_cfg, sizeof(g_cfg));
    uint8_t cb[2] = { (uint8_t)(crc & 0xFF), (uint8_t)(crc >> 8) };

    bool ok = true;
    if (lfs_file_write(fs, &f, hdr, sizeof(hdr)) != (lfs_ssize_t)sizeof(hdr))
        ok = false;
    if (ok && lfs_file_write(fs, &f, &g_cfg, sizeof(g_cfg)) != (lfs_ssize_t)sizeof(g_cfg))
        ok = false;
    if (ok && lfs_file_write(fs, &f, cb, sizeof(cb)) != (lfs_ssize_t)sizeof(cb))
        ok = false;
    if (lfs_file_close(fs, &f) != 0)
        ok = false;

    if (ok) {
        g_cfg_dirty = false;
        LOG_INF("[MAP] config saved\n");
    } else {
        LOG_ERR("[MAP] config save FAILED\n");
    }
    return ok;
}

/* ==================================================================== */
/* RPC protocol                                                         */
/* ==================================================================== */

static uint32_t region_size(uint8_t region)
{
    switch (region) {
    case MAP_REG_META:      return MAP_META_SIZE;
    case MAP_REG_SLOTS:     return sizeof(g_cfg.slots);
    /* 0x02（旧 passthru）与 0x04（原摇杆曲线区）都已删除：落到 default → 0 / NULL，
     * READ/WRITE 一律回 MAP_RPC_ERR（不支持）。 */
    case MAP_REG_LAYERBIND: return sizeof(g_cfg.binds);
    /* 0x04（原摇杆曲线区）已删除：落到 default → size 0 / ptr NULL，
     * READ/WRITE 一律回 MAP_RPC_ERR（不支持）。 */
    case MAP_REG_GYRO:      return sizeof(g_cfg.gyro);   /* map_gyro_t (14 B), 全局一份 */
    case MAP_REG_TP:        return sizeof(g_cfg.tp);
    case MAP_REG_VK:        return sizeof(g_cfg.vk);
    case MAP_REG_STRAT:     return sizeof(g_cfg.strats);
    case MAP_REG_S2T:       return sizeof(g_cfg.s2t);     /* R2.5 */
    case MAP_REG_FLICK:     return sizeof(g_cfg.flick);   /* R2.5 落位 */
    case MAP_REG_GEST:      return sizeof(g_cfg.gest);    /* R2.5 落位 */
    /* 宏目录必须按【16 条 id 索引全数组】整读，不能回 count*28：
     * 网页 readDir() 固定读 MAX_COUNT*28 = 448 字节再按 size≠0 过滤，
     * 而 region_ptr 指向的就是 s_dir 全数组。以前回 count*28，宏数 < 16 时
     * 网页第二块 off 就 > total 吃 ERR → 目录永远读不出来
     * （2026-10-11 修；症状：保存宏后弹「已写入，但目录回读不到」）。 */
    case MAP_REG_MACRO_DIR: return (uint32_t)MAP_MACRO_MAX_COUNT * sizeof(map_macro_dir_t);
    case MAP_REG_MACRO:     return 1u << 12;
    case MAP_REG_TLM_RAW:   return INPUT_LEN;
    case MAP_REG_TLM_OUT:   return INPUT_LEN;
    case MAP_REG_TLM_STATE: return MAP_TLM_STATE_SIZE;
    case MAP_REG_CRASH:     return sizeof(g_crash_record);
    case MAP_REG_DIAG:      return DIAG_SIZE;
    case MAP_REG_DIAG_PREV: return DIAG_SIZE;   /* S30：上一次开机的快照，布局同 DIAG */
    case MAP_REG_VER:       return MAP_VER_SIZE;
    case MAP_REG_PERF:      return PERF_BLOB_SIZE;   /* S33 */
    case MAP_REG_CFG_ID:    return sizeof(g_cfg_id);  /* R2.4 */
    case MAP_REG_STATUS:    return MAP_STATUS_SIZE;
    default: return 0;
    }
}

static void resp_begin(uint8_t op, uint8_t region, uint16_t off, uint8_t status)
{
    g_resp[0] = status;
    g_resp[1] = op;
    g_resp[2] = region;
    g_resp[3] = (uint8_t)(off & 0xFF);
    g_resp[4] = (uint8_t)(off >> 8);
    g_resp_len = 5;
}

static void resp_data(const uint8_t *d, uint16_t n)
{
    for (uint16_t i = 0; i < n; i++) {
        if (g_resp_len >= MAP_RPC_REPORT_LEN)
            break;
        g_resp[g_resp_len++] = d[i];
    }
}

static uint8_t build_status(uint8_t *out)
{
    /* see header: MAP_REG_STATUS layout */
    memset(out, 0, MAP_STATUS_SIZE);
    out[0] = 2;
    out[1] = 1;                                  /* proto ver 2.1 — 本体=0 无=1 的加法模型 */
    uint8_t flags = 0;
    if (g_cfg_dirty || g_pend_active)
        flags |= 0x01;
    if (macro_store_stage_busy())
        flags |= 0x02;
    if (g_any_mapping)
        flags |= 0x04;
    if (g_has_kbd || macro_store_count() > 0)
        flags |= 0x08;
    if (g_has_mouse || macro_store_count() > 0)
        flags |= 0x10;
    out[2] = flags;
    out[3] = g_layer_mask;
    out[4] = top_layer();
    out[5] = (uint8_t)macro_store_count();
    uint32_t freesz = macro_store_free_bytes();
    out[6] = (uint8_t)freesz;
    out[7] = (uint8_t)(freesz >> 8);
    out[8] = (uint8_t)(freesz >> 16);
    out[9] = (uint8_t)(freesz >> 24);
    out[10] = MAP_MACRO_MAX_COUNT - 1;
    out[11] = g_mp.active ? (uint8_t)(g_mp.id + 1) : 0;
    return MAP_STATUS_SIZE;
}

static void build_meta(uint8_t *out)
{
    out[0] = 0x43;                               /* 'C' */
    out[1] = 0x35;                               /* '5' */
    out[2] = DS5MAP_CFG_VER;
    out[3] = MAP_LAYERS;
    out[4] = (uint8_t)(g_cfg.timing.hold_ms & 0xFF);
    out[5] = (uint8_t)(g_cfg.timing.hold_ms >> 8);
    out[6] = (uint8_t)(g_cfg.timing.dtap_ms & 0xFF);
    out[7] = (uint8_t)(g_cfg.timing.dtap_ms >> 8);
    out[8] = (uint8_t)(g_cfg.timing.ttap_ms & 0xFF);
    out[9] = (uint8_t)(g_cfg.timing.ttap_ms >> 8);
    out[10] = (uint8_t)(g_cfg.timing.tap_pulse_ms & 0xFF);
    out[11] = (uint8_t)(g_cfg.timing.tap_pulse_ms >> 8);
    memcpy(out + 12, g_cfg.layer_names, sizeof(g_cfg.layer_names));
}

static const uint8_t *region_ptr(uint8_t region)
{
    switch (region) {
    case MAP_REG_META:      return NULL; /* built on the fly */
    case MAP_REG_SLOTS:     return (const uint8_t *)&g_cfg.slots[0][0][0];
    /* 0x02（旧 passthru）与 0x04（原摇杆曲线区）都已删除：返回 NULL → MAP_RPC_ERR */
    case MAP_REG_LAYERBIND: return (const uint8_t *)g_cfg.binds;
    /* 0x04（原摇杆曲线区）已删除：返回 NULL，调用方回 MAP_RPC_ERR */
    case MAP_REG_GYRO:      return (const uint8_t *)&g_cfg.gyro;
    case MAP_REG_TP:        return (const uint8_t *)&g_cfg.tp;   /* 全局，不分层 */
    case MAP_REG_VK:        return (const uint8_t *)g_cfg.vk;
    case MAP_REG_STRAT:     return (const uint8_t *)g_cfg.strats;
    case MAP_REG_S2T:       return (const uint8_t *)&g_cfg.s2t;     /* R2.5 */
    case MAP_REG_FLICK:     return (const uint8_t *)&g_cfg.flick;   /* R2.5 落位 */
    case MAP_REG_GEST:      return (const uint8_t *)&g_cfg.gest;    /* R2.5 落位 */
    case MAP_REG_MACRO_DIR: return (const uint8_t *)macro_store_dir();
    case MAP_REG_TLM_RAW:   return g_tlm_raw;
    case MAP_REG_TLM_OUT:   return g_tlm_out;
    case MAP_REG_PERF:      return perf_blob();       /* S33：纯 RAM 视图，ISR 安全 */
    case MAP_REG_CFG_ID:    return (const uint8_t *)&g_cfg_id;   /* R2.4 */
    default: return NULL;
    }
}

/* float → 有符号 16 位定点的四舍五入；负数要往负方向偏，不然 -0.4 会舍成 0 */
static int16_t f_to_i16(float v)
{
    if (v > 32767.f) return 32767;
    if (v < -32768.f) return -32768;
    return (int16_t)(v >= 0.f ? v + 0.5f : v - 0.5f);
}

static void tlm_gyro_pack(uint8_t *out)
{
    gyro_status_t gs;
    gyro_get_status(&gs);
    out[0] = gs.dir;
    out[1] = gs.flags;
    out[2] = gs.confidence;
    out[3] = gs.cal_prog;
    for (int i = 0; i < 3; i++) {
        uint16_t gv = (uint16_t)f_to_i16(gs.grav[i] * 16384.f);   /* Q14 */
        uint16_t bv = (uint16_t)f_to_i16(gs.bias[i] * 256.f);     /* Q8, 256 = 1 dps */
        out[4 + i * 2]  = (uint8_t)(gv & 0xFF);
        out[5 + i * 2]  = (uint8_t)(gv >> 8);
        out[10 + i * 2] = (uint8_t)(bv & 0xFF);
        out[11 + i * 2] = (uint8_t)(bv >> 8);
    }
}

/* R2.6：扳机效果状态字节（TLM_STATE[47]）。main.c 每帧算一份写这里，遥测原样抄出去 ——
 * 「我明明配了效果为什么手上没感觉」的答案就在这个字节：是我们没装上，还是游戏在管。 */
static volatile uint8_t g_s2t_state;

void ds5map_s2t_set_state(uint8_t bits)
{
    g_s2t_state = bits;
}

static void build_tlm_state(uint8_t *out)
{
    uint8_t *st = g_rpc_status;
    build_status(st);
    memset(out, 0, MAP_TLM_STATE_SIZE);
    uint16_t seq = g_tlm_seq;
    out[0] = (uint8_t)(seq & 0xFF);
    out[1] = (uint8_t)(seq >> 8);
    memcpy(out + 2, st + 2, 10);                 /* flags..active macro */
    /* 键鼠输出实况（供测试页显示「到底按了哪些键」） */
    memcpy(out + 12, g_last_kbd, 8);             /* [0] 修饰键 + [2..7] 键码 */
    out[20] = g_last_mouse_btns;
    out[21] = g_tlm_wheel;
    out[22] = g_tlm_wheel_seq;
    out[44] = (uint8_t)(g_last_media & 0xFF);    /* R1 / R2.2：媒体键位图（44-45，小端 u16） */
    out[45] = (uint8_t)(g_last_media >> 8);
    /* R2.7 Flick Stick 状态（跑在 ISR 里，只读几个 bool/整数，不碰浮点）：
     * bit0 本帧接管右摇杆、bit1 摇杆在激活圈里、bit2 配了启用模式 */
    out[46] = (uint8_t)((g_flick_live ? 1 : 0) | (g_flick_engaged ? 2 : 0) | (g_flick_on ? 4 : 0));
    out[47] = g_s2t_state;                       /* R2.6：扳机效果状态（main.c 每帧写，见 ds5map.h） */

    /* 触摸板 / 虚拟键 / 陀螺仪：测试页右侧「输出」面板全靠这一段 */
    out[23] = g_cfg.tp.single;
    out[24] = (uint8_t)(g_tp_finger | (g_tp_stick ? 4 : 0));
    out[25] = g_tp_stick ? (uint8_t)g_tp_sx : 0;   /* 没在推杆就是 0，不留上一帧的残值 */
    out[26] = g_tp_stick ? (uint8_t)g_tp_sy : 0;
    out[27] = g_tp_mask;
    out[28] = g_tlm_vk_mask;
    out[29] = (uint8_t)((g_gyro_live ? 1 : 0) | (g_cfg.gyro.target << 1));
    out[30] = g_tp_pinch_seq[0];
    out[31] = g_tp_pinch_seq[1];
    for (int i = 0; i < 6; i++) {
        uint16_t v = (uint16_t)(int16_t)g_tlm_acc[i];
        out[32 + i * 2] = (uint8_t)(v & 0xFF);
        out[33 + i * 2] = (uint8_t)(v >> 8);
    }

    /* 陀螺仪姿态：方向模式 / 状态位 / 零偏置信度 / 校准进度 / 重力方向 / 零偏。
     * 测试页靠这一段画方向模式、置信度条、重力矢量，静置校准的进度条也读这里。
     * 本函数跑在 USB ISR 里：只能搬缓存，打包（浮点）在任务侧做，见 tlm_gyro_pack。 */
    memcpy(out + 48, g_tlm_gyro, 16);
}

/* handle a request; ISR-safe for RAM-only ops. Flash-touching ops are
 * copied to the pending slot and completed by ds5map_deferred(). */
void ds5map_rpc_request(const uint8_t *req, uint16_t len)
{
    if (len < 5)
        return;

    uint8_t op = req[0];
    uint8_t region = req[1];
    uint16_t off = (uint16_t)(req[2] | (req[3] << 8));
    uint8_t dlen = req[4];
    if (dlen > len - 5)
        dlen = len - 5;
    const uint8_t *data = req + 5;

    if (op == 0)                 /* NOP: keep current response buffer */
        return;
    if (op != MAP_OP_READ)
        g_force_full = true;     /* S36：配置 / 动作可能变了，下一帧别走快速路径 */

    switch (op) {
    case MAP_OP_READ: {
        if (region == MAP_REG_STATUS) {
            uint8_t *st = g_rpc_status;
            build_status(st);
            resp_begin(op, region, off, MAP_RPC_OK);
            resp_data(st, MAP_STATUS_SIZE);
            return;
        }
        if (region == MAP_REG_TLM_STATE) {
            uint8_t *st = g_rpc_tlm;
            build_tlm_state(st);
            resp_begin(op, region, off, MAP_RPC_OK);
            resp_data(st, MAP_TLM_STATE_SIZE);
            /* 鼠标增量累计是「读一次清一次」：主机看到的就是两次读取之间的位移 */
            memset(g_tlm_acc, 0, sizeof(g_tlm_acc));
            /* 有人盯着遥测 → 把陀螺仪融合叫醒（关着也跑，见 ds5map_process） */
            g_gyro_probe = 1;
            return;
        }
        if (region == MAP_REG_CRASH) {
            /* 上次异常现场：SDK exception_entry 留在 noinit RAM，软复位不丢。
             * 纯 RAM 拷贝，ISR 安全。magic 不对说明从没崩过（或复位时丢了）。 */
            resp_begin(op, region, off, MAP_RPC_OK);
            resp_data((const uint8_t *)&g_crash_record, sizeof(g_crash_record));
            return;
        }
        if (region == MAP_REG_DIAG) {
            /* 诊断事件环形日志（noinit，316 B）：一次塞进 122 B 响应装不下，
             * 所以按 off/dlen 分块读（同 META 的写法）。
             * **只拷这一块**（diag_read_at）——本函数跑在 USB ISR 里，中断栈只有
             * 4 KB，早先版本在这里开了 uint8_t[316] 的栈缓冲，一读就把设备读挂。 */
            if (off >= DIAG_SIZE) {
                resp_begin(op, region, off, MAP_RPC_ERR);
                return;
            }
            uint8_t *chunk = g_rpc_work;
            uint32_t remain = DIAG_SIZE - off;
            uint32_t n = (dlen && dlen < remain) ? dlen : remain;
            if (n > MAP_RPC_CHUNK_MAX)
                n = MAP_RPC_CHUNK_MAX;
            diag_read_at(chunk, (uint16_t)off, (uint16_t)n);
            resp_begin(op, region, off, MAP_RPC_OK);
            resp_data(chunk, (uint16_t)n);
            return;
        }
        if (region == MAP_REG_DIAG_PREV) {
            /* S30：上一次开机的 DIAG 快照（noinit，316 B），读法与 MAP_REG_DIAG 完全一样；
             * 表头 magic 不对就是"上次是冷启动，没有可信快照"，由主机判断。 */
            if (off >= DIAG_SIZE) {
                resp_begin(op, region, off, MAP_RPC_ERR);
                return;
            }
            uint8_t *chunk = g_rpc_work;
            uint32_t remain = DIAG_SIZE - off;
            uint32_t n = (dlen && dlen < remain) ? dlen : remain;
            if (n > MAP_RPC_CHUNK_MAX)
                n = MAP_RPC_CHUNK_MAX;
            diag_read_prev_at(chunk, (uint16_t)off, (uint16_t)n);
            resp_begin(op, region, off, MAP_RPC_OK);
            resp_data(chunk, (uint16_t)n);
            return;
        }
        if (region == MAP_REG_META) {
            if (off >= MAP_META_SIZE) {
                resp_begin(op, region, off, MAP_RPC_ERR);
                return;
            }
            uint8_t *meta = g_rpc_meta;
            build_meta(meta);
            resp_begin(op, region, off, MAP_RPC_OK);
            uint32_t remain = MAP_META_SIZE - off;
            uint32_t n = (dlen && dlen < remain) ? dlen : remain;
            if (n > MAP_RPC_CHUNK_MAX)
                n = MAP_RPC_CHUNK_MAX;
            resp_data(meta + off, (uint16_t)n);
            return;
        }
        if (region == MAP_REG_MACRO) {
            /* needs file IO → defer; stage a PENDING response now */
            if (len <= sizeof(g_pend_req)) {
                memcpy(g_pend_req, req, len);
                g_pend_len = len;
                g_pend_active = true;
            }
            resp_begin(op, region, off, MAP_RPC_PENDING);
            return;
        }
        const uint8_t *base = region_ptr(region);
        uint32_t total = region_size(region);
        if (!base || off > total) {
            resp_begin(op, region, off, MAP_RPC_ERR);
            return;
        }
        uint32_t remain = total - off;
        uint32_t n = (dlen && dlen < remain) ? dlen : remain;
        if (n > MAP_RPC_CHUNK_MAX)
            n = MAP_RPC_CHUNK_MAX;
        resp_begin(op, region, off, MAP_RPC_OK);
        resp_data(base + off, (uint16_t)n);
        return;
    }

    case MAP_OP_WRITE: {
        if (region == MAP_REG_MACRO) {
            uint8_t id = (uint8_t)(off >> 12);
            uint16_t boff = (uint16_t)(off & 0xFFF);
            int ret = macro_store_stage_write(id, boff, data, dlen);
            resp_begin(op, region, off, ret == 0 ? MAP_RPC_OK : MAP_RPC_ERR);
            return;
        }
        if (region == MAP_REG_META) {
            /* timing fields + names, partial writes allowed */
            for (uint8_t i = 0; i < dlen; i++) {
                uint16_t p = (uint16_t)(off + i);
                uint8_t v = data[i];
                if (p >= 4 && p < 12) {
                    uint8_t *t = (uint8_t *)&g_cfg.timing;
                    t[p - 4] = v;
                } else if (p >= 12 && p < 12 + sizeof(g_cfg.layer_names)) {
                    ((uint8_t *)g_cfg.layer_names)[p - 12] = v;
                } else if (p < 4) {
                    /* magic/ver/layers are read-only */
                }
            }
            cfg_derive();
            g_cfg_dirty = true;
            resp_begin(op, region, off, MAP_RPC_OK);
            return;
        }
        if (region == MAP_REG_CFG_ID) {
            /* R2.4：身份牌只改内存，不碰 g_cfg_dirty（不然改个名字也要重写一遍 cfg.bin）；
             * 落盘等 COMMIT(0x18) */
            if (off + dlen > sizeof(g_cfg_id)) {
                resp_begin(op, region, off, MAP_RPC_ERR);
                return;
            }
            memcpy((uint8_t *)&g_cfg_id + off, data, dlen);
            g_cfg_id.name[sizeof(g_cfg_id.name) - 1] = 0;
            resp_begin(op, region, off, MAP_RPC_OK);
            return;
        }
        if (region == MAP_REG_MACRO_DIR || region == MAP_REG_TLM_RAW ||
            region == MAP_REG_TLM_OUT || region == MAP_REG_TLM_STATE ||
            region == MAP_REG_STATUS) {
            resp_begin(op, region, off, MAP_RPC_ERR); /* read-only */
            return;
        }
        uint8_t *base = (uint8_t *)region_ptr(region);
        uint32_t total = region_size(region);
        if (!base || off + dlen > total) {
            resp_begin(op, region, off, MAP_RPC_ERR);
            return;
        }
        memcpy(base + off, data, dlen);
        /* **这里不能每次调用 cfg_derive()**（2026-10-11 修）。
         * cfg_derive() 会把整个区域当作「用户配置」来做规整 —— 例如曲线区里
         * 还没被写到的轴是全 0，它会把那条轴"修"成恒等；于是后续分块写进去的
         * 数据又被覆盖。1024 字节的曲线区要分 9 块写，实测每块都触发一次，
         * 最终只有前 ~480 字节是对的（LX 完整、LY 缺 32 字节、RX/RY 完全丢失），
         * 而每块的响应都还是 OK —— 所以网页端看不出失败，只看到曲线被写坏。
         * 规整统一挪到 COMMIT（见下方 MAP_OP_COMMIT 分支）。 */
        g_cfg_dirty = true;
        resp_begin(op, region, off, MAP_RPC_OK);
        return;
    }

    case MAP_OP_COMMIT: {
        /* 分块写入期间不做规整，统一在这里做一次（见 MAP_OP_WRITE 里的说明）。
         * R2.4：身份牌的 COMMIT 只在 deferred 里写 cfgid.bin，不规整、不重写 cfg.bin。 */
        if (region != MAP_REG_CFG_ID) {
            cfg_derive();
            /* R2.9：扳机相关区改完 → 立刻让 bt 循环重发一次状态（改完即生效，不用断电上电） */
            if (region == MAP_REG_S2T || region == MAP_REG_FLICK || region == MAP_REG_GEST)
                ds5map_s2t_request_resend();
            if (region == MAP_REG_MACRO)
                g_pend_active = true;            /* macro stage flush in deferred */
            else
                g_cfg_dirty = true;              /* deferred cfg_save() */
        }
        /* **必须回 PENDING，不能先回 OK**（2026-10-11 S26 修）：
         * 原来这里立刻回 OK，deferred 里干完活又回一次（同一个 tag）。
         * 主机那边第一个 OK 一到就认为 COMMIT 完成、接着发下一个请求；
         * 随后 deferred 的"第二次响应"就落进了新请求的响应寄存器 ——
         * 主机轮询到的是上一条的旧响应，tag 不匹配 → 一直等到超时，
         * 表现为宏保存/删除后页面报「RPC 等待超时」。 */
        if (len <= sizeof(g_pend_req)) {
            memcpy(g_pend_req, req, len);
            g_pend_len = len;
            g_pend_active = true;
            resp_begin(op, region, off, MAP_RPC_PENDING);
        } else {
            resp_begin(op, region, off, MAP_RPC_OK);   /* 没排上 deferred，没有第二次响应 */
        }
        return;
    }

    case MAP_OP_ERASE: {
        if (region == MAP_REG_MACRO && len <= sizeof(g_pend_req)) {
            memcpy(g_pend_req, req, len);
            g_pend_len = len;
            g_pend_active = true;
            /* 同上：真删在 deferred 里做，这里只回 PENDING（S26 修）。 */
            resp_begin(op, region, off, MAP_RPC_PENDING);
        } else {
            resp_begin(op, region, off, MAP_RPC_ERR);
        }
        return;
    }

    case MAP_OP_ACTION: {
        /* 陀螺仪静置校准的起停。纯 RAM 标志位，不进 deferred（不碰 flash）。
         * 进度由 gyro_update() 每帧推进，测试页按 20Hz 轮询 TLM_STATE 画条。 */
        if (region != MAP_REG_GYRO) {
            resp_begin(op, region, off, MAP_RPC_ERR);
            return;
        }
        if (off == MAP_GYRO_ACT_CAL_START)
            gyro_cal_start();
        else if (off == MAP_GYRO_ACT_CAL_CANCEL)
            gyro_cal_cancel();
        else {
            resp_begin(op, region, off, MAP_RPC_ERR);
            return;
        }
        g_gyro_probe = 1;   /* 校准期间融合必须一直跑，别指望主机还在轮询 */
        /* ISR 里不能调 gyro_get_status()（浮点），进度回缓存里的 cal_prog，
         * 实时进度主机本来就是靠 20Hz 轮询 TLM_STATE 拿的。 */
        resp_begin(op, region, off, MAP_RPC_OK);
        resp_data(&g_tlm_gyro[3], 1);
        return;
    }

    default:
        resp_begin(op, region, off, MAP_RPC_ERR);
        return;
    }
}

/* task context: complete deferred (flash-touching) RPC work */
/* Create+触摸板：循环「单指移动映射到」（原生 → 鼠标 → 左摇杆 → 右摇杆 → 十字键）。
 * 配置一样要落盘，所以借 RPC 的 COMMIT 走 deferred（flash 写不能在这个任务里做）。 */

void ds5map_deferred(void)
{
    /* 先在锁外做一次无锁快速判断（2026-10-10 修）。
     *
     * 这一段原本是 MAP_LOCK() 之后再判断 g_pend_active，于是**每个 USB 任务
     * 循环都要抢一次递归互斥锁**；而 1ms 定时器回调（最高优先级）每毫秒也在
     * 抢同一把锁。结果：USB 任务被反复卡在抢锁上 → 报文来不及发给主机 →
     * 主机判定 USB 掉线 → 重枚举，表现为"连上几秒就断/半连不连/反复重启"。
     *
     * g_pend_active 是 volatile，绝大多数时候都是 false，所以放到锁外判断即可
     * 完全避免这条路径上的锁竞争；只有真有请求时才去抢锁处理。 */
    /* g_cfg_dirty 也要放进来：普通配置写入（非宏）只置这个标志、
     * 不排请求，若这里只看 g_pend_active 就会直接早退，
     * cfg_save() 永远不执行 → 配置只活在内存里、断电就丢。 */
    if (!g_pend_active && !g_cfg_dirty && !g_gcal_dirty)
        return;

    MAP_LOCK();

    if (g_gcal_dirty) {                /* R1.3：静置校准的零偏落盘（一次性，用户点了校准才有） */
        g_gcal_dirty = false;
        gcal_save();
    }

    if (g_pend_active) {
    uint8_t req[MAP_RPC_REPORT_LEN];
    uint16_t len = g_pend_len;
    if (len > sizeof(req))
        len = sizeof(req);
    memcpy(req, g_pend_req, len);
    g_pend_active = false;

    uint8_t op = req[0];
    uint8_t region = req[1];
    uint16_t off = (uint16_t)(req[2] | (req[3] << 8));
    uint16_t dlen = (len >= 5) ? req[4] : 0;

    if (op == MAP_OP_READ && region == MAP_REG_MACRO) {
        uint8_t buf[MAP_RPC_CHUNK_MAX];
        uint16_t ask = dlen ? dlen : (uint16_t)sizeof(buf);
        if (ask > sizeof(buf))
            ask = sizeof(buf);
        uint16_t got = 0;
        int ret = macro_store_read((uint8_t)(off >> 12),
                                   (uint16_t)(off & 0x0FFF), buf, ask, &got);
        resp_begin(op, region, off, ret == 0 ? MAP_RPC_OK : MAP_RPC_ERR);
        resp_data(buf, got);
    } else if (op == MAP_OP_COMMIT && region == MAP_REG_MACRO) {
        int ret = macro_store_stage_commit();
        resp_begin(op, region, off, ret == 0 ? MAP_RPC_OK : MAP_RPC_ERR);
    } else if (op == MAP_OP_ERASE && region == MAP_REG_MACRO) {
        int ret = macro_store_erase((uint8_t)off);
        resp_begin(op, region, off, ret == 0 ? MAP_RPC_OK : MAP_RPC_ERR);
    } else if (op == MAP_OP_COMMIT && region == MAP_REG_CFG_ID) {
        bool ok = cfgid_save();          /* R2.4：身份牌 */
        resp_begin(op, region, off, ok ? MAP_RPC_OK : MAP_RPC_ERR);
    } else if (op == MAP_OP_COMMIT) {
        /* 排进来的配置 COMMIT：这里才真正落盘，并顺手清掉 g_cfg_dirty ——
         * 不清的话下面的兜底分支会紧接着再 cfg_save() 一次（白写一遍 flash）。 */
        g_cfg_dirty = false;
        cfg_save();
        resp_begin(op, region, off, MAP_RPC_OK);
    }
    }   /* end if (g_pend_active) */

    /* 普通配置写入的落盘兜底。
     *
     * **必须"无论成败都清标志"**（2026-10-11 修）：原来只有 cfg_save() 内部
     * 写成功才清 g_cfg_dirty。一旦写失败，标志就永远是 1，而本函数被 BT 任务
     * 主循环反复调用 —— 于是**每一轮都在重写一次 flash**。写 flash 会阻塞 CPU，
     * 蓝牙链路的监督超时随即到期，表现为「DS5/DSE 刚连上就立刻掉线」，
     * 同时「待落盘」标志一直亮着。现在尝试一次就清标志，失败也不每轮重试。 */
    if (g_cfg_dirty) {
        cfg_save();
        g_cfg_dirty = false;
    }

    MAP_UNLOCK();
}

uint16_t ds5map_rpc_response(uint8_t *out, uint16_t max)
{
    uint16_t n = g_resp_len;
    if (n > max)
        n = max;
    for (uint16_t i = 0; i < n; i++)
        out[i] = g_resp[i];
    return n;
}

/* ==================================================================== */
/* Public API                                                           */
/* ==================================================================== */

void ds5map_init(void)
{
    memset(g_btn, 0, sizeof(g_btn));
    acts_clear();
    memset(g_stack, 0xFF, sizeof(g_stack));
    g_layer_mask = 0;
    g_bind_hold_act = 0;
    g_mp.id = 0xFF;
    g_resp_len = 5;
    g_resp[0] = MAP_RPC_ERR;

    if (!s_lock)
        s_lock = xSemaphoreCreateRecursiveMutex();
    g_boot_count++;   /* noinit：软复位不清，配合 g_crash_record 定位崩溃发生在第几次启动后 */
    diag_init(g_boot_count);   /* 诊断环形日志：清空并记下本次启动 */
    macro_store_init();
    ds5map_load();
    gcal_load();                 /* R1.3：静置校准存下来的零偏 */
    cfgid_load();                /* R2.4：配置身份牌 */
    tlm_gyro_pack(g_tlm_gyro);   /* 开机快照：ISR 遥测在第一帧输入前也有正确初值 */

    if (!s_tick_timer)
        s_tick_timer = xTimerCreate("ds5map", pdMS_TO_TICKS(MAP_TICK_MS), pdTRUE,
                                    NULL, tick_timer_cb);
    if (s_tick_timer)
        xTimerStart(s_tick_timer, 0);
    LOG_INF("[MAP] init: managed=%d mapping=%d kbd=%d mouse=%d\n",
            (int)g_any_managed, (int)g_any_mapping, (int)g_has_kbd, (int)g_has_mouse);

    /* 记录固件身份：主机读 MAP_REG_VER(0x15) 即可确认设备里跑的是哪一版，
     * 不必再靠人对账。FIRMWARE_VERSION 由 usb_gamepad.c 定义。 */
#ifdef DS5MAP_BUILD_TAG
    ds5map_set_build_tag(DS5MAP_BUILD_TAG);
#else
    ds5map_set_build_tag(0);
#endif
}

bool ds5map_has_kbd_outputs(void)
{
    return g_has_kbd || macro_store_count() > 0;
}

bool ds5map_has_mouse_outputs(void)
{
    return g_has_mouse || macro_store_count() > 0;
}

/* ==================================================================== */
/* R2.6 扳机效果：四种官方效果拼成 11 字节（关 / 阻力 / 扳机 / 自动扳机）      */
/* ==================================================================== */
/* 官方自适应扳机效果分 10 个行程档（zone i ≈ 行程的 10i% ~ 10(i+1)%）。网页给的是
 * 0~255 的「起始位置 / 结束位置」，这里按比例落档（200/255 ≈ 78% → 档 8，正好是社区实测的
 * 「80% 卡点」写法 25 80 01 07）。参数布局来自 ExtendInput 的 TriggerEffectGenerator
 * （Python 移植 dualsense-controller 的 TriggerEffectProperty.py），2026-10-17 逐条核对过：
 *   阻力     FEEDBACK  0x21：[1][2]=启用档位位图（起始档..结束档；不设结束位置=到 9 档）
 *                            [3..6]=每档 3bit 幅度（力度-1） 其余 0
 *   扳机     WEAPON    0x25：[1][2]=start/end 档位位图 [3]=力度-1 其余 0
 *                            硬约束 start 2..7、end = start+1..8；压过 end 阻力立刻归零（"咔哒"）
 *   自动扳机 VIBRATION 0x26：[1][2]=启用档位位图 [3..6]=每档 3bit 幅度 [9]=频率(Hz) 其余 0
 * 关 = 直接返回 false（main.c 就不碰这一侧的字节，游戏自己的效果照旧）。 */
/* R3.6：分界点有自己的字节了（`map_s2t_t.bound_l/r`），不再借体感手势区。
 * 它和手感起始位置是两件事；0 = 没设过 → 回落到手感起始位置。
 * R3.25：按层了 —— 这两个导出函数取/写**当前激活层**（top_layer()）那一份，
 * 也就是固件真正在用的那个值；指定层用 ds5map_t2_bound_layer()（同上，在文件前部）。 */
uint8_t ds5map_t2_bound(uint8_t w)
{
    return ds5map_t2_bound_layer(top_layer(), w);
}

void ds5map_t2_set_bound(uint8_t w, uint8_t v)
{
    uint8_t l = top_layer();
    if (l == MAP_LAYER_ID_MAIN || l >= MAP_LAYERS) {
        if (w) g_cfg.s2t.bound_r = v; else g_cfg.s2t.bound_l = v;
    } else if (w) {
        g_cfg.s2t.t2_bnd_r[l - 1] = v;
    } else {
        g_cfg.s2t.t2_bnd_l[l - 1] = v;
    }
}

/* R2.9：**配置改了要立刻重发一次手柄状态**。
 * 用户报的"改参数不生效、只能把接收机断电上电"就是这个原因：我们的效果只在
 * ①有 PC 输出报文经过（`s2t_apply_to_payload()` 在转发路径里）或
 * ②连上时那次 primer 才注入；在桌面上调参数时根本没有输出报文 → 注入没跑过。
 * 所以只要是往扳机相关区写入/COMMIT，就置这个标志，main.c 的 bt 循环看到后立刻
 * 构造一份当前状态（state_mgr + 缓存）走 0x32 发一次 —— 改完即生效，不用断电。 */
static volatile bool g_s2t_resend;

void ds5map_s2t_request_resend(void)
{
    g_s2t_resend = true;
}

bool ds5map_s2t_take_resend(void)
{
    bool r = g_s2t_resend;
    g_s2t_resend = false;
    return r;
}

/* R3.25b：**切层**也要立刻重发，但必须走"不 forget"那条 ——
 * main.c 的 forget 分支会 s2t_forget()（把 s2t_applied 清 0），于是新层"没有效果"时
 * 「撤下（发 OFF）」那个分支被跳过，手柄就一直粘着上一层的效果。
 * 实测症状：主层开 L2 阻力、覆盖层 1 关掉 → 切到覆盖层 1，手上还有阻力。
 * 配置 COMMIT（网页保存）仍旧用上面那个 forget 版：配置变了要强制重装。 */
static volatile bool g_s2t_resend_nf;

void ds5map_s2t_request_resend_nf(void)
{
    g_s2t_resend_nf = true;
}

bool ds5map_s2t_take_resend_nf(void)
{
    bool r = g_s2t_resend_nf;
    g_s2t_resend_nf = false;
    return r;
}

/* 这一侧/任意一侧现在有没有配"扳机效果"（main.c 用它决定要不要做 1 秒兜底重发）
 * R3.25（第二轮）：看的是**当前激活层**那一套 —— 层没了效果就不用再兜底重发。 */
bool ds5map_s2t_any_effect(void)
{
    map_s2t_t v;
    s2t_fx_view(top_layer(), &v);
    const map_s2t_t *s2 = &v;
    /* R3.13：位置现在是档位，**档 0 合法** —— 不再用 thresh 判断"有没有开" */
    if (s2->effect_mode & 0x01) return true;
    if (s2->effect_mode & 0x02) return true;
    return false;
}

/* R3.0：`[11]` 的两个行为位（网页「自适应扳机」那两个开关）：
 *   bit0 = 忽略游戏自己下发的原生自适应扳机（＝界面上「原生游戏的自适应扳机优先」没打勾）
 *   bit1 = 压穿时释放一段（＝界面上「压穿时一段仍然有效」没打勾）
 * R3.25（第二轮）：两个位也按层 —— 取当前激活层那一份。 */
bool ds5map_s2t_ignore_game(void)
{
    map_s2t_t v;
    s2t_fx_view(top_layer(), &v);
    return (v.flags & 0x01) != 0;
}

bool ds5map_s2t_release_s1(void)
{
    map_s2t_t v;
    s2t_fx_view(top_layer(), &v);
    return (v.flags & 0x02) != 0;
}

/* R2.8：自动扳机（VIBRATION 0x26）**会衰减** —— 真游戏捕获样本的 duration_ms 是 12~51 ms，
 * 也就是它们在不停重发；我们按"状态式"只发一次，用户就完全没感觉（2026-10-17 实测报的）。
 * 这个函数告诉 main.c：这一侧要不要周期重发（只有自动扳机要；别的模式重发反而会把刚压过去的
 * 墙重新装上、手感变差）。 */
bool ds5map_s2t_needs_refresh(uint8_t side)
{
    map_s2t_t v;
    s2t_fx_view(top_layer(), &v);        /* R3.25：当前激活层那一套 */
    const map_s2t_t *s2 = &v;
    uint8_t w = side ? 0 : 1;
    uint8_t bit = (uint8_t)(1u << w);
    if (!(s2->effect_mode & bit))
        return false;
    return s2t_kind(s2, w) == MAP_S2T_KIND_AUTO;
}

bool ds5map_s2t_effect(uint8_t side, uint8_t out11[11])
{
    /* R3.25（第二轮）：**整套扳机设置按层** —— 先取当前激活层那一份（临时视图），
     * 下面的换算/夹取逻辑一个字都没改。层一变，这里算出来的字节就变，
     * main.c 的 s2t_last 比对会看出来并重新下发（见 ds5map_s2t_request_resend 的层切换触发）。 */
    map_s2t_t v;
    s2t_fx_view(top_layer(), &v);
    const map_s2t_t *s2 = &v;
    /* **两套"侧"的编号必须翻译一次**（2026-10-17 抓到的 bug）：
     *   · 这个函数的 side 用 DS5MAP_S2T_SIDE_*：0 = 右扳机、1 = 左扳机（和 main.c 里
     *     "右在 [10..20]、左在 [21..31]" 一致）；
     *   · 而配置区（`effect_mode` 位、`strength`/`kind` 的高低半、`thresh_*`）用的编号是
     *     **0 = 左 / 1 = 右**（网页右栏、`set_s2t.py`、`watch_s2t.py` 都是这么读写的）。
     * 以前这里直接用 side 去查配置，结果"给左边配的效果装到了右扳机上"（设备状态字节
     * 报"已装上=右"而用户配的是 L2 就是这么来的）。w = 配置侧的编号。 */
    uint8_t w    = side ? 0 : 1;                           /* 0 = 左 / 1 = 右（配置侧编号） */
    uint8_t bit  = (uint8_t)(1u << w);                     /* effect_mode 的位（bit0 左 / bit1 右） */
    uint8_t th   = w ? s2->thresh_r : s2->thresh_l;        /* 起始位置 */
    uint8_t str  = s2t_str(s2, w);                         /* 力度 1..8（这一侧） */
    uint8_t kind = s2t_kind(s2, w);                        /* 效果类型（这一侧） */

    if (!th || !(s2->effect_mode & bit) || kind > MAP_S2T_KIND_AUTO)
        return false;

    memset(out11, 0, 11);
    uint8_t z = s2t_zone(th);

    if (kind == MAP_S2T_KIND_RESIST || kind == MAP_S2T_KIND_AUTO) {
        /* 启用位图 + 每档 3bit 幅度（档 i 占 bit 3i..3i+2）。
         * 阻力 FEEDBACK：**起始档 .. 9 档**（用户 2026-10-17 明确："阻力不该有结束位置"，
         *   所以 `end_*` 在阻力这一侧被忽略；区间阻力那种玩法不做了）。
         * 自动扳机 VIBRATION：起始档 .. 9 档，多一个 `[9]` = 频率。
         *   ⚠ 这个模式**会衰减**：真游戏是每 12~51 ms 重发一次（捕获样本的 duration_ms 就是重发间隔），
         *   所以 main.c 对它做周期重发（`ds5map_s2t_needs_refresh()`）。 */
        uint32_t zones = 0, amp = 0;
        for (uint8_t i = z; i <= 9; i++) {
            zones |= (uint32_t)1u << i;
            amp   |= (uint32_t)((str - 1) & 0x07u) << (3u * i);
        }
        out11[0] = (kind == MAP_S2T_KIND_RESIST) ? 0x21 : 0x26;
        out11[1] = (uint8_t)(zones & 0xFF);
        out11[2] = (uint8_t)((zones >> 8) & 0x03);
        out11[3] = (uint8_t)(amp & 0xFF);
        out11[4] = (uint8_t)((amp >> 8) & 0xFF);
        out11[5] = (uint8_t)((amp >> 16) & 0xFF);
        out11[6] = (uint8_t)((amp >> 24) & 0xFF);
        if (kind == MAP_S2T_KIND_AUTO) {
            /* R3.1（用户："原生是什么就写什么"）：**不再折半** —— 真游戏抓到的样本这个字节从 1 到 180
             * 都有（Spider-Man 180、TLOU1 1），原作者工具里显示的也是这个数本身；我们自己发明
             * "2 Hz 一格"（那是 DualSense-Windows 那套 API 的换算）只会让人跟官方对不上号。
             * 配置里存的就当作"原生字节"用，0 就取默认 10。 */
            uint8_t hz = w ? s2->freq_r : s2->freq_l;
            out11[9] = hz ? hz : 10;
        }
        return true;
    }

    /* 扳机 WEAPON 0x25：起始~结束之间一堵墙，压过结束档阻力立刻归零（那下"咔哒"）。
     * 硬件硬约束 start 2..7、end = start+1..8 —— 两头都会被夹（网页里会如实显示落点）。 */
    uint8_t st = z;
    if (st < 2) st = 2;
    if (st > 7) st = 7;
    uint8_t en_pos = w ? s2->end_r : s2->end_l;
    uint8_t en = en_pos ? s2t_zone(en_pos) : (uint8_t)(st + 1);   /* 0 = 没设 → 起始档+1 */
    if (en <= st) en = (uint8_t)(st + 1);                         /* 比起始还低 → 也当没设 */
    if (en > 8) en = 8;
    if (en <= st) st = (uint8_t)(en - 1);                  /* 起始档贴到上限时往回退一档 */
    out11[0] = 0x25;
    out11[1] = (uint8_t)((uint8_t)(1u << st) | (en < 8 ? (uint8_t)(1u << en) : 0));
    if (en >= 8)
        out11[2] = (uint8_t)(1u << (en - 8));
    out11[3] = (uint8_t)((str - 1) & 0x07u);
    return true;
}
