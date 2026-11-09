#ifndef LED_STATUS_H
#define LED_STATUS_H

#include <stdbool.h>

enum led_pattern {
    LED_OFF,
    LED_PURPLE_BLINK_SLOW, /* Slow blink (~1 Hz) — idle / waiting for controller */
    LED_PURPLE_BLINK_FAST, /* Fast blink (~3 Hz) — pairing / scanning */
    LED_GREEN_SOLID,       /* Solid — connected */
    LED_BLUE_SOLID,        /* Blue solid */
    LED_RED_BLINK,         /* Red blink — just disconnected, auto→purple */
    LED_BLINK_ONCE,        /* Single flash — event acknowledge */
    LED_BLINK_DOUBLE,      /* Double flash — remap profile 2 */
    LED_BLINK_TRIPLE,      /* Triple flash — bonds cleared */
    LED_BLINK_QUAD,        /* Quadruple flash — 触摸板单指目标 4 */
    LED_BLINK_QUINT,       /* Quintuple flash — 触摸板单指目标 5 */
    LED_BLINK_BATTERY,     /* Red blink — critical battery (<=10%) */
    LED_BLINK_BATTERY_WARN,/* Yellow blink — low battery (<=20%) */
};

/* Semantic aliases used by the LED state policy in main.c.
 * On the single-LED boards (LCTech BL616) all three drive the blue LED. */
#define LED_WAITING   LED_PURPLE_BLINK_SLOW /* ~1 Hz blink — waiting for controller */
#define LED_PAIRING   LED_PURPLE_BLINK_FAST /* ~3 Hz blink — pairing mode */
#define LED_CONNECTED LED_GREEN_SOLID       /* solid — controller connected */

void led_status_init(void);
void led_status_set(enum led_pattern pattern);
/* Resting pattern that one-shot event flashes (once/double/triple) return to.
 * Call whenever the connection state changes. */
void led_status_set_default(enum led_pattern pattern);
enum led_pattern led_status_get(void);
void led_status_tick(void);
void led_status_lock(void);
void led_status_unlock(void);

#endif /* LED_STATUS_H */
