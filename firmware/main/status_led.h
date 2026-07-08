/*
 * status_led.h — node status on the onboard WS2812 RGB (GPIO38, U11).
 *
 * The animation (breathe / pulse / blink) runs on its own low-priority task; the field app just
 * calls status_led_set() at state transitions and status_led_event_uplink() on a successful uplink.
 * Cheap, non-blocking, and independent of the LoRa/Modbus path (unlike the bench APP_LED_HEARTBEAT
 * mode).
 */
#ifndef STATUS_LED_H
#define STATUS_LED_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    STATUS_LED_BOOT = 0,     /* startup — dim white */
    STATUS_LED_SENSOR_WAIT,  /* red slow breathe — no RS-485 sensor answering yet */
    STATUS_LED_JOINING,      /* amber pulse — OTAA join in progress */
    STATUS_LED_IDLE,         /* dim green breathe — healthy, waiting for the next sample */
    STATUS_LED_SENSOR_FAULT, /* red fast blink — sensor read failed / payload stale */
} status_led_state_t;

/* Init the WS2812 on GPIO38 and start the animation task. Returns the led_strip error if the device
 * can't init (the node keeps running without the indicator). Call once. */
esp_err_t status_led_init(void);

/* Set the persistent status the LED shows. */
void status_led_set(status_led_state_t st);

/* One-shot bright-green flash overlaid on the current state (e.g. right after `uplink OK`). */
void status_led_event_uplink(void);

#ifdef __cplusplus
}
#endif

#endif /* STATUS_LED_H */
