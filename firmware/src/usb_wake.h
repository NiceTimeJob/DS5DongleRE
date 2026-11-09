#ifndef USB_WAKE_H
#define USB_WAKE_H

#include <stdint.h>
#include <stdbool.h>

void usb_wake_init(void);
void usb_wake_on_suspend(void);
void usb_wake_on_resume(void);
void usb_wake_on_configured(void);
void usb_wake_on_bt_connect(void);
void usb_wake_on_bt_disconnect(void);
bool usb_wake_host_suspended(void);

/** 让手柄立即静音关机（DualSense 特征 0x08 子命令 0x02）。任务上下文调用；
 *  手柄收到后自己断电，ACL 随后自然掉落（conn_disconnected 会重开 page scan）。 */
void usb_wake_power_off_controller(void);

/**
 * Feed BT input to the wake FSM for button-change detection.
 * @param payload  USB-equivalent payload (63 bytes, starting from stick axes)
 * @param len      payload length (must be >= 10)
 */
void usb_wake_on_bt_input(const uint8_t *payload, uint16_t len);

/** Advance the wake FSM. Call periodically from the USB task loop. */
void usb_wake_task(void);

/** Check and clear radio wake flag. Call from bt_task context. */
bool usb_wake_radio_wake_pending(void);

#endif /* USB_WAKE_H */
