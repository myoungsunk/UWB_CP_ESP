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
 // Caller holds the replay lock. The writer owns the record during either
 // the initial send or a replay, so a concurrent ACK only marks it then.
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
 // An unsuccessful send releases storage but still accounts for a host
 // ACK already accepted. The caller separately records the wire failure.
 bool cancelSend(){
  occupied=false;
  return ack;
 }
};

constexpr size_t CONTROL_REPLAY_BYTES=768;
constexpr uint8_t CONTROL_REPLAY_LIMIT=2;

// State for a bounded kind=1 control-frame cache. The app owns payload writes
// and replay serialization. Callers protect these state methods with its lock.
struct ControlReplaySlot {
 RecordKey key{};
 uint8_t payload[CONTROL_REPLAY_BYTES]{};
 uint16_t size=0;
 bool occupied=false, sending=false, nackPending=false, replaying=false;
 uint8_t attempts=0;
 uint64_t expires=0;

 bool offerNack(const RecordKey& incoming,uint64_t now){
  if(!occupied||!(key==incoming)||(!sending&&now>=expires)||attempts>=CONTROL_REPLAY_LIMIT||
     nackPending)return false;
  nackPending=true;
  return true;
 }
 bool beginReplay(uint64_t now){
  if(!occupied||sending||replaying||!nackPending)return false;
  if(now>=expires||attempts>=CONTROL_REPLAY_LIMIT)return false;
  nackPending=false;
  replaying=true;
  return true;
 }
 bool finishReplay(){
  if(!occupied||!replaying)return false;
  replaying=false;
  ++attempts;
  return true;
 }
 bool finishInitial(uint64_t expiry){
  if(!occupied||!sending)return false;
  sending=false;
  expires=expiry;
  return true;
 }
 uint8_t failSend(){
  if(!occupied)return 0;
  const uint8_t pending=uint8_t(nackPending)+uint8_t(replaying);
  occupied=false;sending=false;nackPending=false;replaying=false;expires=0;size=0;
  return pending;
 }
 bool expire(uint64_t now){
  if(!occupied||sending||replaying||now<expires)return false;
  const bool pending=nackPending;
  occupied=false;nackPending=false;expires=0;size=0;
  return pending;
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

inline bool writerQuiet(uint32_t pending,unsigned awaiting,bool uartIdle,unsigned controlReplayPending=0){
 return !pending&&!awaiting&&!controlReplayPending&&uartIdle;
}
inline bool canChangeTransport(bool active,bool draining,bool sessionDrained,bool quiet){
 return !active&&!draining&&sessionDrained&&quiet;
}
} // namespace dual8app
