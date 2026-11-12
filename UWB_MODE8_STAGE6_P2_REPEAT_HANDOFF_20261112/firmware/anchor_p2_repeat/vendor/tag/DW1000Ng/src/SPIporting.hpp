/*
 * MIT License
 * 
 * Copyright (c) 2018 Michele Biondi, Andrea Salvatori
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 * 
 * @file SPIporting.hpp
 * Arduino porting for the SPI interface.
*/

#pragma once

#include <Arduino.h>
#include "DW1000NgConstants.hpp"
#include <SPI.h>

namespace SPIporting{
    // Main-loop-only RX scope. Preserve initialization/TX FAST settings.
    class RxSpeedScope {
        const SPISettings* previous;
        bool enabled;
    public:
        explicit RxSpeedScope(bool enabled=true);
        ~RxSpeedScope();
        RxSpeedScope(const RxSpeedScope&)=delete;
        RxSpeedScope& operator=(const RxSpeedScope&)=delete;
    };

    /** 
	Initializes the SPI bus.
	*/
    void SPIinit(SPIClass &spi = SPI);

    /** 
	Tells the driver library that no communication to a DW1000 will be required anymore.
	This basically just frees SPI and the previously used pins.
	*/
	void SPIend();

    /** 
	(Re-)selects a specific DW1000 chip for communication. Used in case you switched SPI to another device.
	*/
	void SPIselect(uint8_t slaveSelectPIN, uint8_t irq = 0xff);

    /**
    Arduino function to write to the SPI.
    Takes two separate byte buffers for write header and write data

    @param [in] Header lenght
    @param [in] Header array built before 
    @param [in] Data lenght
    @param [in] Data array 
    */
    void writeToSPI(uint8_t slaveSelectPIN, uint8_t headerLen, byte header[], uint16_t dataLen, byte data[]);

    /**
    Arduino function to read from the SPI.
    Takes two separate byte buffers for write header and write data

    @param [in] Header lenght
    @param [in] Header array built before 
    @param [in] Data lenght
    @param [out] Data array 
    */
    void readFromSPI(uint8_t slaveSelectPIN, uint8_t headerLen, byte header[], uint16_t dataLen, byte data[]);

    /**
    Enables the ESP32/DW1000 2 MHz MISO one-bit-shift correction for SLOW reads.
    This is an opt-in workaround; FAST reads and all writes are unaffected.
    */
    void setProbeTiming(uint32_t hz,uint32_t delayMode,uint32_t gap);
    void bindRxState(const bool* state);
    void beginExternalTransaction(uint32_t hz);
    void endExternalTransaction();
    uint32_t rxTransactionCount();
    uint32_t rxViolationCount();
    uint32_t rxLastClock();
    uint32_t rxLastCtrl2();
    uint32_t lastTransactionCtrl2();
    void setSlowReadShiftFix(bool enabled);

    /**
    Sets speed of SPI clock, fast or slow(20MHz or 2MHz)

    @param [in] SPIClock FAST or SLOW
    */
    void setDiagnosticFast10(bool enabled); // R1J6_SPIAUDIT_E1E4_V1
    void setE5WriteObserver(void (*fn)(const uint8_t*,uint8_t,const uint8_t*,uint16_t)); // R1J6_E5_ARM_TRACE_V1
    void setSPIspeed(SPIClock speed);

}