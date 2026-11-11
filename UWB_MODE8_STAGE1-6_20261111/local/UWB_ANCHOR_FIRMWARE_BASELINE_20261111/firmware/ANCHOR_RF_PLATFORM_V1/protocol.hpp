#line 1 "C:\\UWB\\UWB_R04_R1J6_FIELD_FIX_20261010\\R1M_CTRL_V2_RC1\\built\\sketches\\ANCHOR_RF_PLATFORM_V1\\protocol.hpp"
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
namespace bench {
constexpr uint32_t AIR_MAGIC = 0x35424346UL; // FCB5
constexpr uint32_t WIRE_MAGIC = 0x35424652UL; // RFB5
constexpr uint64_t MASK40 = (1ULL<<40)-1;
constexpr double TICK_HZ = 63897600000.0;
constexpr double DTU_S = 1.0/TICK_HZ;
constexpr double C_AIR_MPS = 299702547.0;
constexpr double CI_PPM_PER_RAW = -0.000573121584378756;
constexpr unsigned NSAMP = 992, CIR_BYTES = NSAMP*4;
constexpr uint32_t R04_PAIR_GAP_US = 300000;
enum AirType : uint8_t {
  POLL=1, RESPONSE=2, SESSION_START=3, A1=4, B=5, A2=6,
  POINT_START=7, POINT_ACCEPT=8, POINT_COMMIT=9, POINT_QUERY=10,
  ROUTE_END=11, ROUTE_END_ACK=12, B1=13, PROBE_A=14, B2=15, POINT_READY=16
};
inline const char* typeName(uint8_t t) {
  switch(t){
    case POLL:return "POLL";case RESPONSE:return "RESP";case SESSION_START:return "SESSION_START";
    case A1:return "A1";case B:return "B";case A2:return "A2";
    case POINT_START:return "POINT_START";case POINT_ACCEPT:return "POINT_ACCEPT";case POINT_COMMIT:return "POINT_COMMIT";
    case POINT_QUERY:return "POINT_QUERY";case ROUTE_END:return "ROUTE_END";case ROUTE_END_ACK:return "ROUTE_END_ACK";case B1:return "B1";case PROBE_A:return "A";case B2:return "B2";case POINT_READY:return "POINT_READY";
    default:return "UNKNOWN";
  }
}
struct Config;
enum AntRoute : uint8_t { ROUTE_A=0, ROUTE_B=1 };
inline const char* antRouteName(uint8_t r){return (r&1U)?"B":"A";}
inline uint8_t fastRouteForTxn(const Config& c,uint32_t txn);
inline uint8_t probeRouteForType(uint8_t t){return (t==B||t==B1||t==B2)?ROUTE_B:ROUTE_A;}
inline uint8_t expectedTagRoute(const Config& c,uint32_t txn,uint8_t t){
  if(t==POLL||t==RESPONSE)return fastRouteForTxn(c,txn);
  return probeRouteForType(t);
}
inline uint64_t delta40(uint64_t b,uint64_t a){return (b-a)&MASK40;}
inline int64_t signed40(uint64_t b,uint64_t a){uint64_t d=delta40(b,a);return (d&(1ULL<<39))?int64_t(d)-int64_t(1ULL<<40):int64_t(d);}
inline uint64_t usTicks(uint64_t us){return us*638976ULL/10ULL;}
inline double ticksUs(int64_t t){return double(t)*1e6/TICK_HZ;}
inline uint32_t crcUpdate(uint32_t c,const void* v,size_t n){auto p=(const uint8_t*)v;while(n--){c^=*p++;for(unsigned j=0;j<8;j++)c=(c>>1)^((c&1)?0xEDB88320UL:0);}return c;}
inline uint32_t crc32(const void* v,size_t n){return crcUpdate(0xFFFFFFFFUL,v,n)^0xFFFFFFFFUL;}
#pragma pack(push,1)
struct AirHeader {uint32_t magic; uint8_t version,type;uint16_t bytes;uint32_t session,txn,config_id,boot;};
struct WireHeader {uint32_t magic;uint8_t version,kind;uint16_t header_bytes;uint32_t record_seq,json_bytes,raw_bytes,crc;};
struct Config {
  uint32_t mode=3,reply_us=7000,probe_gap_us=8000,burst_guard_us=5000,period_us=300000;
  uint32_t switch_us=100,cir_hz=20000000,cir_backend=0,verify_cir=0,min_lead_us=300;
  uint32_t rx_guard_us=500,rx_grace_us=3000,rate_kbps=850,preamble=256;
  uint32_t ant_anchor=16436,ant_tag=16436,diag_fast=1,ci_bias_ppb=255,ci_smooth_n=3;
};
#pragma pack(pop)
constexpr unsigned CONFIG_FIELDS=19;
static_assert(sizeof(Config)==CONFIG_FIELDS*4,"config layout");
inline bool isBabTxn(const Config& c,uint32_t txn){return c.mode==7||(c.mode==6&&((txn&1U)!=0U));}
inline uint8_t fastRouteForTxn(const Config& c,uint32_t txn){
  return (c.mode==7||(c.mode==6&&((txn&1U)!=0U)))?ROUTE_B:ROUTE_A;
}
inline uint32_t logicalSampleId(const Config& c,uint32_t txn){return c.mode==6?(txn/2U):txn;}
static_assert(sizeof(AirHeader)==24,"air header layout");
static_assert(sizeof(WireHeader)==24,"wire header layout");
constexpr const char* CONFIG_NAMES[CONFIG_FIELDS]={"mode","reply_us","probe_gap_us","burst_guard_us","period_us","switch_us","cir_hz","cir_backend","verify_cir","min_lead_us","rx_guard_us","rx_grace_us","rate_kbps","preamble","ant_anchor","ant_tag","diag_fast","ci_bias_ppb","ci_smooth_n"};
inline uint32_t configId(const Config& c){return crc32(&c,sizeof(c));}
inline bool validConfig(const Config& c,const char*& why){
#define CHECK(x,s) if(!(x)){why=s;return false;}
  CHECK(c.mode>=3&&c.mode<=7,"MODE_3_4_5_6_7_ONLY");
  CHECK(c.reply_us>=1000&&c.reply_us<=100000,"REPLY_1000_TO_100000_US");
  CHECK(c.probe_gap_us>=2500&&c.probe_gap_us<=100000,"PROBE_GAP_2500_TO_100000_US");
  CHECK(c.burst_guard_us>=1000&&c.burst_guard_us<=100000,"BURST_GUARD_1000_TO_100000_US");
  CHECK(c.period_us>=10000&&c.period_us<=5000000,"PERIOD_10000_TO_5000000_US");
  CHECK(c.switch_us>=20&&c.switch_us<=10000,"SWITCH_20_TO_10000_US");
  CHECK(c.cir_hz==10000000||c.cir_hz==20000000,"CIR_SPI_10_OR_20_MHZ_ONLY");
  CHECK(c.cir_backend<=1&&c.verify_cir<=1,"BACKEND_0_BYTE_1_BLOCK_VERIFY_0_1");
  CHECK(!(c.mode==3&&c.verify_cir),"MODE3_HAS_NO_CIR");
  CHECK(c.min_lead_us>=50&&c.min_lead_us<=3000,"MIN_LEAD_50_TO_3000_US");
  CHECK(c.rx_guard_us>=100&&c.rx_guard_us<=3000,"RX_GUARD_100_TO_3000_US");
  CHECK(c.rx_grace_us>=1000&&c.rx_grace_us<=50000,"RX_GRACE_1000_TO_50000_US");
  CHECK(c.rate_kbps==850||c.rate_kbps==6800,"RATE_850_OR_6800_ONLY");
  CHECK(c.preamble==128||c.preamble==256,"PREAMBLE_128_OR_256_ONLY");
  CHECK(c.ant_anchor<=65535&&c.ant_tag<=65535&&c.diag_fast==1,"RAWCAL_REQUIRES_DIAG_FAST_1");
  CHECK(c.ci_bias_ppb<=2000,"CI_BIAS_0_TO_2000_PPB");
  CHECK(c.ci_smooth_n==1||c.ci_smooth_n==3,"CI_SMOOTH_1_OR_3_ONLY");
  uint64_t span=6000;
  if(c.mode!=4)span+=c.reply_us;
  if(c.mode!=3)span+=((c.mode==5||c.mode==6||c.mode==7)?c.burst_guard_us:0)+2ULL*c.probe_gap_us;
  if(c.mode==6){CHECK(c.period_us>span+3000,"PERIOD_SHORTER_THAN_PLANNED_RF_SPAN");CHECK(c.period_us==R04_PAIR_GAP_US,"MODE6_PERIOD_MUST_EQUAL_PAIR_GAP");}
  else CHECK(c.period_us>span+3000,"PERIOD_SHORTER_THAN_PLANNED_RF_SPAN");
#undef CHECK
  why="OK";return true;
}
// R1L: probe frames (A1/B/A2/B1/A/B2) may carry the Anchor block (AirHeader.version 2).
// Block = Anchor values known after RESPONSE RX: POLL TX, RESP RX, RESP CI and RX quality.
constexpr uint8_t R1L_BLOCK_BYTES=44, R1L_BLOCK_VERSION=1;
constexpr size_t AIR_MAX_LEN=sizeof(AirHeader)+8+R1L_BLOCK_BYTES+4; // 80
struct AirPacket {AirHeader h{};uint64_t ts[3]{};uint8_t blk[R1L_BLOCK_BYTES]{};uint8_t blk_len=0;};
inline bool isProbeType(uint8_t t){return t==A1||t==B||t==A2||t==B1||t==PROBE_A||t==B2;}
// Anchor-side txn outcome carried in the NEXT block (prev_txn_status).
enum R1LTxnStatus : uint8_t {R1L_TXN_OK=0,R1L_TXN_POLL_TX_FAIL=1,R1L_TXN_RESP_TIMEOUT=2,R1L_TXN_FAST_INVALID=3,R1L_TXN_PROBE_TX_FAIL=4,R1L_TXN_OTHER=5,R1L_TXN_NONE=255};
struct AnchorBlock {
  uint8_t flags=0;          // bit0 Anchor flash store, bit1 fast_valid, bits4-7 block version
  uint8_t prev_status=R1L_TXN_NONE,resp_ok_bitmap=0,ci_hist_count=0;
  uint16_t valid_count=0;   // fast_valid txns since CI history reset (incl. this one)
  uint64_t t_poll_tx=0,t_resp_rx=0;
  int32_t ci_raw=0,ci_prev1=0,ci_prev2=0;
  uint16_t fp_raw=0,rxpacc=0,cir_pwr=0,f1=0,f2=0,f3=0,std_noise=0;uint32_t sys_status=0;
};
inline void r1lPut(uint8_t* o,uint64_t v,unsigned n){for(unsigned i=0;i<n;i++)o[i]=uint8_t(v>>(8*i));}
inline uint64_t r1lGet(const uint8_t* o,unsigned n){uint64_t v=0;for(unsigned i=0;i<n;i++)v|=uint64_t(o[i])<<(8*i);return v;}
inline int32_t r1lSign24(uint32_t v){v&=0xFFFFFFUL;return (v&0x800000UL)?int32_t(v)-0x1000000L:int32_t(v);}
inline void packAnchorBlock(const AnchorBlock& b,uint8_t* o){
  memset(o,0,R1L_BLOCK_BYTES);o[0]=uint8_t((b.flags&0x0F)|(R1L_BLOCK_VERSION<<4));o[1]=b.prev_status;o[2]=b.resp_ok_bitmap;o[3]=b.ci_hist_count;
  r1lPut(o+4,b.valid_count,2);r1lPut(o+6,b.t_poll_tx&MASK40,5);r1lPut(o+11,b.t_resp_rx&MASK40,5);
  r1lPut(o+16,uint32_t(b.ci_raw),4);r1lPut(o+20,uint32_t(b.ci_prev1)&0xFFFFFFUL,3);r1lPut(o+23,uint32_t(b.ci_prev2)&0xFFFFFFUL,3);
  r1lPut(o+26,b.fp_raw,2);r1lPut(o+28,b.rxpacc,2);r1lPut(o+30,b.cir_pwr,2);r1lPut(o+32,b.f1,2);r1lPut(o+34,b.f2,2);r1lPut(o+36,b.f3,2);
  r1lPut(o+38,b.std_noise,2);r1lPut(o+40,b.sys_status,4);
}
inline bool unpackAnchorBlock(const uint8_t* o,AnchorBlock& b){
  if((o[0]>>4)!=R1L_BLOCK_VERSION)return false;
  b=AnchorBlock{};
  b.flags=uint8_t(o[0]&0x0F);b.prev_status=o[1];b.resp_ok_bitmap=o[2];b.ci_hist_count=o[3];b.valid_count=uint16_t(r1lGet(o+4,2));
  b.t_poll_tx=r1lGet(o+6,5);b.t_resp_rx=r1lGet(o+11,5);b.ci_raw=int32_t(uint32_t(r1lGet(o+16,4)));b.ci_prev1=r1lSign24(uint32_t(r1lGet(o+20,3)));b.ci_prev2=r1lSign24(uint32_t(r1lGet(o+23,3)));
  b.fp_raw=uint16_t(r1lGet(o+26,2));b.rxpacc=uint16_t(r1lGet(o+28,2));b.cir_pwr=uint16_t(r1lGet(o+30,2));b.f1=uint16_t(r1lGet(o+32,2));b.f2=uint16_t(r1lGet(o+34,2));b.f3=uint16_t(r1lGet(o+36,2));
  b.std_noise=uint16_t(r1lGet(o+38,2));b.sys_status=uint32_t(r1lGet(o+40,4));return true;
}
inline unsigned airTsCount(uint8_t type){
  switch(type){
    case RESPONSE:return 2U;
    case POINT_START:return 2U;
    case POINT_ACCEPT:return 3U;
    case POINT_READY:return 3U;
    case POINT_COMMIT:return 3U;
    case ROUTE_END_ACK:return 2U;
    default:return 1U;
  }
}
inline size_t encodeAir(const AirPacket& p,uint8_t* out){
  AirHeader h=p.h;size_t count=airTsCount(h.type);const bool blk=(p.blk_len==R1L_BLOCK_BYTES&&isProbeType(h.type));
  size_t len=sizeof(h)+8*count+(blk?R1L_BLOCK_BYTES:0)+4;
  h.magic=AIR_MAGIC;h.version=blk?2:1;h.bytes=uint16_t(len);memcpy(out,&h,sizeof(h));memcpy(out+sizeof(h),p.ts,8*count);
  if(blk)memcpy(out+sizeof(h)+8*count,p.blk,R1L_BLOCK_BYTES);
  uint32_t crc=crc32(out,len-4);memcpy(out+len-4,&crc,4);return len;
}
inline bool decodeAir(const uint8_t* in,size_t len,AirPacket& p){
  if(len<36||len>AIR_MAX_LEN)return false;memcpy(&p.h,in,sizeof(p.h));
  if(p.h.magic!=AIR_MAGIC||(p.h.version!=1&&p.h.version!=2)||p.h.bytes!=len||p.h.type<1||p.h.type>16)return false;
  const bool blk=(p.h.version==2);if(blk&&!isProbeType(p.h.type))return false;
  size_t count=airTsCount(p.h.type);if(len!=sizeof(AirHeader)+8*count+(blk?R1L_BLOCK_BYTES:0)+4)return false;
  uint32_t crc;memcpy(&crc,in+len-4,4);if(crc!=crc32(in,len-4))return false;
  memset(p.ts,0,sizeof(p.ts));memcpy(p.ts,in+sizeof(AirHeader),8*count);
  p.blk_len=0;if(blk){memcpy(p.blk,in+sizeof(AirHeader)+8*count,R1L_BLOCK_BYTES);p.blk_len=R1L_BLOCK_BYTES;}return true;
}
inline bool ciRange(uint64_t ta1,uint64_t ra1,uint64_t tt,uint64_t tr,int32_t ci_raw,double ci_bias_ppm,double& meters,double& tof,double& ci_corr_ppm){
  double roundTrip=double(delta40(tr,ta1)),reply=double(delta40(tt,ra1));
  if(!(roundTrip>0&&reply>0&&roundTrip<TICK_HZ&&reply<TICK_HZ))return false;
  ci_corr_ppm=double(ci_raw)*CI_PPM_PER_RAW+ci_bias_ppm;
  double q=ci_corr_ppm*1e-6;
  tof=0.5*(roundTrip-reply*(1.0-q));meters=tof*DTU_S*C_AIR_MPS;
  return isfinite(meters)&&isfinite(tof);
}
// R1L: rebuild the Anchor's ci3 exactly (same ring slots, same summation order as pushCiHistory()).
inline double r1lCi3Ppm(const AnchorBlock& b,double ci_bias_ppm,uint32_t smooth_n){
  const double inst=double(b.ci_raw)*CI_PPM_PER_RAW+ci_bias_ppm;
  if(smooth_n<=1||b.valid_count==0)return inst;
  double h[3]{};const uint32_t k=b.valid_count;
  for(uint32_t j=(k>3?k-3:0);j<k;j++){int32_t raw=(j==k-1)?b.ci_raw:((j==k-2)?b.ci_prev1:b.ci_prev2);h[j%3]=double(raw)*CI_PPM_PER_RAW+ci_bias_ppm;}
  const uint32_t n=k<3?k:3;double sum=0;for(uint32_t i=0;i<n;i++)sum+=h[i];return sum/double(n);
}
inline bool abaDelta(const uint64_t tx[3],const uint64_t rx[3],double& ppm,double& dmeters){
  double at=double(delta40(tx[2],tx[0])),bt=double(delta40(tx[1],tx[0]));
  double ar=double(delta40(rx[2],rx[0])),br=double(delta40(rx[1],rx[0]));
  if(! (0<bt&&bt<at&&at<TICK_HZ&&0<br&&br<ar&&ar<TICK_HZ))return false;
  double k=ar/at;ppm=(k-1)*1e6;double residual=br-(bt/at)*ar;
  dmeters=(residual/k)*DTU_S*C_AIR_MPS;return isfinite(dmeters)&&isfinite(ppm);
}
} // namespace bench
