/*
 * modbus_slave_sim.h — bench fixture: emulate a Modbus RTU slave device on this board's RS-485.
 *
 * Turns the node into a Modbus *server* that answers FC03 reads with the Honeywell EEM400-D-MO
 * register map, so the Pi scanner station can discover + profile it blind (wire this board's CN1 to
 * a scanner node's CN1). Gated by CONFIG_APP_MODBUS_SLAVE_SIM; does not join LoRaWAN. Not a product
 * feature — a test fixture kept on its own branch.
 */
#ifndef MODBUS_SLAVE_SIM_H
#define MODBUS_SLAVE_SIM_H

#ifdef __cplusplus
extern "C" {
#endif

/* Initialise RS-485 (unit/baud from Kconfig, 8N1) and serve FC03 reads forever. Does not return. */
void run_modbus_slave_sim(void);

#ifdef __cplusplus
}
#endif

#endif /* MODBUS_SLAVE_SIM_H */
