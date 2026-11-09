# -*- coding: utf-8 -*-
"""R1.3：陀螺仪去掉自动校准、静置校准存 flash；链路诊断（RSSI / 链路质量 / AFH 信道数 / 输入停顿）。
gyro.c / gyro.h 已整份重写（Write 工具），这里改其余文件。
用法：python tools/_r13_patch.py"""
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _patchlib import P

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
SRC = os.path.join(ROOT, 'firmware', 'src')

p = P(os.path.join(SRC, 'version.h'))
p.rep('#define FW_VERSION      "R1.2"', '#define FW_VERSION      "R1.3"')
p.save()

# ======================= ds5map.c =======================
p = P(os.path.join(SRC, 'ds5map.c'))
p.rep("""/* R1.2：陀螺仪开着但没生效时，快速路径里融合只跑 1/GYRO_IDLE_DECIM 帧（引擎 ~500 Hz → ~125 Hz），
 * 只为维持零偏校准（零偏以分钟计变化，采样率低四倍测出来一样）。 */
#define GYRO_IDLE_DECIM 4
static uint8_t       g_gyro_decim;
""", "")
p.rep("""/* 测试页读一次遥测就把融合续命这么久：20 Hz 轮询够用，关掉页面约 1.5 s 后自动回到快速路径 */
#define GYRO_PROBE_MS 1500
""", "")
p.rep("""static volatile uint8_t g_gyro_probe;  /* 测试页读遥测时置位：把融合临时叫醒 */
static uint32_t g_gyro_probe_ms;   /* 最近一次被叫醒的时刻 */
""", """static volatile uint8_t g_gyro_probe;  /* 测试页读遥测 / 发校准命令时置位：下一帧刷新陀螺仪遥测 */
""")
p.rep("""    /* 融合什么时候跑：陀螺仪开着当然每帧都得跑（不然按住启用键那一刻重力估计还没收敛）；
     * 关着也可能有人在看——测试页轮询遥测时（静置校准按钮就挂在那页）把融合临时叫醒。
     * 别写成「配了目标就跑」：默认配置就是「映射到鼠标 + 启用＝关闭」，那样快速路径
     * 等于永远不生效。 */
    if (g_gyro_probe) {
        g_gyro_probe = 0;
        g_gyro_probe_ms = now ? now : 1;
    }
    const bool gyro_track = g_gyro_on ||
        (g_gyro_probe_ms && (uint32_t)(now - g_gyro_probe_ms) < GYRO_PROBE_MS);
""", """    /* R1.3：自动零偏校准已删除，平时陀螺仪没有任何每帧计算。只有静置校准的那 3 秒
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
""")
p.rep("""        !gyro_track && !g_tp_on && g_last_mouse_btns == 0) {
""", """        !g_gyro_on && !g_tp_on && g_last_mouse_btns == 0) {
""")
p.rep("""        if (gyro_track && ++g_gyro_decim >= GYRO_IDLE_DECIM) {
            g_gyro_decim = 0;
            uint32_t gc0 = perf_cyc();
            gyro_update(p, now);
            tlm_gyro_pack(g_tlm_gyro);
            perf_gyro_cyc(perf_cyc() - gc0);
        }
        perf_frame_fast();
""", """        perf_frame_fast();
""")
p.rep("""    if (gyro_track) {
        uint32_t gc0 = perf_cyc();   /* R1.1：量陀螺仪这一段到底多贵 */
        gyro_update(p, now);
        tlm_gyro_pack(g_tlm_gyro);   /* 任务侧打包，ISR 的 build_tlm_state 只搬缓存 */
        perf_gyro_cyc(perf_cyc() - gc0);
    }

""", "")
p.rex(r"    gyro_reset\(\);[^\n]*\n", "    gyro_reset();          /* R1.3：只取消进行中的静置校准；存下来的零偏保留 */\n")
# gcal.bin：放在 cfg_save 前面（crc16_ccitt 之后）
p.rep("""static bool cfg_save(void)
{
""", """/* R1.3：静置校准出来的零偏单独存一个小文件，和按键映射的 cfg.bin 在同一个 LittleFS 上。
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
        LOG_ERR("[GYRO] gcal save: open failed\\n");
        return;
    }
    bool ok = lfs_file_write(fs, &f, &g, sizeof(g)) == (lfs_ssize_t)sizeof(g);
    if (lfs_file_close(fs, &f) != 0)
        ok = false;
    LOG_INF("[GYRO] gcal %s\\n", ok ? "saved" : "save FAILED");
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
    LOG_INF("[GYRO] gcal loaded\\n");
}

static bool cfg_save(void)
{
""")
# 全局状态：放在 g_cfg_dirty 旁边（gcal_save / ds5map_process 都要用）
p.rep("""/* Staged-but-unflushed config flag */
static volatile bool g_cfg_dirty;
""", """/* Staged-but-unflushed config flag */
static volatile bool g_cfg_dirty;

/* R1.3：静置校准出来的新零偏，等 deferred（bt 任务）写进 gcal.bin */
static float         g_gcal_bias[3];
static volatile bool g_gcal_dirty;
""")
p.rep("""    if (!g_pend_active && !g_cfg_dirty)
        return;

    MAP_LOCK();
""", """    if (!g_pend_active && !g_cfg_dirty && !g_gcal_dirty)
        return;

    MAP_LOCK();

    if (g_gcal_dirty) {                /* R1.3：静置校准的零偏落盘（一次性，用户点了校准才有） */
        g_gcal_dirty = false;
        gcal_save();
    }
""")
p.rep("""    macro_store_init();
    ds5map_load();
    tlm_gyro_pack(g_tlm_gyro);   /* 开机快照：ISR 遥测在第一帧输入前也有正确初值 */
""", """    macro_store_init();
    ds5map_load();
    gcal_load();                 /* R1.3：静置校准存下来的零偏 */
    tlm_gyro_pack(g_tlm_gyro);   /* 开机快照：ISR 遥测在第一帧输入前也有正确初值 */
""")
p.save()
for w in ('gyro_track', 'g_gyro_probe_ms', 'GYRO_PROBE_MS', 'g_gyro_decim'):
    assert w not in p.s, w

# ======================= bt_hid_host.c / .h =======================
p = P(os.path.join(SRC, 'bt_hid_host.c'))
p.rep('#include "diag.h"', '#include "perf.h"      /* R1.3：链路诊断进 PERF */\n#include "diag.h"', cnt=1)
p.rep("""int bt_hid_host_read_rssi(int8_t *rssi)
{
    if (!hid_ctx.conn || hid_ctx.state != BT_HID_STATE_CONNECTED)
        return -1;
""", """int bt_hid_host_read_rssi(int8_t *rssi)
{
    struct bt_conn *c = hid_ctx.conn;   /* R1.3：只读一次——检查完再读 hid_ctx.conn 会被 BT 线程置空 */
    if (!c || hid_ctx.state != BT_HID_STATE_CONNECTED)
        return -1;
""")
p.rep("""    uint16_t handle = hid_ctx.conn->handle;
    struct net_buf *buf = bt_hci_cmd_create(BT_HCI_OP_READ_RSSI, 2);
""", """    uint16_t handle = c->handle;
    struct net_buf *buf = bt_hci_cmd_create(BT_HCI_OP_READ_RSSI, 2);
""")
p.rep("""int8_t bt_hid_host_get_cached_rssi(void)
""", """/* R1.3：链路诊断。2026-10-14 下午实测掉线前手柄送达的帧率从 ~700 掉到 150~250 再到 0，
 * RSSI 读数一直正常 —— 要看清是不是链路本身在变差（干扰 → 重传 → AFH 砍信道）。
 * 每 ~2 s 由 bt 任务调一次：Read_Link_Quality (0x05/0x0003)、Read_AFH_Channel_Map (0x05/0x0006)。 */
static volatile uint8_t s_link_quality, s_afh_used, s_afh_mode;

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
        uint8_t n = 0;
        for (int i = 0; i < 10; i++) {
            uint8_t b = r[4 + i];
            if (i == 9)
                b &= 0x7F;
            while (b) {
                n += b & 1u;
                b >>= 1;
            }
        }
        s_afh_used = n;
    }
    perf_link(bt_hid_host_get_cached_rssi(), s_link_quality, s_afh_used, s_afh_mode);
}

uint32_t bt_hid_host_link_stats_packed(void)
{
    return (uint32_t)(uint8_t)bt_hid_host_get_cached_rssi() |
           ((uint32_t)s_link_quality << 8) |
           ((uint32_t)s_afh_used << 16) |
           ((uint32_t)s_afh_mode << 24);
}

int8_t bt_hid_host_get_cached_rssi(void)
""")
p.save()

p = P(os.path.join(SRC, 'bt_hid_host.h'))
p.rep("""int8_t bt_hid_host_get_cached_rssi(void);
""", """int8_t bt_hid_host_get_cached_rssi(void);

/* R1.3：链路诊断（task 上下文，会发 HCI 同步命令）：RSSI / 链路质量 / AFH 在用信道数，写进 PERF */
void bt_hid_host_read_link_stats(void);
/* 最近一次读到的：rssi(u8) | 链路质量<<8 | AFH 在用信道数<<16 | AFH 模式<<24 */
uint32_t bt_hid_host_link_stats_packed(void);
""")
p.save()

# ======================= perf =======================
p = P(os.path.join(SRC, 'perf.h'))
p.rep("""#define PERF_BLOB_SIZE  140   /* S36：96 → 116（发送失败分类、编码参数）；R1.1：→ 136（每帧分段计时）；R1.2：→ 140 */
""", """#define PERF_BLOB_SIZE  144   /* S36：96 → 116；R1.1：→ 136；R1.2：→ 140；R1.3：→ 144（链路诊断） */
""")
p.rep("""    uint16_t gyro_win;         /* 138 R1.2：陀螺仪融合跑了几次 */
} perf_blob_t;
""", """    uint16_t gyro_win;         /* 138 R1.2：陀螺仪融合跑了几次（R1.3 起只有静置校准时才跑） */
    int8_t   rssi;             /* 140 R1.3：最近一次 Read_RSSI（BR/EDR 是相对黄金区间的 dB，0 = 在区间内） */
    uint8_t  link_quality;     /* 141 R1.3：Read_Link_Quality 0..255（厂商定义，越大越好） */
    uint8_t  afh_used;         /* 142 R1.3：AFH 在用信道数 0..79（被干扰砍掉的越多越少） */
    uint8_t  afh_mode;         /* 143 R1.3：AFH 模式 0 关 / 1 开 */
} perf_blob_t;
""")
p.rep("""void perf_frame_fast(void);                             /* R1.2：这一帧走了快速路径 */
""", """void perf_frame_fast(void);                             /* R1.2：这一帧走了快速路径 */
void perf_link(int8_t rssi, uint8_t quality, uint8_t afh_used, uint8_t afh_mode);   /* R1.3 */
""")
p.save()

p = P(os.path.join(SRC, 'perf.c'))
p.rep("""void perf_frame_fast(void)
{
    s_fast_n++;
}
""", """void perf_frame_fast(void)
{
    s_fast_n++;
}

void perf_link(int8_t rssi, uint8_t quality, uint8_t afh_used, uint8_t afh_mode)
{
    g_perf.rssi = rssi;
    g_perf.link_quality = quality;
    g_perf.afh_used = afh_used;
    g_perf.afh_mode = afh_mode;
}
""")
p.save()

# ======================= diag.h =======================
p = P(os.path.join(SRC, 'diag.h'))
p.rep("""    DIAG_EV_SCAN_SET,     /* S37：arg1 = 1 连上后关 page/inquiry scan / 2 断开后重开 */
""", """    DIAG_EV_SCAN_SET,     /* S37：arg1 = 1 连上后关 page/inquiry scan / 2 断开后重开 */
    DIAG_EV_STALL,        /* R1.3：连着时输入停了 ≥1 s；arg1 = 当时的链路诊断（rssi | 质量<<8 | AFH 信道<<16 | AFH 模式<<24） */
    DIAG_EV_STALL_END,    /* R1.3：停顿后输入恢复；arg1 = 停了多少 ms */
""")
p.save()

# ======================= main.c =======================
p = P(os.path.join(SRC, 'main.c'))
p.rep("""        /* Periodic RSSI read (~every 5 s) */
        {
            static uint16_t rssi_tick = 0;
            if (++rssi_tick >= 500 && ds5_connected) {
                rssi_tick = 0;
                bt_hid_host_read_rssi(NULL);
            }
        }
""", """        /* R1.3：链路诊断（RSSI / 链路质量 / AFH 在用信道数）每 ~2 s 读一次，进 PERF；
         * 原来只读 RSSI、按循环计数大约 12 s 一次。 */
        {
            static uint64_t link_stat_us;
            uint64_t ln = bflb_mtimer_get_time_us();
            if (ds5_connected && ln - link_stat_us >= 2000000ULL) {
                link_stat_us = ln;
                bt_hid_host_read_link_stats();
            }
        }
""")
p.rep("""        if (ds5_connected && usb_fwd_count > 0 &&
            (bflb_mtimer_get_time_us() - last_input_us) > CONN_WATCHDOG_US) {
""", """        /* R1.3：输入停顿 ≥1 s 记一条（带当时的链路诊断），恢复时再记停了多久；
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

        if (ds5_connected && usb_fwd_count > 0 &&
            (bflb_mtimer_get_time_us() - last_input_us) > CONN_WATCHDOG_US) {
""")
p.save()

# ======================= 主机工具 =======================
p = P(os.path.join(ROOT, 'tools', 'diag.py'))
p.rep("""    35: 'SCAN_SET',                                                             # S37
""", """    35: 'SCAN_SET',                                                             # S37
    36: 'STALL', 37: 'STALL_END',                                               # R1.3
""")
p.rep("""PERF_SIZE = 140                         # S36：96 → 116；R1.1：→ 136（每帧分段计时）；R1.2：→ 140。一次 RPC 最多 122 B，分两段读
""", """PERF_SIZE = 144                         # S36：96 → 116；R1.1：→ 136；R1.2：→ 140；R1.3：→ 144（链路诊断）。一次 RPC 最多 122 B，分两段读
""")
p.rep("""    if len(buf) >= 140:
        (fw, gw) = struct.unpack_from('<HH', buf, 136)
        d.update(fast_win=fw, gyro_win=gw)
    return d
""", """    if len(buf) >= 140:
        (fw, gw) = struct.unpack_from('<HH', buf, 136)
        d.update(fast_win=fw, gyro_win=gw)
    if len(buf) >= 144:
        (rs, lq, au, am) = struct.unpack_from('<bBBB', buf, 140)
        d.update(rssi=rs, link_quality=lq, afh_used=au, afh_mode=am)
    return d
""")
p.rep("""    if 'fast_win' in d:
        s += ' 快路径%d 陀螺仪%d次' % (d['fast_win'], d['gyro_win'])
    return s
""", """    if 'fast_win' in d:
        s += ' 快路径%d 陀螺仪%d次' % (d['fast_win'], d['gyro_win'])
    if 'link_quality' in d:
        s += ' | 链路 RSSI%d 质量%d AFH%d/79%s' % (d['rssi'], d['link_quality'], d['afh_used'],
                                                  '' if d['afh_mode'] else '(AFH关)')
    return s
""")
p.save()

p = P(os.path.join(ROOT, 'tools', 'watch_state.py'))
p.rep("""    elif ev == 35:
""", """    elif ev == 36:
        x = '输入停了 >=1 s：RSSI=%d 链路质量=%d AFH 信道=%d/79' % (
            (a1 & 0xFF) - 256 if (a1 & 0x80) else (a1 & 0xFF), (a1 >> 8) & 0xFF, (a1 >> 16) & 0xFF)
    elif ev == 37:
        x = '输入恢复，停了 %d ms' % a1
    elif ev == 35:
""")
p.save()

# ======================= 网页 =======================
p = P(os.path.join(ROOT, 'webgui', 'js', 'page-test.js'))
p.rep("""  /* ---- 陀螺仪姿态（TLM_STATE 从第 48 字节起，总共 64）----
   * 固件在 gyro.c 里跑互补滤波估重力 + 静置学零偏；这一块把内部状态摊开：
   * 方向模式（降级后报的是实际生效的那个）、置信度、重力矢量、零偏三轴。
   * 静置校准按钮发 OP.ACTION，进度从 st[51] 来。老固件只到 48 字节，整块显示占位。 */
""", """  /* ---- 陀螺仪状态（TLM_STATE 从第 48 字节起，总共 64）----
   * R1.3 起固件没有自动零偏校准了：零偏只来自「静置校准」，成功后存进接收器（gcal.bin），
   * 开机读回、断开重连不丢。st[50] = 255 表示已有零偏；flags 0x10 = 上次校准失败（手柄动了）。
   * 静置校准按钮发 OP.ACTION，进度从 st[51] 来。老固件只到 48 字节，整块显示占位。 */
""")
p.rep("""  const GYRO_FLAG = [[1, '重力在用'], [2, '校准中'], [4, '已收敛'], [8, '重力不可信']];
""", """  const GYRO_FLAG = [[2, '校准中'], [4, '已校准（存在接收器里）'], [16, '上次校准失败：期间手柄动了，放平再来']];
""")
p.rep("""      || '刚连上，还在收数据';
""", """      || '未校准：手柄放平别动，点下面的「开始」';
""")
p.rep("""    set('gys-conv', `${Math.round(st[50] * 100 / 255)}%${flags & 4 ? '（已收敛）' : ''}`, flags & 4);
""", """    set('gys-conv', flags & 4 ? '已校准' : '未校准', flags & 4);
""")
p.rep("""          '静置校准中：把手柄放平、别动，3 秒';
""", """          '静置校准中：把手柄放平、别动，3 秒；成功后自动存进接收器';
""")
p.save()

p = P(os.path.join(ROOT, 'webgui', 'index.html'))
p.rep("零偏自动学习，也可在测试页「静置校准」。", "零偏靠测试页「静置校准」（手柄放平别动 3 秒），结果存在接收器里、重连不丢。")
p.rep("""          <div class="gy-stat-row"><label>零偏置信度</label>""", """          <div class="gy-stat-row"><label>零偏校准</label>""")
p.save()
print('R1.3 PATCH DONE')
