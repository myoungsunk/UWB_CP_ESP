#include "engine.hpp"
#include "record_json.hpp"
#include "rx_guard_policy.hpp"

static_assert(!rxguard::startupClockQualified(0x02800002,0x03,0xfc),
              "observed A clock fault and invalid detector configuration");
static_assert(!rxguard::startupClockQualified(0x02800002,0x03,0x04) &&
              !rxguard::startupClockQualified(0x02800002,0x0b,0x04),
              "an asserted CPLLLOW blocks operation before and after RX");
static_assert(rxguard::startupClockQualified(0x02800002,0x01,0x04),
              "the latched event can be acknowledged only after lock is healthy");
static_assert(!rxguard::startupClockSettled(0x02800002,0x01,0x04),
              "a persistent or newly raised clock-loss event blocks operation");
static_assert(rxguard::startupClockSettled(0x00800002,0x01,0x04) &&
              rxguard::startupClockSettled(0x00800002,0x09,0x04),
              "observed B status is valid before and after enabling RX");
static_assert(rxguard::startupClockSettled(0x00800002,0x01,0xdead0004) &&
              !rxguard::startupClockSettled(0x00800002,0x01,0xdead00fc),
              "ignore reserved EC_CTRL readback while enforcing all documented bits");
static_assert(!rxguard::startupClockQualified(0x01800002,0x01,0x04) &&
              !rxguard::startupClockQualified(rxguard::MASK40,0x01,0x04) &&
              !rxguard::startupClockQualified(0x00800002,0xff,0x04) &&
              !rxguard::startupClockQualified(0x00800002,0x01,0xff),
              "RF loss and unreadable registers cannot qualify");
#include <iostream>
#include <fstream>
#include <vector>
#include <random>
#include <stdexcept>
#include <algorithm>
using namespace dual8;
static unsigned checks=0;
#define CHECK(x) do{++checks;if(!(x))throw std::runtime_error(std::string(__FILE__)+":"+std::to_string(__LINE__)+" " #x);}while(0)
struct Clock {long double rate=1;uint64_t base=0;uint64_t at(long double ticks)const{return (base+uint64_t(llroundl(ticks*rate)))&MASK40;}};
struct SimTag:Port {
 Config cfg;Clock ac,tc[2];long double t=0,t0=usTicks(10000),tau[2]={1066,1090};
 bool armed[2]{},used[2][8]{},drop[2][8]{},retryProbe=false,retryFirst=false,badP1=false,conflict=false,failTx[2]{},corruptCir=false,hwFault=false;
 long double armT[2]{};uint64_t respAnchor[2]{};unsigned last[2]{},calls[2]{},arms[2]{};
 uint8_t respMask=0;uint32_t cirCost=3600,p2CirCost=0,txn=0,holdResponseASetupUntilUs=0,holdResponse2ASetupUntilUs=0;
 unsigned probeReadsDuringFrame=0,probe2ReadsInsideProtection=0;
 uint64_t injectProbe2StatusA=0;
 bool repeatMetadataConflict=false;
 RxWindowTrace* windowTrace=nullptr;
 bool injectProbeWakeDelay=false, injected=false;
 int armFaultRadio=-1,cirFaultRadio=-1;unsigned cirFaultProbe=1;
 SimTag(){ac.base=MASK40-usTicks(10000);ac.rate=1.000002;tc[0].base=MASK40-usTicks(12000);tc[0].rate=1.000011;tc[1].base=813439167;tc[1].rate=0.999983;}
 uint64_t nowUs()override{return uint64_t(t/HZ*1e6L);}
 uint64_t system(uint8_t i)override{return tc[i].at(t);}
 bool healthy(uint8_t)override{return !hwFault;}
 bool arm(uint8_t i)override{
  ++arms[i];
  if(hwFault||(i==armFaultRadio&&arms[i]==2))return false;
  armed[i]=true;armT[i]=t;return true;
 }
 void idle(uint8_t i)override{armed[i]=false;}
 void traceRx(RxWindowTrace* trace)override{windowTrace=trace;}
 long double delayedGlobal(long double start,uint32_t offset){
  const uint64_t base=ac.at(start),req=(base+usTicks(offset))&MASK40;
  return start+static_cast<long double>(signed40(plannedStamp(req,16436),base))/ac.rate;
 }
 long double firstGlobal(){return retryFirst?delayedGlobal(t0,cfg.probe1DelayUs()):t0;}
 long double probeGlobal(unsigned k){
  if(k==6)return firstGlobal();
  if(k==7)return delayedGlobal(firstGlobal(),cfg.probe2RepeatUs(retryProbe));
  const uint32_t offset=(k==3||k==4)?cfg.probe2PollDelayUs(k==4):(k==1?cfg.probe1DelayUs():cfg.probe2_us(k==5));
  return delayedGlobal(firstGlobal(),offset);
 }
 long double eventAt(unsigned k,unsigned i){return (k?probeGlobal(k):t0)+tau[i];}
 Frame frame(unsigned k,unsigned i){Frame f;f.session=77;f.txn=txn;f.config=configId(cfg);f.boot=101;f.stage=cfg.stage;f.poll_tx=ac.at(k?firstGlobal():t0);
  if(k==6){f.tx_ok_mask=1;f.probe_tx=ac.at(t0);}
  else if(k==3||k==4){f.type=Type::Poll2;f.peer_boot=202;f.poll_tx=ac.at(probeGlobal(k));}
  else if(k){f.type=k==1?Type::Probe1:Type::Probe2;f.peer_boot=respMask?202:0;f.resp_mask=respMask;f.resp_rx[0]=respAnchor[0];f.resp_rx[1]=respAnchor[1];f.tx_ok_mask=1;
   f.probe_tx=ac.at(probeGlobal(k));
   if(k==2||k==5||k==7){f.prev_probe_actual=ac.at(probeGlobal(1));if(!badP1)f.tx_ok_mask|=2;else ++f.prev_probe_actual;}
   if(conflict&&i==1&&k==1)++f.resp_rx[0];
   if(repeatMetadataConflict&&k==7)++f.ci[0];
  }return f;
 }
 int receive(uint8_t i,Frame&f,Rx&o)override{
  if(hwFault){return -2;}
  if(!armed[i])return 0;
  for(unsigned k:{0U,6U,1U,3U,2U,4U,5U,7U}){
   if((k==3&&!cfg.rangesProbe2())||(k==2&&retryProbe)||(k>=4&&k<=5&&!retryProbe)||
      (k==6&&!retryFirst)||(k==7&&cfg.stage!=6)||used[i][k])continue;
   const long double at=eventAt(k,i);
   if(k&&t>=at-usTicks(350)&&t<at+usTicks(1050))++probeReadsDuringFrame;
   if((k==2||k==5||k==7)&&t>=at-usTicks(900)&&t<at+usTicks(1800))++probe2ReadsInsideProtection;
   if(t<at+usTicks(1200))return 0;
   used[i][k]=true;if(drop[i][k]||armT[i]>at-usTicks(250))continue;
   if(injectProbe2StatusA&&(k==2||k==5)&&i==0){
    const uint64_t status=injectProbe2StatusA;
    CHECK(rxguard::classify(status)==rxguard::Action::Failed);
    CHECK(windowTrace);
    windowTrace[i].fail(status,nowUs(),uint8_t(rxguard::Action::Failed));
    t+=usTicks(527);
    windowTrace[i].recovery_us=nowUs();
    armed[i]=false;
    return -1;
   }
   f=frame(k,i);o=Rx{};o.received=true;o.lde=true;o.diag=true;o.radio=i;o.type=uint8_t(f.type);o.hw=tc[i].at(at);o.raw=(o.hw+100)&MASK40;
   o.esp_us=nowUs();o.arm_us=uint64_t(armT[i]/HZ*1e6L);o.status=(1ULL<<10)|(1ULL<<13)|(1ULL<<14);o.bytes=AIR_BYTES;o.fp_raw=uint16_t(740*64+i*5);o.nacc=240;o.pwr=12000;o.f1=120;o.f2=130;o.f3=140;o.noise=3;o.ci=i?44:-20;CHECK(encode(f,o.air));
   last[i]=k;armed[i]=false;return 1;
  }return 0;
 }
 bool send(uint8_t i,Frame&f,uint64_t req,Tx&o)override{
  o=Tx{};o.attempted=true;o.radio=i;o.type=uint8_t(f.type);o.requested=req;o.planned=plannedStamp(req,16436);o.begin_us=nowUs();
  if(i==0&&holdResponseASetupUntilUs)t=std::max(t,eventAt(0,i)+usTicks(holdResponseASetupUntilUs));
  const bool second=f.type==responseType(true,i);
  if(second&&i==0&&holdResponse2ASetupUntilUs)t=std::max(t,eventAt(3,i)+usTicks(holdResponse2ASetupUntilUs));
  const int64_t lead=signed40(o.planned,system(i));if(lead<int64_t(usTicks(600)))return false;
  const long double sendAt=t+static_cast<long double>(lead)/tc[i].rate;
  t=sendAt+usTicks(1200);o.actual=o.planned+(failTx[i]?1:0);o.ok=second||!failTx[i];if(second)o.actual=o.planned;o.status=o.ok?0x80:0;o.end_us=nowUs();f.tag_resp_tx=o.planned;
  if(!second){respAnchor[i]=ac.at(sendAt+tau[i]);respMask|=uint8_t(1U<<i);}return o.ok;
 }
 CirRead readCir(uint8_t i,uint8_t*d,Cir&o)override{
  CHECK(!armed[0]&&!armed[1]);CHECK(last[i]==1||last[i]==2||last[i]==5||last[i]==7);
  const uint32_t cost=p2CirCost&&last[i]!=1?p2CirCost:cirCost;
  ++calls[i];o.present=true;o.bytes=CIR_BYTES;o.read_us=cost/2;o.repeat_us=cost/2;
  const int repeatOffset=last[i]==7?13:0;
  for(size_t k=0;k<NSAMP;++k){putLE(d+4*k,uint16_t(int16_t((int(k)+int(i)*3)%50-25+repeatOffset)),2);putLE(d+4*k+2,uint16_t(int16_t(20-int(k)%31)),2);}
  o.crc=crc32(d,CIR_BYTES);t+=usTicks(cost);
  if(i==cirFaultRadio&&last[i]==cirFaultProbe){hwFault=true;return CirRead::HardwareFault;}
  o.repeat_equal=!corruptCir;o.valid=!corruptCir;
  return o.valid?CirRead::Valid:CirRead::Invalid;
 }
 void relax()override{
  if(injectProbeWakeDelay&&!injected&&t>=probeGlobal(1)+usTicks(1330)&&t<probeGlobal(1)+usTicks(1400)){
   t+=usTicks(1000);injected=true;return;
  }
  t+=usTicks(25);
 }
 Record execute(bool useRepeat=false){Record r;arm(0);arm(1);Frame f;Rx o;uint8_t first=0;
  bool found=false;for(unsigned n=0;n<2000&&!found;++n){for(unsigned i=0;i<2&&!found;++i)if(receive(i,f,o)==1){first=i;found=true;}if(!found)relax();}CHECK(found);
  Probe2RepeatCapture repeat;
  runTag(*this,cfg,202,first,f,o,r,useRepeat?&repeat:nullptr);return r;
 }
};
struct SimAnchor:Port {
 Config cfg;uint64_t t=0,poll=0,poll2=0;bool armed=false,seen[2][2]{},lost[2]{},lost2[2]{},badBoot=false,txfail=false;uint32_t peer=202;std::vector<Frame> txframes;
 unsigned readsDuringResponse=0,pollAttempts=0,poll2Attempts=0;bool loseRetry=false;
 uint64_t nowUs()override{return t;}
 uint64_t system(uint8_t)override{return (1000000+usTicks(t))&MASK40;}
 bool healthy(uint8_t)override{return true;}
 bool arm(uint8_t)override{armed=true;return true;}
 void idle(uint8_t)override{armed=false;}
 int receive(uint8_t,Frame&f,Rx&o)override{
  if(!armed)return 0;
  const bool second=poll2!=0;
  const uint64_t origin=second?poll2:poll;
  for(unsigned i=0;i<2;++i){if(seen[second][i])continue;
   uint64_t at=(origin+usTicks(cfg.responseDelayUs(i))+2132)&MASK40;
   const int64_t relative=signed40(system(0),at);
   if(relative>=-int64_t(usTicks(350))&&relative<int64_t(usTicks(1050)))++readsDuringResponse;
   if(relative<int64_t(usTicks(1200))){return 0;}
   seen[second][i]=true;if(second?(lost2[i]&&(poll2Attempts==1||loseRetry)):(lost[i]&&pollAttempts==1))continue;
   f=Frame{};f.type=responseType(second,i);f.radio=i;f.session=77;f.txn=0;f.config=configId(cfg);f.boot=badBoot&&i?999:peer;f.peer_boot=101;f.stage=cfg.stage;f.poll_tx=origin;
   o=Rx{};o.received=o.lde=true;o.hw=at;o.radio=0;o.type=uint8_t(f.type);o.esp_us=t;o.bytes=AIR_BYTES;CHECK(encode(f,o.air));armed=false;return 1;
  }return 0;
 }
 bool send(uint8_t,Frame&f,uint64_t requested,Tx&o)override{
  o=Tx{};o.attempted=true;o.planned=plannedStamp(requested,16436);o.requested=requested;o.actual=o.planned;o.ok=!(txfail&&f.type==Type::Probe1);
  t+=uint64_t(double(signed40(o.planned,system(0)))*1e6/HZ)+1200;
  if(f.type==Type::Poll){++pollAttempts;seen[0][0]=seen[0][1]=false;poll=o.planned;f.poll_tx=poll;}else if(f.type==Type::Poll2){++poll2Attempts;seen[1][0]=seen[1][1]=false;poll2=o.planned;f.poll_tx=poll2;return o.ok;}else f.probe_tx=o.planned;txframes.push_back(f);return o.ok;
 }
 CirRead readCir(uint8_t,uint8_t*,Cir&)override{return CirRead::Invalid;}
 void relax()override{t+=25;}
 Record execute(){Record r;runAnchor(*this,cfg,77,0,101,0,r);return r;}
};
static void testsCore(){
 {const uint8_t original[]={0x80,0xff,0x81,0xff,0xff,0xff,0x00,0x00,0x7f,0x00,0x80,0x00,0x00,0x00};
  const uint8_t expected[]={0x80,0x80,0xff,0x81,0xff,0x00,0x7f,0x80,0x80,0x00,0x00};
  uint8_t packed[sizeof original]{};
  CHECK(packUartIq(original,sizeof original,packed)==sizeof expected);
  CHECK(memcmp(packed,expected,sizeof expected)==0);
  CHECK(packUartIq(original,2,packed)==0);
 }
 Config c;CHECK(c.stage==5&&c.responseDelayUs(0)==10000&&c.responseDelayUs(1)==20000);CHECK(validate(c)==nullptr);CHECK(configId(c)==459867368UL);Config bad=c;bad.period_us=200000;CHECK(validate(bad)!=nullptr);bad.baud=2000000;CHECK(validate(bad)==nullptr);bad.period_us=100000;CHECK(validate(bad)!=nullptr);
 Config stage3=c;stage3.stage=3;
 CHECK(validate(stage3)==nullptr&&configId(stage3)!=2523421921UL);
 CHECK(stage3.responseDelayUs(0)==10000&&stage3.responseDelayUs(1)==20000);
 CHECK(stage3.probe1DelayUs()==28000&&stage3.probe2_us()==60000);
 Config stage4=stage3;stage4.stage=4;
 CHECK(validate(stage4)==nullptr&&configId(stage4)!=configId(stage3));
 CHECK(stage4.responseDelayUs(0)==10000&&stage4.responseDelayUs(1)==20000);
 CHECK(stage4.probe1DelayUs()==28000&&stage4.probe2_us()==60000);
 Frame f;f.session=77;f.boot=101;f.config=configId(c);f.poll_tx=MASK40;uint8_t wire[AIR_BYTES];CHECK(encode(f,wire));Frame g;CHECK(decode(wire,AIR_BYTES,g));CHECK(sameFrame(f,g));
 for(size_t n=0;n<AIR_BYTES;++n)CHECK(!decode(wire,n,g));
 for(unsigned bit=0;bit<AIR_BYTES*8;++bit){uint8_t b[AIR_BYTES];memcpy(b,wire,AIR_BYTES);b[bit/8]^=uint8_t(1U<<(bit%8));CHECK(!decode(b,AIR_BYTES,g));}
 f.type=Type::RespA;f.radio=1;f.peer_boot=101;CHECK(!encode(f,wire));f.radio=0;CHECK(encode(f,wire));
 CHECK(diff40(5,MASK40-3)==9);CHECK(signed40(MASK40-3,5)==-9);
 for(unsigned i=0;i<4096;++i){uint64_t req=(MASK40-2000+i)&MASK40;uint64_t raw=plannedRaw(req,16436);CHECK((raw&511)==0);CHECK(diff40(req,plannedStamp(req,16436))<=511);}
 CHECK(fabs(relDelayS(740,740*64))<1e-18);CHECK(fabs(physicalDelayS(740,740*64,3)*C_AIR-3)<1e-12);
 CHECK(isnan(rxPower(100,0)));CHECK(isfinite(rxPower(12000,240)));CHECK(isnan(fpPower(0,0,0,240)));
 std::mt19937_64 gen(20261111);
 for(unsigned n=0;n<1000;++n){
  double da=double(int(gen()%80001)-40000)*1e-9,dt=double(int(gen()%80001)-40000)*1e-9;Clock a,t;a.base=gen()&MASK40;t.base=gen()&MASK40;a.rate=1+da;t.rate=1+dt;
  long double tau=1000+gen()%20000,pg=usTicks(10000),rg=pg+tau+usTicks(7000),fg=pg+usTicks(19000);
  DsInput x;x.a_poll_tx=a.at(pg);x.t_poll_rx=t.at(pg+tau);x.t_resp_tx=t.at(rg);x.a_resp_rx=a.at(rg+tau);x.a_probe_tx=a.at(fg);x.t_probe_rx=t.at(fg+tau);
  auto d=dsTwr(x);CHECK(d.valid);CHECK(fabs(d.range_m-double(tau)*DTU*C_AIR)<0.01);
 }
 DsInput invalid;CHECK(!dsTwr(invalid).valid);
}
static void testsEngine(const std::string&out){
 {SimTag q;q.injectProbeWakeDelay=true;auto x=q.execute();
  CHECK(q.injected&&(x.errors&ErrProbe1)&&!x.probe[0][0].received&&!x.probe[0][1].received);}
 {SimTag q;q.cfg.stage=6;q.injectProbeWakeDelay=true;auto x=q.execute();
  CHECK(q.injected&&x.complete&&x.probe[0][0].received&&x.probe[0][1].received);
  CHECK((x.rx_deadline_latch&3)==3&&q.probeReadsDuringFrame==0);}

 SimTag s;Record r=s.execute();CHECK(r.complete);CHECK(r.p1_tx_confirmed);
 for(unsigned i=0;i<2;++i){
  CHECK(r.response2_tx[i].ok&&r.response2_tx[i].type==uint8_t(responseType(true,i)));
  CHECK(r.response2_tx[i].requested==((r.poll2_rx[i].hw+usTicks(r.cfg.responseDelayUs(i)))&MASK40));
 }CHECK(r.range[0].valid&&r.range[1].valid);CHECK(s.calls[0]==2&&s.calls[1]==2);
 CHECK(fabs(r.range[0].range_m-1066*DTU*C_AIR)<0.01);CHECK(fabs(r.range[1].range_m-1090*DTU*C_AIR)<0.01);
 char json[JSON_MAX+1];size_t n=0;CHECK(serializeRecord(r,json,sizeof json,n));CHECK(n<=JSON_MAX);
 std::ofstream(out+"/synthetic_tag_record.json")<<json<<"\n";std::ofstream raw(out+"/SYNTHETIC_CIR_i16.raw",std::ios::binary);raw.write(reinterpret_cast<const char*>(r.cir),sizeof r.cir);
 std::cout<<"tag_json_bytes="<<n<<" sizeof_Record="<<sizeof(Record)<<"\n";
 {SimTag q;q.drop[1][0]=true;auto x=q.execute();CHECK(x.range[0].valid);CHECK(!x.range[1].valid);CHECK(x.cir_meta[1].valid&&x.cir_meta[3].valid);}
 {SimTag q;q.drop[0][0]=true;auto x=q.execute();CHECK(!x.range[0].valid);CHECK(x.range[1].valid);}
 {SimTag q;q.drop[1][1]=true;auto x=q.execute();CHECK(x.range[0].valid);CHECK(!x.range[1].valid);CHECK(!x.cir_meta[1].present);CHECK(x.cir_meta[3].valid);}
 {SimTag q;q.drop[0][2]=true;auto x=q.execute();CHECK(x.range[0].valid&&x.range[1].valid);CHECK(!x.complete);}
 {SimTag q;q.drop[0][2]=q.drop[1][2]=true;auto x=q.execute();CHECK(!x.range[0].valid&&!x.range[1].valid);CHECK(x.cir_meta[0].valid);}
 {SimTag q;q.badP1=true;auto x=q.execute();CHECK(!x.range[0].valid&&!x.p1_tx_confirmed);}
 {SimTag q;q.conflict=true;auto x=q.execute();CHECK(x.metadata_conflict);CHECK(!x.range[0].valid&&!x.range[1].valid);}
 {SimTag q;q.failTx[0]=true;auto x=q.execute();CHECK(!x.range[0].valid);CHECK(x.range[1].valid);}
 {SimTag q;q.corruptCir=true;auto x=q.execute();CHECK(!x.complete);CHECK(x.range[0].valid);CHECK(x.errors==ErrCir);CHECK(q.calls[0]==2&&q.calls[1]==2);}
 for(unsigned first=0;first<2;++first){
  for(unsigned probe=1;probe<=2;++probe){
   SimTag q;q.txn=first;q.cirFaultRadio=int(first);q.cirFaultProbe=probe;
   auto x=q.execute();
   CHECK(q.hwFault&&(x.errors&ErrHardware)&&!(x.errors&ErrCir));
   CHECK(!x.complete&&!x.range[0].valid&&!x.range[1].valid);
   CHECK(q.calls[first]==probe&&q.calls[first^1U]==probe-1);
   CHECK(q.arms[0]==2*probe&&q.arms[1]==2*probe&&!q.armed[0]&&!q.armed[1]);
   const unsigned slot=(probe-1)*2+first;
   CHECK(x.cir_meta[slot].present&&!x.cir_meta[slot].valid);
   CHECK(x.cir_meta[slot].crc==crc32(x.cir[slot],CIR_BYTES));
  }
 }
 {SimTag q;q.armFaultRadio=0;auto x=q.execute();CHECK(x.errors&ErrHardware);
  CHECK(q.arms[0]==2&&q.arms[1]==1&&!q.armed[0]&&!q.armed[1]);}
 {SimTag q;q.cirCost=65000;auto x=q.execute();CHECK(x.errors&ErrLate);CHECK(!x.range[0].valid);}
 {SimTag q;auto x=q.execute();x.errors|=ErrHardware;finalizeRanges(x);CHECK(!x.p1_tx_confirmed&&!x.range[0].valid&&!x.range[1].valid&&!x.complete);}
 {SimTag q;auto x=q.execute();x.errors|=ErrLate;finalizeRanges(x);CHECK(!x.p1_tx_confirmed&&!x.range[0].valid&&!x.complete);}
 {SimTag q;q.cfg.stage=2;auto x=q.execute();CHECK(x.complete);CHECK(!x.tx[0].attempted&&!x.tx[1].attempted);CHECK(!x.range[0].valid);CHECK(q.probeReadsDuringFrame==0);}
 {SimTag q;q.cfg.stage=3;q.holdResponseASetupUntilUs=6463;auto x=q.execute();
  CHECK(x.complete&&x.tx[0].ok&&x.tx[1].ok);CHECK(!x.range[0].valid);CHECK(q.probeReadsDuringFrame==0);}
 {SimTag q;q.cfg.stage=4;q.holdResponseASetupUntilUs=6463;auto x=q.execute();
  CHECK(x.complete&&x.p1_tx_confirmed&&x.tx[0].ok&&x.tx[1].ok);
  CHECK(fabs(x.range[0].range_m-1066*DTU*C_AIR)<0.01);
  CHECK(fabs(x.range[1].range_m-1090*DTU*C_AIR)<0.01);CHECK(q.probeReadsDuringFrame==0);}
 {SimTag q;q.holdResponseASetupUntilUs=6463;auto x=q.execute();CHECK(x.complete&&x.p1_tx_confirmed&&x.range[0].valid&&x.range[1].valid);}
 {SimTag q;q.cfg.stage=6;q.cfg.baud=2000000;q.cfg.period_us=150000;CHECK(validate(q.cfg)!=nullptr);q.cfg.period_us=200000;
  q.holdResponseASetupUntilUs=6463;q.holdResponse2ASetupUntilUs=6463;auto x=q.execute();
  CHECK(!validate(q.cfg)&&q.cfg.probe2PollDelayUs()==86000&&
        q.cfg.probe2_us()==114000&&q.cfg.probe2RepeatUs()==128000&&x.complete);
  CHECK(x.tx[0].ok&&x.tx[1].ok&&x.have_poll2_frame);
  CHECK(x.response2_tx[0].ok&&x.response2_tx[1].ok);
  CHECK(serializeRecord(x,json,sizeof json,n));
  std::ofstream(out+"/synthetic_tag_stage6.json")<<json<<"\n";
  CHECK(probeReceiveMask(x,0)==3&&probeReceiveMask(x,1)==3);
  CHECK(probeCirMask(x,0)==3&&probeCirMask(x,1)==3);
  CHECK(x.rx_windows[0][0].accepted&&x.rx_windows[4][1].accepted);
  CHECK(q.probe2ReadsInsideProtection==0);
  CHECK(x.probe2_spi_quiet[0][1]-x.probe2_spi_quiet[0][0]==2900);
  for(auto& traces:x.rx_windows)for(auto& t:traces){
   t.fail(MASK40,UINT64_MAX-3,12);
   t.fail(1,UINT64_MAX-2,8);
   t.recovery_us=UINT64_MAX-1;t.close_us=UINT64_MAX;
   CHECK(t.first_status==MASK40&&t.first_us==UINT64_MAX-3&&t.first_reason==12&&t.failures==2);
  }
  CHECK(serializeRecord(x,json,sizeof json,n));
  q.cfg.period_us=100000;CHECK(validate(q.cfg)!=nullptr);
 }
 for(unsigned probe=0;probe<2;++probe){
  SimTag q;q.cfg.stage=6;q.cfg.period_us=200000;q.cfg.baud=2000000;q.drop[1][probe+1]=true;
  auto x=q.execute();const unsigned window=probe?4:0;
  CHECK(!x.complete&&probeReceiveMask(x,probe)==1&&probeCirMask(x,probe)==1);
  CHECK(x.rx_windows[window][0].accepted&&!x.rx_windows[window][1].accepted);
  CHECK(x.rx_windows[window][1].close_us>0);
 }
 {SimTag q;q.cfg.stage=6;q.cfg.period_us=200000;q.cfg.baud=2000000;
  q.cfg.grace_us=1500;q.txn=781;q.injectProbe2StatusA=0x00810f02;
  auto x=q.execute();
  CHECK(x.errors==ErrProbe2&&!x.complete);
  CHECK(probeReceiveMask(x,0)==3&&probeReceiveMask(x,1)==2&&probeCirMask(x,1)==2);
  CHECK(x.rx_windows[4][0].first_status==0x00810f02&&!x.rx_windows[4][0].accepted);
  CHECK(x.rx_windows[4][0].recovery_us-x.rx_windows[4][0].first_us==527);
  CHECK(x.rx_windows[4][1].accepted&&(x.rx_deadline_latch&(1U<<5)));
  CHECK(x.probe2_spi_quiet[0][1]-x.probe2_spi_quiet[0][0]==2500);
  CHECK(serializeRecord(x,json,sizeof json,n)&&strstr(json,"probe2_spi_quiet_offsets_us"));
 }
 for(const uint64_t status:{0x00810f02ULL,0x00801302ULL}){
  for(bool responseRetry:{false,true}){
   SimTag q;q.cfg.stage=6;q.cfg.period_us=200000;q.cfg.baud=2000000;
   q.injectProbe2StatusA=status;q.retryProbe=responseRetry;
   auto x=q.execute(true);
   CHECK(x.complete&&x.p2_repeat_used&&!x.p2_repeat_late&&x.p2_retry==responseRetry);
   CHECK(x.p2_primary_rx_mask==2&&x.p2_primary_cir_mask==2);
   CHECK(x.rx_windows[responseRetry?5:4][0].first_status==status);
   CHECK(x.rx_windows[3][0].accepted&&x.rx_windows[3][1].accepted);
   CHECK(probeReceiveMask(x,1)==3&&probeCirMask(x,1)==3&&q.last[0]==7&&q.last[1]==7);
   CHECK(x.probe_frame[1].probe_tx==q.ac.at(q.probeGlobal(7)));
   CHECK(x.probe[1][0].hw==q.tc[0].at(q.eventAt(7,0))&&x.probe[1][1].hw==q.tc[1].at(q.eventAt(7,1)));
   for(unsigned i=0;i<2;++i)CHECK(x.cir_meta[2+i].crc==crc32(x.cir[2+i],CIR_BYTES));
   CHECK(serializeRecord(x,json,sizeof json,n)&&strstr(json,"\"p2_repeat_used\":true"));
  }
 }
 {SimTag q;q.cfg.stage=6;q.cfg.period_us=200000;q.cfg.baud=2000000;auto x=q.execute(true);
  CHECK(x.complete&&!x.p2_repeat_used&&x.p2_primary_rx_mask==3&&x.p2_primary_cir_mask==3);
  CHECK(!q.used[0][7]&&!q.used[1][7]);}
 {SimTag q;q.cfg.stage=6;q.cfg.period_us=200000;q.cfg.baud=2000000;
  q.drop[0][2]=true;q.drop[1][7]=true;auto x=q.execute(true);
  CHECK(!x.complete&&!x.p2_repeat_used&&(x.errors&ErrProbe2));
  CHECK(probeReceiveMask(x,1)==2&&probeCirMask(x,1)==2&&x.p2_primary_rx_mask==2);
  CHECK(x.probe[1][1].hw==q.tc[1].at(q.eventAt(2,1))&&x.cir_meta[3].crc==crc32(x.cir[3],CIR_BYTES));}
 {SimTag q;q.cfg.stage=6;q.cfg.period_us=200000;q.cfg.baud=2000000;
  q.drop[0][2]=true;q.p2CirCost=13000;auto x=q.execute(true);
  CHECK(!x.complete&&!x.p2_repeat_used&&x.p2_repeat_late&&(x.errors&ErrProbe2));
  CHECK(!q.used[0][7]&&!q.used[1][7]&&probeReceiveMask(x,1)==2);}
 {SimTag q;q.cfg.stage=6;q.cfg.period_us=200000;q.cfg.baud=2000000;
  q.drop[0][2]=true;q.repeatMetadataConflict=true;auto x=q.execute(true);
  CHECK(!x.complete&&!x.p2_repeat_used&&(x.errors&ErrMetadata)&&x.metadata_conflict);}
 for(bool firstPollMissed:{false,true}){
  SimTag q;q.cfg.stage=6;q.cfg.period_us=200000;q.cfg.baud=2000000;q.retryFirst=q.retryProbe=true;
  q.drop[0][0]=q.drop[1][0]=firstPollMissed;
  auto x=q.execute();
  CHECK(x.complete&&x.p1_retry&&x.p2_retry&&x.errors==0);
  CHECK(x.p1_first_errors==(firstPollMissed?ErrPoll:0U)&&x.initial_poll_tx==q.ac.at(q.t0));
  CHECK(x.poll_frame.poll_tx==q.ac.at(q.firstGlobal())&&x.poll_frame.probe_tx==x.initial_poll_tx);
  CHECK(x.poll[0].hw==q.tc[0].at(q.eventAt(6,0))&&x.poll[1].hw==q.tc[1].at(q.eventAt(6,1)));
 }
 for(bool missedPoll2:{false,true}){
  SimTag q;q.cfg.stage=6;q.cfg.period_us=200000;q.cfg.baud=2000000;q.retryProbe=true;q.drop[1][3]=missedPoll2;
  auto x=q.execute();
  CHECK(x.complete&&x.p2_retry&&x.errors==0&&x.have_poll2_frame);
  CHECK(x.p2_first_errors==(missedPoll2?(ErrPoll2|ErrResponse2B):0));
  CHECK(x.response2_tx[0].ok&&x.response2_tx[1].ok&&x.cir_meta[2].valid&&x.cir_meta[3].valid);
  CHECK(q.last[0]==5&&q.last[1]==5&&x.poll2_frame.poll_tx==q.ac.at(q.probeGlobal(4)));
 }
 {SimTag q;q.cfg.stage=6;q.cfg.period_us=200000;q.cfg.baud=2000000;q.retryProbe=true;
  q.drop[1][4]=true;auto x=q.execute();
  CHECK(x.p2_retry&&!x.complete&&(x.errors&ErrPoll2)&&(x.errors&ErrResponse2B));
  CHECK(x.response2_tx[0].ok&&!x.response2_tx[1].ok);
 }
 {SimAnchor q;q.cfg.stage=4;auto x=q.execute();CHECK(x.complete&&x.response_mask==3);CHECK(q.readsDuringResponse==0);}
 {SimAnchor q;q.cfg.stage=3;auto x=q.execute();CHECK(x.complete&&x.response_mask==3);CHECK(q.readsDuringResponse==0);}
 {SimAnchor q;q.cfg.stage=3;q.lost[0]=true;auto x=q.execute();
  CHECK(x.response_mask==2&&!x.complete);CHECK(x.tx[1].ok&&x.tx[2].ok);}
 {SimAnchor q;auto x=q.execute();CHECK(x.complete);CHECK(x.response2_mask==3);CHECK(q.readsDuringResponse==0);CHECK(q.txframes.size()==3);CHECK(q.txframes[2].tx_ok_mask==3);CHECK(q.txframes[2].prev_probe_actual==x.tx[1].actual);CHECK(fabs(double(diff40(x.tx[2].actual,x.tx[0].actual))*1e6/HZ-178000)<0.02);}
 {SimTag q;q.holdResponse2ASetupUntilUs=6463;auto x=q.execute();
  CHECK(x.complete&&x.response2_tx[0].ok&&x.response2_tx[1].ok);}
 {SimTag q;q.drop[0][3]=true;auto x=q.execute();
  CHECK((x.errors&ErrPoll2)&&(x.errors&ErrResponse2A)&&x.response2_tx[1].ok);
  CHECK(x.p1_tx_confirmed&&x.cir_meta[2].valid&&x.cir_meta[3].valid);}
 {SimAnchor q;q.lost2[1]=true;auto x=q.execute();
  CHECK(x.response_mask==3&&x.response2_mask==1&&!x.complete);
  CHECK(x.tx[2].ok&&(x.errors&ErrResponse2B));}
 {SimAnchor q;q.cfg.stage=6;q.cfg.period_us=200000;q.cfg.baud=2000000;q.lost[0]=true;q.lost2[1]=true;
  auto x=q.execute();
  CHECK(q.pollAttempts==2&&q.poll2Attempts==2&&x.complete&&x.p1_retry&&x.p2_retry);
  CHECK(x.p1_first_errors==ErrResponseA&&x.p2_first_errors==ErrResponse2B);
  CHECK(isRetryPoll(x.poll_frame)&&x.poll_frame.probe_tx==x.initial_poll_tx);
  CHECK(diff40((x.initial_poll_tx+usTicks(28000))&MASK40,x.tx[0].actual)<=511);
  CHECK(diff40((x.initial_poll_tx+usTicks(170000))&MASK40,x.tx[2].actual)<=1533);
  CHECK(diff40((x.initial_poll_tx+usTicks(184000))&MASK40,x.p2_repeat_tx.actual)<=1533);
 }
 for(bool loseRetry:{false,true}){
  SimAnchor q;q.cfg.stage=6;q.cfg.period_us=200000;q.cfg.baud=2000000;q.lost2[1]=true;q.loseRetry=loseRetry;
  auto x=q.execute();
  CHECK(q.poll2Attempts==2&&x.p2_retry&&x.p2_first_errors==ErrResponse2B);
  CHECK(x.complete==!loseRetry&&x.response2_mask==(loseRetry?1:3));
  CHECK(diff40((x.tx[0].actual+usTicks(114000))&MASK40,x.poll2_tx.actual)<=511);
  CHECK(diff40((x.tx[0].actual+usTicks(142000))&MASK40,x.tx[2].actual)<=511);
  CHECK(diff40((x.tx[0].actual+usTicks(156000))&MASK40,x.p2_repeat_tx.actual)<=511);
  CHECK(x.p2_repeat_tx.ok&&x.p2_repeat_frame.probe_tx==x.p2_repeat_tx.actual);
  Frame normalized=x.p2_repeat_frame;normalized.probe_tx=x.probe_frame[1].probe_tx;
  CHECK(sameFrame(normalized,x.probe_frame[1]));
  CHECK(serializeRecord(x,json,sizeof json,n)&&strstr(json,"p2_repeat_frame")&&strstr(json,"p2_repeat_tx"));
 }
 {SimAnchor q;q.lost[0]=true;auto x=q.execute();CHECK(x.response_mask==2);CHECK(!x.complete);CHECK(q.txframes.size()==3);}
 {SimAnchor q;q.lost[1]=true;auto x=q.execute();CHECK(x.response_mask==1);CHECK(x.tx[1].ok&&x.tx[2].ok);}
 {SimAnchor q;q.badBoot=true;auto x=q.execute();CHECK(x.response_mask==1);}
 {SimAnchor q;q.txfail=true;auto x=q.execute();CHECK(!x.complete);CHECK(q.txframes.size()==3&&q.txframes[2].tx_ok_mask==1);}
 {SimAnchor q;q.cfg.stage=2;auto x=q.execute();CHECK(x.complete);CHECK(x.response_mask==0);
  CHECK(!x.peer_boot&&!x.probe_frame[0].peer_boot&&!x.probe_frame[1].peer_boot);}
 // Serialization also accepts largest diagnostic integers without overrunning the bounded writer buffer.
 for(auto&z:r.poll){z.hw=MASK40;z.raw=MASK40;z.status=MASK40;z.esp_us=0xffffffffffffffffULL;z.arm_us=z.esp_us;z.ci=-2147483647;}
 for(auto&pp:r.probe)for(auto&z:pp){z.hw=MASK40;z.raw=MASK40;z.status=MASK40;z.esp_us=0xffffffffffffffffULL;z.arm_us=z.esp_us;z.ci=-2147483647;}
 CHECK(serializeRecord(r,json,sizeof json,n));std::cout<<"wide_json_bytes="<<n<<"\n";
}
static void testRxDelayReadback(){
 const uint16_t expected=16436;
 const auto run=[&](uint16_t first,uint16_t retry1,uint16_t retry2,uint64_t status,
                    bool accepted,unsigned expectedReads,unsigned expectedStatusReads){
  const uint16_t samples[]={retry1,retry2};
  unsigned reads=0,statusReads=0;
  CHECK(rxguard::confirmRxDelay(first,expected,[&](){CHECK(reads<2);return samples[reads++];},
                                [&](){++statusReads;return status;})==accepted);
  CHECK(reads==expectedReads&&statusReads==expectedStatusReads);
 };
 run(expected,0,0,0,true,0,0);
 run(123,expected,expected,0,false,0,0);
 run(UINT16_MAX,expected,expected,0x800002,true,2,1);
 run(UINT16_MAX,UINT16_MAX,expected,0,false,1,0);
 run(UINT16_MAX,expected,123,0,false,2,0);
 run(UINT16_MAX,expected,expected,0x2800002,false,2,1);
 run(UINT16_MAX,expected,expected,MASK40,false,2,1);
}
int main(int argc,char**argv){try{testsCore();testRxDelayReadback();testsEngine(argc>1?argv[1]:".");std::cout<<"PASS checks="<<checks<<" (HOST SYNTHETIC ONLY)\n";return 0;}catch(const std::exception&e){std::cerr<<"FAIL "<<e.what()<<"\n";return 1;}}
