#pragma once
#include "core.hpp"
namespace dual8 {
enum class CirRead { Valid, Invalid, HardwareFault };
// Exactly one task owns this interface and all SPI/context selection. Writer never calls it.
class Port {
public:
 virtual ~Port(){}
 virtual uint64_t nowUs()=0;
 virtual uint64_t system(uint8_t radio)=0;
 virtual bool healthy(uint8_t radio)=0;
 virtual bool arm(uint8_t radio)=0;
 virtual void idle(uint8_t radio)=0;
 // 1 = CRC-valid new-protocol packet and frozen receiver; 0 = pending;
 // -1 = rejected/error (safe to re-arm); -2 = latched hardware integrity failure.
 virtual int receive(uint8_t radio,Frame&f,Rx&o)=0;
 virtual bool send(uint8_t radio,Frame&f,uint64_t requested,Tx&o)=0;
 virtual CirRead readCir(uint8_t radio,uint8_t*dst,Cir&o)=0;
 virtual void relax()=0;
};
inline uint64_t deadlineAt(Port&p,uint8_t radio,uint64_t origin,uint32_t offset_us){
 const uint64_t now=p.nowUs();const int64_t lead=signed40((origin+usTicks(offset_us))&MASK40,p.system(radio));
 return lead<=0?now:now+uint64_t(double(lead)*1e6/HZ);
}
inline void stopPair(Port&p){p.idle(0);p.idle(1);}
inline bool stage3RxWindow(uint64_t now,uint64_t target){
 return now+500>=target&&now<target+1400;
}
inline bool waitPair(Port&p,Record&r,unsigned probe,uint64_t deadline){
 bool got[2]={false,false};
 for(unsigned i=0;i<2;++i)if(!p.arm(uint8_t(i))){
  r.errors|=ErrHardware;stopPair(p);return false;
 }
 // Stages 2 and 3 keep SPI quiet across the scheduled 850 kbps RF frame.
 // Both DW1000s remain armed; their RX status and CIR are latched on reception.
 const uint64_t target=deadline>=r.cfg.grace_us?deadline-r.cfg.grace_us:0;
 constexpr uint32_t preambleMarginUs=450, frameTailUs=1150;
 while(p.nowUs()<deadline&&!(got[0]&&got[1])){
  const uint64_t now=p.nowUs();
  if((r.cfg.stage==2&&now+preambleMarginUs>=target&&now<target+frameTailUs)||
     (r.cfg.stage==3&&stage3RxWindow(now,target))){
   p.relax();continue;
  }
  for(unsigned i=0;i<2;++i){
   if(got[i]){continue;}
   Frame f;Rx o;const int rc=p.receive(uint8_t(i),f,o);
   if(rc==-2){r.errors|=ErrHardware;stopPair(p);return false;}
   if(rc==1){
    if(eligibleProbe(f,r,probe)){
     const uint64_t target=(r.poll_frame.poll_tx+usTicks(probe?r.cfg.probe2_us():r.cfg.probe1DelayUs()))&MASK40;
     if(f.poll_tx!=r.poll_frame.poll_tx||diff40(target,f.probe_tx)>511){r.metadata_conflict=true;r.errors|=ErrMetadata;}
     got[i]=true;r.probe[probe][i]=o;
     if(r.have_probe_frame[probe]&&!sameFrame(r.probe_frame[probe],f)){r.metadata_conflict=true;r.errors|=ErrMetadata;}
     else {r.probe_frame[probe]=f;r.have_probe_frame[probe]=true;}
    }else if(!p.arm(uint8_t(i))){r.errors|=ErrHardware;stopPair(p);return false;}
   }else if(rc==-1&&!p.arm(uint8_t(i))){r.errors|=ErrHardware;stopPair(p);return false;}
  }
  p.relax();
 }
 // No receiver is restarted before both snapshots are copied. This also keeps
 // both radios out of RX while using the high-speed accumulator-read path.
 stopPair(p);
 if(!got[0]||!got[1])r.errors|=probe?ErrProbe2:ErrProbe1;
 // Alternate the SPI read order to expose order-dependent retention defects.
 for(unsigned j=0;j<2;++j){const unsigned i=j^(r.txn&1U);if(got[i]){
  const unsigned slot=2*probe+i;
  const CirRead result=p.readCir(uint8_t(i),r.cir[slot],r.cir_meta[slot]);
  if(result==CirRead::HardwareFault){r.errors|=ErrHardware;return false;}
  if(result==CirRead::Invalid)r.errors|=ErrCir;
 }}
 return got[0]||got[1];
}
inline void runTag(Port&p,const Config&c,uint32_t localBoot,uint8_t firstRadio,const Frame&first,const Rx&firstRx,Record&r){
 r.anchor=false;r.cfg=c;r.begin_us=p.nowUs();r.session=first.session;r.txn=first.txn;r.boot=localBoot;r.peer_boot=first.boot;r.config=configId(c);
 r.poll_frame=first;r.poll[firstRadio]=firstRx;
 const unsigned other=firstRadio^1U;
 const uint64_t pollDeadline=deadlineAt(p,firstRadio,firstRx.hw,c.reply_a_us-2500);
 while(p.nowUs()<pollDeadline&&!r.poll[other].received){
  Frame f;Rx o;const int rc=p.receive(uint8_t(other),f,o);
  if(rc==-2){r.errors|=ErrHardware;break;}
  if(rc==1){
   if(sameFrame(f,first)){r.poll[other]=o;if(c.stage==3)break;}
   else if(!p.arm(uint8_t(other))){r.errors|=ErrHardware;break;}
  }else if(rc==-1&&!p.arm(uint8_t(other))){r.errors|=ErrHardware;break;}
  p.relax();
 }
 stopPair(p);
 if(!r.poll[0].received||!r.poll[1].received)r.errors|=ErrPoll;
 if(r.errors&ErrHardware){r.end_us=p.nowUs();return;}
 // Each RESPONSE is scheduled exclusively in its own radio's time domain.
 // The other radio is OFF (not receiving), including when its POLL was lost.
 if(c.stage>=3){
  for(unsigned i=0;i<2;++i){
   if(!r.poll[i].received||!r.poll[i].lde){r.errors|=i?ErrResponseB:ErrResponseA;continue;}
   Frame f;f.type=i?Type::RespB:Type::RespA;f.radio=uint8_t(i);f.session=r.session;f.txn=r.txn;f.config=r.config;f.boot=localBoot;f.peer_boot=first.boot;f.stage=c.stage;
   f.poll_tx=first.poll_tx;f.tag_poll_rx=r.poll[i].hw;
   const uint64_t requested=(r.poll[i].hw+usTicks(c.responseDelayUs(i)))&MASK40;
   if(!p.send(uint8_t(i),f,requested,r.tx[i]))r.errors|=i?ErrResponseB:ErrResponseA;
  }
 }
 if(!p.healthy(0)||!p.healthy(1)){r.errors|=ErrHardware;stopPair(p);r.end_us=p.nowUs();return;}
 const uint64_t p1Deadline=deadlineAt(p,firstRadio,firstRx.hw,c.probe1DelayUs()+c.grace_us);
 waitPair(p,r,0,p1Deadline);
 if(!(r.errors&ErrHardware)){
  const uint64_t p2Target=deadlineAt(p,firstRadio,firstRx.hw,c.probe2_us());
  if(p.nowUs()+c.min_lead_us>=p2Target)r.errors|=ErrLate;
  else waitPair(p,r,1,p2Target+c.grace_us);
 }
 stopPair(p);
 if(r.have_probe_frame[0])r.response_mask=r.probe_frame[0].resp_mask;
 if(r.have_probe_frame[0]&&r.have_probe_frame[1]&&!coherentAnchorBlock(r.probe_frame[0],r.probe_frame[1])){r.metadata_conflict=true;r.errors|=ErrMetadata;}
 finalizeRanges(r);
 if(c.stage>=4&&(!r.range[0].valid||!r.range[1].valid))r.errors|=ErrRange;
 r.end_us=p.nowUs();
}
inline void runAnchor(Port&p,const Config&c,uint32_t session,uint32_t txn,uint32_t localBoot,uint32_t expectedTagBoot,Record&r){
 r.anchor=true;r.cfg=c;r.begin_us=p.nowUs();r.session=session;r.txn=txn;r.boot=localBoot;r.config=configId(c);r.peer_boot=expectedTagBoot;
 Frame f;f.type=Type::Poll;f.session=session;f.txn=txn;f.config=r.config;f.boot=localBoot;f.stage=c.stage;
 const uint64_t requested=(p.system(0)+usTicks(3000))&MASK40;
 if(!p.send(0,f,requested,r.tx[0])){r.errors|=ErrPoll;r.end_us=p.nowUs();return;}
 r.poll_frame=f;
 if(c.stage>=3){
  if(!p.arm(0)){r.errors|=ErrHardware;r.end_us=p.nowUs();return;}
  const uint64_t end=deadlineAt(p,0,r.tx[0].actual,c.responseDelayUs(1)+c.grace_us);
  const uint64_t responseB=end-c.grace_us;
  const uint64_t responseA=responseB-(c.responseDelayUs(1)-c.responseDelayUs(0));
  while(p.nowUs()<end&&r.response_mask!=3){
   const uint64_t now=p.nowUs();
   if(c.stage==3&&(stage3RxWindow(now,responseA)||stage3RxWindow(now,responseB))){
    p.relax();continue;
   }
   Frame q;Rx o;int rc=p.receive(0,q,o);
   if(rc==-2){r.errors|=ErrHardware;break;}
   if(rc==1){
    const unsigned t=unsigned(q.type);const bool resp=t==2||t==3;const unsigned i=t==3?1:0;
    const int64_t timeError=signed40(o.hw,(r.tx[0].actual+usTicks(c.responseDelayUs(i)))&MASK40);
    const bool inSlot=timeError>=-int64_t(usTicks(1000))&&timeError<int64_t(usTicks(c.grace_us));
    if(resp&&q.session==session&&q.txn==txn&&q.config==r.config&&q.peer_boot==localBoot&&q.stage==c.stage&&
       q.poll_tx==r.tx[0].planned&&(!r.peer_boot||r.peer_boot==q.boot)&&inSlot&&!(r.response_mask&(1U<<i))&&o.lde){
      r.peer_boot=q.boot;r.responses[i]=o;r.response_mask|=uint8_t(1U<<i);
    }
    if(r.response_mask!=3&&!p.arm(0)){r.errors|=ErrHardware;break;}
   }else if(rc==-1&&!p.arm(0)){r.errors|=ErrHardware;break;}
   p.relax();
  }
  p.idle(0);
  if(!(r.response_mask&1))r.errors|=ErrResponseA;
  if(!(r.response_mask&2))r.errors|=ErrResponseB;
 }
 if(r.errors&ErrHardware){r.end_us=p.nowUs();return;}
 for(unsigned k=0;k<2;++k){
  Frame q;q.type=k?Type::Probe2:Type::Probe1;q.session=session;q.txn=txn;q.config=r.config;q.boot=localBoot;q.peer_boot=r.peer_boot;q.stage=c.stage;
  q.poll_tx=r.tx[0].actual;q.resp_mask=r.response_mask;q.tx_ok_mask=1;
  for(unsigned i=0;i<2;++i){q.resp_rx[i]=r.responses[i].hw;q.ci[i]=r.responses[i].ci;}
  if(k){q.prev_probe_actual=r.tx[1].actual;if(r.tx[1].ok)q.tx_ok_mask|=2;}
  const uint64_t at=(r.tx[0].actual+usTicks(k?c.probe2_us():c.probe1DelayUs()))&MASK40;
  if(!p.send(0,q,at,r.tx[k+1]))r.errors|=k?ErrProbe2:ErrProbe1;
  r.probe_frame[k]=q;r.have_probe_frame[k]=r.tx[k+1].ok;
 }
 r.p1_tx_confirmed=r.tx[1].ok;r.complete=r.errors==0&&r.tx[0].ok&&r.tx[1].ok&&r.tx[2].ok&&(c.stage==2||r.response_mask==3);
 p.idle(0);r.end_us=p.nowUs();
}
} // namespace dual8
