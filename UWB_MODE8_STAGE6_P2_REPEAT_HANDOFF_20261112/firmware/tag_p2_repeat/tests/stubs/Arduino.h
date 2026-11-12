#pragma once
// HOST SYNTAX ONLY. Not an ESP32 SDK and never linked into a firmware image.
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <string>
using byte=uint8_t;using boolean=bool;
#define INPUT 0
#define OUTPUT 1
#define INPUT_PULLUP 5
#define LOW 0
#define HIGH 1
#define RISING 1
#define ARDUINO_ISR_ATTR
#define IRAM_ATTR
void pinMode(uint8_t,int);void digitalWrite(uint8_t,int);int digitalRead(uint8_t);
void delay(unsigned long);void delayMicroseconds(unsigned int);unsigned long millis();
void noInterrupts();void interrupts();void attachInterruptArg(uint8_t,void(*)(void*),void*,int);
int digitalPinToInterrupt(uint8_t);void attachInterrupt(int,void(*)(),int);
class String {public: String(){}String(const char*){}size_t length()const;void getBytes(unsigned char*,unsigned int,unsigned int=0)const;};
class HardwareSerial {public:bool setRxBufferSize(size_t);bool setTxBufferSize(size_t);void begin(unsigned long);void end();void flush();size_t write(const uint8_t*,size_t);int read();int available();void println(const char*);};
extern HardwareSerial Serial;
