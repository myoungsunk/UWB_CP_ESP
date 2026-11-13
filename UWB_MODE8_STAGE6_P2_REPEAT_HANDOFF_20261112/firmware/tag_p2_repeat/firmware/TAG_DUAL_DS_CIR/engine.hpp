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
 virtual void traceRx(RxWindowTrace* traces){(void)traces;}
 virtual void relax()=0;
 virtual void relaxRx(bool critical){(void)critical;relax();}
};
inline uint64_t deadlineAt(Port&p,uint8_t radio,uint64_t origin,uint32_t offset_us,bool centerSample=false){
 const uint64_t before=p.nowUs();
 const uint64_t ticks=p.system(radio);
 const int64_t lead=signed40((origin+usTicks(offset_us))&MASK40,ticks);
 if(!centerSample)return lead<=0?before:before+uint64_t(double(lead)*1e6/HZ);
 const uint64_t after=p.nowUs();
 if(lead<=0)return after;
 const uint64_t estimate=before+(after-before)/2+uint64_t(double(lead)*1e6/HZ);
 return estimate>after?estimate:after;
}
inline void stopPair(Port&p){p.idle(0);p.idle(1);}
inline bool scheduledRxQuiet(uint64_t now,uint64_t target){
 return now+500>=target&&now<target+1400;
}
struct ProbeRepeatCapture {
 Frame frame{};
 Rx rx[2]{};
 Cir cir[2]{};
 uint8_t samples[2][CIR_BYTES];
 bool have_frame=false;
 uint8_t deadline_latch=0;
 void reset(){
  have_frame=false;deadline_latch=0;
  for(unsigned i=0;i<2;++i){rx[i]=Rx{};cir[i]=Cir{};}
 }
};
enum class Probe1Recovery { None, Poll, Final };
inline bool waitPair(Port&p,Record&r,unsigned probe,uint64_t deadline,bool secondPoll=false,
                     bool acceptRetry=false,ProbeRepeatCapture* repeat=nullptr,
                     Probe1Recovery recovery=Probe1Recovery::None){
 bool got[2]={false,false},probeGot[2]={false,false};
 const bool repeatPoll=recovery==Probe1Recovery::Poll;
 const bool repeatFinal=recovery==Probe1Recovery::Final;
 const unsigned window=repeatPoll?7:secondPoll?(r.p2_retry?3:2):probe?(r.p2_retry?5:4):repeat?6:(r.p1_retry?1:0);
 auto& traces=r.rx_windows[window];
 struct TraceScope {
  Port& port; RxWindowTrace* trace;
  TraceScope(Port& p,RxWindowTrace* t):port(p),trace(t){port.traceRx(trace);}
  ~TraceScope(){
   const uint64_t now=port.nowUs();
   for(unsigned i=0;i<2;++i)trace[i].close_us=now;
   port.traceRx(nullptr);
  }
 } trace(p,traces);
 for(unsigned i=0;i<2;++i)if(!p.arm(uint8_t(i))){
  r.errors|=ErrHardware;stopPair(p);return false;
 }
 // Stages 2 through 6 keep SPI quiet across the scheduled 850 kbps RF frame.
 // Both DW1000s remain armed; their RX status and CIR are latched on reception.
 const uint64_t target=deadline>=r.cfg.grace_us?deadline-r.cfg.grace_us:0;
 constexpr uint32_t preambleMarginUs=450, frameTailUs=1150;
 const bool stage6=r.cfg.stage==6;
 const bool probeQuiet=stage6&&!secondPoll&&!repeatPoll;
 // Keep shared SPI inactive around scheduled Probe reception on both radios.
 // The larger guard covers the PHY frame and clock/projection uncertainty.
 // Cap it at the existing deadline so the final latched sample still runs.
 constexpr uint32_t probeQuietLeadUs=1000,probeQuietTailUs=1900;
 const uint64_t quietBegin=target>probeQuietLeadUs?target-probeQuietLeadUs:0;
 const uint64_t quietEnd=target+probeQuietTailUs<deadline?target+probeQuietTailUs:deadline;
 if(probeQuiet&&probe&&!repeat){
  auto& bounds=r.probe2_spi_quiet[r.p2_retry?1:0];
  bounds[0]=quietBegin;bounds[1]=quietEnd;
 }
 const uint32_t offset=secondPoll?r.cfg.probe2PollDelayUs(r.p2_retry):
                       repeatPoll?r.cfg.probe1RepeatUs():
                       probe?r.cfg.probe2_us(r.p2_retry):r.cfg.probe1DelayUs();
 const uint64_t reference=repeatFinal?r.p1_repeat_poll_frame.poll_tx:r.poll_frame.poll_tx;
 const Rx* referenceRx=repeatFinal?r.p1_repeat_poll_rx:r.poll;
 bool retrySelected=false;
 uint64_t closeTicks[2]{};
 if(stage6)for(unsigned i=0;i<2;++i){
  if(referenceRx[i].received)closeTicks[i]=(referenceRx[i].hw+usTicks(offset+r.cfg.grace_us))&MASK40;
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
     (r.cfg.usesExtendedSchedule()&&(probeQuiet?
        now>=quietBegin&&now<quietEnd:scheduledRxQuiet(now,target)))){
   p.relaxRx(stage6);continue;
  }
  for(unsigned i=0;i<2;++i){
   if(got[i]){continue;}
   Frame f;Rx o;const int rc=p.receive(uint8_t(i),f,o);
   if(rc==-2){r.errors|=ErrHardware;stopPair(p);return false;}
   if(rc==1){
    const bool p1Retry=acceptRetry&&!probe&&isRetryPoll(f);
    const bool retryPacket=p1Retry;
    const bool pollPacket=secondPoll||retryPacket||repeatPoll;
    const bool expected=pollPacket?
      (p1Retry?(f.probe_tx==reference&&(!f.peer_boot||f.peer_boot==r.boot)):
                repeatPoll?(isProbe1RepeatPoll(f)&&f.probe_tx==reference&&f.peer_boot==r.boot):
                (f.type==Type::Poll2&&f.peer_boot==r.boot))&&
      f.session==r.session&&f.txn==r.txn&&f.config==r.config&&f.boot==r.peer_boot&&f.stage==r.cfg.stage:
      eligibleProbe(f,r,probe)&&
      (!repeatFinal||(f.peer_boot==r.boot&&f.resp_mask==3&&f.tx_ok_mask==1&&
                      !f.prev_probe_actual&&!f.tag_poll_rx&&!f.tag_resp_tx))&&
      (!stage6||!probe||
       (r.p2_retry?(r.have_poll2_frame&&f.tag_resp_tx==r.poll2_frame.poll_tx):f.tag_resp_tx==0));
    // Host scheduling can delay inspection of a latched event. Use the
    // receiver's timestamp to enforce the original Stage 6 on-air deadline.
    if(expected&&(!stage6||signed40(o.hw,closeTicks[i])<=0)){
     const uint64_t scheduled=(reference+usTicks(offset))&MASK40;
     const bool onSchedule=(pollPacket||f.poll_tx==reference)&&
       diff40(scheduled,pollPacket?f.poll_tx:f.probe_tx)<=511;
     const bool mixed=acceptRetry&&(retryPacket?r.have_probe_frame[probe]:retrySelected);
     if(retryPacket&&onSchedule&&!mixed&&!retrySelected){
      retrySelected=true;
      r.p1_retry=true;r.p1_first_errors=r.errors&P1ExchangeErrors;r.errors&=~P1ExchangeErrors;
      r.poll_frame=f;
      for(auto& rx:r.poll)rx=Rx{};
      for(auto& tx:r.tx)tx=Tx{};
     }
     auto& captured=repeat?repeat->frame:p1Retry?r.poll_frame:repeatPoll?r.p1_repeat_poll_frame:
                    pollPacket?r.poll2_frame:r.probe_frame[probe];
     auto& have=repeat?repeat->have_frame:p1Retry?r.p1_retry:repeatPoll?r.have_p1_repeat_poll_frame:
                pollPacket?r.have_poll2_frame:r.have_probe_frame[probe];
     if(!onSchedule||mixed||(have&&!sameFrame(captured,f))){
      r.metadata_conflict=true;r.errors|=ErrMetadata;
     }else{captured=f;have=true;}
     if(!mixed){
      if(!pollPacket){
       auto& sample=repeat?repeat->rx[i]:r.probe[probe][i];
       sample=o;probeGot[i]=true;
      }
      else if(p1Retry&&retrySelected)r.poll[i]=o;
      else if(repeatPoll)r.p1_repeat_poll_rx[i]=o;
      else if(!p1Retry)r.poll2_rx[i]=o;
     }
     got[i]=true;traces[i].accepted=!mixed&&onSchedule;
     if(finalSample&&!repeatPoll){
      auto& latch=repeat?repeat->deadline_latch:r.rx_deadline_latch;
      latch|=uint8_t(1U<<((secondPoll?2U:probe?4U:0U)+i));
     }
    }else{
     traces[i].fail(o.status,now,expected?12:11);
     if(!finalSample&&!p.arm(uint8_t(i))){r.errors|=ErrHardware;stopPair(p);return false;}
    }
   }else if(rc==-1&&!finalSample&&!p.arm(uint8_t(i))){r.errors|=ErrHardware;stopPair(p);return false;}
  }
  if(finalSample)break;
  p.relaxRx(stage6&&now+3000>=deadline);
 }
 // No receiver is restarted before both snapshots are copied. This also keeps
 // both radios out of RX while using the high-speed accumulator-read path.
 stopPair(p);
 const bool pollSlot=secondPoll||retrySelected||repeatPoll;
 if(!got[0]||!got[1])r.errors|=repeatPoll?ErrProbe1:pollSlot?(retrySelected&&!probe?ErrPoll:ErrPoll2):(probe?ErrProbe2:ErrProbe1);
 if(pollSlot)return got[0]||got[1];
 if(repeat&&(!repeat->have_frame||r.metadata_conflict||!probeGot[0]||!probeGot[1]||
             !repeat->rx[0].lde||!repeat->rx[1].lde))return false;
 // Alternate the SPI read order to expose order-dependent retention defects.
 for(unsigned j=0;j<2;++j){const unsigned i=j^(r.txn&1U);if(probeGot[i]){
  const unsigned slot=2*probe+i;
  const CirRead result=p.readCir(uint8_t(i),repeat?repeat->samples[i]:r.cir[slot],
                                 repeat?repeat->cir[i]:r.cir_meta[slot]);
  if(result==CirRead::HardwareFault){r.errors|=ErrHardware;return false;}
  if(result==CirRead::Invalid)r.errors|=ErrCir;
 }}
 return got[0]||got[1];
}
inline void commitProbeCapture(Record&r,unsigned probe,const ProbeRepeatCapture&capture){
 r.probe_frame[probe]=capture.frame;r.have_probe_frame[probe]=true;
 for(unsigned i=0;i<2;++i){
  const unsigned slot=2*probe+i;
  r.probe[probe][i]=capture.rx[i];r.cir_meta[slot]=capture.cir[i];
  memcpy(r.cir[slot],capture.samples[i],CIR_BYTES);
 }
 const unsigned latchMask=probe?0x30U:0x03U;
 r.rx_deadline_latch=uint8_t((r.rx_deadline_latch&~latchMask)|capture.deadline_latch);
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
inline void selectProbe1Repeat(Port&p,Record&r,uint8_t firstRadio,uint64_t originRx,ProbeRepeatCapture* repeat){
 r.p1_primary_rx_mask=probeReceiveMask(r,0);r.p1_primary_cir_mask=probeCirMask(r,0);
 r.p1_primary_frame=r.probe_frame[0];
 r.have_p1_primary_frame=r.have_probe_frame[0];
 if(r.p1_primary_rx_mask==3&&r.p1_primary_cir_mask==3)return;
 r.errors|=ErrProbe1;
 if(!repeat||(r.errors&(ErrHardware|ErrMetadata|ErrLate)))return;
 const auto& c=r.cfg;
 const uint64_t target=deadlineAt(p,firstRadio,originRx,c.probe1RepeatUs(),true);
 if(p.nowUs()+c.min_lead_us>=target){r.p1_repeat_late=true;return;}
 waitPair(p,r,0,target+c.grace_us,false,false,nullptr,Probe1Recovery::Poll);
 if(!r.have_p1_repeat_poll_frame||!r.p1_repeat_poll_rx[0].lde||!r.p1_repeat_poll_rx[1].lde||
    (r.errors&(ErrHardware|ErrMetadata)))return;
 const uint32_t firstErrors=r.errors;
 r.errors&=~(ErrProbe1|ErrCir);
 sendTagResponses(p,r,r.p1_repeat_poll_rx,r.p1_repeat_poll_frame.poll_tx,r.p1_repeat_response_tx,false);
 if(!p.healthy(0)||!p.healthy(1))r.errors|=ErrHardware;
 if(!(r.errors&ErrHardware)&&r.p1_repeat_response_tx[0].ok&&r.p1_repeat_response_tx[1].ok){
  const uint64_t final=deadlineAt(p,firstRadio,r.p1_repeat_poll_rx[firstRadio].hw,c.probe1DelayUs(),true);
  if(p.nowUs()+c.min_lead_us<final){
   repeat->reset();
   waitPair(p,r,0,final+c.grace_us,false,false,repeat,Probe1Recovery::Final);
   if(repeat->cir[0].valid&&repeat->cir[1].valid&&!(r.errors&(ErrHardware|ErrMetadata))){
    commitProbeCapture(r,0,*repeat);r.p1_repeat_used=true;
    return;
   }
  }else r.p1_repeat_late=true;
 }
 r.errors|=firstErrors;
}
inline void runTagProbe2Poll(Port&p,Record&r,uint8_t firstRadio,uint64_t originRx){
 const auto& c=r.cfg;
 const uint64_t target=deadlineAt(p,firstRadio,originRx,c.probe2PollDelayUs(r.p2_retry));
 if(p.nowUs()+c.min_lead_us>=target)r.errors|=ErrLate|ErrPoll2;
 else waitPair(p,r,0,target+c.grace_us,true);
 if(r.have_poll2_frame&&!(r.errors&(ErrHardware|ErrMetadata)))
  sendTagResponses(p,r,r.poll2_rx,r.poll2_frame.poll_tx,r.response2_tx,true);
 else r.errors|=ErrResponse2A|ErrResponse2B;
}
inline void runTagProbe2Final(Port&p,Record&r,uint8_t firstRadio,uint64_t originRx,ProbeRepeatCapture* capture=nullptr){
 const auto& c=r.cfg;
 const uint64_t target=deadlineAt(p,firstRadio,originRx,c.probe2_us(r.p2_retry),c.stage==6);
 if(p.nowUs()+c.min_lead_us>=target)r.errors|=ErrLate|ErrProbe2;
 else waitPair(p,r,1,target+c.grace_us,false,false,capture);
}
inline void selectFreshProbe2(Port&p,Record&r,uint8_t firstRadio,uint64_t originRx,
                              bool priorCirError,ProbeRepeatCapture* capture){
 r.p2_primary_rx_mask=probeReceiveMask(r,1);r.p2_primary_cir_mask=probeCirMask(r,1);
 if(r.p2_primary_rx_mask==3&&r.p2_primary_cir_mask==3&&!(r.errors&P2ExchangeErrors))return;
 r.errors|=ErrProbe2;
 if(!capture||(r.errors&(ErrHardware|ErrMetadata|ErrLate)))return;
 const uint32_t firstErrors=r.errors;
 const Frame firstPoll=r.poll2_frame;
 const bool firstPollValid=r.have_poll2_frame;
 const Rx firstRx[]={r.poll2_rx[0],r.poll2_rx[1]};
 const Tx firstTx[]={r.response2_tx[0],r.response2_tx[1]};
 const uint8_t firstLatch=r.rx_deadline_latch;
 r.p2_retry=true;r.p2_first_errors=firstErrors&(P2ExchangeErrors|ErrProbe2|ErrCir);
 r.errors&=~(P2ExchangeErrors|ErrProbe2);
 if(!priorCirError)r.errors&=~ErrCir;
 r.have_poll2_frame=false;r.rx_deadline_latch&=~uint8_t(0x0c);
 for(unsigned i=0;i<2;++i){r.poll2_rx[i]=Rx{};r.response2_tx[i]=Tx{};}
 runTagProbe2Poll(p,r,firstRadio,originRx);
 if(!(r.errors&(P2ExchangeErrors|ErrHardware|ErrMetadata|ErrLate))){
  capture->reset();
  runTagProbe2Final(p,r,firstRadio,originRx,capture);
  if(capture->cir[0].valid&&capture->cir[1].valid&&!(r.errors&(ErrHardware|ErrMetadata|ErrLate))){
   commitProbeCapture(r,1,*capture);
   return;
  }
 }
 // Retain valid primary observations if the fresh exchange failed.
 r.errors|=firstErrors;r.p2_retry=false;r.poll2_frame=firstPoll;r.have_poll2_frame=firstPollValid;
 r.rx_deadline_latch=firstLatch;
 for(unsigned i=0;i<2;++i){r.poll2_rx[i]=firstRx[i];r.response2_tx[i]=firstTx[i];}
}
inline void runTag(Port&p,const Config&c,uint32_t localBoot,uint8_t firstRadio,const Frame&first,
                   const Rx&firstRx,Record&r,ProbeRepeatCapture* repeat=nullptr){
 r.anchor=false;r.cfg=c;r.begin_us=p.nowUs();r.session=first.session;r.txn=first.txn;r.boot=localBoot;r.peer_boot=first.boot;r.config=configId(c);
 r.poll_frame=first;r.poll[firstRadio]=firstRx;
 r.p1_retry=isRetryPoll(first);r.initial_poll_tx=r.p1_retry?first.probe_tx:first.poll_tx;
 if(r.p1_retry)r.p1_first_errors=ErrPoll;
 uint64_t originRx=firstRx.hw;
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
 const uint64_t p1Deadline=deadlineAt(p,firstRadio,originRx,c.probe1DelayUs()+c.grace_us,c.stage==6);
 waitPair(p,r,0,p1Deadline,false,c.stage==6&&!r.p1_retry);
 if(r.p1_retry&&!isRetryPoll(first)&&!(r.errors&(ErrHardware|ErrMetadata))){
  firstRadio=r.poll[0].received?0:1;originRx=r.poll[firstRadio].hw;
  sendTagResponses(p,r,r.poll,r.poll_frame.poll_tx,r.tx,false);
  if(!p.healthy(0)||!p.healthy(1)){r.errors|=ErrHardware;stopPair(p);r.end_us=p.nowUs();return;}
  waitPair(p,r,0,deadlineAt(p,firstRadio,originRx,c.probe1DelayUs()+c.grace_us,true));
 }
 if(c.stage==6){
  selectProbe1Repeat(p,r,firstRadio,originRx,repeat);
  if(r.p1_primary_rx_mask==3&&r.p1_primary_cir_mask==3&&!(r.errors&(ErrHardware|ErrMetadata))){
   // When primary P1 is complete, leave both receivers idle through the
   // optional P1 recovery POLL before arming for the distinct POLL2 frame.
   const uint64_t repeatEnd=deadlineAt(p,firstRadio,originRx,c.probe1RepeatUs()+c.grace_us,true);
   while(p.nowUs()<repeatEnd)p.relax();
  }
 }
 const bool priorCirError=(r.errors&ErrCir)!=0;
 if(c.rangesProbe2()&&!(r.errors&(ErrHardware|ErrMetadata)))
  runTagProbe2Poll(p,r,firstRadio,originRx);
 if(!(r.errors&ErrHardware))
  runTagProbe2Final(p,r,firstRadio,originRx);
 if(c.stage==6)selectFreshProbe2(p,r,firstRadio,originRx,priorCirError,repeat);
 stopPair(p);
 if(r.have_probe_frame[0])r.response_mask=r.probe_frame[0].resp_mask;
 if((r.p1_repeat_used?r.have_p1_primary_frame:r.have_probe_frame[0])&&r.have_probe_frame[1]&&
    !coherentAnchorBlock(r.p1_repeat_used?r.p1_primary_frame:r.probe_frame[0],r.probe_frame[1])){r.metadata_conflict=true;r.errors|=ErrMetadata;}
 finalizeRanges(r);
 if(c.stage>=4&&(!r.range[0].valid||!r.range[1].valid))r.errors|=ErrRange;
 r.end_us=p.nowUs();
}
inline void receiveAnchorResponses(Port&p,Record&r,uint64_t origin,Rx*responses,uint8_t&mask,bool second,bool repeat=false){
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
      if(finalSample&&!repeat)r.rx_deadline_latch|=uint8_t(1U<<(i+(second?2U:0U)));
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
inline void pollAnchorProbe2(Port&p,Record&r,const Frame&poll,bool retry){
 const uint32_t offset=r.cfg.probe2PollDelayUs(retry);
 const uint64_t prepare=deadlineAt(p,0,r.tx[0].actual,offset-3500);
 while(p.nowUs()<prepare)p.relax();
 Frame q=poll;q.type=Type::Poll2;q.peer_boot=r.peer_boot;q.tx_ok_mask=0;q.probe_tx=0;
 const uint64_t target=(r.tx[0].actual+usTicks(offset))&MASK40;
 r.poll2_tx=Tx{};r.have_poll2_frame=false;r.response2_mask=0;
 for(auto& rx:r.response2_rx)rx=Rx{};
 if(q.peer_boot&&p.send(0,q,target,r.poll2_tx)){
  r.poll2_frame=q;r.have_poll2_frame=true;
  receiveAnchorResponses(p,r,r.poll2_tx.actual,r.response2_rx,r.response2_mask,true);
 }else r.errors|=P2ExchangeErrors;
 if(r.poll2_tx.hardware_fault)r.errors|=ErrHardware;
}
inline void sendAnchorProbe(Port&p,Record&r,Frame&frame,unsigned probe,uint32_t offset){
 const uint64_t at=(r.tx[0].actual+usTicks(offset))&MASK40;
 Tx& tx=r.tx[probe+1];
 if(!p.send(0,frame,at,tx))r.errors|=probe?ErrProbe2:ErrProbe1;
 r.probe_frame[probe]=frame;r.have_probe_frame[probe]=tx.ok;
 if(tx.hardware_fault)r.errors|=ErrHardware;
}
inline void recoverAnchorProbe1(Port&p,Record&r,const Frame&poll,const Frame&primary){
 const auto& c=r.cfg;
 const uint32_t offset=c.probe1RepeatUs();
 const uint64_t canonical=r.tx[0].actual;
 const uint64_t prepare=deadlineAt(p,0,canonical,offset-3500);
 while(p.nowUs()<prepare)p.relax();
 Frame retry=poll;
 retry.peer_boot=r.peer_boot;
 retry.tx_ok_mask=2;
 retry.probe_tx=canonical;
 const uint64_t target=(canonical+usTicks(offset))&MASK40;
 if(!p.send(0,retry,target,r.p1_repeat_poll_tx)){
  if(r.p1_repeat_poll_tx.hardware_fault)r.errors|=ErrHardware;
  return;
 }
 r.p1_repeat_poll_frame=retry;
 r.have_p1_repeat_poll_frame=true;
 const uint64_t origin=r.p1_repeat_poll_tx.actual;
 const uint32_t firstErrors=r.errors;
 uint8_t mask=0;
 receiveAnchorResponses(p,r,origin,r.p1_repeat_responses,mask,false,true);
 // Silence is expected if the tag captured both primary observations.
 r.errors=firstErrors|(r.errors&ErrHardware);
 if(mask!=3||(r.errors&ErrHardware))return;
 Frame final=primary;
 final.poll_tx=origin;
 final.resp_mask=mask;
 for(unsigned i=0;i<2;++i){
  final.resp_rx[i]=r.p1_repeat_responses[i].hw;
  final.ci[i]=r.p1_repeat_responses[i].ci;
 }
 const uint64_t finalAt=(origin+usTicks(c.probe1DelayUs()))&MASK40;
 p.send(0,final,finalAt,r.p1_repeat_tx);
 r.p1_repeat_frame=final;
 if(r.p1_repeat_tx.hardware_fault)r.errors|=ErrHardware;
}
inline void recoverAnchorProbe2(Port&p,Record&r,const Frame&poll){
 const uint32_t firstErrors=r.errors;
 const Tx firstPoll=r.poll2_tx;
 const Frame firstPollFrame=r.poll2_frame;
 const bool firstPollValid=r.have_poll2_frame;
 const uint8_t firstMask=r.response2_mask,firstLatch=r.rx_deadline_latch;
 const Rx firstResponses[]={r.response2_rx[0],r.response2_rx[1]};

 // A tag that already has the primary pair does not answer the reserved POLL2.
 r.p2_retry=true;
 r.p2_first_errors=firstErrors&(P2ExchangeErrors|ErrProbe2);
 r.errors&=~(P2ExchangeErrors|ErrProbe2);
 r.rx_deadline_latch&=~uint8_t(0x0c);
 pollAnchorProbe2(p,r,poll,true);
 if(r.response2_mask==3&&!(r.errors&ErrHardware)){
  r.p2_primary_tx=r.tx[2];r.p2_primary_frame=r.probe_frame[1];
  Frame final=r.probe_frame[0];
  final.type=Type::Probe2;final.peer_boot=r.peer_boot;final.prev_probe_actual=r.tx[1].actual;
  final.tx_ok_mask=1|(r.tx[1].ok?2:0);
  final.tag_poll_rx=r.p1_repeat_tx.ok?r.p1_repeat_tx.actual:0;
  final.tag_resp_tx=r.poll2_tx.actual;
  sendAnchorProbe(p,r,final,1,r.cfg.probe2_us(true));
  return;
 }
 // No response is expected on primary success. Preserve hardware protection.
 r.errors=firstErrors|(r.errors&ErrHardware);r.p2_retry=false;r.p2_first_errors=0;
 r.poll2_tx=firstPoll;r.poll2_frame=firstPollFrame;r.have_poll2_frame=firstPollValid;
 r.response2_mask=firstMask;r.rx_deadline_latch=firstLatch;
 for(unsigned i=0;i<2;++i)r.response2_rx[i]=firstResponses[i];
 if(!r.tx[2].ok)r.errors|=ErrProbe2;
}
inline void runAnchor(Port&p,const Config&c,uint32_t session,uint32_t txn,uint32_t localBoot,uint32_t expectedTagBoot,Record&r){
 r.anchor=true;r.cfg=c;r.begin_us=p.nowUs();r.session=session;r.txn=txn;r.boot=localBoot;r.config=configId(c);r.peer_boot=expectedTagBoot;
 Frame f;f.type=Type::Poll;f.session=session;f.txn=txn;f.config=r.config;f.boot=localBoot;f.stage=c.stage;
 const uint64_t requested=(p.system(0)+usTicks(3000))&MASK40;
 if(!p.send(0,f,requested,r.tx[0])){
  r.errors|=ErrPoll;
  if(r.tx[0].hardware_fault)r.errors|=ErrHardware;
  r.end_us=p.nowUs();return;
 }
 r.poll_frame=f;r.initial_poll_tx=r.tx[0].actual;
 if(c.stage>=3)receiveAnchorResponses(p,r,r.tx[0].actual,r.responses,r.response_mask,false);
 if(c.stage==6&&r.response_mask!=3&&!(r.errors&ErrHardware)){
  // Use the first P1 slot for a new POLL when either first RESPONSE is missing.
  r.p1_retry=true;r.p1_first_errors=r.errors&P1ExchangeErrors;r.errors&=~P1ExchangeErrors;
  r.tx[0]=Tx{};r.response_mask=0;
  for(auto& rx:r.responses)rx=Rx{};
  f.tx_ok_mask=1;f.probe_tx=r.initial_poll_tx;
  const uint64_t retryAt=(r.initial_poll_tx+usTicks(c.probe1DelayUs()))&MASK40;
  if(!p.send(0,f,retryAt,r.tx[0])){
   r.errors|=ErrPoll;
   if(r.tx[0].hardware_fault)r.errors|=ErrHardware;
   r.end_us=p.nowUs();return;
  }
  r.poll_frame=f;
  receiveAnchorResponses(p,r,r.tx[0].actual,r.responses,r.response_mask,false);
 }
 if(r.errors&ErrHardware){r.end_us=p.nowUs();return;}
 for(unsigned k=0;k<2;++k){
  if(k&&c.rangesProbe2()){
   pollAnchorProbe2(p,r,f,false);
   if(r.errors&ErrHardware)break;
   // Withhold primary P2 to tell the tag that the RESPONSE2 pair was incomplete.
   if(c.stage==6&&r.response2_mask!=3)break;
  }
  Frame q;q.type=k?Type::Probe2:Type::Probe1;q.session=session;q.txn=txn;q.config=r.config;q.boot=localBoot;q.peer_boot=r.peer_boot;q.stage=c.stage;
  q.poll_tx=r.tx[0].actual;q.resp_mask=r.response_mask;q.tx_ok_mask=1;
  for(unsigned i=0;i<2;++i){q.resp_rx[i]=r.responses[i].hw;q.ci[i]=r.responses[i].ci;}
  if(k){
   q.prev_probe_actual=r.tx[1].actual;
   if(r.tx[1].ok)q.tx_ok_mask|=2;
   // In the Stage 6 Probe2 contract this slot confirms the repeat's actual TX.
   if(c.stage==6&&r.p1_repeat_tx.ok)q.tag_poll_rx=r.p1_repeat_tx.actual;
  }
  sendAnchorProbe(p,r,q,k,k?c.probe2_us():c.probe1DelayUs());
  if(r.errors&ErrHardware)break;
  if(!k&&c.stage==6&&r.tx[1].ok&&r.peer_boot)
   recoverAnchorProbe1(p,r,f,q);
  if(r.errors&ErrHardware)break;
 }
 if(c.stage==6&&r.peer_boot&&!(r.errors&ErrHardware))
  recoverAnchorProbe2(p,r,f);
 r.p1_tx_confirmed=r.tx[1].ok;r.complete=r.errors==0&&r.tx[0].ok&&r.tx[1].ok&&r.tx[2].ok&&(c.stage==2||r.response_mask==3);
 p.idle(0);r.end_us=p.nowUs();
}
} // namespace dual8
