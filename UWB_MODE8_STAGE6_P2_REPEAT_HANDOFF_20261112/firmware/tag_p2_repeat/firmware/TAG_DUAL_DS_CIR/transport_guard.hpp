#pragma once
#include <stddef.h>
#include <stdint.h>

namespace dual8app {

struct RecordKey {
 uint32_t boot, session, config, sequence;
 RecordKey(uint32_t b=0,uint32_t s=0,uint32_t c=0,uint32_t q=0):
  boot(b),session(s),config(c),sequence(q){}
 bool operator==(const RecordKey& other) const {
  return boot==other.boot&&session==other.session&&config==other.config&&sequence==other.sequence;
 }
};
enum class ReplayDecision:uint8_t { None, Ack, Nack, Expired };

struct ReplaySlot {
 RecordKey key{};
 bool occupied=false, ack=false, nack=false;
 uint8_t attempts=0;
 // Zero while the initial send is still in progress.
 uint64_t expires=0;

 bool offer(const RecordKey& incoming,bool positive,uint64_t now){
  if(!occupied||!(key==incoming)||(expires&&now>=expires)||ack)return false;
  if(positive){ack=true;nack=false;return true;}
  if(nack)return false;
  nack=true;return true;
 }
 ReplayDecision take(uint64_t now){
  if(!occupied||!expires)return ReplayDecision::None;
  if(ack){occupied=false;return ReplayDecision::Ack;}
  if(now>=expires){occupied=false;return ReplayDecision::Expired;}
  if(nack){nack=false;return ReplayDecision::Nack;}
  return ReplayDecision::None;
 }
};

enum class WireResult:uint8_t { Ok, Encoding, Timeout, Driver };

template<class Uart>
class BoundedWire {
 Uart& uart;
 const uint64_t deadline;
 WireResult result=WireResult::Ok;
 public:
 BoundedWire(Uart& port,uint64_t budget):uart(port),deadline(port.now()+budget){}
 bool send(const uint8_t* data,size_t size){
  while(size&&result==WireResult::Ok){
   if(uart.now()>=deadline){result=WireResult::Timeout;break;}
   const int count=uart.write(data,size);
   if(count<0||size_t(count)>size){result=WireResult::Driver;break;}
   if(!count){uart.pause();continue;}
   data+=count;size-=size_t(count);
  }
  return result==WireResult::Ok;
 }
 WireResult waitIdle(){
  while(result==WireResult::Ok){
   if(uart.now()>=deadline){result=WireResult::Timeout;break;}
   const int idle=uart.idle();
   if(idle>0)break;
   if(idle<0){result=WireResult::Driver;break;}
   uart.pause();
  }
  return result;
 }
 WireResult finish(){return waitIdle();}
};

inline bool writerQuiet(uint32_t pending,unsigned awaiting,bool uartIdle){
 return !pending&&!awaiting&&uartIdle;
}
inline bool canChangeTransport(bool active,bool draining,bool sessionDrained,bool quiet){
 return !active&&!draining&&sessionDrained&&quiet;
}
} // namespace dual8app
