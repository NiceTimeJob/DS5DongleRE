#ifndef DS5MAP_DIAG_H_
#define DS5MAP_DIAG_H_

/*
 * 诊断事件环形日志（MAP_REG_DIAG = 0x14）。
 *
 * 本机没有可用的串口日志通道（设备只枚举成 HID），所以把蓝牙状态机 / USB 侧
 * 的关键事件记在一块软复位不丢的 RAM 里，主机用 0xFC RPC 分块读回来。
 * 「手柄连上几秒就断」这类问题必须靠它定位：能看到状态机走到哪一步、
 * ACL 断开的 HCI reason、安全协商结果、有没有收到过输入报告。
 *
 * 不依赖 ds5map（ds5map.c 与本文件互不包含，只有本文件被两边引用）。
 */

#include <stdint.h>

/* 事件码。arg1/arg2 的含义见 diag.c 里的注释。 */
enum diag_ev {
    DIAG_EV_NONE = 0,
    DIAG_EV_BOOT,
    DIAG_EV_STATE,        /* arg1 = 旧状态, arg2 = 新状态 */
    DIAG_EV_ACL_UP,       /* arg1 = handle, arg2 = 1 主动连接 / 0 被动 */
    DIAG_EV_ACL_DOWN,     /* arg1 = reason(HCI), arg2 = bonded_count */
    DIAG_EV_SECURITY,     /* arg1 = level, arg2 = err */
    DIAG_EV_L2CAP_CTRL_UP,
    DIAG_EV_L2CAP_CTRL_DOWN,
    DIAG_EV_L2CAP_INTR_UP,
    DIAG_EV_L2CAP_INTR_DOWN,
    DIAG_EV_SDP_DONE,     /* arg1 = HID 服务找到(1/0) */
    DIAG_EV_HANDSHAKE,    /* arg1 = 已发条数, arg2 = send_failed */
    DIAG_EV_HANDSHAKE_OK, /* arg1 = is_dse */
    DIAG_EV_INPUT_FIRST,  /* arg1 = len */
    DIAG_EV_INPUT,        /* arg1 = len（只在长时间没输入后重记一次） */
    DIAG_EV_INTR_RECV,    /* arg1 = trans, arg2 = param */
    DIAG_EV_WD_SEC_OK,    /* 加密已就绪，安全看门狗放行 */
    DIAG_EV_WD_SEC_FAIL,  /* 安全 8 s 超时：清 link key 并断开 */
    DIAG_EV_WD_CONN,      /* main.c 3 s 无输入看门狗触发强制断开 */
    DIAG_EV_SCAN,         /* arg1 = 1 开始扫描 / 0 结束 */
    DIAG_EV_PAIRMODE,     /* arg1 = 1 进入对频模式 */
    DIAG_EV_BTN,          /* arg1 = 1 按下 / 0 松开 */
    DIAG_EV_L2CAP_FALLBACK,
    DIAG_EV_FORCE_DISC,
    DIAG_EV_RESET,        /* 代码里主动复位 */
    DIAG_EV_USB_INIT,     /* arg1 = usb_gamepad_init 返回值 */
    DIAG_EV_USB_DISC,     /* 软摘除（置 UNPLUG 位）；arg1 = 1 调用点A / 2 调用点B */
    DIAG_EV_USB_CONN,     /* 软接回；arg1 = 原来的 0x114 值 */
    DIAG_EV_RST_REASON,   /* arg1 低 8 位 = BL_RST_REASON_E：0 上电(保持区丢失=真断电) / 1 来历不明的热复位
                           *   (RAM 保留但无人标记) / 2 异常 / 3 软件故障钩子 / 4 软复位 / 5 HBN / 6 掉电 BOD；
                           * arg1 位 8..15 = HBN 原始复位事件位（bit4 = BOD）。S30 起 bl_sys_rstinfo_init() 真的被调用了 */
    DIAG_EV_SCAN_STUCK,   /* S31：SCANNING 卡满 30 s 被强制结束；arg1 = 卡了多少 ms */
    DIAG_EV_HB_SCAN_FIX,  /* S31：等待连接时心跳发现 page scan 没开，已补开；arg1 = 读到的 Scan_Enable */
    DIAG_EV_USB_SUSPEND,  /* S31：arg1 = 1 主机挂起 / 2 挂起 5 s 去抖到期（手柄关机 + 射频静默） */
    DIAG_EV_USB_RESUME,   /* S31：arg1 = 1 主机恢复 / 2 重新配置 */
    DIAG_EV_DISC_REQ,     /* S32：本机主动发起断开；arg1 = 来源编号（enum bt_disc_site），arg2 = 当时状态机状态 */
    DIAG_EV_AUDIO,        /* S32：arg1 = 1 音频流开始 / 2 结束 / 3 发送连续失败近阈值 / 4 连败超限强制断开；arg2 = 连败次数 */
    DIAG_EV_SCAN_SET,     /* S37：arg1 = 1 连上后关 page/inquiry scan / 2 断开后重开 */
    DIAG_EV_STALL,        /* R1.3：连着时输入停了 ≥1 s；arg1 = 当时的链路诊断（rssi | 质量<<8 | AFH 信道<<16 | AFH 模式<<24） */
    DIAG_EV_STALL_END,    /* R1.3：停顿后输入恢复；arg1 = 停了多少 ms */
    DIAG_EV_LINK_PARAM,   /* R1.4：连上后一次；arg1 = 监督超时(slot) | 角色<<16 | 写 10 s 的结果<<24 */
    DIAG_EV_HCI_EVT,      /* R1.5：连着时收到的普通 HCI 事件；arg1 = 事件码 | 挑出的 3 字节 <<8/<<16/<<24 */
    DIAG_EV_AFH,          /* R1.5：AFH 信道表变了；arg1 = 在用数 | 被排除最低信道<<8 | 最高<<16 | 模式<<24 */
    DIAG_EV_ROLE_SW,      /* R1.5：申请当主；arg1 = 原链路策略 | 写策略结果<<16 | 切换命令结果<<24 */
    DIAG_EV_AUTO_OFF,     /* R1.7：手柄自动省电关机；arg1 = 配置的分钟数 */
    DIAG_EV_ACT_FIX,      /* R1.12：活动计时器发现"未来值"（RAM 被野写的唯一可能），已自愈；
                           * arg1 = 抓到的脏值原样（排查野写来源的物证） */
    DIAG_EV_MAX
};

/* 复位原因（对应 BL_RST_REASON_E） */
#define DIAG_RST_POWER_OFF   0
#define DIAG_RST_HW_WDT      1
#define DIAG_RST_EXCEPTION   2
#define DIAG_RST_SW_WDT      3
#define DIAG_RST_SOFTWARE    4
#define DIAG_RST_HBN         5
#define DIAG_RST_BOD         6   /* 掉电检测 —— 供电不足的典型特征 */

#define DIAG_REC_SZ  12
#define DIAG_SLOTS   25
#define DIAG_SIZE    (16 + DIAG_REC_SZ * DIAG_SLOTS)   /* 316 B */

/* 记一条事件。可从任何上下文调用（ISR 安全：只写 RAM + 读 mtimer）。 */
void    diag_log(uint8_t ev, uint32_t arg1, uint32_t arg2);

/* 把整块日志（含头）拷进 out（>= DIAG_SIZE 字节）。ISR 安全。 */
void    diag_read(uint8_t *out);

/* 按 offset 拷一段（0..DIAG_SIZE）。out 至少 n 字节，n 建议 <= 32：
 * 这个函数在 USB ISR 里被调用，调用方不该为了读一小段而在中断栈上开大缓冲。 */
void    diag_read_at(uint8_t *out, uint16_t off, uint16_t n);

/* 上一次开机的整块日志快照（MAP_REG_DIAG_PREV = 0x16，布局同 DIAG，316 B）：
 * diag_init() 在清环之前把旧表头+记录拷过来；表头 magic 不对 = 上次 RAM 没保住（冷启动）。 */
void    diag_read_prev_at(uint8_t *out, uint16_t off, uint16_t n);

/* 头/记录的直接读接口（避免调用方在中断栈上开缓冲）。 */
void    diag_read_hdr(void *out16);              /* diag_hdr_t 的 16 字节 */
void    diag_read_rec(int idx, void *out12);     /* idx 0..DIAG_SLOTS-1，12 字节 */

/* 开机初始化：清空并把 DIAG_EV_BOOT 记成第一条。 */
void    diag_init(uint32_t boot_count);

#endif /* DS5MAP_DIAG_H_ */
