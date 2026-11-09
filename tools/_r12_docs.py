# -*- coding: utf-8 -*-
"""R1.2 文档：CHANGELOG / HANDOFF / 记忆。"""
import io
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _patchlib import P

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')

p = P(os.path.join(ROOT, 'firmware', 'CHANGELOG.md'))
p.rep("""## R1.1 — 2026-10-14
""", """## R1.2 — 2026-10-14

### Changed
- 陀螺仪开着但没生效（按住/开关模式没触发）时，映射引擎走快速路径复用上一帧结果；陀螺仪融合降到 1/4 频率只维持零偏校准，一按启用键立刻回到每帧完整计算
- 性能统计加「快速路径帧数」「陀螺仪融合次数」

### Fixed
- 「原键 + 开关 + 连发」锁存后松开按键时，快速路径可能把连发输出冻住

---

## R1.1 — 2026-10-14
""")
p.save()

p = P(os.path.join(ROOT, 'firmware', 'HANDOFF.md'))
p.rep("""### R1.1（2026-10-14 下午，用户要求）：回报率固定 500 Hz + 按回报率处理 + 每帧分段计时
""", """### R1.2（2026-10-14 下午，用户出门前要求「把 2 和 3 都做了」）

- 优化清单：1 分段计时（R1.1）、2 陀螺仪没生效时走快速路径（**本版**）、3 按回报率处理（R1.1 已做；R1.1 从未运行就被 R1.2 覆盖，所以随本版一起上）、4 热点代码搬 TCM（**没做**：要先看第 1 项的数字，且 TCM 挤占的是系统堆，蓝牙栈靠它分配，盲搬可能伤稳定性）。
- `ds5map_process` 快速路径去掉 `!gyro_track` 条件：启用键状态只由按键位决定，按键位没变它就没变；`g_fast_ok` 本来就要求上一帧 `!g_gyro_live`，「常开」模式永远走完整路径。快速路径里每 `GYRO_IDLE_DECIM`=4 帧跑一次 `gyro_update`+`tlm_gyro_pack`（引擎 ~500 Hz → 融合 ~125 Hz）。
- `tick_needed()` 修旧漏洞：原生连发 `nturbo_run` 不管键按没按着都要计时（「原键 + 开关 + 连发」锁存后松手仍在翻转），以前只看按着的键 → 快速路径会冻住输出。S36/S37 就有，只是用户开着陀螺仪、快速路径从来没进去过所以没暴露。
- PERF 136 → 140 B：`fast_win`（每秒走快速路径的帧数）、`gyro_win`（每秒融合次数）；diag.py 性能行末尾「快路径N 陀螺仪M次」。**预期**：不按 L2 时快路径 ≈ 处理帧数、陀螺仪 ≈ 125；按住 L2 时快路径 0、陀螺仪 ≈ 处理帧数。

### R1.1（2026-10-14 下午，用户要求）：回报率固定 500 Hz + 按回报率处理 + 每帧分段计时
""")
p.save()

mem = os.path.join(os.path.expanduser('~'), '.claude', 'projects', 'E--BL616-Project-DongleRE', 'memory', 'fw-version-scheme-r.md')
with io.open(mem, 'a', encoding='utf-8') as f:
    f.write("\n**R1.2（2026-10-14 下午，用户出门前要求「2 和 3」）**：第 2 项 = 陀螺仪没生效时走快速路径（融合 1/4 频率维持零偏）+ 修 tick_needed 漏掉锁存原生连发；第 3 项 R1.1 已做（R1.1 未运行即被覆盖）；第 4 项 TCM 未做（等分段计时数字，且 TCM 挤占系统堆）。PERF 140 B（fast_win / gyro_win）。\n")
print('R1.2 DOCS DONE')
