#include "engine.hpp"
#include "record_json.hpp"
#include "rx_guard_policy.hpp"
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
 bool armed[2]{},used[2][3]{},drop[2][3]{},badP1=false,conflict=false,failTx[2]{},corruptCir=false,hwFault=false;
 long double armT[2]{};uint64_t respAnchor[2]{};unsigned last[2]{},calls[2]{},arms[2]{};
 uint8_t respMask=0;uint32_t cirCost=3600,txn=0,holdResponseASetupUntilUs=0;
 unsigned probeReadsDuringFrame=0;
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
 long double probeGlobal(unsigned k){const uint64_t req=(ac.at(t0)+usTicks(k==1?cfg.probe1DelayUs():cfg.probe2_us()))&MASK40;const uint64_t actual=plannedStamp(req,16436);return t0+static_cast<long double>(signed40(actual,ac.at(t0)))/ac.rate;}
 long double eventAt(unsigned k,unsigned i){return (k?probeGlobal(k):t0)+tau[i];}
 Frame frame(unsigned k,unsigned i){Frame f;f.session=77;f.txn=txn;f.config=configId(cfg);f.boot=101;f.stage=cfg.stage;f.poll_tx=ac.at(t0);
  if(k){f.type=k==1?Type::Probe1:Type::Probe2;f.peer_boot=respMask?202:0;f.resp_mask=respMask;f.resp_rx[0]=respAnchor[0];f.resp_rx[1]=respAnchor[1];f.tx_ok_mask=1;
   f.probe_tx=ac.at(probeGlobal(k));
   if(k==2){f.prev_probe_actual=ac.at(probeGlobal(1));if(!badP1)f.tx_ok_mask|=2;else ++f.prev_probe_actual;}
   if(conflict&&i==1&&k==1)++f.resp_rx[0];
  }return f;
 }
 int receive(uint8_t i,Frame&f,Rx&o)override{
  if(hwFault){return -2;}
  if(!armed[i])return 0;
  for(unsigned k=0;k<3;++k){if(used[i][k])continue;const long double at=eventAt(k,i);
   if(k&&t>=at-usTicks(350)&&t<at+usTicks(1050))++probeReadsDuringFrame;
   if(t<at+usTicks(1200))return 0;
   used[i][k]=true;if(drop[i][k]||armT[i]>at-usTicks(250))continue;
   f=frame(k,i);o=Rx{};o.received=true;o.lde=true;o.diag=true;o.radio=i;o.type=uint8_t(f.type);o.hw=tc[i].at(at);o.raw=(o.hw+100)&MASK40;
   o.esp_us=nowUs();o.arm_us=uint64_t(armT[i]/HZ*1e6L);o.status=(1ULL<<10)|(1ULL<<13)|(1ULL<<14);o.bytes=AIR_BYTES;o.fp_raw=uint16_t(740*64+i*5);o.nacc=240;o.pwr=12000;o.f1=120;o.f2=130;o.f3=140;o.noise=3;o.ci=i?44:-20;CHECK(encode(f,o.air));
   last[i]=k;armed[i]=false;return 1;
  }return 0;
 }
 bool send(uint8_t i,Frame&f,uint64_t req,Tx&o)override{
  o=Tx{};o.attempted=true;o.radio=i;o.type=uint8_t(f.type);o.requested=req;o.planned=plannedStamp(req,16436);o.begin_us=nowUs();
  if(i==0&&holdResponseASetupUntilUs)t=std::max(t,eventAt(0,i)+usTicks(holdResponseASetupUntilUs));
  const int64_t lead=signed40(o.planned,system(i));if(lead<int64_t(usTicks(600)))return false;
  const bool second=f.type==responseType(true,i);
  const long double sendAt=t+static_cast<long double>(lead)/tc[i].rate;
  t=sendAt+usTicks(1200);o.actual=o.planned+(failTx[i]?1:0);o.ok=second||!failTx[i];if(second)o.actual=o.planned;o.status=o.ok?0x80:0;o.end_us=nowUs();f.tag_resp_tx=o.planned;
  if(!second){respAnchor[i]=ac.at(sendAt+tau[i]);respMask|=uint8_t(1U<<i);}return o.ok;
 }
 CirRead readCir(uint8_t i,uint8_t*d,Cir&o)override{
  CHECK(!armed[0]&&!armed[1]);CHECK(last[i]==1||last[i]==2);++calls[i];o.present=true;o.bytes=CIR_BYTES;o.read_us=cirCost/2;o.repeat_us=cirCost/2;
  for(size_t k=0;k<NSAMP;++k){putLE(d+4*k,uint16_t(int16_t((int(k)+int(i)*3)%50-25)),2);putLE(d+4*k+2,uint16_t(int16_t(20-int(k)%31)),2);}
  o.crc=crc32(d,CIR_BYTES);t+=usTicks(cirCost);
  if(i==cirFaultRadio&&last[i]==cirFaultProbe){hwFault=true;return CirRead::HardwareFault;}
  o.repeat_equal=!corruptCir;o.valid=!corruptCir;
  return o.valid?CirRead::Valid:CirRead::Invalid;
 }
 void relax()override{t+=usTicks(25);}
 Record execute(){Record r;arm(0);arm(1);Frame f;Rx o;uint8_t first=0;
  bool found=false;for(unsigned n=0;n<2000&&!found;++n){for(unsigned i=0;i<2&&!found;++i)if(receive(i,f,o)==1){first=i;found=true;}if(!found)relax();}CHECK(found);
  runTag(*this,cfg,202,first,f,o,r);return r;
 }
};
struct SimAnchor:Port {
 Config cfg;uint64_t t=0,poll=0,p1=0;bool armed=false,seen[2][2]{},lost[2]{},lost2[2]{},badBoot=false,txfail=false;uint32_t peer=202;std::vector<Frame> txframes;
 unsigned readsDuringResponse=0;
 uint64_t nowUs()override{return t;}
 uint64_t system(uint8_t)override{return (1000000+usTicks(t))&MASK40;}
 bool healthy(uint8_t)override{return true;}
 bool arm(uint8_t)override{armed=true;return true;}
 void idle(uint8_t)override{armed=false;}
 int receive(uint8_t,Frame&f,Rx&o)override{
  if(!armed)return 0;
  const bool second=p1!=0;
  const uint64_t origin=second?p1:poll;
  for(unsigned i=0;i<2;++i){if(seen[second][i])continue;
   uint64_t at=(origin+usTicks(second?cfg.response2DelayUs(i):cfg.responseDelayUs(i))+2132)&MASK40;
   const int64_t relative=signed40(system(0),at);
   if(relative>=-int64_t(usTicks(350))&&relative<int64_t(usTicks(1050)))++readsDuringResponse;
   if(relative<int64_t(usTicks(1200))){return 0;}
   seen[second][i]=true;if(second?lost2[i]:lost[i])continue;
   f=Frame{};f.type=responseType(second,i);f.radio=i;f.session=77;f.txn=0;f.config=configId(cfg);f.boot=badBoot&&i?999:peer;f.peer_boot=101;f.stage=cfg.stage;f.poll_tx=origin;
   o=Rx{};o.received=o.lde=true;o.hw=at;o.radio=0;o.type=uint8_t(f.type);o.esp_us=t;o.bytes=AIR_BYTES;CHECK(encode(f,o.air));armed=false;return 1;
  }return 0;
 }
 bool send(uint8_t,Frame&f,uint64_t requested,Tx&o)override{
  o=Tx{};o.attempted=true;o.planned=plannedStamp(requested,16436);o.requested=requested;o.actual=o.planned;o.ok=!(txfail&&f.type==Type::Probe1);
  t+=uint64_t(double(signed40(o.planned,system(0)))*1e6/HZ)+1200;
  if(f.type==Type::Poll){poll=o.planned;f.poll_tx=poll;}else {f.probe_tx=o.planned;if(f.type==Type::Probe1)p1=o.planned;}txframes.push_back(f);return o.ok;
 }
 CirRead readCir(uint8_t,uint8_t*,Cir&)override{return CirRead::Invalid;}
 void relax()override{t+=25;}
 Record execute(){Record r;runAnchor(*this,cfg,77,0,101,0,r);return r;}
};
static void testsCore(){
 Config c;CHECK(c.stage==5&&c.responseDelayUs(0)==10000&&c.responseDelayUs(1)==20000);CHECK(validate(c)==nullptr);CHECK(configId(c)==4117194180UL);Config bad=c;bad.period_us=200000;CHECK(validate(bad)!=nullptr);bad.baud=2000000;CHECK(validate(bad)==nullptr);bad.period_us=100000;CHECK(validate(bad)!=nullptr);
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
 SimTag s;Record r=s.execute();CHECK(r.complete);CHECK(r.p1_tx_confirmed);
 for(unsigned i=0;i<2;++i){
  CHECK(r.response2_tx[i].ok&&r.response2_tx[i].type==uint8_t(responseType(true,i)));
  CHECK(r.response2_tx[i].requested==((r.probe[0][i].hw+usTicks(r.cfg.response2DelayUs(i)))&MASK40));
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
   CHECK(q.arms[0]==probe+1&&q.arms[1]==probe+1&&!q.armed[0]&&!q.armed[1]);
   const unsigned slot=(probe-1)*2+first;
   CHECK(x.cir_meta[slot].present&&!x.cir_meta[slot].valid);
   CHECK(x.cir_meta[slot].crc==crc32(x.cir[slot],CIR_BYTES));
  }
 }
 {SimTag q;q.armFaultRadio=0;auto x=q.execute();CHECK(x.errors&ErrHardware);
  CHECK(q.arms[0]==2&&q.arms[1]==1&&!q.armed[0]&&!q.armed[1]);}
 {SimTag q;q.cirCost=20000;auto x=q.execute();CHECK(x.errors&ErrLate);CHECK(!x.range[0].valid);}
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
 {SimTag q;q.cfg.stage=6;q.holdResponseASetupUntilUs=6463;auto x=q.execute();CHECK(!x.tx[0].ok&&x.tx[1].ok);}
 {SimAnchor q;q.cfg.stage=4;auto x=q.execute();CHECK(x.complete&&x.response_mask==3);CHECK(q.readsDuringResponse==0);}
 {SimAnchor q;q.cfg.stage=3;auto x=q.execute();CHECK(x.complete&&x.response_mask==3);CHECK(q.readsDuringResponse==0);}
 {SimAnchor q;q.cfg.stage=3;q.lost[0]=true;auto x=q.execute();
  CHECK(x.response_mask==2&&!x.complete);CHECK(x.tx[1].ok&&x.tx[2].ok);}
 {SimAnchor q;auto x=q.execute();CHECK(x.complete);CHECK(x.response2_mask==3);CHECK(q.readsDuringResponse==0);CHECK(q.txframes.size()==3);CHECK(q.txframes[2].tx_ok_mask==3);CHECK(q.txframes[2].prev_probe_actual==x.tx[1].actual);CHECK(fabs(double(diff40(x.tx[2].actual,x.tx[0].actual))*1e6/HZ-60000)<0.02);}
 {SimAnchor q;q.lost2[1]=true;auto x=q.execute();
  CHECK(x.response_mask==3&&x.response2_mask==1&&!x.complete);
  CHECK(x.tx[2].ok&&(x.errors&ErrResponse2B));}
 {SimAnchor q;q.lost[0]=true;auto x=q.execute();CHECK(x.response_mask==2);CHECK(!x.complete);CHECK(q.txframes.size()==3);}
 {SimAnchor q;q.lost[1]=true;auto x=q.execute();CHECK(x.response_mask==1);CHECK(x.tx[1].ok&&x.tx[2].ok);}
 {SimAnchor q;q.badBoot=true;auto x=q.execute();CHECK(x.response_mask==1);}
 {SimAnchor q;q.txfail=true;auto x=q.execute();CHECK(!x.complete);CHECK(q.txframes.size()==2);}
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
