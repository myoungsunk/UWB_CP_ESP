#line 1 "D:\\SLAM_bot\\artifacts\\UWB_MODE8_R6_ANCHOR_INIT_DIAG_20261111\\firmware\\ANCHOR_DUAL_DS_CIR\\core.hpp"
#pragma once
// Dual-DW1000 Mode 8, protocol v1. Portable C++11; no MCU/Arduino dependency.
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>
#include <limits>
#include <initializer_list>
namespace dual8 {
constexpr uint64_t MASK40=(1ULL<<40)-1, HALF40=1ULL<<39;
constexpr double HZ=63897600000.0, DTU=1.0/HZ, C_AIR=299702547.0;
constexpr double SAMPLE_S=1.0/998400000.0; // 64 DW1000 timestamp ticks, not 1 DTU.
constexpr size_t NSAMP=992, CIR_BYTES=NSAMP*4, AIR_BYTES=96;
constexpr uint32_t AIR_MAGIC=0x38424346, WIRE_MAGIC=0x38424652;
constexpr uint8_t PROTOCOL_VERSION=1, MODE=8;
inline uint64_t diff40(uint64_t b,uint64_t a){return (b-a)&MASK40;}
inline int64_t signed40(uint64_t b,uint64_t a){uint64_t d=diff40(b,a);return d&HALF40?int64_t(d)-int64_t(MASK40+1):int64_t(d);}
inline uint64_t usTicks(uint64_t us){return us*638976ULL/10ULL;}
inline uint64_t getLE(const uint8_t*p,size_t n){uint64_t v=0;for(size_t j=0;j<n;++j)v|=uint64_t(p[j])<<(8*j);return v;}
inline void putLE(uint8_t*p,uint64_t v,size_t n){for(size_t j=0;j<n;++j)p[j]=uint8_t(v>>(8*j));}
inline uint32_t crcStep(uint32_t c,const void*p,size_t n){auto b=static_cast<const uint8_t*>(p);while(n--){c^=*b++;for(unsigned j=0;j<8;++j)c=(c>>1)^((c&1)?0xedb88320UL:0);}return c;}
inline uint32_t crc32(const void*p,size_t n){return crcStep(0xffffffffUL,p,n)^0xffffffffUL;}
struct Pins {uint8_t cs,irq,rst;};
constexpr Pins TAG_PINS[2]={{21,22,17},{25,26,27}};
constexpr Pins ANCHOR_PINS={21,22,5}; // Original anchor wiring; GPIO5 here is RSTn, NOT a switch.
struct Config {
 uint8_t stage=5;
 uint32_t period_us=300000,reply_a_us=7000,reply_b_us=14000,probe1_us=19000,probe_gap_us=32000;
 uint32_t grace_us=2200,min_lead_us=600,cir_hz=20000000,baud=921600;
 bool verify_cir=true;
 uint32_t probe2_us()const{return probe1_us+probe_gap_us;}
};
inline uint32_t configId(const Config&c){
 uint8_t b[40]{};b[0]=MODE;b[1]=PROTOCOL_VERSION;b[2]=c.stage;
 const uint32_t v[]={c.period_us,c.reply_a_us,c.reply_b_us,c.probe1_us,c.probe_gap_us,c.grace_us,c.min_lead_us};
 for(unsigned i=0;i<7;++i)putLE(b+4+4*i,v[i],4);
 // RF is fixed: ch5, 850 kbps, 16 MHz PRF, 256-symbol preamble, code3, standard SFD.
 b[32]=5;putLE(b+33,850,2);b[35]=16;putLE(b+36,256,2);b[38]=3;
 // R4 contract: FCS-safe transmit buffer and provenance-hardened capture.
 b[39]=4;
 return crc32(b,sizeof b);
}
inline const char* validate(const Config&c){
 if(c.stage<1||c.stage>6)return "STAGE_1_TO_6";
 if(c.reply_a_us<5000||c.reply_a_us>100000)return "REPLY_A_BOUND";
 if(c.reply_b_us<c.reply_a_us+5000||c.reply_b_us>200000)return "RESPONSE_SEPARATION";
 if(c.grace_us<1500||c.grace_us>3000)return "RX_GRACE_BOUND";
 if(c.probe1_us<c.reply_b_us+c.grace_us+2500||c.probe1_us>250000)return "P1_PREPARE_BUDGET";
 if(c.probe_gap_us<16000||c.probe_gap_us>100000)return "CIR_GAP_BUDGET";
 if(c.min_lead_us<500||c.min_lead_us>1500)return "TX_LEAD_BOUND";
 if(c.cir_hz!=10000000&&c.cir_hz!=20000000)return "CIR_HZ_10M_OR_20M";
 if(c.baud!=921600&&c.baud!=2000000)return "BAUD_921600_OR_2000000";
 if(c.period_us<100000||c.period_us>5000000||c.period_us<c.probe2_us()+10000)return "PERIOD_RF_BUDGET";
 if(c.stage>=2){ // Conservative raw + metadata throughput with 20% UART headroom.
  const uint64_t minPeriod=(uint64_t(4*CIR_BYTES+7000)*10*1200000ULL+c.baud-1)/c.baud;
  if(c.period_us<minPeriod)return "PERIOD_UART_BUDGET";
 }
 return nullptr;
}
struct Calibration {uint16_t tx=16436,rx=16436;uint32_t id=0;double range_sigma_m=NAN;};
enum class Type:uint8_t {Poll=1,RespA=2,RespB=3,Probe1=4,Probe2=5};
struct Frame {
 Type type=Type::Poll;uint8_t radio=255,resp_mask=0,tx_ok_mask=0,stage=5;
 uint32_t session=0,txn=0,config=0,boot=0,peer_boot=0;
 uint64_t poll_tx=0,resp_rx[2]{},probe_tx=0,prev_probe_actual=0,tag_poll_rx=0,tag_resp_tx=0;
 int32_t ci[2]{};
};
inline bool validFrameFields(const Frame&f){
 const unsigned t=unsigned(f.type);
 if(t<1||t>5||!f.session||!f.boot||f.stage<2||f.stage>6||(f.resp_mask&~3U)||(f.tx_ok_mask&~3U))return false;
 if(t==2||t==3){if(f.radio!=t-2||!f.peer_boot)return false;}else if(f.radio!=255)return false;
 const uint64_t ts[]={f.poll_tx,f.resp_rx[0],f.resp_rx[1],f.probe_tx,f.prev_probe_actual,f.tag_poll_rx,f.tag_resp_tx};
 for(auto v:ts)if(v>MASK40)return false;
 return true;
}
inline bool encode(const Frame&f,uint8_t out[AIR_BYTES]){
 if(!validFrameFields(f))return false;
 memset(out,0,AIR_BYTES);putLE(out,AIR_MAGIC,4);out[4]=PROTOCOL_VERSION;out[5]=uint8_t(f.type);putLE(out+6,AIR_BYTES,2);
 putLE(out+8,f.session,4);putLE(out+12,f.txn,4);putLE(out+16,f.config,4);putLE(out+20,f.boot,4);putLE(out+24,f.peer_boot,4);
 out[28]=f.radio;out[29]=f.resp_mask;out[30]=f.tx_ok_mask;out[31]=f.stage;
 const uint64_t ts[]={f.poll_tx,f.resp_rx[0],f.resp_rx[1],f.probe_tx,f.prev_probe_actual,f.tag_poll_rx,f.tag_resp_tx};
 for(unsigned i=0;i<7;++i)putLE(out+32+5*i,ts[i],5);
 putLE(out+67,uint32_t(f.ci[0]),4);putLE(out+71,uint32_t(f.ci[1]),4);
 putLE(out+92,crc32(out,92),4);return true;
}
inline bool decode(const uint8_t*in,size_t n,Frame&f){
 if(n!=AIR_BYTES||getLE(in,4)!=AIR_MAGIC||in[4]!=PROTOCOL_VERSION||getLE(in+6,2)!=n||getLE(in+92,4)!=crc32(in,92))return false;
 for(unsigned i=75;i<92;++i)if(in[i])return false;
 f=Frame{};f.type=Type(in[5]);f.session=uint32_t(getLE(in+8,4));f.txn=uint32_t(getLE(in+12,4));f.config=uint32_t(getLE(in+16,4));
 f.boot=uint32_t(getLE(in+20,4));f.peer_boot=uint32_t(getLE(in+24,4));f.radio=in[28];f.resp_mask=in[29];f.tx_ok_mask=in[30];f.stage=in[31];
 uint64_t* ts[]={&f.poll_tx,&f.resp_rx[0],&f.resp_rx[1],&f.probe_tx,&f.prev_probe_actual,&f.tag_poll_rx,&f.tag_resp_tx};
 for(unsigned i=0;i<7;++i)*ts[i]=getLE(in+32+5*i,5);
 f.ci[0]=int32_t(uint32_t(getLE(in+67,4)));f.ci[1]=int32_t(uint32_t(getLE(in+71,4)));return validFrameFields(f);
}
inline bool sameFrame(const Frame&a,const Frame&b){uint8_t x[AIR_BYTES],y[AIR_BYTES];return encode(a,x)&&encode(b,y)&&!memcmp(x,y,AIR_BYTES);}
inline uint64_t plannedRaw(uint64_t requested,uint16_t txAnt){return ((requested-txAnt)&MASK40)&~uint64_t(511);}
inline uint64_t plannedStamp(uint64_t requested,uint16_t txAnt){return (plannedRaw(requested,txAnt)+txAnt)&MASK40;}
struct DsInput {uint64_t a_poll_tx=0,a_resp_rx=0,a_probe_tx=0,t_poll_rx=0,t_resp_tx=0,t_probe_rx=0;};
struct DsResult {bool valid=false;double tof_ticks=NAN,range_m=NAN;uint64_t r1=0,d1=0,r2=0,d2=0;};
inline DsResult dsTwr(const DsInput&i){
 DsResult o;o.r1=diff40(i.a_resp_rx,i.a_poll_tx);o.d1=diff40(i.t_resp_tx,i.t_poll_rx);o.r2=diff40(i.t_probe_rx,i.t_resp_tx);o.d2=diff40(i.a_probe_tx,i.a_resp_rx);
 for(auto v:{o.r1,o.d1,o.r2,o.d2})if(!v||v>usTicks(500000))return o;
 // Convert BEFORE multiplication: 32/64-bit integer products can overflow. ESP32 double is IEEE-754 binary64.
 const double r1=double(o.r1),d1=double(o.d1),r2=double(o.r2),d2=double(o.d2);
 o.tof_ticks=(r1*r2-d1*d2)/(r1+d1+r2+d2);o.range_m=o.tof_ticks*DTU*C_AIR;
 o.valid=isfinite(o.range_m)&&o.range_m>=0&&o.range_m<=300.0;
 return o;
}
inline double rxPower(uint16_t p,uint16_t n){return p&&n?10.0*log10(double(p)*131072.0/(double(n)*n))-113.77:NAN;}
inline double fpPower(uint16_t f1,uint16_t f2,uint16_t f3,uint16_t n){const double a=double(f1)*f1+double(f2)*f2+double(f3)*f3;return a>0&&n?10*log10(a/(double(n)*n))-113.77:NAN;}
inline double relDelayS(double k,uint16_t fp_raw){return (k-double(fp_raw)/64)*SAMPLE_S;}
inline double physicalDelayS(double k,uint16_t fp_raw,double range){return range/C_AIR+relDelayS(k,fp_raw);}
struct Rx {
 bool received=false,lde=false,diag=false;uint8_t radio=255,type=0;uint16_t bytes=0,fp_raw=0,nacc=0,pwr=0,f1=0,f2=0,f3=0,noise=0;
 int32_t ci=0;uint64_t hw=0,raw=0,status=0,esp_us=0,arm_us=0;uint32_t diag_us=0;uint8_t air[AIR_BYTES]{};
};
struct Tx {bool attempted=false,ok=false;uint8_t radio=255,type=0;uint64_t requested=0,planned=0,actual=0,status=0,begin_us=0,end_us=0;int32_t lead_us=0;bool hardware_fault=false;};
struct Cir {bool present=false,valid=false,repeat_equal=false;uint32_t bytes=0,crc=0,read_us=0,repeat_us=0;};
struct Record {
 uint32_t session=0,txn=0,boot=0,peer_boot=0,config=0;Config cfg{};uint64_t begin_us=0,end_us=0;
 bool anchor=false,metadata_conflict=false,p1_tx_confirmed=false,complete=false;uint8_t response_mask=0;
 uint32_t errors=0;Rx poll[2]{},probe[2][2]{},responses[2]{};Tx tx[3]{};Frame poll_frame{},probe_frame[2]{};
 bool have_probe_frame[2]{};DsResult range[2]{};DsInput range_input[2]{};
 Calibration cal[2]{};Cir cir_meta[4]{};uint8_t cir[4][CIR_BYTES]{};
};
enum Error:uint32_t {ErrHardware=1,ErrPoll=2,ErrResponseA=4,ErrResponseB=8,ErrProbe1=16,ErrProbe2=32,ErrMetadata=64,ErrCir=128,ErrLate=256,ErrRange=512};
inline bool eligibleProbe(const Frame&f,const Record&r,unsigned p){
 return f.type==(p?Type::Probe2:Type::Probe1)&&f.session==r.session&&f.txn==r.txn&&f.boot==r.peer_boot&&f.config==r.config&&f.stage==r.cfg.stage&&(f.peer_boot==0||f.peer_boot==r.boot);
}
inline bool coherentAnchorBlock(const Frame&a,const Frame&b){
 return a.poll_tx==b.poll_tx&&a.resp_mask==b.resp_mask&&a.resp_rx[0]==b.resp_rx[0]&&a.resp_rx[1]==b.resp_rx[1]&&a.peer_boot==b.peer_boot;
}
inline void finalizeRanges(Record&r){
 // A latched hardware fault, late deadline or metadata fault invalidates all
 // physical ranges even if the on-air timestamp fields happen to look coherent.
 const uint32_t fatal_integrity=ErrHardware|ErrMetadata|ErrLate;
 r.p1_tx_confirmed=!(r.errors&fatal_integrity)&&r.have_probe_frame[0]&&r.have_probe_frame[1]&&!r.metadata_conflict&&
   coherentAnchorBlock(r.probe_frame[0],r.probe_frame[1])&&((r.probe_frame[1].tx_ok_mask&3)==3)&&
   r.probe_frame[1].prev_probe_actual==r.probe_frame[0].probe_tx;
 for(unsigned i=0;i<2;++i){
  r.range[i]=DsResult{};
  if(r.cfg.stage<4||!r.p1_tx_confirmed||!r.poll[i].received||!r.poll[i].lde||!r.tx[i].ok||!r.probe[0][i].received||!r.probe[0][i].lde||!(r.probe_frame[0].resp_mask&(1U<<i)))continue;
  const auto&f=r.probe_frame[0];auto&d=r.range_input[i];d.a_poll_tx=f.poll_tx;d.a_resp_rx=f.resp_rx[i];d.a_probe_tx=f.probe_tx;
  d.t_poll_rx=r.poll[i].hw;d.t_resp_tx=r.tx[i].actual;d.t_probe_rx=r.probe[0][i].hw;r.range[i]=dsTwr(d);
 }
 r.complete=r.errors==0&&!r.metadata_conflict;
 for(unsigned s=0;s<4;++s)r.complete=r.complete&&r.cir_meta[s].valid;
 if(r.cfg.stage>=3)r.complete=r.complete&&r.tx[0].ok&&r.tx[1].ok;
 if(r.cfg.stage>=4)r.complete=r.complete&&r.range[0].valid&&r.range[1].valid;
}
} // namespace dual8
