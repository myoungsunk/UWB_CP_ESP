#include <cassert>
#include "clock_history.hpp"

using namespace dual8;

int main(){
 ClockHistory a,b;
 for(unsigned i=0;i<20;++i){
  ClockSample sample;
  sample.esp_after_us=1000*i;
  sample.system_time=i;
  a.push(sample);
 }
 assert(a.count()==ClockHistory::Capacity && a.at(0).system_time==4);
 assert(a.at(15).system_time==19 && !a.due(19999) && a.due(20000));
 assert(b.count()==0 && b.due(0));
 ClockSample fault;
 fault.phase=ClockPhase::BeforeTrip;
 fault.txn=7;
 fault.status=1ULL<<25;
 a.push(fault);
 fault.phase=ClockPhase::AfterTrip;
 a.push(fault);
 assert(a.at(14).phase==ClockPhase::BeforeTrip && a.at(15).phase==ClockPhase::AfterTrip);
 assert(a.at(14).txn==7 && a.at(15).status==(1ULL<<25));
 a.reset();
 assert(a.count()==0 && a.due(0));
}
