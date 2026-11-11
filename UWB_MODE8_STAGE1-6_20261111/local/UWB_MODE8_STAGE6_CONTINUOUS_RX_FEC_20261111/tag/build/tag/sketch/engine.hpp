#line 1 "D:\\SLAM_bot\\artifacts\\UWB_MODE8_STAGE6_CONTINUOUS_RX_FEC_20261111\\tag\\firmware\\TAG_DUAL_DS_CIR\\engine.hpp"
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
 virtual void relaxRx(bool critical){(void)critical;relax();}
};
inline uint64_t deadlineAt(Port&p,uint8_t radio,uint64_t origin,uint32_t offset_us){
 const uint64_t now=p.nowUs();const int64_t lead=signed40((origin+usTicks(offset_us))&MASK40,p.system(radio));
 return lead<=0?now:now+uint64_t(double(lead)*1e6/HZ);
}
inline void stopPair(Port&p){p.idle(0);p.idle(1);}
inline bool scheduledRxQuiet(uint64_t now,uint64_t target){
 return now+500>=target&&now<target+1400;
}
inline bool waitPair(Port&p,Record&r,unsigned probe,uint64_t deadline,bool secondPoll=false){
 bool got[2]={false,false};
 for(unsigned i=0;i<2;++i)if(!p.arm(uint8_t(i))){
  r.errors|=ErrHardware;stopPair(p);return false;
 }
 // Stages 2 through 6 keep SPI quiet across the scheduled 850 kbps RF frame.
 // Both DW1000s remain armed; their RX status and CIR are latched on reception.
 const uint64_t target=deadline>=r.cfg.grace_us?deadline-r.cfg.grace_us:0;
 constexpr uint32_t preambleMarginUs=450, frameTailUs=1150;
 const bool stage6=r.cfg.stage==6;
 const uint32_t offset=secondPoll?r.cfg.probe2PollDelayUs():(probe?r.cfg.probe2_us():r.cfg.probe1DelayUs());
 uint64_t closeTicks[2]{};
 if(stage6)for(unsigned i=0;i<2;++i){
  if(r.poll[i].received)closeTicks[i]=(r.poll[i].hw+usTicks(offset+r.cfg.grace_us))&MASK40;
  else{
   const uint64_t ticks=p.system(uint8_t(i)),now=p.nowUs();
   closeTicks[i]=(ticks+usTicks(deadline>now?deadline-now:0))&MASK40;
  }
 }
 while(!(got[0]&&got[1])){
  const uint64_t now=p.nowUs();
  const bool finalSample=now>=deadline;
  if(finalSample&&!stage6)break;
  if((r.cfg.stage==2&&now+preambleMarginUs>=target&&now<target+frameTailUs)||
     (r.cfg.usesExtendedSchedule()&&scheduledRxQuiet(now,target))){
   p.relaxRx(stage6);continue;
  }
  for(unsigned i=0;i<2;++i){
   if(got[i]){continue;}
   Frame f;Rx o;const int rc=p.receive(uint8_t(i),f,o);
   if(rc==-2){r.errors|=ErrHardware;stopPair(p);return false;}
   if(rc==1){
    const bool expected=secondPoll?
      f.type==Type::Poll2&&f.session==r.session&&f.txn==r.txn&&f.config==r.config&&
      f.boot==r.peer_boot&&f.peer_boot==r.boot&&f.stage==r.cfg.stage:
      eligibleProbe(f,r,probe);
    // Host scheduling can delay inspection of a latched event. Use the
    // receiver's timestamp to enforce the original Stage 6 on-air deadline.
    if(expected&&(!stage6||signed40(o.hw,closeTicks[i])<=0)){
     const uint64_t scheduled=(r.poll_frame.poll_tx+usTicks(offset))&MASK40;
     auto& captured=secondPoll?r.poll2_frame:r.probe_frame[probe];
     auto& have=secondPoll?r.have_poll2_frame:r.have_probe_frame[probe];
     if((!secondPoll&&f.poll_tx!=r.poll_frame.poll_tx)||diff40(scheduled,secondPoll?f.poll_tx:f.probe_tx)>511||
        (have&&!sameFrame(captured,f))){
      r.metadata_conflict=true;r.errors|=ErrMetadata;
     }else{captured=f;have=true;}
     got[i]=true;
     if(finalSample)r.rx_deadline_latch|=uint8_t(1U<<((secondPoll?2U:probe?4U:0U)+i));
     (secondPoll?r.poll2_rx[i]:r.probe[probe][i])=o;
    }else if(!finalSample&&!p.arm(uint8_t(i))){r.errors|=ErrHardware;stopPair(p);return false;}
   }else if(rc==-1&&!finalSample&&!p.arm(uint8_t(i))){r.errors|=ErrHardware;stopPair(p);return false;}
  }
  if(finalSample)break;
  p.relaxRx(stage6&&now+3000>=deadline);
 }
 // No receiver is restarted before both snapshots are copied. This also keeps
 // both radios out of RX while using the high-speed accumulator-read path.
 stopPair(p);
 if(!got[0]||!got[1])r.errors|=secondPoll?ErrPoll2:(probe?ErrProbe2:ErrProbe1);
 if(secondPoll)return got[0]||got[1];
 // Alternate the SPI read order to expose order-dependent retention defects.
 for(unsigned j=0;j<2;++j){const unsigned i=j^(r.txn&1U);if(got[i]){
  const unsigned slot=2*probe+i;
  const CirRead result=p.readCir(uint8_t(i),r.cir[slot],r.cir_meta[slot]);
  if(result==CirRead::HardwareFault){r.errors|=ErrHardware;return false;}
  if(result==CirRead::Invalid)r.errors|=ErrCir;
 }}
 return got[0]||got[1];
}
inline void sendTagResponses(Port&p,Record&r,const Rx*rx,uint64_t origin,Tx*tx,bool second){
 const auto& c=r.cfg;
 for(unsigned i=0;i<2;++i){
  const uint32_t error=second?(i?ErrResponse2B:ErrResponse2A):(i?ErrResponseB:ErrResponseA);
  if(!rx[i].received||!rx[i].lde){r.errors|=error;continue;}
  Frame f;f.type=responseType(second,i);f.radio=uint8_t(i);f.session=r.session;f.txn=r.txn;
  f.config=r.config;f.boot=r.boot;f.peer_boot=r.peer_boot;f.stage=c.stage;
  f.poll_tx=origin;f.tag_poll_rx=rx[i].hw;
  const uint32_t delay=c.responseDelayUs(i);
  const uint64_t requested=(rx[i].hw+usTicks(delay))&MASK40;
  if(!p.send(uint8_t(i),f,requested,tx[i]))r.errors|=error;
 }
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
   if(sameFrame(f,first)){r.poll[other]=o;if(c.usesExtendedSchedule())break;}
   else if(!p.arm(uint8_t(other))){r.errors|=ErrHardware;break;}
  }else if(rc==-1&&!p.arm(uint8_t(other))){r.errors|=ErrHardware;break;}
  p.relax();
 }
 stopPair(p);
 if(!r.poll[0].received||!r.poll[1].received)r.errors|=ErrPoll;
 if(r.errors&ErrHardware){r.end_us=p.nowUs();return;}
 // The other radio is idle during TX, even if it missed the reference packet.
 if(c.stage>=3)sendTagResponses(p,r,r.poll,first.poll_tx,r.tx,false);
 if(!p.healthy(0)||!p.healthy(1)){r.errors|=ErrHardware;stopPair(p);r.end_us=p.nowUs();return;}
 const uint64_t p1Deadline=deadlineAt(p,firstRadio,firstRx.hw,c.probe1DelayUs()+c.grace_us);
 waitPair(p,r,0,p1Deadline);
 if(c.rangesProbe2()&&!(r.errors&(ErrHardware|ErrMetadata))){
  const uint64_t target=deadlineAt(p,firstRadio,firstRx.hw,c.probe2PollDelayUs());
  if(p.nowUs()+c.min_lead_us>=target)r.errors|=ErrLate|ErrPoll2;
  else waitPair(p,r,0,target+c.grace_us,true);
  if(r.have_poll2_frame&&!(r.errors&(ErrHardware|ErrMetadata)))
   sendTagResponses(p,r,r.poll2_rx,r.poll2_frame.poll_tx,r.response2_tx,true);
  else r.errors|=ErrResponse2A|ErrResponse2B;
 }
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
inline void receiveAnchorResponses(Port&p,Record&r,uint64_t origin,Rx*responses,uint8_t&mask,bool second){
  const auto& c=r.cfg;
  if(!p.arm(0)){r.errors|=ErrHardware;return;}
  const uint32_t delayA=c.responseDelayUs(0);
  const uint32_t delayB=c.responseDelayUs(1);
  const uint32_t delays[]={delayA,delayB};
  const uint64_t end=deadlineAt(p,0,origin,delayB+c.grace_us);
  const uint64_t responseB=end-c.grace_us;
  const uint64_t responseA=responseB-(delayB-delayA);
  const bool stage6=c.stage==6;
  while(mask!=3){
   const uint64_t now=p.nowUs();
   const bool finalSample=now>=end;
   if(finalSample&&!stage6)break;
   if(c.usesExtendedSchedule()&&(scheduledRxQuiet(now,responseA)||scheduledRxQuiet(now,responseB))){
    p.relaxRx(stage6);continue;
   }
   Frame q;Rx o;int rc=p.receive(0,q,o);
   if(rc==-2){r.errors|=ErrHardware;break;}
   if(rc==1){
    const unsigned i=q.type==responseType(second,1)?1:0;
    const int64_t timeError=signed40(o.hw,(origin+usTicks(delays[i]))&MASK40);
    const bool inSlot=timeError>=-int64_t(usTicks(1000))&&timeError<int64_t(usTicks(c.grace_us));
    if(q.type==responseType(second,i)&&q.session==r.session&&q.txn==r.txn&&q.config==r.config&&q.peer_boot==r.boot&&q.stage==c.stage&&
       q.poll_tx==origin&&(!r.peer_boot||r.peer_boot==q.boot)&&inSlot&&!(mask&(1U<<i))&&o.lde){
      r.peer_boot=q.boot;responses[i]=o;mask|=uint8_t(1U<<i);
      if(finalSample)r.rx_deadline_latch|=uint8_t(1U<<(i+(second?2U:0U)));
    }
    if(!finalSample&&mask!=3&&!p.arm(0)){r.errors|=ErrHardware;break;}
   }else if(rc==-1&&!finalSample&&!p.arm(0)){r.errors|=ErrHardware;break;}
   if(finalSample)break;
   p.relaxRx(stage6&&(now+3500>=end||(now+800>=responseA&&now<responseA+c.grace_us)));
  }
  p.idle(0);
  if(!(mask&1))r.errors|=second?ErrResponse2A:ErrResponseA;
  if(!(mask&2))r.errors|=second?ErrResponse2B:ErrResponseB;
}
inline void runAnchor(Port&p,const Config&c,uint32_t session,uint32_t txn,uint32_t localBoot,uint32_t expectedTagBoot,Record&r){
 r.anchor=true;r.cfg=c;r.begin_us=p.nowUs();r.session=session;r.txn=txn;r.boot=localBoot;r.config=configId(c);r.peer_boot=expectedTagBoot;
 Frame f;f.type=Type::Poll;f.session=session;f.txn=txn;f.config=r.config;f.boot=localBoot;f.stage=c.stage;
 const uint64_t requested=(p.system(0)+usTicks(3000))&MASK40;
 if(!p.send(0,f,requested,r.tx[0])){r.errors|=ErrPoll;r.end_us=p.nowUs();return;}
 r.poll_frame=f;
 if(c.stage>=3)receiveAnchorResponses(p,r,r.tx[0].actual,r.responses,r.response_mask,false);
 if(r.errors&ErrHardware){r.end_us=p.nowUs();return;}
 for(unsigned k=0;k<2;++k){
  if(k&&c.rangesProbe2()){
   // Begin the second exchange with the same POLL, reply and probe intervals.
   const uint32_t offset=c.probe2PollDelayUs();
   const uint64_t prepare=deadlineAt(p,0,r.tx[0].actual,offset-3500);
   while(p.nowUs()<prepare)p.relax();
   Frame q=f;q.type=Type::Poll2;q.peer_boot=r.peer_boot;
   const uint64_t target=(r.tx[0].actual+usTicks(offset))&MASK40;
   if(q.peer_boot&&p.send(0,q,target,r.poll2_tx)){
    r.poll2_frame=q;r.have_poll2_frame=true;
    receiveAnchorResponses(p,r,r.poll2_tx.actual,r.response2_rx,r.response2_mask,true);
   }else r.errors|=ErrPoll2|ErrResponse2A|ErrResponse2B;
   if(r.errors&ErrHardware)break;
  }
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
