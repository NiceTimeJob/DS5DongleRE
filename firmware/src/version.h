#ifndef DONGLERE_VERSION_H_
#define DONGLERE_VERSION_H_

/*
 * 固件版本号（2026-10-14 起用我们自己的编号，用户定义）：
 *   R1、R2、R3 … 大版本；R1.1、R1.2 … 小版本。只改下面 FW_VERSION 这一处 ——
 *   0xF8 特征报告、网页「固件版本」、tools/verify_fw.py、tools/check_bin.ps1 全部从它派生。
 *
 * 沿革（详细见 CHANGELOG.md）：
 *   R2.5 两段式扳机（派生键 39/40）→ R2.6 四种官方扳机效果 →
 *   R2.7 扳机效果必须走 BT 的 0x32 扩展报文 → R2.8 自动扳机周期重发 →
 *   R2.9 改完参数即时下发（不用断电）→ R3.0 右栏重排 + 两个行为开关 →
 *   R3.1 参数口径对齐原生 → R3.2 两段分界点与手感解耦 → R3.3 模式位真正生效 →
 *   R3.4 派生键松手真的释放 → R3.5 分界点按设定值触发 →
 *   R3.6 分界点独立字段（0x0b 12→14 字节，配置重置一次）→
 *   R3.7 未勾「保持一阶段的输出」时一段真的松开 → R3.8 两段判定改纯阈值（去掉迟滞）→
 *   R3.9 起始位置 0（档 0）不再被当成"未设置" →
 *   R3.10 试过「逐档实测表」换算（精度不足，R3.11 已回线性）→
 *   R3.11 换算回线性 + 效果改名（恒定阻力 / 狙击 / 自动步枪）→
 *   R3.12 未勾保持一阶段时，报文里的扳机数字位也一起清 →
 *   R3.13 设为「关闭」后主动撤掉手柄上残留的效果（不再依赖主机再发一次报文）→
 *   R3.14~R3.24 都只动了网页（没出固件包，所以 FW_VERSION 一直停在 R3.13）→
 *   R3.25 **整套扳机设置按层**（L2/R2 的模式位/分界点/自适应扳机参数每层一套；
 *         0x0b 14→66 字节、DS5MAP_CFG_VER 15 → 配置重置一次）→
 *   R3.26 切层后效果不跟（切层重发改走**不 forget** 的请求）→
 *   R3.27 关掉效果后手柄还留着旧效果（配置改了只**强制重发**，绝不清"装过"状态）→
 *   R3.28 音频挡位进配置体（audio_kbps/audio_bw/audio_cx 三字段，CONFIG_VERSION 10）
 *
 * 版本串形态： <产品> <版本>[ HS][ <SDK 标记>]
 *   产品 BL616-DongleRE / M0S-DongleRE / BL618-DongleRE（按板型）
 *   HS   仅高轮询率版带；普通版（Full-Speed，默认）不带
 *   SDK 标记 仅编译时给了 FW_SDK_TAG 的试验包带（如 "sdk232"）
 */

#include "board_config.h"

#define FW_VERSION      "R3.28"

#if defined(BOARD_LCTECH_616)
#define FW_PRODUCT      "BL616-DongleRE"
#elif defined(BOARD_M0S_DOCK)
#define FW_PRODUCT      "M0S-DongleRE"
#else
#define FW_PRODUCT      "BL618-DongleRE"
#endif

#ifdef FORCE_FS_MODE
#define FW_SPEED        ""          /* 普通版（Full-Speed，默认） */
#else
#define FW_SPEED        " HS"       /* 高轮询率版（High-Speed） */
#endif

#ifdef FW_SDK_TAG
#define FW_VERSION_STRING   FW_PRODUCT " " FW_VERSION FW_SPEED " " FW_SDK_TAG
#else
#define FW_VERSION_STRING   FW_PRODUCT " " FW_VERSION FW_SPEED
#endif

#endif /* DONGLERE_VERSION_H_ */
