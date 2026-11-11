#line 1 "D:\\SLAM_bot\\artifacts\\UWB_MODE8_R5_ANCHOR_PLL_STABILITY_20261111\\firmware\\ANCHOR_DUAL_DS_CIR\\rx_guard_policy.hpp"
#pragma once
#include <stdint.h>
// Candidate policy; bit positions match the pinned DW1000NgRegisters.hpp.
namespace rxguard {
constexpr uint64_t statusBit(unsigned n){return uint64_t(1)<<n;}
constexpr uint64_t MASK40=0xffffffffffULL;
constexpr uint64_t DONE=statusBit(13)|statusBit(14);
constexpr uint64_t FAILED=statusBit(12)|statusBit(15)|statusBit(16)|statusBit(18);
constexpr uint64_t TIMEOUT=statusBit(17)|statusBit(21)|statusBit(26);
constexpr uint64_t RF_PLL_LOSS=statusBit(24);
constexpr uint64_t CLOCK_PLL_LOSS=statusBit(25);
constexpr uint64_t PLL=RF_PLL_LOSS|CLOCK_PLL_LOSS;
// RF_STATUS bits 0–2 are CPLLLOCK, CPLLLOW, and CPLLHIGH.
constexpr bool clockLocked(uint8_t rfStatus){return (rfStatus&7)==1;}
constexpr bool clockCheckReadable(uint64_t s,uint8_t rfStatus,uint8_t ecCtrl){
  return s!=MASK40 && !(s&RF_PLL_LOSS) && rfStatus!=0xff && ecCtrl!=0xff && (ecCtrl&4)!=0;
}
constexpr bool canClearStartupClockEvent(uint64_t s,uint8_t rfStatus,uint8_t ecCtrl){
  return clockCheckReadable(s,rfStatus,ecCtrl) && clockLocked(rfStatus);
}
constexpr bool settledStartupClock(uint64_t s,uint8_t rfStatus){
  return s!=MASK40 && !(s&PLL) && clockLocked(rfStatus);
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
constexpr bool goodSnapshot(uint32_t dev,uint16_t tx,uint16_t rx,uint16_t wanted,uint64_t a,uint64_t b){
  return dev==0xdeca0130UL && tx==wanted && rx==wanted && goodClock(a,b);
}
constexpr bool selftestReady(bool radio,bool power,bool clock,bool antenna,bool device,bool latched){
  return radio && power && clock && antenna && device && !latched;
}
}
