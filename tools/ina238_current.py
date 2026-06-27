#!/usr/bin/env python3
"""
ina238_current.py — measure the rak3112-rs485-node average current with a TI INA238 high-side
monitor on a Raspberry Pi (I2C). The INA238 has no energy/charge accumulator (that's the INA228),
so this samples VSHUNT/VBUS fast and integrates the TRUE time-average over the light-sleep duty
cycle, then reports sleep baseline / active spike / average + an estimated battery runtime.

This is the Phase 7b power-gate (OQ-12) measurement. Power the node through P1/DC with the native
USB UNPLUGGED — no VBUS on the USB-Serial-JTAG means light-sleep runs without wedging.

Wiring (high-side, INA shunt in series on the DUT's P1/DC input):
    source(+)  ->  INA238 IN+        INA238 IN-  ->  DUT P1 V+
    source(-)  ----------------------------------->  DUT P1 GND      (common ground)
    INA238 VS  ->  Pi 3V3            INA238 GND  ->  Pi GND (+ DUT GND)
    INA238 SDA ->  Pi SDA (GPIO2)    INA238 SCL  ->  Pi SCL (GPIO3)
  source = 3.7 V Li-ion or 5 V power bank leads to P1 (RT6160 input 2.2-5.5 V). NOT the board's USB.

Prereqs:  sudo raspi-config -> enable I2C ;  pip install smbus2
Run:      python3 ina238_current.py --rshunt 0.015 --secs 120
          (set --rshunt to YOUR board's shunt — check the INA238 breakout; Adafruit = 0.015 ohm)
"""
import argparse
import csv
import sys
import time

try:
    from smbus2 import SMBus
except ImportError:
    sys.exit("pip install smbus2   (and enable I2C via raspi-config)")

# ---- INA238 register map ----
REG_CONFIG = 0x00
REG_ADC_CONFIG = 0x01
REG_VSHUNT = 0x04
REG_VBUS = 0x05
REG_MANUF_ID = 0x3E  # expect 0x5449 ('TI')
REG_DEVICE_ID = 0x3F  # 0x2381 = INA238

VBUS_LSB = 3.125e-3                 # V/LSB
VSHUNT_LSB = {0: 5e-6, 1: 1.25e-6}  # V/LSB by ADCRANGE (0=+/-163.84mV, 1=+/-40.96mV)


def s16(x):
    return x - 0x10000 if x & 0x8000 else x


def rd16(bus, addr, reg):  # INA238 is big-endian; smbus word is little-endian -> swap
    w = bus.read_word_data(addr, reg)
    return ((w & 0xFF) << 8) | (w >> 8)


def wr16(bus, addr, reg, val):
    bus.write_word_data(addr, reg, ((val & 0xFF) << 8) | (val >> 8))


def main():
    ap = argparse.ArgumentParser(description="INA238 average-current meter (Phase 7b / OQ-12).")
    ap.add_argument("--bus", type=int, default=1, help="I2C bus (Pi default 1)")
    ap.add_argument("--addr", type=lambda x: int(x, 0), default=0x40, help="INA238 I2C address")
    ap.add_argument("--rshunt", type=float, required=True, help="shunt ohms (your board, e.g. 0.015)")
    ap.add_argument("--adcrange", type=int, choices=[0, 1], default=1, help="1=+/-40.96mV (finer)")
    ap.add_argument("--secs", type=float, default=120.0, help="capture seconds (>= a few intervals)")
    ap.add_argument("--csv", default="ina238_log.csv")
    ap.add_argument("--battery-mah", type=float, default=2000.0, help="for the runtime estimate")
    a = ap.parse_args()

    bus = SMBus(a.bus)
    manuf = rd16(bus, a.addr, REG_MANUF_ID)
    devid = rd16(bus, a.addr, REG_DEVICE_ID)
    if manuf != 0x5449:
        print(f"WARN: MANUFACTURER_ID=0x{manuf:04x} (expected 0x5449 'TI') — check --addr / wiring")
    print(f"INA238 @0x{a.addr:02x} manuf=0x{manuf:04x} dev=0x{devid:04x} "
          f"Rshunt={a.rshunt} ohm ADCRANGE={a.adcrange}")

    wr16(bus, a.addr, REG_CONFIG, (a.adcrange & 1) << 4)  # ADCRANGE bit4, RST=0
    # ADC_CONFIG: MODE=0xB (continuous shunt+bus), VBUSCT=VSHCT=0b010 (150us), VTCT=0, AVG=0 (1)
    adc = (0xB << 12) | (0b010 << 9) | (0b010 << 6) | (0b000 << 3) | 0b000
    wr16(bus, a.addr, REG_ADC_CONFIG, adc)
    time.sleep(0.05)

    vsh_lsb = VSHUNT_LSB[a.adcrange]
    samples = []  # (t_s, I_A, V_V)
    print(f"sampling {a.secs:.0f}s … (Ctrl-C to stop early)")
    t0 = time.perf_counter()
    try:
        while True:
            t = time.perf_counter() - t0
            if t >= a.secs:
                break
            vsh = s16(rd16(bus, a.addr, REG_VSHUNT))
            vb = rd16(bus, a.addr, REG_VBUS)
            samples.append((t, vsh * vsh_lsb / a.rshunt, vb * VBUS_LSB))
    except KeyboardInterrupt:
        print(" (stopped)")

    if len(samples) < 2:
        sys.exit("no samples captured — check wiring / address")

    # true time-average via trapezoidal integral of I dt
    charge = 0.0
    for i in range(1, len(samples)):
        dt = samples[i][0] - samples[i - 1][0]
        charge += 0.5 * (samples[i][1] + samples[i - 1][1]) * dt
    span = samples[-1][0] - samples[0][0]
    avg = charge / span
    cur = [s[1] for s in samples]
    imin, imax = min(cur), max(cur)
    vavg = sum(s[2] for s in samples) / len(samples)

    # split sleep vs active at 20% of the min..max span
    thr = imin + 0.2 * (imax - imin)
    sleep = [i for i in cur if i < thr]
    active = [i for i in cur if i >= thr]
    duty = 100.0 * len(active) / len(cur)
    sleep_avg = sum(sleep) / len(sleep) if sleep else 0.0
    active_avg = sum(active) / len(active) if active else 0.0

    with open(a.csv, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["t_s", "I_mA", "V_V"])
        for t, I, V in samples:
            w.writerow([f"{t:.4f}", f"{I * 1e3:.3f}", f"{V:.3f}"])

    print("\n=== INA238 result (Phase 7b / OQ-12) ===")
    print(f"  window         : {span:.1f} s, {len(samples)} samples (~{len(samples) / span:.0f} Hz), "
          f"Vbus≈{vavg:.3f} V")
    print(f"  AVERAGE current: {avg * 1e3:.2f} mA   <-- the OQ-12 number (true time-average)")
    print(f"  sleep baseline : {sleep_avg * 1e3:.2f} mA   (min {imin * 1e3:.2f} mA)")
    print(f"  active spike   : {active_avg * 1e3:.2f} mA   (max {imax * 1e3:.2f} mA), active duty {duty:.1f}%")
    print(f"  avg power      : {avg * vavg * 1e3:.1f} mW @ {vavg:.2f} V")
    if avg > 0:
        hrs = a.battery_mah / (avg * 1e3)
        print(f"  est. runtime   : {hrs:.0f} h (~{hrs / 24:.1f} days) on a {a.battery_mah:.0f} mAh battery")
    else:
        print("  (average <= 0 — if negative, swap INA IN+/IN- ; current is flowing the other way)")
    print(f"  CSV            : {a.csv}")


if __name__ == "__main__":
    main()
