/**
 * @file RAK4630_Honeywell_Reader.ino
 * @brief Reads Honeywell EEM400-D-MO via Modbus and sends data over LoRaWAN
 * All values are scaled ×100 (2 decimals preserved).
 * Based on Honeywell EEM400-D-MO datasheet
 */

#include <Arduino.h>
#include <ArduinoModbus.h>
#include <LoRaWan-RAK4630.h>

//================================================================//
//======= CONFIGURATION ==========================================//
//================================================================//
#define HONEYWELL_SLAVE_ID    0x01      // Honeywell Modbus address (0x20 = 32)
#define RS485_BAUDRATE        19200
#define RS485_SERIAL_CONFIG   SERIAL_8E1  // 8 data bits, Even Parity, 1 Stop Bit

// --- Energy Counters (32-bit values) ---
#define T1_TOTAL_KWH    0x001B  // Register 28-29 
#define T1_PART_KWH     0x001D  // Register 30-31 
#define T2_TOTAL_KWH    0x001F  // Register 32-33 
#define T2_PART_KWH     0x0021  // Register 34-35 

// --- Block Reading Configuration ---
#define START_ADDRESS_BLOCK   0x0023  // Register 36
#define NUM_REGISTERS_BLOCK   17      // Reads registers 36 through 52

// Data structure to hold all meter parameters
struct HoneywellParameter {
  const char* name;
  float value;
  int32_t scaled_value; // Value × 100 for transmission
};

// All parameters we're reading from Honeywell meter
HoneywellParameter honeywellData[] = {
  // Energy counters (32-bit values)
  {"T1 Total kWh", 0.0, 0},
  {"T1 Part kWh", 0.0, 0},
  {"T2 Total kWh", 0.0, 0},
  {"T2 Part kWh", 0.0, 0},
  
  // Phase 1 parameters
  {"Voltage V1", 0.0, 0},
  {"Current I1", 0.0, 0},
  {"Active Power P1", 0.0, 0},
  {"Reactive Power Q1", 0.0, 0},
  {"Power Factor cos1", 0.0, 0},
  
  // Phase 2 parameters
  {"Voltage V2", 0.0, 0},
  {"Current I2", 0.0, 0},
  {"Active Power P2", 0.0, 0},
  {"Reactive Power Q2", 0.0, 0},
  {"Power Factor cos2", 0.0, 0},
  
  // Phase 3 parameters
  {"Voltage V3", 0.0, 0},
  {"Current I3", 0.0, 0},
  {"Active Power P3", 0.0, 0},
  {"Reactive Power Q3", 0.0, 0},
  {"Power Factor cos3", 0.0, 0},
  
  // Total values
  {"Total Active Power", 0.0, 0},
  {"Total Reactive Power", 0.0, 0}
};

// Function Prototypes
bool read_energy_counter(uint16_t address, float &result);
bool read_modbus_registers_block(uint16_t startAddress, uint16_t numRegisters, uint16_t* buffer);
void scale_and_prepare_payload();

//================================================================//
//======= SETUP ==================================================//
//================================================================//
void setup() {
  Serial.begin(115200);
  while (!Serial) {
    delay(10);
  }

  Serial.println("\n==============================================");
  Serial.println("RAK4630 Honeywell EEM400-D-MO Reader");
  Serial.println("==============================================");
  
  // Initialize RS485 transceiver
  pinMode(WB_IO2, OUTPUT);
  digitalWrite(WB_IO2, HIGH);
  delay(100);

  // Initialize Modbus RTU Client with 8E1 configuration
  if (!ModbusRTUClient.begin(RS485_BAUDRATE, RS485_SERIAL_CONFIG)) {
    Serial.println("Failed to start Modbus RTU Client! Halting.");
    while (1);
  }

  Serial.println("Modbus Client started successfully");
  Serial.printf("Configuration: %d baud, 8E1, Slave ID: 0x%02X\n", RS485_BAUDRATE, HONEYWELL_SLAVE_ID);
  Serial.println("Reading Honeywell EEM400-D-MO meter...");
}

//================================================================//
//======= MAIN LOOP ==============================================//
//================================================================//
void loop() {
  Serial.println("\n--- Reading Honeywell Meter Data ---");
  
  bool readSuccess = true;
  
  // --- 1. Read 32-bit energy counters ---
  Serial.println("Reading energy counters...");
  
  if (!read_energy_counter(T1_TOTAL_KWH, honeywellData[0].value)) {
    Serial.println("Failed to read T1 Total kWh");
    readSuccess = false;
  }
  delay(200);
  
  if (!read_energy_counter(T1_PART_KWH, honeywellData[1].value)) {
    Serial.println("Failed to read T1 Part kWh");
    readSuccess = false;
  }
  delay(200);
  
  if (!read_energy_counter(T2_TOTAL_KWH, honeywellData[2].value)) {
    Serial.println("Failed to read T2 Total kWh");
    readSuccess = false;
  }
  delay(200);
  
  if (!read_energy_counter(T2_PART_KWH, honeywellData[3].value)) {
    Serial.println("Failed to read T2 Part kWh");
    readSuccess = false;
  }
  delay(200);

  // --- 2. Read 17-register block ---
  Serial.println("Reading measurement block...");
  uint16_t registerBlock[NUM_REGISTERS_BLOCK];
  
  if (!read_modbus_registers_block(START_ADDRESS_BLOCK, NUM_REGISTERS_BLOCK, registerBlock)) {
    Serial.println("Failed to read register block!");
    readSuccess = false;
  } else {
    // Process the register block data
    for (int i = 0; i < NUM_REGISTERS_BLOCK; i++) {
      uint16_t raw = registerBlock[i];
      float scaled = 0;
      
      // Apply scaling based on parameter type
      switch (i % 5) {
        case 0: scaled = raw * 1.0; break;        // Voltage - no scaling
        case 1: scaled = raw / 10.0; break;       // Current - ÷10
        case 2: case 3: case 4: scaled = raw / 100.0; break; // Power, PF - ÷100
      }
      
      honeywellData[4 + i].value = scaled;
    }
  }

  // --- 3. Scale values for transmission ---
  scale_and_prepare_payload();

  // --- 4. Display results ---
  Serial.println("\n--- Honeywell Meter Data (scaled ×100) ---");
  Serial.printf("T1 total kWh: %.2f\n", honeywellData[0].value);
  Serial.printf("T1 part  kWh: %.2f\n", honeywellData[1].value);
  Serial.printf("T2 total kWh: %.2f\n", honeywellData[2].value);
  Serial.printf("T2 part  kWh: %.2f\n", honeywellData[3].value);
  
  Serial.printf("V1: %.2f V | I1: %.2f A | P1: %.2f kW | Q1: %.2f kvar | cos1: %.2f\n", 
                honeywellData[4].value, honeywellData[5].value, honeywellData[6].value, 
                honeywellData[7].value, honeywellData[8].value);
  Serial.printf("V2: %.2f V | I2: %.2f A | P2: %.2f kW | Q2: %.2f kvar | cos2: %.2f\n", 
                honeywellData[9].value, honeywellData[10].value, honeywellData[11].value, 
                honeywellData[12].value, honeywellData[13].value);
  Serial.printf("V3: %.2f V | I3: %.2f A | P3: %.2f kW | Q3: %.2f kvar | cos3: %.2f\n", 
                honeywellData[14].value, honeywellData[15].value, honeywellData[16].value, 
                honeywellData[17].value, honeywellData[18].value);
  Serial.printf("Total Active Power: %.2f kW | Total Reactive Power: %.2f kvar\n", 
                honeywellData[19].value, honeywellData[20].value);

  // --- 5. Here you would send data via LoRaWAN ---
  if (readSuccess) {
    Serial.println("Data ready for LoRaWAN transmission");
    // Add your LoRaWAN transmission code here
    // Example: send_lora_data(honeywellData);
  } else {
    Serial.println("Some readings failed - check connection");
  }

  Serial.println("----------------------------------------");
  delay(10000); // Wait 10 seconds before next reading
}

//================================================================//
//======= HELPER FUNCTIONS ======================================//
//================================================================//

// Read 32-bit energy counter (2 registers)
bool read_energy_counter(uint16_t address, float &result) {
  if (!ModbusRTUClient.requestFrom(HONEYWELL_SLAVE_ID, HOLDING_REGISTERS, address, 2)) {
    return false;
  }

  if (ModbusRTUClient.available() < 2) {
    return false;
  }

  // Read the two 16-bit registers
  uint16_t highWord = ModbusRTUClient.read();
  uint16_t lowWord = ModbusRTUClient.read();

  // Combine into 32-bit value
  uint32_t fullValue = ((uint32_t)highWord << 16) | lowWord;
  
  // Apply scaling (datasheet multiplier 0.01)
  result = (float)fullValue / 100.0;
  
  return true;
}

// Read a block of holding registers
bool read_modbus_registers_block(uint16_t startAddress, uint16_t numRegisters, uint16_t* buffer) {
  if (!ModbusRTUClient.requestFrom(HONEYWELL_SLAVE_ID, HOLDING_REGISTERS, startAddress, numRegisters)) {
    return false;
  }

  if (ModbusRTUClient.available() < numRegisters) {
    return false;
  }

  // Read all registers into buffer
  for (int i = 0; i < numRegisters; i++) {
    buffer[i] = ModbusRTUClient.read();
  }
  
  return true;
}

// Scale all values ×100 for transmission
void scale_and_prepare_payload() {
  int num_params = sizeof(honeywellData) / sizeof(honeywellData[0]);
  
  for (int i = 0; i < num_params; i++) {
    honeywellData[i].scaled_value = (int32_t)(honeywellData[i].value * 100);
  }
}

// Optional: Function to prepare LoRaWAN payload
void prepare_lora_payload(uint8_t* payload, size_t& length) {
  // This would prepare the actual bytes for LoRaWAN transmission
  // Based on your specific payload format requirements
  
  int num_params = sizeof(honeywellData) / sizeof(honeywellData[0]);
  length = num_params * sizeof(int32_t); // 21 parameters × 4 bytes each = 84 bytes
  
  // Copy scaled values to payload buffer
  for (int i = 0; i < num_params; i++) {
    payload[i*4] = (honeywellData[i].scaled_value >> 24) & 0xFF;
    payload[i*4 + 1] = (honeywellData[i].scaled_value >> 16) & 0xFF;
    payload[i*4 + 2] = (honeywellData[i].scaled_value >> 8) & 0xFF;
    payload[i*4 + 3] = honeywellData[i].scaled_value & 0xFF;
  }
}