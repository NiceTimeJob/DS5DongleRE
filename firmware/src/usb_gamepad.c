#include "usb_gamepad.h"
#include "compiler/compiler_ld.h"
#include "ds5_usb_audio.h"
#include "audio.h"
#include "bt_hid_host.h"
#include "ds5_protocol.h"
#include "dse.h"
#include "config.h"
#include "state_mgr.h"
#include "ds5map.h"
#include "diag.h"        /* S30: 主动复位记进 DIAG */
#include "bl616_sys.h"   /* S30: bl_sys_rstinfo_set */
#include "bflb_l1c.h"     /* R1：复位前把 D-cache 刷回 RAM */

/* ===== RPC / 诊断通道子开关（逐步打开，一次一个）=====
 *   RPT_0FC   : 报告描述符加 0xFC feature 项
 *   RPC_HANDL : GET_REPORT 的 0xFC 分支（读取）
 *   SET_REPORT: SET_REPORT 的 0xFC 分支（写入）
 *   GATING    : 键鼠接口现在**永远注册**（2026-10-11）；只有 remote-wake 属性看配置
 * R4 只打开 RPT_0FC。 */
#ifndef DS5MAP_RPT_0FC_ENABLE
#define DS5MAP_RPT_0FC_ENABLE 1
#endif
#ifndef DS5MAP_RPC_HANDLER_ENABLE
#define DS5MAP_RPC_HANDLER_ENABLE 1
#endif
#ifndef DS5MAP_SET_REPORT_ENABLE
#define DS5MAP_SET_REPORT_ENABLE 1
#endif
#ifndef DS5MAP_GATING_ENABLE
#define DS5MAP_GATING_ENABLE 1
#endif
#include "usbd_core.h"
#include "usbd_hid.h"
#include "bl616_glb.h"
#include "bflb_efuse.h"
#include "bflb_mtimer.h"
#include "board.h"
#include <string.h>
#include "board_config.h"
#include "debug_log.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

/* 版本串统一由 src/version.h 派生（R1 起），这里不再手写 */
#include "version.h"
#define FIRMWARE_VERSION FW_VERSION_STRING

#define USBD_MAX_POWER      250
#define USBD_LANGID         0x0409

/* Config(9) + Audio(186) + Gamepad(9+9+7) + Keyboard(9+9+7) = 245 */
#define USB_KBD_DESC_SIZE  25   /* intf(9) + HID(9) + EP_IN(7) */
#define USB_HID_ONLY_SIZE (9 + 9 + 7 + USB_KBD_DESC_SIZE)
#define USB_AUDIO_DESC_SIZE 186
#define USB_HID_CONFIG_SIZE (9 + USB_AUDIO_DESC_SIZE + USB_HID_ONLY_SIZE)
/* S35：S33 删掉 0xFB（8 字节）后这里没跟着减，数组定长零填充 → 报告描述符尾部多出 8 个 0x00
 * → Windows 给手柄接口报 Code 10（无法启动），键鼠/音频接口照常。数值必须等于下面初始化列表的字节数。 */
#if DS5MAP_RPT_0FC_ENABLE
#define HID_REPORT_DESC_SIZE_DS  329
#else
#define HID_REPORT_DESC_SIZE_DS  321
#endif
#define KBD_REPORT_DESC_SIZE 165  /* Keyboard + Consumer(16 位媒体/多功能键, R2.2) + Mouse；= 初始化列表字节数，改描述符必须同步改 */

static volatile bool usb_config_save_pending = false;
static volatile bool usb_reset_pending = false;
static bool first_usb_send_logged = false;

#define SET_REPORT_MAX_DATA    64
#define SET_REPORT_QUEUE_DEPTH 4

typedef struct {
    uint8_t  report_id;
    uint16_t len;
    bool     is_dse;
    uint8_t  data[SET_REPORT_MAX_DATA];
} set_report_entry_t;

static QueueHandle_t set_report_queue = NULL;

/* Pending feature GET request, deferred from ISR to task context.
 * 0 = no pending request. Written by ISR, consumed by process_deferred(). */
static volatile uint8_t pending_feature_rid = 0;

/* DualSense USB HID Report Descriptor (321 bytes) */
static const uint8_t hid_report_desc_ds[HID_REPORT_DESC_SIZE_DS] = {
    0x05, 0x01,       /* Usage Page (Generic Desktop Ctrls) */
    0x09, 0x05,       /* Usage (Game Pad) */
    0xA1, 0x01,       /* Collection (Application) */

    /* Input Report 0x01 -------------------------------------------- */
    0x85, 0x01,       /*   Report ID (1) */
    0x09, 0x30,       /*   Usage (X)  — LX */
    0x09, 0x31,       /*   Usage (Y)  — LY */
    0x09, 0x32,       /*   Usage (Z)  — RX */
    0x09, 0x35,       /*   Usage (Rz) — RY */
    0x09, 0x33,       /*   Usage (Rx) — L2 trigger */
    0x09, 0x34,       /*   Usage (Ry) — R2 trigger */
    0x15, 0x00,       /*   Logical Minimum (0) */
    0x26, 0xFF, 0x00, /*   Logical Maximum (255) */
    0x75, 0x08,       /*   Report Size (8) */
    0x95, 0x06,       /*   Report Count (6) */
    0x81, 0x02,       /*   Input (Data,Var,Abs) */
    0x06, 0x00, 0xFF, /*   Usage Page (Vendor 0xFF00) */
    0x09, 0x20,       /*   Usage (0x20) — counter byte */
    0x95, 0x01,       /*   Report Count (1) */
    0x81, 0x02,       /*   Input (Data,Var,Abs) */
    0x05, 0x01,       /*   Usage Page (Generic Desktop Ctrls) */
    0x09, 0x39,       /*   Usage (Hat switch) */
    0x15, 0x00,       /*   Logical Minimum (0) */
    0x25, 0x07,       /*   Logical Maximum (7) */
    0x35, 0x00,       /*   Physical Minimum (0) */
    0x46, 0x3B, 0x01, /*   Physical Maximum (315) */
    0x65, 0x14,       /*   Unit (Degrees) */
    0x75, 0x04,       /*   Report Size (4) */
    0x95, 0x01,       /*   Report Count (1) */
    0x81, 0x42,       /*   Input (Data,Var,Abs,Null) */
    0x65, 0x00,       /*   Unit (None) */
    0x05, 0x09,       /*   Usage Page (Button) */
    0x19, 0x01,       /*   Usage Minimum (0x01) */
    0x29, 0x0F,       /*   Usage Maximum (0x0F) — 15 buttons */
    0x15, 0x00,       /*   Logical Minimum (0) */
    0x25, 0x01,       /*   Logical Maximum (1) */
    0x75, 0x01,       /*   Report Size (1) */
    0x95, 0x0F,       /*   Report Count (15) */
    0x81, 0x02,       /*   Input (Data,Var,Abs) */
    0x06, 0x00, 0xFF, /*   Usage Page (Vendor 0xFF00) */
    0x09, 0x21,       /*   Usage (0x21) — 13 vendor bits (padding+flags) */
    0x95, 0x0D,       /*   Report Count (13) */
    0x81, 0x02,       /*   Input (Data,Var,Abs) */
    0x06, 0x00, 0xFF, /*   Usage Page (Vendor 0xFF00) */
    0x09, 0x22,       /*   Usage (0x22) — sensor/touch/battery blob */
    0x15, 0x00,       /*   Logical Minimum (0) */
    0x26, 0xFF, 0x00, /*   Logical Maximum (255) */
    0x75, 0x08,       /*   Report Size (8) */
    0x95, 0x34,       /*   Report Count (52) */
    0x81, 0x02,       /*   Input (Data,Var,Abs) */

    /* Output Report 0x02 (47 bytes) -------------------------------- */
    0x85, 0x02,       /*   Report ID (2) */
    0x09, 0x23,       /*   Usage (0x23) */
    0x95, 0x2F,       /*   Report Count (47) */
    0x91, 0x02,       /*   Output (Data,Var,Abs) */

    /* Feature Reports ---------------------------------------------- */
    0x85, 0x05,       /*   Report ID (5) — calibration */
    0x09, 0x33,
    0x95, 0x28,       /*   Report Count (40) */
    0xB1, 0x02,
    0x85, 0x08,       /*   Report ID (8) */
    0x09, 0x34,
    0x95, 0x2F,       /*   Report Count (47) */
    0xB1, 0x02,
    0x85, 0x09,       /*   Report ID (9) — pairing info */
    0x09, 0x24,
    0x95, 0x13,       /*   Report Count (19) */
    0xB1, 0x02,
    0x85, 0x0A,       /*   Report ID (10) */
    0x09, 0x25,
    0x95, 0x1A,       /*   Report Count (26) */
    0xB1, 0x02,
    0x85, 0x0B,       /*   Report ID (11) */
    0x09, 0x41,
    0x95, 0x29,       /*   Report Count (41) */
    0xB1, 0x02,
    0x85, 0x0C,       /*   Report ID (12) */
    0x09, 0x42,
    0x95, 0x29,       /*   Report Count (41) */
    0xB1, 0x02,
    0x85, 0x20,       /*   Report ID (32) — firmware info */
    0x09, 0x26,
    0x95, 0x3F,       /*   Report Count (63) */
    0xB1, 0x02,
    0x85, 0x21,       /*   Report ID (33) */
    0x09, 0x27,
    0x95, 0x04,       /*   Report Count (4) */
    0xB1, 0x02,
    0x85, 0x22,       /*   Report ID (34) */
    0x09, 0x40,
    0x95, 0x3F,       /*   Report Count (63) */
    0xB1, 0x02,
    0x85, 0x80,       /*   Report ID (128) */
    0x09, 0x28,
    0x95, 0x3F,
    0xB1, 0x02,
    0x85, 0x81,       /*   Report ID (129) */
    0x09, 0x29,
    0x95, 0x3F,
    0xB1, 0x02,
    0x85, 0x82,       /*   Report ID (130) */
    0x09, 0x2A,
    0x95, 0x09,       /*   Report Count (9) */
    0xB1, 0x02,
    0x85, 0x83,       /*   Report ID (131) */
    0x09, 0x2B,
    0x95, 0x3F,
    0xB1, 0x02,
    0x85, 0x84,       /*   Report ID (132) */
    0x09, 0x2C,
    0x95, 0x3F,
    0xB1, 0x02,
    0x85, 0x85,       /*   Report ID (133) */
    0x09, 0x2D,
    0x95, 0x02,       /*   Report Count (2) */
    0xB1, 0x02,
    0x85, 0xA0,       /*   Report ID (160) */
    0x09, 0x2E,
    0x95, 0x01,       /*   Report Count (1) */
    0xB1, 0x02,
    0x85, 0xE0,       /*   Report ID (224) */
    0x09, 0x2F,
    0x95, 0x3F,
    0xB1, 0x02,
    0x85, 0xF0,       /*   Report ID (240) */
    0x09, 0x30,
    0x95, 0x3F,
    0xB1, 0x02,
    0x85, 0xF1,       /*   Report ID (241) */
    0x09, 0x31,
    0x95, 0x3F,
    0xB1, 0x02,
    0x85, 0xF2,       /*   Report ID (242) */
    0x09, 0x32,
    0x95, 0x0F,       /*   Report Count (15) */
    0xB1, 0x02,
    0x85, 0xF4,       /*   Report ID (244) */
    0x09, 0x35,
    0x95, 0x3F,
    0xB1, 0x02,
    0x85, 0xF5,       /*   Report ID (245) */
    0x09, 0x36,
    0x95, 0x03,       /*   Report Count (3) */
    0xB1, 0x02,
    0x85, 0xF6,       /*   Report ID (246) */
    0x09, 0x37,
    0x95, 0x3F,
    0xB1, 0x02,
    0x85, 0xF7,       /*   Report ID (247) */
    0x09, 0x38,
    0x95, 0x3F,
    0xB1, 0x02,
    0x85, 0xF8,       /*   Report ID (248) */
    0x09, 0x39,
    0x95, 0x3F,
    0xB1, 0x02,
    0x85, 0xF9,       /*   Report ID (249) */
    0x09, 0x3A,
    0x95, 0x3F,
    0xB1, 0x02,
#if DS5MAP_RPT_0FC_ENABLE
    0x85, 0xFC,       /*   Report ID (252) — mapping config RPC */
    0x09, 0x3D,
    0x95, 0x7F,       /*   Report Count (127) */
    0xB1, 0x02,
#endif

    0xC0,             /* End Collection */
};

/* DualSense Edge 的报告描述符 + DSE 模式整套已按用户要求【彻底删除】
 * （2026-10-11，S15）：接收机在 PC 上永远以普通 DS5 出现（PID 054C:0CE6 +
 * 下面这份 DS 描述符），所以 current_dse_mode / usb_gamepad_set_dse_mode() /
 * hid_report_desc_dse 全都不需要了。DSE 手柄本身照常支持：它多出来的 4 个键
 * 在报文 p[9] 高 4 位里，引擎当映射/宏的输入源用（见 ds5map.c）。 */

/* Multi-TLC HID report descriptor: Keyboard (Report ID 1) + Consumer Control (Report ID 2) + Mouse (Report ID 3) */
static const uint8_t kbd_report_desc[KBD_REPORT_DESC_SIZE] = {
    /* ── Collection 1: Keyboard (Report ID 1, 9 bytes total) ── */
    0x05, 0x01,       /* Usage Page (Generic Desktop) */
    0x09, 0x06,       /* Usage (Keyboard) */
    0xA1, 0x01,       /* Collection (Application) */
    0x85, 0x01,       /*   Report ID (1) */
    0x05, 0x07,       /*   Usage Page (Keyboard/Keypad) */
    0x19, 0xE0,       /*   Usage Minimum (Left Control) */
    0x29, 0xE7,       /*   Usage Maximum (Right GUI) */
    0x15, 0x00,       /*   Logical Minimum (0) */
    0x25, 0x01,       /*   Logical Maximum (1) */
    0x75, 0x01,       /*   Report Size (1) */
    0x95, 0x08,       /*   Report Count (8) */
    0x81, 0x02,       /*   Input (Data,Var,Abs) — modifier byte */
    0x95, 0x01,       /*   Report Count (1) */
    0x75, 0x08,       /*   Report Size (8) */
    0x81, 0x01,       /*   Input (Const) — reserved byte */
    0x95, 0x06,       /*   Report Count (6) */
    0x75, 0x08,       /*   Report Size (8) */
    0x15, 0x00,       /*   Logical Minimum (0) */
    0x26, 0xFF, 0x00, /*   Logical Maximum (255) */
    0x05, 0x07,       /*   Usage Page (Keyboard/Keypad) */
    0x19, 0x00,       /*   Usage Minimum (0) */
    0x2A, 0xFF, 0x00, /*   Usage Maximum (255) */
    0x81, 0x00,       /*   Input (Data,Array) — 6 keycodes */
    0xC0,             /* End Collection */

    /* ── Collection 2: Consumer Control (Report ID 2, 2 bytes = 16 bits, no padding) ── */
    0x05, 0x0C,       /* Usage Page (Consumer) */
    0x09, 0x01,       /* Usage (Consumer Control) */
    0xA1, 0x01,       /* Collection (Application) */
    0x85, 0x02,       /*   Report ID (2) */
    0x15, 0x00,       /*   Logical Minimum (0) */
    0x25, 0x01,       /*   Logical Maximum (1) */
    0x75, 0x01,       /*   Report Size (1) */
    0x95, 0x10,       /*   Report Count (16) — R2.2：16 位媒体/多功能键（正好 2 字节，无填充位） */
    0x09, 0xE9,       /*   Usage (Volume Increment)    — bit 0  音量 + */
    0x09, 0xEA,       /*   Usage (Volume Decrement)    — bit 1  音量 − */
    0x09, 0xE2,       /*   Usage (Mute)                — bit 2  静音 */
    0x09, 0xCD,       /*   Usage (Play/Pause)          — bit 3  播放 / 暂停 */
    0x09, 0xB5,       /*   Usage (Scan Next Track)     — bit 4  下一曲 */
    0x09, 0xB6,       /*   Usage (Scan Previous Track) — bit 5  上一曲 */
    0x09, 0xB7,       /*   Usage (Stop)                — bit 6  停止 */
    0x09, 0xB3,       /*   Usage (Fast Forward)        — bit 7  快进 */
    0x09, 0xB4,       /*   Usage (Rewind)              — bit 8  快退 */
    0x0A, 0x83, 0x01, /*   Usage (AL Consumer Control Configuration) — bit 9  媒体播放器 */
    0x0A, 0x92, 0x01, /*   Usage (AL Calculator)       — bit 10 计算器 */
    0x0A, 0x21, 0x02, /*   Usage (AC Search)           — bit 11 搜索 */
    0x0A, 0x23, 0x02, /*   Usage (AC Home)             — bit 12 主页 */
    0x0A, 0x24, 0x02, /*   Usage (AC Back)             — bit 13 后退 */
    0x0A, 0x25, 0x02, /*   Usage (AC Forward)          — bit 14 前进 */
    0x0A, 0x27, 0x02, /*   Usage (AC Refresh)          — bit 15 刷新 */
    0x81, 0x02,       /*   Input (Data,Var,Abs) */
    0xC0,             /* End Collection */

    /* ── Collection 3: Mouse (Report ID 3, 4 bytes: btns + X + Y + wheel) ── */
    0x05, 0x01,       /* Usage Page (Generic Desktop) */
    0x09, 0x02,       /* Usage (Mouse) */
    0xA1, 0x01,       /* Collection (Application) */
    0x85, 0x03,       /*   Report ID (3) */
    0x09, 0x01,       /*   Usage (Pointer) */
    0xA1, 0x00,       /*   Collection (Physical) */
    0x05, 0x09,       /*   Usage Page (Button) */
    0x19, 0x01,       /*   Usage Minimum (1) */
    0x29, 0x03,       /*   Usage Maximum (3) */
    0x15, 0x00,       /*   Logical Minimum (0) */
    0x25, 0x01,       /*   Logical Maximum (1) */
    0x95, 0x03,       /*   Report Count (3) */
    0x75, 0x01,       /*   Report Size (1) */
    0x81, 0x02,       /*   Input (Data,Var,Abs) — 3 buttons */
    0x95, 0x01,       /*   Report Count (1) */
    0x75, 0x05,       /*   Report Size (5) */
    0x81, 0x01,       /*   Input (Const) — padding */
    0x05, 0x01,       /*   Usage Page (Generic Desktop) */
    0x09, 0x30,       /*   Usage (X) */
    0x09, 0x31,       /*   Usage (Y) */
    0x15, 0x81,       /*   Logical Minimum (-127) */
    0x25, 0x7F,       /*   Logical Maximum (127) */
    0x75, 0x08,       /*   Report Size (8) */
    0x95, 0x02,       /*   Report Count (2) */
    0x81, 0x06,       /*   Input (Data,Var,Rel) — X, Y */
    0x09, 0x38,       /*   Usage (Wheel) */
    0x95, 0x01,       /*   Report Count (1) */
    0x81, 0x06,       /*   Input (Data,Var,Rel) — Wheel */
    0xC0,             /*   End Collection (Physical) */
    0xC0,             /* End Collection (Application) */
};

static uint8_t device_desc[] = {
    0x12,                    /* bLength */
    0x01,                    /* bDescriptorType: Device */
    0x00, 0x02,              /* bcdUSB: 2.00 */
    0x00,                    /* bDeviceClass */
    0x00,                    /* bDeviceSubClass */
    0x00,                    /* bDeviceProtocol */
    0x40,                    /* bMaxPacketSize0 */
    (USB_GAMEPAD_VID & 0xFF), (USB_GAMEPAD_VID >> 8),
    (USB_GAMEPAD_PID & 0xFF), (USB_GAMEPAD_PID >> 8),
    0x00, 0x01,              /* bcdDevice: 1.00 */
    0x01,                    /* iManufacturer */
    0x02,                    /* iProduct */
    0x00,                    /* iSerialNumber: none */
    0x01,                    /* bNumConfigurations */
};

/* Byte offsets within config_desc for dynamic modification.
 * Audio desc = 186 bytes at offset 9, HID starts at 9+186 = 195.
 * Gamepad intf(9) at 195, HID desc(9) at 204, wDescriptorLength at byte 7 of HID = 211. */
#define AUDIO_DESC_OFFSET           9    /* audio descriptor starts after config header */
#define HID_SECTION_OFFSET          (AUDIO_DESC_OFFSET + USB_AUDIO_DESC_SIZE)  /* 195 */
#define BINTERVAL_GAMEPAD_EP_IN_OFF (HID_SECTION_OFFSET + 9 + 9 + 6) /* 219 */
#define BINTERVAL_GAMEPAD_EP_OUT_OFF (BINTERVAL_GAMEPAD_EP_IN_OFF + 7) /* 226 */
#define KBD_INTF_OFF  (BINTERVAL_GAMEPAD_EP_OUT_OFF + 1) /* 227: keyboard intf descriptor */
#define KBD_SUBCLASS_OFF (KBD_INTF_OFF + 6)  /* bInterfaceSubClass in kbd intf */
#define KBD_PROTOCOL_OFF (KBD_INTF_OFF + 7)  /* bInterfaceProtocol in kbd intf */

static uint8_t config_desc[USB_HID_CONFIG_SIZE] = {
    /* Configuration Descriptor (9 bytes) */
    0x09, 0x02,
    USB_HID_CONFIG_SIZE & 0xFF, (USB_HID_CONFIG_SIZE >> 8) & 0xFF,
    0x05,                    /* bNumInterfaces: AC + AS_SPK + AS_MIC + Gamepad + Kbd */
    0x01,                    /* bConfigurationValue */
    0x00,                    /* iConfiguration */
    0xE0,                    /* bmAttributes: self-powered + remote wakeup */
    USBD_MAX_POWER,          /* bMaxPower: 500mA */

    /* ---- Audio descriptors (194 bytes) injected at init ---- */
    /* Placeholder: filled by memcpy from usb_audio_get_desc() in init */
#define AUDIO_PLACEHOLDER_BYTES 186
    [AUDIO_DESC_OFFSET ... AUDIO_DESC_OFFSET + AUDIO_PLACEHOLDER_BYTES - 1] = 0,

    /* ---- Interface 2: DualSense Gamepad ---- */
    0x09, 0x04,
    USB_INTF_GAMEPAD,        /* bInterfaceNumber: 2 */
    0x00,                    /* bAlternateSetting */
    0x01,                    /* bNumEndpoints: IN only (OUT via EP0 SET_REPORT) */
    0x03,                    /* bInterfaceClass: HID */
    0x00,                    /* bInterfaceSubClass */
    0x00,                    /* bInterfaceProtocol */
    0x00,                    /* iInterface */

    /* HID Descriptor (Gamepad) */
    0x09, 0x21,
    0x11, 0x01,              /* bcdHID: 1.11 */
    0x00,                    /* bCountryCode */
    0x01,                    /* bNumDescriptors */
    0x22,                    /* bDescriptorType: Report */
    HID_REPORT_DESC_SIZE_DS & 0xFF, (HID_REPORT_DESC_SIZE_DS >> 8) & 0xFF,

    /* EP IN (Gamepad) */
    0x07, 0x05,
    USB_GAMEPAD_EP_IN,       /* bEndpointAddress */
    0x03,                    /* bmAttributes: Interrupt */
    USB_GAMEPAD_EP_MPS, 0x00,
    USB_GAMEPAD_INTERVAL_MS,

    /* ---- Interface 4: Keyboard + Consumer + Mouse（ds5map 引擎的键鼠输出 / PS 唤醒键）---- */
    0x09, 0x04,
    USB_INTF_KBD,            /* bInterfaceNumber: 4 */
    0x00,                    /* bAlternateSetting */
    0x01,                    /* bNumEndpoints: IN only */
    0x03,                    /* bInterfaceClass: HID */
    0x01,                    /* bInterfaceSubClass: Boot Interface Subclass */
    0x01,                    /* bInterfaceProtocol: Keyboard — forces kbdhid.sys on Windows */
    0x00,                    /* iInterface */

    /* HID Descriptor (Keyboard) */
    0x09, 0x21,
    0x11, 0x01,              /* bcdHID: 1.11 */
    0x00,                    /* bCountryCode */
    0x01,                    /* bNumDescriptors */
    0x22,                    /* bDescriptorType: Report */
    KBD_REPORT_DESC_SIZE & 0xFF, (KBD_REPORT_DESC_SIZE >> 8) & 0xFF,

    /* EP IN (Keyboard) */
    0x07, 0x05,
    USB_KBD_EP_IN,           /* bEndpointAddress */
    0x03,                    /* bmAttributes: Interrupt */
    USB_KBD_EP_MPS, 0x00,
    USB_KBD_INTERVAL_MS,
};

/* ---- Advance-mode descriptor callbacks ---- */

static const uint8_t *desc_device_cb(uint8_t speed)
{
    (void)speed;
    device_desc[16] = config_get()->enable_usb_sn ? 0x03 : 0x00;
    return device_desc;
}

static const uint8_t *desc_config_cb(uint8_t speed)
{
    (void)speed;
    return config_desc;
}

static const uint8_t device_qualifier_desc[] = {
    0x0A,        /* bLength */
    0x06,        /* bDescriptorType: Device Qualifier */
    0x00, 0x02,  /* bcdUSB: 2.00 */
    0x00, 0x00, 0x00,
    0x40,        /* bMaxPacketSize0: 64 */
    0x01, 0x00,
};

static const uint8_t *desc_qualifier_cb(uint8_t speed)
{
    (void)speed;
    return device_qualifier_desc;
}

static uint8_t other_speed_desc[USB_HID_CONFIG_SIZE];

static const uint8_t *desc_other_speed_cb(uint8_t speed)
{
    (void)speed;
    memcpy(other_speed_desc, config_desc, sizeof(other_speed_desc));
    other_speed_desc[1] = 0x07;
    uint16_t total = other_speed_desc[2] | (other_speed_desc[3] << 8);
    for (uint16_t i = 0; i + 2 < total && i + 2 < sizeof(other_speed_desc); ) {
        uint8_t len = other_speed_desc[i];
        if (len < 2) break;
        if (other_speed_desc[i + 1] == 0x05 && len >= 7) {
            uint8_t bmAttr = other_speed_desc[i + 3];
            if ((bmAttr & 0x03) == 0x01)
                other_speed_desc[i + 6] = 1;   /* ISO: FS bInterval=1 (1ms) */
            else if ((bmAttr & 0x03) == 0x03)
                other_speed_desc[i + 6] = other_speed_desc[i + 6]; /* INT: keep as-is */
        }
        i += len;
    }
    return other_speed_desc;
}

static char serial_str[24]; /* "xxxxxxxxxxxx xxxx" max 16 hex + nul */
static bool serial_init = false;

static const char *desc_string_cb(uint8_t speed, uint8_t index)
{
    (void)speed;
    switch (index) {
    case 0: return "\x09\x04";
    case 1: return "Sony Interactive Entertainment";
    case 2: return "DualSense Wireless Controller";
    case 3:
        if (!serial_init) {
            uint8_t chipid[8];
            bflb_efuse_get_chipid(chipid);
            snprintf(serial_str, sizeof(serial_str),
                     "%02X%02X%02X%02X%02X%02X%02X%02X",
                     chipid[0], chipid[1], chipid[2], chipid[3],
                     chipid[4], chipid[5], chipid[6], chipid[7]);
            serial_init = true;
            LOG_INF("[USB] Serial: %s\n", serial_str);
        }
        return serial_str;
    default: return NULL;
    }
}

/* BOS descriptor: declare USB 2.0 Extension with LPM NOT supported.
 * Prevents Linux from attempting L1 power-state transitions. */
static const uint8_t bos_desc_raw[] = {
    /* BOS Header */
    0x05,                   /* bLength */
    0x0F,                   /* bDescriptorType: BOS */
    0x0C, 0x00,             /* wTotalLength: 12 */
    0x01,                   /* bNumDeviceCaps: 1 */

    /* USB 2.0 Extension Device Capability */
    0x07,                   /* bLength */
    0x10,                   /* bDescriptorType: DEVICE_CAPABILITY */
    0x02,                   /* bDevCapabilityType: USB 2.0 Extension */
    0x00, 0x00, 0x00, 0x00, /* bmAttributes: bit1=0 → LPM not supported */
};

static const struct usb_bos_descriptor bos_desc = {
    .string     = bos_desc_raw,
    .string_len = sizeof(bos_desc_raw),
};

static const struct usb_descriptor usb_desc = {
    .device_descriptor_callback          = desc_device_cb,
    .config_descriptor_callback          = desc_config_cb,
    .device_quality_descriptor_callback  = desc_qualifier_cb,
    .other_speed_descriptor_callback     = desc_other_speed_cb,
    .string_descriptor_callback          = desc_string_cb,
    .msosv1_descriptor                   = NULL,
    .msosv2_descriptor                   = NULL,
    .webusb_url_descriptor               = NULL,
    .bos_descriptor                      = NULL,
};

/* Gamepad interface / endpoints */
static struct usbd_interface hid_intf;
static struct usbd_endpoint  hid_ep_in;
/* EP OUT removed: output reports arrive via EP0 SET_REPORT to avoid
 * FIFO F2 sharing conflict with keyboard EP3 IN. */

/* Keyboard interface / endpoint */
static struct usbd_interface kbd_intf;
static struct usbd_endpoint  kbd_ep_in;

static volatile bool usb_configured = false;
static volatile bool ep_in_busy = false;
static volatile bool kbd_ep_busy = false;
static volatile uint64_t kbd_ep_busy_since_us = 0;
static bool kbd_registered = false;
static usb_gamepad_output_cb_t output_callback = NULL;

/* Suspend/resume hooks for wake module */
static void (*hook_suspend)(void)    = NULL;
static void (*hook_resume)(void)     = NULL;
static void (*hook_configured)(void) = NULL;

static USB_NOCACHE_RAM_SECTION uint8_t usb_in_buf[64];
static USB_NOCACHE_RAM_SECTION uint8_t kbd_buf[USB_KBD_EP_MPS];

static volatile uint8_t pending_payload[DS5_USB_INPUT_PAYLOAD_LEN];
static volatile bool    pending_active = false;

ATTR_TCM_SECTION
static void try_send_pending(void)
{
    if (!pending_active || !usb_configured || ep_in_busy)
         return;
    usb_in_buf[0] = DS5_USB_REPORT_ID_INPUT;
    memcpy(usb_in_buf + 1, (const void *)pending_payload,
           DS5_USB_INPUT_PAYLOAD_LEN);
    ep_in_busy = true;
    int ret = usbd_ep_start_write(0, USB_GAMEPAD_EP_IN, usb_in_buf, 64);
    if (ret < 0) {
        ep_in_busy = false;
    } else if (!first_usb_send_logged) {
        first_usb_send_logged = true;
        LOG_INF("[USB] First input: [%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x]\n",
               usb_in_buf[0], usb_in_buf[1], usb_in_buf[2], usb_in_buf[3],
               usb_in_buf[4], usb_in_buf[5], usb_in_buf[6], usb_in_buf[7],
               usb_in_buf[8], usb_in_buf[9]);
    }
}

ATTR_TCM_SECTION
static void hid_ep_in_handler(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)busid; (void)ep; (void)nbytes;
    ep_in_busy = false;
    try_send_pending();
}

/* R2.4：键盘（ID 1）/ 媒体（ID 2）/ 鼠标（ID 3）共用一个中断 IN 端点 0x83（全速 bInterval 10，
 * Windows 实际约 8 ms 读一次，一秒最多过 ~125 份报告）。以前端点一忙就直接丢，引擎却照样记成
 * "已发"、以后不再补：陀螺仪/触摸板控鼠标时几乎每帧都发鼠标报告，紧跟着的键盘「松开」大概率
 * 被丢 → 电脑上的键一直按着、Windows 自动连发；断开时的三份「全部松开」也会互相挤掉。
 * 现在每类报告先进自己的小队列，端点一空（完成中断里，或 usb_task 每轮轮询）就按
 * 键盘 → 媒体 → 鼠标 的顺序补发：按键状态的每一次变化都按顺序送到。
 * 只有鼠标位移（按键没变、没滚轮）的报告**不进队**：端点空就发、忙就丢，和以前一模一样 ——
 * 进队的话完成中断会先把端点忙时攒下的那份旧位移挂上去，电脑拿到的位移平均旧 1~2 ms（陀螺仪瞄准会钝一点）。
 * 位移本来就是每份报告带一帧的量、不累加，所以手感（速度和延迟）都和以前一样。滚轮格数累加。
 * 队列满了就改写最后一条，保证"最终状态"一定对。
 * kq_* 只能在关中断时调用：任务里包 taskENTER_CRITICAL，完成中断里本来就不会被任务打断。 */
#define KQ_KBD_DEPTH    16
#define KQ_MEDIA_DEPTH  8
#define KQ_MOUSE_DEPTH  8

typedef struct { uint8_t btns; int8_t dx, dy; int16_t wheel; } kq_mouse_t;

static uint8_t    kq_kbd[KQ_KBD_DEPTH][8];
static uint16_t   kq_media[KQ_MEDIA_DEPTH];
static kq_mouse_t kq_mouse[KQ_MOUSE_DEPTH];
static uint8_t    kq_kbd_head, kq_kbd_cnt;
static uint8_t    kq_media_head, kq_media_cnt;
static uint8_t    kq_mouse_head, kq_mouse_cnt;
static uint8_t    kq_mouse_btns;        /* 最近一次进队 / 发出的鼠标按键（判断"只有位移"用） */

/* 主机挂起/还没枚举好时只留最终状态：醒来后不重放一串过时的按下/松开 */
static void kq_kbd_push(const uint8_t *rep)
{
    if (!usb_configured)
        kq_kbd_cnt = 0;
    if (kq_kbd_cnt < KQ_KBD_DEPTH)
        kq_kbd_cnt++;
    memcpy(kq_kbd[(kq_kbd_head + kq_kbd_cnt - 1) % KQ_KBD_DEPTH], rep, 8);
}

static void kq_media_push(uint16_t bits)
{
    if (!usb_configured)
        kq_media_cnt = 0;
    if (kq_media_cnt < KQ_MEDIA_DEPTH)
        kq_media_cnt++;
    kq_media[(kq_media_head + kq_media_cnt - 1) % KQ_MEDIA_DEPTH] = bits;
}

/* 按键没变就并进最后一条（位移换成最新的、滚轮累加）；按键变了才新开一条，单击不会被并掉 */
static void kq_mouse_push(uint8_t btns, int8_t dx, int8_t dy, int8_t wheel)
{
    kq_mouse_btns = btns;
    if (!usb_configured)
        kq_mouse_cnt = 0;
    if (kq_mouse_cnt) {
        kq_mouse_t *last = &kq_mouse[(kq_mouse_head + kq_mouse_cnt - 1) % KQ_MOUSE_DEPTH];
        if (last->btns == btns || kq_mouse_cnt == KQ_MOUSE_DEPTH) {
            int w = last->wheel + wheel;
            if (w > 1000) w = 1000; else if (w < -1000) w = -1000;
            last->btns  = btns;
            last->dx    = dx;
            last->dy    = dy;
            last->wheel = (int16_t)w;
            return;
        }
    }
    kq_mouse_t *m = &kq_mouse[(kq_mouse_head + kq_mouse_cnt) % KQ_MOUSE_DEPTH];
    kq_mouse_cnt++;
    m->btns  = btns;
    m->dx    = dx;
    m->dy    = dy;
    m->wheel = wheel;
}

/* 主机挂起 / 总线复位时调用（中断里）：每类只留最后一条（= 最终状态） */
static void kq_collapse(void)
{
    if (kq_kbd_cnt > 1) {
        kq_kbd_head = (uint8_t)((kq_kbd_head + kq_kbd_cnt - 1) % KQ_KBD_DEPTH);
        kq_kbd_cnt = 1;
    }
    if (kq_media_cnt > 1) {
        kq_media_head = (uint8_t)((kq_media_head + kq_media_cnt - 1) % KQ_MEDIA_DEPTH);
        kq_media_cnt = 1;
    }
    if (kq_mouse_cnt > 1) {
        kq_mouse_head = (uint8_t)((kq_mouse_head + kq_mouse_cnt - 1) % KQ_MOUSE_DEPTH);
        kq_mouse_cnt = 1;
    }
}

static void kq_flush(void)
{
    if (!usb_configured || !kbd_registered || kbd_ep_busy)
        return;

    uint8_t  rid;
    uint32_t len;
    int w = 0;
    if (kq_kbd_cnt) {
        rid = 0x01; /* Report ID 1 = Keyboard */
        memcpy(kbd_buf + 1, kq_kbd[kq_kbd_head], 8);
        len = 9;
    } else if (kq_media_cnt) {
        uint16_t bits = kq_media[kq_media_head];
        rid = 0x02; /* Report ID 2 = Consumer Control */
        kbd_buf[1] = (uint8_t)(bits & 0xFF);
        kbd_buf[2] = (uint8_t)(bits >> 8);
        len = 3;
    } else if (kq_mouse_cnt) {
        const kq_mouse_t *m = &kq_mouse[kq_mouse_head];
        w = m->wheel;
        if (w > 127) w = 127; else if (w < -127) w = -127;
        rid = 0x03; /* Report ID 3 = Mouse */
        kbd_buf[1] = m->btns;
        kbd_buf[2] = (uint8_t)m->dx;
        kbd_buf[3] = (uint8_t)m->dy;
        kbd_buf[4] = (uint8_t)(int8_t)w;
        len = 5;
    } else {
        return;
    }
    kbd_buf[0] = rid;

    kbd_ep_busy = true;
    kbd_ep_busy_since_us = bflb_mtimer_get_time_us();
    if (usbd_ep_start_write(0, USB_KBD_EP_IN, kbd_buf, len) < 0) {
        kbd_ep_busy = false;    /* 这条留在队里，下次再试 */
        return;
    }

    if (rid == 0x01) {
        kq_kbd_head = (uint8_t)((kq_kbd_head + 1) % KQ_KBD_DEPTH);
        kq_kbd_cnt--;
    } else if (rid == 0x02) {
        kq_media_head = (uint8_t)((kq_media_head + 1) % KQ_MEDIA_DEPTH);
        kq_media_cnt--;
    } else {
        kq_mouse_t *m = &kq_mouse[kq_mouse_head];
        m->wheel = (int16_t)(m->wheel - w);
        if (m->wheel) {         /* 滚轮超过一份报告的量：剩下的格数下次接着发，位移不重复 */
            m->dx = m->dy = 0;
        } else {
            kq_mouse_head = (uint8_t)((kq_mouse_head + 1) % KQ_MOUSE_DEPTH);
            kq_mouse_cnt--;
        }
    }
}

static void kbd_ep_in_handler(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)busid; (void)ep; (void)nbytes;
    kbd_ep_busy = false;
    kbd_ep_busy_since_us = 0;
    kq_flush();     /* R2.4：队里还有就接着发 */
}

static void usb_poll_endpoints(void);

static void usbd_event_handler(uint8_t busid, uint8_t event)
{
    switch (event) {
    case USBD_EVENT_CONFIGURED:
        usb_configured = true;
        ep_in_busy = false;
        kbd_ep_busy = false;
        LOG_INF("[USB-EVT] CONFIGURED — host enumeration complete!\n");
        if (hook_configured)
            hook_configured();
        break;
    case USBD_EVENT_RESET:
        usb_configured = false;
        ep_in_busy = false;
        kbd_ep_busy = false;
        kq_collapse();          /* R2.4 */
        first_usb_send_logged = false;
        usb_audio_stop();
        usb_audio_mic_stop();
        audio_set_mic_active(false);
        audio_reset_encoder();
        LOG_INF("[USB-EVT] RESET — host detected device, bus reset sent\n");
        break;
    case USBD_EVENT_SUSPEND:
        usb_configured = false;
        ep_in_busy = false;
        kbd_ep_busy = false;
        kq_collapse();          /* R2.4 */
        usb_audio_stop();
        usb_audio_mic_stop();
        audio_set_mic_active(false);
        LOG_INF("[USB-EVT] SUSPEND\n");
        if (hook_suspend)
            hook_suspend();
        break;
    case USBD_EVENT_RESUME:
        usb_configured = true;
        ep_in_busy = false;
        kbd_ep_busy = false;
        LOG_INF("[USB-EVT] RESUME\n");
        if (hook_resume)
            hook_resume();
        break;
    case USBD_EVENT_CONNECTED:
        LOG_INF("[USB-EVT] CONNECTED (VBUS detected)\n");
        break;
    case USBD_EVENT_DISCONNECTED:
        LOG_INF("[USB-EVT] DISCONNECTED (VBUS lost)\n");
        break;
    case USBD_EVENT_SET_REMOTE_WAKEUP:
        LOG_INF("[USB-EVT] SET_REMOTE_WAKEUP\n");
        break;
    case USBD_EVENT_CLR_REMOTE_WAKEUP:
        LOG_INF("[USB-EVT] CLR_REMOTE_WAKEUP\n");
        break;
    default:
        LOG_INF("[USB-EVT] UNKNOWN event=%d\n", event);
        break;
    }
}

ATTR_TCM_SECTION
void usb_gamepad_process_deferred(void)
{
    if (usb_reset_pending) {
        usb_reset_pending = false;
        config_save();
        LOG_INF("[USB] CMD 0x03: USB reconnect via reset\n");
        diag_log(DIAG_EV_RESET, 3, 0);          /* S30：主机命令 0x03 主动复位 */
        bl_sys_rstinfo_set(BL_RST_SOFTWARE);    /* S30：标成"软复位"，别和来历不明的热复位混在一起 */
        vTaskDelay(pdMS_TO_TICKS(100));
        bflb_l1c_dcache_clean_all();   /* R1：DIAG 环在 D-cache 里，复位前刷回 RAM */
        GLB_SW_System_Reset();
    }
    if (usb_config_save_pending) {
        usb_config_save_pending = false;
        config_save();
        LOG_INF("[USB] CMD 0x02: config saved\n");
    }

    uint8_t frid = pending_feature_rid;
    if (frid != 0) {
        pending_feature_rid = 0;
        bt_hid_host_get_feature(frid);
        LOG_INF("[USB] Deferred GET_REPORT(Feature 0x%02x) → BT\n", frid);
    }

    usb_poll_endpoints();   /* R2.4 */

    set_report_entry_t entry;
    while (set_report_queue &&
           xQueueReceive(set_report_queue, &entry, 0) == pdTRUE) {
        LOG_INF("[USB] Deferred SET_REPORT(0x%02x) → BT, %u bytes%s\n",
               entry.report_id, entry.len, entry.is_dse ? " (DSE)" : "");
        bt_hid_host_set_feature_crc(entry.report_id, entry.data, entry.len);
    }

    /* 对照版：暂时去掉 ds5map_deferred() 调用（R8 相对 R7 的唯一差异），
     * 用于确认"上电 2-3 秒蓝灯常亮"是否由它引起。
     * ds5map_deferred() 只影响"延迟落盘/宏 IO"；RPC 的读写本身是同步的，
     * 不依赖它，所以去掉它不影响设备可读可写。 */
    /* ds5map_deferred(); */
}

int usb_gamepad_init(usb_gamepad_output_cb_t output_cb)
{
    output_callback = output_cb;
    memset(usb_in_buf, 0, sizeof(usb_in_buf));
    memset(kbd_buf, 0, sizeof(kbd_buf));

    if (!set_report_queue)
        set_report_queue = xQueueCreate(SET_REPORT_QUEUE_DEPTH,
                                        sizeof(set_report_entry_t));

    LOG_INF("[USB-INIT] === USB Initialization Start ===\n");

    /* Inject audio descriptor bytes into config_desc */
    uint16_t audio_len;
    const uint8_t *audio_bytes = usb_audio_get_desc(&audio_len);
    memcpy(&config_desc[AUDIO_DESC_OFFSET], audio_bytes, audio_len);
    LOG_INF("[USB-INIT] Audio desc injected: %u bytes at offset %d\n",
           audio_len, AUDIO_DESC_OFFSET);

    /* 键鼠接口（键盘 + 消费类 + 鼠标，一个 multi-TLC 接口）**永远注册**（2026-10-11 改）。
     *
     * 原来只在"开机时配置里就已经有键盘/鼠标输出"才注册它（DS5DongleRE 那套 gating），
     * 于是之后在网页上把陀螺仪目标改成「鼠标移动」、或把某个键改成键盘/鼠标时，
     * **接口不会凭空出现**（USB 描述符是开机时定死的），用户看到的就是"改了完全没反应"。
     * 多挂一个标准 HID 接口对主机没有副作用（Windows 直接认），比这个坑划算得多。
     *
     * `has_kbd_targets` 现在只用来决定 remote-wake 属性（跟以前保持一致）。 */
    /* S33：原作者的 remap 档案 / PS 快捷键 / 触摸板鼠标模式已删，只看 ds5map 引擎有没有键鼠输出 */
    const bool has_kbd_targets = ds5map_has_kbd_outputs() ||
                                 ds5map_has_mouse_outputs();
    const bool need_remote_wake = config_get()->enable_wake || has_kbd_targets;
    config_desc[KBD_SUBCLASS_OFF] = 0x00; /* No subclass (multi-TLC, not pure boot) */
    config_desc[KBD_PROTOCOL_OFF] = 0x00; /* None — keyboard + consumer control + mouse */
    config_desc[7] = need_remote_wake ? 0xE0 : 0xC0;
    /* wTotalLength / bNumInterfaces 保持编译进去的那份（含键鼠接口） */
    LOG_INF("[USB-INIT] Keyboard+CC+mouse intf: INCLUDED (always)\n");

    usbd_desc_register(0, &usb_desc);
    LOG_INF("[USB-INIT] Descriptors registered (dev=%uB, cfg=%uB)\n",
           (unsigned)sizeof(device_desc), (unsigned)sizeof(config_desc));

    /* Interface 0,1,2: Audio Control + Streaming OUT + Streaming IN */
    usb_audio_register(0);
    LOG_INF("[USB-INIT] Audio interfaces registered\n");

    /* Interface 3: Gamepad — 只有一份 DS5 报告描述符（DSE 描述符已删，见文件上方） */
    usbd_add_interface(0, usbd_hid_init_intf(0, &hid_intf,
                                             hid_report_desc_ds, sizeof(hid_report_desc_ds)));
    LOG_INF("[USB-INIT] Gamepad HID intf added (report_desc=%uB)\n",
           (unsigned)sizeof(hid_report_desc_ds));

    /* Interface 4: Keyboard + Consumer + Mouse（永远在，见上面注释） */
    kbd_registered = true;
    usbd_add_interface(0, usbd_hid_init_intf(
        0, &kbd_intf, kbd_report_desc, sizeof(kbd_report_desc)));
    LOG_INF("[USB-INIT] Keyboard+CC+mouse HID intf added\n");

    hid_ep_in.ep_addr = USB_GAMEPAD_EP_IN;
    hid_ep_in.ep_cb = hid_ep_in_handler;
    usbd_add_endpoint(0, &hid_ep_in);

    kbd_ep_in.ep_addr = USB_KBD_EP_IN;
    kbd_ep_in.ep_cb = kbd_ep_in_handler;
    usbd_add_endpoint(0, &kbd_ep_in);
    LOG_INF("[USB-INIT] Endpoints: GP_IN=0x%02x KBD_IN=0x%02x (OUT via EP0)\n",
           USB_GAMEPAD_EP_IN, USB_KBD_EP_IN);

    /* Read registers BEFORE usbd_initialize */
    {
        volatile uint32_t *dev_ctl = (volatile uint32_t *)(USB_BASE + 0x100);
        volatile uint32_t *phy_tst = (volatile uint32_t *)(USB_BASE + 0x114);
        LOG_INF("[USB-INIT] PRE-init: DEV_CTL=0x%08lx PHY_TST=0x%08lx\n",
               (unsigned long)*dev_ctl, (unsigned long)*phy_tst);
    }

    LOG_INF("[USB-INIT] Calling usbd_initialize(0, 0x%08lx, handler)...\n",
           (unsigned long)USB_BASE);
    usbd_initialize(0, USB_BASE, usbd_event_handler);
    LOG_INF("[USB-INIT] usbd_initialize returned\n");

    /* Read registers AFTER usbd_initialize (before fixup) */
    {
        volatile uint32_t *dev_ctl = (volatile uint32_t *)(USB_BASE + 0x100);
        volatile uint32_t *phy_tst = (volatile uint32_t *)(USB_BASE + 0x114);
        LOG_INF("[USB-INIT] POST-init (raw): DEV_CTL=0x%08lx PHY_TST=0x%08lx\n",
               (unsigned long)*dev_ctl, (unsigned long)*phy_tst);
        LOG_INF("[USB-INIT]   GLINT_EN=%d CHIP_EN=%d FORCE_FS=%d UNPLUG=%d\n",
               (int)((*dev_ctl >> 2) & 1), (int)((*dev_ctl >> 5) & 1),
               (int)((*dev_ctl >> 9) & 1), (int)(*phy_tst & 1));
    }

    {
        volatile uint32_t *dev_ctl = (volatile uint32_t *)(USB_BASE + 0x100);
        uint32_t v = *dev_ctl;
        v |= (1 << 2);  /* USB_GLINT_EN_HOV */
        v &= ~(1 << 25); /* Clear USB_LPM_EN — refuse L1 power state */
        v &= ~(1 << 26); /* Clear USB_LPM_ACCEPT — NYET all LPM requests */
        *dev_ctl = v;
    }

    /* Final register state after fixup */
    {
        volatile uint32_t *pds_usb_ctl  = (volatile uint32_t *)(0x2000e000 + 0x500);
        volatile uint32_t *usb_dev_ctl  = (volatile uint32_t *)(USB_BASE + 0x100);
        volatile uint32_t *usb_phy_tst  = (volatile uint32_t *)(USB_BASE + 0x114);
        volatile uint32_t *usb_glb_int  = (volatile uint32_t *)(USB_BASE + 0x0C4);
        volatile uint32_t *usb_otg_csr  = (volatile uint32_t *)(USB_BASE + 0x080);

        LOG_INF("[USB-INIT] POST-fixup final state:\n");
        LOG_INF("[USB-INIT]   PDS_USB_CTL=0x%08lx\n", (unsigned long)*pds_usb_ctl);
        LOG_INF("[USB-INIT]   DEV_CTL=0x%08lx (GLINT_EN=%d CHIP_EN=%d FORCE_FS=%d)\n",
               (unsigned long)*usb_dev_ctl,
               (int)((*usb_dev_ctl >> 2) & 1), (int)((*usb_dev_ctl >> 5) & 1),
               (int)((*usb_dev_ctl >> 9) & 1));
        LOG_INF("[USB-INIT]   PHY_TST=0x%08lx (UNPLUG=%d) — expect 0\n",
               (unsigned long)*usb_phy_tst, (int)(*usb_phy_tst & 1));
        LOG_INF("[USB-INIT]   GLB_INT=0x%08lx OTG_CSR=0x%08lx\n",
               (unsigned long)*usb_glb_int, (unsigned long)*usb_otg_csr);
    }

    LOG_INF("[USB-INIT] === USB Ready. Waiting for host to send RESET ===\n");
#if USB_NATIVE_TYPEC
    LOG_INF("[USB-INIT] USB Type-C native — no wiring needed\n");
#else
    LOG_INF("[USB-INIT] If no [USB-EVT] RESET appears, check wiring:\n");
    LOG_INF("[USB-INIT]   Kit Pin 37 (USB_DM) -> cable D- (white)\n");
    LOG_INF("[USB-INIT]   Kit Pin 38 (USB_DP) -> cable D+ (green)\n");
    LOG_INF("[USB-INIT]   Kit Pin 21/22 (GND) -> cable GND (black)\n");
#endif
    return 0;
}

void usb_gamepad_set_suspend_hooks(void (*on_suspend)(void),
                                   void (*on_resume)(void),
                                   void (*on_configured)(void))
{
    hook_suspend    = on_suspend;
    hook_resume     = on_resume;
    hook_configured = on_configured;
}

void usb_gamepad_set_polling_rate(uint8_t mode)
{
    uint8_t interval;
#ifdef FORCE_FS_MODE
    /* FS: bInterval = N ms (linear), USB polls 2x target for headroom */
    switch (mode) {
    case 0:  interval = 2; break;  /* FS 2ms = 500Hz USB → ~250Hz effective */
    case 1:  interval = 1; break;  /* FS 1ms = 1000Hz USB → ~500Hz effective */
    default: interval = 1; break;  /* FS 1ms = 1000Hz USB (BT-limited ~750Hz) */
    }
#else
    /* HS: bInterval = 2^(N-1) microframes */
    switch (mode) {
    case 0:  interval = 5; break;  /* HS 2^4=16µf = 2ms → 500Hz, effective ~250Hz */
    case 1:  interval = 4; break;  /* HS 2^3=8µf  = 1ms → 1000Hz, effective ~500Hz */
    default: interval = 3; break;  /* HS 2^2=4µf  = 0.5ms → 2000Hz, effective ~750Hz */
    }
#endif
    config_desc[BINTERVAL_GAMEPAD_EP_IN_OFF]  = interval;
    config_desc[BINTERVAL_GAMEPAD_EP_OUT_OFF] = interval;
    LOG_INF("[USB] Polling rate: mode=%d bInterval=%d\n", mode, interval);
}

ATTR_TCM_SECTION
int usb_gamepad_send_raw_input(const uint8_t *payload)
{
    memcpy((void *)pending_payload, payload, DS5_USB_INPUT_PAYLOAD_LEN);
    pending_active = true;
    try_send_pending();
    return 0;
}

bool usb_gamepad_is_ready(void)
{
    return usb_configured && !ep_in_busy;
}


/* R2.4：下面三个都是"进队 + 端点空就发"，返回 0 = 已进队（一定会按顺序送到）。
 * 主机挂起时也进队（只留最终状态），醒来后由 usb_gamepad_process_deferred() 补发。 */
int usb_gamepad_send_kbd_report(const uint8_t *report, uint8_t len)
{
    if (!kbd_registered)
        return -1;
    uint8_t rep[8] = { 0 };
    memcpy(rep, report, (len > 8) ? 8 : len);
    taskENTER_CRITICAL();
    kq_kbd_push(rep);
    kq_flush();
    taskEXIT_CRITICAL();
    return 0;
}

/* R1 / R2.2：媒体与多功能键（Consumer Control，报告 ID 2，2 字节位图：bit0 音量+ 1 音量−
 * 2 静音 3 播放/暂停 4 下一曲 5 上一曲 6 停止 7 快进 8 快退 9 媒体播放器 10 计算器 11 搜索
 * 12 主页 13 后退 14 前进 15 刷新）。与键盘/鼠标共用同一个中断 IN 端点（MPS 9，够放）。 */
int usb_gamepad_send_consumer_report(uint16_t bits)
{
    if (!kbd_registered)
        return -1;
    taskENTER_CRITICAL();
    kq_media_push(bits);
    kq_flush();
    taskEXIT_CRITICAL();
    return 0;
}

int usb_gamepad_send_mouse_report(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel)
{
    if (!kbd_registered)
        return -1;
    int ret = 0;
    taskENTER_CRITICAL();
    if (wheel == 0 && buttons == kq_mouse_btns &&
        (kbd_ep_busy || !usb_configured || kq_kbd_cnt || kq_media_cnt || kq_mouse_cnt)) {
        ret = -1;               /* 只有位移、端点又忙：丢掉（同以前），见文件上方 kq_* 的说明 */
    } else {
        kq_mouse_push(buttons, dx, dy, wheel);
        kq_flush();
    }
    taskEXIT_CRITICAL();
    return ret;
}

/* R2.4：断开 / 重载配置时用：丢掉还没发的按下，键盘、媒体、鼠标各排一份「全部松开」。
 * 不看之前发过什么，无条件发——电脑上本来就是松开的话，再收一份全 0 也没有任何效果。 */
void usb_gamepad_kbd_release_all(void)
{
    static const uint8_t zero[8] = { 0 };
    if (!kbd_registered)
        return;
    taskENTER_CRITICAL();
    kq_kbd_cnt = kq_media_cnt = kq_mouse_cnt = 0;
    kq_kbd_push(zero);
    kq_media_push(0);
    kq_mouse_push(0, 0, 0, 0);
    kq_flush();
    taskEXIT_CRITICAL();
}

/* R2.4：usb_task 每轮经 usb_gamepad_process_deferred() 调一次。
 * ① 键盘端点忙了 >20 ms 还没完成（主机没在读）就强清 busy，再补发队列；
 * ② 手柄端点：挂起/唤醒、总线复位只清了 ep_in_busy，没人把最后一帧重新挂上——断开时
 *    排好的回中帧会一直发不出去，等到下一帧手柄数据才走。这里补挂一次。 */
static void usb_poll_endpoints(void)
{
    taskENTER_CRITICAL();
    if (kbd_ep_busy && kbd_ep_busy_since_us &&
        (bflb_mtimer_get_time_us() - kbd_ep_busy_since_us) > 20000ULL) {
        kbd_ep_busy = false;
        kbd_ep_busy_since_us = 0;
    }
    kq_flush();
    taskEXIT_CRITICAL();

    if (pending_active && usb_configured && !ep_in_busy)
        try_send_pending();
}

/* ---------- HID class request callbacks (Feature Report forwarding) ---------- */

/* +1 for Report ID prefix required by CherryUSB */
static uint8_t feature_resp_buf[FEATURE_DATA_MAX + 1];
static uint8_t kbd_idle_report[USB_KBD_EP_MPS];

static bool is_donglere_cmd(uint8_t report_id)
{
#if DS5MAP_RPT_0FC_ENABLE
    return (report_id >= 0xF6 && report_id <= 0xF9) || report_id == MAP_RPT_RPC;
#else
    return (report_id >= 0xF6 && report_id <= 0xF9);
#endif
}

/*
 * Static fallback for DualSense feature reports 0x09/0x20/0x05.
 * Used when BT cache is empty (controller not yet connected) so that
 * the Linux hid-playstation driver can probe successfully.
 * Returns true if a fallback was provided.
 */
static bool ds5_feature_fallback(uint8_t report_id, uint8_t *buf, uint32_t *len)
{
    switch (report_id) {

    case DS5_FEATURE_REPORT_PAIRING: {
        /* Report 0x09 — Pairing info (20 bytes)
         * bytes[0]   = report ID
         * bytes[1:6] = controller BT MAC (placeholder)
         * bytes[7:19]= reserved */
        memset(buf, 0, 20);
        buf[0] = 0x09;
        buf[1] = 0x00; buf[2] = 0x05; buf[3] = 0xD5;
        buf[4] = 0xBE; buf[5] = 0x18; buf[6] = 0x61;
        *len = 20;
        return true;
    }

    case DS5_FEATURE_REPORT_FIRMWARE: {
        /* Report 0x20 — Firmware info (64 bytes)
         * bytes[0]     = report ID
         * bytes[24:27] = hw_version  (uint32 LE)
         * bytes[28:31] = fw_version  (uint32 LE)
         * bytes[44:45] = update_version (uint16 LE, >= 0x0215 for vibration_v2) */
        memset(buf, 0, 64);
        buf[0] = 0x20;
        buf[24] = 0x00; buf[25] = 0x02; buf[26] = 0x00; buf[27] = 0x00;
        buf[28] = 0x56; buf[29] = 0x02; buf[30] = 0x00; buf[31] = 0x00;
        buf[44] = 0x21; buf[45] = 0x02;
        *len = 64;
        return true;
    }

    case DS5_FEATURE_REPORT_CALIBRATION: {
        /* Report 0x05 — Calibration data (41 bytes)
         * Typical DualSense factory values; the kernel driver has
         * division-by-zero protection and will use its own defaults
         * if any denominator is zero. */
        memset(buf, 0, 41);
        buf[0] = 0x05;
        int16_t *cal = (int16_t *)(buf + 1);
        cal[0]  = 0;       /* gyro_pitch_bias */
        cal[1]  = 0;       /* gyro_yaw_bias   */
        cal[2]  = 0;       /* gyro_roll_bias  */
        cal[3]  = 8192;    /* gyro_pitch_plus  */
        cal[4]  = -8192;   /* gyro_pitch_minus */
        cal[5]  = 8192;    /* gyro_yaw_plus    */
        cal[6]  = -8192;   /* gyro_yaw_minus   */
        cal[7]  = 8192;    /* gyro_roll_plus   */
        cal[8]  = -8192;   /* gyro_roll_minus  */
        cal[9]  = 1024;    /* gyro_speed_plus  */
        cal[10] = -1024;   /* gyro_speed_minus */
        cal[11] = 8192;    /* acc_x_plus   */
        cal[12] = -8192;   /* acc_x_minus  */
        cal[13] = 8192;    /* acc_y_plus   */
        cal[14] = -8192;   /* acc_y_minus  */
        cal[15] = 8192;    /* acc_z_plus   */
        cal[16] = -8192;   /* acc_z_minus  */
        *len = 41;
        return true;
    }

    default:
        if (report_id >= 0x70 && report_id <= 0x7B) {
            /* DSE profile reports — return factory defaults (zeroed)
             * while BT-side prefetch is still in progress. */
            memset(buf, 0, 64);
            buf[0] = report_id;
            *len = 64;
            return true;
        }
        return false;
    }
}

ATTR_TCM_SECTION
void usbd_hid_get_report(uint8_t busid, uint8_t intf, uint8_t report_id,
                         uint8_t report_type, uint8_t **data, uint32_t *len)
{
    (void)busid;

    if (intf == USB_INTF_KBD) {
        memset(kbd_idle_report, 0, sizeof(kbd_idle_report));
        *data = kbd_idle_report;
        *len  = (report_id == 0x02) ? 1 : 8; /* CC=1B bitmap, Kbd=8B (no Report ID prefix) */
        return;
    }

    if (report_type != 0x03) {
        *len = 0;
        return;
    }

    /* DongleRE config reports 0xF6-0xF9 */
    if (is_donglere_cmd(report_id)) {
        feature_resp_buf[0] = report_id;

        if (report_id == 0xF7) {
            struct config_body *cfg = config_get();
            uint16_t cfg_len = sizeof(struct config_body);
            if (cfg_len > FEATURE_DATA_MAX)
                cfg_len = FEATURE_DATA_MAX;
            memcpy(feature_resp_buf + 1, cfg, cfg_len);
            *data = feature_resp_buf;
            *len  = 1 + cfg_len;
            LOG_INF("[USB] GET_REPORT(0xF7) → config %u bytes\n", cfg_len);
        } else if (report_id == 0xF8) {
            uint16_t ver_len = strlen(FIRMWARE_VERSION);
            if (ver_len > FEATURE_DATA_MAX)
                ver_len = FEATURE_DATA_MAX;
            memcpy(feature_resp_buf + 1, FIRMWARE_VERSION, ver_len);
            *data = feature_resp_buf;
            *len  = 1 + ver_len;
            LOG_INF("[USB] GET_REPORT(0xF8) → firmware version\n");
        } else if (report_id == 0xF9) {
            extern int8_t bt_hid_host_get_cached_rssi(void);
            extern uint8_t get_battery_level(void);
            extern uint8_t get_battery_state(void);

            feature_resp_buf[1] = (uint8_t)bt_hid_host_get_cached_rssi();
            uint8_t aflags = 0x80;
            if (state_mgr_is_spk_active())        aflags |= 0x02;
            if (usb_audio_mic_is_active())         aflags |= 0x01;
            feature_resp_buf[2] = aflags;
            /* Layout kept identical to the private firmware so the companion
             * web tool can parse RSSI / audio / battery. OTA fields are
             * reserved and always zero — this build has no OTA. */
            feature_resp_buf[3]  = 0;   /* OTA status (idle) */
            feature_resp_buf[4]  = 0;   /* OTA bytes received */
            feature_resp_buf[5]  = 0;
            feature_resp_buf[6]  = 0;
            feature_resp_buf[7]  = 0;
            feature_resp_buf[8]  = 0;   /* OTA total size */
            feature_resp_buf[9]  = 0;
            feature_resp_buf[10] = 0;
            feature_resp_buf[11] = 0;
            feature_resp_buf[12] = get_battery_level();
            feature_resp_buf[13] = get_battery_state();
            *data = feature_resp_buf;
            *len  = 14;
#if DS5MAP_RPC_HANDLER_ENABLE
        } else if (report_id == MAP_RPT_RPC) {
            /* R5: 只加读取分支。
             * 关键：0xFC 在描述符里声明 127 字节 payload（+1 report id = 128 上限），
             * 这里必须按描述符截断 —— 返回超长会让 Windows 拒绝启动该 HID 接口。 */
            feature_resp_buf[0] = MAP_RPT_RPC;
            uint16_t n = ds5map_rpc_response(feature_resp_buf + 1,
                                             MAP_RPC_REPORT_LEN - 1);
            if (n > MAP_RPC_REPORT_LEN - 1)
                n = MAP_RPC_REPORT_LEN - 1;
            *data = feature_resp_buf;
            *len  = 1 + n;
            LOG_INF("[USB] GET_REPORT(0xFC) → rpc %d bytes\n", n);
#endif
        } else {
            *len = 0;
        }
        return;
    }

    /* DSE profile reports (0x70-0x7B): NAK while prefetch in progress
     * so the PS Accessories app retries instead of caching empty data. */
    if (dse_is_profile_report(report_id) && !dse_profiles_ready()) {
        *len = 0;
        return;
    }

    /* DualSense Feature Reports — forwarded to/from BT */
    const uint8_t *cached;
    uint16_t cached_len;
    if (bt_hid_host_get_cached_feature(report_id, &cached, &cached_len)) {
        if (cached_len > FEATURE_DATA_MAX)
            cached_len = FEATURE_DATA_MAX;
        feature_resp_buf[0] = report_id;
        memcpy(feature_resp_buf + 1, cached, cached_len);
        *data = feature_resp_buf;
        *len  = 1 + cached_len;
        LOG_INF("[USB] GET_REPORT(Feature 0x%02x) → %u bytes from cache\n",
               report_id, cached_len);
    } else if (ds5_feature_fallback(report_id, feature_resp_buf, len)) {
        *data = feature_resp_buf;
        LOG_INF("[USB] GET_REPORT(Feature 0x%02x) → %u bytes from fallback\n",
               report_id, *len);
    } else {
        pending_feature_rid = report_id;
        *len = 0;
        LOG_INF("[USB] GET_REPORT(Feature 0x%02x) → not cached, deferred to task\n",
               report_id);
    }
}

ATTR_TCM_SECTION
void usbd_hid_set_report(uint8_t busid, uint8_t intf, uint8_t report_id,
                         uint8_t report_type, uint8_t *report,
                         uint32_t report_len)
{
    (void)busid;

    if (intf == USB_INTF_KBD) {
        return;
    }

    const uint8_t *payload = report;
    uint32_t payload_len = report_len;
    if (report_len > 0 && report[0] == report_id) {
        payload++;
        payload_len--;
    }

    /* DongleRE config command (0xF6 SET) */
    if (report_type == 0x03 && report_id == 0xF6 && payload_len > 0) {
        uint8_t cmd = payload[0];
        if (cmd == 0x01 && payload_len > 1) {
            config_set(payload + 1, payload_len - 1);
            usb_gamepad_set_polling_rate(config_get()->polling_rate_mode);
            /* R3.28：音频挡位（audio_kbps/bw/cx）跟着配置一起改，立刻生效 */
            extern void audio_apply_config(void);
            audio_apply_config();
            LOG_INF("[USB] CMD 0x01: config updated\n");
        } else if (cmd == 0x02) {
            usb_config_save_pending = true;
        } else if (cmd == 0x03) {
            usb_reset_pending = true;
        } else if (cmd == 0x10 && payload_len >= 6) {
            /* S36：音频编码参数运行时调（不落盘）：[1..2] kbps LE，[3] 带宽码 0 自动/1 NB/2 MB/3 WB/4 SWB/5 FB，
             * [4] 模式 0 补填充到 200 B / 1 按真实长度发，[5] 复杂度 0..10。audio_task 在两次编码之间应用。 */
            extern void audio_set_tuning(uint16_t kbps, uint8_t bw, uint8_t mode, uint8_t complexity);
            audio_set_tuning((uint16_t)(payload[1] | (payload[2] << 8)), payload[3], payload[4], payload[5]);
            LOG_INF("[USB] CMD 0x10: audio tune\n");
        } else if (cmd == 0x11 && payload_len >= 2) {
            /* R1.5：「接收器当主」开关（不落盘）：[1] = 1 开 / 0 关。开着时 bt 任务下一次链路诊断就申请。
             * R1.6 起默认开，这条命令主要用来关（拔插恢复开） */
            bt_hid_host_set_role_req(payload[1]);
            LOG_INF("[USB] CMD 0x11: role request %u\n", (unsigned)payload[1]);
        }
        return;
    }

#if DS5MAP_SET_REPORT_ENABLE
    /* R6: RPC 请求入口 —— 请求格式 [op region off_lo off_hi len data...] */
    if (report_type == 0x03 && report_id == MAP_RPT_RPC && payload_len >= 5) {
        ds5map_rpc_request(payload, (uint16_t)payload_len);
        return;
    }
#endif

    /* DSE profile writes (0x60-0x62) and unlock (0x80) — forward + notify */
    if (report_type == 0x03 && payload_len > 0 &&
        (report_id == 0x60 || report_id == 0x61 ||
         report_id == 0x62 || report_id == 0x80)) {
        set_report_entry_t entry;
        entry.report_id = report_id;
        entry.is_dse    = true;
        entry.len = (payload_len <= SET_REPORT_MAX_DATA)
                        ? payload_len : SET_REPORT_MAX_DATA;
        memcpy(entry.data, payload, entry.len);
        BaseType_t ok = xQueueSendFromISR(set_report_queue, &entry, NULL);
        LOG_ISR("[USB-ISR] SET_REPORT(DSE 0x%02x) queued=%d len=%lu\n",
               report_id, (int)(ok == pdTRUE), (unsigned long)payload_len);
        dse_on_profile_write(report_id);
        return;
    }

    /* DS5DongleRE only forwards DSE profile reports (0x60-0x62, 0x80) to BT.
     * All other Feature SET_REPORTs (0x08, 0x09 etc.) are dropped — forwarding
     * 0x08 (BT control) would cause the controller to power-off or re-pair. */
    if (report_type == 0x02 && payload_len > 0) {
        if (output_callback)
            output_callback(report, report_len);
    }
}

/* ---- USB soft plug/unplug via PHY UNPLUG bit ---- */

void usb_soft_disconnect(void)
{
    volatile uint32_t *phy_tst = (volatile uint32_t *)(USB_BASE + 0x114);
    *phy_tst |= 1u;
    LOG_INF("[USB] Soft disconnect (stealth)\n");
}

void usb_soft_connect(void)
{
    volatile uint32_t *phy_tst = (volatile uint32_t *)(USB_BASE + 0x114);
    *phy_tst &= ~1u;
    LOG_INF("[USB] Soft connect (stealth)\n");
}
