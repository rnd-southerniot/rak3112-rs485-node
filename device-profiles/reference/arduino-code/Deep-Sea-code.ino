#include <ModbusMaster.h>

// ==== CONFIG ====
// List your meter slave IDs here (e.g., 10, 11, 12)
const uint8_t SLAVES[] = {10, 11, 12};
const uint8_t NUM_SLAVES = sizeof(SLAVES) / sizeof(SLAVES[0]);

#define MODBUS_BAUDRATE 115200
#define TX_PIN 16
#define RX_PIN 17
#define READ_INTERVAL 10000  // ms (delay between full cycles over all meters)

#define RAK_RX_PIN 23  // <-- Change if needed
#define RAK_TX_PIN 18  // <-- Change if needed

ModbusMaster node;

// --- Serial Ports ---
HardwareSerial RakSerial(1);
HardwareSerial ModbusSerial(2);

// Register list: {address, quantity, scale}
struct RegisterItem {
  uint16_t addr;
  uint8_t qty;
  float scale;
};

RegisterItem registers[] = {
  {1027, 1, 1.0},   // Fuel Level
  {1029, 1, 0.1},   // Engine Battery Voltage
  {1030, 1, 1.0},   // Engine Speed
  {1031, 1, 0.1},   // Generator Frequency
  {1032, 2, 0.1},   // Mains L3-L1 Voltage
  {1034, 2, 0.1},   // Generator L3-L1 Voltage
  {1036, 2, 0.1},   // Mains L1-L2 Voltage
  {1038, 2, 0.1},   // Generator L1-L2 Voltage
  {1040, 2, 0.1},   // Mains L2-L3 Voltage
  {1042, 2, 0.1},   // Generator L2-L3 Voltage
  {1059, 1, 0.1},   // Mains Frequency
  {1060, 2, 0.1},   // Generator L1-N Voltage
  {1062, 2, 0.1},   // Generator L2-N Voltage
  {1064, 2, 0.1},   // Mains L1-N Voltage
  {1066, 2, 0.1},   // Mains L2-N Voltage
  {1068, 2, 0.1},   // Mains L2-L3 Voltage
  {1070, 2, 0.1},   // Generator L3-N Voltage
  {1536, 2, 1.0},   // Generator Total Watts
  {1557, 1, 0.1},   // Generator Average Power Factor
  {1630, 1, 0.1},   // Generator Percentage of Full Power
  {1798, 2, 1.0}    // Engine Run Time
};

const char* registerNames[] = {
  "Fuel Level",
  "Engine Battery Voltage",
  "Engine Speed",
  "Generator Frequency",
  "Mains L3-L1 Voltage",
  "Generator L3-L1 Voltage",
  "Mains L1-L2 Voltage",
  "Generator L1-L2 Voltage",
  "Mains L2-L3 Voltage",
  "Generator L2-L3 Voltage",
  "Mains Frequency",
  "Generator L1-N Voltage",
  "Generator L2-N Voltage",
  "Mains L1-N Voltage",
  "Mains L2-N Voltage",
  "Mains L2-L3 Voltage",
  "Generator L3-N Voltage",
  "Generator Total Watts",
  "Generator Average Power Factor",
  "Generator Percentage of Full Power",
  "Engine Run Time"
};

const uint8_t NUM_REGISTERS = sizeof(registers) / sizeof(registers[0]);

// Read all registers for a given slave and fill payload (×100 scaled int32)
bool readAllForSlave(uint8_t slaveId, int32_t* payloadOut) {
  // (Re)bind node to this slave each time to be explicit
  node.begin(slaveId, ModbusSerial);

  bool anySuccess = false;

  Serial.printf("\n----- Reading Modbus Registers (Slave ID %u) -----\n", slaveId);

  for (uint8_t i = 0; i < NUM_REGISTERS; i++) {
    uint16_t reg  = registers[i].addr;
    uint8_t  qty  = registers[i].qty;
    float    scale = registers[i].scale;

    // Clear buffers to avoid stale data
    node.clearResponseBuffer();
    node.clearTransmitBuffer();

    uint8_t result = node.readHoldingRegisters(reg, qty);

    if (result == node.ku8MBSuccess) {
      anySuccess = true;

      if (qty == 1) {
        uint16_t value = node.getResponseBuffer(0);
        float scaled = value * scale;
        payloadOut[i] = (int32_t)(scaled * 100.0f);
        Serial.printf("%2u. %-32s = %.2f\n", i + 1, registerNames[i], scaled);
      } else if (qty == 2) {
        // Combine two 16-bit registers into 32-bit (big-endian: high, then low)
        uint32_t high = node.getResponseBuffer(0);
        uint32_t low  = node.getResponseBuffer(1);
        uint32_t combined = (high << 16) | low;
        float scaled = (float)combined * scale;
        payloadOut[i] = (int32_t)(scaled * 100.0f);
        Serial.printf("%2u. %-32s = %.2f\n", i + 1, registerNames[i], scaled);
      }
      delay(200);  // small gap between queries
    } else {
      Serial.printf("%2u. Error reading %-32s (code=%u)\n", i + 1, registerNames[i], result);
      payloadOut[i] = -1;  // mark error
    }
  }

  return anySuccess;
}

void setup() {
  Serial.begin(115200);

  // RS-485/Modbus port: 8N2 as in your original
  ModbusSerial.begin(MODBUS_BAUDRATE, SERIAL_8N2, RX_PIN, TX_PIN);

  // RAK UART bridge
  RakSerial.begin(115200, SERIAL_8N1, RAK_RX_PIN, RAK_TX_PIN);

  // Initialize once to any slave; we rebind inside readAllForSlave()
  node.begin(SLAVES[0], ModbusSerial);

  Serial.println("=== ESP32 Modbus Reader (3 meters) + RAK Bridge ===");
  Serial.print("Slaves: ");
  for (uint8_t i = 0; i < NUM_SLAVES; i++) {
    Serial.print(SLAVES[i]);
    if (i < NUM_SLAVES - 1) Serial.print(", ");
  }
  Serial.println();
}

void loop() {
  // Iterate over each meter
  for (uint8_t s = 0; s < NUM_SLAVES; s++) {
    uint8_t slaveId = SLAVES[s];

    int32_t payload[NUM_REGISTERS];
    bool ok = readAllForSlave(slaveId, payload);

    // Send this meter's payload to RAK (same binary format as before: array of int32_t)
    RakSerial.write((uint8_t*)payload, sizeof(payload));
    Serial.printf("Payload (Slave %u) sent to RAK3172. (%u bytes)\n", slaveId, (unsigned)sizeof(payload));

    // Optional: small delay between different meters to keep the bus calm
    delay(300);
  }

  Serial.println("----------------------------------------");
  delay(READ_INTERVAL);  // wait before the next full round over all meters
}
