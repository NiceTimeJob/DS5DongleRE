#pragma once
/* ==================================================================== */
/* ds5map — advanced mapping engine                                      */
/*                                                                       */
/*   - 41 source buttons x 6 input forms per button                      */
/*     (单击/长按/双击/三击/按下/松开)                                    */
/*     = the 31 DS/DS5 buttons + the 4 DualSense Edge extras (2 Fn + 2   */
/*       back paddles; they read 0 on a non-Edge pad) + the 4 virtual    */
/*       combo keys V1..V4 + the 2 两段式扳机 keys (L2/R2 二段, R2.5)      */
/*   - up to 5 layers: layer 0 = 主层 (main) + up to 4 覆盖层 (override)   */
/*   - global gesture timings (long-press / double / triple windows)     */
/*   - gyro → mouse / left or right stick (axis pick, invert, sens, dz)  */
/*   - macros: recorded on PC, stored on device (LittleFS), played by FW */
/*                                                                       */
/* Output model:                                                         */
/*   - the TAP slot decides what a button BECOMES in the active layer:   */
/*       OUT_SELF (本体, the default) -> the button itself (＝原生);      */
/*       OUT_NONE (无)                -> nothing at all (＝旧「空置」);   */
/*       anything else                 -> that action INSTEAD of the      */
/*                                        button (remap, not additive);  */
/*     so "L1 held + Triangle -> 方向上" yields 方向上 only;             */
/*   - the other five forms (长按/双击/三击/按下/松开) are pure additions  */
/*     on top of whatever the TAP slot produced;                         */
/*   - the native line follows the raw button exactly, with no           */
/*     judgement-window delay.                                           */
/*                                                                       */
/* Layer chain: the active 覆盖层 are searched top-down, then the        */
/* main layer; the first slot whose out != OUT_SELF wins. So a shift     */
/* layer overrides only the (button, form) pairs it actually defines and */
/* everything else keeps following the main layer.                       */
/*                                                                       */
/* Gesture rules (managed buttons; unmanaged buttons pass straight thru):*/
/*   - tap (单击): fires on the press edge, held until release — there   */
/*     is no double/triple window wait, a single press ALWAYS fires it;   */
/*   - press/release (按下/松开): additive edge triggers on the raw line */
/*   - hold (长按): fires once at hold_ms while still held               */
/*   - double/triple (双击/三击): an extra action fired on the 2nd/3rd   */
/*     press inside dtap_ms / ttap_ms of the sequence; the earlier taps  */
/*     are not undone by it                                              */
/*                                                                       */
/* Storage: config blob = LittleFS "cfg.bin" on the media partition      */
/* (writes staged in RAM, flushed by OP_COMMIT); macros = "mNN" files.   */
/*                                                                       */
/* Open items (待定): trigger (L2/R2) analog behavior, vibration effect  */
/* format, gyro curve format, stick/trigger analog macro actions.        */
/* ==================================================================== */

#include <stdint.h>
#include <stdbool.h>

/* ------------------------------------------------------------------ */
/* Source buttons — ids 0..30 are identical to legacy remap.h            */
/* ------------------------------------------------------------------ */
/* R2.5：39 → 41（加了两个两段式扳机源键）。**改这个数会改 SLOTS 区布局**
 * （slots[5][MAP_BTN_COUNT][6]），所以每次改都必须同步升 DS5MAP_CFG_VER +
 * 网页的槽位尺寸常量。 */
#define MAP_BTN_COUNT 41

enum map_btn {
    MAP_BTN_SQUARE       = 0,
    MAP_BTN_CROSS        = 1,
    MAP_BTN_CIRCLE       = 2,
    MAP_BTN_TRIANGLE     = 3,
    MAP_BTN_L1           = 4,
    MAP_BTN_R1           = 5,
    MAP_BTN_L2           = 6,
    MAP_BTN_R2           = 7,
    MAP_BTN_CREATE       = 8,
    MAP_BTN_OPTIONS      = 9,
    MAP_BTN_L3           = 10,
    MAP_BTN_R3           = 11,
    MAP_BTN_PS           = 12,
    MAP_BTN_TP_CLICK     = 13,
    MAP_BTN_MUTE         = 14,
    MAP_BTN_DPAD_UP      = 15,
    MAP_BTN_DPAD_DOWN    = 16,
    MAP_BTN_DPAD_LEFT    = 17,
    MAP_BTN_DPAD_RIGHT   = 18,
    /* 触摸板手指分区：手指1 = 第一根按下的手指，手指2 = 第二根
     * （源位由 ds5map.c 的 src_bits() 直接从报文算；S33 起不再依赖已删除的 remap.c） */
    MAP_BTN_TP_F1_TOUCH  = 19,
    MAP_BTN_TP_F1_UP     = 20,
    MAP_BTN_TP_F1_DOWN   = 21,
    MAP_BTN_TP_F1_LEFT   = 22,
    MAP_BTN_TP_F1_RIGHT  = 23,
    MAP_BTN_TP_F1_CLICK  = 24,
    MAP_BTN_TP_F2_TOUCH  = 25,
    MAP_BTN_TP_F2_UP     = 26,
    MAP_BTN_TP_F2_DOWN   = 27,
    MAP_BTN_TP_F2_LEFT   = 28,
    MAP_BTN_TP_F2_RIGHT  = 29,
    MAP_BTN_TP_F2_CLICK  = 30,
    /* DualSense Edge only — payload byte 9 bits 4..7 (same bits the Linux
     * driver decodes as DS_EDGE_BUTTONS_*). A plain DS/DS5 reports 0 here. */
    MAP_BTN_FN_L         = 31,
    MAP_BTN_FN_R         = 32,
    MAP_BTN_BACK_L       = 33,
    MAP_BTN_BACK_R       = 34,
    /* 虚拟组合键 V1..V4（不是报文里的键，只在固件内部成立）：配置里给每个
     * 虚拟键挑两个物理键（map_vk_t），两个都按住时虚拟键就算「按下」。
     * 源键本身照常输出（加法式），虚拟键只有单击一种形态；
     * 它也能当覆盖层的激活键（binds[].src_btn 可以填 35..38）。 */
    MAP_BTN_V1           = 35,
    MAP_BTN_V2           = 36,
    MAP_BTN_V3           = 37,
    MAP_BTN_V4           = 38,
    /* 两段式扳机（R2.5）：**不是报文里的键**，由扳机模拟值 p[4]/p[5] 与配置的
     * 阈值 + 迟滞现算，有真实的按下/松开态 —— 所以走普通源键那条路，单击/长按/
     * 双击/三击/按下/松开六种形态与全部输出类型都白捡。
     * 它们排在虚拟组合键 V1..V4 后面，**不是虚拟键**：任何「虚拟键」判定都必须用
     * MAP_BTN_IS_VK()，不能再用 `b >= MAP_BTN_V1`。 */
    MAP_BTN_L2_S2        = 39,   /* 左扳机二段（行程越过阈值 = 按下） */
    MAP_BTN_R2_S2        = 40,   /* 右扳机二段 */
};

#define MAP_VK_COUNT 4
/* 虚拟组合键区间（35..38）：只有单击一种形态、不查报文、不进长按/双击/三击判定。 */
#define MAP_BTN_IS_VK(b)  ((b) >= MAP_BTN_V1 && (b) <= MAP_BTN_V4)
/* 能当「源键」用的 id：物理键 + 两段式扳机键（虚拟组合键不能当源，会互相套娃）。 */
#define MAP_BTN_IS_SRC(b) ((b) < MAP_BTN_COUNT && !MAP_BTN_IS_VK(b))
/* 两段式扳机的两个派生键：**报文里没有它们的本体输出**（payload_rebuild_buttons 不认这两个位），
 * 所以 OUT_SELF 对它们＝什么都不发，连发/开关也没有可翻转的东西。 */
#define MAP_BTN_IS_S2T(b) ((b) == MAP_BTN_L2_S2 || (b) == MAP_BTN_R2_S2)

/* ------------------------------------------------------------------ */
/* Input forms                                                          */
/* ------------------------------------------------------------------ */
enum map_form {
    FORM_TAP     = 0,   /* 单击 — default gesture */
    FORM_HOLD    = 1,   /* 长按 */
    FORM_DTAP    = 2,   /* 双击 */
    FORM_TTAP    = 3,   /* 三击 */
    FORM_PRESS   = 4,   /* 按下 — additive edge trigger */
    FORM_RELEASE = 5,   /* 松开 — additive edge trigger */
    MAP_FORM_COUNT
};

/* ------------------------------------------------------------------ */
/* Output actions                                                       */
/* ------------------------------------------------------------------ */
/* Zero-filled memory is the correct default: every slot starts as 本体. */
enum map_out {
    OUT_SELF  = 0,   /* 本体：输出按键自己（＝原生输出）。默认值；
                      *   在非 TAP 槽上等于「本层不额外输出」 */
    OUT_NONE  = 1,   /* 无：什么都不输出。在 TAP 槽上同时取消该键的原生
                      *   输出（原「空置」）；在其它槽上只是不额外输出 */
    OUT_PAD   = 2,   /* val = target button id (0..18 effective) 手柄→手柄 */
    OUT_KBD   = 3,   /* val = HID keycode, mod = modifier bitmask */
    OUT_MOUSE = 4,   /* val = 0 left / 1 right / 2 middle / 3 wheel_up / 4 wheel_down */
    OUT_MACRO = 5,   /* val = macro id (0..15) */
    OUT_VIB   = 6,   /* val = vibration effect id (reserved, format TBD) */
    OUT_STRAT = 7,   /* val = 战备表下标 (0..MAP_STRAT_COUNT-1)：把
                      *   g_cfg.strats[] 里的方向序列按十字键打出去
                      *   （《绝地潜兵 2》战备宏，不占 16 个宏名额） */
    OUT_MEDIA = 8,   /* R1：媒体与多功能键。val = Consumer Control 位图的位号（0..15）：
                      *   0 音量+  1 音量−  2 静音  3 播放/暂停  4 下一曲  5 上一曲  6 停止
                      *   7 快进  8 快退  9 媒体播放器  10 计算器  11 搜索  12 主页
                      *   13 后退  14 前进  15 刷新            （7..15 是 R2.2 加的）
                      *   按住期间位置 1，松开清 0（系统按"按下"沿触发） */
};

#define MAP_FLAG_TURBO  0x01   /* 连发: 50% duty toggle at param*10 ms period while held */
#define MAP_FLAG_TOGGLE 0x02   /* 开关: latch on first pulse, off on next */
/* 保留原键输出（2026-10-11 用户要的）：只在「单击」槽上有意义 ——
 * 这个键改成别的动作（键盘/鼠标/宏/别的键…）时，**原键照常发给主机**，
 * 也就是把单击从"替换"变成"叠加"。不设＝老行为（动作替换原键）。 */
#define MAP_FLAG_PASS_NATIVE 0x04

#define MAP_TURBO_UNIT_MS    10
/* R1：引擎定时器周期。S36 起只在有宏/连发/长按在计时时运行，所以周期只影响那些时候的
 * 分辨率与开销：5 ms 对宏步进、连发（最小半周期 5 ms）、长按/双击窗口都够用，
 * 比 1 ms 少跑 80%。宏步间隔会按 5 ms 向上取整。 */
#define MAP_TICK_MS          5
#define MAP_TURBO_DEFAULT_MS 200

typedef struct {
    uint8_t out;     /* enum map_out */
    uint8_t val;
    uint8_t mod;     /* OUT_KBD: HID modifier bitmask (0 = none) */
    uint8_t flags;   /* MAP_FLAG_* */
    uint8_t param;   /* turbo period in MAP_TURBO_UNIT_MS units; 0 = default */
} map_slot_t;        /* 5 bytes */

/* ------------------------------------------------------------------ */
/* Layers                                                               */
/* ------------------------------------------------------------------ */
#define MAP_LAYERS        5    /* 0 = 主层, 1..4 = 覆盖层 */
#define MAP_SHIFT_LAYERS  4

/* How a (button, form) activates its 覆盖层 */
#define MAP_LAYER_MODE_HOLD   0  /* layer active while button held */
#define MAP_LAYER_MODE_TOGGLE 1  /* form toggles layer on/off */

#define MAP_LAYER_ID_MAIN     0

typedef struct {
    uint8_t src_btn;   /* enum map_btn; 0xFF = unbound */
    uint8_t src_form;  /* enum map_form */
    uint8_t mode;      /* MAP_LAYER_MODE_* */
    uint8_t flags;     /* reserved */
} map_layer_bind_t;

/* ------------------------------------------------------------------ */
/* Global gesture timings (UI: 偏好设置)                                 */
/* ------------------------------------------------------------------ */
typedef struct {
    uint16_t hold_ms;      /* 长按判定, default 300 */
    uint16_t dtap_ms;      /* 双击判定, default 150 */
    uint16_t ttap_ms;      /* 三击总窗口, default 300 */
    uint16_t tap_pulse_ms; /* synthesized pulse length for tap-level outputs, default 40 */
} map_timing_t;

/* 摇杆响应曲线（map_stick_curves_t / MAP_CURVE_LUT_LEN / MAP_REG_CURVES）已按用户
 * 要求**彻底删除**（2026-10-11）：摇杆值原样通过，不再查表。配置里也不再有这一块。 */

/* ------------------------------------------------------------------ */
/* Gyro → mouse / stick                                                 */
/* ------------------------------------------------------------------ */
enum map_gyro_target {
    MAP_GYRO_OFF    = 0,   /* 关闭（零填充默认） */
    MAP_GYRO_MOUSE  = 1,   /* 鼠标移动 */
    MAP_GYRO_LSTICK = 2,   /* 左摇杆 */
    MAP_GYRO_RSTICK = 3,   /* 右摇杆 */
};

/* 陀螺仪是**全局一份**，不分层：切到哪个覆盖层都还是这套设置（曾经在 v8 里按层
 * 分过，太绕，v9 退了回来）。想「只在某个层开体感」就靠下面这个启用键去按住/开关。
 * 报文里的三条角速度轴（p[15]/p[17]/p[19]，16 位有符号，16 LSB 每度/秒）：
 *   俯仰 pitch = p[15]，绕左右轴转 = 抬头/低头
 *   偏摆 yaw   = p[17]，绕竖直轴转 = 左右转身
 *   翻滚 roll  = p[19]，绕前后轴转 = 身体左右倾斜
 * 加速度在 p[21]/p[23]/p[25]（8192 LSB 每 g）。加速度**不再**用来算重力方向：
 * 「跟随重力」已按用户要求删除（2026-10-11），只剩本体轴通路，见 gyro.c。 */
enum map_gyro_axis {
    MAP_GYRO_AXIS_PITCH = 0,
    MAP_GYRO_AXIS_YAW   = 1,
    MAP_GYRO_AXIS_ROLL  = 2,
};

/* 体感方向：水平轴怎么取。**只剩本体轴**。
 * 「跟随重力」(player space) 已按用户要求删除（2026-10-11），对应的枚举值
 * MAP_GYRO_GRAV_FOLLOW = 0 也一并删掉；固件里无论 gravity 字节读到什么，
 * cfg_derive() 都强制成本体轴。 */
enum map_gyro_grav {
    /* 本体轴：水平轴直接取 axis_h 选的那条。手柄一歪水平方向就跟着歪。 */
    MAP_GYRO_GRAV_LOCAL  = 1,
};

/* 启用模式：决定陀螺仪什么时候生效（「映射到」里不再有「关闭」这一项） */
enum map_gyro_en {
    MAP_GYRO_EN_OFF    = 0,   /* 关闭（零填充默认） */
    MAP_GYRO_EN_ALWAYS = 1,   /* 常开 */
    MAP_GYRO_EN_HOLD   = 2,   /* 按住 enable_btn 时启用 */
    MAP_GYRO_EN_TOGGLE = 3,   /* 按一下 enable_btn 开，再按一下关 */
};

typedef struct {
    uint8_t  target;      /* enum map_gyro_target（1..3；0 = 没选目标，仍视为关） */
    uint8_t  axis_h;      /* enum map_gyro_axis：水平方向（左右）取哪条轴；跟随重力时不用 */
    uint8_t  axis_v;      /* enum map_gyro_axis：垂直方向（上下）取哪条轴 */
    uint8_t  inv_h;       /* 水平反向 */
    uint8_t  inv_v;       /* 垂直反向 */
    uint8_t  deadzone;    /* 死区，单位度/秒（≈16 raw LSB），0..255；减去式，不是硬截断 */
    uint8_t  enable_mode; /* enum map_gyro_en */
    uint8_t  enable_btn;  /* enum map_btn；0xFF = 未绑定 */
    uint16_t sens_h;      /* 灵敏度 Q4（16 = 1.0×），建议 8..320 */
    uint16_t sens_v;
    uint8_t  gravity;     /* enum map_gyro_grav：只有 1 = 本体轴这一个合法值。
                           * 字节**必须留着**：网页的 MAP_REG_GYRO 区固定 14 字节，
                           * 第 12 字节就是它（page-map.js 写死 1）。固件一律强制 LOCAL。 */
    uint8_t  rsv2;
} map_gyro_t;             /* 14 bytes；全局一份，不分层（磁盘布局由 cfg 版本号守住）。 */

/* ------------------------------------------------------------------ */
/* Touchpad — single-finger move + two-finger pinch                     */
/* ------------------------------------------------------------------ */
/* 触摸板按下（MAP_BTN_TP_CLICK）跟普通按键一样走映射表，这里只管「移动」：
 *   单指：整块板子按一个目标走（原生 / 鼠标移动 / 左摇杆 / 右摇杆 / 十字按键）
 *   双指：两指距离变大＝远离、变小＝靠近，各触发一个动作（比如缩放地图）
 * ids 19..30 的「手指1/手指2」分区源仍然照旧由 remap 提供，跟这里无关。 */
enum map_tp_single {
    MAP_TP_NATIVE = 0,   /* 原生（零填充默认）：触摸数据照原样送下去 */
    MAP_TP_MOUSE  = 1,   /* 鼠标移动 */
    MAP_TP_LSTICK = 2,   /* 左摇杆：手指位置 → 推杆量（板中心＝回中） */
    MAP_TP_RSTICK = 3,   /* 右摇杆 */
    MAP_TP_DPAD   = 4,   /* 十字按键：手指相对板中心的方向 → 方向键 */
};

typedef struct {
    uint8_t    single;     /* enum map_tp_single */
    uint8_t    rsv0;
    map_slot_t pinch_out;  /* 双指远离（张开）触发的动作，脉冲式 */
    map_slot_t pinch_in;   /* 双指靠近（捏合）触发的动作 */
    uint8_t    sens;       /* 灵敏度 1..32，0 按 8 算；鼠标移动和摇杆目标共用 */
    uint8_t    rsv1;
    uint16_t   pinch_px;   /* 两指距离变化多少板单位算一次，默认 150 */
    uint8_t    rsv2[2];
} map_tp_t;                /* 18 bytes, appended last in ds5map_body_t */

/* ------------------------------------------------------------------ */
/* 虚拟组合键 V1..V4                                                     */
/* ------------------------------------------------------------------ */
/* 每个虚拟键配两个物理键，两个都按住 = 虚拟键按下。源键只能是物理键
 * （0..MAP_BTN_V1-1），不能拿虚拟键当源；两个源必须不同，否则该虚拟键
 * 视为未配置。源键本身的行为完全不受影响。 */
typedef struct {
    uint8_t a;   /* enum map_btn；0xFF = 未配置 */
    uint8_t b;
} map_vk_t;      /* 2 bytes */

/* ------------------------------------------------------------------ */
/* 两段式扳机 + 扳机效果（R2.5 起，R2.6 扩成四种官方效果）                  */
/* ------------------------------------------------------------------ */
/* 每一侧一份「扳机效果」：关 / 阻力 / 扳机 / 自动扳机（＝官方 OFF / FEEDBACK /
 * WEAPON / VIBRATION）。参数照官方那套：
 *   起始位置 = 效果从行程的哪里开始（0..255，固件换算成硬件 10 档）
 *   结束位置 = 只对「扳机」有效：墙到哪一档为止（压过去阻力归零＝那下"咔哒"）
 *   力度     = 1..8（**硬件只有 3 bit = 8 档**，网页那根 0~255 的滑条只是个好用的外衣）
 *   频率     = 只对「自动扳机」有效，Hz
 * 位置量也兼作「两段模式」的二段触发点（同一个行程点：压过它＝二段按下、同时手指能感到效果）。
 * 由 main.c 在转给手柄的输出报告里注入（状态式：只在切换沿发一次）；**游戏优先**：
 * 这一侧最近 1 秒收到过游戏自己下发的效果就让位（见 main.c 的 s2t_apply_to_payload）。 */
enum map_s2t_kind {
    MAP_S2T_KIND_RESIST = 0,    /* 阻力：从起始位置一路顶到底（FEEDBACK 0x21） */
    MAP_S2T_KIND_WEAPON = 1,    /* 扳机：起始~结束之间一堵墙，压过就"咔哒"（WEAPON 0x25） */
    MAP_S2T_KIND_AUTO   = 2,    /* 自动扳机：从起始位置起按频率震动（VIBRATION 0x26） */
};

/* R3.25：覆盖层 1..4 那一套「自适应扳机」参数在 map_s2t_t.fx[][] 里的列号
 * （**顺序就是网页 page-map.js 的 FX_KEYS 顺序**，两边都靠 tools/test_s2t_layout.js 钉住）。
 * 主层那份不用这里 —— 它就是结构体开头那几个老字段。 */
#define S2T_FX_EFFECT   0    /* 效果开关位（bit0 左 / bit1 右） */
#define S2T_FX_TH_L     1    /* 起始位置（左） */
#define S2T_FX_TH_R     2    /* 起始位置（右） */
#define S2T_FX_END_L    3    /* 结束位置（左） */
#define S2T_FX_END_R    4    /* 结束位置（右） */
#define S2T_FX_STR      5    /* 力度（低半左 / 高半右） */
#define S2T_FX_KIND     6    /* 效果类型（低 2 位左 / bit2-3 右） */
#define S2T_FX_FREQ_L   7    /* 频率（左） */
#define S2T_FX_FREQ_R   8    /* 频率（右） */
#define S2T_FX_FLAGS    9    /* 两个行为位（游戏优先 / 保持一阶段的输出） */
#define S2T_FX_COUNT    10

typedef struct {
    uint8_t thresh_l;        /* [0] 左「起始位置」0..255；0 = 这一侧关（默认） */
    uint8_t thresh_r;        /* [1] 右 */
    uint8_t hyst;            /* [2] 迟滞 1..63，默认 8（R3.8 起判定只用纯阈值，这一格留着不读） */
    uint8_t effect_mode;     /* [3] bit0 = 左造效果，bit1 = 右；0 = 都关（默认） */
    uint8_t strength;        /* [4] 力度：低 4 位 = 左（1..8）、高 4 位 = 右（**硬件只有 8 档**） */
    uint8_t kind;            /* [5] 效果类型：低 2 位 = 左、bit2-3 = 右（enum map_s2t_kind） */
    /* --- R2.6 新加进原来保留字节里的（含义就下面各行的注释）；
     *     R3.25 又在这后面追加了「每层一套」的设置，结构体 14 → 66 B（见下）。
     *     这些字节在**主层（层 0）**里是它自己那一份；覆盖层 1..4 用结构体末尾那两段。 --- */
    uint8_t end_l;           /* [6] 左「结束位置」0..255；0 = 未设（扳机→起始档+1；阻力→到 9 档） */
    uint8_t end_r;           /* [7] 右 */
    uint8_t freq_l;          /* [8] 左「频率」Hz；0 = 默认 10（只对「自动扳机」有效；
                              * 发给手柄时按**硬件单位 2 Hz/格**折半，见 ds5map_s2t_effect()） */
    uint8_t freq_r;          /* [9] 右 */
    /* --- R3.25（用户 2026-10-17："这些功能全部做到层里面"）：**整套扳机设置按层**，
     *     L2/R2 的所有设置都跟着当前层走，和按键映射一个规矩。
     *     [10]/[12]/[13] 和上面 [0..9] 是**主层（层 0）**那一份 —— 位置一个字节都没动，
     *     老网页 / 老工具照旧按老偏移读写主层；覆盖层 1..4 的那份追加在结构体末尾。 --- */
    uint8_t trig_mode;       /* [10] 主层：bit0 = 左两段 / bit1 = 右两段。固件只存不解释 */
    uint8_t flags;           /* [11] 主层两个行为位（网页「自适应扳机」里那两个开关）：
                              *   bit0 = 忽略游戏自己下发的原生扳机效果（默认 0 = 游戏优先）
                              *   bit1 = 压穿时释放一段（默认 0 = 一段仍然有效） */
    uint8_t bound_l;         /* [12] 主层：**两段判定的分界点**（左）——
                              * 它和 [0] 的手感起始位置是两件事：一个是"一次逻辑还是两次"，
                              * 一个是手感。0 = 没设过 → 回落到手感起始位置（开箱默认） */
    uint8_t bound_r;         /* [13] 主层（右） */
    uint8_t t2_mode[MAP_SHIFT_LAYERS];   /* [14..17] 覆盖层 1..4：bit0 = 左两段 / bit1 = 右两段 */
    uint8_t t2_bnd_l[MAP_SHIFT_LAYERS];  /* [18..21] 覆盖层 1..4 的分界点（左），0 = 回落起始位置 */
    uint8_t t2_bnd_r[MAP_SHIFT_LAYERS];  /* [22..25] 覆盖层 1..4 的分界点（右） */
    /* [26..65] 覆盖层 1..4 各自那一套「自适应扳机」参数。行 = 覆盖层-1，列 = S2T_FX_*。
     * 顺序必须和网页 page-map.js 的 FX_KEYS / packRegions 一致（tools/test_s2t_layout.js 钉住）。 */
    uint8_t fx[MAP_SHIFT_LAYERS][S2T_FX_COUNT];
} map_s2t_t;                 /* 66 B；整套扳机设置**每层一套**（主层 + 4 覆盖层） */

/* ------------------------------------------------------------------ */
/* Flick Stick（R2.5 落位结构体，R2.6 实现；默认关）                      */
/* ------------------------------------------------------------------ */
/* 右摇杆拨向哪个方向人物就转向哪里（**角度增量积分**，免校准）：拨杆瞬间方向改变多少，
 * 视角就转多少；停在某个角度不动＝不转。精细瞄准交给陀螺仪（两路鼠标增量在 synth_mouse
 * 里相加）。**接管期间右摇杆会被写回 0x80**，否则游戏里摇杆+鼠标双重输入、视角乱飘。 */
typedef struct {
    uint8_t  enable_mode;    /* enum map_gyro_en 同语义（0 关 = 默认） */
    uint8_t  enable_btn;     /* enum map_btn；0xFF = 未绑定 */
    uint8_t  r_on;           /* 激活半径 %，默认 60 */
    uint8_t  y_mode;         /* 0 关（默认）/ 1 比例俯仰 / 2 反向比例 */
    uint16_t sens;           /* Q4（16 = 1.0×）；换算见 ds5map.c 的 flick_apply()：
                              * 每弧度 = sens/2 个鼠标计数 ⇒ 默认 1024 时 180° ≈ 1608 counts。
                              * 默认值待实机调（设计稿 §3.3 原话）。 */
    uint8_t  r_off;          /* 迟滞半径 %，默认 50（< r_on） */
    uint8_t  rsv[1];
} map_flick_t;               /* 8 B；全局一份，不分层 */

/* ------------------------------------------------------------------ */
/* 体感手势（R2.7 上线，region 与结构体 R2.5 就落位）                     */
/* ------------------------------------------------------------------ */
/* 按住 src_btn + 朝一个方向快速甩一下手柄 = 触发那个方向的脉冲动作。
 * 只用角速度（摇晃 ≠ 转动，加速度计误触率高）。 */
typedef struct {
    uint8_t    src_btn;      /* 按住键 enum map_btn；0xFF = 手势关（默认） */
    uint8_t    kind_mask;    /* bit0..3 = 上/下/左/右 启用，默认 0x0F */
    uint16_t   thresh;       /* 触发角速度阈值 °/s，默认 300 */
    map_slot_t act[4];       /* 上/下/左/右 各一个脉冲动作（5 B × 4） */
    uint8_t    rsv[4];
} map_gest_t;                /* 28 B；全局一份，不分层 */

/* ------------------------------------------------------------------ */
/* 《绝地潜兵 2》战备宏：方向序列表                                          */
/* ------------------------------------------------------------------ */
/* 游戏里最长 9 个方向，所以一个方向一个字节（不做位压缩），
 * 32 个槽位只有 352 B，塞进配置体比省这点 Flash 划算。 */
#define MAP_STRAT_COUNT   32   /* 战备序列槽位数 */
#define MAP_STRAT_MAX_LEN 10   /* 单条序列最多几个方向 */

enum map_strat_dir {
    MAP_STRAT_UP    = 0,
    MAP_STRAT_DOWN  = 1,
    MAP_STRAT_LEFT  = 2,
    MAP_STRAT_RIGHT = 3,
};

typedef struct {
    uint8_t len;                       /* 0 = 空槽 */
    uint8_t dir[MAP_STRAT_MAX_LEN];    /* enum map_strat_dir */
} map_strat_t;                         /* 11 bytes */

/* ------------------------------------------------------------------ */
/* Macros                                                               */
/* ------------------------------------------------------------------ */
#define MAP_MACRO_MAX_COUNT 16
#define MAP_MACRO_NAME_LEN  24    /* UTF-8 in header, truncated */
#define MAP_MACRO_MAX_STEPS 640
#define MAP_MACRO_MAX_BYTES 4096  /* per-macro blob cap (header+steps+crc) */

#define MAP_MACRO_MAGIC 0x4D414331 /* 'MAC1' */

enum map_macro_trigger {
    MAP_MACRO_TRIG_ONCE  = 0,  /* 按下触发一次: play fully (repeat N times) */
    MAP_MACRO_TRIG_HOLD  = 1,  /* 按住循环: loop while trigger held, stop on release */
};

enum map_macro_act {
    MACRO_ACT_END   = 0,
    MACRO_ACT_KEY   = 1,   /* key = HID keycode   */
    MACRO_ACT_MOUSE = 2,   /* key = 0 left / 1 right / 2 middle / 3 wheel_up / 4 wheel_down */
    MACRO_ACT_PAD   = 3,   /* key = enum map_btn (gamepad button output) */
    /* stick/trigger analog actions reserved */
};

#define MACRO_EDGE_UP   0
#define MACRO_EDGE_DOWN 1

/* Each step = one edge event + delay until next event (recorded timeline). */
typedef struct {
    uint8_t  act;    /* enum map_macro_act */
    uint8_t  key;
    uint8_t  edge;   /* MACRO_EDGE_* */
    uint8_t  rsv;
    uint16_t dt_ms;  /* ms from this event to the next event */
} map_macro_step_t;  /* 6 bytes */

typedef struct {
    uint32_t magic;            /* MAP_MACRO_MAGIC */
    uint8_t  ver;
    uint8_t  trigger;          /* enum map_macro_trigger */
    uint8_t  repeat;           /* whole-macro repeat count, 1..255 */
    uint8_t  rsv;
    uint16_t repeat_pause_ms;  /* pause between repeats */
    uint16_t step_count;
    char     name[MAP_MACRO_NAME_LEN];
    /* map_macro_step_t steps[step_count]; uint16 crc16 (ccitt, over whole blob minus crc) */
} map_macro_hdr_t;  /* 40 bytes */

/* Directory entry (RAM cache + MAP_REG_MACRO_DIR wire format), 28 bytes */
typedef struct {
    uint8_t  id;               /* 0..MAP_MACRO_MAX_COUNT-1 */
    uint8_t  flags;            /* bit0: staged (modified, not yet committed) */
    uint16_t size;             /* blob size in bytes */
    char     name[MAP_MACRO_NAME_LEN];
} map_macro_dir_t;

/* ------------------------------------------------------------------ */
/* RPC protocol — USB feature report 0xFC (127-byte payload)            */
/*   SET_FEATURE 0xFC = request, GET_FEATURE 0xFC = response            */
/*                                                                       */
/*   request:  [0]=op [1]=region [2..3]=offset(u16 LE) [4]=len [5..]=data */
/*   response: [0]=status(0=ok,1=err,2=pending) [1]=op [2]=region        */
/*             [3..4]=offset [5..]=data                                  */
/*                                                                       */
/*   Writes stage into RAM + mark dirty; OP_COMMIT flushes to flash      */
/*   (handled in task context, response to a flash-touching op may come  */
/*   one poll later — retry GET while status==2). Tag echo fields let    */
/*   the host correlate responses with requests.                         */
/* ------------------------------------------------------------------ */
#define MAP_RPT_RPC 0xFC

#define MAP_RPC_REPORT_LEN 127
#define MAP_RPC_CHUNK_MAX  122  /* 127 - 5 header bytes */

enum map_op {
    MAP_OP_READ    = 1,
    MAP_OP_WRITE   = 2,
    MAP_OP_COMMIT  = 3,
    MAP_OP_ERASE   = 4,   /* MACRO region: offset = macro id */
    MAP_OP_ACTION  = 5,   /* 纯动作，不读写数据：region + off 决定干什么。
                           * 目前只有 MAP_REG_GYRO 用它，off = enum map_gyro_action */
};

/* MAP_OP_ACTION + MAP_REG_GYRO 的动作码 */
enum map_gyro_action {
    MAP_GYRO_ACT_CAL_START  = 1,   /* 开始静置校准（手柄放平别动 3 秒） */
    MAP_GYRO_ACT_CAL_CANCEL = 2,   /* 取消 */
};

/* 映射引擎接入总开关（加法式移植用）。
 *   0 = 引擎一行都不执行（LTO 会把引擎代码剪掉，行为等于增量 1）
 *   1 = 只打开 ds5map_init()
 *   2 = 再打开 ds5map_process()（每帧）
 *
 * 【重要】本头文件**故意不定义**这个宏。曾经在这里写过
 *   #ifndef DS5MAP_ENGINE_ENABLE / #define DS5MAP_ENGINE_ENABLE 0 / #endif
 * 结果 main.c 里用 #ifndef 兜底的 "#define ... 2" 永远不生效，
 * 开关其实一直是 0，引擎被整段优化掉，排查了很久。
 * 真正的取值由 main.c 定义（并 -D 覆盖）；其它文件用下面的兜底。 */
#if !defined(DS5MAP_ENGINE_ENABLE) && defined(DS5MAP_ENGINE_ENABLE_FALLBACK_0)
#define DS5MAP_ENGINE_ENABLE 0
#endif

enum map_reg {
    MAP_REG_META      = 0x00, /* magic/ver + map_timing_t + layer names[5][16] (92 B) */
    MAP_REG_SLOTS     = 0x01, /* dense map_slot_t[MAP_LAYERS][MAP_BTN_COUNT][6]
                               *  = 5×41×6×5 = 6150 B（R2.5：MAP_BTN_COUNT 39→41 之后） */
    /* 0x02（旧 map_passthru_t，早已停用）与 0x04（摇杆曲线区）两个 region 号都
     * 已删除：它们落到 default → size 0 / ptr NULL，RPC 一律回 MAP_RPC_ERR。 */
    MAP_REG_LAYERBIND = 0x03, /* map_layer_bind_t[MAP_SHIFT_LAYERS] (16 B) */
    MAP_REG_MACRO_DIR = 0x05, /* read: map_macro_dir_t[count] (28 B each) */
    MAP_REG_MACRO     = 0x06, /* offset = id<<12 | byte offset; WRITE/READ/ERASE/COMMIT */
    MAP_REG_GYRO      = 0x07, /* map_gyro_t (14 B): 陀螺仪，全局一份，不分层 */
    MAP_REG_TP        = 0x08, /* map_tp_t (18 B): 触摸板 单指目标 + 双指开合动作。
                               * 触摸板**不分层** —— 只有一块，永远看主层那份（见 ds5map.c 里
                               * 用 g_cfg.tp 的地方）。界面在覆盖层只给提示，不提供编辑。 */
    MAP_REG_VK        = 0x09, /* map_vk_t[MAP_VK_COUNT] (8 B): 虚拟组合键 V1..V4 的源键 */
    MAP_REG_STRAT     = 0x0A, /* map_strat_t[MAP_STRAT_COUNT] (352 B): 战备方向序列 */
    MAP_REG_S2T       = 0x0B, /* R2.5：map_s2t_t (66 B) 扳机：**整套设置按层**（R3.25）——
                               * [0..13] 是主层那份，[14..25] 覆盖层 1..4 的两段位/分界点，
                               * [26..65] 覆盖层 1..4 的自适应扳机参数（fx[][]，列号见 S2T_FX_*） */
    MAP_REG_FLICK     = 0x0C, /* R2.6：map_flick_t (8 B) Flick Stick（已实现，默认关） */
    MAP_REG_GEST      = 0x0D, /* R2.5 落位、R2.7 上线：map_gest_t (28 B) 体感手势，默认关 */
    MAP_REG_TLM_RAW   = 0x10, /* telemetry: last raw input payload (pre-mapping, 63 B) */
    MAP_REG_TLM_OUT   = 0x11, /* telemetry: last mapped payload (63 B) */
    MAP_REG_TLM_STATE = 0x12, /* telemetry: runtime state (32 B, see below) */
    MAP_REG_CRASH     = 0x13, /* 上次异常现场（SDK interrupt.c 的 noinit 记录，28 B，只读） */
    MAP_REG_DIAG      = 0x14,
    MAP_REG_VER       = 0x15, /* 固件版本标识（只读，40 B）：magic+构建标签+版本+开关 */
    MAP_REG_DIAG_PREV = 0x16, /* 上一次开机的 DIAG 环快照（只读，316 B，布局同 0x14；S30） */
    MAP_REG_PERF      = 0x17, /* S33：性能统计（只读，96 B，布局见 perf.h）：音频丢块/编码耗时/各任务 CPU */
    MAP_REG_CFG_ID    = 0x18, /* R2.4：配置身份牌（40 B，map_cfg_id_t，可读写）。WRITE 只改内存，
                               * COMMIT(0x18) 才落盘（单独的 cfgid.bin，不进 cfg.bin、不升 CFG_VER）。
                               * 老固件读这个区会回 MAP_RPC_ERR，网页当作「没有身份牌」。 */
    MAP_REG_STATUS    = 0x7F, /* config status (see below) */
};

/* MAP_REG_META layout (92 bytes):
 *   0    u16 magic 0x3543 ('C5')
 *   2    u8  ver
 *   3    u8  layers (== MAP_LAYERS)
 *   4    u16 hold_ms, 6 u16 dtap_ms, 8 u16 ttap_ms, 10 u16 tap_pulse_ms
 *   12   char layer_names[5][16]
 */
#define MAP_META_SIZE 92

/* MAP_REG_STATUS response data layout (proto ver 2.0):
 *   0    u16 proto ver
 *   2    u8  flags: bit0 cfg dirty, bit1 macro stage busy, bit2 any mapping,
 *                   bit3 has kbd outputs, bit4 has mouse outputs
 *   3    u8  active shift-layer mask (bits 1..4)
 *   4    u8  top layer id
 *   5    u8  macro count
 *   6    u32 macro free bytes
 *   10   u8  macro id max
 *   11   u8  active macro id + 1 (0 = none)
 *   12   u16 reserved
 */
#define MAP_STATUS_SIZE 16

/* MAP_REG_TLM_STATE layout (64 bytes):
 *   0    u16 seq (increments per processed input report)
 *   2..  same as STATUS flags/mask/top/macro fields from offset 2 (10 B)
 *   12   u8  keyboard report modifier bitmap, as last sent (bit0 左Ctrl .. bit7 右Win)
 *   13   u8  reserved
 *   14   u8  keyboard keycodes[6] (HID usage ids, 0 = 空槽)
 *   20   u8  mouse buttons bitmap (bit0 左键, bit1 右键, bit2 中键)
 *   21   i8  last wheel delta (+上滚 / -下滚)
 *   22   u8  wheel event counter (wraps; 变化即表示刚滚了一下)
 *   23   u8  触摸板单指目标 enum map_tp_single (0..4)
 *   24   u8  触摸板状态 bit0 单指在板, bit1 双指在板, bit2 摇杆目标正在覆盖输出
 *   25   u8  单指→摇杆 X (0..255，摇杆目标没生效时为 0)
 *   26   u8  单指→摇杆 Y
 *   27   u8  单指→十字方向位 (bit0..3 = 上/下/左/右)
 *   28   u8  虚拟组合键位 (bit0..3 = V1..V4)
 *   29   u8  陀螺仪 bit0 生效中, bit1..3 目标 enum map_gyro_target
 *   30   u8  张开触发计数 (wraps; 变化即表示刚触发了一次)
 *   31   u8  捏合触发计数
 *   32   i16 触摸板鼠标增量累计     ┐
 *   34   i16 触摸板鼠标增量累计     │ 这三个是一组「自上次读 TLM_STATE 以来
 *   36   i16 陀螺仪鼠标增量累计     │ 的累计量」，读完清零：主机 20Hz 轮询，
 *   38   i16 陀螺仪鼠标增量累计     │ 单帧增量落在两帧中间根本看不见
 *   40   i16 实际发出鼠标增量累计   │
 *   42   i16 实际发出鼠标增量累计   ┘
 *   44   u16 媒体键位图（R1 起；R2.2 扩到 16 位，小端。位号 = OUT_MEDIA 的 val：
 *            bit0 音量+ … bit6 停止、bit7 快进 bit8 快退 bit9 媒体播放器 bit10 计算器
 *            bit11 搜索 bit12 主页 bit13 后退 bit14 前进 bit15 刷新）
 *   46   u8  Flick Stick 状态位（**R2.7 计划，还没实现**）：bit0 本帧生效、bit1 摇杆在激活圈里、
 *            bit2 配了启用模式 —— 测试页/命令行脚本看它判断"接管没有"
 *   47   u8  扳机效果状态（R2.6）：**位号跟配置区统一 —— bit0 = 左(L2)、bit1 = 右(R2)**，
 *            bit2 = 游戏正在管左、bit3 = 游戏正在管右（后两位 = 1 秒窗口内收到过游戏下发的效果，
 *            我们这时让位）—— "配了却没手感"就看它：bit0/1 亮＝装上了；bit2/3 亮＝游戏在抢
 *   48   u8  陀螺仪方向模式 enum map_gyro_grav（恒为 1 = 本体轴）
 *   49   u8  陀螺仪状态位，见 GYRO_FLAG_*（src/gyro.h）
 *   50   u8  零偏校准置信度 0..255
 *   51   u8  手动校准进度 0..100；255 = 没在校准
 *   52   i16 重力方向 X ┐
 *   54   i16 重力方向 Y │ Q14，16384 = 1.0；重力估计已删除，恒为 0（字段留给协议）
 *   56   i16 重力方向 Z ┘
 *   58   i16 陀螺仪零偏 X ┐
 *   60   i16 陀螺仪零偏 Y │ Q8，256 = 1 度/秒
 *   62   i16 陀螺仪零偏 Z ┘
 */
#define MAP_TLM_STATE_SIZE 64

/* R2.4：MAP_REG_CFG_ID —— 网页「游戏按键映射配置」写进来的这份配置是谁（2026-10-16 方案 B）。
 * 名字是身份（= 电脑上的文件名），哈希是内容指纹；固件只负责存着，不解释内容。
 *   0   char name[32]  配置名 UTF-8，最多 30 字节（= 10 个汉字），'\0' 结尾、后面补 0；
 *                      name[0] == 0 = 从没写过
 *   32  u32 saved_at   这份内容在电脑上保存的时刻（Unix 秒，电脑时钟；比较两边谁新用）
 *   36  u32 hash       网页 packRegions() 的 FNV-1a（写进来那一刻接收机里的内容） */
#define MAP_CFG_ID_NAME_MAX 30
typedef struct __attribute__((packed)) {
    char     name[32];
    uint32_t saved_at;
    uint32_t hash;
} map_cfg_id_t;

enum map_rpc_status {
    MAP_RPC_OK      = 0,
    MAP_RPC_ERR     = 1,
    MAP_RPC_PENDING = 2,
};

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */
void     ds5map_init(void);            /* mount FS, load cfg, start 1ms tick timer */
void     ds5map_load(void);            /* (re)load persisted config */
void     ds5map_process(uint8_t *payload, uint32_t now_ms);  /* per input report; mutates payload */
void     ds5map_on_bt_disconnect(void);      /* release all synthesized outputs */
void     ds5map_deferred(void);              /* task-context: flash commits, macro IO */
bool     ds5map_has_kbd_outputs(void);       /* USB keyboard-intf gating */
bool     ds5map_has_mouse_outputs(void);

/* R2.6 扳机效果（关/阻力/扳机/自动扳机）的 11 字节由这里算（配置在 g_cfg.s2t 里），
 * 由 main.c 在把 PC 输出报告转给手柄之前注入 —— 只有 main.c 知道游戏什么时候下发过效果。
 * side：0 = 右扳机（报告里的 [10..20]）/ 1 = 左扳机（[21..31]），见 DS5MAP_S2T_SIDE_*。
 * 返回 true = 这一侧现在要用我们的效果，11 字节写进 out11（out11[0] = 效果模式码）。
 * 返回 false = 别动这一侧的字节（关 / 这一侧没开）。 */
#define DS5MAP_S2T_SIDE_R 0
#define DS5MAP_S2T_SIDE_L 1
bool     ds5map_s2t_effect(uint8_t side, uint8_t out11[11]);
/* R2.8：这一侧要不要周期重发（只有「自动扳机」＝VIBRATION 会衰减，真游戏每 12~51 ms 重发一次） */
bool     ds5map_s2t_needs_refresh(uint8_t side);
/* R2.9：配置改动后请求"立刻把手柄状态重发一次"（不然要断电上电才生效） */
void     ds5map_s2t_request_resend(void);
bool     ds5map_s2t_take_resend(void);
/* R3.25b：切层用「不 forget」的重发 —— 新层没有效果时必须能走到「撤下」分支
 * （forget 版会把 s2t_applied 清掉，撤下就发不出去，手上会粘着上一层的手感） */
void     ds5map_s2t_request_resend_nf(void);
bool     ds5map_s2t_take_resend_nf(void);
bool     ds5map_s2t_any_effect(void);
/* R3.0：网页「自适应扳机」那两个行为开关（存在 `0x0b` 的 `[11]` 位里） */
bool     ds5map_s2t_ignore_game(void);   /* bit0：忽略游戏的原生扳机效果 */
bool     ds5map_s2t_release_s1(void);    /* bit1：压穿时释放一段 */
/* R3.2：两段判定的分界点（w：0=左 1=右）。和扳机效果的手感起始位置**无关**；
 * 0 = 回落到手感起始位置。
 * R3.25：现在是**按层**存的（主层在 [12]/[13]，覆盖层 1..4 在 [18..25]）——
 * 这两个函数取/写的是**当前激活层**（top_layer()）那一份，也就是固件真正会用的那个值。 */
uint8_t  ds5map_t2_bound(uint8_t w);
void     ds5map_t2_set_bound(uint8_t w, uint8_t v);
/* R3.25：指定层的那一份（layer 0 = 主层，1..4 = 覆盖层；超范围按主层算） */
uint8_t  ds5map_t2_bound_layer(uint8_t layer, uint8_t w);
uint8_t  ds5map_t2_mode_layer(uint8_t layer);
/* R2.6：main.c 每帧把扳机效果状态写进来（遥测 TLM_STATE[47] 原样抄，见上面布局注释） */
void     ds5map_s2t_set_state(uint8_t bits);

/* Feature report 0xFC. Request may run in USB ISR context (RAM-only ops),
 * flash-touching ops complete later in ds5map_deferred(); response is
 * fetched by ds5map_rpc_response() from the GET_REPORT path. */
void     ds5map_rpc_request(const uint8_t *req, uint16_t len);
uint16_t ds5map_rpc_response(uint8_t *out, uint16_t max);
