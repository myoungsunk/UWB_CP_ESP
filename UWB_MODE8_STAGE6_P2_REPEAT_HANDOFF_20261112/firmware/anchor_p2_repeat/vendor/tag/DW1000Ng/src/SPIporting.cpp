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

#include <Arduino.h>
#include <SPI.h>
#include <soc/spi_reg.h>
#include "SPIporting.hpp"
#include "DW1000NgConstants.hpp"
#include "DW1000NgRegisters.hpp"

    
static SPIClass *_spi;
static uint32_t savedCtrl2=0,lastAppliedCtrl2=0;
static const bool* rxState=nullptr;
static uint32_t probeHz=0,probeDelay=0,probeGap=0;
static uint32_t rxTransactions=0,rxViolations=0,lastRxClock=0,lastRxCtrl2=0;

namespace SPIporting {
	
	namespace {

		constexpr uint32_t EspSPImaximumSpeed = 20000000; //20MHz
		constexpr uint32_t ArduinoSPImaximumSpeed = 16000000; //16MHz
		constexpr uint32_t SPIminimumSpeed = 2000000; //2MHz

		/* SPI relative variables */
		#if defined(ESP32) || defined(ESP8266)
			const SPISettings _fastSPI = SPISettings(EspSPImaximumSpeed, MSBFIRST, SPI_MODE0);
		#else
			const SPISettings _fastSPI = SPISettings(ArduinoSPImaximumSpeed, MSBFIRST, SPI_MODE0);
		#endif
		const SPISettings _rxSPI = SPISettings(2000000, MSBFIRST, SPI_MODE0);
        unsigned rxScopeDepth = 0;
		const SPISettings _slowSPI = SPISettings(SPIminimumSpeed, MSBFIRST, SPI_MODE0);
		const SPISettings _diagSPI10 = SPISettings(10000000, MSBFIRST, SPI_MODE0); // R1J6_SPIAUDIT_E1E4_V1
		bool _diagFast10 = false;
		const SPISettings* _currentSPI = &_fastSPI;
		bool _slowReadShiftFix = false;
                void (*_e5WriteObserver)(const uint8_t*,uint8_t,const uint8_t*,uint16_t)=nullptr; // R1J6_E5_ARM_TRACE_V1

        void _beginTransaction(const SPISettings& normal,bool scopedRx,bool normal20){
            const bool receiving=(rxState && *rxState);
            const bool useRx=receiving||scopedRx;
            if(probeGap)delayMicroseconds(probeGap);
            _spi->beginTransaction(probeHz ? SPISettings(probeHz,MSBFIRST,SPI_MODE0) : (useRx ? _rxSPI : normal));
            savedCtrl2=REG_READ(SPI_CTRL2_REG(3));
            uint32_t ctrl=savedCtrl2;
            if(useRx)ctrl=(ctrl & ~(SPI_MISO_DELAY_MODE_M|SPI_MISO_DELAY_NUM_M))|(2U<<SPI_MISO_DELAY_MODE_S);
            else if(normal20)ctrl &= ~(SPI_MISO_DELAY_MODE_M|SPI_MISO_DELAY_NUM_M);
            if(probeHz)ctrl=(ctrl & ~(SPI_MISO_DELAY_MODE_M|SPI_MISO_DELAY_NUM_M))|(probeDelay<<SPI_MISO_DELAY_MODE_S);
            REG_WRITE(SPI_CTRL2_REG(3),ctrl);
            lastAppliedCtrl2=REG_READ(SPI_CTRL2_REG(3));
            if(receiving){
                ++rxTransactions;lastRxClock=REG_READ(SPI_CLOCK_REG(3));lastRxCtrl2=lastAppliedCtrl2;
                if(lastRxClock!=0x4c1001 || (lastRxCtrl2&(SPI_MISO_DELAY_MODE_M|SPI_MISO_DELAY_NUM_M))!=(2U<<SPI_MISO_DELAY_MODE_S))++rxViolations;
            }
        }
        void _openSPI(uint8_t slaveSelectPIN){
            _beginTransaction((_diagFast10 && _currentSPI==&_fastSPI)?_diagSPI10:*_currentSPI,_currentSPI==&_rxSPI,_currentSPI==&_fastSPI && !_diagFast10);
            digitalWrite(slaveSelectPIN,LOW);
        }

    	void _closeSPI(uint8_t slaveSelectPIN) {
			digitalWrite(slaveSelectPIN, HIGH);
			REG_WRITE(SPI_CTRL2_REG(3),probeHz ? ((savedCtrl2 & ~(SPI_MISO_DELAY_MODE_M|SPI_MISO_DELAY_NUM_M)) | (probeDelay<<SPI_MISO_DELAY_MODE_S)) : (rxState ? ((savedCtrl2 & ~(SPI_MISO_DELAY_MODE_M|SPI_MISO_DELAY_NUM_M)) | (*rxState ? (2U<<SPI_MISO_DELAY_MODE_S) : 0U)) : savedCtrl2));
			_spi->endTransaction();
		}
	}

	void SPIinit(SPIClass &spi) {
		_spi = &spi;
		_spi->begin();
	}

	void SPIend() {
		_spi->end();
	}

	void SPIselect(uint8_t slaveSelectPIN, uint8_t irq) {
		#if !defined(ESP32) && !defined(ESP8266)
			if(irq != 0xff)
				_spi->usingInterrupt(digitalPinToInterrupt(irq));
		#endif
		pinMode(slaveSelectPIN, OUTPUT);
		digitalWrite(slaveSelectPIN, HIGH);
	}

	void writeToSPI(uint8_t slaveSelectPIN, uint8_t headerLen, byte header[], uint16_t dataLen, byte data[]) {
                if(_e5WriteObserver)_e5WriteObserver(header,headerLen,data,dataLen);
		_openSPI(slaveSelectPIN);
		for(auto i = 0; i < headerLen; i++) {
			_spi->transfer(header[i]); // send header
		}
		for(auto i = 0; i < dataLen; i++) {
			_spi->transfer(data[i]); // write values
		}
		delayMicroseconds(5);
		_closeSPI(slaveSelectPIN);
	}

    void readFromSPI(uint8_t slaveSelectPIN, uint8_t headerLen, byte header[], uint16_t dataLen, byte data[]){
		_openSPI(slaveSelectPIN);

		// On the affected ESP32 + DW1000 Anchor hardware, low-speed (2 MHz)
		// MISO reads are shifted left by exactly one bit across byte boundaries.
		// V3 diagnostics proved that writes are correct and that reconstructing each
		// byte with the previous raw byte LSB restores the exact 16 MHz reference.
		// Capture the final header RX byte because its LSB is the carry into the
		// first data byte. FAST reads and all writes remain untouched.
		uint8_t headerRx = 0;
		for(auto i = 0; i < headerLen; i++) {
			headerRx = _spi->transfer(header[i]); // send header, retain RX carry
		}

		if(_slowReadShiftFix && _currentSPI == &_slowSPI && !(rxState && *rxState)) {
			uint8_t carry = headerRx & 0x01U;
			for(auto i = 0; i < dataLen; i++) {
				const uint8_t raw = _spi->transfer(0x00);
				data[i] = static_cast<uint8_t>((carry << 7) | (raw >> 1));
				carry = raw & 0x01U;
			}
		} else {
			for(auto i = 0; i < dataLen; i++) {
				data[i] = _spi->transfer(0x00); // read values
			}
		}
		delayMicroseconds(5);
		_closeSPI(slaveSelectPIN);
	}

	RxSpeedScope::RxSpeedScope(bool enabled):previous(nullptr),enabled(enabled) {
        if(enabled){previous=_currentSPI;++rxScopeDepth;_currentSPI=&_rxSPI;}
    }
    RxSpeedScope::~RxSpeedScope(){
        if(enabled){--rxScopeDepth;_currentSPI=previous;}
    }
    void setProbeTiming(uint32_t hz,uint32_t delayMode,uint32_t gap){probeHz=hz;probeDelay=delayMode;probeGap=gap;}
    void bindRxState(const bool* state){rxState=state;}
    void beginExternalTransaction(uint32_t hz){_beginTransaction(SPISettings(hz,MSBFIRST,SPI_MODE0),false,hz==20000000);}
    void endExternalTransaction(){REG_WRITE(SPI_CTRL2_REG(3),probeHz ? ((savedCtrl2 & ~(SPI_MISO_DELAY_MODE_M|SPI_MISO_DELAY_NUM_M)) | (probeDelay<<SPI_MISO_DELAY_MODE_S)) : (rxState ? ((savedCtrl2 & ~(SPI_MISO_DELAY_MODE_M|SPI_MISO_DELAY_NUM_M)) | (*rxState ? (2U<<SPI_MISO_DELAY_MODE_S) : 0U)) : savedCtrl2));_spi->endTransaction();}
    uint32_t rxTransactionCount(){return rxTransactions;}
    uint32_t rxViolationCount(){return rxViolations;}
    uint32_t rxLastClock(){return lastRxClock;}
    uint32_t rxLastCtrl2(){return lastRxCtrl2;}
    uint32_t lastTransactionCtrl2(){return lastAppliedCtrl2;}
    void setSlowReadShiftFix(bool enabled) {
		_slowReadShiftFix = enabled;
	}

	void setDiagnosticFast10(bool enabled) { _diagFast10 = enabled; } // R1J6_SPIAUDIT_E1E4_V1
	void setE5WriteObserver(void (*fn)(const uint8_t*,uint8_t,const uint8_t*,uint16_t)) { _e5WriteObserver=fn; } // R1J6_E5_ARM_TRACE_V1
             void setSPIspeed(SPIClock speed) {
		if(speed == SPIClock::FAST) {
			_currentSPI = rxScopeDepth ? &_rxSPI : &_fastSPI;
		 } else if(speed == SPIClock::SLOW) {
			_currentSPI = &_slowSPI;
		 }
	}

}