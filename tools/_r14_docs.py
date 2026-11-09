# -*- coding: utf-8 -*-
"""R1.14 文档：HANDOFF「当前状态」加 R1.14 段（R1.13 降为普通段）、R1.6 段补夜测结论；记忆同步。"""
import sys
sys.path.insert(0, 'E:/BL616_Project/DongleRE/tools')
from _patchlib import P

H = 'E:/BL616_Project/DongleRE/firmware/HANDOFF.md'
MEM = 'C:/Users/yiqi/.claude/projects/E--BL616-Project-DongleRE/memory/'

h = P(H)
h.rep('**最新固件：`R1.13`**（2026-10-15 03:19 编译',
      '**最新固件：`R1.14`**（2026-10-15 11:05 公司机器编译，两份包：`_archive/R1.14_sdk232_1d46b668`'
      '（787184 B，0xF8 = `BL616-DongleRE R1.14 sdk232`，上游 2.3.32 + 控制器库 1.6.199，接 R1.6 夜测那条线）与 '
      '`_archive/R1.14_sdk2328_cfa62cfb`（787136 B，0xF8 = `BL616-DongleRE R1.14`，E 盘 3.18 fork）；'
      '**都未烧录、网页包未重出，等用户定哪条 SDK 线是发布线**）。内容 = R1.13 + **「禁用 LED」改成真全灭**'
      '（用户 2026-10-15 要求：「把所有的 led 灯都关掉……除非在对频状态下蓝灯快闪 其他状态下全部把 led 关掉」）：'
      '`led_refresh_state()` 里 `config_led_disabled() && !pairing_mode` ⇒ `LED_OFF`，它是状态灯唯一出口；'
      '删掉原作者的 1 分钟定时器整套（`LED_DISABLE_OFF_US` / `conn_led_start_us` / `conn_led_off` / '
      '`led_status_can_auto_off()`，`if (!conn_led_off)` 守卫全拆）；开关实时切换只剩「恢复 or 灭」；'
      '电量告警 `LED_BLINK_BATTERY` 只在没禁用时点亮。**红灯动不了**：LCTech 616 只有 GPIO27 一颗蓝灯受控'
      '（`board_config.h`：power LED hardwired, not controllable），红灯是电源灯，网页 hint 已写明。'
      '网页 r47：只改 `page-config.js` 的 disable_led hint；`test_config_layout.js` / `test_web_load.js` 通过。'
      '另：`build_variant.sh` 的版本串回显正则补上 `BL616-` 前缀（R1.7 改名后一直抠空）。'
      '**观察**：家里 R1.7~R1.13 的 `build/CMakeCache.txt` 显示 SDK 是 `F:/BL616/bouffalo_sdk`，R1.13 app 774848 B，'
      '比公司两条线都小 12 KB ⇒ 家里那棵 SDK 树和公司两棵都不一样，哪条是发布线待用户定。'
      '补丁脚本 `tools/_r14_led_patch.py`。\n\n'
      '**R1.13**（2026-10-15 03:19 编译')
h.rep('- **实测（20:23:54 烧录，3 次校验；0xF8 = R1.6 sdk232）**：',
      '- **夜测（2026-10-14 20:30 → 10-15 10:50）**：公司这台 R1.6 sdk232 连着 DualSense **14 h 20 min 零掉线、零复位**'
      '（7 个 110 min 告警窗口全「没有断链」+ 02:12–02:25 空档扫日志 + 10:50 核对 DIAG 运行计数 51.95M ms ≈ 14.4 h、启动计数没变）。'
      '压测段只有 20:36–22:22：车入水持续震动（输出报告 94~97/s）+ 音乐；之后空闲在线。震动转发 10 s 窗口均值约 5 ms、最大 47 ms（21:20），'
      '>20 ms 的窗口 96 个，队列丢 3 条 / 约 54 万条；音频 32.7 万块丢 0、发送失败 0；帧率 439~500/s。'
      '手柄电量 19:01 起一直 90%、读「放电」（解码与 Linux hid-playstation 一致，16:04–19:01 读到过充电/满）⇒ 那段时间其实没在充。'
      '原始记录器日志 10-15 10:47 被家里副本整体覆盖冲成 20:33 快照，只剩各窗口汇总。\n'
      '- **实测（20:23:54 烧录，3 次校验；0xF8 = R1.6 sdk232）**：')
h.save()

m = P(MEM + 'fw-version-scheme-r.md')
m.s = m.s.rstrip('\n') + ('\n\n**R1.14（2026-10-15 11:05 公司编译，未烧）**：「禁用 LED」真全灭（只留对频快闪；红灯是电源灯管不到），'
                         '删 1 分钟定时器；两份包 `_archive/R1.14_sdk232_1d46b668`（0xF8 带 sdk232）/ `_archive/R1.14_sdk2328_cfa62cfb`'
                         '（0xF8 = `BL616-DongleRE R1.14`，与家里命名一致）。家里 R1.7~R1.13 用的 SDK 是 F:/BL616/bouffalo_sdk、'
                         'app 比公司两条线小 12 KB ⇒ 三棵 SDK 树，发布线待用户定。网页 r47。\n')
m.save()
print('R1.14 docs done')
