# -*- coding: utf-8 -*-
"""R1 功能补丁（网页侧）：OUT_MEDIA 媒体控制、「静默 LED」开关、测试页显示媒体键。
用法：python tools/_r1_feat_web_patch.py"""
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _patchlib import P

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
os.chdir(os.path.join(ROOT, 'webgui', 'js'))

# ---------------- page-map.js ----------------
p = P('page-map.js')
p.rep("""  const OUT_STRAT = 7;                 // 绝地潜兵 2 战备宏：val = state.strats 里的槽位
""", """  const OUT_STRAT = 7;                 // 绝地潜兵 2 战备宏：val = state.strats 里的槽位
  const OUT_MEDIA = 8;                 // R1 媒体控制：val = Consumer Control 位号（见 MEDIA_NAMES 下标）
""")
p.rep("""  const MOUSE_NAMES = ['左键', '右键', '中键', '滚轮上滚', '滚轮下滚'];
""", """  const MOUSE_NAMES = ['左键', '右键', '中键', '滚轮上滚', '滚轮下滚'];
  /* 下标 = 固件 Consumer Control 位图的位号（usb_gamepad.c 键鼠描述符 报告 ID 2） */
  const MEDIA_NAMES = ['音量 +', '音量 −', '静音', '播放 / 暂停', '下一曲', '上一曲', '停止'];
""")
p.rep("""    { v: OUT_MOUSE, n: '鼠标' },
    { v: OUT_MACRO, n: '宏' },
""", """    { v: OUT_MOUSE, n: '鼠标' },
    { v: OUT_MEDIA, n: '媒体控制' },
    { v: OUT_MACRO, n: '宏' },
""", cnt=2)
p.rep("""    if (v === OUT_KBD) s.val = 4;                                      /* 默认 A */
""", """    if (v === OUT_KBD) s.val = 4;                                      /* 默认 A */
    else if (v === OUT_MEDIA) s.val = 3;                               /* 默认 播放/暂停 */
""")
p.rep("""    case OUT_MOUSE: return mouseName(s.val);
""", """    case OUT_MOUSE: return mouseName(s.val);
    case OUT_MEDIA: return MEDIA_NAMES[s.val] || '媒体';
""")
p.rep("""    if (s.out === OUT_MOUSE) {
      return `<div class="field"><label>鼠标动作</label>
        <div class="stack pick">
          <select id="${id('mouse')}">${MOUSE_NAMES.map((n, i) =>
        `<option value="${i}" ${s.val === i ? 'selected' : ''}>${n}</option>`).join('')}</select>
          ${cap}
        </div></div>`;
    }
""", """    if (s.out === OUT_MOUSE) {
      return `<div class="field"><label>鼠标动作</label>
        <div class="stack pick">
          <select id="${id('mouse')}">${MOUSE_NAMES.map((n, i) =>
        `<option value="${i}" ${s.val === i ? 'selected' : ''}>${n}</option>`).join('')}</select>
          ${cap}
        </div></div>`;
    }
    if (s.out === OUT_MEDIA) {
      return `<div class="field"><label>媒体键</label>
        <div class="stack"><select id="${id('media')}">${MEDIA_NAMES.map((n, i) =>
        `<option value="${i}" ${s.val === i ? 'selected' : ''}>${n}</option>`).join('')}</select></div></div>
        <div class="hint">按下发出、松开收回，系统按"按下"那一下触发：播放/暂停点一下就行；音量键按住不放会由系统自己连续调。走键鼠接口，和键盘/鼠标输出互不影响。</div>`;
    }
""")
p.rep("""    const mm = q('mouse');
    if (mm) mm.addEventListener('change', e => { s.val = +e.target.value; markDirty(); });
""", """    const mm = q('mouse');
    if (mm) mm.addEventListener('change', e => { s.val = +e.target.value; markDirty(); });
    const mdk = q('media');
    if (mdk) mdk.addEventListener('change', e => { s.val = +e.target.value; markDirty(); updatePadMarks(); });
""")
p.save()

# ---------------- page-test.js ----------------
p = P('page-test.js')
p.rep("""    for (let i = 0; i < 6; i++)
      if (st[14 + i]) parts.push(MapPage.keyName(st[14 + i]));
    return parts;
""", """    for (let i = 0; i < 6; i++)
      if (st[14 + i]) parts.push(MapPage.keyName(st[14 + i]));
    /* R1：媒体键位图（TLM_STATE[44]），位号同固件 Consumer Control 描述符 */
    if (st.length > 44) {
      [[1, '音量+'], [2, '音量−'], [4, '静音'], [8, '播放/暂停'], [16, '下一曲'], [32, '上一曲'], [64, '停止']]
        .forEach(([bit, n]) => { if (st[44] & bit) parts.push(n); });
    }
    return parts;
""")
p.save()

# ---------------- page-config.js ----------------
p = P('page-config.js')
p.rep("""   * 结构体 29 → 27 → 26 → 22 字节（S15 删手柄模式、S32 删闲置断连、
   * S33 删原作者的 PS 快捷键 + 触摸板模式四个字段），配置版本 3 → 4 → 5 → 6
   * （老配置在固件里整份回落默认值）。 */
""", """   * 结构体 29 → 27 → 26 → 22 → 23 字节（S15 删手柄模式、S32 删闲置断连、
   * S33 删原作者的 PS 快捷键 + 触摸板模式四个字段、R1 加「静默 LED」），
   * 配置版本 3 → 4 → 5 → 6 → 7（老配置在固件里整份回落默认值）。 */
""")
p.rep("""    audio_haptic: 21,
  };
  const SIZE = 22;
  const CFG_VERSION = 6;      /* 必须与固件 CONFIG_VERSION 一致 */
""", """    audio_haptic: 21, led_quiet_connected: 22,
  };
  const SIZE = 23;
  const CFG_VERSION = 7;      /* 必须与固件 CONFIG_VERSION 一致 */
""")
p.rep("""    { k: 'disable_led', t: 'bool', n: '禁用 LED' },
""", """    { k: 'disable_led', t: 'bool', n: '禁用 LED', hint: '原作者逻辑：连上 1 分钟后熄灭，等待时照常闪' },
    { k: 'led_quiet_connected', t: 'bool', n: '静默 LED', hint: '手柄一连上状态灯就熄灭；等待 / 对频时照常闪，电量告警仍会闪' },
""")
p.save()
print('R1 FEATURE PATCH (web) DONE')
