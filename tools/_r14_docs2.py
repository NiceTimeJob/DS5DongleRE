# -*- coding: utf-8 -*-
"""R1.14 收尾文档：用户拍板发布线 = sdk232、版本串去掉后缀；11:22 刷进公司这台。HANDOFF + 记忆同步。"""
import sys
sys.path.insert(0, 'E:/BL616_Project/DongleRE/tools')
from _patchlib import P

H = 'E:/BL616_Project/DongleRE/firmware/HANDOFF.md'
MEM = 'C:/Users/yiqi/.claude/projects/E--BL616-Project-DongleRE/memory/'

h = P(H)
h.rep('（2026-10-15 11:05 公司机器编译，两份包：`_archive/R1.14_sdk232_1d46b668`'
      '（787184 B，0xF8 = `BL616-DongleRE R1.14 sdk232`，上游 2.3.32 + 控制器库 1.6.199，接 R1.6 夜测那条线）与 '
      '`_archive/R1.14_sdk2328_cfa62cfb`（787136 B，0xF8 = `BL616-DongleRE R1.14`，E 盘 3.18 fork）；'
      '**都未烧录、网页包未重出，等用户定哪条 SDK 线是发布线**）',
      '（2026-10-15 公司机器编译。发布包 `_archive/R1.14_sdk232_75e7500f`（787184 B，0xF8 = `BL616-DongleRE R1.14`，'
      '上游 2.3.32 + 控制器库 1.6.199，接 R1.6 夜测那条线），**11:22:32 刷进公司这台接收器**（COM12，3 次校验，设备 SHA 75e7500f）。'
      '用户拍板：发布线就是 sdk232，版本串**不再带「 sdk232」后缀**（`build_variant.sh sdk232` 的 TAG 已清空，`sdk2328` 变体只剩对照用）；'
      '此前的 `_archive/R1.14_sdk232_1d46b668`（带后缀，11:16 刷过一次，已被覆盖）和 `_archive/R1.14_sdk2328_cfa62cfb`（E 盘 3.18 fork）是试验包。'
      '**网页刷写包 webgui/firmware 仍是 R1.13，未重出**）')
h.rep('比公司两条线都小 12 KB ⇒ 家里那棵 SDK 树和公司两棵都不一样，哪条是发布线待用户定。',
      '比公司两条线都小 12 KB。用户答复：「R1.13 是最新的版本，SDK232 也已经更新在里面了，不过我把命名的这个后缀给去掉了」'
      '⇒ 家里那棵也是 2.3.32 线，发布线 = sdk232、版本串不带后缀；12 KB 的体积差没有深究。')
h.save()

m = P(MEM + 'fw-version-scheme-r.md')
m.rep('**R1.14（2026-10-15 11:05 公司编译，未烧）**：「禁用 LED」真全灭（只留对频快闪；红灯是电源灯管不到），'
      '删 1 分钟定时器；两份包 `_archive/R1.14_sdk232_1d46b668`（0xF8 带 sdk232）/ `_archive/R1.14_sdk2328_cfa62cfb`'
      '（0xF8 = `BL616-DongleRE R1.14`，与家里命名一致）。家里 R1.7~R1.13 用的 SDK 是 F:/BL616/bouffalo_sdk、'
      'app 比公司两条线小 12 KB ⇒ 三棵 SDK 树，发布线待用户定。网页 r47。',
      '**R1.14（2026-10-15 11:19 公司编译、11:22 刷进公司这台）**：「禁用 LED」真全灭（只留对频快闪；红灯是电源灯管不到），'
      '删 1 分钟定时器；发布包 `_archive/R1.14_sdk232_75e7500f`（0xF8 = `BL616-DongleRE R1.14`，上游 2.3.32 线）。'
      '**用户拍板：发布线 = sdk232，版本串不再带「 sdk232」后缀**（`build_variant.sh sdk232` 的 TAG 已清空；'
      '家里那棵 F:/BL616/bouffalo_sdk 用户说也是 2.3.32，12 KB 体积差没深究）。网页 r47；webgui/firmware 包仍是 R1.13 未重出。')
m.save()

i = P(MEM + 'MEMORY.md')
i.rep('最新 R1.13（10-15 03:19 家里编的，包在 webgui/firmware，云端已发，config v9）；公司这台接收器还是 R1.6 sdk232（config v7）',
      '最新 R1.14（10-15 11:22 刷进公司这台，0xF8 `BL616-DongleRE R1.14`，发布线 = sdk232、后缀已去）；webgui/firmware 包与云端仍 R1.13（config v9）')
i.save()

p = P(MEM + 'project-sync-overwrites-local.md')
p.rep('公司这台（R1.6 sdk232，20:30 起连着 DualSense 夜测）', '公司这台（夜测时 R1.6 sdk232，10-15 11:22 刷成 R1.14）')
p.save()
print('R1.14 docs2 done')
