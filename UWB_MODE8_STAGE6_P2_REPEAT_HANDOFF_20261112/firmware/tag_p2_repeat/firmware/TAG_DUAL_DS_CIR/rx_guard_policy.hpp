#pragma once
#include <stdint.h>
// Candidate policy; bit positions match the pinned DW1000NgRegisters.hpp.
namespace rxguard {
constexpr uint64_t statusBit(unsigned n){return uint64_t(1)<<n;}
constexpr uint64_t MASK40=0xffffffffffULL;
constexpr uint64_t DONE=statusBit(13)|statusBit(14);
constexpr uint64_t FAILED=statusBit(12)|statusBit(15)|statusBit(16)|statusBit(18);
constexpr uint64_t TIMEOUT=statusBit(17)|statusBit(21)|statusBit(26);
constexpr uint64_t RF_PLL_LOSS=statusBit(24), CLOCK_PLL_LOSS=statusBit(25);
constexpr uint64_t PLL=RF_PLL_LOSS|CLOCK_PLL_LOSS;
constexpr uint32_t INTERNAL_CLOCK_CONTROL=4;
constexpr bool clockDetectorConfigured(uint32_t ecCtrl){
  return (ecCtrl&0x0fff)==INTERNAL_CLOCK_CONTROL;
}
// RF_STATUS bits 0–2 are CPLLLOCK, CPLLLOW and CPLLHIGH.
// The RF PLL is separate and can lock when RX is enabled.
constexpr bool startupClockQualified(uint64_t status,uint8_t rfStatus,uint32_t detector){
  return status!=MASK40 && (status&statusBit(1)) && !(status&RF_PLL_LOSS) &&
    (rfStatus&7)==1 && clockDetectorConfigured(detector);
}
constexpr bool startupClockSettled(uint64_t status,uint8_t rfStatus,uint32_t detector){
  return startupClockQualified(status,rfStatus,detector) && !(status&CLOCK_PLL_LOSS);
}
constexpr uint64_t RX_CLEAR=statusBit(8)|statusBit(9)|statusBit(10)|statusBit(11)|DONE|FAILED|TIMEOUT|statusBit(20)|statusBit(29);
constexpr uint64_t TX_CLEAR=statusBit(3)|statusBit(4)|statusBit(5)|statusBit(6)|statusBit(7);
enum class Action { None, Done, Failed, Timeout, Untrusted, ClockFault, Overrun, FilterReject };
constexpr Action classify(uint64_t s){
  return s==MASK40 ? Action::Untrusted : (s&PLL) ? Action::ClockFault :
    (s&statusBit(20)) ? Action::Overrun : (s&FAILED) ? Action::Failed :
    (s&TIMEOUT) ? Action::Timeout : (s&statusBit(29)) ? Action::FilterReject :
    ((s&DONE)==DONE) ? Action::Done : Action::None;
}
constexpr bool goodClock(uint64_t a,uint64_t b){
  return a!=0 && b!=0 && a!=MASK40 && b!=MASK40 &&
    ((b-a)&MASK40)>0 && ((b-a)&MASK40)<statusBit(39);
}
// An isolated all-ones LDE read can be a bus read failure. A recovery
// requires two fresh matches and a readable status with both PLL bits clear.
template<class ReadDelay, class ReadStatus>
bool confirmRxDelay(uint16_t observed,uint16_t expected,ReadDelay readDelay,ReadStatus readStatus){
  if(observed==expected)return true;
  if(observed!=UINT16_MAX||readDelay()!=expected||readDelay()!=expected)return false;
  const uint64_t status=readStatus();
  return status!=MASK40&&!(status&PLL);
}
constexpr bool goodSnapshot(uint32_t dev,uint16_t tx,uint16_t rx,uint16_t wanted,uint64_t a,uint64_t b){
  return dev==0xdeca0130UL && tx==wanted && rx==wanted && goodClock(a,b);
}
constexpr bool selftestReady(bool radio,bool power,bool clock,bool antenna,bool device,bool latched){
  return radio && power && clock && antenna && device && !latched;
}
}
