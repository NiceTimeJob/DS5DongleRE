# -*- coding: utf-8 -*-
import sys
sys.path.insert(0, 'E:/BL616_Project/DongleRE/tools')
from _patchlib import P
h = P('E:/BL616_Project/DongleRE/firmware/HANDOFF.md')
h.rep('**11:22:32 刷进公司这台接收器**（COM12，3 次校验，设备 SHA 75e7500f）。',
      '**11:22:32 刷进公司这台接收器**（COM12，3 次校验，设备 SHA 75e7500f；用户拔插后 verify_fw.py 校验通过：'
      '0xF8 = `BL616-DongleRE R1.14`，config v9 / 13 B，DIAG 头只有上电 BOD + BOOT + USB_RESUME，正常）。')
h.save()
print('docs3 done')
