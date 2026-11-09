# -*- coding: utf-8 -*-
"""R1.6 文档：CHANGELOG / HANDOFF / 记忆。"""
import io
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _patchlib import P

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')

p = P(os.path.join(ROOT, 'firmware', 'CHANGELOG.md'))
p.rep("""## R1.5 — 2026-10-14
""", """## R1.6 — 2026-10-14

### Changed
- 「接收器当主」改成默认打开：每次手柄连上满 3 秒后接收器申请当主，链路监督超时由接收器定为 10 秒（R1.5 实测切换不掉线、帧率更高、震动和声音正常、48 分钟零停顿）。`tools/link_tune.py 0` 可临时关掉（拔插恢复打开）
- 普通震动 / 灯光 / 扳机效果收到就转发：原作者在手柄喇叭有声音时改成每 24 ms 攒一批再发（平均晚 12 ms、最多 24 ms），现在不再扣着；发出去的包数不变

### Added
- 性能统计加输出报告：每秒多少份、「USB 收到 → 交给蓝牙」的平均 / 最大延迟、排队丢弃数

---

## R1.5 — 2026-10-14
""")
p.save()

p = P(os.path.join(ROOT, 'firmware', 'HANDOFF.md'))
p.rep("""### R1.5（2026-10-14 18:4x，起因：18:27 R1.4 上又掉了，而且是手柄的 5 s 监督超时判的）
""", """### R1.6（2026-10-14 19:5x，用户：「你直接出R1.6吧；改成收到就转发会影响性能吗 不影响就一起改」）

- **「接收器当主」默认开**：`bt_hid_host.c` 的 `s_role_req` 初值 1；新增 `ROLE_REQ_DELAY_MS` 3000 —— `link_params()`（连上后第一次链路诊断）记 `s_conn_ms`，满 3 s 后的那次链路诊断（每 2 s 一次，所以实际是连上后约 4~6 s）才 `link_request_master()`。R1.5 是在连接稳定后切的，这里也别挤在连接建立 / LED primer 那一下。`0xF6/0x11 0` 仍可在 RAM 里关掉（拔插恢复开），`tools/link_tune.py 0`。
- **输出报告收到就转发**：删掉原作者 `state_mgr_is_spk_active()` 分支（喇叭有声音时 `while` 抽干队列再 `vTaskDelay(24)`）；现在不管有没有声音都走 `xQueueReceive(output_queue, 24 ms)` 那条（收到一份转一份，顺带让隐身模式的 primer 计数在有声音时也生效）。**性能**：发出去的 0x31 包数不变（每份 USB 输出报告一个；蓝牙侧照旧 app_tx FIFO 一次只飞一个），只是不再成批；bt 任务每份报告醒一次，一次几微秒。唯一的差别是原来队列只有 5 格、24 ms 才抽一次，电脑发得比 ~208 份/s 还快时会丢掉旧的；现在不丢 ⇒ 用 PERF 的「输出报告 n/s」确认实际速率。
- 队列元素改成 `usb_out_msg_t { t_us; data[64]; }`，USB ISR（`on_usb_output`）记收到时刻并 `portYIELD_FROM_ISR`（同 `ds5_usb_audio.c`）；bt 任务转发后 `perf_out_fwd(延迟)`。PERF 152 → **160**：152 out_win、154 平均延迟 µs、156 最大延迟 µs、158 排队丢弃累计；`perf.c` 加 `_Static_assert(sizeof(perf_blob_t) == PERF_BLOB_SIZE)`。延迟只量「USB 收到 → 交给蓝牙 FIFO」，不含空中。bt 任务优先级 4，低于 usb(6) / audio(5)：Opus 编码正在跑时最多再等一次编码（5~8 ms），所以「最大延迟」在放声音时会有几毫秒，平均应在 1 ms 内。
- 包：`_archive/R1.6_sdk232_938b8122`（787184 B）。

### R1.5（2026-10-14 18:4x，起因：18:27 R1.4 上又掉了，而且是手柄的 5 s 监督超时判的）
""")
p.save()

mem = os.path.join(os.path.expanduser('~'), '.claude', 'projects', 'E--BL616-Project-DongleRE', 'memory')

def edit(name, old, new):
    path = os.path.join(mem, name)
    s = io.open(path, encoding='utf-8').read()
    assert s.count(old) == 1, (name, old[:40])
    io.open(path, 'w', encoding='utf-8').write(s.replace(old, new))

edit('bt-link-master-role.md',
     'description: 静默掉线的根因链与当前对策（R1.4/R1.5）：接收器原本是蓝牙从、5 s 监督超时由手柄定；R1.5「接收器当主」试验开关（RAM，拔插即失）成功且 48 分钟零停顿，下一步做成默认',
     'description: 静默掉线的根因链与当前对策（R1.4~R1.6）：接收器原本是蓝牙从、5 s 监督超时由手柄定；R1.5 试验「接收器当主」成功（48 分钟零停顿），R1.6 起默认开')
edit('bt-link-master-role.md',
     '**How to apply:** 开关只在接收器 RAM 里，拔插/断电后要再跑 `python -X utf8 tools/link_tune.py 1`（新会话开始先确认）。如果持续稳定，下一版（R1.6）把「连上后申请当主」做成默认或配置项。',
     '**How to apply:** R1.6（2026-10-14 晚）起固件默认申请当主（连上满 3 s 后），不用再手动开；`tools/link_tune.py 0` 只在想对照「手柄当主」时临时关（RAM，拔插恢复开）。看 DIAG 的 ROLE_SW / 角色变化 / LINK_PARAM 确认每次连上都切成功了；若某个手柄拒绝切换，它会留在从、5 s 监督超时，停顿 >5 s 仍会断。')
edit('dongle-test-workflow.md',
     '4. 「接收器当主」开关（R1.5 起，RAM）：`python -X utf8 link_tune.py 1`。',
     '4. 「接收器当主」：R1.6 起默认开；`python -X utf8 link_tune.py 0|1` 临时关 / 开（RAM，拔插恢复开）。')
edit('MEMORY.md',
     '- [掉线根因与「接收器当主」对策（R1.4/R1.5）](bt-link-master-role.md) — 接收器原是从、5 s 监督超时归手柄；link_tune.py 1 切主成功、48 分钟零停顿；开关在 RAM，拔插要重开；下一步做成默认',
     '- [掉线根因与「接收器当主」对策（R1.4~R1.6）](bt-link-master-role.md) — 接收器原是从、5 s 监督超时归手柄；R1.5 试验切主成功、48 分钟零停顿；R1.6 起默认当主（link_tune.py 0 临时关）')
edit('MEMORY.md',
     '— 2026-10-14 用户决定；设备上现在是 R1.5 sdk232（93124f85）',
     '— 2026-10-14 用户决定；R1.6 sdk232（938b8122）= 默认当主 + 震动收到就转发')
with io.open(os.path.join(mem, 'fw-version-scheme-r.md'), 'a', encoding='utf-8') as f:
    f.write('\n**R1.6（2026-10-14 19:5x，包 `_archive/R1.6_sdk232_938b8122`）**：「接收器当主」默认开（连上满 3 s 申请）；删原作者「喇叭有声音时输出报告每 24 ms 攒一批」改为收到就转发（USB ISR portYIELD_FROM_ISR 叫醒 bt 任务）；PERF 160 B（输出报告数 / 转发延迟均值与最大 / 排队丢弃）。\n')
print('R1.6 DOCS DONE')
