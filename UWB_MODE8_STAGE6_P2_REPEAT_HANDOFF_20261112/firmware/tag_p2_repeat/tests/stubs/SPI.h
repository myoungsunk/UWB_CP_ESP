#pragma once
#include "Arduino.h"
#define MSBFIRST 1
#define SPI_MODE0 0
class SPISettings {public:SPISettings(uint32_t=1000000,uint8_t=MSBFIRST,uint8_t=SPI_MODE0){}};
class SPIClass {public:void begin(int8_t=-1,int8_t=-1,int8_t=-1,int8_t=-1);void end();void beginTransaction(const SPISettings&);void endTransaction();uint8_t transfer(uint8_t);void transferBytes(const uint8_t*,uint8_t*,uint32_t);void usingInterrupt(int);};
extern SPIClass SPI;
