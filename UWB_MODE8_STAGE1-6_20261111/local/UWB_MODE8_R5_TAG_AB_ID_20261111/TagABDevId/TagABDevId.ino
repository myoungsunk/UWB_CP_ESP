#include <Arduino.h>
#include <SPI.h>

struct RadioPins {
  char name;
  uint8_t cs;
  uint8_t rst;
};

constexpr RadioPins radios[] = {{'A', 21, 17}, {'B', 25, 27}};
constexpr uint32_t speeds[] = {1000000, 2000000};
constexpr size_t samples = 5;

uint32_t readDeviceId(const RadioPins& radio, uint32_t hz) {
  SPI.beginTransaction(SPISettings(hz, MSBFIRST, SPI_MODE0));
  digitalWrite(radio.cs, LOW);
  delayMicroseconds(2);
  SPI.transfer(0x00);
  uint32_t id = 0;
  for (unsigned int index = 0; index < 4; ++index) {
    id |= static_cast<uint32_t>(SPI.transfer(0x00)) << (8 * index);
  }
  digitalWrite(radio.cs, HIGH);
  SPI.endTransaction();
  return id;
}

void measure(const RadioPins& radio, const char* phase) {
  for (const uint32_t hz : speeds) {
    uint32_t readings[samples];
    for (uint32_t& reading : readings) {
      reading = readDeviceId(radio, hz);
      delay(2);
    }
    Serial.printf(
      "{\"event\":\"DW1000_AB_DEV_ID\",\"radio\":\"%c\","
      "\"phase\":\"%s\",\"spi_hz\":%lu,\"ids\":[",
      radio.name, phase, static_cast<unsigned long>(hz));
    for (size_t index = 0; index < samples; ++index) {
      Serial.printf("%s\"0x%08lX\"", index == 0 ? "" : ",",
                    static_cast<unsigned long>(readings[index]));
    }
    Serial.println("]}");
  }
}

void setup() {
  for (const RadioPins& radio : radios) {
    pinMode(radio.cs, OUTPUT);
    digitalWrite(radio.cs, HIGH);
    pinMode(radio.rst, INPUT);
  }
  SPI.begin(18, 19, 23, -1);
  Serial.begin(115200);
}

void loop() {
  if (Serial.read() != 'D') {
    delay(2);
    return;
  }
  Serial.println("DW1000_AB_DIAG_BEGIN");
  for (const RadioPins& radio : radios) {
    measure(radio, "before_reset");
  }
  for (const RadioPins& radio : radios) {
    digitalWrite(radio.rst, LOW);
    pinMode(radio.rst, OUTPUT);
    delay(2);
    pinMode(radio.rst, INPUT);
    delay(20);
    measure(radio, "after_reset");
  }
  Serial.println("DW1000_AB_DIAG_END");
}
