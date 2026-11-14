#pragma once
#include <stddef.h>
#include <stdint.h>

namespace dual8app {

// Strict decimal parser for TCP inputs. strtoul can accept signed values
// and saturate on overflow; neither is valid for a Mode 8 32-bit identifier.
inline bool parseDecimalU32(const char* str,uint32_t& out){
 if(!str||!*str)return false;
 uint64_t n=0;
 for(const char* p=str;*p;++p){
  if(*p<'0'||*p>'9')return false;
  n=n*10+uint64_t(*p-'0');
  if(n>0xffffffffULL)return false;
 }
 out=uint32_t(n);return true;
}


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
 bool occupied=false, ack=false, nack=false, replaying=false;
 uint8_t attempts=0;
 // Zero while the initial send is still in progress.
 uint64_t expires=0;

 bool offer(const RecordKey& incoming,bool positive,uint64_t now){
  if(!occupied||!(key==incoming)||(expires&&now>=expires)||ack)return false;
  if(positive){ack=true;nack=false;return true;}
  if(nack)return false;
  nack=true;return true;
 }
 // Writer owns the record during initial/replay transmission. An ACK received
 // while the initial UART send is still active is retired by the writer after
 // it publishes expiry; during replay it waits for finishReplay().
 bool takeAck(){
  if(!occupied||!expires||replaying||!ack)return false;
  occupied=false;
  return true;
 }
 ReplayDecision take(uint64_t now){
  if(!occupied||!expires||replaying)return ReplayDecision::None;
  if(takeAck())return ReplayDecision::Ack;
  if(now>=expires){occupied=false;return ReplayDecision::Expired;}
  if(nack){nack=false;replaying=true;return ReplayDecision::Nack;}
  return ReplayDecision::None;
 }
 bool finishReplay(){
  replaying=false;
  return takeAck();
 }
 bool cancelSend(){
  occupied=false;
  return ack;
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
