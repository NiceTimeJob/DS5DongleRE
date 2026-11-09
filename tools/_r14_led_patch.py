# -*- coding: utf-8 -*-
"""R1.14（2026-10-15，用户要求）：「禁用 LED」= 除对频快闪外全灭。
原来是「连上/等待/对频都亮 1 分钟才灭、电量告警照闪」；现在开着就立刻灭、电量告警也不闪，
只有长按按钮进对频（pairing_mode）时蓝灯快闪。板上红灯是电源灯、硬件直连，固件管不到
（board_config.h：power LED hardwired）。顺带删掉 1 分钟定时器那套（conn_led_off /
conn_led_start_us / LED_DISABLE_OFF_US / led_status_can_auto_off）。"""
import sys
sys.path.insert(0, 'E:/BL616_Project/DongleRE/tools')
from _patchlib import P

SRC = 'E:/BL616_Project/DongleRE/firmware/src/'
WEB = 'E:/BL616_Project/DongleRE/webgui/'
FW = 'E:/BL616_Project/DongleRE/firmware/'


def L(*lines):
    return '\n'.join(lines) + '\n'


# ---------------- main.c ----------------
m = P(SRC + 'main.c')

# 状态灯唯一出口：禁用时除对频外一律 LED_OFF
m.rep(L(' *   otherwise       → ~1 Hz blink (waiting for controller)',
        ' * Also registers the resting pattern so one-shot event flashes return to it. */'),
      L(' *   otherwise       → ~1 Hz blink (waiting for controller)',
        ' * R1.14（用户要求）：「禁用 LED」开着时除对频快闪外一律灭——不再是亮 1 分钟才灭，',
        ' * 电量告警也不闪。板上另一颗红灯是电源灯、硬件直连，固件管不到。',
        ' * Also registers the resting pattern so one-shot event flashes return to it. */'))
m.rep(L('    else if (pairing_mode)',
        '        p = LED_PAIRING;',
        '',
        '    led_status_set_default(p);'),
      L('    else if (pairing_mode)',
        '        p = LED_PAIRING;',
        '',
        '    if (config_led_disabled() && !pairing_mode)',
        '        p = LED_OFF;',
        '',
        '    led_status_set_default(p);'))

# 1 分钟定时器的变量
m.rep(L('/* LED auto-off timers */',
        '#define LED_DISABLE_OFF_US   (60ULL * 1000000ULL)  /* 1 minute */',
        'static volatile uint64_t conn_led_start_us = 0;',
        'static volatile bool conn_led_off = false;',
        'static volatile uint64_t handshake_start_us;'),
      L('static volatile uint64_t handshake_start_us;'))

# IDLE：断开时重置定时器
m.rep(L('        if (was_connected) {',
        '            conn_led_start_us = bflb_mtimer_get_time_us();',
        '            conn_led_off = false;',
        '        }',
        '        if (!conn_led_off)',
        '            led_refresh_state();',
        '        if (scan_after_disconnect) {'),
      L('        led_refresh_state();',
        '        if (scan_after_disconnect) {'))
# SCANNING / CONNECTING / L2CAP 三处
m.rep(L('        if (!conn_led_off)',
        '            led_refresh_state();'),
      L('        led_refresh_state();'), cnt=3)
# CONNECTED
m.rep(L('        conn_led_start_us = bflb_mtimer_get_time_us();',
        '        conn_led_off = false;',
        '        pairing_mode = false;',
        '        led_refresh_state();'),
      L('        pairing_mode = false;',
        '        led_refresh_state();'))
# bt_task 开头
m.rep(L('    prev_led_disabled = config_led_disabled();',
        '    conn_led_start_us = bflb_mtimer_get_time_us();'),
      L('    prev_led_disabled = config_led_disabled();'))
# 长按进对频
m.rep(L('                        pairing_mode = true;',
        '                        conn_led_off = false;',
        '                        scan_fail_count = 0;'),
      L('                        pairing_mode = true;',
        '                        scan_fail_count = 0;'))
# 实时响应开关 + 1 分钟自动灭
m.rep(L('        /* React to disable_led config toggle in real-time */',
        '        bool cur_led_disabled = config_led_disabled();',
        '        if (cur_led_disabled != prev_led_disabled) {',
        '            prev_led_disabled = cur_led_disabled;',
        '            if (cur_led_disabled) {',
        '                conn_led_start_us = bflb_mtimer_get_time_us();',
        '                conn_led_off = false;',
        '                LOG_DBG("[LED-DBG] toggle ON: start 1min timer, conn=%d\\n",',
        '                       ds5_connected);',
        '            } else {',
        '                conn_led_off = false;',
        '                if (ds5_connected && battery_low)',
        '                    led_status_set(LED_BLINK_BATTERY);',
        '                else',
        '                    led_refresh_state();',
        '                LOG_DBG("[LED-DBG] toggle OFF: restored, conn=%d\\n",',
        '                       ds5_connected);',
        '            }',
        '        }',
        '',
        '        /* 1-min auto-off: applies to any pattern in the whitelist */',
        '        if (config_led_disabled() && !conn_led_off &&',
        '            led_status_can_auto_off()) {',
        '            uint64_t now_us = bflb_mtimer_get_time_us();',
        '            uint64_t elapsed = now_us - conn_led_start_us;',
        '            if (elapsed >= LED_DISABLE_OFF_US) {',
        '                led_status_set(LED_OFF);',
        '                conn_led_off = true;',
        '                LOG_DBG("[LED-DBG] auto-off fired: elapsed=%llus\\n",',
        '                       (unsigned long long)(elapsed / 1000000ULL));',
        '            }',
        '        }'),
      L('        /* React to disable_led config toggle in real-time.',
        '         * R1.14：开 = 立刻全灭（对频快闪除外），关 = 恢复状态灯；电量告警只在没禁用时闪。 */',
        '        bool cur_led_disabled = config_led_disabled();',
        '        if (cur_led_disabled != prev_led_disabled) {',
        '            prev_led_disabled = cur_led_disabled;',
        '            if (!cur_led_disabled && ds5_connected && battery_low)',
        '                led_status_set(LED_BLINK_BATTERY);',
        '            else',
        '                led_refresh_state();',
        '            LOG_DBG("[LED-DBG] disable_led=%d conn=%d\\n", cur_led_disabled, ds5_connected);',
        '        }'))
# 电量告警
m.rep(L('                if (is_low && !battery_low) {',
        '                    battery_low = true;',
        '                    led_status_set(LED_BLINK_BATTERY);',
        '                    LOG_WRN("[MAIN] Battery critical (%d%%)\\n", pct * 10);',
        '                } else if (!is_low && battery_low) {',
        '                    battery_low = false;',
        '                    if (config_led_disabled() && conn_led_off)',
        '                        led_status_set(LED_OFF);',
        '                    else',
        '                        led_refresh_state();',
        '                }'),
      L('                if (is_low && !battery_low) {',
        '                    battery_low = true;',
        '                    if (!config_led_disabled())   /* R1.14：禁用 LED 时电量告警也不闪 */',
        '                        led_status_set(LED_BLINK_BATTERY);',
        '                    LOG_WRN("[MAIN] Battery critical (%d%%)\\n", pct * 10);',
        '                } else if (!is_low && battery_low) {',
        '                    battery_low = false;',
        '                    led_refresh_state();',
        '                }'))
assert 'conn_led_off' not in m.s and 'conn_led_start_us' not in m.s and 'LED_DISABLE_OFF_US' not in m.s
m.save()

# ---------------- led_status.c / .h：删掉只给 1 分钟定时器用的白名单 ----------------
h = P(SRC + 'led_status.h')
h.rep(L('bool led_status_can_auto_off(void);'), '')
h.save()
c = P(SRC + 'led_status.c')
c.rep(L('bool led_status_can_auto_off(void)',
        '{',
        '    return current_pattern == LED_GREEN_SOLID ||',
        '           current_pattern == LED_PURPLE_BLINK_SLOW ||',
        '           current_pattern == LED_PURPLE_BLINK_FAST;',
        '}',
        ''), '')
c.save()

# ---------------- version.h ----------------
v = P(SRC + 'version.h')
v.rep('#define FW_VERSION      "R1.13"', '#define FW_VERSION      "R1.14"')
v.save()

# ---------------- build_variant.sh：产品名 R1.7 起是 BL616-DongleRE，版本串回显要认 ----------------
b = P(FW + 'build_variant.sh')
b.rep('grep -aoE "LCT616-DongleRE R[0-9]+', 'grep -aoE "(LCT616|BL616)-DongleRE R[0-9]+')
b.save()

# ---------------- 网页 hint ----------------
w = P(WEB + 'js/page-config.js')
w.rep("hint: '接收器状态灯：连上 / 等待 / 对频都只亮 1 分钟就熄灭，电量告警仍会闪'",
      "hint: '开 = 接收器蓝色状态灯全灭（连上 / 等待 / 断开都不亮，电量告警也不闪），只有长按按钮进对频时蓝灯快闪；红色电源灯是硬件直连，关不掉'")
w.save()

# ---------------- CHANGELOG（面向玩家，名词短语，不以「修复/新增」开头） ----------------
cl = P(FW + 'CHANGELOG.md')
cl.rep(L('## R1.13 — 2026-10-15'),
       L('## R1.14 — 2026-10-15',
         '',
         '### Changed',
         '- 「禁用 LED」改为真正全灭：开启后接收器的蓝色状态灯在连接、等待、断开时都不亮（原来是亮 1 分钟才熄灭，电量告警还会闪），只有长按按钮进入对频时才蓝灯快闪。红色电源灯是硬件直连，不受这个开关影响',
         '',
         '---',
         '',
         '## R1.13 — 2026-10-15'))
cl.save()
print('R1.14 patch done')
