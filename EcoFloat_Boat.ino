/*
 * EcoFloat — Boat Node (v3 FINAL, 6s survey interval)
 * Board: Heltec WiFi LoRa 32 V3 (ESP32-S3 + SX1262)
 *
 * Fixes from v2:
 *   - setDio2AsRfSwitch(true): the V3 routes its RF switch through DIO2.
 *     Without it the radio reports TX success but barely radiates.
 *   - Explicit SPI pin mapping for the radio bus.
 *   - 2KB GPS RX buffer so NMEA survives the DO retry window.
 *
 * Changes in this version:
 *   - TX interval set to 6s (was 2s). Lower duty cycle = less radio power
 *     burned, less LoRa airtime, and a much longer on-device log at the
 *     base station (3000 readings now covers ~5 hours instead of ~100 min).
 *   - DO sensor timeout remains 700ms, comfortably under the 6s cycle
 *     budget so a failed sensor read can never starve GPS parsing or TX.
 *
 * Wiring:
 *   GPS:    VCC->3V3, GND->GND, GPS TX -> GPIO 26
 *   RS485:  VCC->3V3, GND->GND, adapter T -> GPIO 48, adapter R -> GPIO 47
 *   Sensor: brown->12V, black->GND, yellow->A, blue->B
 */

#include <RadioLib.h>
#include <TinyGPSPlus.h>
#include <SPI.h>

// ---------------- Pin assignments ----------------
#define LORA_NSS    8
#define LORA_DIO1   14
#define LORA_RST    12
#define LORA_BUSY   13

#define LORA_SCK    9
#define LORA_MISO   11
#define LORA_MOSI   10

#define GPS_RX_PIN  26
#define GPS_TX_PIN  34
#define GPS_BAUD    9600

#define RS485_RX_PIN 48  // ESP32 RX  <-- adapter "T"
#define RS485_TX_PIN 47  // ESP32 TX  --> adapter "R"
#define RS485_BAUD   4800

// LoRa settings — base station MUST mirror these exactly
#define LORA_FREQ_MHZ   915.0
#define LORA_BW_KHZ     125.0
#define LORA_SF         9
#define LORA_CR         7
#define LORA_SYNC_WORD  0x12
#define LORA_TX_DBM     17
#define LORA_PREAMBLE   8
#define LORA_TCXO_V     1.6

// Survey interval: one reading + TX every 6 seconds
#define TX_INTERVAL_MS  6000UL

// Max time to wait for a DO sensor response before giving up this cycle.
// Well under the TX interval so a dead sensor can't starve GPS/TX.
#define DO_TIMEOUT_MS   700UL

SX1262 radio = new Module(LORA_NSS, LORA_DIO1, LORA_RST, LORA_BUSY);
TinyGPSPlus gps;
HardwareSerial GPSSerial(1);
HardwareSerial DOSerial(2);

uint32_t lastTx = 0;
uint32_t packetCount = 0;

// ---------------- Modbus helpers ----------------
uint16_t modbusCRC(const uint8_t *buf, uint8_t len) {
  uint16_t crc = 0xFFFF;
  for (uint8_t i = 0; i < len; i++) {
    crc ^= buf[i];
    for (uint8_t b = 0; b < 8; b++) {
      if (crc & 0x0001) crc = (crc >> 1) ^ 0xA001;
      else              crc >>= 1;
    }
  }
  return crc;
}

float bytesToFloat(const uint8_t *b) {
  uint32_t v = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
               ((uint32_t)b[2] << 8)  |  (uint32_t)b[3];
  float f;
  memcpy(&f, &v, 4);
  return f;
}

bool readDOSensor(float &doSatPct, float &doMgL, float &tempC) {
  const uint8_t query[8] = {0x01, 0x03, 0x00, 0x00, 0x00, 0x06, 0xC5, 0xC8};
  uint8_t resp[17];
  uint8_t idx = 0;
  uint32_t start = millis();
  uint32_t lastSend = 0;

  while (millis() - start < DO_TIMEOUT_MS) {
    if (millis() - lastSend > 100 && idx == 0) {
      while (DOSerial.available()) DOSerial.read();
      DOSerial.write(query, 8);
      lastSend = millis();
    }
    if (DOSerial.available()) {
      uint8_t b = DOSerial.read();
      if (idx == 0 && b != 0x01) continue;
      resp[idx++] = b;
      if (idx >= sizeof(resp)) break;
    }
  }

  if (idx < sizeof(resp))                                    return false;
  if (resp[0] != 0x01 || resp[1] != 0x03 || resp[2] != 0x0C) return false;
  uint16_t rxCrc = resp[15] | ((uint16_t)resp[16] << 8);
  if (modbusCRC(resp, 15) != rxCrc)                          return false;

  doSatPct = bytesToFloat(&resp[3]) * 100.0f;
  doMgL    = bytesToFloat(&resp[7]);
  tempC    = bytesToFloat(&resp[11]);
  return true;
}

// ---------------- Setup / loop ----------------
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println(F("EcoFloat boat node v3 (6s interval) starting..."));

  GPSSerial.setRxBufferSize(2048);   // must come BEFORE begin()
  GPSSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  DOSerial.begin(RS485_BAUD, SERIAL_8N1, RS485_RX_PIN, RS485_TX_PIN);

  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_NSS);

  int state = radio.begin(LORA_FREQ_MHZ, LORA_BW_KHZ, LORA_SF, LORA_CR,
                          LORA_SYNC_WORD, LORA_TX_DBM, LORA_PREAMBLE,
                          LORA_TCXO_V, false);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.print(F("LoRa init failed, code "));
    Serial.println(state);
    while (true) delay(1000);
  }

  radio.setDio2AsRfSwitch(true);   // CRITICAL on Heltec V3
  Serial.println(F("LoRa init OK"));
}

void loop() {
  while (GPSSerial.available()) {
    gps.encode(GPSSerial.read());
  }

  if (millis() - lastTx < TX_INTERVAL_MS) return;
  lastTx = millis();

  float doSat = NAN, doMgL = NAN, tempC = NAN;
  bool doOK = readDOSensor(doSat, doMgL, tempC);
  if (doOK) {
    Serial.printf("[DO] %.2f mg/L | %.1f%% | %.2f C\n", doMgL, doSat, tempC);
  } else {
    Serial.println(F("WARN: DO sensor read failed"));
  }

  char date[12] = "0000-00-00";
  char tim[10]  = "00:00:00";
  double lat = 0.0, lng = 0.0;
  int sats = 0;
  float hdop = 99.9;

  if (gps.location.isValid()) {
    lat = gps.location.lat();
    lng = gps.location.lng();
  }
  if (gps.date.isValid()) {
    snprintf(date, sizeof(date), "%04u-%02u-%02u",
             gps.date.year(), gps.date.month(), gps.date.day());
  }
  if (gps.time.isValid()) {
    snprintf(tim, sizeof(tim), "%02u:%02u:%02u",
             gps.time.hour(), gps.time.minute(), gps.time.second());
  }
  if (gps.satellites.isValid()) sats = gps.satellites.value();
  if (gps.hdop.isValid())       hdop = gps.hdop.hdop();

  packetCount++;
  char packet[160];
  snprintf(packet, sizeof(packet),
           "EF1,%lu,%s,%s,%.6f,%.6f,%d,%.1f,%.2f,%.1f,%.2f",
           (unsigned long)packetCount, date, tim, lat, lng, sats, hdop,
           doOK ? doMgL : -1.0f,
           doOK ? doSat : -1.0f,
           doOK ? tempC : -99.0f);

  Serial.print(F("TX: "));
  Serial.println(packet);

  int state = radio.transmit(packet);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.print(F("LoRa TX failed, code "));
    Serial.println(state);
  }
}
