#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""两段式扳机（R2.5）真机验证：把扳机行程、阈值、二段到底有没有触发、以及映射出来的
键鼠/媒体输出，实时打在屏幕上。

只读三个 RPC 区（不改任何东西）：
  0x0b S2T        66 字节（R3.25 起）：**整套设置按层** —— 前 14 字节是主层那套
                  （阈值×2 / 迟滞 / 效果位 / 力度 / 结束 / 频率 / 两段位 / 行为位 / 左右分界点），
                  [14..25] 覆盖层 1..4 的两段位/分界点，[26..65] 覆盖层各自的自适应扳机参数。
                  本脚本只解前 12 字节 —— 没有覆盖层激活时固件用的就是主层那一套。
  0x10 TLM_RAW    63 字节：**映射前**的原始报文（p[4]=L2、p[5]=R2 行程，0..255）
  0x12 TLM_STATE  64 字节：映射后的实况（键盘报告 [12]/[14..19]、鼠标 [20]/[21]、媒体位图 [44..45]）

它**不判断卡点**（那个只有手指能感觉到），只看「过了阈值之后电脑有没有收到该收的东西」：
  轻按（没过阈值）→ 不该出键；压过阈值 → 出键；松开 → 键消失。
卡点手感 + 游戏里让位那条，照 `firmware/HANDOFF.md` §⑮ 用手指和游戏试。

用法：
  python -X utf8 watch_s2t.py [秒数]     # 默认 60 秒。手柄放桌上，慢慢把 L2 / R2 压到底再松开
  python -X utf8 watch_s2t.py --sim      # 不用设备：合成一段行程，自检这个脚本自己（改完先跑它）

⚠ 和网页抢 0xFC：跑这个脚本时先关掉网页配置工具（或者反过来）。
"""
import sys
import time

import diag

REG_S2T = 0x0B
REG_TLM_RAW = 0x10
REG_TLM_STATE = 0x12
REG_VER = 0x15

S2T_LEN = 12
TLM_STATE_LEN = 64
KIND_NAMES = ['阻力（Feedback 0x21）', '扳机（Weapon 0x25）', '自动扳机（Vibration 0x26）']

# 媒体位图的名字（下标 = 固件 OUT_MEDIA 的 val，和 js/page-map.js 的 MEDIA_NAMES 同一张表）
MEDIA_NAMES = ['系统音量 +', '系统音量 −', '系统静音', '播放/暂停', '下一曲', '上一曲', '停止',
               '快进', '快退', '媒体播放器', '计算器', '搜索', '主页', '后退', '前进', '刷新']
MOUSE_BTNS = ['左键', '右键', '中键']

# 键盘 usage → 名字（够用就行：字母/数字/几个常用键/修饰键）
KEY_NAMES = {}
for _i in range(26):
    KEY_NAMES[0x04 + _i] = chr(ord('A') + _i)
for _i in range(9):
    KEY_NAMES[0x1E + _i] = str(_i + 1)
KEY_NAMES[0x27] = '0'
KEY_NAMES.update({0x28: '回车', 0x29: 'Esc', 0x2A: '退格', 0x2B: 'Tab', 0x2C: '空格',
                  0xE0: '左Ctrl', 0xE1: '左Shift', 0xE2: '左Alt', 0xE3: '左Win',
                  0xE4: '右Ctrl', 0xE5: '右Shift', 0xE6: '右Alt', 0xE7: '右Win'})


def key_names(codes):
    return '+'.join(KEY_NAMES.get(c, '0x%02x' % c) for c in codes if c) or '（无）'


def media_names(bits):
    return '+'.join(MEDIA_NAMES[i] for i in range(16) if bits & (1 << i)) or '（无）'


def s2t_state_txt(bits):
    """TLM_STATE[47]（R2.6）：bit0/1 = 我们已给**左/右**扳机装上效果；bit2/3 = 游戏正在管左/右（我们让位）。
    位号和配置区统一（bit0 = 左 = L2）。"配了效果却没手感"先看这一行：装上了没？是不是游戏一直在抢？"""
    ours = '+'.join(nm for i, nm in ((0, '左'), (1, '右')) if (bits >> i) & 1) or '无'
    game = '+'.join(nm for i, nm in ((2, '左'), (3, '右')) if (bits >> i) & 1)
    return '设备侧：我们已装上=%s%s' % (ours, ('，游戏在管=%s（这时候我们让位）' % game) if game else '')


def parse_s2t(b):
    # 12 字节（R2.6）：0/1 起始位置、2 迟滞、3 效果开关位、4 力度（低半左/高半右 1..8）、
    # 5 效果类型（低 2 位左/bit2-3 右：0 阻力 / 1 扳机 / 2 自动扳机）、6/7 结束位置、
    # 8/9 频率 Hz（0=默认 30）、10 网页的「两段模式」位（固件只存不解释）、11 保留。
    # 工具按模式位换措辞：普通模式那根滑条只当"起始位置"（纯手感），两段模式过它＝二段按下。
    return {
        'thresh_l': b[0], 'thresh_r': b[1], 'hyst': b[2] or 8,
        'effect_mode': b[3] & 3,
        'str_l': (b[4] & 0x0F) or 6, 'str_r': (b[4] >> 4) or 6,
        'kind_l': b[5] & 3, 'kind_r': (b[5] >> 2) & 3,
        'end_l': b[6], 'end_r': b[7],
        'freq_l': b[8] or 10, 'freq_r': b[9] or 10,
        'mode': b[10] & 3 if len(b) > 10 else 0,
    }


def fx_on(cfg, side):
    return bool(cfg['effect_mode'] >> (0 if side == 0 else 1) & 1)


def kind_name(cfg, side):
    k = cfg['kind_l' if side == 0 else 'kind_r']
    return ('阻力', '扳机', '自动扳机')[k] if k <= 2 else '?'


def mode_name(cfg, side):
    return '两段' if (cfg['mode'] >> (0 if side == 0 else 1)) & 1 else '普通'


def s2t_zone(pos):
    """镜像 ds5map.c 的 s2t_zone()：起始/结束位置 0..255 → 硬件行程档位 0..9。"""
    z = (pos * 10 + 127) // 255
    return min(z, 9)


def eff_zone(cfg, side):
    """这一侧的效果**实际**落在哪几档（lo, hi, 原始起始档, 是否被夹）—— 镜像 ds5map.c 的
    ds5map_s2t_effect()：阻力 = 起始档..结束档（不设结束=到 9 档）；自动扳机 = 起始档..9；
    扳机 = 起始~结束档（硬件 start 2..7 / end ≤8）。"""
    pos = cfg['thresh_l' if side == 0 else 'thresh_r']
    z = s2t_zone(pos)
    kind = cfg['kind_l' if side == 0 else 'kind_r']
    en_pos = cfg['end_l' if side == 0 else 'end_r']
    if kind == 0:
        hi = s2t_zone(en_pos) if en_pos else 9
        if hi < z:
            hi = z
        return (z, hi, z, False)
    if kind == 2:
        return (z, 9, z, False)
    st = min(max(z, 2), 7)
    en = s2t_zone(en_pos) if en_pos else st + 1
    if en <= st:
        en = st + 1
    if en > 8:
        en = 8
    if en <= st:
        st = en - 1
    return (st, en, z, st != z)


class Analyzer:
    """把 TLM_RAW / TLM_STATE 的帧喂进来，负责判定与汇总。真机和 --sim 走同一条路。"""

    def __init__(self, cfg):
        self.cfg = cfg
        self.latch = 0                      # bit0 左 / bit1 右：二段「已按下」（镜像 s2t_edge 的迟滞）
        self.travel = [255, 255]            # 每侧见过的最小行程（松开到底应该是 0 附近）
        self.max_seen = [0, 0]
        self.cross_up = [0, 0]              # 每侧过了阈值几次
        self.cross_dn = [0, 0]
        self.up_value = [None, None]        # 那一次过阈值时的行程值（对不对一眼能看出）
        self.out_while_on = [0, 0]          # 二段按着期间收到过几次键/鼠标/媒体
        self.out_while_off = 0              # 没过阈值却收到键的次数（应为 0）
        self.last_out = None
        self.frames = 0

    # ---- 阈值判定：与固件 ds5map.c s2t_edge() 同一套（迟滞、thresh=0 = 该侧关） ----
    def edge(self, side, v):
        th = self.cfg['thresh_l'] if side == 0 else self.cfg['thresh_r']
        if not th:
            return 0
        hyst = self.cfg['hyst']
        bit = 1 << side
        hi, lo = min(th + hyst, 255), (th - hyst if th > hyst else 0)
        if v >= hi:
            self.latch |= bit
        elif v <= lo:
            self.latch &= ~bit
        return 1 if (self.latch & bit) else 0

    def outputs(self, st):
        if not st or len(st) < 46:
            return ('（无遥测）', 0, 0)
        kb = (st[12], tuple(st[14:20]))
        mouse = (st[20], st[21])
        media = st[44] | (st[45] << 8)
        return (kb, mouse, media)

    def feed(self, raw, st, t):
        """返回要打印的行（列表）。raw = 63 字节 TLM_RAW，st = 64 字节 TLM_STATE。"""
        lines = []
        if not raw or len(raw) < 10 or not any(raw[0:4]):
            return lines                      # 手柄没连 / 还没收到过报文
        self.frames += 1
        vals = (raw[4], raw[5])               # [0]=L2 [1]=R2
        states = []
        for side in (0, 1):
            v = vals[side]
            self.travel[side] = min(self.travel[side], v)
            self.max_seen[side] = max(self.max_seen[side], v)
            st_on = self.edge(side, v)
            states.append(st_on)

        on = (states[0] << 0) | (states[1] << 1)
        prev_latch = getattr(self, '_prev_on', None)
        if prev_latch is None:
            self._prev_on = on
            lines.append('%6.2fs  基准：L2=%3d%s  R2=%3d%s' % (
                t, vals[0], '（二段已按下）' if states[0] else '',
                vals[1], '（二段已按下）' if states[1] else ''))
        else:
            for side in (0, 1):
                was, now = (prev_latch >> side) & 1, states[side]
                if now and not was:
                    self.cross_up[side] += 1
                    self.up_value[side] = vals[side]
                    if mode_name(self.cfg, side) == '两段':
                        lines.append('%6.2fs  ★ %s 过二段触发点：行程 %d（触发点 %d±%d）→ 二段按下（第 %d 次）'
                                     % (t, 'L2' if side == 0 else 'R2', vals[side],
                                        self.cfg['thresh_l' if side == 0 else 'thresh_r'],
                                        self.cfg['hyst'], self.cross_up[side]))
                    else:
                        lines.append('%6.2fs  ·  %s 过起始位置：行程 %d（效果起点 %d±%d）'
                                     '（普通模式：只影响手感，不触发二段）'
                                     % (t, 'L2' if side == 0 else 'R2', vals[side],
                                        self.cfg['thresh_l' if side == 0 else 'thresh_r'],
                                        self.cfg['hyst']))
                elif was and not now:
                    self.cross_dn[side] += 1
                    lines.append('%6.2fs     %s 松回起始位置以下：行程 %d（第 %d 次）'
                                 % (t, 'L2' if side == 0 else 'R2', vals[side], self.cross_dn[side]))
            self._prev_on = on

        out = self.outputs(st)
        if out != self.last_out:
            kb, mouse, media = out
            bits = mouse[0]
            # 注意：kb[1] 是**定长 6 元的元组**，bool() 永远为真 —— 必须 any()
            active = bool(kb[0]) or any(kb[1]) or bool(bits) or bool(media)
            if active:
                if on:
                    self.out_while_on[0] += 1 if states[0] else 0
                    self.out_while_on[1] += 1 if states[1] else 0
                else:
                    self.out_while_off += 1
            name = []
            if kb[0] or any(kb[1]):
                name.append('键盘 修饰键=0x%02x 键=%s' % (kb[0], key_names(kb[1])))
            if bits or mouse[1]:
                name.append('鼠标 %s%s' % ('+'.join(MOUSE_BTNS[i] for i in range(3) if bits & (1 << i)) or '（无键）',
                                           ' 滚轮%+d' % mouse[1] if mouse[1] else ''))
            if media:
                name.append('媒体 %s' % media_names(media))
            if name:
                lines.append('%6.2fs     输出：%s   %s' % (
                    t, ' | '.join(name),
                    '（二段按着）' if on else '（二段没触发 → 这不该出现）'))
            self.last_out = out
        return lines

    def status(self, raw):
        """一行实时行程条：L2/R2 各自带阈值刻度。"""
        parts = []
        for side in (0, 1):
            v = raw[4 + side] if raw and len(raw) > 5 else 0
            th = self.cfg['thresh_l' if side == 0 else 'thresh_r']
            bar = ['-'] * 20
            for i in range(20):
                if (i + 1) * 13 <= v:
                    bar[i] = '#'
            mark = ''
            if th:
                # 标记画在**起始档的实际落点**（扳机那种会被 start 2..7 的硬约束夹），
                # 不然拖到很低时进度条上的竖线会和手指摸到的位置对不上
                lo, _hi, _z, _c = eff_zone(self.cfg, side)
                pos = min(19, int((lo * 10 + 5) / 100 * 20))
                bar[pos] = '|'
                mark = '/%d' % th
            parts.append('%s [%s] %3d%s%s' % ('L2' if side == 0 else 'R2', ''.join(bar), v, mark,
                                              ' 二段' if (self.latch >> side) & 1 else '    '))
        return '  '.join(parts)

    def summary(self):
        out = ['', '=== 汇总 ===']
        for side in (0, 1):
            nm = 'L2' if side == 0 else 'R2'
            th = self.cfg['thresh_l' if side == 0 else 'thresh_r']
            md = mode_name(self.cfg, side)
            fx = ('%s（起始档 %d 起，力度 %d）' % (kind_name(self.cfg, side), s2t_zone(th),
                                              self.cfg['str_l' if side == 0 else 'str_r'])
                  if fx_on(self.cfg, side) else '关闭')
            if not th:
                out.append('%s（%s模式）：起始位置 0 —— 这一侧不会触发，也不会造效果' % (nm, md))
                continue
            what = '二段触发点' if md == '两段' else '起始位置'
            out.append('%s（%s模式，效果 %s）：%s %d±%d，行程见过 %d..%d；过 %d 次 / 松回 %d 次；过它时的行程 %s'
                       % (nm, md, fx, what, th, self.cfg['hyst'], self.travel[side],
                          self.max_seen[side], self.cross_up[side], self.cross_dn[side],
                          self.up_value[side] if self.up_value[side] is not None else '（没过过）'))
            if md == '两段' and self.cross_up[side] and self.out_while_on[side] == 0:
                out.append('    [!] 过二段触发点期间一次键/鼠标/媒体都没看到'
                           ' —— 检查这一侧的二段在网页上到底配了输出没有')
            if md == '普通' and self.cross_up[side]:
                out.append('    （普通模式：这里只是扳机手感，本来就不该出键 —— 想触发第二段就去右栏切「两段模式」）')
        if self.out_while_off:
            out.append('[!] 有没到位却收到键/鼠标的帧 %d 次 —— 那多半是别的映射' % self.out_while_off)
        if self.frames == 0:
            out.append('[!] 一帧有效报文都没读到：手柄没连（0x10 还是全 0）')
        return out


def run_sim():
    """不用设备：合成「慢慢压到底、再松开」，两种模式各跑一遍 —— 检查位置判定、输出归属、
    以及**措辞**（两段模式说「★ 过二段触发点」，普通模式说「· 过卡点位置」）。"""
    def one(mode_bits):
        # 12 字节（R2.6）：[0]左起始 [1]右起始 [2]迟滞 [3]效果位 [4]力度 [5]类型
        # [6][7]结束位置 [8][9]频率 [10]两段模式位
        cfg = parse_s2t(bytes([200, 200, 8, 0x01, 0x66, 0x05, 0, 0, 0, 0, mode_bits, 0]))
        an = Analyzer(cfg)
        lines = []
        t = 0.0
        # 合成帧里「这一刻该不该有那个键」用一份**独立**的锁存算（不能调 an.edge() ——
        # 那是 Analyzer 的状态机，先调一次会把它的迟滞状态推进一步、后面的判定就错位了）
        sim_latch = 0

        def sim_on(v):
            nonlocal sim_latch
            if v >= 200 + 8:
                sim_latch |= 1
            elif v <= 200 - 8:
                sim_latch &= ~1
            return bool(sim_latch)

        def frame(l2, r2, keys=()):
            raw = bytearray(63)
            raw[0] = raw[1] = raw[2] = raw[3] = 0x80
            raw[4], raw[5] = l2, r2
            raw[7] = 0x08
            st = bytearray(64)
            if keys:
                for i, k in enumerate(keys[:6]):
                    st[14 + i] = k
            return bytes(raw), bytes(st)

        for v in range(0, 256, 5):                    # 压到底
            raw, st = frame(v, 0, keys=(0x1A,) if sim_on(v) else ())
            lines += an.feed(raw, st, t)
            t += 0.02
        for v in range(255, -1, -5):                  # 松开
            raw, st = frame(v, 0, keys=(0x1A,) if sim_on(v) else ())
            lines += an.feed(raw, st, t)
            t += 0.02
        raw, st = frame(10, 0, keys=(0x1A,))          # 没到位就出键（错误情况，要被抓到）
        lines += an.feed(raw, st, t)
        return an, lines

    bad = []
    an, lines = one(0x03)                             # 两侧都「两段模式」
    if an.cross_up[0] != 1 or an.cross_dn[0] != 1:
        bad.append('过位置/松回的次数不对：%d / %d' % (an.cross_up[0], an.cross_dn[0]))
    if an.up_value[0] != 210:            # 位置 200 + 迟滞 8 = 208 → 步长 5 的第一个 ≥208 是 210
        bad.append('过位置时的行程不对：%s（应该 210）' % an.up_value[0])
    if an.cross_up[1] != 0:
        bad.append('R2 没碰过却报了触发')
    if an.out_while_off != 1:
        bad.append('「没到位就出键」没被抓到：out_while_off=%d（应该 1）' % an.out_while_off)
    if an.out_while_on[0] == 0:
        bad.append('过位置期间出的键没被算进 out_while_on')
    if not any('过二段触发点' in ln and '★' in ln for ln in lines):
        bad.append('两段模式没打出「★ 过二段触发点」措辞')
    print('--- 合成行程（模拟）：两段模式 ---')
    for ln in ([lines[0]] + [l for l in lines if ('★' in l or '输出：' in l or '松回' in l)][:6]):
        print(ln)
    for ln in an.summary():
        print(ln)

    an2, lines2 = one(0x00)                           # 两侧都「普通模式」
    if not any('过起始位置' in ln and '普通模式' in ln for ln in lines2):
        bad.append('普通模式没打出「过起始位置 + 普通模式」措辞')
    if an2.cross_up[0] != 1:
        bad.append('普通模式下的位置判定没生效（应该照样能过）')
    print('\n--- 合成行程（模拟）：普通模式（同一根滑条，只当卡点位置）---')
    for ln in ([lines2[0]] + [l for l in lines2 if ('·' in l or '松回' in l)][:4]):
        print(ln)
    for ln in an2.summary():
        print(ln)

    if bad:
        print('\n自检失败：')
        for b in bad:
            print('  - ' + b)
        return 1
    print('\n自检通过：位置判定（208 起过 / 192 以下松）、两段与普通两种措辞、输出归属、汇总统计都对')
    return 0


def main():
    if len(sys.argv) > 1 and sys.argv[1] == '--sim':
        return run_sim()

    secs = float(sys.argv[1]) if len(sys.argv) > 1 else 60.0
    h = diag.open_dev()
    if h is None:
        print('[!] 接收器不在（或没答 0xFC）')
        return 1
    try:
        ver = diag.rpc_read(h, REG_VER, 0, 40)
        if ver:
            tag = bytes(ver[20:36]).split(b'\0')[0].decode('utf-8', 'replace')
            print('固件版本：%s' % (tag or '（0x15 区没有版本串）'))
        b = diag.rpc_read(h, REG_S2T, 0, S2T_LEN)
        if not b:
            print('[!] 这台固件没有 0x0b（扳机）区 —— R2.4 及以前都是这样。')
            print('    先刷 R2.5 以上（网页「固件刷写」页，版本应显示 BL616-DongleRE R2.5/R2.6…），再跑这个脚本。')
            return 1
        cfg = parse_s2t(b)

        def side_txt(side):
            th = cfg['thresh_l'] if side == 0 else cfg['thresh_r']
            if not th:
                return '关'
            fx = kind_name(cfg, side)
            s = cfg['str_l'] if side == 0 else cfg['str_r']
            if not fx_on(cfg, side):
                return '%d（档 %d）·效果关闭' % (th, s2t_zone(th))
            lo, hi, z, clamped = eff_zone(cfg, side)
            rng = '档 %d' % lo if lo == hi else '档 %d-%d' % (lo, hi)
            extra = ''
            if fx == '扳机':
                extra = '，力度 %d' % s
            elif fx == '自动扳机':
                extra = '，力度 %d，%d Hz' % (s, cfg['freq_l' if side == 0 else 'freq_r'])
            else:
                extra = '，力度 %d' % s
            note = '（起始档 %d 被硬件约束夹到 %d）' % (z, lo) if clamped else ''
            return '%d（%s：%s＝%d~%d%%%s）%s' % (th, fx, rng, lo * 10, (hi + 1) * 10, extra, note)

        print('L2：%s' % side_txt(0))
        print('R2：%s' % side_txt(1))
        print('迟滞 %d   模式：L2=%s / R2=%s（普通模式＝这些效果只是手感、不触发第二段；'
              '两段模式＝过起始位置就是二段按下）' % (cfg['hyst'], mode_name(cfg, 0), mode_name(cfg, 1)))
        if not cfg['thresh_l'] and not cfg['thresh_r']:
            print('[!] 两侧起始位置都是 0（关）——先在网页右栏「扳机逻辑」里选个效果、把起始位置拉起来再测。')
        print('盯 %.0f 秒：请把 L2 / R2 **慢慢**压到底再松开（别碰其它键）……' % secs)
        an = Analyzer(cfg)
    except Exception as e:
        print('[!] 读配置失败：%s' % e)
        h.close()
        return 1

    t0 = time.time()
    last_status = 0.0
    last_state = None
    try:
        while time.time() - t0 < secs:
            t = time.time() - t0
            raw = diag.rpc_read(h, REG_TLM_RAW, 0, 63, tries=30)
            st = diag.rpc_read(h, REG_TLM_STATE, 0, TLM_STATE_LEN, tries=30)
            for ln in an.feed(raw, st, t):
                print(ln)
            if st and len(st) > 47 and st[47] != last_state:
                last_state = st[47]
                print('%6.2fs  [设备] %s' % (t, s2t_state_txt(last_state)))
            if t - last_status >= 0.5:
                last_status = t
                print('        ' + an.status(raw) +
                      ('   [%s]' % s2t_state_txt(last_state) if last_state is not None else ''))
            time.sleep(0.01)
    except KeyboardInterrupt:
        print('\n（手动中断）')
    finally:
        try:
            h.close()
        except Exception:
            pass
    for ln in an.summary():
        print(ln)
    return 0


if __name__ == '__main__':
    sys.exit(main())
