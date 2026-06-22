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

/* RS-485 driver/receiver enable (DE/RE tied). STANDARD polarity — empirically
 * confirmed on the project board (Phase 4, 2026-06-20): GPIO21 HIGH = TRANSMIT,
 * GPIO21 LOW (idle) = RECEIVE. (ADR-001 EC-5a's "inverted, HIGH=receive" was wrong;
 * see ADR-002 rev2.) Driven by the UART as non-inverted RTS in RS-485 half-duplex. */
#define PIN_RS485_DE_RE GPIO_NUM_21 /* schematic net "GPIO21", pin 41 */

/* ----------------------------------------------------------------------------
 * SX1262 LoRa radio (module-internal SPI bus) — Phase 5.
 * CONFIRMED on the project board 2026-06-20 (no-TX SPI probe, 5a): hardware reset +
 * GetStatus=0x2a (STBY_RC) + register write/read round-trip all passed on these pins.
 * Source: RAK3112 datasheet + RAK forum + Zephyr rak3112 DTS (all agree); bench-verified.
 *
 * IMPORTANT: these are NOT the module-edge "GPIO10..13 / SPI_*" pins — those are a
 * SEPARATE user SPI broken out to the edge. ADR-001's pin-map labelled GPIO10-13 as the
 * SX1262 SPI — that was WRONG (corrected hw-side after this confirmation). See ADR-003.
 * --------------------------------------------------------------------------*/
#define PIN_LORA_MISO                                                                              \
    GPIO_NUM_3 /* SX1262 MISO (also ESP32-S3 JTAG-src strap @reset; free post-boot) */
#define PIN_LORA_SCK GPIO_NUM_5   /* SX1262 SCK */
#define PIN_LORA_MOSI GPIO_NUM_6  /* SX1262 MOSI */
#define PIN_LORA_NSS GPIO_NUM_7   /* SX1262 NSS / CS */
#define PIN_LORA_RESET GPIO_NUM_8 /* SX1262 NRESET (active-low, open-drain) */
#define PIN_LORA_BUSY GPIO_NUM_48 /* SX1262 BUSY */
#define PIN_LORA_DIO1 GPIO_NUM_47 /* SX1262 DIO1 (IRQ) */
#define PIN_LORA_ANT_SW                                                                            \
    GPIO_NUM_4 /* RF/antenna switch power (active-low). RF switching is                            \
                * driven by SX1262 DIO2 (setDio2AsRfSwitch); TCXO by DIO3                          \
                * — both SX1262-internal, controlled over SPI, not MCU pins. */

/* ----------------------------------------------------------------------------
 * Strapping / module-internal pins — DO NOT DRIVE from application code
 * --------------------------------------------------------------------------*/
#define PIN_BOOT_SW2                                                                               \
    GPIO_NUM_0 /* strap: boot-mode select; SW2; pin 24.                                            \
                 HIGH at reset = normal SPI boot. */
/* GPIO45 (VDD_SPI voltage), GPIO46 (ROM print): module-internal — intentionally NOT defined
 * here so nothing references them. (GPIO3 is a JTAG-src strap at reset but is the SX1262 MISO
 * post-boot — defined above as PIN_LORA_MISO.) */

#endif /* GPIO_REMAP_H */
