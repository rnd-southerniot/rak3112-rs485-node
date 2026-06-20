/*
 * gpio_remap.h — SINGLE source of truth for every board-level pin assignment.
 *
 * Authority: hardware ADR-001-pin-map (rnd-southerniot/rak3112-rs485-node-hw,
 * tag adr-001-locked, commit a0b002c). Mirrored human-readable in docs/PIN_MAP.md.
 *
 * RULE (firmware domain CLAUDE.md §1 #6, §11): no driver hardcodes a GPIO number.
 * Every pin lives here. Any change to this file MUST update docs/PIN_MAP.md in the
 * same commit and is an ask-before-acting trigger.
 *
 * Board: RAK3112-9-SM-I (ESP32-S3 + SX1262) on "RAK3112 + RS485 P2P Node V1.1".
 *
 * Strapping pins (ESP32-S3 datasheet §2.4): GPIO0/3/45/46 are NEVER driven by
 * application code (project CLAUDE.md §3 #2). GPIO45/46 are module-internal / NC.
 */
#ifndef GPIO_REMAP_H
#define GPIO_REMAP_H

#include "driver/gpio.h" /* gpio_num_t, GPIO_NUM_* */

/* ----------------------------------------------------------------------------
 * Active in Phase 3 (first flash / bring-up)
 * --------------------------------------------------------------------------*/

/* Reserved-for-V1.2 I2C expansion header — routed-but-unterminated on V1.1.
 * ADR-001 EC-4/EC-9: Phase 3 firmware MUST hold both as deliberate-floating
 * (mode disabled, no internal pull, no drive) to avoid undefined power draw /
 * spurious ULP wakeup. Becomes I2C peripheral init once V1.2 boards exist (#1). */
#define PIN_I2C1_SDA_RESERVED GPIO_NUM_9  /* schematic net "GPIO9",  pin 28 */
#define PIN_I2C1_SCL_RESERVED GPIO_NUM_40 /* schematic net "GPIO40", pin 13 */

/* WS2812 addressable RGB indicator (U11 DIN). Optional Phase 3 status LED. */
#define PIN_WS2812_DIN GPIO_NUM_38 /* schematic net "GPIO38", pin 11 */

/* ----------------------------------------------------------------------------
 * Defined for completeness — first DRIVEN in later phases (do not touch in P3)
 * --------------------------------------------------------------------------*/

/* RS-485 field bus (TP8485E / U9). UART pin-mux assigned in Phase 4.
 * Console is native USB-CDC, so UART0/1 on these pins is free for the field bus. */
#define PIN_RS485_TX GPIO_NUM_43 /* "GPIO43_TX" -> U9 D via R34, pin 7 */
#define PIN_RS485_RX GPIO_NUM_44 /* "GPIO44_RX" -> U9 R,        pin 6 */

/* RS-485 driver/receiver enable via Q6 inverter (ADR-001 EC-5a).
 * INVERTED polarity: GPIO21 HIGH = DE/RE LOW = RECEIVE; GPIO21 LOW = TRANSMIT.
 * First driven in Phase 4 — leave unconfigured in Phase 3. */
#define PIN_RS485_DE_RE GPIO_NUM_21 /* schematic net "GPIO21", pin 41 */

/* ----------------------------------------------------------------------------
 * Strapping / module-internal pins — DO NOT DRIVE from application code
 * --------------------------------------------------------------------------*/
#define PIN_BOOT_SW2                                                                               \
    GPIO_NUM_0 /* strap: boot-mode select; SW2; pin 24.                                            \
                 HIGH at reset = normal SPI boot. */
/* GPIO3  (JTAG src), GPIO45 (VDD_SPI voltage), GPIO46 (ROM print): module-internal
 * / NC on RAK3112 — intentionally NOT defined here so nothing references them. */

#endif /* GPIO_REMAP_H */
