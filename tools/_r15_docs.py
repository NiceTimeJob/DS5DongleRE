# -*- coding: utf-8 -*-
"""R1.5 文档：CHANGELOG / HANDOFF / 记忆。"""
import io
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _patchlib import P

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')

p = P(os.path.join(ROOT, 'firmware', 'CHANGELOG.md'))
p.rep("""## R1.4 — 2026-10-14
""", """## R1.5 — 2026-10-14

### Added
- 蓝牙底层事件记录：连着时收到的角色变化、sniff 等模式变化、监督超时变化、加密变化、控制器硬件错误、厂商事件等记进诊断日志（HCI_EVT）
- 跳频信道表（AFH）变化记进诊断日志（在用信道数 + 被排除的信道范围），性能统计里也带上排除范围
- 「接收器当主」试验开关（`tools/link_tune.py 1|0`，不落盘、默认关）：打开后每次连上都申请当主（像 PS5 那样），当上主就把链路监督超时设成 10 秒

---

## R1.4 — 2026-10-14
""")
p.save()

p = P(os.path.join(ROOT, 'firmware', 'HANDOFF.md'))
p.rep("""### R1.4（2026-10-14 17:2x，起因：17:06 掉线抓到「手柄自己恢复了、是我们砍的」）
""", """### R1.5（2026-10-14 18:4x，起因：18:27 R1.4 上又掉了，而且是手柄的 5 s 监督超时判的）

- **R1.4 刷完的实测**：LINK_PARAM = **角色从(Peripheral)、监督超时 5.00 s、没改** ⇒ 5 s 是手柄（主）定的。blestack `hci_core.c` 的 `accept_conn()` 写死 `cp->role = BT_HCI_ROLE_SLAVE`，手柄 page 进来我们永远是从。
- **18:27:53 掉线（R1.4，连上 27 分钟，一直在放音乐压测）**：18:27:39 之前 650~690 帧/s、零失败 → 约 1605461 ms 输入全停（STALL 1606461）→ 0.57 s 后音频发送开始失败（池空）→ **ACL_DOWN 0x08 @1610475 = 停后 5.01 s**，我们的 10 s 看门狗没轮到。R1.4 对 >5 s 的停顿无能为力。
- **AFH 一直在动**：这一轮 18:01~18:27 手柄的信道表在 73~79 之间来回变（周围有 2.4G 干扰，手柄在躲）；停顿前 10 s 那次读数是 77/79、停顿中读回 79/79。AFH 变化很频繁，单凭这一条不能定案。
- **R1.5 改动**：① SDK（只打在 `E:\\BL616_Project\\sdk_v2.3.32` worktree）`hci_core.c` 的 `hci_event()` 加弱符号钩子 `bt_hci_evt_tap()`；固件 `bt_hid_host.c` 里实现：只在 CONNECTED 时记、每 10 s 最多 6 条 → `DIAG_EV_HCI_EVT`(39)，PERF 150 计总数；② AFH 每 2 s 读时算被排除信道的最低/最高编号，变了记 `DIAG_EV_AFH`(40)（最多 5 s 一条），PERF 148/149；③ `link_request_master()`：读链路策略（0x02/0x000C），没开主从切换就写（0x02/0x000D, bit0），再 `HCI_Switch_Role`(0x02/0x000B, 对端地址, 0x00) → `DIAG_EV_ROLE_SW`(41)；结果靠 Role Change 事件（DIAG 39）；当上主后 `link_write_lsto()` 写 10 s 并再记 LINK_PARAM。开关 = `0xF6` 命令 `0x11`（`tools/link_tune.py 1|0`），**RAM、默认关**。PERF 152 B。
- **试验要看什么**：切成主以后手柄送达的帧率（当从时手柄想发就发，650~780/s；当主后要靠我们轮询，可能变少）、音频是否正常、停顿还有没有、有的话能不能在 10 s 内自愈。
- 包：`_archive/R1.5_sdk232_93124f85`（787184 B）。只编了 sdk232（SDK 钩子只在那棵树上）。

### R1.4（2026-10-14 17:2x，起因：17:06 掉线抓到「手柄自己恢复了、是我们砍的」）
""")
p.save()

mem = os.path.join(os.path.expanduser('~'), '.claude', 'projects', 'E--BL616-Project-DongleRE', 'memory')
with io.open(os.path.join(mem, 'sdk-v2332-upgrade-path.md'), 'a', encoding='utf-8') as f:
    f.write("\n**2026-10-14 R1.5**：worktree 的 `components/wireless/bluetooth/blestack/src/host/hci_core.c` 多了一个本地改动：`hci_event()` 里调弱符号 `bt_hci_evt_tap()`（固件 bt_hid_host.c 覆盖，记 DIAG 39）。2.3.28 fork 没打这个钩子，用 fork 编也能过，只是不记 HCI 事件。现在默认刷 sdk232 包。另：blestack `accept_conn()` 写死 `BT_HCI_ROLE_SLAVE`，手柄 page 进来接收器永远是从，监督超时 5 s 由手柄定。\n")
with io.open(os.path.join(mem, 'random-reset-incident-2026-10-12.md'), 'a', encoding='utf-8') as f:
    f.write("\n**18:27（R1.4）**：LINK_PARAM 证实接收器是从、LSTO 5 s 是手柄定的；停后 5.01 s 以 0x08 断，10 s 看门狗没轮到。AFH 在 73~79 之间频繁变动（有 2.4G 干扰）。R1.5 加 HCI 事件旁路（DIAG 39）、AFH 变化（DIAG 40）、「接收器当主」试验开关（tools/link_tune.py，RAM 默认关，DIAG 41）。\n")
print('R1.5 DOCS DONE')
