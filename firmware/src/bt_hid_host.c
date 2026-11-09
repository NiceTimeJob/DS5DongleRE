#include "bt_hid_host.h"
#include "ds5_protocol.h"
#include "dse.h"
#include "perf.h"      /* R1.3：链路诊断进 PERF */
#include "diag.h"      /* S27：断连排查——BT 状态机事件进诊断环（RPC 0x14 可读） */

#include <bluetooth/bluetooth.h>
#include <bluetooth/conn.h>
#include <bluetooth/l2cap.h>
#include <bluetooth/sdp.h>
#include <bluetooth/hci_host.h>
#include "conn_internal.h"
#include "keys.h"
#include "settings.h"
#include <string.h>
#include "debug_log.h"
#include <errno.h>
#include "bflb_efuse.h"
#include "hci_core.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "bl616_glb.h"
#include "bflb_mtimer.h"   /* R1.5：HCI 事件旁路 / AFH 记录的限流计时 */


#define BT_INQUIRY_LEN         5
#define BT_MAX_DISCOVERED      8
#define L2CAP_BR_MTU           672

/* ---------- feature report cache ---------- */

#define FEATURE_CACHE_SLOTS  24
#define FEATURE_DATA_MAX     256

static struct {
    uint8_t  report_id;
    uint8_t  data[FEATURE_DATA_MAX];
    uint16_t len;
} feature_cache[FEATURE_CACHE_SLOTS];

static void feature_cache_clear(void)
{
    memset(feature_cache, 0, sizeof(feature_cache));
}

static void feature_cache_store(uint8_t report_id,
                                const uint8_t *data, uint16_t len)
{
    if (len > FEATURE_DATA_MAX)
        len = FEATURE_DATA_MAX;

    int slot = -1;
    for (int i = 0; i < FEATURE_CACHE_SLOTS; i++) {
        if (feature_cache[i].report_id == report_id && feature_cache[i].len > 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        for (int i = 0; i < FEATURE_CACHE_SLOTS; i++) {
            if (feature_cache[i].len == 0) {
                slot = i;
                break;
            }
        }
    }
    if (slot < 0)
        slot = 0;

    feature_cache[slot].report_id = report_id;
    memcpy(feature_cache[slot].data, data, len);
    feature_cache[slot].len = len;
}

/* ---------- persistent blacklist ----------
 *
 * When the user clears bonds (BOOT long-press), all bonded MACs are added
 * to a persistent blacklist stored in flash. This prevents the controller
 * from silently re-pairing via PS-only button press (controller-initiated
 * reconnect). The user must put the controller into PS+Share pairing mode
 * (DongleRE-initiated inquiry) to re-pair, which removes the MAC from the
 * blacklist on successful L2CAP HID connection.
 * Mirrors DS5DongleRE's bt_cleared_addrs / BLCK TLV mechanism. */

#define BLACKLIST_MAX     8
#define NV_BLACKLIST_KEY  "DS5_BLK"

static bt_addr_t blacklist_addrs[BLACKLIST_MAX];
static int       blacklist_count;
static volatile bool blacklist_dirty;

static void blacklist_load(void)
{
    size_t real_len = 0;
    int ret = bt_settings_get_bin(NV_BLACKLIST_KEY,
                                  (u8_t *)blacklist_addrs,
                                  sizeof(blacklist_addrs), &real_len);
    if (ret == 0 && real_len > 0 &&
        (real_len % sizeof(bt_addr_t)) == 0) {
        blacklist_count = (int)(real_len / sizeof(bt_addr_t));
        if (blacklist_count > BLACKLIST_MAX)
            blacklist_count = BLACKLIST_MAX;
        LOG_INF("[BT] Blacklist: loaded %d entries\n", blacklist_count);
    } else {
        blacklist_count = 0;
    }
}

static void blacklist_persist(void)
{
    if (blacklist_count == 0) {
        settings_delete(NV_BLACKLIST_KEY);
    } else {
        bt_settings_set_bin(NV_BLACKLIST_KEY,
                            (const u8_t *)blacklist_addrs,
                            blacklist_count * sizeof(bt_addr_t));
    }
}

static bool blacklist_contains(const bt_addr_t *addr)
{
    for (int i = 0; i < blacklist_count; i++) {
        if (bt_addr_cmp(addr, &blacklist_addrs[i]) == 0)
            return true;
    }
    return false;
}

static void __attribute__((unused)) blacklist_add(const bt_addr_t *addr)
{
    if (blacklist_count >= BLACKLIST_MAX || blacklist_contains(addr))
        return;
    bt_addr_copy(&blacklist_addrs[blacklist_count++], addr);
    char a[BT_ADDR_STR_LEN];
    bt_addr_to_str(addr, a, sizeof(a));
    LOG_INF("[BT] Blacklist: added %s\n", a);
}

static void blacklist_remove(const bt_addr_t *addr)
{
    for (int i = 0; i < blacklist_count; i++) {
        if (bt_addr_cmp(addr, &blacklist_addrs[i]) == 0) {
            for (int j = i; j < blacklist_count - 1; j++)
                bt_addr_copy(&blacklist_addrs[j], &blacklist_addrs[j + 1]);
            blacklist_count--;
            char a[BT_ADDR_STR_LEN];
            bt_addr_to_str(addr, a, sizeof(a));
            LOG_INF("[BT] Blacklist: removed %s, %d remaining\n",
                   a, blacklist_count);
            return;
        }
    }
}

/* ---------- multi-controller memory ---------- */

#define MAX_BONDED_CONTROLLERS 8
#define NV_ACTIVE_CTRL_KEY     "active_ctrl"

static bt_addr_t bonded_list[MAX_BONDED_CONTROLLERS];
static uint8_t   bonded_count = 0;
static uint8_t   active_idx   = 0;

static void bonded_list_collect_cb(const struct bt_br_bond_info *info,
                                   void *user_data)
{
    (void)user_data;
    if (bonded_count < MAX_BONDED_CONTROLLERS) {
        bt_addr_copy(&bonded_list[bonded_count], info->addr);
        bonded_count++;
    }
}

static void bonded_list_load(void)
{
    bonded_count = 0;
    bt_br_foreach_bond(bonded_list_collect_cb, NULL);

    size_t real_len = 0;
    uint8_t stored_idx = 0;
    int ret = bt_settings_get_bin(NV_ACTIVE_CTRL_KEY, &stored_idx,
                                  sizeof(stored_idx), &real_len);
    if (ret == 0 && real_len == 1 && stored_idx < bonded_count) {
        active_idx = stored_idx;
    } else {
        active_idx = 0;
    }

    LOG_INF("[BT] Bonded list: %d controllers, active_idx=%d\n",
           bonded_count, active_idx);
    for (int i = 0; i < bonded_count; i++) {
        char a[BT_ADDR_STR_LEN];
        bt_addr_to_str(&bonded_list[i], a, sizeof(a));
        LOG_INF("[BT]   [%d]%s %s\n", i, (i == active_idx) ? "*" : " ", a);
    }
}

static void bonded_list_persist_idx(void)
{
    bt_settings_set_bin(NV_ACTIVE_CTRL_KEY, &active_idx, sizeof(active_idx));
}

static int bonded_list_find_addr(const bt_addr_t *addr)
{
    for (int i = 0; i < bonded_count; i++) {
        if (bt_addr_cmp(&bonded_list[i], addr) == 0)
            return i;
    }
    return -1;
}

/* ---------- internal state ---------- */

static struct {
    volatile enum bt_hid_host_state state;
    struct bt_conn *conn;
    bt_addr_t target_addr;
    bool has_target;

    struct bt_l2cap_br_chan ctrl_chan;
    struct bt_l2cap_br_chan intr_chan;
    volatile bool ctrl_connected;
    volatile bool intr_connected;
    volatile bool ctrl_accepting;
    volatile bool intr_accepting;
    bool hid_service_found;

    volatile bool outgoing;
    volatile bool is_dse;
    volatile bool check_dse;

    volatile bool connect_pending;
    volatile TickType_t connect_pending_tick;
    bt_addr_t     pending_addr;

    volatile bool fallback_retry;

    volatile bool sdp_pending;

    /* Security establishment deferred to task context */
    volatile bool security_pending;
    volatile bool security_done;
    volatile TickType_t acl_connect_tick;
    volatile TickType_t security_done_tick;

    /* Paced handshake: send feature GETs one at a time */
    const uint8_t *handshake_ids;
    volatile uint8_t handshake_count;
    volatile uint8_t handshake_next;
    volatile bool handshake_send_failed;

    bt_hid_input_cb_t  input_cb;
    bt_hid_state_cb_t  state_cb;
    bt_hid_primer_cb_t primer_cb;
    uint16_t filter_vid;
    uint16_t filter_pid;
} hid_ctx;

/* L2CAP requires headroom for ACL + L2CAP headers (typically ~8 bytes).
 * BT_L2CAP_BUF_SIZE adds this headroom to the payload MTU. */
#ifndef BT_L2CAP_BUF_SIZE
#define BT_L2CAP_BUF_SIZE(mtu) ((mtu) + 8)
#endif

struct net_buf_pool hid_tx_pool;
struct net_buf_pool hid_rx_pool;

static bool first_intr_logged = false;
static bool first_output_logged = false;

/* ---------- Application-level TX FIFO (CAN_SEND_NOW equivalent) ----------
 *
 * DS5DongleRE uses BTstack's L2CAP_EVENT_CAN_SEND_NOW for flow control:
 *   bt_write() → enqueue to send_fifo (10-deep)
 *   CAN_SEND_NOW fires → dequeue one → l2cap_send → if more, request again
 *
 * Zephyr equivalent:
 *   bt_hid_host_send_output() → enqueue to app_tx_fifo (10-deep)
 *   on_hid_output_sent() fires when HCI ACKs the packet → drain next one
 *
 * This ensures only ONE packet is in-flight in the BT stack at any time,
 * preventing net_buf pool exhaustion and HCI credit starvation.
 */
#define APP_TX_DEPTH    10
#define APP_TX_DATA_SZ  80   /* 1 HID header + 79 bytes (covers 0x31 78-byte report) */

static uint8_t  app_tx_buf[APP_TX_DEPTH][APP_TX_DATA_SZ];
static uint16_t app_tx_len[APP_TX_DEPTH];
static volatile uint8_t  app_tx_head = 0;   /* next slot to consume */
static volatile uint8_t  app_tx_tail = 0;   /* next slot to fill */
static volatile bool     app_tx_busy = false; /* true: one packet is in HCI */

/* bt_conn_tx_cb_t: SDK declaration from conn_internal.h */
static void hid_tx_drain_one(void);  /* forward declaration */

static void on_hid_output_sent(struct bt_conn *conn, void *user_data)
{
    /* HCI controller acknowledged the previous packet — equivalent to
     * BTstack's L2CAP_EVENT_CAN_SEND_NOW: mark idle then drain next. */
    app_tx_busy = false;
    hid_tx_drain_one();
}

static void hid_tx_drain_one(void)
{
    if (app_tx_busy)
        return;
    if (app_tx_head == app_tx_tail)
        return;  /* FIFO empty */
    if (!hid_ctx.intr_connected || !hid_ctx.conn)
        return;

    app_tx_busy = true;

    uint8_t idx = app_tx_head;
    app_tx_head = (app_tx_head + 1) % APP_TX_DEPTH;

    struct net_buf *buf = net_buf_alloc(&hid_tx_pool, K_NO_WAIT);
    if (!buf) {
        LOG_WRN("[HID] drain: pool empty, dropping frame (head=%u tail=%u)\n",
               app_tx_head, app_tx_tail);
        app_tx_busy = false;
        return;
    }

    net_buf_reserve(buf, BT_L2CAP_CHAN_SEND_RESERVE);
    net_buf_add_mem(buf, app_tx_buf[idx], app_tx_len[idx]);

    /* bt_l2cap_br_chan_send_cb added to SDK l2cap_br.c */
    extern int bt_l2cap_br_chan_send_cb(struct bt_l2cap_chan *chan,
                                        struct net_buf *buf,
                                        bt_conn_tx_cb_t cb, void *user_data);

    int err = bt_l2cap_br_chan_send_cb(&hid_ctx.intr_chan.chan, buf,
                                        on_hid_output_sent, NULL);
    if (err < 0) {
        LOG_ERR("[HID] drain: send err=%d\n", err);
        net_buf_unref(buf);
        app_tx_busy = false;
    }
    /* On success: app_tx_busy stays true until on_hid_output_sent fires */
}

static void app_tx_fifo_reset(void)
{
    app_tx_head = 0;
    app_tx_tail = 0;
    app_tx_busy = false;
}

static struct bt_br_discovery_result discovery_results[BT_MAX_DISCOVERED];

/* ---------- helpers ---------- */

static const bt_addr_t *conn_get_br_addr(const struct bt_conn *conn)
{
    struct bt_conn_info info;
    if (bt_conn_get_info(conn, &info) == 0)
        return info.br.dst;
    return NULL;
}

static const char *state_name(enum bt_hid_host_state s) {
    switch (s) {
    case BT_HID_STATE_IDLE:          return "IDLE";
    case BT_HID_STATE_SCANNING:      return "SCANNING";
    case BT_HID_STATE_CONNECTING:    return "CONNECTING";
    case BT_HID_STATE_SDP_QUERY:     return "SDP_QUERY";
    case BT_HID_STATE_L2CAP_CONTROL: return "L2CAP_CTRL";
    case BT_HID_STATE_L2CAP_INTERRUPT: return "L2CAP_INTR";
    case BT_HID_STATE_HANDSHAKE:     return "HANDSHAKE";
    case BT_HID_STATE_CONNECTED:     return "CONNECTED";
    case BT_HID_STATE_DISCONNECTING: return "DISCONNECTING";
    default:                         return "?";
    }
}

/* S33：连续主动 page 失败的次数。等待回连时每 ~9 s 一轮 IDLE→CONNECTING→(超时)→IDLE 会往
 * 25 格的 DIAG 环里写 3 条，75 s 就把掉线现场全冲掉。前 3 轮照记，之后只在第 20/40/… 轮
 * 记一次（ACL_UP 的 a1 高 16 位 = 累计失败次数）。任何一次连上就清零。 */
static uint16_t page_fail_streak;

static void set_state(enum bt_hid_host_state new_state)
{
    LOG_INF("[BT-FSM] %s -> %s\n", state_name(hid_ctx.state), state_name(new_state));
    /* a1 低 8 位=旧状态，高 8 位=新状态（a2 传不到主机，打包进 a1） */
    bool page_cycle = (hid_ctx.state == BT_HID_STATE_IDLE && new_state == BT_HID_STATE_CONNECTING) ||
                      (hid_ctx.state == BT_HID_STATE_CONNECTING && new_state == BT_HID_STATE_IDLE);
    bool quiet = page_cycle && page_fail_streak > 3 && (page_fail_streak % 20) != 0;
    if (!quiet)
        diag_log(DIAG_EV_STATE, (uint32_t)hid_ctx.state | ((uint32_t)new_state << 8), 0);
    hid_ctx.state = new_state;

    if (new_state == BT_HID_STATE_CONNECTED && hid_ctx.conn) {
        const bt_addr_t *remote = conn_get_br_addr(hid_ctx.conn);
        if (remote) {
            bt_addr_copy(&hid_ctx.target_addr, remote);
            hid_ctx.has_target = true;
            char a[BT_ADDR_STR_LEN];
            bt_addr_to_str(remote, a, sizeof(a));
            LOG_INF("[BT] Connected to bonded device: %s\n", a);
        }
    }

    if (hid_ctx.state_cb)
        hid_ctx.state_cb(new_state);
}

/* ---------- L2CAP callbacks ---------- */

static struct net_buf *l2cap_alloc_buf(struct bt_l2cap_chan *chan)
{
    return net_buf_alloc(&hid_rx_pool, K_NO_WAIT);
}

static void handshake_send_next(void)
{
    if (hid_ctx.state != BT_HID_STATE_HANDSHAKE)
        return;

    if (hid_ctx.handshake_next < hid_ctx.handshake_count) {
        uint8_t rid = hid_ctx.handshake_ids[hid_ctx.handshake_next];
        if (bt_hid_host_get_feature(rid) == 0) {
            hid_ctx.handshake_next++;
            hid_ctx.handshake_send_failed = false;
            diag_log(DIAG_EV_HANDSHAKE, rid, hid_ctx.handshake_next);
        } else {
            hid_ctx.handshake_send_failed = true;
            diag_log(DIAG_EV_HANDSHAKE, 0x8000u | rid, hid_ctx.handshake_next);
            LOG_ERR("[HID] GET_REPORT(0x%02x) send failed, will retry\n", rid);
        }
    } else if (hid_ctx.check_dse &&
               hid_ctx.state == BT_HID_STATE_HANDSHAKE) {
        hid_ctx.check_dse = false;
        hid_ctx.is_dse    = false;
        LOG_INF("[HID] DualSense (non-Edge) assumed (handshake complete)\n");
        set_state(BT_HID_STATE_CONNECTED);
    }
}

static int l2cap_ctrl_recv(struct bt_l2cap_chan *chan, struct net_buf *buf)
{
    uint8_t *data = buf->data;
    uint16_t len  = buf->len;

    if (len < 1)
        return 0;

    uint8_t header = data[0];
    uint8_t trans  = (header >> 4) & 0x0F;
    uint8_t param  = header & 0x0F;

    switch (trans) {
    case HID_TRANS_HANDSHAKE:
        LOG_INF("[HID] Handshake: 0x%02x\n", param);
        if (hid_ctx.check_dse && param == HID_HANDSHAKE_INVALID_ID) {
            hid_ctx.check_dse = false;
            hid_ctx.is_dse    = false;
            LOG_INF("[HID] DualSense (non-Edge) confirmed\n");
            set_state(BT_HID_STATE_CONNECTED);
        }
        handshake_send_next();
        break;

    case HID_TRANS_DATA:
        if (param == HID_REPORT_FEATURE) {
            if (len > 2) {
                uint8_t rid = data[1];
                LOG_INF("[HID] Feature report received, id=0x%02x len=%d\n",
                       rid, len - 2);

                feature_cache_store(rid, data + 2, len - 2);

                if (rid == 0x70 && hid_ctx.check_dse) {
                    hid_ctx.check_dse = false;
                    hid_ctx.is_dse    = true;
                    LOG_INF("[HID] DualSense Edge detected\n");
                    dse_on_edge_detected();
                    set_state(BT_HID_STATE_CONNECTED);
                }
            } else {
                LOG_WRN("[HID] Feature DATA too short (len=%d), skipping\n",
                       len);
            }
            handshake_send_next();
        }
        break;

    default:
        LOG_DBG("[HID] Control: trans=0x%x param=0x%x len=%d\n",
               trans, param, len);
        break;
    }

    dse_on_control_packet(data, len);

    return 0;
}

static int l2cap_intr_recv(struct bt_l2cap_chan *chan, struct net_buf *buf)
{
    uint8_t *data = buf->data;
    uint16_t len  = buf->len;

    if (len < 2)
        return 0;

    uint8_t header = data[0];
    uint8_t trans  = (header >> 4) & 0x0F;
    uint8_t param  = header & 0x0F;

    if (trans == HID_TRANS_DATA && param == HID_REPORT_INPUT) {
        if (!first_intr_logged) {
            first_intr_logged = true;
            diag_log(DIAG_EV_INPUT_FIRST, len, data[1]);
            LOG_INF("[HID] First input report from controller: len=%d rid=0x%02x\n",
                   len - 1, data[1]);
        }
        if (hid_ctx.input_cb) {
            hid_ctx.input_cb(data + 1, len - 1);
        }
    } else {
        LOG_WRN("[HID] intr_recv: unexpected trans=0x%x param=0x%x len=%d\n",
               trans, param, len);
    }

    return 0;
}

static void l2cap_ctrl_connected(struct bt_l2cap_chan *chan)
{
    LOG_DBG("[HID-CB] l2cap_ctrl_connected ENTER chan=%p state=%d\n",
           chan, hid_ctx.state);
    hid_ctx.ctrl_accepting = false;
    hid_ctx.ctrl_connected = true;
    diag_log(DIAG_EV_L2CAP_CTRL_UP, hid_ctx.outgoing, 0);

    /* Only create outgoing interrupt channel if WE initiated the connection
     * (via inquiry → connect). For controller-initiated reconnects, the
     * controller will open the interrupt channel via the L2CAP server. */
    if (hid_ctx.outgoing) {
        set_state(BT_HID_STATE_L2CAP_INTERRUPT);

        int err = bt_l2cap_chan_connect(hid_ctx.conn,
                                        &hid_ctx.intr_chan.chan,
                                        HID_PSM_INTERRUPT);
        if (err) {
            LOG_ERR("[HID] Failed to connect interrupt channel: %d\n", err);
            bt_conn_disconnect(hid_ctx.conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        }
    }
}

static void l2cap_intr_connected(struct bt_l2cap_chan *chan)
{
    LOG_DBG("[HID-CB] l2cap_intr_connected ENTER chan=%p state=%d\n",
           chan, hid_ctx.state);
    hid_ctx.intr_accepting = false;
    hid_ctx.intr_connected = true;
    diag_log(DIAG_EV_L2CAP_INTR_UP, 0, 0);


    /* Fire primer callback immediately — equivalent to DS5DongleRE's
     * L2CAP_EVENT_CHANNEL_OPENED handler calling update_state(yellow).
     * This is the earliest possible moment to send the 0x32 primer. */
    if (hid_ctx.primer_cb)
        hid_ctx.primer_cb();

    /* Successful L2CAP HID open removes this MAC from the blacklist,
     * treating it as an explicit user re-pair (PS+Share mode). */
    if (hid_ctx.conn) {
        const bt_addr_t *remote = conn_get_br_addr(hid_ctx.conn);
        if (remote && blacklist_contains(remote)) {
            blacklist_remove(remote);
            blacklist_dirty = true;
        }
    }

    /* Stop accepting further connections while connected (matches
     * DS5DongleRE's gap_connectable_control(false) on HID open). */
    bt_br_set_connectable(false);
    bt_br_set_discoverable(false);

    /* S31：原来这里把 Link Supervision Timeout 从默认 ~20 s 压到 5 s（HCI 0x0037），
     * 上游 3.21 的"蓝牙连接层重构"把这条命令删掉了。5 s 的监督超时在射频被干扰时
     * 更容易判成断链；死链本来就有 main.c 的 3 s 无输入看门狗兜底，照 3.21 删除。 */

    set_state(BT_HID_STATE_HANDSHAKE);

    /* Request feature reports in the same order as DS5DongleRE:
     * 0x09 (pairing/firmware), 0x20 (firmware info), 0x22 (additional),
     * 0x05 (calibration), then 0x70 to probe for DualSense Edge.
     * Paced: send one at a time, next sent in l2cap_ctrl_recv on response. */
    static const uint8_t handshake_ids[] = {
        DS5_FEATURE_REPORT_PAIRING, DS5_FEATURE_REPORT_FIRMWARE,
        0x22, DS5_FEATURE_REPORT_CALIBRATION, 0x70
    };
    hid_ctx.handshake_ids   = handshake_ids;
    hid_ctx.handshake_count = sizeof(handshake_ids);
    hid_ctx.handshake_next  = 0;
    hid_ctx.handshake_send_failed = false;
    hid_ctx.check_dse       = true;

    handshake_send_next();
}

static void l2cap_ctrl_disconnected(struct bt_l2cap_chan *chan)
{
    LOG_INF("[HID] Control channel disconnected (fallback_retry=%d conn=%p)\n",
           hid_ctx.fallback_retry, hid_ctx.conn);
    hid_ctx.ctrl_accepting = false;
    hid_ctx.ctrl_connected = false;
    diag_log(DIAG_EV_L2CAP_CTRL_DOWN, hid_ctx.intr_connected, 0);
    struct bt_conn *c = hid_ctx.conn;   /* R1：只读一次 */
    if (!hid_ctx.intr_connected && c) {
        set_state(BT_HID_STATE_DISCONNECTING);
        bt_conn_disconnect(c, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    }
}

static void l2cap_intr_disconnected(struct bt_l2cap_chan *chan)
{
    LOG_INF("[HID] Interrupt channel disconnected\n");
    hid_ctx.intr_accepting = false;
    hid_ctx.intr_connected = false;
    diag_log(DIAG_EV_L2CAP_INTR_DOWN, hid_ctx.ctrl_connected, 0);
    struct bt_conn *c = hid_ctx.conn;   /* R1：只读一次 */
    if (!hid_ctx.ctrl_connected && c) {
        set_state(BT_HID_STATE_DISCONNECTING);
        bt_conn_disconnect(c, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    }
}

static struct bt_l2cap_chan_ops ctrl_ops = {
    .alloc_buf    = l2cap_alloc_buf,
    .recv         = l2cap_ctrl_recv,
    .connected    = l2cap_ctrl_connected,
    .disconnected = l2cap_ctrl_disconnected,
};

static struct bt_l2cap_chan_ops intr_ops = {
    .alloc_buf    = l2cap_alloc_buf,
    .recv         = l2cap_intr_recv,
    .connected    = l2cap_intr_connected,
    .disconnected = l2cap_intr_disconnected,
};

/* ---------- SDP callback ---------- */

static uint8_t sdp_discover_cb(struct bt_conn *conn,
                                struct bt_sdp_client_result *result)
{
    if (conn != hid_ctx.conn)
        return BT_SDP_DISCOVER_UUID_STOP;

    if (!result) {
        LOG_INF("[SDP] Discovery complete, HID found=%d\n",
               hid_ctx.hid_service_found);

        if (!hid_ctx.hid_service_found) {
            LOG_INF("[SDP] No HID service, disconnecting\n");
            bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
            return BT_SDP_DISCOVER_UUID_STOP;
        }

        set_state(BT_HID_STATE_L2CAP_CONTROL);

        memset(&hid_ctx.ctrl_chan, 0, sizeof(hid_ctx.ctrl_chan));
        memset(&hid_ctx.intr_chan, 0, sizeof(hid_ctx.intr_chan));

        hid_ctx.ctrl_chan.chan.ops = &ctrl_ops;
        hid_ctx.ctrl_chan.rx.mtu  = L2CAP_BR_MTU;
        hid_ctx.intr_chan.chan.ops = &intr_ops;
        hid_ctx.intr_chan.rx.mtu  = L2CAP_BR_MTU;

        int err = bt_l2cap_chan_connect(conn, &hid_ctx.ctrl_chan.chan,
                                           HID_PSM_CONTROL);
        if (err) {
            LOG_ERR("[SDP] Failed to connect control channel: %d\n", err);
            bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        }
        return BT_SDP_DISCOVER_UUID_STOP;
    }

    LOG_INF("[SDP] Found HID service record\n");
    hid_ctx.hid_service_found = true;
    return BT_SDP_DISCOVER_UUID_CONTINUE;
}

/* ---------- SSP Authentication ---------- */

static void auth_passkey_confirm(struct bt_conn *conn, unsigned int passkey)
{
    LOG_INF("[BT] SSP confirm passkey: %06u (auto-accepting)\n", passkey);
    bt_conn_auth_passkey_confirm(conn);
}

static void auth_passkey_display(struct bt_conn *conn, unsigned int passkey)
{
    LOG_INF("[BT] SSP display passkey: %06u\n", passkey);
}

static void auth_cancel(struct bt_conn *conn)
{
    LOG_INF("[BT] SSP pairing cancelled\n");
}

static void auth_pairing_complete(struct bt_conn *conn, bool bonded)
{
    LOG_INF("[BT] Pairing complete, bonded=%d\n", bonded);

    if (bonded) {
        const bt_addr_le_t *dst = bt_conn_get_dst(conn);
        if (dst) {
            const bt_addr_t *addr = &dst->a;
            int idx = bonded_list_find_addr(addr);
            if (idx >= 0) {
                active_idx = (uint8_t)idx;
            } else {
                /* link_key_notify may not have fired yet, so
                 * bt_br_foreach_bond won't find the new key.
                 * Manually add this address to bonded_list. */
                if (bonded_count < MAX_BONDED_CONTROLLERS) {
                    bt_addr_copy(&bonded_list[bonded_count], addr);
                    active_idx = bonded_count;
                    bonded_count++;
                } else {
                    active_idx = 0;
                }
            }
            bonded_list_persist_idx();
            LOG_INF("[BT] Active controller updated: idx=%d, total=%d\n",
                   active_idx, bonded_count);
        }
    }
}

static void auth_pairing_failed(struct bt_conn *conn,
                                 enum bt_security_err reason)
{
    LOG_ERR("[BT] Pairing failed, reason=%d\n", reason);
}

static struct bt_conn_auth_cb auth_callbacks = {
    .passkey_display   = auth_passkey_display,
    .passkey_confirm   = auth_passkey_confirm,
    .cancel            = auth_cancel,
    .pairing_complete  = auth_pairing_complete,
    .pairing_failed    = auth_pairing_failed,
};

/* ---------- SDP ---------- */

static void start_sdp_discovery(struct bt_conn *conn)
{
    static struct bt_sdp_discover_params params;

    static struct bt_uuid_16 hid_uuid = BT_UUID_INIT_16(0x1124);

    params.uuid  = &hid_uuid.uuid;
    params.func  = sdp_discover_cb;
    params.pool  = &hid_tx_pool;

    set_state(BT_HID_STATE_SDP_QUERY);

    int err = bt_sdp_discover(conn, &params);
    if (err) {
        LOG_ERR("[SDP] Failed to start discovery: %d\n", err);
        bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    }
}

/* ---------- BR/EDR connection callbacks ---------- */

static void security_changed(struct bt_conn *conn, bt_security_t level,
                              enum bt_security_err err)
{
    LOG_DBG("[BT-CB] security_changed ENTER conn=%p level=%d err=%d state=%d\n",
           conn, level, err, hid_ctx.state);

    if (conn != hid_ctx.conn) {
        LOG_DBG("[BT-CB] security_changed: not our conn, ignoring\n");
        return;
    }

    /* a1 低 8 位=level，高 8 位=err */
    diag_log(DIAG_EV_SECURITY, (uint32_t)level | ((uint32_t)err << 8),
             hid_ctx.outgoing);

    if (err) {
        LOG_ERR("[BT] Security elevation failed: level=%d err=%d\n", level, err);
        /* Clear stale link key so next reconnect triggers fresh SSP pairing
         * (mirrors DS5DongleRE's gap_drop_link_key on AUTHENTICATION_COMPLETE fail) */
        const bt_addr_t *remote = conn_get_br_addr(conn);
        if (remote) {
            extern void bt_keys_link_key_clear_addr(const bt_addr_t *addr);
            bt_keys_link_key_clear_addr(remote);
            LOG_WRN("[BT] Cleared stale link key for re-pairing\n");
        }
        bt_conn_disconnect(conn, BT_HCI_ERR_AUTH_FAIL);
        return;
    }

    LOG_INF("[BT] Security level raised to %d (outgoing=%d state=%s)\n",
           level, hid_ctx.outgoing, state_name(hid_ctx.state));

    hid_ctx.security_done = true;
    hid_ctx.security_done_tick = xTaskGetTickCount();

    if (level >= BT_SECURITY_L2 && hid_ctx.outgoing
        && (hid_ctx.state == BT_HID_STATE_CONNECTING
            || hid_ctx.state == BT_HID_STATE_SDP_QUERY)) {
        /* Encryption complete for outgoing connection (new pairing).
         * Defer L2CAP channel creation to task context, matching
         * DS5DongleRE's ENCRYPTION_CHANGE + new_pair path. */
        LOG_DBG("[BT] security_changed: outgoing — deferring L2CAP to task\n");
        hid_ctx.sdp_pending = true;
    } else if (level >= BT_SECURITY_L2 && !hid_ctx.outgoing) {
        /* Incoming reconnection: encryption OK.
         * Controller will create L2CAP channels via our servers. */
        LOG_INF("[BT] security OK (incoming), waiting for controller L2CAP\n");
    }
    LOG_DBG("[BT-CB] security_changed EXIT\n");
}

static void conn_connected(struct bt_conn *conn, uint8_t err)
{
    LOG_DBG("[BT-CB] conn_connected ENTER conn=%p err=%d state=%d\n",
           conn, err, hid_ctx.state);

    if (err) {
        LOG_ERR("[BT] Connection failed: 0x%02x\n", err);
        if (hid_ctx.outgoing && page_fail_streak < 0xFFFF)
            page_fail_streak++;                        /* S33：主动 page 又没连上 */
        if (page_fail_streak <= 3 || (page_fail_streak % 20) == 0)
            diag_log(DIAG_EV_ACL_UP, 0x8000u | err | ((uint32_t)page_fail_streak << 16), 2);   /* 2=连接建立失败 */
        if (hid_ctx.conn != NULL && conn != hid_ctx.conn)
            return;
        hid_ctx.conn = NULL;
        hid_ctx.outgoing = false;
        bt_br_set_connectable(true);
        bt_br_set_discoverable(true);
        LOG_INF("[BT] Re-enabled page scan after failed connection\n");
        set_state(BT_HID_STATE_IDLE);
        return;
    }
    page_fail_streak = 0;   /* S33：连上了（主动或被动），安静模式解除 */

    char addr_str[BT_ADDR_STR_LEN];
    const bt_addr_t *remote = conn_get_br_addr(conn);
    if (remote)
        bt_addr_to_str(remote, addr_str, sizeof(addr_str));
    else
        snprintf(addr_str, sizeof(addr_str), "??:??:??:??:??:??");
    LOG_INF("[BT] Connected to %s\n", addr_str);

    if (hid_ctx.conn != NULL && conn != hid_ctx.conn) {
        LOG_WRN("[BT] Already connected, rejecting second ACL from %s\n",
               addr_str);
        bt_conn_disconnect(conn, BT_HCI_ERR_CONN_LIMIT_EXCEEDED);
        return;
    }

    if (hid_ctx.state == BT_HID_STATE_SCANNING)
        bt_br_discovery_stop();

    if (!hid_ctx.outgoing && remote && blacklist_contains(remote)) {
        LOG_WRN("[BT] Rejecting blacklisted device %s "
               "(re-pair via PS+Share to unblock)\n", addr_str);
        bt_conn_disconnect(conn, BT_HCI_ERR_AUTH_FAIL);
        return;
    }

    if (!hid_ctx.conn) {
        LOG_DBG("[BT] conn_connected: taking ref on conn=%p\n", conn);
        hid_ctx.conn = bt_conn_ref(conn);
    }

    hid_ctx.fallback_retry = false;
    hid_ctx.connect_pending = false;
    hid_ctx.connect_pending_tick = 0;
    LOG_INF("[BT] conn_connected: fallback_retry CLEARED (incoming conn)\n");

    set_state(BT_HID_STATE_CONNECTING);

    /* Log internal security state for diagnostics */
    LOG_DBG("[BT] conn handle=0x%04x sec_level=%d required_sec=%d encrypt=%d\n",
           conn->handle, conn->sec_level, conn->required_sec_level,
           conn->encrypt);

    /* Defer security establishment to task context.
     *
     * Root cause found in SDK: conn_new() does NOT clear the bt_conn struct,
     * so required_sec_level retains the stale value (L2) from the previous
     * connection.  bt_conn_set_security(L2) checks:
     *     if (sec_level >= sec || required_sec_level >= sec) return 0;
     * and returns immediately without doing anything.
     *
     * Fix: reset required_sec_level in poll_security() (task context) before
     * calling bt_conn_set_security.  We also can't call bt_hci_cmd_send_sync
     * from this callback (deadlock risk), so deferring is the safe approach. */
    diag_log(DIAG_EV_ACL_UP, conn->handle, hid_ctx.outgoing);

    hid_ctx.security_pending = true;
    hid_ctx.security_done = false;
    hid_ctx.acl_connect_tick = xTaskGetTickCount();

    LOG_DBG("[BT] %s connection — security deferred to task context\n",
           hid_ctx.outgoing ? "Outgoing" : "Incoming");
    LOG_DBG("[BT-CB] conn_connected EXIT\n");
}

static volatile int8_t cached_rssi = 1; /* 1 = unknown (positive → web shows "Unknown") */
static volatile bool switch_pending = false;

/* R1：拆链路径的竞态修复（2026-10-14 10:53:41 实抓整机复位，RST_REASON=EXCEPTION）。
 * 链路先静默断掉 → 音频任务的优雅断开、usb 任务的 3 s 无输入看门狗、bt 任务的
 * "DISCONNECTING 卡 1.5 s" 三条路前后脚都来拆同一条链；后两条进 force_disconnect，
 * 都卡在 bt_conn_disconnect 的 HCI 同步命令上。断开完成事件一到，BT 线程的
 * conn_disconnected 把 hid_ctx.conn unref 并置空，卡住的 force_disconnect 醒来再
 * bt_conn_unref(hid_ctx.conn) —— 读到 NULL → 空指针异常 → 复位（USB 也跟着掉）。
 * 现在：谁拆链谁先在临界区里把指针摘走，只有摘到的人才 unref；expect 非空时只摘这一条。 */
static struct bt_conn *conn_take(struct bt_conn *expect)
{
    struct bt_conn *c = NULL;
    taskENTER_CRITICAL();
    if (hid_ctx.conn && (expect == NULL || hid_ctx.conn == expect)) {
        c = hid_ctx.conn;
        hid_ctx.conn = NULL;
    }
    taskEXIT_CRITICAL();
    return c;
}

static void conn_disconnected(struct bt_conn *conn, uint8_t reason)
{
    LOG_INF("[BT-CB] conn_disconnected: conn=%p hid_ctx.conn=%p reason=0x%02x "
           "state=%d fallback_retry=%d\n",
           conn, hid_ctx.conn, reason, hid_ctx.state, hid_ctx.fallback_retry);

    struct bt_conn *mine = conn_take(conn);   /* R1：原子认领；没认领到 = 别人已经拆过 */
    if (!mine) {
        LOG_INF("[BT] Old ACL teardown complete (unowned conn, reason 0x%02x, "
               "state=%d)\n", reason, hid_ctx.state);
        /* S33：force_disconnect 先把 hid_ctx.conn 置空，之后 HCI 断开完成只会走到这里，
         * 以前这条分支不记 DIAG，于是「3 s 无输入 → 强拆」之后的真实原因码永远看不到。
         * a1 位 16 = 1 标记"强拆后补记"，低 8 位仍是 HCI reason。 */
        diag_log(DIAG_EV_ACL_DOWN, (uint32_t)reason | ((uint32_t)bonded_count << 8) | 0x10000u, 0);
        /* If a fallback outgoing connect is waiting for ACL teardown, fire it now */
        if (hid_ctx.connect_pending && hid_ctx.connect_pending_tick != 0 &&
            hid_ctx.state == BT_HID_STATE_IDLE) {
            hid_ctx.connect_pending_tick = xTaskGetTickCount();
            LOG_INF("[BT] ACL clear — fallback connect ready\n");
        }
        return;
    }

    const char *r_str = reason == 0x08 ? "CONN_TIMEOUT" :
                        reason == 0x13 ? "REMOTE_USER" :
                        reason == 0x16 ? "LOCAL_HOST" : "OTHER";
    LOG_INF("[BT] Disconnected reason=0x%02x (%s)\n", reason, r_str);
    /* 断连定性就靠这一条：a1 低 8 位=HCI reason，高 8 位=bonded_count */
    diag_log(DIAG_EV_ACL_DOWN, (uint32_t)reason | ((uint32_t)bonded_count << 8), 0);

    bt_conn_unref(mine);

    hid_ctx.ctrl_connected = false;
    hid_ctx.intr_connected = false;
    hid_ctx.ctrl_accepting = false;
    hid_ctx.intr_accepting = false;
    hid_ctx.outgoing = false;
    hid_ctx.is_dse = false;
    hid_ctx.check_dse = false;
    hid_ctx.handshake_next  = 0;
    hid_ctx.handshake_count = 0;
    hid_ctx.handshake_send_failed = false;
    hid_ctx.security_pending = false;
    hid_ctx.security_done = false;
    hid_ctx.sdp_pending = false;
    hid_ctx.connect_pending = false;

    first_intr_logged  = false;
    first_output_logged = false;

    app_tx_fifo_reset();

    feature_cache_clear();
    dse_reset();
    cached_rssi = 1;

    bt_br_set_connectable(true);
    bt_br_set_discoverable(true);

    LOG_INF("[BT] Re-enabled connectable+discoverable, ready for new connection\n");
    diag_log(DIAG_EV_SCAN_SET, 2, 0);   /* S37 */
    set_state(BT_HID_STATE_IDLE);

    if (switch_pending) {
        switch_pending = false;
        LOG_INF("[BT] Switch: old ACL teardown complete, waiting for "
               "controller [%d/%d] to reconnect\n", active_idx, bonded_count);
        return;
    }

    LOG_INF("[BT] conn_disconnected: fallback_retry=%d bonded_count=%d\n",
           hid_ctx.fallback_retry, bonded_count);
    if (hid_ctx.fallback_retry && bonded_count > 0) {
        hid_ctx.fallback_retry = false;
        memcpy(hid_ctx.pending_addr.val, bonded_list[active_idx].val, 6);
        hid_ctx.connect_pending = true;
        hid_ctx.connect_pending_tick = xTaskGetTickCount();
        LOG_INF("[BT] Fallback: outgoing connect scheduled (ACL already down)\n");
    }
}

static struct bt_conn_cb conn_callbacks = {
    .connected        = conn_connected,
    .disconnected     = conn_disconnected,
    .security_changed = security_changed,
};

/* ---------- Inquiry / Discovery ---------- */

static void discovery_complete(struct bt_br_discovery_result *results,
                               size_t count)
{
    LOG_INF("[BT] Inquiry complete, found %zu device(s)\n", count);

    if (hid_ctx.conn) {
        LOG_INF("[BT] Inquiry ended but ACL already established, skipping\n");
        return;
    }

    for (size_t i = 0; i < count; i++) {
        char addr_str[BT_ADDR_STR_LEN];
        bt_addr_to_str(&results[i].addr, addr_str, sizeof(addr_str));

        uint32_t cod = results[i].cod[0]
                     | ((uint32_t)results[i].cod[1] << 8)
                     | ((uint32_t)results[i].cod[2] << 16);
        uint8_t major = (cod >> 8) & 0x1F;
        uint8_t minor = (cod >> 2) & 0x3F;

        LOG_DBG("[BT] Device: %s COD=0x%06x (major=%d minor=%d)\n",
               addr_str, cod, major, minor);

        /* CoD Major Class 0x05 = Peripheral, Minor bit 1 = Gamepad */
        bool is_gamepad = (major == 0x05) && (minor & 0x02);

        if (is_gamepad || hid_ctx.has_target) {
            if (hid_ctx.has_target &&
                bt_addr_cmp(&results[i].addr, &hid_ctx.target_addr) != 0)
                continue;

            LOG_INF("[BT] Gamepad found, deferring connect to task context\n");
            bt_addr_copy(&hid_ctx.pending_addr, &results[i].addr);
            hid_ctx.connect_pending = true;
            hid_ctx.connect_pending_tick = 0;
            /* Set CONNECTING instead of IDLE to prevent the auto-reconnect
             * logic from starting a new scan before poll_connect runs. */
            set_state(BT_HID_STATE_CONNECTING);
            return;
        }
    }

    LOG_INF("[BT] No gamepad found, will retry via bt_task\n");
    set_state(BT_HID_STATE_IDLE);
}

/* ---------- L2CAP server (for controller-initiated reconnection) ---------- */

static int l2cap_server_reject_blacklisted(struct bt_conn *conn)
{
    const bt_addr_t *remote = conn_get_br_addr(conn);
    if (remote && blacklist_contains(remote)) {
        char a[BT_ADDR_STR_LEN];
        bt_addr_to_str(remote, a, sizeof(a));
        LOG_WRN("[HID] Rejecting L2CAP from blacklisted %s\n", a);
        return -EACCES;
    }
    return 0;
}

static void l2cap_server_ensure_conn(struct bt_conn *conn)
{
    if (!hid_ctx.conn) {
        hid_ctx.conn = bt_conn_ref(conn);
        set_state(BT_HID_STATE_CONNECTING);
    }
}

static int l2cap_ctrl_server_accept(struct bt_conn *conn,
                                     struct bt_l2cap_chan **chan)
{
    char addr_str[BT_ADDR_STR_LEN] = "??";
    const bt_addr_t *remote = conn_get_br_addr(conn);
    if (remote)
        bt_addr_to_str(remote, addr_str, sizeof(addr_str));

    LOG_DBG("[HID] Incoming L2CAP control (PSM 0x%04x) from %s "
           "state=%s conn=%p our_conn=%p\n",
           HID_PSM_CONTROL, addr_str, state_name(hid_ctx.state),
           conn, hid_ctx.conn);

    int err = l2cap_server_reject_blacklisted(conn);
    if (err) return err;

    /* Reject L2CAP from a different ACL than the primary connection —
     * prevents a racing "rejected" second controller from stealing channels */
    if (hid_ctx.conn && conn != hid_ctx.conn) {
        LOG_WRN("[HID] Control: rejecting L2CAP from non-primary conn %p "
               "(primary=%p)\n", conn, hid_ctx.conn);
        return -EACCES;
    }

    if (hid_ctx.ctrl_connected || hid_ctx.ctrl_accepting) {
        LOG_WRN("[HID] Control channel already connected/accepting\n");
        return -ENOMEM;
    }
    hid_ctx.ctrl_accepting = true;

    memset(&hid_ctx.ctrl_chan, 0, sizeof(hid_ctx.ctrl_chan));
    hid_ctx.ctrl_chan.chan.ops = &ctrl_ops;
    hid_ctx.ctrl_chan.rx.mtu  = L2CAP_BR_MTU;
    *chan = &hid_ctx.ctrl_chan.chan;

    l2cap_server_ensure_conn(conn);
    LOG_INF("[HID] Control server: accepted, chan=%p\n", *chan);
    return 0;
}

static int l2cap_intr_server_accept(struct bt_conn *conn,
                                     struct bt_l2cap_chan **chan)
{
    char addr_str[BT_ADDR_STR_LEN] = "??";
    const bt_addr_t *remote = conn_get_br_addr(conn);
    if (remote)
        bt_addr_to_str(remote, addr_str, sizeof(addr_str));

    LOG_DBG("[HID] Incoming L2CAP interrupt (PSM 0x%04x) from %s "
           "state=%s conn=%p our_conn=%p\n",
           HID_PSM_INTERRUPT, addr_str, state_name(hid_ctx.state),
           conn, hid_ctx.conn);

    int err = l2cap_server_reject_blacklisted(conn);
    if (err) return err;

    /* Reject L2CAP from a different ACL than the primary connection */
    if (hid_ctx.conn && conn != hid_ctx.conn) {
        LOG_WRN("[HID] Interrupt: rejecting L2CAP from non-primary conn %p "
               "(primary=%p)\n", conn, hid_ctx.conn);
        return -EACCES;
    }

    if (hid_ctx.intr_connected || hid_ctx.intr_accepting) {
        LOG_WRN("[HID] Interrupt channel already connected/accepting\n");
        return -ENOMEM;
    }
    hid_ctx.intr_accepting = true;

    memset(&hid_ctx.intr_chan, 0, sizeof(hid_ctx.intr_chan));
    hid_ctx.intr_chan.chan.ops = &intr_ops;
    hid_ctx.intr_chan.rx.mtu  = L2CAP_BR_MTU;
    *chan = &hid_ctx.intr_chan.chan;

    l2cap_server_ensure_conn(conn);
    LOG_INF("[HID] Interrupt server: accepted, chan=%p\n", *chan);
    return 0;
}

static struct bt_l2cap_server l2cap_hid_ctrl_server = {
    .psm    = HID_PSM_CONTROL,
    .accept = l2cap_ctrl_server_accept,
};

static struct bt_l2cap_server l2cap_hid_intr_server = {
    .psm    = HID_PSM_INTERRUPT,
    .accept = l2cap_intr_server_accept,
};

/* ---------- SDP Service Record for HID Host ---------- */

static struct bt_sdp_attribute hid_host_attrs[] = {
    BT_SDP_NEW_SERVICE,
    /* Service Class ID List: HID (0x1124) */
    {
        BT_SDP_ATTR_SVCLASS_ID_LIST,
        { BT_SDP_TYPE_SIZE_VAR(BT_SDP_SEQ8, 3),
          BT_SDP_DATA_ELEM_LIST(
              { BT_SDP_TYPE_SIZE(BT_SDP_UUID16), BT_SDP_ARRAY_16(0x1124) },
          ),
        }
    },
    /* Protocol Descriptor List: L2CAP(PSM=0x0011) + HIDP */
    {
        BT_SDP_ATTR_PROTO_DESC_LIST,
        { BT_SDP_TYPE_SIZE_VAR(BT_SDP_SEQ8, 13),
          BT_SDP_DATA_ELEM_LIST(
              { BT_SDP_TYPE_SIZE_VAR(BT_SDP_SEQ8, 6),
                BT_SDP_DATA_ELEM_LIST(
                    { BT_SDP_TYPE_SIZE(BT_SDP_UUID16), BT_SDP_ARRAY_16(0x0100) },
                    { BT_SDP_TYPE_SIZE(BT_SDP_UINT16), BT_SDP_ARRAY_16(HID_PSM_CONTROL) },
                ),
              },
              { BT_SDP_TYPE_SIZE_VAR(BT_SDP_SEQ8, 3),
                BT_SDP_DATA_ELEM_LIST(
                    { BT_SDP_TYPE_SIZE(BT_SDP_UUID16), BT_SDP_ARRAY_16(0x0011) },
                ),
              },
          ),
        }
    },
    /* Additional Protocol Descriptor List: L2CAP(PSM=0x0013) + HIDP */
    {
        BT_SDP_ATTR_ADD_PROTO_DESC_LIST,
        { BT_SDP_TYPE_SIZE_VAR(BT_SDP_SEQ8, 15),
          BT_SDP_DATA_ELEM_LIST(
              { BT_SDP_TYPE_SIZE_VAR(BT_SDP_SEQ8, 13),
                BT_SDP_DATA_ELEM_LIST(
                    { BT_SDP_TYPE_SIZE_VAR(BT_SDP_SEQ8, 6),
                      BT_SDP_DATA_ELEM_LIST(
                          { BT_SDP_TYPE_SIZE(BT_SDP_UUID16), BT_SDP_ARRAY_16(0x0100) },
                          { BT_SDP_TYPE_SIZE(BT_SDP_UINT16), BT_SDP_ARRAY_16(HID_PSM_INTERRUPT) },
                      ),
                    },
                    { BT_SDP_TYPE_SIZE_VAR(BT_SDP_SEQ8, 3),
                      BT_SDP_DATA_ELEM_LIST(
                          { BT_SDP_TYPE_SIZE(BT_SDP_UUID16), BT_SDP_ARRAY_16(0x0011) },
                      ),
                    },
                ),
              },
          ),
        }
    },
    /* Profile Descriptor List: HID v1.11 */
    {
        BT_SDP_ATTR_PROFILE_DESC_LIST,
        { BT_SDP_TYPE_SIZE_VAR(BT_SDP_SEQ8, 8),
          BT_SDP_DATA_ELEM_LIST(
              { BT_SDP_TYPE_SIZE_VAR(BT_SDP_SEQ8, 6),
                BT_SDP_DATA_ELEM_LIST(
                    { BT_SDP_TYPE_SIZE(BT_SDP_UUID16), BT_SDP_ARRAY_16(0x1124) },
                    { BT_SDP_TYPE_SIZE(BT_SDP_UINT16), BT_SDP_ARRAY_16(0x0111) },
                ),
              },
          ),
        }
    },
    BT_SDP_SERVICE_NAME("DS5DongleRE HID"),
};

static struct bt_sdp_record hid_host_rec = {
    .attrs = hid_host_attrs,
    .attr_count = ARRAY_SIZE(hid_host_attrs),
};

/* ---------- bt_enable ready synchronization ---------- */

static volatile bool bt_ready_flag;
static SemaphoreHandle_t bt_ready_sem;
static StaticSemaphore_t bt_ready_sem_buf;

static void bt_ready_cb(int err)
{
    LOG_INF("[BT] bt_ready_cb: err=%d (stack init complete)\n", err);
    bt_ready_flag = (err == 0);
    xSemaphoreGive(bt_ready_sem);
}

/* ---------- public API ---------- */

int bt_hid_host_init(const struct bt_hid_host_config *config)
{
    memset(&hid_ctx, 0, sizeof(hid_ctx));
    feature_cache_clear();
    hid_ctx.input_cb   = config->input_cb;
    hid_ctx.state_cb   = config->state_cb;
    hid_ctx.primer_cb  = config->primer_cb;
    hid_ctx.filter_vid = config->target_vid;
    hid_ctx.filter_pid = config->target_pid;
    hid_ctx.state      = BT_HID_STATE_IDLE;

    net_buf_init(&hid_tx_pool, 20, BT_L2CAP_BUF_SIZE(L2CAP_BR_MTU), NULL);
    net_buf_init(&hid_rx_pool, 4, BT_L2CAP_BUF_SIZE(L2CAP_BR_MTU), NULL);

    bt_conn_cb_register(&conn_callbacks);
    bt_conn_auth_cb_register(&auth_callbacks);

    bt_ready_sem = xSemaphoreCreateBinaryStatic(&bt_ready_sem_buf);

    int err = bt_enable(bt_ready_cb);
    if (err) {
        LOG_ERR("[BT] bt_enable failed: %d\n", err);
        return err;
    }

    LOG_INF("[BT] bt_enable returned, waiting for stack init...\n");
    if (xSemaphoreTake(bt_ready_sem, pdMS_TO_TICKS(10000)) != pdTRUE) {
        LOG_ERR("[BT] ERROR: stack init timeout!\n");
        return -1;
    }
    if (!bt_ready_flag) {
        LOG_ERR("[BT] ERROR: stack init failed\n");
        return -1;
    }
    LOG_INF("[BT] Bluetooth fully initialized\n");

    /* Generate BD_ADDR from chip eFuse ID (boards often ship without
     * a pre-programmed BT MAC, resulting in 00:00:00:00:00:00).
     * Must be called AFTER bt_enable (HCI transport must be up). */
    {
        bt_addr_le_t cur;
        bt_get_local_public_address(&cur);
        bool all_zero = true;
        for (int i = 0; i < 6; i++)
            if (cur.a.val[i]) { all_zero = false; break; }

        if (all_zero) {
            uint8_t chipid[8];
            bflb_efuse_get_chipid(chipid);
            bt_addr_t bd;
            bd.val[0] = chipid[0];
            bd.val[1] = chipid[1];
            bd.val[2] = chipid[2];
            bd.val[3] = chipid[3];
            bd.val[4] = chipid[4];
            bd.val[5] = chipid[5] | 0xC0;
            int r1 = bt_set_bd_addr(&bd);
            int r2 = bt_set_local_public_address(bd.val);
            LOG_INF("[BT] Set BD_ADDR from chipid: %02X:%02X:%02X:%02X:%02X:%02X "
                   "(hci=%d host=%d)\n",
                   bd.val[5], bd.val[4], bd.val[3],
                   bd.val[2], bd.val[1], bd.val[0], r1, r2);
        }

        bt_get_local_public_address(&cur);
        LOG_INF("[BT] BD_ADDR: %02X:%02X:%02X:%02X:%02X:%02X\n",
               cur.a.val[5], cur.a.val[4], cur.a.val[3],
               cur.a.val[2], cur.a.val[1], cur.a.val[0]);
    }

    blacklist_load();
    bonded_list_load();

    /* Register L2CAP servers so the controller can initiate reconnection
     * (matching DS5DongleRE's l2cap_register_service for both PSMs). */
    err = bt_l2cap_br_server_register(&l2cap_hid_ctrl_server);
    if (err)
        LOG_ERR("[BT] Warning: register control server failed: %d\n", err);

    err = bt_l2cap_br_server_register(&l2cap_hid_intr_server);
    if (err)
        LOG_ERR("[BT] Warning: register interrupt server failed: %d\n", err);

    err = bt_sdp_register_service(&hid_host_rec);
    if (err)
        LOG_ERR("[BT] Warning: register SDP HID record failed: %d\n", err);
    else
        LOG_INF("[BT] SDP HID service record registered\n");

    /* Match Pico's aggressive page scan: interval=window=0x0012 (11.25ms)
     * = 100% duty cycle. Ensures near-instant response to controller
     * reconnection attempts, critical for low-battery reconnect success.
     * HCI Write_Page_Scan_Activity: OGF 0x03, OCF 0x001C. */
    {
        struct net_buf *ps_buf = bt_hci_cmd_create(
            BT_OP(BT_OGF_BASEBAND, 0x001c), 4);
        if (ps_buf) {
            net_buf_add_le16(ps_buf, 0x0012);  /* interval: 11.25ms */
            net_buf_add_le16(ps_buf, 0x0012);  /* window:   11.25ms */
            int ps_err = bt_hci_cmd_send_sync(
                BT_OP(BT_OGF_BASEBAND, 0x001c), ps_buf, NULL);
            LOG_INF("[BT] Write Page Scan Activity: interval=0x0012 "
                   "window=0x0012 err=%d\n", ps_err);
        }
    }

    /* Make device discoverable so the controller can find us if needed */
    err = bt_br_set_connectable(true);
    LOG_INF("[BT] set_connectable: %d\n", err);

    err = bt_br_set_discoverable(true);
    LOG_INF("[BT] set_discoverable: %d\n", err);

    /* Diagnostic: check if BR/EDR write_scan_enable succeeded.
     * If both return 0, the controller accepted BR/EDR commands,
     * confirming that BR/EDR HCI path is functional. */
    LOG_INF("[BT] BR/EDR init complete — phone should see \"%s\"\n",
           CONFIG_BT_DEVICE_NAME);

    return 0;
}

int bt_hid_host_scan_start(uint8_t duration_sec)
{
    if (hid_ctx.state != BT_HID_STATE_IDLE || hid_ctx.conn)
        return -1;

    /* Don't scan if an outgoing connect retry is pending —
     * HCI can't do inquiry and create_connection simultaneously */
    if (hid_ctx.connect_pending) {
        LOG_INF("[BT] scan_start suppressed: outgoing connect pending\n");
        return -2;
    }

    set_state(BT_HID_STATE_SCANNING);

    memset(discovery_results, 0, sizeof(discovery_results));

    struct bt_br_discovery_param param = {
        .length     = duration_sec ? duration_sec : BT_INQUIRY_LEN,
        .limited    = false,
    };

    int err = bt_br_discovery_start(&param, discovery_results,
                                     BT_MAX_DISCOVERED,
                                     discovery_complete);
    if (err) {
        LOG_ERR("[BT] Discovery start failed: %d\n", err);
        set_state(BT_HID_STATE_IDLE);
        return err;
    }

    LOG_INF("[BT] Scanning for devices...\n");
    return 0;
}

void bt_hid_host_scan_stop(void)
{
    bt_br_discovery_stop();
    if (hid_ctx.state == BT_HID_STATE_SCANNING)
        set_state(BT_HID_STATE_IDLE);
}

bool bt_hid_host_poll_scan_early(void)
{
    if (hid_ctx.state != BT_HID_STATE_SCANNING)
        return false;

    for (int i = 0; i < BT_MAX_DISCOVERED; i++) {
        static const bt_addr_t zero = {{0}};
        if (bt_addr_cmp(&discovery_results[i].addr, &zero) == 0)
            break;

        uint32_t cod = discovery_results[i].cod[0]
                     | ((uint32_t)discovery_results[i].cod[1] << 8)
                     | ((uint32_t)discovery_results[i].cod[2] << 16);
        uint8_t major = (cod >> 8) & 0x1F;
        uint8_t minor = (cod >> 2) & 0x3F;
        bool is_gamepad = (major == 0x05) && (minor & 0x02);

        if (is_gamepad || (hid_ctx.has_target &&
            bt_addr_cmp(&discovery_results[i].addr, &hid_ctx.target_addr) == 0)) {

            if (hid_ctx.has_target &&
                bt_addr_cmp(&discovery_results[i].addr, &hid_ctx.target_addr) != 0)
                continue;

            LOG_INF("[BT] Early termination: gamepad found during inquiry\n");
            bt_br_discovery_stop();
            bt_addr_copy(&hid_ctx.pending_addr, &discovery_results[i].addr);
            hid_ctx.connect_pending = true;
            hid_ctx.connect_pending_tick = 0;
            set_state(BT_HID_STATE_CONNECTING);
            return true;
        }
    }
    return false;
}

bool bt_hid_host_poll_connect(void)
{
    if (!hid_ctx.connect_pending)
        return false;

    /* If controller already reconnected (incoming), cancel outgoing */
    if (hid_ctx.conn != NULL && hid_ctx.state != BT_HID_STATE_IDLE) {
        LOG_INF("[BT] poll_connect: cancelled (controller already connected, "
               "state=%d)\n", hid_ctx.state);
        hid_ctx.connect_pending = false;
        hid_ctx.connect_pending_tick = 0;
        return false;
    }

    /* connect_pending_tick == UINT32_MAX means "waiting for ACL teardown" */
    if (hid_ctx.connect_pending_tick == (TickType_t)UINT32_MAX)
        return false;

    /* After ACL teardown event fires, wait before connecting */
    if (hid_ctx.connect_pending_tick != 0) {
        TickType_t elapsed = xTaskGetTickCount() - hid_ctx.connect_pending_tick;
        if (elapsed < pdMS_TO_TICKS(500))
            return false;
    }

    /* Cancel if another connect path is active (not from our deferred flow).
     * States IDLE, SCANNING, and CONNECTING are valid — early termination and
     * discovery_complete set CONNECTING before deferring to poll_connect. */
    if (hid_ctx.state != BT_HID_STATE_IDLE &&
        hid_ctx.state != BT_HID_STATE_SCANNING &&
        hid_ctx.state != BT_HID_STATE_CONNECTING) {
        LOG_INF("[BT] poll_connect: cancelled (state=%d, not idle)\n",
               hid_ctx.state);
        hid_ctx.connect_pending = false;
        hid_ctx.connect_pending_tick = 0;
        return false;
    }

    /* If scanning is in progress, cancel it first (HCI can't do both) */
    if (hid_ctx.state == BT_HID_STATE_SCANNING) {
        LOG_INF("[BT] poll_connect: cancelling active scan first\n");
        bt_br_discovery_stop();
        set_state(BT_HID_STATE_IDLE);
    }

    LOG_INF("[BT] Fallback: attempting outgoing connect (state=%d conn=%p)\n",
           hid_ctx.state, hid_ctx.conn);
    int rc = bt_hid_host_connect(hid_ctx.pending_addr.val);
    if (rc != 0) {
        static uint8_t retry_count = 0;
        retry_count++;
        if (retry_count >= 3) {
            LOG_WRN("[BT] Fallback: gave up after %d attempts\n", retry_count);
            retry_count = 0;
            hid_ctx.connect_pending = false;
            hid_ctx.connect_pending_tick = 0;
        } else {
            LOG_WRN("[BT] Fallback: attempt %d failed (%d), retry in 500ms\n",
                   retry_count, rc);
            hid_ctx.connect_pending_tick = xTaskGetTickCount();
        }
    } else {
        hid_ctx.connect_pending = false;
        hid_ctx.connect_pending_tick = 0;
    }
    return true;
}

bool bt_hid_host_poll_security(void)
{
    if (!hid_ctx.security_pending || !hid_ctx.conn)
        return false;

    hid_ctx.security_pending = false;

    struct bt_conn *conn = hid_ctx.conn;

    LOG_DBG("[BT] poll_security: conn=%p handle=0x%04x state=%s "
           "sec_level=%d required_sec=%d encrypt=%d outgoing=%d\n",
           conn, conn->handle, state_name(hid_ctx.state),
           conn->sec_level, conn->required_sec_level, conn->encrypt,
           hid_ctx.outgoing);

    /* Root cause fix: conn_new() doesn't clear bt_conn, so
     * required_sec_level retains the stale value from a previous
     * connection.  Reset it so bt_conn_set_security() actually
     * initiates authentication. */
    if (conn->required_sec_level >= BT_SECURITY_L2) {
        LOG_DBG("[BT] poll_security: resetting stale required_sec_level "
               "%d -> L1\n", conn->required_sec_level);
        conn->required_sec_level = BT_SECURITY_L1;
    }

    int err = bt_conn_set_security(conn, BT_SECURITY_L2);
    LOG_DBG("[BT] poll_security: bt_conn_set_security(L2) = %d "
           "(now sec=%d req=%d)\n",
           err, conn->sec_level, conn->required_sec_level);

    if (err == -EBUSY) {
        LOG_DBG("[BT] poll_security: pairing already in progress, waiting\n");
    } else if (err && err != -EALREADY) {
        LOG_DBG("[BT] poll_security: set_security failed (%d), "
               "trying HCI Auth_Requested directly\n", err);
        struct net_buf *buf = bt_hci_cmd_create(
            BT_HCI_OP_AUTH_REQUESTED, 2);
        if (buf) {
            net_buf_add_le16(buf, conn->handle);
            int hci_err = bt_hci_cmd_send_sync(
                BT_HCI_OP_AUTH_REQUESTED, buf, NULL);
            LOG_DBG("[BT] poll_security: HCI Auth_Requested = %d\n", hci_err);
        }
    }

    return true;
}

bool bt_hid_host_poll_sdp(void)
{
    if (!hid_ctx.sdp_pending || !hid_ctx.conn)
        return false;

    hid_ctx.sdp_pending = false;

    if (hid_ctx.state != BT_HID_STATE_CONNECTING) {
        LOG_DBG("[BT] poll_sdp: state already advanced (%s), skipping\n",
               state_name(hid_ctx.state));
        return false;
    }

    /* Skip SDP for bonded devices — directly create L2CAP channels.
     * This matches DS5DongleRE behavior where reconnection never does SDP. */
    LOG_DBG("[BT] poll_sdp: creating outgoing L2CAP channels\n");

    set_state(BT_HID_STATE_L2CAP_CONTROL);

    memset(&hid_ctx.ctrl_chan, 0, sizeof(hid_ctx.ctrl_chan));
    memset(&hid_ctx.intr_chan, 0, sizeof(hid_ctx.intr_chan));

    hid_ctx.ctrl_chan.chan.ops = &ctrl_ops;
    hid_ctx.ctrl_chan.rx.mtu  = L2CAP_BR_MTU;
    hid_ctx.intr_chan.chan.ops = &intr_ops;
    hid_ctx.intr_chan.rx.mtu  = L2CAP_BR_MTU;

    int err = bt_l2cap_chan_connect(hid_ctx.conn,
                                    &hid_ctx.ctrl_chan.chan,
                                    HID_PSM_CONTROL);
    if (err) {
        LOG_DBG("[BT] poll_sdp: L2CAP control connect failed: %d\n", err);
        bt_conn_disconnect(hid_ctx.conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    }
    return true;
}

#define SECURITY_TIMEOUT_TICKS  pdMS_TO_TICKS(8000)

bool bt_hid_host_poll_security_watchdog(void)
{
    if (hid_ctx.security_done || !hid_ctx.conn)
        return false;

    if (hid_ctx.state != BT_HID_STATE_CONNECTING)
        return false;

    TickType_t elapsed = xTaskGetTickCount() - hid_ctx.acl_connect_tick;
    if (elapsed < SECURITY_TIMEOUT_TICKS)
        return false;

    struct bt_conn *conn = hid_ctx.conn;
    LOG_WRN("[BT] WATCHDOG: security not established after %lu ms! "
           "sec_level=%d required_sec=%d encrypt=%d\n",
           (unsigned long)(elapsed * portTICK_PERIOD_MS),
           conn->sec_level, conn->required_sec_level, conn->encrypt);

    if (conn->encrypt && conn->sec_level >= BT_SECURITY_L2) {
        LOG_WRN("[BT] WATCHDOG: encryption active, forcing L2CAP path\n");
        diag_log(DIAG_EV_WD_SEC_OK,
                 (uint32_t)conn->sec_level | ((uint32_t)conn->encrypt << 8), 0);
        hid_ctx.security_done = true;
        if (hid_ctx.outgoing) {
            hid_ctx.sdp_pending = true;
        }
        return true;
    }

    diag_log(DIAG_EV_WD_SEC_FAIL,
             (uint32_t)conn->sec_level | ((uint32_t)conn->encrypt << 8), 0);

    /* Auth failed: required_sec_level was reset by reset_pairing() in the
     * SDK, meaning the controller rejected our link key.  Delete the stale
     * key so the next connection attempt triggers fresh SSP pairing. */
    const bt_addr_t *remote = conn_get_br_addr(conn);
    if (remote) {
        LOG_WRN("[BT] WATCHDOG: clearing stale link key for "
               "%02X:%02X:%02X:%02X:%02X:%02X\n",
               remote->val[5], remote->val[4], remote->val[3],
               remote->val[2], remote->val[1], remote->val[0]);
        extern void bt_keys_link_key_clear_addr(const bt_addr_t *addr);
        bt_keys_link_key_clear_addr(remote);
    }

    LOG_WRN("[BT] WATCHDOG: disconnecting to retry with fresh pairing\n");
    bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    return false;
}

#define INCOMING_L2CAP_FALLBACK_MS 3000

bool bt_hid_host_poll_incoming_l2cap_fallback(void)
{
    if (hid_ctx.state != BT_HID_STATE_CONNECTING)
        return false;
    if (!hid_ctx.security_done || hid_ctx.outgoing || !hid_ctx.conn)
        return false;

    TickType_t elapsed = xTaskGetTickCount() - hid_ctx.security_done_tick;
    if (elapsed < pdMS_TO_TICKS(INCOMING_L2CAP_FALLBACK_MS))
        return false;

    /* Control L2CAP handshake stuck (accept fired but connected never came) */
    if (hid_ctx.ctrl_accepting && !hid_ctx.ctrl_connected) {
        diag_log(DIAG_EV_L2CAP_FALLBACK, 1, 0);   /* 1=ctrl 握手卡住 */
        LOG_INF("[BT] L2CAP fallback: Control handshake stuck, disconnecting "
               "(ctrl_accept=%d ctrl_conn=%d intr_conn=%d)\n",
               hid_ctx.ctrl_accepting, hid_ctx.ctrl_connected, hid_ctx.intr_connected);
        /* Explicitly disconnect the stuck L2CAP channel to cancel RTX timer
         * before tearing down the ACL — speeds up ACL teardown significantly */
        bt_l2cap_chan_disconnect(&hid_ctx.ctrl_chan.chan);
        hid_ctx.fallback_retry = true;
        LOG_INF("[BT] fallback_retry SET=1 (stuck path)\n");
        bt_hid_host_disconnect();
        return true;
    }

    if (hid_ctx.ctrl_connected) {
        /* Control is up but Interrupt isn't — open Interrupt only */
        if (!hid_ctx.intr_connected && !hid_ctx.intr_accepting) {
            LOG_INF("[BT] L2CAP fallback: Control connected, opening Interrupt\n");
            hid_ctx.outgoing = true;
            set_state(BT_HID_STATE_L2CAP_INTERRUPT);
            memset(&hid_ctx.intr_chan, 0, sizeof(hid_ctx.intr_chan));
            hid_ctx.intr_chan.chan.ops = &intr_ops;
            hid_ctx.intr_chan.rx.mtu  = L2CAP_BR_MTU;
            int err = bt_l2cap_chan_connect(hid_ctx.conn,
                                            &hid_ctx.intr_chan.chan,
                                            HID_PSM_INTERRUPT);
            if (err)
                LOG_ERR("[BT] Interrupt connect failed: %d\n", err);
            return true;
        }
        return false;
    }

    /* Neither Control connecting nor connected — controller didn't open L2CAP.
     * Controller's SDP query may have closed before receiving our response.
     * Set fallback_retry to actively page controller and open L2CAP from host side
     * (standard HID Host behavior, same as Pico2W DS5DongleRE). */
    LOG_WRN("[BT] L2CAP fallback: no activity in %dms, retry outgoing\n",
            (int)(elapsed * portTICK_PERIOD_MS));
    diag_log(DIAG_EV_L2CAP_FALLBACK, 2, 0);   /* 2=安全完成后控制器没开 L2CAP */
    hid_ctx.fallback_retry = true;
    bt_hid_host_disconnect();
    return true;
}

int bt_hid_host_connect(const uint8_t *addr)
{
    bt_addr_t bt_addr;
    memcpy(bt_addr.val, addr, 6);

    LOG_DBG("[BT] connect ENTER addr=%02X:%02X:%02X:%02X:%02X:%02X state=%d conn=%p\n",
           addr[5], addr[4], addr[3], addr[2], addr[1], addr[0],
           hid_ctx.state, hid_ctx.conn);

    if (hid_ctx.state == BT_HID_STATE_SCANNING) {
        LOG_DBG("[BT] connect [1] stopping scan...\n");
        int stop_err = bt_br_discovery_stop();
        LOG_DBG("[BT] connect [2] scan stop rc=%d\n", stop_err);
    }

    if (hid_ctx.conn) {
        LOG_DBG("[BT] connect: already have conn=%p, skip create\n", hid_ctx.conn);
        hid_ctx.outgoing = true;
        set_state(BT_HID_STATE_CONNECTING);
        return 0;
    }

    hid_ctx.outgoing = true;
    set_state(BT_HID_STATE_CONNECTING);
    LOG_DBG("[BT] connect [3] state=CONNECTING, outgoing=true\n");

    /* Clear link key only for devices NOT in our bonded list (fresh discovery).
     * For bonded devices (fallback retry / reconnection), keep the key. */
    {
        bool is_bonded = false;
        for (uint8_t i = 0; i < bonded_count; i++) {
            if (bt_addr_cmp(&bonded_list[i], &bt_addr) == 0) {
                is_bonded = true;
                break;
            }
        }
        if (!is_bonded) {
            extern void bt_keys_link_key_clear_addr(const bt_addr_t *addr);
            bt_keys_link_key_clear_addr(&bt_addr);
            LOG_INF("[BT] Cleared stale link key for outgoing connection "
                   "(fresh SSP)\n");
        }
    }

    LOG_DBG("[BT] connect [4] bt_conn_create_br...\n");
    /* Disable page scan before paging — BL616 HCI controller can't do both */
    bt_br_set_connectable(false);
    bt_br_set_discoverable(false);
    struct bt_conn *conn = bt_conn_create_br(&bt_addr,
                                              BT_BR_CONN_PARAM_DEFAULT);
    LOG_DBG("[BT] connect [5] returned %p\n", conn);
    if (!conn) {
        LOG_WRN("[BT] bt_conn_create_br failed — re-enabling page scan\n");
        hid_ctx.outgoing = false;
        bt_br_set_connectable(true);
        bt_br_set_discoverable(true);
        set_state(BT_HID_STATE_IDLE);
        return -1;
    }

    /* Bouffalo SDK's bt_conn_create_br does NOT return a caller
     * reference (unlike standard Zephyr). SDK examples never call
     * bt_conn_unref after bt_conn_create_br. */
    LOG_DBG("[BT] connect EXIT OK\n");
    return 0;
}

void bt_hid_host_disconnect(void)
{
    LOG_INF("[BT] bt_hid_host_disconnect called (state=%d fallback_retry=%d)\n",
           hid_ctx.state, hid_ctx.fallback_retry);
    struct bt_conn *c = hid_ctx.conn;   /* R1：只读一次，别在检查和使用之间被别的线程置空 */
    if (c) {
        set_state(BT_HID_STATE_DISCONNECTING);
        bt_conn_disconnect(c, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    }
}

void bt_hid_host_disconnect_site(uint8_t site)
{
    /* S32：先留痕再拆链。diag_log 只写 RAM，任务/ISR 上下文都能调。 */
    diag_log(DIAG_EV_DISC_REQ, site, (uint32_t)hid_ctx.state);
    LOG_WRN("[BT] Disconnect requested by site %u (state=%d)\n",
            (unsigned)site, (int)hid_ctx.state);
    bt_hid_host_disconnect();
}

static volatile bool s_force_busy;   /* R1：force_disconnect 正在执行（可能卡在 HCI 同步命令上） */

void bt_hid_host_force_disconnect(void)
{
    bool busy;
    taskENTER_CRITICAL();
    busy = s_force_busy;
    s_force_busy = true;
    taskEXIT_CRITICAL();
    if (busy) {
        /* 另一个任务正在拆（10:53 就是 usb 任务看门狗和 bt 任务兜底同时进来）：别重复拆 */
        diag_log(DIAG_EV_FORCE_DISC, (uint32_t)hid_ctx.state | 0x100u, 0);
        return;
    }

    LOG_WRN("[BT] Force disconnect — HID cleanup, conn=%p\n", hid_ctx.conn);
    diag_log(DIAG_EV_FORCE_DISC, hid_ctx.state, 0);

    hid_ctx.ctrl_connected = false;
    hid_ctx.intr_connected = false;
    hid_ctx.ctrl_accepting = false;
    hid_ctx.intr_accepting = false;
    hid_ctx.outgoing = false;
    hid_ctx.is_dse = false;
    hid_ctx.check_dse = false;
    hid_ctx.handshake_next  = 0;
    hid_ctx.handshake_count = 0;
    hid_ctx.handshake_send_failed = false;
    hid_ctx.connect_pending = false;
    hid_ctx.sdp_pending = false;
    hid_ctx.security_pending = false;
    hid_ctx.security_done = false;

    first_intr_logged  = false;
    first_output_logged = false;

    app_tx_fifo_reset();

    feature_cache_clear();
    dse_reset();

    {
        struct bt_conn *c = conn_take(NULL);   /* R1：先摘指针，再做会阻塞的 HCI 调用 */
        if (c) {
            LOG_INF("[BT] Sending HCI Disconnect for ACL cleanup\n");
            bt_conn_disconnect(c, 0x13);
            bt_conn_unref(c);
        }
    }

    bt_br_set_connectable(true);
    bt_br_set_discoverable(true);

    set_state(BT_HID_STATE_IDLE);

    LOG_INF("[BT] force_disconnect: fallback_retry=%d bonded_count=%d\n",
           hid_ctx.fallback_retry, bonded_count);
    if (hid_ctx.fallback_retry && bonded_count > 0) {
        hid_ctx.fallback_retry = false;
        memcpy(hid_ctx.pending_addr.val, bonded_list[active_idx].val, 6);
        hid_ctx.connect_pending = true;
        hid_ctx.connect_pending_tick = (TickType_t)UINT32_MAX;
        LOG_INF("[BT] Fallback: outgoing connect pending (waiting for ACL teardown)\n");
    }
    s_force_busy = false;
}

bool bt_hid_host_has_pending_conn(void)
{
    return hid_ctx.conn != NULL;
}

void bt_hid_host_drop_stale_conn(void)
{
    struct bt_conn *c = conn_take(NULL);   /* R1：原子摘取 */
    if (!c)
        return;

    LOG_WRN("[BT] conn_disconnected never arrived — force cleanup\n");

    bt_conn_unref(c);

    hid_ctx.ctrl_connected = false;
    hid_ctx.intr_connected = false;
    hid_ctx.ctrl_accepting = false;
    hid_ctx.intr_accepting = false;
    hid_ctx.outgoing = false;
    hid_ctx.is_dse = false;
    hid_ctx.check_dse = false;
    hid_ctx.connect_pending = false;
    hid_ctx.sdp_pending = false;
    hid_ctx.security_pending = false;
    hid_ctx.security_done = false;

    app_tx_fifo_reset();

    feature_cache_clear();

    set_state(BT_HID_STATE_IDLE);

    if (switch_pending) {
        switch_pending = false;
        LOG_INF("[BT] Switch: stale conn cleared, waiting for "
               "controller [%d/%d] to reconnect\n", active_idx, bonded_count);
    }

    bt_br_set_connectable(true);
    bt_br_set_discoverable(true);
    LOG_INF("[BT] Re-enabled connectable+discoverable after stale cleanup\n");
}

int bt_hid_host_read_rssi(int8_t *rssi)
{
    struct bt_conn *c = hid_ctx.conn;   /* R1.3：只读一次——检查完再读 hid_ctx.conn 会被 BT 线程置空 */
    if (!c || hid_ctx.state != BT_HID_STATE_CONNECTED)
        return -1;

    /* 手柄走的是**经典蓝牙（BR/EDR）**，必须用通用的 HCI Read_RSSI 命令
     * （OGF 0x05 / OCF 0x0005）—— 它对 BR/EDR 的 ACL 句柄同样有效。
     *
     * 原来这里调的是 bt_le_read_rssi()，那是 **BLE 专用** 的接口，
     * 对 BR 句柄无意义：命令发不出去或立刻失败，cached_rssi 一直停在 0，
     * 于是网页"信号强度"永远显示"未连接手柄"（而电量却正常，看起来自相矛盾）。 */
    uint16_t handle = c->handle;
    struct net_buf *buf = bt_hci_cmd_create(BT_HCI_OP_READ_RSSI, 2);
    if (!buf)
        return -1;
    net_buf_add_le16(buf, handle);

    struct net_buf *rsp = NULL;
    int err = bt_hci_cmd_send_sync(BT_HCI_OP_READ_RSSI, buf, &rsp);
    if (err || !rsp) {
        LOG_WRN("[BT] Read RSSI failed: err=%d rsp=%p\n", err, (void *)rsp);
        if (rsp)
            net_buf_unref(rsp);
        return -1;
    }

    struct bt_hci_rp_read_rssi *rp = (void *)rsp->data;
    uint8_t status = rp->status;
    int8_t val = rp->rssi;
    net_buf_unref(rsp);

    if (status == 0) {
        cached_rssi = val;
        if (rssi) *rssi = val;
    } else {
        LOG_WRN("[BT] Read RSSI status=0x%02x\n", status);
    }
    return status;
}

/* R1.3：链路诊断。2026-10-14 下午实测掉线前手柄送达的帧率从 ~700 掉到 150~250 再到 0，
 * RSSI 读数一直正常 —— 要看清是不是链路本身在变差（干扰 → 重传 → AFH 砍信道）。
 * 每 ~2 s 由 bt 任务调一次：Read_Link_Quality (0x05/0x0003)、Read_AFH_Channel_Map (0x05/0x0006)。 */
static volatile uint8_t s_link_quality, s_afh_used, s_afh_mode;
#define LINK_LSTO_SLOTS 16000u   /* R1.4：10 s / 0.625 ms（= 无输入看门狗） */
static void link_request_master(struct bt_conn *c, uint16_t handle);
static uint8_t link_write_lsto(uint16_t handle);
static volatile uint8_t  s_role = 0xFF;   /* R1.4：0 主（Central）/ 1 从 / 0xFF 没读到 */
static volatile uint16_t s_lsto;          /* R1.4：链路监督超时，slot（0.625 ms） */
/* R1.5：「接收器当主」开关（0xF6 命令 0x11 运行时改，不落盘）和本次连接的进度。
 * R1.6 起默认开：R1.5 实测手柄接受切换、不断线，当主后监督超时由我们定（10 s），帧率反而更高。
 * 连上满 ROLE_REQ_DELAY_MS 才申请——R1.5 是在连接稳定以后切的，别挤在连接建立那一下。 */
#define ROLE_REQ_DELAY_MS 3000u
static volatile uint8_t  s_role_req = 1;
static bool              s_rs_tried, s_lsto_done;
static uint32_t          s_conn_ms;      /* R1.6：这次连接做 link_params 的时刻 */
static uint8_t           s_afh_logged_n = 0xFF, s_afh_logged_lo, s_afh_logged_hi;
static uint32_t          s_afh_logged_ms;

static int hci_status_read(uint16_t opcode, uint16_t handle, uint8_t *out, uint16_t out_len)
{
    struct net_buf *buf = bt_hci_cmd_create(opcode, 2);
    if (!buf)
        return -1;
    net_buf_add_le16(buf, handle);
    struct net_buf *rsp = NULL;
    int err = bt_hci_cmd_send_sync(opcode, buf, &rsp);
    if (err || !rsp) {
        if (rsp)
            net_buf_unref(rsp);
        return -1;
    }
    uint16_t n = rsp->len < out_len ? rsp->len : out_len;
    memcpy(out, rsp->data, n);
    net_buf_unref(rsp);
    return n;
}

void bt_hid_host_read_link_stats(void)
{
    struct bt_conn *c = hid_ctx.conn;
    if (!c || hid_ctx.state != BT_HID_STATE_CONNECTED)
        return;
    uint16_t handle = c->handle;
    bt_hid_host_read_rssi(NULL);
    uint8_t r[16];
    /* status, handle(2), link_quality */
    if (hci_status_read(BT_OP(BT_OGF_STATUS, 0x0003), handle, r, sizeof(r)) >= 4 && r[0] == 0)
        s_link_quality = r[3];
    /* status, handle(2), AFH_mode, AFH_channel_map[10]（79 位） */
    if (hci_status_read(BT_OP(BT_OGF_STATUS, 0x0006), handle, r, sizeof(r)) >= 14 && r[0] == 0) {
        s_afh_mode = r[3];
        uint8_t n = 0, lo = 0xFF, hi = 0xFF;
        for (int ch = 0; ch < 79; ch++) {
            if (r[4 + ch / 8] & (1u << (ch % 8))) {
                n++;
            } else {                       /* R1.5：被排除的信道（2402+ch MHz） */
                if (lo == 0xFF)
                    lo = (uint8_t)ch;
                hi = (uint8_t)ch;
            }
        }
        s_afh_used = n;
        perf_afh_range(lo, hi);
        /* R1.5：信道表变了记一条（最多 5 s 一条，别把 25 格的环冲掉；记录器在跑时每条都会落盘） */
        uint32_t now_ms = (uint32_t)(bflb_mtimer_get_time_us() / 1000ULL);
        if ((n != s_afh_logged_n || lo != s_afh_logged_lo || hi != s_afh_logged_hi) &&
            (s_afh_logged_n == 0xFF || (uint32_t)(now_ms - s_afh_logged_ms) >= 5000u)) {
            s_afh_logged_n = n;
            s_afh_logged_lo = lo;
            s_afh_logged_hi = hi;
            s_afh_logged_ms = now_ms;
            diag_log(DIAG_EV_AFH, (uint32_t)n | ((uint32_t)lo << 8) | ((uint32_t)hi << 16) |
                                  ((uint32_t)s_afh_mode << 24), 0);
        }
    }
    /* R1.4：status, handle(2), role */
    if (hci_status_read(BT_OP(BT_OGF_LINK_POLICY, 0x0009), handle, r, sizeof(r)) >= 4 && r[0] == 0)
        s_role = r[3];
    /* R1.4：status, handle(2), link_supervision_timeout(2) */
    if (hci_status_read(BT_OP(BT_OGF_BASEBAND, 0x0036), handle, r, sizeof(r)) >= 5 && r[0] == 0)
        s_lsto = (uint16_t)(r[3] | (r[4] << 8));
    perf_link(bt_hid_host_get_cached_rssi(), s_link_quality, s_afh_used, s_afh_mode);
    perf_link_param(s_role, s_lsto);

    /* R1.5：「接收器当主」。开着、我们是从、这次连接还没试过、连上满 3 s（R1.6）→ 申请当主；
     * 当上主（不管是切换来的还是连上时就是）→ 监督超时写成 10 s，再记一条 LINK_PARAM。 */
    if (s_role_req && s_role == 0x01 && !s_rs_tried &&
        (uint32_t)((uint32_t)(bflb_mtimer_get_time_us() / 1000ULL) - s_conn_ms) >= ROLE_REQ_DELAY_MS) {
        s_rs_tried = true;
        link_request_master(c, handle);
    }
    if (s_role == 0x00 && s_lsto != LINK_LSTO_SLOTS && !s_lsto_done) {
        s_lsto_done = true;
        uint8_t wr = link_write_lsto(handle);
        diag_log(DIAG_EV_LINK_PARAM, (uint32_t)s_lsto | ((uint32_t)s_role << 16) | ((uint32_t)wr << 24), 0);
    }
}

/* 写监督超时 10 s；返回 0 = 成功 / 其它 = 失败的 errno 绝对值 / 0xFF = 命令没建出来 */
static uint8_t link_write_lsto(uint16_t handle)
{
    struct net_buf *buf = bt_hci_cmd_create(BT_OP(BT_OGF_BASEBAND, 0x0037), 4);
    if (!buf)
        return 0xFF;
    net_buf_add_le16(buf, handle);
    net_buf_add_le16(buf, LINK_LSTO_SLOTS);
    struct net_buf *rsp = NULL;
    int err = bt_hci_cmd_send_sync(BT_OP(BT_OGF_BASEBAND, 0x0037), buf, &rsp);
    if (rsp)
        net_buf_unref(rsp);
    return err ? (uint8_t)(err < 0 ? -err : err) : 0;
}

/* R1.5：申请当主（PS5 连手柄就是它当主，手柄在 PS5 上本来就是从）。先让本端链路策略允许
 * 主从切换（bit0），再发 HCI_Switch_Role(对端地址, 0x00 = 当主)。命令只回 Command Status，
 * 真正结果是之后的 Role Change 事件（HCI 事件旁路记 DIAG 39）。
 * DIAG_EV_ROLE_SW a1 = 原链路策略（0xFFFF = 没读到）| 写策略结果<<16 | 切换命令结果<<24
 *（结果：0 成功 / 0xFE 不需要写 / 0xFF 命令没建出来 / 其它 = 失败的 errno 绝对值）。 */
static void link_request_master(struct bt_conn *c, uint16_t handle)
{
    uint8_t  r[8];
    uint16_t pol = 0xFFFF;
    uint8_t  e_pol = 0xFE, e_sw = 0xFF;

    if (hci_status_read(BT_OP(BT_OGF_LINK_POLICY, 0x000C), handle, r, sizeof(r)) >= 5 && r[0] == 0)
        pol = (uint16_t)(r[3] | (r[4] << 8));
    if (pol != 0xFFFF && !(pol & 0x0001)) {
        struct net_buf *buf = bt_hci_cmd_create(BT_OP(BT_OGF_LINK_POLICY, 0x000D), 4);
        e_pol = 0xFF;
        if (buf) {
            net_buf_add_le16(buf, handle);
            net_buf_add_le16(buf, (uint16_t)(pol | 0x0001));
            struct net_buf *rsp = NULL;
            int err = bt_hci_cmd_send_sync(BT_OP(BT_OGF_LINK_POLICY, 0x000D), buf, &rsp);
            if (rsp)
                net_buf_unref(rsp);
            e_pol = err ? (uint8_t)(err < 0 ? -err : err) : 0;
        }
    }
    struct net_buf *buf = bt_hci_cmd_create(BT_OP(BT_OGF_LINK_POLICY, 0x000B), 7);
    if (buf) {
        net_buf_add_mem(buf, &c->br.dst, 6);
        net_buf_add_u8(buf, 0x00);
        int err = bt_hci_cmd_send_sync(BT_OP(BT_OGF_LINK_POLICY, 0x000B), buf, NULL);
        e_sw = err ? (uint8_t)(err < 0 ? -err : err) : 0;
    }
    diag_log(DIAG_EV_ROLE_SW, (uint32_t)pol | ((uint32_t)e_pol << 16) | ((uint32_t)e_sw << 24), 0);
    LOG_INF("[BT] request master: policy=0x%04x pol_wr=%u switch=%u\n",
            (unsigned)pol, (unsigned)e_pol, (unsigned)e_sw);
}

void bt_hid_host_set_role_req(uint8_t on)
{
    s_role_req = on ? 1 : 0;
}

/* R1.5：HCI 事件旁路（SDK hci_core.c 的 hci_event() 里调；SDK 里是弱符号空函数）。
 * 只在连着的时候记（连接建立那一串加密 / 远端特性不记，免得 25 格的 DIAG 环被冲掉），
 * 每 10 s 最多 6 条；PERF 里另有总数。a1 = 事件码 | 按事件挑出的 3 个字节 <<8/<<16/<<24。
 * 跑在蓝牙协议栈的接收线程里：只做 diag_log（只写 RAM）。 */
void bt_hci_evt_tap(uint8_t evt, const uint8_t *d, uint16_t len)
{
    static uint32_t win_ms;
    static uint8_t  win_n;

    if (hid_ctx.state != BT_HID_STATE_CONNECTED || evt == 0x05)   /* 断开完成：ACL_DOWN 已经记了 */
        return;
    perf_hci_evt();
    uint32_t now_ms = (uint32_t)(bflb_mtimer_get_time_us() / 1000ULL);
    if ((uint32_t)(now_ms - win_ms) >= 10000u) {
        win_ms = now_ms;
        win_n = 0;
    }
    if (win_n >= 6)
        return;
    win_n++;

    uint8_t b0 = len > 0 ? d[0] : 0, b1 = len > 1 ? d[1] : 0, b2 = len > 2 ? d[2] : 0;
    switch (evt) {
    case 0x12:   /* Role Change：status, bdaddr(6), role */
        b1 = len > 7 ? d[7] : 0xFF;
        b2 = 0;
        break;
    case 0x14:   /* Mode Change：status, handle(2), mode, interval(2) */
        b1 = len > 3 ? d[3] : 0xFF;
        b2 = len > 5 ? (d[5] ? 0xFF : d[4]) : 0;
        break;
    case 0x1B:   /* Max Slots Change：handle(2), lmp_max_slots */
        b0 = len > 2 ? d[2] : 0;
        b1 = b2 = 0;
        break;
    case 0x1D:   /* Packet Type Changed：status, handle(2), packet_type(2) */
        b1 = len > 3 ? d[3] : 0;
        b2 = len > 4 ? d[4] : 0;
        break;
    case 0x38:   /* Link Supervision Timeout Changed：handle(2), lsto(2) */
        b0 = len > 2 ? d[2] : 0;
        b1 = len > 3 ? d[3] : 0;
        b2 = 0;
        break;
    case 0x08:   /* Encryption Change：status, handle(2), enabled */
        b1 = len > 3 ? d[3] : 0;
        b2 = 0;
        break;
    default:     /* 其它照原样记前 3 字节（硬件错误 0x10 的错误码、厂商事件 0xFF 的子码都在这） */
        break;
    }
    diag_log(DIAG_EV_HCI_EVT, (uint32_t)evt | ((uint32_t)b0 << 8) | ((uint32_t)b1 << 16) |
                              ((uint32_t)b2 << 24), 0);
}

/* R1.4：连上后做一次。16:34 那次 ACL_DOWN 0x08 恰好在最后一帧之后 5.0 s ⇒ 这条链路的监督超时
 * 是 5 s（S31 起我们不再写 0x0037，那 5 s 是谁定的要看角色）。我们是主（Central）就设成 10 s
 * （= 无输入看门狗），让 3~10 s 的射频停顿能自愈；是从就只能听手柄的。
 * 不设得更长：新控制器库拆一条死链要等监督超时到点，旧 ACL 没拆完手柄就回不来。
 * DIAG_EV_LINK_PARAM a1 = 连上时的监督超时(slot) | 角色<<16 | 写结果<<24
 *（写结果 0 = 成功 / 0xFE = 没写（不是主或没读到）/ 其它 = 失败的 errno）。 */
void bt_hid_host_link_params(void)
{
    struct bt_conn *c = hid_ctx.conn;
    if (!c || hid_ctx.state != BT_HID_STATE_CONNECTED)
        return;
    uint16_t handle = c->handle;
    uint8_t  r[8];
    uint8_t  role = 0xFF;
    uint16_t lsto = 0;
    uint8_t  wr = 0xFE;

    if (hci_status_read(BT_OP(BT_OGF_LINK_POLICY, 0x0009), handle, r, sizeof(r)) >= 4 && r[0] == 0)
        role = r[3];
    if (hci_status_read(BT_OP(BT_OGF_BASEBAND, 0x0036), handle, r, sizeof(r)) >= 5 && r[0] == 0)
        lsto = (uint16_t)(r[3] | (r[4] << 8));

    s_rs_tried = false;                    /* R1.5：每次连上重新来 */
    s_conn_ms = (uint32_t)(bflb_mtimer_get_time_us() / 1000ULL);
    s_lsto_done = (role == 0x00);
    s_afh_logged_n = 0xFF;
    if (role == 0x00 && lsto != LINK_LSTO_SLOTS)
        wr = link_write_lsto(handle);
    diag_log(DIAG_EV_LINK_PARAM, (uint32_t)lsto | ((uint32_t)role << 16) | ((uint32_t)wr << 24), 0);
    LOG_INF("[BT] link params: role=%u lsto=%u slots write=%u\n",
            (unsigned)role, (unsigned)lsto, (unsigned)wr);
}

uint32_t bt_hid_host_link_stats_packed(void)
{
    return (uint32_t)(uint8_t)bt_hid_host_get_cached_rssi() |
           ((uint32_t)s_link_quality << 8) |
           ((uint32_t)s_afh_used << 16) |
           ((uint32_t)s_afh_mode << 24);
}

int8_t bt_hid_host_get_cached_rssi(void)
{
    return cached_rssi;
}

int bt_hid_host_send_output(const uint8_t *data, uint16_t len)
{
    if (!hid_ctx.intr_connected || !hid_ctx.conn)
        return -1;

    /* Route by report type:
     *   0x31 game output (78 B)  → 10-deep FIFO with CAN_SEND_NOW flow control
     *   0x32 init (142 B)        → direct send (large, infrequent, bypasses FIFO)
     *   0x39 audio (547 B)       → direct send (too large for FIFO slot, high rate)
     *
     * This matches DS5DongleRE which routes only game output through send_fifo;
     * audio haptics and init go via separate direct l2cap_send calls. */
    bool is_game_output = (len <= APP_TX_DATA_SZ - 1) &&
                          (len == DS5_BT_OUTPUT_REPORT_SIZE || data[0] == DS5_BT_OUTPUT_REPORT_ID);

    if (!is_game_output) {
        /* Direct path: allocate and send immediately */
        struct net_buf *buf = net_buf_alloc(&hid_tx_pool, K_NO_WAIT);
        if (!buf) {
            LOG_WRN("[HID] send_output direct: pool empty (len=%d)\n", (int)len);
            return -2;   /* S36：和"没连接"(-1) 区分开，audio.c 按原因计数 */
        }
        net_buf_reserve(buf, BT_L2CAP_CHAN_SEND_RESERVE);
        net_buf_add_u8(buf, HID_HEADER(HID_TRANS_DATA, HID_REPORT_OUTPUT));
        net_buf_add_mem(buf, data, len);
        int err = bt_l2cap_chan_send(&hid_ctx.intr_chan.chan, buf);
        if (err < 0) {
            net_buf_unref(buf);
            LOG_ERR("[HID] send_output direct: l2cap err=%d\n", err);
            return err;
        }
        return 0;
    }

    /* FIFO path for 0x31 game output (equivalent to DS5DongleRE's send_fifo) */
    uint8_t next_tail = (app_tx_tail + 1) % APP_TX_DEPTH;

    /* FIFO full: drop oldest frame to make room (same as DS5DongleRE) */
    if (next_tail == app_tx_head) {
        app_tx_head = (app_tx_head + 1) % APP_TX_DEPTH;
    }

    uint8_t slot = app_tx_tail;
    app_tx_buf[slot][0] = HID_HEADER(HID_TRANS_DATA, HID_REPORT_OUTPUT);
    memcpy(app_tx_buf[slot] + 1, data, len);
    app_tx_len[slot] = len + 1;
    app_tx_tail = next_tail;

    if (!first_output_logged) {
        first_output_logged = true;
        LOG_INF("[HID] First output report enqueued, len=%d\n", (int)len);
    }

    /* Trigger drain if channel is idle (CAN_SEND_NOW equivalent) */
    if (!app_tx_busy) {
        hid_tx_drain_one();
    }

    return 0;
}

int bt_hid_host_set_feature(uint8_t report_id, const uint8_t *data,
                             uint16_t len)
{
    if (!hid_ctx.ctrl_connected || !hid_ctx.conn)
        return -1;

    struct net_buf *buf = net_buf_alloc(&hid_tx_pool, K_NO_WAIT);
    if (!buf)
        return -1;

    net_buf_reserve(buf, BT_L2CAP_CHAN_SEND_RESERVE);
    net_buf_add_u8(buf, HID_HEADER(HID_TRANS_SET_REPORT, HID_REPORT_FEATURE));
    net_buf_add_u8(buf, report_id);
    if (data && len > 0)
        net_buf_add_mem(buf, data, len);

    int err = bt_l2cap_chan_send(&hid_ctx.ctrl_chan.chan, buf);
    if (err < 0) {
        net_buf_unref(buf);
        return err;
    }

    return 0;
}

int bt_hid_host_get_feature(uint8_t report_id)
{
    if (!hid_ctx.ctrl_connected || !hid_ctx.conn)
        return -1;

    struct net_buf *buf = net_buf_alloc(&hid_tx_pool, K_NO_WAIT);
    if (!buf)
        return -1;

    net_buf_reserve(buf, BT_L2CAP_CHAN_SEND_RESERVE);
    net_buf_add_u8(buf, HID_HEADER(HID_TRANS_GET_REPORT, HID_REPORT_FEATURE));
    net_buf_add_u8(buf, report_id);

    int err = bt_l2cap_chan_send(&hid_ctx.ctrl_chan.chan, buf);
    if (err < 0) {
        net_buf_unref(buf);
        return err;
    }

    LOG_DBG("[HID] GET_REPORT(Feature, 0x%02x) sent\n", report_id);
    return 0;
}

enum bt_hid_host_state bt_hid_host_get_state(void)
{
    return hid_ctx.state;
}

bool bt_hid_host_get_connected_addr(uint8_t *addr)
{
    if (!hid_ctx.conn)
        return false;

    const bt_addr_t *remote = conn_get_br_addr(hid_ctx.conn);
    if (!remote)
        return false;

    memcpy(addr, remote->val, 6);
    return true;
}

/* ---------- bonding persistence ---------- */

int bt_hid_host_try_reconnect(void)
{
    if (hid_ctx.state != BT_HID_STATE_IDLE)
        return -1;

    if (bonded_count == 0) {
        LOG_INF("[BT] No bonded BR/EDR device found\n");
        return -1;
    }

    if (active_idx >= bonded_count)
        active_idx = 0;

    bt_addr_t *addr = &bonded_list[active_idx];
    char addr_str[BT_ADDR_STR_LEN];
    bt_addr_to_str(addr, addr_str, sizeof(addr_str));

    bt_addr_copy(&hid_ctx.target_addr, addr);
    hid_ctx.has_target = true;

    /* DS5DongleRE approach: don't actively page the controller.
     * Stay connectable and let the controller reconnect to us.
     * The controller will create ACL + L2CAP channels. */
    LOG_INF("[BT] Bonded device [%d/%d] %s — waiting for controller to reconnect "
           "(page scan enabled)\n", active_idx, bonded_count, addr_str);
    return 0;
}

int bt_hid_host_page_bonded(void)
{
    if (hid_ctx.conn || hid_ctx.state != BT_HID_STATE_IDLE ||
        hid_ctx.connect_pending)
        return -1;
    if (bonded_count == 0 || !hid_ctx.has_target)
        return -1;

    bt_addr_copy(&hid_ctx.pending_addr, &hid_ctx.target_addr);
    hid_ctx.connect_pending = true;
    hid_ctx.connect_pending_tick = 0;  /* fire on next poll_connect pass */

    char addr_str[BT_ADDR_STR_LEN];
    bt_addr_to_str(&hid_ctx.target_addr, addr_str, sizeof(addr_str));
    LOG_INF("[BT] Active reconnect: paging bonded %s\n", addr_str);
    return 0;
}

int bt_hid_host_switch_next(void)
{
    if (bonded_count <= 1) {
        LOG_INF("[BT] switch_next: only %d bonded, nothing to switch\n",
               bonded_count);
        return -1;
    }

    active_idx = (active_idx + 1) % bonded_count;
    bonded_list_persist_idx();

    bt_addr_t *addr = &bonded_list[active_idx];
    bt_addr_copy(&hid_ctx.target_addr, addr);
    hid_ctx.has_target = true;

    char addr_str[BT_ADDR_STR_LEN];
    bt_addr_to_str(addr, addr_str, sizeof(addr_str));

    if (hid_ctx.conn) {
        /* Block further BT sends immediately */
        hid_ctx.ctrl_connected = false;
        hid_ctx.intr_connected = false;
        switch_pending = true;

        LOG_INF("[BT] Switching: disconnect current → wait ACL teardown → "
               "reconnect [%d/%d] %s\n", active_idx, bonded_count, addr_str);
        diag_log(DIAG_EV_DISC_REQ, BT_DISC_SITE_SWITCH_NEXT, (uint32_t)hid_ctx.state);   /* S32 */
        bt_conn_disconnect(hid_ctx.conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        /* conn_disconnected callback will handle full cleanup and
         * enable connectable only after old ACL is fully torn down. */
    } else {
        if (hid_ctx.state == BT_HID_STATE_SCANNING)
            bt_br_discovery_stop();

        hid_ctx.ctrl_accepting = false;
        hid_ctx.intr_accepting = false;
        hid_ctx.outgoing = false;
        hid_ctx.is_dse = false;
        hid_ctx.check_dse = false;
        hid_ctx.security_pending = false;
        hid_ctx.security_done = false;
        hid_ctx.sdp_pending = false;
        hid_ctx.connect_pending = false;
        hid_ctx.handshake_next  = 0;
        hid_ctx.handshake_count = 0;
        hid_ctx.handshake_send_failed = false;

        app_tx_fifo_reset();
        feature_cache_clear();
        dse_reset();
        cached_rssi = 1;

        bt_br_set_connectable(true);
        bt_br_set_discoverable(true);

        LOG_INF("[BT] Switched to controller [%d/%d] %s — waiting for reconnect\n",
               active_idx, bonded_count, addr_str);
        set_state(BT_HID_STATE_IDLE);
    }

    return (int)active_idx;
}

bool bt_hid_host_is_switching(void)
{
    return switch_pending;
}

uint8_t bt_hid_host_get_bonded_count(void)
{
    return bonded_count;
}

uint8_t bt_hid_host_get_active_idx(void)
{
    return active_idx;
}

void bt_hid_host_clear_bonds(void)
{
    LOG_INF("[BT] Clearing all bonding info\n");

    if (hid_ctx.state == BT_HID_STATE_SCANNING)
        bt_br_discovery_stop();

    /* Disconnect the current controller if connected */
    {
        struct bt_conn *c = conn_take(NULL);   /* R1：原子摘取，再做阻塞的 HCI 调用 */
        if (c) {
            diag_log(DIAG_EV_DISC_REQ, BT_DISC_SITE_CLEAR_BONDS, (uint32_t)hid_ctx.state);   /* S32 */
            bt_conn_disconnect(c, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
            bt_conn_unref(c);
        }
    }

    /* Clear the blacklist too — we want the controller to reconnect freely
     * after a manual bond clear. The security watchdog handles stale keys. */
    blacklist_count = 0;

    hid_ctx.ctrl_connected = false;
    hid_ctx.intr_connected = false;
    hid_ctx.ctrl_accepting = false;
    hid_ctx.intr_accepting = false;
    hid_ctx.outgoing = false;
    hid_ctx.is_dse = false;
    hid_ctx.check_dse = false;
    hid_ctx.handshake_next  = 0;
    hid_ctx.handshake_count = 0;
    hid_ctx.handshake_send_failed = false;
    hid_ctx.security_pending = false;
    hid_ctx.security_done = false;
    hid_ctx.sdp_pending = false;
    hid_ctx.connect_pending = false;

    bt_keys_link_key_clear_addr(NULL);

    blacklist_persist();
    blacklist_dirty = false;
    LOG_INF("[BT] All link keys and blacklist cleared\n");

    bonded_count = 0;
    active_idx = 0;
    bonded_list_persist_idx();

    hid_ctx.has_target = false;
    memset(&hid_ctx.target_addr, 0, sizeof(hid_ctx.target_addr));

    bt_br_set_connectable(true);
    bt_br_set_discoverable(true);

    feature_cache_clear();
    dse_reset();

    set_state(BT_HID_STATE_IDLE);
}

bool bt_hid_host_get_cached_feature(uint8_t report_id,
                                    const uint8_t **data, uint16_t *len)
{
    for (int i = 0; i < FEATURE_CACHE_SLOTS; i++) {
        if (feature_cache[i].report_id == report_id &&
            feature_cache[i].len > 0) {
            *data = feature_cache[i].data;
            *len  = feature_cache[i].len;
            return true;
        }
    }
    return false;
}

bool bt_hid_host_is_dse(void)
{
    return hid_ctx.is_dse;
}

int bt_hid_host_set_feature_crc(uint8_t report_id, const uint8_t *data,
                                uint16_t len)
{
    if (!hid_ctx.ctrl_connected || !hid_ctx.conn) {
        LOG_WRN("[HID] SET_FEATURE_CRC(0x%02x): not connected (ctrl=%d conn=%p)\n",
               report_id, hid_ctx.ctrl_connected, hid_ctx.conn);
        return -1;
    }

    struct net_buf *buf = net_buf_alloc(&hid_tx_pool, K_NO_WAIT);
    if (!buf) {
        LOG_ERR("[HID] SET_FEATURE_CRC(0x%02x): net_buf alloc failed\n", report_id);
        return -1;
    }

    net_buf_reserve(buf, BT_L2CAP_CHAN_SEND_RESERVE);
    net_buf_add_u8(buf, HID_HEADER(HID_TRANS_SET_REPORT, HID_REPORT_FEATURE));
    net_buf_add_u8(buf, report_id);
    if (data && len > 0)
        net_buf_add_mem(buf, data, len);

    /* Overwrite last 4 bytes of data with CRC (matching DS5DongleRE's
     * fill_feature_report_checksum). The HID descriptor's Report Count
     * includes 4 bytes of CRC space that the USB host sends as zeros. */
    if (len >= 4) {
        uint8_t *crc_region = buf->data + 1;
        uint16_t crc_data_len = 1 + (len - 4);
        uint32_t crc = ds5_crc32(DS5_BT_FEATURE_CRC_SEED,
                                  crc_region, crc_data_len);
        ds5_write_le32(crc_region + crc_data_len, crc);
    }

    int err = bt_l2cap_chan_send(&hid_ctx.ctrl_chan.chan, buf);
    if (err < 0) {
        LOG_ERR("[HID] SET_FEATURE_CRC(0x%02x): l2cap send failed err=%d\n",
               report_id, err);
        net_buf_unref(buf);
        return err;
    }

    LOG_INF("[HID] SET_REPORT(Feature, 0x%02x) sent with CRC, len=%d\n",
           report_id, (int)(len + 2));
    return 0;
}

void bt_hid_host_persist_if_dirty(void)
{
    if (blacklist_dirty) {
        blacklist_dirty = false;
        blacklist_persist();
    }
}

void bt_hid_host_handshake_tick(void)
{
    if (hid_ctx.state == BT_HID_STATE_HANDSHAKE &&
        hid_ctx.handshake_send_failed) {
        handshake_send_next();
    }
}

void bt_hid_host_radio_idle(void)
{
    bt_br_set_connectable(false);
    bt_br_set_discoverable(false);
    LOG_INF("[BT] Radio idle — page/inquiry scan disabled\n");
}

/* S31（照上游 3.21 的 [BT-HB] 心跳）：读控制器当前的 Scan_Enable。
 * HCI Read_Scan_Enable：OGF 0x03 / OCF 0x0019，应答 [status][scan_enable]，
 * bit0 = inquiry scan，bit1 = page scan。返回 0..3，失败返回负数。
 * 用途：等待连接期间 page scan 若被某条路径悄悄关掉（手柄就永远连不上），
 * 主循环每 ~10 s 对一次账并补开。 */
int bt_hid_host_read_scan_enable(void)
{
    struct net_buf *buf = bt_hci_cmd_create(BT_OP(BT_OGF_BASEBAND, 0x0019), 0);
    if (!buf)
        return -1;
    struct net_buf *rsp = NULL;
    int err = bt_hci_cmd_send_sync(BT_OP(BT_OGF_BASEBAND, 0x0019), buf, &rsp);
    if (err || !rsp) {
        if (rsp)
            net_buf_unref(rsp);
        return err ? err : -1;
    }
    int ret = -1;
    if (rsp->len >= 2 && rsp->data[0] == 0)
        ret = rsp->data[1];
    net_buf_unref(rsp);
    return ret;
}

void bt_hid_host_radio_wake(void)
{
    bt_br_set_connectable(true);
    bt_br_set_discoverable(true);
    LOG_INF("[BT] Radio wake — page/inquiry scan re-enabled\n");
}
