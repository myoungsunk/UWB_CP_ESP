#pragma once
#include <Arduino.h>
#include <SPI.h>
#include <DW1000Ng.hpp>
#include <SPIporting.hpp>
#include <esp_timer.h>
#include "engine.hpp"
#include "rx_guard_policy.hpp"
namespace dual8 {
struct RegisterFault {
 const char* check=nullptr;
 uint64_t at_us=0, expected=0, first=0, reread=0, reread_2mhz=0;
 int32_t txn=-1;
 uint16_t offset=0xffff;
 uint8_t reg=0, bytes=0;
};
struct AntennaDelaySample {
 uint16_t tx=0,rx=0;
};
#if !DUAL8_ANCHOR
struct AntennaInitTrace {
 DW1000Ng::AntennaDelayReadback set_tx{},set_rx{};
 AntennaDelaySample first_guard{};
};
struct ClockTraceSample {
 uint64_t status=MASK40;
 uint32_t detector=UINT32_MAX;
 uint8_t rf_status=0xff;
 bool qualified()const{return rxguard::startupClockQualified(status,rf_status,detector);}
 bool settled()const{return rxguard::startupClockSettled(status,rf_status,detector);}
};
struct StartClockTrace {
 ClockTraceSample before_start[2], before_rx[2], after_rx[2], after_arms[2];
};
struct ClockInitLdo {
 uint32_t otp=0, before=0, after=0;
 bool kicked=false;
};
#endif
class Esp32Port:public Port {
 static constexpr unsigned N=DUAL8_ANCHOR?1:2;
 RxWindowTrace* window_trace_=nullptr;
 Config cfg_;Calibration calibration_[2]{};bool rx_[2]{},ready_[2]{},latched_[2]{};uint64_t arm_us_[2]{};
 uint8_t active_=0;uint64_t lastYield_=0;uint8_t accum_[CIR_BYTES+1]{},repeat_[CIR_BYTES]{};
 RegisterFault register_fault_[2]{};
#if !DUAL8_ANCHOR
 AntennaInitTrace antenna_init_a_{};
 DW1000Ng::ClockInitSample clock_steps_[2][DW1000Ng::ClockInitStepCount]{};
 ClockInitLdo clock_ldo_[2]{};
 ClockTraceSample vendor_init_clock_[2], configured_clock_[2];
 ClockTraceSample pre_ack_clock_[2], settled_clock_[2];
 bool clock_ack_[2]{};
 StartClockTrace* start_clock_trace_=nullptr;
#endif
 uint64_t last_status_[2]{};
 uint8_t sampled_[2]{};
 int32_t diagnostic_txn_=-1;
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
 uint64_t registerValue(uint8_t reg,uint16_t off,uint8_t bytes){
  uint8_t b[5];read(reg,off,b,bytes);return getLE(b,bytes);
 }
 uint64_t status(){
  last_status_[active_]=registerValue(0x0f,0xffff,5);
  sampled_[active_]|=2;
  return last_status_[active_];
 }
 void clear(uint64_t mask){uint8_t b[5];putLE(b,mask,5);write(0x0f,0xffff,b,5);}
#if !DUAL8_ANCHOR
 ClockTraceSample sampleClock(){
  SPIporting::RxSpeedScope slow(true);
  ClockTraceSample sample;
  sample.status=registerValue(0x0f,0xffff,5);
  read(0x28,0x2c,&sample.rf_status,1);
  sample.detector=uint32_t(registerValue(0x24,0,4));
  return sample;
 }
 void sampleBothClocks(ClockTraceSample (&samples)[2]){
  const uint8_t previous=active_;
  for(uint8_t i=0;i<N;++i){choose(i);samples[i]=sampleClock();}
  choose(previous);
 }
 bool checkClock(uint8_t i,const ClockTraceSample& sample){
  if(sample.settled())return true;
  choose(i);
  SPIporting::RxSpeedScope slow(true);
  if(sample.status==MASK40||!(sample.status&rxguard::statusBit(1))||(sample.status&rxguard::PLL))
   tripRegister(i,sample.status,"PLL_SYS_STATUS",0x0f,0xffff,5,rxguard::statusBit(1),sample.status);
  else if(!rxguard::clockDetectorConfigured(sample.detector))
   tripRegister(i,sample.status,"PLL_EC_CTRL",0x24,0,4,rxguard::INTERNAL_CLOCK_CONTROL,sample.detector);
  else
   tripRegister(i,sample.status,"PLL_RF_STATUS",0x28,0x2c,1,1,sample.rf_status);
  return false;
 }
 bool settleInitClock(uint8_t i){
  SPIporting::RxSpeedScope slow(true);
  const uint64_t deadline=nowUs()+250000;
  auto sample=vendor_init_clock_[i];
  while(!sample.qualified()&&nowUs()<deadline){
   delay(10);
   sample=sampleClock();
  }
  pre_ack_clock_[i]=settled_clock_[i]=sample;
  if(!sample.qualified())return checkClock(i,sample);
  const uint64_t dev=registerValue(0x00,0xffff,4);
  if(dev!=0xdeca0130){
   tripRegister(i,sample.status,"PLL_DEV_ID",0x00,0xffff,4,0xdeca0130,dev);
   return false;
  }
  if(sample.status&rxguard::CLOCK_PLL_LOSS){
   clear(rxguard::CLOCK_PLL_LOSS);
   clock_ack_[i]=true;
  }
  // Never clear another event after acknowledging the qualified idle snapshot.
  const uint64_t stableUntil=nowUs()+100000;
  do{
   sample=sampleClock();
   settled_clock_[i]=sample;
   if(!checkClock(i,sample))return false;
   delay(10);
  }while(nowUs()<stableUntil);
  return true;
 }
#endif
 void trip(uint8_t i,uint64_t st){
  ++faults;fault_status[i]=st;
  if(!latched_[i]){choose(i);DW1000Ng::forceTRxOff();} // One best-effort shutdown; do not claim physical confirmation.
  latched_[i]=true;ready_[i]=false;rx_[i]=false;
 }
 void tripRegister(uint8_t i,uint64_t statusAtFault,const char* check,uint8_t reg,
                   uint16_t off,uint8_t bytes,uint64_t expected,uint64_t observed){
  auto& fault=register_fault_[i];
  if(!fault.check){
   fault.check=check;fault.reg=reg;fault.offset=off;fault.bytes=bytes;
   fault.expected=expected;fault.first=observed;fault.at_us=nowUs();fault.txn=diagnostic_txn_;
   fault.reread=registerValue(reg,off,bytes);
#if !DUAL8_ANCHOR
   SPIporting::RxSpeedScope slow(true);
#endif
   fault.reread_2mhz=registerValue(reg,off,bytes);
  }
  trip(i,statusAtFault);
 }
 bool guard(uint8_t i,AntennaDelaySample* initial=nullptr){
  if(i>=N||latched_[i]||!ready_[i]){return false;}
  choose(i);
  uint8_t dev[4],tx[2],rr[2],t1[5],t2[5];read(0,0xffff,dev,4);read(0x18,0xffff,tx,2);read(0x2e,0x1804,rr,2);
  read(6,0xffff,t1,5);read(6,0xffff,t2,5);
  last_dev[i]=uint32_t(getLE(dev,4));last_ant_tx[i]=uint16_t(getLE(tx,2));last_ant_rx[i]=uint16_t(getLE(rr,2));
  if(initial){initial->tx=last_ant_tx[i];initial->rx=last_ant_rx[i];}
  sampled_[i]|=1;
  const uint64_t time1=getLE(t1,5),time2=getLE(t2,5);
  const bool clockOk=rxguard::goodClock(time1,time2);
  const bool baseOk=last_dev[i]==0xdeca0130&&last_ant_tx[i]==calibration_[i].tx&&clockOk;
  bool rxDelayOk=last_ant_rx[i]==calibration_[i].rx;
  uint64_t retryStatus=0;
  bool statusSampled=false;
#if !DUAL8_ANCHOR
  if(baseOk&&!rxDelayOk&&last_ant_rx[i]==UINT16_MAX){
   ++rxEvidence[i].delay_rechecks;
   rxDelayOk=rxguard::confirmRxDelay(last_ant_rx[i],calibration_[i].rx,[&](){
    read(0x2e,0x1804,rr,2);return uint16_t(getLE(rr,2));
   },[&](){
    statusSampled=true;retryStatus=status();return retryStatus;
   });
   if(window_trace_){
    window_trace_[i].fail(statusSampled?retryStatus:status(),nowUs(),10);
    if(rxDelayOk)window_trace_[i].recovery_us=nowUs();
   }
   if(rxDelayOk){
    last_ant_rx[i]=uint16_t(getLE(rr,2));
    ++rxEvidence[i].delay_recovered;
   }
  }
#endif
  if(!baseOk||!rxDelayOk){
   const uint64_t st=statusSampled?retryStatus:status();
   if(last_dev[i]!=0xdeca0130)tripRegister(i,st,"DEV_ID",0x00,0xffff,4,0xdeca0130,last_dev[i]);
   else if(last_ant_tx[i]!=calibration_[i].tx)tripRegister(i,st,"TX_ANTD",0x18,0xffff,2,calibration_[i].tx,last_ant_tx[i]);
   else if(!rxDelayOk)tripRegister(i,st,"LDE_RXANTD",0x2e,0x1804,2,calibration_[i].rx,last_ant_rx[i]);
   else tripRegister(i,st,"SYS_TIME",0x06,0xffff,5,time1,time2);
   return false;
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
 const RegisterFault& registerFault(uint8_t i)const{return register_fault_[i];}
 void beginRegisterTrace(uint32_t txn){
  diagnostic_txn_=int32_t(txn);sampled_[0]=sampled_[1]=0;
 }
 void finishRegisterTrace(Record& r){
  for(unsigned i=0;i<N;++i){
   r.dev_id[i]=last_dev[i];r.sys_status[i]=last_status_[i];r.register_sample_mask[i]=sampled_[i];
  }
  diagnostic_txn_=-1;
 }
#if !DUAL8_ANCHOR
 struct RxEvidence {
  uint64_t status_or=0, first_state=0, last_error_status=0;
  uint32_t arms=0, polls=0, probes=0, failures=0, bad_length=0, bad_frame=0;
  uint32_t delay_rechecks=0, delay_recovered=0;
  uint16_t last_length=0;
 };
 RxEvidence rxEvidence[2]{};
 void clearRxEvidence(){for(auto& e:rxEvidence)e=RxEvidence{};}
 uint64_t rxRegister(uint8_t i,uint8_t reg,uint16_t off,uint8_t length){
  choose(i);uint8_t data[5]{};read(reg,off,data,length);return getLE(data,length);
 }
 uint32_t readDeviceId(uint8_t i){
  const uint8_t previous=active_;
  // The idle command uses the same FAST profile as the initialization guard.
  const uint32_t id=uint32_t(rxRegister(i,0x00,0xffff,4));
  choose(previous);
  return id;
 }
#endif
 uint32_t faults=0,rejected[2]{},rx_resets[2]{},last_dev[2]{};uint16_t last_ant_tx[2]{},last_ant_rx[2]{};uint64_t fault_status[2]{};
 uint64_t nowUs()override{return uint64_t(esp_timer_get_time());}
 void configure(const Config&c){cfg_=c;}
 const Calibration& calibration(unsigned i)const{return calibration_[i];}
 void traceRx(RxWindowTrace* traces)override{window_trace_=traces;}
 bool faulted()const{return latched_[0]||latched_[1];}
#if !DUAL8_ANCHOR
 const AntennaInitTrace& antennaInitA()const{return antenna_init_a_;}
 const ClockTraceSample& vendorInitClock(unsigned i)const{return vendor_init_clock_[i];}
 const DW1000Ng::ClockInitSample& clockInitStep(unsigned i,unsigned step)const{return clock_steps_[i][step];}
 const ClockInitLdo& clockInitLdo(unsigned i)const{return clock_ldo_[i];}
 const ClockTraceSample& configuredClock(unsigned i)const{return configured_clock_[i];}
 const ClockTraceSample& preAckClock(unsigned i)const{return pre_ack_clock_[i];}
 const ClockTraceSample& settledClock(unsigned i)const{return settled_clock_[i];}
 bool startupClockAcknowledged(unsigned i)const{return clock_ack_[i];}
 bool checkStartClocks(const ClockTraceSample (&samples)[2]){
  const bool a=checkClock(0,samples[0]);
  const bool b=checkClock(1,samples[1]);
  return a&&b;
 }
 bool beginStartClockTrace(StartClockTrace& trace){
  sampleBothClocks(trace.before_start);
  start_clock_trace_=&trace;
  return checkStartClocks(trace.before_start);
 }
 void endStartClockTrace(){
  sampleBothClocks(start_clock_trace_->after_arms);
  start_clock_trace_=nullptr;
 }
#endif
 bool initialize(){
  for(unsigned i=0;i<N;++i){
   ready_[i]=false;
   const Pins p=pins(i);
   // Claim CS as GPIO and preload its HIGH latch before enabling output.
   pinMode(p.cs,INPUT_PULLUP);
   digitalWrite(p.cs,HIGH);
   pinMode(p.cs,OUTPUT);
   pinMode(p.irq,INPUT);
   pinMode(p.rst,INPUT);
  }
  SPI.begin(18,19,23,pins(0).cs);
  bool initialized=true;
  for(unsigned i=0;i<N;++i){
   rx_[i]=false;latched_[i]=false;register_fault_[i]=RegisterFault{};choose(uint8_t(i));
#if DUAL8_ANCHOR
   SPIporting::setSlowReadShiftFix(true);DW1000Ng::initializeNoInterrupt(pins(i).cs,pins(i).rst);
#else
   SPIporting::setSlowReadShiftFix(false);
   configured_clock_[i]=pre_ack_clock_[i]=settled_clock_[i]=ClockTraceSample{};
   clock_ack_[i]=false;
   {
    SPIporting::RxSpeedScope slow(true);
    DW1000Ng::initializeNoInterrupt(uint8_t(i),pins(i).cs,pins(i).rst,SPI,clock_steps_[i]);
    vendor_init_clock_[i]=sampleClock();
   }
   auto& ldo=clock_ldo_[i];
   ldo.otp=DW1000Ng::getInitOtpLdoTune();
   ldo.before=DW1000Ng::getInitLdoTuneBefore();
   ldo.after=DW1000Ng::getInitLdoTuneAfter();
   ldo.kicked=DW1000Ng::wasInitLdoTuneKicked();
   if(!settleInitClock(uint8_t(i))){initialized=false;continue;}
#endif
   SPIporting::setSPIspeed(SPIClock::FAST);
   device_configuration_t dc={false,false,false,true,false,SFDMode::STANDARD_SFD,Channel::CHANNEL_5,DataRate::RATE_850KBPS,PulseFrequency::FREQ_16MHZ,PreambleLength::LEN_256,PreambleCode::CODE_3};
   DW1000Ng::applyConfiguration(dc);DW1000Ng::disableFrameFiltering();DW1000Ng::setNetworkId(10);DW1000Ng::setDeviceAddress(uint16_t(DUAL8_ANCHOR?5:6+i));
   AntennaDelaySample* initial=nullptr;
#if DUAL8_ANCHOR
   DW1000Ng::setTxAntennaDelay(calibration_[i].tx);DW1000Ng::setRxAntennaDelay(calibration_[i].rx);
#else
   // Both setters write both registers. Sample each of those SPI writes on A.
   auto* trace=i==0?&antenna_init_a_:nullptr;
   DW1000Ng::setTxAntennaDelay(calibration_[i].tx,trace?&trace->set_tx:nullptr);
   DW1000Ng::setRxAntennaDelay(calibration_[i].rx,trace?&trace->set_rx:nullptr);
   if(trace)initial=&trace->first_guard;
#endif
   DW1000Ng::setWait4Response(0);DW1000Ng::setDoubleBuffering(false);DW1000Ng::setReceiveFrameWaitTimeoutPeriod(0);
   // Polling implementation: IRQ pins are inputs, not interrupt-owned SPI users.
   uint8_t mask[4]{};write(0x0e,0xffff,mask,4);clear(rxguard::RX_CLEAR|rxguard::TX_CLEAR|(1ULL<<27)|(1ULL<<34));
#if !DUAL8_ANCHOR
   configured_clock_[i]=sampleClock();
   if(!checkClock(uint8_t(i),configured_clock_[i])){initialized=false;continue;}
#endif
   ready_[i]=true;
   if(!guard(uint8_t(i),initial))initialized=false;
  }
  return initialized;
 }
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
  choose(i);if(rx_[i])return true;
#if !DUAL8_ANCHOR
  SPIporting::RxSpeedScope scope(true);
#endif
  DW1000Ng::forceTRxOff();clear(rxguard::RX_CLEAR);rx_[i]=true;
#if DUAL8_ANCHOR
  DW1000Ng::startReceive(ReceiveMode::IMMEDIATE);
#else
  if(start_clock_trace_)start_clock_trace_->before_rx[i]=sampleClock();
  DW1000Ng::startReceive(ReceiveMode::IMMEDIATE,true);
  if(start_clock_trace_)start_clock_trace_->after_rx[i]=sampleClock();
  auto& evidence=rxEvidence[i];
  if(evidence.arms++==0)evidence.first_state=rxRegister(i,0x19,0xffff,5);
#endif
  arm_us_[i]=nowUs();return guard(i);
 }
 int receive(uint8_t i,Frame&f,Rx&o)override{
  if(i>=N||latched_[i]){return -2;}
  choose(i);if(!rx_[i])return 0;
  const uint64_t st=status();const auto action=rxguard::classify(st);
#if !DUAL8_ANCHOR
  auto& evidence=rxEvidence[i];
  evidence.status_or|=st;
  if(action!=rxguard::Action::None&&action!=rxguard::Action::Done){
   ++evidence.failures;evidence.last_error_status=st;
  }
#endif
  if(window_trace_&&action!=rxguard::Action::None&&action!=rxguard::Action::Done)
   window_trace_[i].fail(st,nowUs(),uint8_t(action));
  if(action==rxguard::Action::Untrusted||action==rxguard::Action::ClockFault||action==rxguard::Action::Overrun||action==rxguard::Action::FilterReject){
   tripRegister(i,st,"RX_SYS_STATUS",0x0f,0xffff,5,0,st);return -2;
  }
  if(action==rxguard::Action::Failed||action==rxguard::Action::Timeout){
   ++rejected[i];
   const bool recovered=recover(i,st);
   if(recovered&&window_trace_)window_trace_[i].recovery_us=nowUs();
   return recovered?-1:-2;
  }
  if(action!=rxguard::Action::Done)return 0;
  if(!guard(i)){
   if(window_trace_)window_trace_[i].fail(st,nowUs(),13);
   return -2;
  }
  o=Rx{};o.radio=i;o.status=st;o.esp_us=nowUs();o.arm_us=arm_us_[i];o.lde=(st&(1ULL<<10))!=0;
  o.bytes=DW1000Ng::getReceivedDataLength();o.hw=DW1000Ng::getReceiveTimestamp()&MASK40;o.raw=DW1000Ng::getReceiveTimestampRaw()&MASK40;
#if !DUAL8_ANCHOR
  evidence.last_length=o.bytes;
  if(o.bytes!=AIR_BYTES)++evidence.bad_length;
#endif
  if(o.bytes!=AIR_BYTES){
   if(window_trace_)window_trace_[i].fail(st,nowUs(),8);
   idle(i);clear(rxguard::RX_CLEAR);++rejected[i];return -1;}
  DW1000Ng::getReceivedData(o.air,AIR_BYTES);
  if(!decode(o.air,AIR_BYTES,f)){
   if(window_trace_)window_trace_[i].fail(st,nowUs(),9);
#if !DUAL8_ANCHOR
   ++evidence.bad_frame;
#endif
   idle(i);clear(rxguard::RX_CLEAR);++rejected[i];return -1;
  }
#if !DUAL8_ANCHOR
  if(f.type==Type::Poll)++evidence.polls;
  if(f.type==Type::Probe1||f.type==Type::Probe2)++evidence.probes;
#endif
  readDiag(o);o.type=uint8_t(f.type);o.received=true;
  // Hardware receiver auto-reenable and double buffering are disabled. Freeze
  // after copying event metadata; clearing status does not re-start reception.
  idle(i);clear(rxguard::RX_CLEAR);return 1;
 }
 bool send(uint8_t i,Frame&f,uint64_t requested,Tx&o)override{
  o=Tx{};o.attempted=true;o.radio=i;o.type=uint8_t(f.type);o.requested=requested;o.begin_us=nowUs();
  if(i>=N||!guard(i)){return false;}
  choose(i);idle(i);clear(rxguard::TX_CLEAR|(1ULL<<27)|(1ULL<<34));
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
  const int64_t lead0=signed40(dx,system(i));
  if(lead0<int64_t(usTicks(cfg_.min_lead_us))){
   o.lead_us=int32_t(double(lead0)*1e6/HZ);o.end_us=nowUs();return false;
  }
  DW1000Ng::setDelayedTRX(d);DW1000Ng::setTransmitData(data,AIR_BYTES);
  const int64_t lead=signed40(dx,system(i));o.lead_us=int32_t(double(lead)*1e6/HZ);
  if(lead<int64_t(usTicks(cfg_.min_lead_us))){o.end_us=nowUs();return false;}
  DW1000Ng::startTransmit(TransmitMode::DELAYED);
  const uint64_t end=nowUs()+uint64_t(o.lead_us)+cfg_.grace_us+3000;
  while(nowUs()<end){
   const uint64_t st=status();o.status=st;
   if(st==MASK40||(st&((1ULL<<24)|(1ULL<<25)))){
    tripRegister(i,st,"TX_SYS_STATUS",0x0f,0xffff,5,0,st);break;
   }
   if(st&((1ULL<<27)|(1ULL<<34)))break;
   if(st&(1ULL<<7)){o.actual=DW1000Ng::getTransmitTimestamp()&MASK40;o.ok=o.actual==o.planned;break;}
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
  bool ok=true;for(unsigned k=0;k<32;++k)for(unsigned i=0;i<N;++i)ok=guard(uint8_t(i))&&ok;
  if(ok){for(unsigned i=0;i<N;++i)ok=arm(uint8_t(i))&&ok;delay(1);for(unsigned i=0;i<N;++i)idle(uint8_t(i));for(unsigned i=0;i<N;++i)ok=guard(uint8_t(i))&&ok;}
  return ok;
 }
};
} // namespace dual8
