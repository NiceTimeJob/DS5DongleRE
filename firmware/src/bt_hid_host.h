#ifndef BT_HID_HOST_H
#define BT_HID_HOST_H

#include <stdint.h>
#include <stdbool.h>

#define HID_PSM_CONTROL    0x0011
#define HID_PSM_INTERRUPT  0x0013

#define HID_TRANS_HANDSHAKE    0x0
#define HID_TRANS_HID_CONTROL  0x1
#define HID_TRANS_GET_REPORT   0x4
#define HID_TRANS_SET_REPORT   0x5
#define HID_TRANS_GET_PROTOCOL 0x6
#define HID_TRANS_SET_PROTOCOL 0x7
#define HID_TRANS_DATA         0xA
#define HID_TRANS_DATC         0xB

#define HID_REPORT_INPUT   0x01
#define HID_REPORT_OUTPUT  0x02
#define HID_REPORT_FEATURE 0x03

#define HID_HANDSHAKE_OK            0x00
#define HID_HANDSHAKE_NOT_READY     0x01
#define HID_HANDSHAKE_INVALID_ID    0x02
#define HID_HANDSHAKE_UNSUPPORTED   0x03
#define HID_HANDSHAKE_INVALID_PARAM 0x04
#define HID_HANDSHAKE_ERR_UNKNOWN   0x0E
#define HID_HANDSHAKE_ERR_FATAL     0x0F

#define HID_PROTOCOL_BOOT   0x00
#define HID_PROTOCOL_REPORT 0x01

#define HID_HEADER(trans, param) (((trans) << 4) | ((param) & 0x0F))

enum bt_hid_host_state {
    BT_HID_STATE_IDLE,
    BT_HID_STATE_SCANNING,
    BT_HID_STATE_CONNECTING,
    BT_HID_STATE_SDP_QUERY,
    BT_HID_STATE_L2CAP_CONTROL,
    BT_HID_STATE_L2CAP_INTERRUPT,
    BT_HID_STATE_HANDSHAKE,
    BT_HID_STATE_CONNECTED,
    BT_HID_STATE_DISCONNECTING,
};

typedef void (*bt_hid_input_cb_t)(const uint8_t *data, uint16_t len);
typedef void (*bt_hid_state_cb_t)(enum bt_hid_host_state state);
/* Called immediately when HID interrupt L2CAP channel opens —
 * equivalent to DS5DongleRE's L2CAP_EVENT_CHANNEL_OPENED handler.
 * Use this to send the initial 0x32 primer as early as possible. */
typedef void (*bt_hid_primer_cb_t)(void);

struct bt_hid_host_config {
    bt_hid_input_cb_t  input_cb;
    bt_hid_state_cb_t  state_cb;
    bt_hid_primer_cb_t primer_cb;
    uint16_t           target_vid;
    uint16_t           target_pid;
};

/**
 * Initialize BT HID Host subsystem.
 * @param config  callbacks and target device filters
 * @return 0 on success
 */
int bt_hid_host_init(const struct bt_hid_host_config *config);

/**
 * Start scanning for BT Classic HID devices.
 * @param duration_sec  scan duration in seconds (0 = indefinite)
 * @return 0 on success
 */
int bt_hid_host_scan_start(uint8_t duration_sec);

/**
 * Stop scanning.
 */
void bt_hid_host_scan_stop(void);

/**
 * Connect to a discovered device by address.
 * @param addr  6-byte Bluetooth address
 * @return 0 on success
 */
int bt_hid_host_connect(const uint8_t *addr);

/**
 * Disconnect the current device (graceful — waits for HCI ACK).
 */
void bt_hid_host_disconnect(void);

/* S32：断开请求的来源编号，写进 DIAG_EV_DISC_REQ 的 arg1。
 *
 * 起因（2026-10-12 16:41）：接收器在游戏中途主动断开（ACL_DOWN reason=0x16
 * LOCALHOST_TERM_CONN、无看门狗、无复位），但全工程八个拆链入口**一个都没留痕**，
 * 只能靠排除法猜。现在每个入口都带编号，下次一读 DIAG 就知道是谁干的。 */
enum bt_disc_site {
    BT_DISC_SITE_MAIN_BTN_1CLICK    = 1,  /* main.c 板载键单击：只有一个绑定 → 断开重扫 */
    BT_DISC_SITE_MAIN_BTN_2CLICK    = 2,  /* main.c 板载键双击：断开 + 重新扫描 */
    BT_DISC_SITE_MAIN_CONN_TIMEOUT  = 3,  /* main.c CONNECTING/SDP 超过 8 s */
    BT_DISC_SITE_MAIN_L2CAP_TIMEOUT = 4,  /* main.c L2CAP 阶段超过 3 s */
    BT_DISC_SITE_MAIN_HS_TIMEOUT    = 5,  /* main.c 握手超过 10 s */
    BT_DISC_SITE_AUDIO_SENDFAIL     = 6,  /* audio.c 音频连续 50 次发送失败（R1 起不再拆链，编号保留） */
    BT_DISC_SITE_SWITCH_NEXT        = 7,  /* bt_hid_host.c 板载键单击切换手柄 */
    BT_DISC_SITE_CLEAR_BONDS        = 8,  /* bt_hid_host.c 长按清绑定 */
};

/* 与 bt_hid_host_disconnect() 等价，但先记一条 DIAG_EV_DISC_REQ（ISR 安全）。 */
void bt_hid_host_disconnect_site(uint8_t site);

/**
 * Force disconnect — immediately cleans up state without waiting for
 * HCI ACK. Use when the remote device is unresponsive (powered off).
 */
void bt_hid_host_force_disconnect(void);

/**
 * Send an output report (e.g. LED, rumble) to the connected device.
 * Sent via HID Interrupt channel as DATA|OUTPUT.
 * @param data  report data including Report ID
 * @param len   data length
 * @return 0 on success
 */
int bt_hid_host_send_output(const uint8_t *data, uint16_t len);

/**
 * Send a SET_REPORT (Feature) on the control channel.
 * @param report_id  HID report ID
 * @param data       report payload (after report ID)
 * @param len        payload length
 * @return 0 on success
 */
int bt_hid_host_set_feature(uint8_t report_id, const uint8_t *data, uint16_t len);

/**
 * Send a GET_REPORT (Feature) on the control channel.
 * Response arrives via the control channel callback.
 * @param report_id  HID report ID to request
 * @return 0 on success
 */
int bt_hid_host_get_feature(uint8_t report_id);

/**
 * Get current connection state.
 */
enum bt_hid_host_state bt_hid_host_get_state(void);

/**
 * Get the address of the connected device.
 * @param addr  output 6-byte buffer
 * @return true if a device is connected
 */
bool bt_hid_host_get_connected_addr(uint8_t *addr);

/**
 * Try to reconnect to a previously bonded BR/EDR device.
 * Skips inquiry and directly initiates an ACL connection.
 * @return 0 if a bonded device was found and connection initiated,
 *         -1 if no bonded device exists
 */
int bt_hid_host_try_reconnect(void);

/**
 * Schedule an outgoing page (create_connection) to the active bonded
 * controller. Fired by bt_hid_host_poll_connect() on the next main-loop pass.
 *
 * Recovery path for a controller that never noticed our reboot (e.g. a USB
 * power blip shorter than its link supervision timeout): it still believes
 * the old ACL link is alive, so it never pages us and ignores inquiry —
 * passive page scan alone deadlocks until the user presses PS. An outgoing
 * page forces its link manager to drop the zombie link.
 *
 * @return 0 if scheduled, -1 if busy or no bonded target
 */
int bt_hid_host_page_bonded(void);

/**
 * Switch to the next bonded controller. Disconnects current, advances
 * active_idx, persists it, and waits for the new target to reconnect.
 * @return new active_idx on success, -1 if <= 1 bonded controller
 */
int bt_hid_host_switch_next(void);

/**
 * Check if a controller switch is in progress (waiting for old ACL teardown).
 */
bool bt_hid_host_is_switching(void);

/**
 * Get the number of bonded controllers in memory.
 */
uint8_t bt_hid_host_get_bonded_count(void);

/**
 * Get the active controller index (0-based).
 */
uint8_t bt_hid_host_get_active_idx(void);

/**
 * Clear all BR/EDR bonding information and disconnect if connected.
 * After clearing, the device will need to be re-paired via inquiry scan.
 */
void bt_hid_host_clear_bonds(void);

/**
 * Check if the connected controller is a DualSense Edge.
 * Detected by receiving feature report 0x70 from the controller.
 * @return true if Edge controller is connected
 */
bool bt_hid_host_is_dse(void);

/**
 * Get a cached feature report received from the connected controller.
 * @param report_id  HID report ID
 * @param data       output: pointer to cached data (excludes report_id)
 * @param len        output: cached data length
 * @return true if found in cache
 */
bool bt_hid_host_get_cached_feature(uint8_t report_id,
                                    const uint8_t **data, uint16_t *len);

/**
 * Send a SET_REPORT (Feature) with BT CRC to the connected controller.
 * @param report_id  HID report ID
 * @param data       report payload (after report ID, without CRC)
 * @param len        payload length (without CRC)
 * @return 0 on success
 */
int bt_hid_host_set_feature_crc(uint8_t report_id, const uint8_t *data,
                                uint16_t len);

/**
 * Persist blacklist to flash if dirty. Call from task context (not ISR/BT
 * callback) to avoid blocking the BT stack during hot paths.
 */
void bt_hid_host_persist_if_dirty(void);

/**
 * Retry a stalled handshake GET. Call periodically from bt_task when state
 * is HANDSHAKE to recover from transient TX pool exhaustion.
 */
void bt_hid_host_handshake_tick(void);

/**
 * Check for and execute a deferred connect (set by discovery callback).
 * Must be called from task context (bt_task main loop), not from BT
 * callbacks, because bt_conn_create_br blocks on HCI cmd_send_sync.
 * @return true if a deferred connect was processed
 */
bool bt_hid_host_poll_connect(void);

/**
 * Poll discovery_results[] for early termination: if a gamepad
 * appears before the full inquiry period, stop inquiry immediately
 * and initiate connection. Call from task context at ~10 Hz.
 * @return true if early termination triggered
 */
bool bt_hid_host_poll_scan_early(void);

/**
 * Execute deferred security establishment (auth+encrypt).
 * Must be called from task context because it resets internal
 * bt_conn state and calls bt_conn_set_security (which uses
 * bt_hci_cmd_send_sync — would deadlock in BT callback).
 * @return true if a deferred security request was processed
 */
bool bt_hid_host_poll_security(void);

/**
 * Start deferred L2CAP channel creation (for outgoing connections
 * after security_changed fires). Must be called from task context.
 * @return true if a deferred L2CAP was processed
 */
bool bt_hid_host_poll_sdp(void);

/**
 * Security establishment watchdog. If ACL connected but security
 * not established within timeout, take corrective action.
 * @return true if watchdog triggered
 */
bool bt_hid_host_poll_security_watchdog(void);

/**
 * Fallback for incoming connections: if the controller doesn't open L2CAP
 * HID channels within 2s after security completes, the DongleRE initiates.
 */
bool bt_hid_host_poll_incoming_l2cap_fallback(void);

/**
 * Check if a stale ACL connection object exists (e.g. after force
 * disconnect, waiting for conn_disconnected callback).
 * @return true if hid_ctx.conn != NULL
 */
bool bt_hid_host_has_pending_conn(void);

/**
 * Force-release a stale ACL connection object that conn_disconnected
 * never cleaned up. Call only as a last-resort fallback.
 */
void bt_hid_host_drop_stale_conn(void);

/**
 * Read RSSI from the connected device via HCI.
 * Must be called from task context (uses bt_hci_cmd_send_sync).
 * @return 0 on success, negative on error
 */
int bt_hid_host_read_rssi(int8_t *rssi);

/**
 * Get the last cached RSSI value (safe to call from ISR context).
 * @return last RSSI in dBm, or 0 if not available
 */
int8_t bt_hid_host_get_cached_rssi(void);

/* R1.3：链路诊断（task 上下文，会发 HCI 同步命令）：RSSI / 链路质量 / AFH 在用信道数，写进 PERF */
void bt_hid_host_read_link_stats(void);
/* 最近一次读到的：rssi(u8) | 链路质量<<8 | AFH 在用信道数<<16 | AFH 模式<<24 */
uint32_t bt_hid_host_link_stats_packed(void);
/* R1.4：连上后调一次（task 上下文）：读角色 + 链路监督超时，是主就把监督超时设成 10 s，记 DIAG */
void bt_hid_host_link_params(void);
/* R1.5：「接收器当主」开关（0xF6 命令 0x11，ISR 里调安全：只置标志）。1 = 每次连上都申请当主。
 * R1.6 起默认 1；发 0 关掉（不落盘，接收器拔插后恢复 1） */
void bt_hid_host_set_role_req(uint8_t on);
/* R1.5：覆盖 SDK hci_core.c 里的弱符号——每个普通 HCI 事件先经过这里（只记诊断） */
void bt_hci_evt_tap(uint8_t evt, const uint8_t *data, uint16_t len);

/**
 * Disable page scan + inquiry scan to silence the BT radio.
 * Call when USB host is confirmed offline (e.g. sustained SUSPEND after
 * controller power-off) to reduce power consumption / heat.
 * Re-enabled automatically on next bt_hid_host_try_reconnect() or scan_start().
 */
void bt_hid_host_radio_idle(void);

/**
 * Re-enable page scan + inquiry scan after radio_idle.
 * Also triggers reconnection attempt. Call from task context only.
 */
void bt_hid_host_radio_wake(void);

/* S31：读 HCI Scan_Enable（bit0 inquiry scan / bit1 page scan），失败返回负数。 */
int bt_hid_host_read_scan_enable(void);

#endif /* BT_HID_HOST_H */
