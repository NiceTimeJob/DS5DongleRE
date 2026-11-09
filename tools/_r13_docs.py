# -*- coding: utf-8 -*-
"""R1.3 文档 + 更正 15:41 分析里"链路是活的"的错误推断。"""
import io
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _patchlib import P

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')

p = P(os.path.join(ROOT, 'firmware', 'CHANGELOG.md'))
p.rep("""## R1.2 — 2026-10-14
""", """## R1.3 — 2026-10-14

### Changed
- 陀螺仪删除自动零偏校准：平时没有任何每帧计算；零偏只来自测试页「静置校准」（手柄放平别动 3 秒），期间手柄动了就判失败不采用
- 静置校准的结果存进接收器（gcal.bin，和按键映射同一个文件系统），开机读回，断开重连不丢

### Added
- 链路诊断：每 2 秒读 RSSI、链路质量、AFH 在用信道数，进性能统计
- 输入停顿 ≥1 秒记一条诊断事件（带当时的链路诊断），恢复时记停了多久

### Fixed
- 读 RSSI 时连接对象可能被并发清空导致的空指针（与 R1 修的拆链竞态同类）

---

## R1.2 — 2026-10-14
""")
p.save()

p = P(os.path.join(ROOT, 'firmware', 'HANDOFF.md'))
p.rep("""- **关键事实**：WD_CONN 后我们发的 HCI_Disconnect 在 **160 ms** 内就以 0x16 完成 ⇒ 当时链路层是活的（LMP detach 被手柄确认），只是 HID 输入停了 >3 s。看门狗这一刀等于告诉手柄「主机结束会话」，DualSense 收到后自己关机 ⇒ 这就是掉线后手柄从不自己回来的原因。
""", """- ~~关键事实：HCI_Disconnect 160 ms 完成 ⇒ 链路是活的~~ **（16:05 更正，这个推断是错的）**：规范里发起方发 LMP_detach 后，收到对方基带确认等 3×Tpoll、**收不到确认等 6×Tpoll**；Tpoll 默认 40 slot = 25 ms ⇒ 6×Tpoll = 150 ms，正好对上 160~176 ms。所以这更像是手柄**没有**应答、链路当时已经断了或极差。"手柄因为收到我们的 detach 才关机"也就不成立了。
- **16:00:40 第二次（R1.2，用户 15:54:57 按 PS 重连后）**：重连后手柄送达的帧率只有 150~250/s（之前 600~780），btble/recv CPU 同比例下降（不是我们处理慢）；16:00:31 还有 254/s，16:00:41 上一周期只剩 2 帧/s → 停 → 3 s 看门狗 → ACL_DOWN 0x16（176 ms，同上）。RSSI 读数全程 0（在黄金区间）。⇒ **链路本身在恶化**（重传/丢包），不是距离；干扰或控制器闭源库都可能。R1.3 加了 RSSI/链路质量/AFH 在用信道数每 2 s 采样和停顿事件，下次能看清。
""")
p.rep("""### R1.2 上的第一次掉线（2026-10-14 15:41:51，用户刚回家、手柄刚连上 7 分钟）
""", """### R1.3（2026-10-14 16:1x，用户：「彻底去掉自动校准 静止校准你存到配置文件里 就和手柄的按键映射那样的方式一样 希望能把性能节省出来」）

- `gyro.c/h` 整份重写：删 Stillness 自动校准（R1.2 实测 130~140 µs/次）与全部融合状态；`gyro_update()` 只在静置校准期间被调用（3 s 累加均值，任一轴 最大−最小 >4 °/s 或帧数 <50 ⇒ `GYRO_FLAG_CAL_FAILED` 不采用，|零偏| ≥20 °/s 视为读坏）；`gyro_read_hv()` 不再依赖 s_primed；`gyro_reset()`（断开时）只取消进行中的校准、不清零偏。新接口 `gyro_calibrating / gyro_set_bias / gyro_take_new_bias`。TLM_STATE 布局不变：confidence 255 = 已有零偏，flags 0x04 = 已校准、0x10 = 上次失败。
- 持久化：`ds5map.c` 的 `gcal.bin`（magic "GCAL"、ver 1、3×float、crc16），和 cfg.bin 同一个 LittleFS；**不并进 cfg.bin**，否则要升 `DS5MAP_CFG_VER`、用户映射会被清回默认。`ds5map_process` 在校准期间每帧 `gyro_update` → 成功则 `g_gcal_dirty` → `ds5map_deferred`（bt 任务）`gcal_save()`；`ds5map_init` 在 `ds5map_load()` 后 `gcal_load()`。
- 引擎里 R1.2 的 1/4 降频融合、`gyro_track`、`GYRO_PROBE_MS` 全删；"什么都没配"捷径改看 `!g_gyro_on`；`g_gyro_probe` 现在只表示"下一帧刷新一次陀螺仪遥测"。
- 链路诊断：`bt_hid_host_read_link_stats()`（bt 任务每 2 s：Read_RSSI + Read_Link_Quality 0x05/0x0003 + Read_AFH_Channel_Map 0x05/0x0006）→ PERF 140..143（rssi / link_quality / afh_used / afh_mode，PERF 144 B）；`DIAG_EV_STALL`(36，a1 = 打包的链路诊断) / `DIAG_EV_STALL_END`(37，a1 = 停了多少 ms)。顺手修 `bt_hid_host_read_rssi()` 检查完再读 `hid_ctx.conn->handle` 的竞态。
- 网页：测试页陀螺仪块改成「已校准 / 未校准 / 上次校准失败」，地图页陀螺仪提示改成"零偏靠静置校准、存在接收器里"。
- 两个包：`_archive/R1.3_sdk2328_*`（现用 SDK）与 `_archive/R1.3_sdk232_*`（上游 2.3.32 + 新蓝牙控制器库 1.6.199，0xF8 带 " sdk232"），功能完全一样。

### R1.2 上的第一次掉线（2026-10-14 15:41:51，用户刚回家、手柄刚连上 7 分钟）
""")
p.save()

mem = os.path.join(os.path.expanduser('~'), '.claude', 'projects', 'E--BL616-Project-DongleRE', 'memory')
with io.open(os.path.join(mem, 'random-reset-incident-2026-10-12.md'), 'a', encoding='utf-8') as f:
    f.write("\n**16:05 更正**：上面\"HCI_Disconnect 160 ms 完成 ⇒ 链路是活的\"是错的。发起方发 LMP_detach 后收不到确认要等 6×Tpoll（默认 25 ms ⇒ 150 ms），160~176 ms 正好对上 ⇒ 更像手柄没应答、链路已断/极差。16:00:40 第二次：重连后送达帧率只有 150~250/s（正常 600~780），然后掉到 2 帧/s → 停 → 看门狗；RSSI 全程正常 ⇒ 链路本身在恶化（重传/丢包）。R1.3 加了 RSSI/链路质量/AFH 信道数每 2 s 采样 + STALL/STALL_END 事件。\n")
with io.open(os.path.join(mem, 'fw-version-scheme-r.md'), 'a', encoding='utf-8') as f:
    f.write("\n**R1.3（2026-10-14 16:1x）**：陀螺仪删自动校准（用户要求），静置校准存 LittleFS gcal.bin（不并进 cfg.bin 以免升 DS5MAP_CFG_VER 清映射）；链路诊断 PERF 144 B + DIAG 36/37。两个包 sdk2328 / sdk232（新蓝牙库 1.6.199）。\n")
print('R1.3 DOCS DONE')
