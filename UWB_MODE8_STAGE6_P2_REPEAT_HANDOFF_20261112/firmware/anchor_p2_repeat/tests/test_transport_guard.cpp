#include "transport_guard.hpp"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <deque>
#include <vector>

using namespace dual8app;

struct FakeUart {
 uint64_t us=0, idleAt=0;
 unsigned attempts=0;
 std::deque<int> accepted;
 std::vector<uint8_t> written;
 int defaultWrite=0;
 bool idleError=false;
 uint64_t now() const {return us;}
 void pause(){us+=10;}
 int write(const uint8_t* data,size_t size){
  ++attempts;
  int result=defaultWrite;
  if(!accepted.empty()){result=accepted.front();accepted.pop_front();}
  if(result>0&&size_t(result)<=size)written.insert(written.end(),data,data+result);
  return result;
 }
 int idle() const {return idleError?-1:us>=idleAt;}
};

static ReplaySlot cacheAt(const RecordKey& key,uint64_t deadline){
 ReplaySlot s;
 s.occupied=true;
 s.key=key;
 s.expires=deadline;
 return s;
}

static void checkReplay(unsigned pool){
 const RecordKey key(100,200,300,400);
 auto pendingSend=cacheAt(key,0);
 assert(pendingSend.offer(key,true,10)); // Host ACK during final FIFO drain.
 assert(pendingSend.take(11)==ReplayDecision::None);
 pendingSend.expires=100;
 assert(pendingSend.take(100)==ReplayDecision::Ack);
 assert(!pendingSend.offer(key,true,101));

 auto slot=cacheAt(key,100);
 assert(!slot.offer({101,200,300,400},true,11));
 assert(!slot.offer({100,201,300,400},true,11));
 assert(!slot.offer({100,200,301,400},true,11));
 assert(!slot.offer({100,200,300,401},true,11));
 assert(slot.offer(key,false,98));
 assert(!slot.offer(key,false,98));
 assert(slot.offer(key,true,99)); // Durable ACK supersedes NACK.
 assert(!slot.offer(key,true,99));
 assert(!slot.offer(key,false,99));
 assert(slot.take(150)==ReplayDecision::Ack); // Pre-fix writer expired first.
 assert(slot.take(150)==ReplayDecision::None);
 assert(!slot.offer(key,true,150));
 slot=cacheAt(key,100);
 assert(!slot.offer(key,true,100));
 assert(slot.take(100)==ReplayDecision::Expired);

 slot=cacheAt(key,100);
 assert(slot.offer(key,false,98));
 assert(slot.take(99)==ReplayDecision::Nack);
 assert(slot.take(99)==ReplayDecision::None);
 assert(slot.offer(key,true,99));
 assert(slot.take(150)==ReplayDecision::None); // Replay still owns the record.
 assert(slot.finishReplay());
 assert(!slot.takeAck());

 std::vector<ReplaySlot> slots;
 for(unsigned i=0;i<pool;++i)slots.push_back(cacheAt({100,200,300,400+i},100));
 // Triplicate each notice while every cache slot is occupied, bypassing
 // the bounded output queue (five tag slots generate 15 notices).
 for(unsigned i=0;i<pool;++i){
  for(unsigned j=0;j<20;++j)
   assert(slots[i].offer({100,200,300,400+i},true,99)==(j==0));
 }
 for(auto& cached:slots)assert(cached.take(1000)==ReplayDecision::Ack);
 slot=cacheAt({100,200,300,500},100); // A reused index has a fresh sequence.
 assert(!slot.offer(key,true,1));
 assert(slot.take(101)==ReplayDecision::Expired);
}

static void checkAckReclaimsFullPoolDuringOtherWrite(){
 constexpr uint64_t scheduled=1000000;
 constexpr uint64_t deadline=1500000;
 ReplaySlot slots[5];
 for(unsigned i=0;i<5;++i)slots[i]=cacheAt({100,200,300,400+i},i==4?0:deadline);
 // Run 06 txn 5: four accepted ACKs, all five slots occupied, and one
 // UART write still in flight. The RF scheduler needs one free slot now.
 unsigned reclaimed=0;
 for(unsigned i=0;i<4;++i){
  const RecordKey key(100,200,300,400+i);
  assert(slots[i].offer(key,true,scheduled-2000));
  reclaimed+=slots[i].takeAck();
  assert(!slots[i].offer(key,true,scheduled));
  assert(!slots[i].takeAck());
 }
 assert(reclaimed==4);
 assert(slots[4].occupied&&!slots[4].takeAck());
 const RecordKey inFlight(100,200,300,404);
 assert(slots[4].offer(inFlight,true,scheduled));
 assert(!slots[4].takeAck());
 slots[4].expires=deadline;
 assert(slots[4].takeAck());
 assert(!slots[4].takeAck());

 auto failedInitial=cacheAt(inFlight,0);
 assert(failedInitial.offer(inFlight,true,scheduled));
 assert(!failedInitial.takeAck());
 assert(failedInitial.cancelSend()); // Account for ACK even if the UART send fails.
 assert(!failedInitial.occupied&&!failedInitial.takeAck());
 assert(!failedInitial.offer(inFlight,true,scheduled));

 // ACK arriving during a NACK replay is deferred until writer completion.
 auto replay=cacheAt(inFlight,deadline);
 assert(replay.offer(inFlight,false,scheduled));
 assert(replay.take(scheduled)==ReplayDecision::Nack);
 assert(replay.offer(inFlight,true,scheduled));
 assert(!replay.takeAck());
 assert(replay.finishReplay());
 assert(!replay.takeAck());

 // Without a concurrent ACK a successful replay keeps its cache and expiry.
 replay=cacheAt(inFlight,deadline);
 assert(replay.offer(inFlight,false,scheduled));
 assert(replay.take(scheduled)==ReplayDecision::Nack);
 assert(!replay.finishReplay()&&replay.occupied);
 assert(replay.take(deadline)==ReplayDecision::Expired);
}

static void testDecimalBoundaries(){
 uint32_t out=17;
 assert(parseDecimalU32("0",out)&&out==0);
 assert(parseDecimalU32("00042",out)&&out==42);
 assert(parseDecimalU32("4294967295",out)&&out==0xffffffffU);
 for(const char* invalid:{"", "-1", "+1", " 1", "1 ", "1.0",
                          "1e2", "4294967296", "999999999999999999999999"}){
  assert(!parseDecimalU32(invalid,out));
 }
 assert(!parseDecimalU32(nullptr,out));
}

static void checkDrain(){
 assert(!writerQuiet(1,0,true)); // In-flight writer is included in pending.
 assert(!writerQuiet(0,1,true)); // STOP alone does not retire ACK cache.
 assert(!writerQuiet(0,0,false)); // FIFO tail still physically transmitting.
 assert(writerQuiet(0,0,true));
 assert(!canChangeTransport(true,false,true,true));
 assert(!canChangeTransport(false,true,true,true));
 assert(!canChangeTransport(false,false,false,true));
 assert(!canChangeTransport(false,false,true,false));
 assert(canChangeTransport(false,false,true,true));
}

static void checkUart(){
 const uint8_t bytes[]={1,2,3,4};
 {
  FakeUart uart;
  uart.accepted={1,0,1,2};
  uart.idleAt=20;
  BoundedWire<FakeUart> tx(uart,40);
  assert(tx.send(bytes,2));
  assert(tx.send(bytes+2,2));
  assert(tx.finish()==WireResult::Ok);
  assert(uart.us==20);
  assert(uart.written==std::vector<uint8_t>(bytes,bytes+4));
 }
 {
  FakeUart uart;
  uart.accepted={1};
  BoundedWire<FakeUart> tx(uart,30);
  assert(tx.send(bytes,1));
  uart.us=20; // Header and payload use one deadline, not one each.
  assert(!tx.send(bytes+1,3));
  assert(tx.finish()==WireResult::Timeout);
  assert(uart.us==30&&uart.written.size()==1);
  uart.idleAt=40;
  BoundedWire<FakeUart> recovery(uart,30);
  assert(recovery.waitIdle()==WireResult::Ok);
  assert(uart.us==40);
  uart.accepted={3};
  assert(recovery.send(bytes+1,3));
  assert(recovery.finish()==WireResult::Ok);
 }
 {
  FakeUart uart;
  uart.accepted={4};
  uart.idleAt=20;
  BoundedWire<FakeUart> tx(uart,20);
  assert(tx.send(bytes,4));
  assert(tx.finish()==WireResult::Timeout);
 }
 for(int invalid:{-1,5}){
  FakeUart uart;
  uart.accepted={invalid};
  BoundedWire<FakeUart> tx(uart,50);
  assert(!tx.send(bytes,4));
  assert(tx.finish()==WireResult::Driver);
  assert(uart.attempts==1);
 }
 {
  FakeUart uart;
  uart.idleError=true;
  BoundedWire<FakeUart> tx(uart,20);
  assert(tx.waitIdle()==WireResult::Driver);
  assert(!tx.send(bytes,4));
  assert(uart.attempts==0);
 }
}

int main(){
 checkReplay(3);
 checkReplay(5);
 checkDrain();
 testDecimalBoundaries();
 checkAckReclaimsFullPoolDuringOtherWrite();
 checkUart();
 std::puts("Stage 6 bounded UART and drain regressions passed");
}
