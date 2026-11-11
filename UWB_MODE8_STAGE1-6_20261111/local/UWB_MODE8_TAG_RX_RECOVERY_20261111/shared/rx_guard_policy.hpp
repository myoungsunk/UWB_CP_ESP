#pragma once
#include <stdint.h>
// Candidate policy; bit positions match the pinned DW1000NgRegisters.hpp.
namespace rxguard {
constexpr uint64_t statusBit(unsigned n){return uint64_t(1)<<n;}
constexpr uint64_t MASK40=0xffffffffffULL;
constexpr uint64_t DONE=statusBit(13)|statusBit(14);
constexpr uint64_t FAILED=statusBit(12)|statusBit(15)|statusBit(16)|statusBit(18);
constexpr uint64_t TIMEOUT=statusBit(17)|statusBit(21)|statusBit(26);
constexpr uint64_t PLL=statusBit(24)|statusBit(25);
constexpr uint64_t RX_CLEAR=statusBit(8)|statusBit(9)|statusBit(10)|statusBit(11)|DONE|FAILED|TIMEOUT|statusBit(20)|statusBit(29);
constexpr uint64_t TX_CLEAR=statusBit(3)|statusBit(4)|statusBit(5)|statusBit(6)|statusBit(7);
enum class Action { None, Done, Failed, Timeout, Untrusted, ClockFault, Overrun, FilterReject };
constexpr Action classify(uint64_t s){
  return s==MASK40 ? Action::Untrusted : (s&PLL) ? Action::ClockFault :
    (s&statusBit(20)) ? Action::Overrun : (s&FAILED) ? Action::Failed :
    (s&TIMEOUT) ? Action::Timeout : (s&statusBit(29)) ? Action::FilterReject :
    ((s&DONE)==DONE) ? Action::Done : Action::None;
}
// APS022, PMSC_STATE in SYS_STATE[23:16].
constexpr unsigned pmscState(uint64_t state){return unsigned((state>>16)&0xff);}
constexpr bool receiverRunning(uint64_t state){return pmscState(state)==5;}
constexpr bool receiverIdleWithoutEvent(uint64_t state,uint64_t status){
  return pmscState(state)==1 && classify(status)==Action::None;
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
