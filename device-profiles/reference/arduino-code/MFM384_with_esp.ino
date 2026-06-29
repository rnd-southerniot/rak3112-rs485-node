/**
 * @file esp32-mfm384-to-rak3172.ino
 * @brief Reads MFM384 via Modbus (input regs as 32-bit float, LW-first) and sends
 *        a scaled ×100 binary payload (int32_t[]) to RAK3172 over UART,
 *        following the same send method as the Honeywell reference.
 */

#include <ModbusMaster.h>

// ---------------- Pin + Serial Config ----------------
// RAK3172 UART (same style as reference code)
#define RAK_RX_PIN         23     // ESP32 RX  (connect to RAK TX)
#define RAK_TX_PIN         18     // ESP32 TX  (connect to RAK RX)
#define RAK_BAUD           115200 // RAK AT fw default

// MFM384 / RS-485 UART
#define MFM_RX_PIN         16     // ESP32 RX from RS-485 transceiver RO
#define MFM_TX_PIN         17    // ESP32 TX to RS-485 transceiver DI
#define MFM_BAUD           9600
#define MFM_SERIAL_MODE    SERIAL_8N1  // Change if your meter needs parity

// RS-485 DE/RE control
#define MAX485_DE_RE        4     // HIGH=TX, LOW=RX

// Modbus
#define MFM_SLAVE_ID        1
#define INTER_READ_DELAY    100   // ms between Modbus transactions

HardwareSerial RakSerial(1);
HardwareSerial MfmSerial(2);
ModbusMaster   node;

// ---------------- Parameter Table ----------------
struct ModbusParameter {
  const char* name;
  uint16_t    reg;    // protocol index for input regs (30001 -> 0, 30002 -> 1, etc.)
  float       value;
};

// Your requested parameters in order (one float = 2 regs)
ModbusParameter parametersToRead[] = {
  // 30000..30028 range (your indices already 0-based protocol indexes)
  {"Voltage V1N",       0,  0.0}, // 30000/30001 per your list's convention (protocol index 0)
  {"Voltage V2N",       2,  0.0}, // 30002
  {"Voltage V3N",       4,  0.0}, // 30004
  {"Avg Voltage L-N",   6,  0.0}, // 30006
  {"Voltage V12",       8,  0.0}, // 30008
  {"Voltage V23",       10, 0.0}, // 30010
  {"Voltage V31",       12, 0.0}, // 30012
  {"Avg Voltage L-L",   14, 0.0}, // 30014
  {"Current I1",        16, 0.0}, // 30016
  // If you also want I2, add: {"Current I2", 18, 0.0},
  {"Current I3",        20, 0.0}, // 30020
  {"Average Current",   22, 0.0}, // 30022
  {"kW Phase 1",        24, 0.0}, // 30024
  {"kW Phase 2",        26, 0.0}, // 30026
  {"kW Phase 3",        28, 0.0}, // 30028

  // Single 30042
  {"kVAr Phase 3",      42, 0.0}, // 30042

  // 30044..30058
  {"Total kW",          44, 0.0}, // 30044
  {"Total kVAr",        46, 0.0}, // 30046
  {"Power Factor 1",    48, 0.0}, // 30048
  {"Power Factor 2",    50, 0.0}, // 30050
  {"Power Factor 3",    52, 0.0}, // 30052
  {"Average PF",        54, 0.0}, // 30054
  {"Frequency",         56, 0.0}, // 30056
  {"Total kWh",         58, 0.0}  // 30058
};

// ---------------- RS-485 Direction Control ----------------
void preTransmission() {
  digitalWrite(MAX485_DE_RE, HIGH);
}
void postTransmission() {
  digitalWrite(MAX485_DE_RE, LOW);
}

// ---------------- Read a 32-bit float from Input Registers ----------------
// Assumes meter returns floats as Little-Endian (Low Word first): [lo][hi]
float readFloatInputRegister(uint16_t regIndex /*protocol index: 30001->0*/) {
  uint8_t result = node.readInputRegisters(regIndex, 2);
  if (result == node.ku8MBSuccess) {
    uint16_t lo = node.getResponseBuffer(0);
    uint16_t hi = node.getResponseBuffer(1);
    uint32_t u32 = ((uint32_t)hi << 16) | lo;
    float f;
    memcpy(&f, &u32, sizeof(f));
    node.clearResponseBuffer();
    return f;
  } else {
    node.clearResponseBuffer();
    return NAN;
  }
}

// ---------------- Setup ----------------
void setup() {
  Serial.begin(115200);
  delay(100);

  pinMode(MAX485_DE_RE, OUTPUT);
  digitalWrite(MAX485_DE_RE, LOW); // start in RX

  // RAK3172 UART (same pattern as reference)
  RakSerial.begin(RAK_BAUD, SERIAL_8N1, RAK_RX_PIN, RAK_TX_PIN);

  // MFM384 UART
  MfmSerial.begin(MFM_BAUD, MFM_SERIAL_MODE, MFM_RX_PIN, MFM_TX_PIN);

  // Bind Modbus master to MFM serial
  node.begin(MFM_SLAVE_ID, MfmSerial);
  node.preTransmission(preTransmission);
  node.postTransmission(postTransmission);

  Serial.println("ESP32 MFM384 -> RAK3172 Bridge Initialized (scaled ×100 payload).");
  Serial.printf("RAK UART: %ld 8N1 (RX=%d, TX=%d)\n", (long)RAK_BAUD, RAK_RX_PIN, RAK_TX_PIN);
  Serial.printf("MFM UART: %ld (mode=%s) RX=%d TX=%d, DE/RE=%d\n",
                (long)MFM_BAUD,
                (MFM_SERIAL_MODE==SERIAL_8N1 ? "8N1" : "custom"),
                MFM_RX_PIN, MFM_TX_PIN, MAX485_DE_RE);
}

// ---------------- Loop ----------------
void loop() {
  const size_t N = sizeof(parametersToRead) / sizeof(parametersToRead[0]);

  // 1) Read all parameters
  for (size_t i = 0; i < N; ++i) {
    float v = readFloatInputRegister(parametersToRead[i].reg);
    parametersToRead[i].value = v;
    delay(INTER_READ_DELAY);
  }

  // 2) Build scaled ×100 payload (int32_t), same send method as reference
  int32_t payload[N];
  for (size_t i = 0; i < N; ++i) {
    float v = parametersToRead[i].value;
    if (isnan(v)) {
      payload[i] = INT32_MIN; // error marker
    } else {
      // scale ×100 and round to nearest integer
      long scaled = lroundf(v * 100.0f);
      // clamp to int32_t range just in case
      if (scaled > INT32_MAX) scaled = INT32_MAX;
      if (scaled < INT32_MIN) scaled = INT32_MIN;
      payload[i] = (int32_t)scaled;
    }
  }

  // 3) Debug print
  Serial.println("\n--- MFM384 Data (scaled ×100, order = table) ---");
  for (size_t i = 0; i < N; ++i) {
    if (payload[i] == INT32_MIN) {
      Serial.printf("%02u) %-18s : ERROR (reg %u)\n",
                    (unsigned)i, parametersToRead[i].name, parametersToRead[i].reg);
    } else {
      Serial.printf("%02u) %-18s : %.2f  [raw=%ld]\n",
                    (unsigned)i, parametersToRead[i].name,
                    payload[i] / 100.0f, (long)payload[i]);
    }
  }

  // 4) Send to RAK3172 exactly like your reference (binary blob)
  RakSerial.write((uint8_t*)payload, sizeof(payload));
  Serial.printf("Payload (%u x int32 = %u bytes) sent to RAK3172.\n",
                (unsigned)N, (unsigned)sizeof(payload));
  Serial.println("----------------------------------------");

  // Repeat every 10 seconds (match reference pacing if desired)
  delay(10000);
}
