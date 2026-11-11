#line 1 "D:\\SLAM_bot\\artifacts\\UWB_MODE8_STAGE6_CONTINUOUS_RX_UARTFIX_20261111\\anchor\\firmware\\ANCHOR_DUAL_DS_CIR\\esp32_port.hpp"
#pragma once
#include <Arduino.h>
#include <SPI.h>
#include <DW1000Ng.hpp>
#include <SPIporting.hpp>
#include <esp_timer.h>
#include "engine.hpp"
#include "rx_guard_policy.hpp"
namespace dual8 {
class Esp32Port:public Port {
 static constexpr unsigned N=DUAL8_ANCHOR?1:2;
 Config cfg_;Calibration calibration_[2]{};bool rx_[2]{},ready_[2]{},latched_[2]{};uint64_t arm_us_[2]{};
 uint8_t active_=0;uint64_t lastYield_=0;uint8_t accum_[CIR_BYTES+1]{},repeat_[CIR_BYTES]{};
 Pins pins(unsigned i)const{return DUAL8_ANCHOR?ANCHOR_PINS:TAG_PINS[i];}
 void choose(uint8_t i){
  active_=i;
#if !DUAL8_ANCHOR
  DW1000Ng::select(i);SPIporting::bindRxState(&rx_[i]);
#endif
 }
 void read(uint8_t reg,uint16_t off,uint8_t*d,uint16_t n){
  byte h[3];uint8_t k=1;h[0]=reg&63;if(off!=0xffff){h[0]|=0x40;h[k++]=off&127;if(off>127){h[1]|=0x80;h[k++]=uint8_t(off>>7);}}
  SPIporting::readFromSPI(pins(active_).cs,k,h,n,d);
 }
 void write(uint8_t reg,uint16_t off,uint8_t*d,uint16_t n){
  byte h[3];uint8_t k=1;h[0]=0x80|(reg&63);if(off!=0xffff){h[0]|=0x40;h[k++]=off&127;if(off>127){h[1]|=0x80;h[k++]=uint8_t(off>>7);}}
  SPIporting::writeToSPI(pins(active_).cs,k,h,n,d);
 }
 uint64_t status(){uint8_t b[5];read(0x0f,0xffff,b,5);return getLE(b,5);}
 void clear(uint64_t mask){uint8_t b[5];putLE(b,mask,5);write(0x0f,0xffff,b,5);}
 RxReadIssue readRxFrame(Frame&f,Rx&o){
  o.bytes=DW1000Ng::getReceivedDataLength();
  if(o.bytes!=AIR_BYTES)return RxReadIssue::Length;
  DW1000Ng::getReceivedData(o.air,AIR_BYTES);
  return decode(o.air,AIR_BYTES,f)?RxReadIssue::None:RxReadIssue::Decode;
 }
#if DUAL8_ANCHOR
 struct ClockSample {
  uint64_t status;
  uint8_t rf, detector;
  bool usable()const{
   return rxguard::clockCheckReadable(status,rf,detector)&&rxguard::settledStartupClock(status,rf);
  }
 };
 DW1000Ng::InitFailure startupFailure_{};
 ClockSample lastClockFault_{};
 bool clockFaultRecorded_=false;
 void tripClock(uint8_t i,const ClockSample&sample){
  lastClockFault_=sample;clockFaultRecorded_=true;
  trip(i,sample.status);
 }
 ClockSample observeClock(){
  ClockSample c{};
  read(0x28,0x2c,&c.rf,1);
  read(0x24,0,&c.detector,1);
  c.status=status();
  return c;
 }
 bool registerMatches(DW1000Ng::InitFailure&failure,const char* step,
                      uint8_t reg,uint16_t off,uint8_t length,uint32_t mask,uint32_t expected){
  uint8_t bytes[4];read(reg,off,bytes,length);
  return failure.check(step,reg,off,mask,expected,getLE(bytes,length));
 }
 bool verifyPhy(DW1000Ng::InitFailure&failure){
  // Mode 8 uses channel 5, standard SFD/PHR, 850 kbps, PRF16, preamble 256/code 3.
  constexpr uint32_t sys=(1u<<12)|(1u<<18); // single RX buffer and manual TX power
  constexpr uint32_t sysMask=sys|(1u<<0)|(1u<<11)|(3u<<16)|(1u<<22)|(7u<<28);
  constexpr uint32_t chanMask=0xffu|(0x7fffu<<17);
  constexpr uint32_t chan=0x55u|(1u<<18)|(3u<<22)|(3u<<27);
  constexpr uint32_t tx=(1u<<13)|(1u<<16)|(9u<<18);
  constexpr uint32_t txMask=0xffff6000u; // TR (bit 15) marks ranging frames in the PHY header.
  if(registerMatches(failure,"SYS_CFG",0x04,0xffff,4,sysMask,sys)&&
     registerMatches(failure,"CHAN_CTRL",0x1f,0xffff,4,chanMask,chan)&&
     registerMatches(failure,"TX_FCTRL",0x08,0xffff,4,txMask,tx)&&
     registerMatches(failure,"FS_PLLCFG",0x2b,0x07,4,0xffffffffu,0x0800041du)&&
     registerMatches(failure,"FS_PLLTUNE",0x2b,0x0b,1,0xffu,0xbeu))return true;
  trip(0,status());return false;
 }
 bool failStartupClock(uint8_t i,bool allowSticky){
  const uint64_t st=pll_startup.after;
  const uint8_t rf=pll_startup.rf_after, ec=pll_startup.ec_ctrl;
  const uint64_t pllMask=allowSticky?rxguard::RF_PLL_LOSS:rxguard::PLL;
  if(st==MASK40)
   startupFailure_.reject("PLL_SYS_STATUS_READ",0x0f,0xffff,MASK40,MASK40,st,"not_equal");
  else if(st&pllMask)
   startupFailure_.check("PLL_SYS_STATUS",0x0f,0xffff,pllMask,0,st);
  else if(rf==0xff)
   startupFailure_.reject("PLL_RF_STATUS_READ",0x28,0x2c,0xff,0xff,rf,"not_equal");
  else if(ec==0xff)
   startupFailure_.reject("PLL_DETECTOR_READ",0x24,0,0xff,0xff,ec,"not_equal");
  else if(!(ec&4))
   startupFailure_.check("PLL_DETECTOR_ENABLE",0x24,0,4,4,ec);
  else
   startupFailure_.check("PLL_RF_STATUS",0x28,0x2c,7,1,rf);
  pll_startup.clock_failure=rxguard::clockCheckReadable(
    pll_startup.after,pll_startup.rf_after,pll_startup.ec_ctrl);
  tripClock(i,{pll_startup.after,pll_startup.rf_after,pll_startup.ec_ctrl});return false;
 }
 bool settleStartupClock(uint8_t i){
  const auto first=observeClock();
  pll_startup.before=pll_startup.after=first.status;
  pll_startup.rf_before=pll_startup.rf_after=first.rf;
  pll_startup.ec_ctrl=first.detector;
  if(!rxguard::canClearStartupClockEvent(pll_startup.before,pll_startup.rf_before,pll_startup.ec_ctrl))
   return failStartupClock(i,true);
  // CLKPLL_LL is sticky. Acknowledge a recovered boot event once, with RF idle.
  if(pll_startup.before&rxguard::CLOCK_PLL_LOSS){
   clear(rxguard::CLOCK_PLL_LOSS);pll_startup.acknowledged=true;
  }
  // The affected anchor can report CPLLLOW a few hundred ms after reset.
  const uint64_t deadline=nowUs()+3000000;
  do {
   delay(25);
   const auto sample=observeClock();
   pll_startup.after=sample.status;pll_startup.rf_after=sample.rf;pll_startup.ec_ctrl=sample.detector;
   if(!sample.usable())return failStartupClock(i,false);
  } while(nowUs()<deadline);
  pll_startup.ready=true;
  return true;
 }
#endif
 void trip(uint8_t i,uint64_t st){
  ++faults;fault_status[i]=st;
#if DUAL8_ANCHOR
  SPIporting::setSPIspeed(SPIClock::SLOW);
#endif
  if(!latched_[i]){choose(i);DW1000Ng::forceTRxOff();} // One best-effort shutdown; do not claim physical confirmation.
  latched_[i]=true;ready_[i]=false;rx_[i]=false;
 }
 bool guard(uint8_t i,bool startup=false){
  if(i>=N||latched_[i]||!ready_[i]){return false;}
  choose(i);
  uint8_t dev[4],tx[2],rr[2],t1[5],t2[5];read(0,0xffff,dev,4);read(0x18,0xffff,tx,2);read(0x2e,0x1804,rr,2);
  read(6,0xffff,t1,5);read(6,0xffff,t2,5);
  last_dev[i]=uint32_t(getLE(dev,4));last_ant_tx[i]=uint16_t(getLE(tx,2));last_ant_rx[i]=uint16_t(getLE(rr,2));
  const uint64_t clockBefore=getLE(t1,5),clockAfter=getLE(t2,5);
  if(last_dev[i]!=0xdeca0130||last_ant_tx[i]!=calibration_[i].tx||last_ant_rx[i]!=calibration_[i].rx||!rxguard::goodClock(clockBefore,clockAfter)){
#if DUAL8_ANCHOR
   if(startup&&startupFailure_.check("DEV_ID",0,0xffff,0xffffffffu,0xdeca0130u,last_dev[i])&&
      startupFailure_.check("TX_ANTENNA_DELAY",0x18,0xffff,0xffff,calibration_[i].tx,last_ant_tx[i])&&
      startupFailure_.check("RX_ANTENNA_DELAY",0x2e,0x1804,0xffff,calibration_[i].rx,last_ant_rx[i])){
    if(clockBefore==0||clockBefore==MASK40)
     startupFailure_.reject("SYS_TIME_READ",6,0xffff,MASK40,clockBefore,clockBefore,"not_equal");
    else if(clockAfter==0||clockAfter==MASK40)
     startupFailure_.reject("SYS_TIME_READ",6,0xffff,MASK40,clockAfter,clockAfter,"not_equal");
    else
     startupFailure_.reject("SYS_TIME_PROGRESS",6,0xffff,MASK40,clockBefore,clockAfter,"advances_from");
   }
#else
   (void)startup;
#endif
   trip(i,status());return false;
  }
  return true;
 }
 bool recover(uint8_t i,uint64_t st){
  if(!guard(i))return false;
  clear(st&rxguard::RX_CLEAR);DW1000Ng::forceTRxOff();rx_[i]=false;
  uint8_t low=0xe0,high=0xf0;write(0x36,3,&low,1);write(0x36,3,&high,1);++rx_resets[i];return guard(i);
 }
 void readDiag(Rx&o){
  const uint64_t begin=nowUs();uint8_t fi[4],fq[8],rt[14],ci[3];read(0x10,0xffff,fi,4);read(0x12,0xffff,fq,8);read(0x15,0xffff,rt,14);read(0x27,0x28,ci,3);
  o.fp_raw=uint16_t(getLE(rt+5,2));o.f1=uint16_t(getLE(rt+7,2));o.noise=uint16_t(getLE(fq,2));o.f2=uint16_t(getLE(fq+2,2));o.f3=uint16_t(getLE(fq+4,2));o.pwr=uint16_t(getLE(fq+6,2));
  o.nacc=uint16_t((fi[2]>>4)|(uint16_t(fi[3])<<4));uint32_t cr=uint32_t(getLE(ci,3))&0x1fffff;o.ci=cr&0x100000?int32_t(cr)-0x200000:int32_t(cr);
  o.diag=true;o.diag_us=uint32_t(nowUs()-begin);
 }
 bool copyAccumulator(uint8_t i,uint8_t*dst,uint32_t&us){
  choose(i);if(rx_[0]||rx_[1]||latched_[i])return false;
  const uint64_t start=nowUs();uint8_t old[2],on[2];read(0x36,0,old,2);on[0]=(old[0]&0xb3)|0x48;on[1]=old[1]|0x80;write(0x36,0,on,2);
#if !DUAL8_ANCHOR
  SPIporting::beginExternalTransaction(cfg_.cir_hz);
#else
  SPI.beginTransaction(SPISettings(cfg_.cir_hz,MSBFIRST,SPI_MODE0));
#endif
  digitalWrite(pins(i).cs,LOW);SPI.transfer(uint8_t(0x25));SPI.transferBytes(nullptr,accum_,CIR_BYTES+1);delayMicroseconds(5);digitalWrite(pins(i).cs,HIGH);
#if !DUAL8_ANCHOR
  SPIporting::endExternalTransaction();
#else
  SPI.endTransaction();
#endif
  write(0x36,0,old,2);memcpy(dst,accum_+1,CIR_BYTES);us=uint32_t(nowUs()-start);return true;
 }
public:
#if DUAL8_ANCHOR
 struct PllStartup{
  uint64_t before=0,after=0,first_poll_pre_tx=0;
  uint64_t first_poll_entry=0,first_poll_after_idle=0,first_poll_after_clear=0;
  uint64_t first_poll_after_schedule=0,first_poll_after_buffer=0;
  uint64_t retry_status=0;
  uint8_t rf_before=0,rf_after=0,ec_ctrl=0;
  uint8_t attempts=1,retry_rf=0;
  bool acknowledged=false,ready=false,first_poll_checked=false,clock_failure=false;
 } pll_startup;
 struct PllLive{
  uint64_t fast=0,slow=0;
  uint8_t rf_fast=0,rf_slow=0,detector=0;
  bool ready=false,fast_sampled=false;
 };
 PllLive pllLive(){
  choose(0);
  SPIporting::setSPIspeed(SPIClock::SLOW);
  const auto sample=observeClock();
  PllLive live;
  live.slow=sample.status;live.rf_slow=sample.rf;live.detector=sample.detector;
  live.ready=ready_[0]&&!latched_[0]&&sample.usable();
  // Anchor operations stay at 2 MHz. Keep the legacy fast diagnostic fields
  // with fast_sampled=false so callers can tell there is no fast observation.
  return live;
 }
 const DW1000Ng::InitFailure& startupFailure()const{return startupFailure_;}
 bool clockFaultRecorded()const{return clockFaultRecorded_;}
 uint8_t faultRf()const{return lastClockFault_.rf;}
 uint8_t faultDetector()const{return lastClockFault_.detector;}
 bool verifyIdleClock(){
  if(latched_[0]||!ready_[0])return false;
  choose(0);
  const auto sample=observeClock();
  if(sample.usable())return true;
  tripClock(0,sample);return false;
 }
 const char* verifyStart(){
  if(!verifyIdleClock())return "ANCHOR_CLOCK_NOT_READY";
  if(!guard(0))return "ANCHOR_DEVICE_NOT_READY";
  DW1000Ng::InitFailure ignored;
  if(!verifyPhy(ignored))return "ANCHOR_PHY_MISMATCH";
  if(!verifyIdleClock())return "ANCHOR_CLOCK_NOT_READY";
  return nullptr;
 }
#endif
 uint32_t faults=0,rejected[2]{},rx_resets[2]{},last_dev[2]{};uint16_t last_ant_tx[2]{},last_ant_rx[2]{};uint64_t fault_status[2]{};
 uint64_t nowUs()override{return uint64_t(esp_timer_get_time());}
 void configure(const Config&c){cfg_=c;}
 const Calibration& calibration(unsigned i)const{return calibration_[i];}
 bool faulted()const{return latched_[0]||latched_[1];}
 bool initialize(){
#if DUAL8_ANCHOR
  pll_startup=PllStartup{};startupFailure_=DW1000Ng::InitFailure{};
  if(initializeRadios())return true;
  if(!pll_startup.clock_failure)return false;
  const uint64_t firstStatus=pll_startup.after;
  const uint8_t firstRf=pll_startup.rf_after;
  pll_startup=PllStartup{};
  pll_startup.attempts=2;pll_startup.retry_status=firstStatus;pll_startup.retry_rf=firstRf;
#endif
  return initializeRadios();
 }
private:
 bool initializeRadios(){
  // Both CS pins high before any library initialization; never configure tag GPIO4 or GPIO5.
  for(unsigned i=0;i<N;++i){digitalWrite(pins(i).cs,HIGH);pinMode(pins(i).cs,OUTPUT);pinMode(pins(i).irq,INPUT);pinMode(pins(i).rst,INPUT);}
  SPI.begin(18,19,23,pins(0).cs);
  for(unsigned i=0;i<N;++i){
   rx_[i]=false;ready_[i]=false;latched_[i]=false;choose(uint8_t(i));
#if DUAL8_ANCHOR
   clockFaultRecorded_=false;lastClockFault_=ClockSample{};
   SPIporting::setSPIspeed(SPIClock::SLOW);
   if(!DW1000Ng::initializeNoInterruptChecked(pins(i).cs,pins(i).rst,startupFailure_)){
    trip(uint8_t(i),status());return false;
   }
#else
   SPIporting::setSlowReadShiftFix(false);DW1000Ng::initializeNoInterrupt(uint8_t(i),pins(i).cs,pins(i).rst,SPI);
   SPIporting::setSPIspeed(SPIClock::FAST);
#endif
   device_configuration_t dc={false,false,false,true,false,SFDMode::STANDARD_SFD,Channel::CHANNEL_5,DataRate::RATE_850KBPS,PulseFrequency::FREQ_16MHZ,PreambleLength::LEN_256,PreambleCode::CODE_3};
   DW1000Ng::applyConfiguration(dc);DW1000Ng::disableFrameFiltering();DW1000Ng::setNetworkId(10);DW1000Ng::setDeviceAddress(uint16_t(DUAL8_ANCHOR?5:6+i));
   DW1000Ng::setTxAntennaDelay(calibration_[i].tx);DW1000Ng::setRxAntennaDelay(calibration_[i].rx);DW1000Ng::setWait4Response(0);DW1000Ng::setDoubleBuffering(false);DW1000Ng::setReceiveFrameWaitTimeoutPeriod(0);
   // Polling implementation: IRQ pins are inputs, not interrupt-owned SPI users.
   uint8_t mask[4]{};write(0x0e,0xffff,mask,4);clear(rxguard::RX_CLEAR|rxguard::TX_CLEAR|(1ULL<<27)|(1ULL<<34));
#if DUAL8_ANCHOR
   if(!verifyPhy(startupFailure_)||!settleStartupClock(uint8_t(i)))return false;
#endif
   ready_[i]=true;if(!guard(uint8_t(i),true))return false;
  }
  return true;
 }
public:
 bool setCalibration(unsigned i,const Calibration&v){
  if(i>=N||faulted())return false;
  for(unsigned j=0;j<N;++j){idle(uint8_t(j));}
  choose(uint8_t(i));calibration_[i]=v;DW1000Ng::setTxAntennaDelay(v.tx);DW1000Ng::setRxAntennaDelay(v.rx);return guard(uint8_t(i));
 }
 bool healthy(uint8_t i)override{return i<N?guard(i):false;}
 uint64_t system(uint8_t i)override{if(i>=N||latched_[i])return 0;choose(i);return DW1000Ng::getSystemTimestamp()&MASK40;}
 void idle(uint8_t i)override{
  if(i>=N){return;}
  choose(i);if(!latched_[i]&&ready_[i])DW1000Ng::forceTRxOff();rx_[i]=false;
 }
 bool arm(uint8_t i)override{
  if(i>=N||!guard(i)){return false;}
#if DUAL8_ANCHOR
  if(!verifyIdleClock())return false;
#endif
  choose(i);if(rx_[i])return true;
#if !DUAL8_ANCHOR
  SPIporting::RxSpeedScope scope(true);
#endif
  DW1000Ng::forceTRxOff();clear(rxguard::RX_CLEAR);rx_[i]=true;
#if DUAL8_ANCHOR
  DW1000Ng::startReceive(ReceiveMode::IMMEDIATE);
#else
  DW1000Ng::startReceive(ReceiveMode::IMMEDIATE,true);
#endif
  arm_us_[i]=nowUs();return guard(i);
 }
 int receive(uint8_t i,Frame&f,Rx&o)override{
  if(i>=N||latched_[i]){return -2;}
  choose(i);if(!rx_[i])return 0;
  const uint64_t st=status();o.status=st;const auto action=rxguard::classify(st);
  if(action==rxguard::Action::Untrusted||action==rxguard::Action::ClockFault||action==rxguard::Action::Overrun||action==rxguard::Action::FilterReject){trip(i,st);return -2;}
  if(action==rxguard::Action::Failed||action==rxguard::Action::Timeout){++rejected[i];return recover(i,st)?-1:-2;}
  if(action!=rxguard::Action::Done)return 0;
  if(!guard(i))return -2;
  o=Rx{};o.radio=i;o.status=st;o.esp_us=nowUs();o.arm_us=arm_us_[i];o.lde=(st&(1ULL<<10))!=0;
  auto issue=readRxFrame(f,o);
#if DUAL8_ANCHOR
  if(cfg_.usesExtendedSchedule()&&issue!=RxReadIssue::None){
   o.first_read_issue=issue;o.first_read_length=o.bytes;
   // Single buffering and disabled auto-RX keep this frame latched until clear.
   o.status=status();
   if(rxguard::classify(o.status)!=rxguard::Action::Done){trip(i,o.status);return -2;}
   issue=readRxFrame(f,o);
   o.status=status();
   if(rxguard::classify(o.status)!=rxguard::Action::Done){trip(i,o.status);return -2;}
   o.lde=o.lde&&(o.status&(1ULL<<10));
  }
#endif
  if(issue!=RxReadIssue::None){idle(i);clear(rxguard::RX_CLEAR);++rejected[i];return -1;}
  o.hw=DW1000Ng::getReceiveTimestamp()&MASK40;o.raw=DW1000Ng::getReceiveTimestampRaw()&MASK40;
  readDiag(o);o.type=uint8_t(f.type);o.received=true;
  // Hardware receiver auto-reenable and double buffering are disabled. Freeze
  // after copying event metadata; clearing status does not re-start reception.
  idle(i);clear(rxguard::RX_CLEAR);return 1;
 }
 bool send(uint8_t i,Frame&f,uint64_t requested,Tx&o)override{
  o=Tx{};o.attempted=true;o.radio=i;o.type=uint8_t(f.type);o.requested=requested;o.begin_us=nowUs();
#if DUAL8_ANCHOR
  const bool firstPoll=i<N&&!latched_[i]&&f.type==Type::Poll&&f.txn==0;
  if(firstPoll){choose(i);pll_startup.first_poll_entry=status();}
#endif
  if(i>=N)return false;
#if DUAL8_ANCHOR
  if(!verifyIdleClock()||!guard(i)){
   o.hardware_fault=latched_[i];o.status=fault_status[i];o.end_us=nowUs();return false;
  }
#else
  if(!guard(i))return false;
#endif
  choose(i);idle(i);
#if DUAL8_ANCHOR
  if(firstPoll)pll_startup.first_poll_after_idle=status();
#endif
  clear(rxguard::TX_CLEAR|(1ULL<<27)|(1ULL<<34));
#if DUAL8_ANCHOR
  if(firstPoll)pll_startup.first_poll_after_clear=status();
#endif
  const uint64_t dx=plannedRaw(requested,calibration_[i].tx);o.planned=(dx+calibration_[i].tx)&MASK40;
  if(f.type==Type::Poll||f.type==Type::Poll2)f.poll_tx=o.planned;
  else if(isResponse(f.type))f.tag_resp_tx=o.planned;
  else f.probe_tx=o.planned;
  // DW1000Ng::setTransmitData(data, AIR_BYTES) writes AIR_BYTES+2 bytes
  // to TX_BUFFER when hardware FCS is enabled. The last two bytes are
  // hardware-reserved FCS bytes; never read beyond a 96-byte stack array.
  uint8_t data[AIR_BYTES+2]{},d[5];
  static_assert(sizeof(data)>=AIR_BYTES+2,"DW1000 TX buffer requires FCS space");
  if(!encode(f,data))return false;
  putLE(d,dx,5);
  const int64_t lead0=signed40(dx,system(i));if(lead0<int64_t(usTicks(cfg_.min_lead_us)))return false;
  DW1000Ng::setDelayedTRX(d);
#if DUAL8_ANCHOR
  if(firstPoll)pll_startup.first_poll_after_schedule=status();
#endif
  DW1000Ng::setTransmitData(data,AIR_BYTES);
#if DUAL8_ANCHOR
  if(firstPoll)pll_startup.first_poll_after_buffer=status();
#endif
  const int64_t lead=signed40(dx,system(i));o.lead_us=int32_t(double(lead)*1e6/HZ);
#if DUAL8_ANCHOR
  const auto preTx=observeClock();
  if(firstPoll){
   pll_startup.first_poll_pre_tx=preTx.status;pll_startup.first_poll_checked=true;
  }
  if(!preTx.usable()){
   tripClock(i,preTx);o.hardware_fault=true;o.status=preTx.status;o.end_us=nowUs();return false;
  }
#endif
  if(lead<int64_t(usTicks(cfg_.min_lead_us))){o.end_us=nowUs();return false;}
  DW1000Ng::startTransmit(TransmitMode::DELAYED);
  const uint64_t end=nowUs()+uint64_t(o.lead_us)+cfg_.grace_us+3000;
  while(nowUs()<end){
#if DUAL8_ANCHOR
   const auto sample=observeClock();const uint64_t st=sample.status;o.status=st;
   if(!sample.usable()){tripClock(i,sample);o.hardware_fault=true;break;}
#else
   const uint64_t st=status();o.status=st;
   if(st==MASK40||(st&rxguard::PLL)){trip(i,st);break;}
#endif
   if(st&((1ULL<<27)|(1ULL<<34)))break;
   if(st&(1ULL<<7)){
    o.actual=DW1000Ng::getTransmitTimestamp()&MASK40;
#if DUAL8_ANCHOR
    const auto confirmed=observeClock();
    o.status=confirmed.status;
    if(!confirmed.usable()){tripClock(i,confirmed);o.hardware_fault=true;break;}
    if(confirmed.status&((1ULL<<27)|(1ULL<<34)))break;
#endif
    o.ok=o.actual==o.planned;break;
   }
   relax();
  }
  o.end_us=nowUs();if(!latched_[i]){clear(rxguard::TX_CLEAR|(1ULL<<27)|(1ULL<<34));idle(i);}return o.ok;
 }
 CirRead readCir(uint8_t i,uint8_t*dst,Cir&o)override{
  o=Cir{};
  if(i>=N||latched_[i]||!ready_[i])return CirRead::HardwareFault;
  if(!copyAccumulator(i,dst,o.read_us))return CirRead::Invalid;
  const bool intact=guard(i);
  o.present=true;o.bytes=CIR_BYTES;
  o.crc=crc32(dst,CIR_BYTES);
  if(!intact)return CirRead::HardwareFault;
  if(cfg_.verify_cir){
   if(!copyAccumulator(i,repeat_,o.repeat_us))return CirRead::Invalid;
   if(!guard(i))return CirRead::HardwareFault;
   o.repeat_equal=memcmp(dst,repeat_,CIR_BYTES)==0;
  }
  bool nonzero=false,nonff=false;for(size_t j=0;j<CIR_BYTES;++j){nonzero|=dst[j]!=0;nonff|=dst[j]!=255;}
  o.valid=nonzero&&nonff&&(!cfg_.verify_cir||o.repeat_equal);
  return o.valid?CirRead::Valid:CirRead::Invalid;
 }
 void relax()override{delayMicroseconds(25);const uint64_t n=nowUs();if(n-lastYield_>=2000){delay(1);lastYield_=nowUs();}}
 void relaxRx(bool critical)override{if(critical)delayMicroseconds(25);else relax();}
 bool selftest(){
#if DUAL8_ANCHOR
  if(verifyStart())return false;
#endif
  bool ok=true;for(unsigned k=0;k<32;++k)for(unsigned i=0;i<N;++i)ok=guard(uint8_t(i))&&ok;
  if(ok){for(unsigned i=0;i<N;++i)ok=arm(uint8_t(i))&&ok;delay(1);for(unsigned i=0;i<N;++i)idle(uint8_t(i));for(unsigned i=0;i<N;++i)ok=guard(uint8_t(i))&&ok;}
#if DUAL8_ANCHOR
  return ok&&verifyIdleClock();
#else
  return ok;
#endif
 }
};
} // namespace dual8
