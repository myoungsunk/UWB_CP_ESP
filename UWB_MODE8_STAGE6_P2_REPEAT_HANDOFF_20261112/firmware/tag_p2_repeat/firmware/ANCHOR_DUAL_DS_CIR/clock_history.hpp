#pragma once
#include <stdint.h>

namespace dual8 {
enum class ClockPhase : uint8_t { Running, BeforeTrip, AfterTrip };
struct ClockSample {
 uint64_t status=0, system_time=0;
 uint64_t esp_before_us=0, esp_after_us=0;
 int32_t txn=-1;
 uint8_t rf_status=0;
 ClockPhase phase=ClockPhase::Running;
};

class ClockHistory {
public:
 enum : unsigned { Capacity=16, Chunk=4 };
 enum : uint32_t { IntervalUs=1000 };
 void reset(){next_=count_=0;next_due_us_=0;}
 bool due(uint64_t now)const{return now>=next_due_us_;}
 unsigned count()const{return count_;}
 void push(const ClockSample& sample){
  samples_[next_]=sample;
  next_=(next_+1)%Capacity;
  if(count_<Capacity)++count_;
  next_due_us_=sample.esp_after_us+IntervalUs;
 }
 const ClockSample& at(unsigned i)const{return samples_[(next_+Capacity-count_+i)%Capacity];}
private:
 ClockSample samples_[Capacity]{};
 unsigned next_=0,count_=0;
 uint64_t next_due_us_=0;
};
}
