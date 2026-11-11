#line 1 "D:\\SLAM_bot\\artifacts\\UWB_MODE8_STAGE5_P2_RANGING_20261111\\tag\\firmware\\TAG_DUAL_DS_CIR\\esp32_port.hpp"
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
 void trip(uint8_t i,uint64_t st){
  ++faults;fault_status[i]=st;
  if(!latched_[i]){choose(i);DW1000Ng::forceTRxOff();} // One best-effort shutdown; do not claim physical confirmation.
  latched_[i]=true;ready_[i]=false;rx_[i]=false;
 }
 bool guard(uint8_t i){
  if(i>=N||latched_[i]||!ready_[i]){return false;}
  choose(i);
  uint8_t dev[4],tx[2],rr[2],t1[5],t2[5];read(0,0xffff,dev,4);read(0x18,0xffff,tx,2);read(0x2e,0x1804,rr,2);
  read(6,0xffff,t1,5);read(6,0xffff,t2,5);
  last_dev[i]=uint32_t(getLE(dev,4));last_ant_tx[i]=uint16_t(getLE(tx,2));last_ant_rx[i]=uint16_t(getLE(rr,2));
  const bool baseOk=last_dev[i]==0xdeca0130&&last_ant_tx[i]==calibration_[i].tx&&rxguard::goodClock(getLE(t1,5),getLE(t2,5));
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
   if(rxDelayOk){
    last_ant_rx[i]=uint16_t(getLE(rr,2));
    ++rxEvidence[i].delay_recovered;
   }
  }
#endif
  if(!baseOk||!rxDelayOk){trip(i,statusSampled?retryStatus:status());return false;}
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
#endif
 uint32_t faults=0,rejected[2]{},rx_resets[2]{},last_dev[2]{};uint16_t last_ant_tx[2]{},last_ant_rx[2]{};uint64_t fault_status[2]{};
 uint64_t nowUs()override{return uint64_t(esp_timer_get_time());}
 void configure(const Config&c){cfg_=c;}
 const Calibration& calibration(unsigned i)const{return calibration_[i];}
 bool faulted()const{return latched_[0]||latched_[1];}
 bool initialize(){
  for(unsigned i=0;i<N;++i){
   const Pins p=pins(i);
   // Claim CS as GPIO and preload its HIGH latch before enabling output.
   pinMode(p.cs,INPUT_PULLUP);
   digitalWrite(p.cs,HIGH);
   pinMode(p.cs,OUTPUT);
   pinMode(p.irq,INPUT);
   pinMode(p.rst,INPUT);
  }
  SPI.begin(18,19,23,pins(0).cs);
  for(unsigned i=0;i<N;++i){
   rx_[i]=false;latched_[i]=false;choose(uint8_t(i));
#if DUAL8_ANCHOR
   SPIporting::setSlowReadShiftFix(true);DW1000Ng::initializeNoInterrupt(pins(i).cs,pins(i).rst);
#else
   SPIporting::setSlowReadShiftFix(false);DW1000Ng::initializeNoInterrupt(uint8_t(i),pins(i).cs,pins(i).rst,SPI);
#endif
   SPIporting::setSPIspeed(SPIClock::FAST);
   device_configuration_t dc={false,false,false,true,false,SFDMode::STANDARD_SFD,Channel::CHANNEL_5,DataRate::RATE_850KBPS,PulseFrequency::FREQ_16MHZ,PreambleLength::LEN_256,PreambleCode::CODE_3};
   DW1000Ng::applyConfiguration(dc);DW1000Ng::disableFrameFiltering();DW1000Ng::setNetworkId(10);DW1000Ng::setDeviceAddress(uint16_t(DUAL8_ANCHOR?5:6+i));
   DW1000Ng::setTxAntennaDelay(calibration_[i].tx);DW1000Ng::setRxAntennaDelay(calibration_[i].rx);DW1000Ng::setWait4Response(0);DW1000Ng::setDoubleBuffering(false);DW1000Ng::setReceiveFrameWaitTimeoutPeriod(0);
   // Polling implementation: IRQ pins are inputs, not interrupt-owned SPI users.
   uint8_t mask[4]{};write(0x0e,0xffff,mask,4);clear(rxguard::RX_CLEAR|rxguard::TX_CLEAR|(1ULL<<27)|(1ULL<<34));
   ready_[i]=true;if(!guard(uint8_t(i)))return false;
  }
  return true;
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
  DW1000Ng::startReceive(ReceiveMode::IMMEDIATE,true);
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
  if(action==rxguard::Action::Untrusted||action==rxguard::Action::ClockFault||action==rxguard::Action::Overrun||action==rxguard::Action::FilterReject){trip(i,st);return -2;}
  if(action==rxguard::Action::Failed||action==rxguard::Action::Timeout){++rejected[i];return recover(i,st)?-1:-2;}
  if(action!=rxguard::Action::Done)return 0;
  if(!guard(i))return -2;
  o=Rx{};o.radio=i;o.status=st;o.esp_us=nowUs();o.arm_us=arm_us_[i];o.lde=(st&(1ULL<<10))!=0;
  o.bytes=DW1000Ng::getReceivedDataLength();o.hw=DW1000Ng::getReceiveTimestamp()&MASK40;o.raw=DW1000Ng::getReceiveTimestampRaw()&MASK40;
#if !DUAL8_ANCHOR
  evidence.last_length=o.bytes;
  if(o.bytes!=AIR_BYTES)++evidence.bad_length;
#endif
  if(o.bytes!=AIR_BYTES){idle(i);clear(rxguard::RX_CLEAR);++rejected[i];return -1;}
  DW1000Ng::getReceivedData(o.air,AIR_BYTES);
  if(!decode(o.air,AIR_BYTES,f)){
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
  if(f.type==Type::Poll)f.poll_tx=o.planned;
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
  DW1000Ng::setDelayedTRX(d);DW1000Ng::setTransmitData(data,AIR_BYTES);
  const int64_t lead=signed40(dx,system(i));o.lead_us=int32_t(double(lead)*1e6/HZ);
  if(lead<int64_t(usTicks(cfg_.min_lead_us))){o.end_us=nowUs();return false;}
  DW1000Ng::startTransmit(TransmitMode::DELAYED);
  const uint64_t end=nowUs()+uint64_t(o.lead_us)+cfg_.grace_us+3000;
  while(nowUs()<end){
   const uint64_t st=status();o.status=st;
   if(st==MASK40||(st&((1ULL<<24)|(1ULL<<25)))){trip(i,st);break;}
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
 bool selftest(){
  bool ok=true;for(unsigned k=0;k<32;++k)for(unsigned i=0;i<N;++i)ok=guard(uint8_t(i))&&ok;
  if(ok){for(unsigned i=0;i<N;++i)ok=arm(uint8_t(i))&&ok;delay(1);for(unsigned i=0;i<N;++i)idle(uint8_t(i));for(unsigned i=0;i<N;++i)ok=guard(uint8_t(i))&&ok;}
  return ok;
 }
};
} // namespace dual8
