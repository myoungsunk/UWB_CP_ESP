#line 1 "C:\\UWB\\UWB_R04_R1J6_FIELD_FIX_20261010\\R1M_CTRL_V2_RC1\\built\\sketches\\ANCHOR_RF_PLATFORM_V1\\bench_impl.hpp"
#pragma once
#include <Arduino.h>
#include <SPI.h>
#include <DW1000Ng.hpp>
#include <SPIporting.hpp>
#include <esp_timer.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <esp_core_dump.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <atomic>
#include <stdarg.h>
#include <stdlib.h>
#include <ctype.h>
#include <new>
#include <Preferences.h>
#include <mbedtls/gcm.h>
#if BENCH_ANCHOR
#include <WiFi.h>
#include <esp_eap_client.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <ArduinoOTA.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#endif
#include "protocol.hpp"

namespace bench {
constexpr bool IS_ANCHOR = BENCH_ANCHOR != 0;
constexpr uint8_t CS=21,RST=5,IRQ=22,CTRL=4,LS=5;
constexpr unsigned POOL_N=3, MAX_TX=5, MAX_RX=5;
constexpr uint32_t INITIAL_BAUD=921600;
static inline uint64_t nowUs(){return uint64_t(esp_timer_get_time());}
static const char* role(){return IS_ANCHOR?"anchor":"tag";}
static Config cfg;
static uint32_t bootId=0,sessionId=0,peerBoot=0;
static bool hwOK=false,active=false,rxArmed=false;
static uint8_t route=0;
static uint64_t switchAt=0,armedAt=0,armedHw=0,nextStart=0;
static uint32_t txnId=0,remaining=0,lastHeartbeat=0;
static std::atomic<uint32_t> currentBaud{INITIAL_BAUD};
static uint32_t txTimeouts=0,rxTimeouts=0,rxErrors=0,badAir=0,unexpected=0,periodMiss=0;
#if BENCH_ANCHOR
struct R1GBootStats { uint32_t boot_count=0,brownout_count=0,panic_count=0,wdt_count=0,sw_count=0,other_count=0; };
static R1GBootStats gR1GBootStats{};
static esp_reset_reason_t gR1GResetReason=ESP_RST_UNKNOWN;
// R1G1: trace carries uptime; op codes: 1 GETPOINT_STREAM 2 GETPOINT_DONE 3 ROUTE_END 4 WIFI_START
// 5 ROUTE_IDLE_FINALIZE 6 POINT_MEASURE. At boot the previous boot's final trace is snapshotted into
// gR1GPrev so HELLO/RADIOCHECK/BOOT keep reporting it for the whole boot (live trace is overwritten).
struct R1GRtcTrace { uint32_t magic=0,op=0,route=0,point=0,offset=0,uptime_ms=0,boot_id=0; };
RTC_NOINIT_ATTR static R1GRtcTrace gR1GRtcTrace;
static constexpr uint32_t R1G_RTC_MAGIC=0x31475453UL; // R1G1 layout (R1G 0x31475452 is treated as invalid)
static R1GRtcTrace gR1GPrev{};static bool gR1GPrevValid=false;
// R1G2: stack watermarks (bytes still free at the deepest point seen) for loopTask / rf_writer.
static TaskHandle_t gR1GWriterTask=nullptr;static uint32_t gR1GLoopStackMin=0xFFFFFFFFUL,gR1GWriterStackMin=0xFFFFFFFFUL;
static inline uint32_t r1gStackFreeNow(){return uint32_t(uxTaskGetStackHighWaterMark(nullptr));}
static inline void r1gStackSample(){uint32_t v=r1gStackFreeNow();if(v<gR1GLoopStackMin)gR1GLoopStackMin=v;
  if(gR1GWriterTask){uint32_t w=uint32_t(uxTaskGetStackHighWaterMark(gR1GWriterTask));if(w<gR1GWriterStackMin)gR1GWriterStackMin=w;}}
static const char* r1gResetReasonName(esp_reset_reason_t r){switch(r){case ESP_RST_POWERON:return "POWERON";case ESP_RST_SW:return "SW";case ESP_RST_PANIC:return "PANIC";case ESP_RST_INT_WDT:return "INT_WDT";case ESP_RST_TASK_WDT:return "TASK_WDT";case ESP_RST_WDT:return "WDT";case ESP_RST_BROWNOUT:return "BROWNOUT";case ESP_RST_DEEPSLEEP:return "DEEPSLEEP";case ESP_RST_SDIO:return "SDIO";default:return "UNKNOWN";}}
static void r1gBootStatsLoadAndBump(){
  gR1GResetReason=esp_reset_reason();Preferences p;if(p.begin("r1gboot",false)){
    gR1GBootStats.boot_count=p.getUInt("boot",0)+1;gR1GBootStats.brownout_count=p.getUInt("brown",0);gR1GBootStats.panic_count=p.getUInt("panic",0);gR1GBootStats.wdt_count=p.getUInt("wdt",0);gR1GBootStats.sw_count=p.getUInt("sw",0);gR1GBootStats.other_count=p.getUInt("other",0);
    if(gR1GResetReason==ESP_RST_BROWNOUT)++gR1GBootStats.brownout_count;else if(gR1GResetReason==ESP_RST_PANIC)++gR1GBootStats.panic_count;else if(gR1GResetReason==ESP_RST_INT_WDT||gR1GResetReason==ESP_RST_TASK_WDT||gR1GResetReason==ESP_RST_WDT)++gR1GBootStats.wdt_count;else if(gR1GResetReason==ESP_RST_SW)++gR1GBootStats.sw_count;else if(gR1GResetReason!=ESP_RST_POWERON)++gR1GBootStats.other_count;
    p.putUInt("boot",gR1GBootStats.boot_count);p.putUInt("brown",gR1GBootStats.brownout_count);p.putUInt("panic",gR1GBootStats.panic_count);p.putUInt("wdt",gR1GBootStats.wdt_count);p.putUInt("sw",gR1GBootStats.sw_count);p.putUInt("other",gR1GBootStats.other_count);p.end();
  }
  gR1GPrevValid=(gR1GRtcTrace.magic==R1G_RTC_MAGIC);gR1GPrev=gR1GPrevValid?gR1GRtcTrace:R1GRtcTrace{};
  gR1GRtcTrace=R1GRtcTrace{R1G_RTC_MAGIC,0,0,0,0,0,0};
}
static void r1gTrace(uint32_t op,uint32_t routeId=0,uint32_t pointId=0,uint32_t offset=0){gR1GRtcTrace.magic=R1G_RTC_MAGIC;gR1GRtcTrace.op=op;gR1GRtcTrace.route=routeId;gR1GRtcTrace.point=pointId;gR1GRtcTrace.offset=offset;gR1GRtcTrace.uptime_ms=millis();gR1GRtcTrace.boot_id=bootId;}
#endif
// RAWCAL audit: actual DW1000 antenna-delay register values after configuration.
static uint16_t antTxReadback=0,antRxReadback=0;static bool antReadbackOK=false;
static std::atomic<uint32_t> outputDrops{0},resourceDrops{0},sentRecords{0};
static std::atomic<bool> writerBusy{false};
static std::atomic<uint32_t> outputPending{0};
static QueueHandle_t outQ=nullptr,freeQ=nullptr;
static double ciHistPpm[3]{}; static uint8_t ciHistCount=0,ciHistPos=0;
static void resetCiHistory(){ciHistCount=0;ciHistPos=0;for(double &x:ciHistPpm)x=0;}
static double pushCiHistory(double ppm,uint32_t n){
  if(n<=1)return ppm;ciHistPpm[ciHistPos]=ppm;ciHistPos=(ciHistPos+1)%3;if(ciHistCount<3)++ciHistCount;
  double sum=0;for(uint8_t i=0;i<ciHistCount;i++)sum+=ciHistPpm[i];return sum/double(ciHistCount);
}

struct TxObs {
  uint8_t type=0,ctrl=255,ls=255;uint16_t bytes=0;bool ok=false;
  uint64_t planned=0,actual=0,status=0,begin_us=0,done_us=0;
  int32_t lead_start_us=0,lead_arm_us=0,tx_delta_ticks=0;
  uint32_t prepare_us=0,wait_us=0;
  // R7 per-packet Anchor TX power audit. atten_x2 is attenuation from P0 in 0.5 dB units.
  uint8_t power_atten_x2=0,power_octet=0x48;uint32_t power_reg=0x48484848UL,power_read=0;
  bool power_ok=true;uint32_t power_apply_us=0;
};
struct RxObs {
  uint8_t type=0,ctrl=0,ls=0;uint16_t bytes=0;
  int8_t cir_slot=-1;bool received=false,diag=false,settled=false,port_match=false;
  uint64_t hw=0,raw_hw=0,esp_us=0,arm_us=0,switch_us=0,status=0;
  int32_t arm_lead_us=-1000000000,preamble_margin_us=-1000000000;
  uint32_t fetch_us=0,diag_us=0,cir_us=0,compare_us=0,verify_us=0;
  int32_t detect_lag_us=0,observed_arm_to_rmarker_us=0;
  int32_t verify_bad=-1,verify_repeat_bad=-1,ci_raw=0;int64_t lde_adjust_ticks=0;bool lde_done=false;
  uint16_t fp_raw=0,nacc=0,cir_pwr=0,f1=0,f2=0,f3=0,noise=0;
  uint32_t ttcki=0;uint64_t ttcko=0;
  uint64_t tx_planned=0;uint32_t source_boot=0;
  uint8_t raw_air[52]{};
};

// R7: Anchor per-probe manual TX power with 0.5 dB nominal steps.
// Poll stays at the validated P0 reference. A1/A2 share one independently
// controlled setting and B uses another. The host closes the loop from the
// Tag's measured RSL/FP; the UWB A1/B/A2 measurement schedule itself is unchanged.
//
// TX_POWER octet: bits 7:5 coarse DA (3 dB), bits 4:0 fine mixer (0.5 dB).
// This table deliberately preserves every R6 P0..P8 register exactly while
// filling the gaps with the same coarse regions. No setting exceeds P0.
struct R7TxPowerSetting { uint8_t atten_x2; uint8_t gain_x2; uint8_t octet; uint32_t reg; };
static constexpr R7TxPowerSetting R7_TX_SETTINGS[] = {
  { 0, 32, 0x48, 0x48484848UL}, // -0.0 dB, nominal gain 16.0 dB
  { 1, 31, 0x47, 0x47474747UL}, // -0.5 dB, nominal gain 15.5 dB
  { 2, 30, 0x46, 0x46464646UL}, // -1.0 dB, nominal gain 15.0 dB
  { 3, 29, 0x45, 0x45454545UL}, // -1.5 dB, nominal gain 14.5 dB
  { 4, 28, 0x44, 0x44444444UL}, // -2.0 dB, nominal gain 14.0 dB
  { 5, 27, 0x43, 0x43434343UL}, // -2.5 dB, nominal gain 13.5 dB
  { 6, 26, 0x42, 0x42424242UL}, // -3.0 dB, nominal gain 13.0 dB
  { 7, 25, 0x41, 0x41414141UL}, // -3.5 dB, nominal gain 12.5 dB
  { 8, 24, 0x40, 0x40404040UL}, // -4.0 dB, nominal gain 12.0 dB
  { 9, 23, 0x65, 0x65656565UL}, // -4.5 dB, nominal gain 11.5 dB
  {10, 22, 0x64, 0x64646464UL}, // -5.0 dB, nominal gain 11.0 dB
  {11, 21, 0x63, 0x63636363UL}, // -5.5 dB, nominal gain 10.5 dB
  {12, 20, 0x62, 0x62626262UL}, // -6.0 dB, nominal gain 10.0 dB
  {13, 19, 0x61, 0x61616161UL}, // -6.5 dB, nominal gain 9.5 dB
  {14, 18, 0x60, 0x60606060UL}, // -7.0 dB, nominal gain 9.0 dB
  {15, 17, 0x85, 0x85858585UL}, // -7.5 dB, nominal gain 8.5 dB
  {16, 16, 0x84, 0x84848484UL}, // -8.0 dB, nominal gain 8.0 dB
  {17, 15, 0x83, 0x83838383UL}, // -8.5 dB, nominal gain 7.5 dB
  {18, 14, 0x82, 0x82828282UL}, // -9.0 dB, nominal gain 7.0 dB
  {19, 13, 0x81, 0x81818181UL}, // -9.5 dB, nominal gain 6.5 dB
  {20, 12, 0x80, 0x80808080UL}, // -10.0 dB, nominal gain 6.0 dB
  {21, 11, 0xA5, 0xA5A5A5A5UL}, // -10.5 dB, nominal gain 5.5 dB
  {22, 10, 0xA4, 0xA4A4A4A4UL}, // -11.0 dB, nominal gain 5.0 dB
  {23,  9, 0xA3, 0xA3A3A3A3UL}, // -11.5 dB, nominal gain 4.5 dB
  {24,  8, 0xA2, 0xA2A2A2A2UL}, // -12.0 dB, nominal gain 4.0 dB
  {25,  7, 0xA1, 0xA1A1A1A1UL}, // -12.5 dB, nominal gain 3.5 dB
  {26,  6, 0xA0, 0xA0A0A0A0UL}, // -13.0 dB, nominal gain 3.0 dB
  {27,  5, 0xC5, 0xC5C5C5C5UL}, // -13.5 dB, nominal gain 2.5 dB
  {28,  4, 0xC4, 0xC4C4C4C4UL}, // -14.0 dB, nominal gain 2.0 dB
  {29,  3, 0xC3, 0xC3C3C3C3UL}, // -14.5 dB, nominal gain 1.5 dB
  {30,  2, 0xC2, 0xC2C2C2C2UL}, // -15.0 dB, nominal gain 1.0 dB
  {31,  1, 0xC1, 0xC1C1C1C1UL}, // -15.5 dB, nominal gain 0.5 dB
  {32,  0, 0xC0, 0xC0C0C0C0UL}, // -16.0 dB, nominal gain 0.0 dB
};
static constexpr uint8_t R7_TX_SETTING_COUNT = sizeof(R7_TX_SETTINGS)/sizeof(R7_TX_SETTINGS[0]);
static constexpr uint32_t R7_DIS_STXP_MASK = 1UL << 18;

struct R7RegRead32 {
  uint32_t samples[3]{0,0,0};
  uint32_t value=0;
  bool stable=false;
};
struct R7PowerAudit {
  uint8_t atten_x2=0;
  uint32_t expected_reg=0x48484848UL;
  uint32_t tx_power_read=0;
  uint32_t sys_cfg_read=0;
  bool manual_bit=false;
  bool readback_stable=false;
  bool tx_match=false;
  bool valid=false;
};
struct R7RadioHealth {
  uint32_t dev_id=0;
  uint64_t clock_delta=0;
  bool dev_id_ok=false;
  bool clock_ok=false;
  bool ok=false;
};

static constexpr uint8_t R7_POLL_ATTEN_X2=0; // P0 = 0 dB attenuation, 0x48484848
static uint8_t gR7ProbeAAttenX2=0;
static uint8_t gR7ProbeBAttenX2=0;
static bool gR7PowerPlanValid=false;
static R7PowerAudit gR7PollAudit,gR7ProbeAAudit,gR7ProbeBAudit;
static R7RadioHealth gR7Health;
struct Result {
  uint8_t poll_atten_x2=0,probe_a_atten_x2=0,probe_b_atten_x2=0;
  bool tx_manual=false,tx_power_plan_valid=false;
  Config conf{};uint32_t session=0,txn=0,boot=0,peer=0;uint8_t fast_route=ROUTE_A;
  uint16_t ant_tx_requested=0,ant_rx_requested=0,ant_tx_readback=0,ant_rx_readback=0;bool ant_readback_ok=false;
  uint64_t begin_us=0,end_us=0;bool ok=false,fast_valid=false,aba_valid=false;
  char failure[40]{};uint8_t ntx=0,nrx=0,cir_mask=0;
  uint32_t bad_start=0,unexpected_start=0,rx_errors_start=0;
  double range_ci_inst=NAN,range_ci3=NAN,tof_ci_inst=NAN,tof_ci3=NAN;
  double ci_resp_ppm=NAN,ci_corr_ppm=NAN,ci3_corr_ppm=NAN;
  double aba_clock_ppm=NAN,delta_ba=NAN,range_b_inst=NAN,range_b_ci3=NAN;
  int32_t ci_resp_raw=0;uint8_t ci_hist_count=0;
  uint64_t fast[4]{}; // TA1, RA1, TT, TR
  uint64_t aba_tx[3]{},aba_rx[3]{};
  TxObs tx[MAX_TX]{};RxObs rx[MAX_RX]{};
  #if !BENCH_ANCHOR
  uint8_t cir[3][CIR_BYTES]{};
  #endif
};
static Result pool[POOL_N];
#if BENCH_ANCHOR
// R1L probe block (Anchor). Values the Tag cannot measure are copied into B1/A/B2 (or A1/B/A2).
static bool gR1LBlockEnable=true;   // R1LBLOCK 0|1, RAM only (boot = 1)
static bool gR1LStore=true;         // R1LSTORE 0|1, RAM only (boot = 1 = point files as R1J9)
static uint16_t gR1LPushes=0;static int32_t gR1LRaw[3]{};
static uint8_t gR1LPrevStatus=R1L_TXN_NONE,gR1LRespBitmap=0;
static uint32_t gR1LPointBlocks=0,gR1LPointCrc=0xFFFFFFFFUL,gR1LBlocksTotal=0,gR1LBlockTxFail=0;
static void r1lPointReset(){gR1LPrevStatus=R1L_TXN_NONE;gR1LRespBitmap=0;gR1LPointBlocks=0;gR1LPointCrc=0xFFFFFFFFUL;}
static uint32_t r1lPointCrc(){return gR1LPointCrc^0xFFFFFFFFUL;}
// Called right after pushCiHistory(): ciHistCount==1 only on the first push after resetCiHistory().
static void r1lOnCiPush(int32_t raw){if(ciHistCount==1)gR1LPushes=0;gR1LRaw[gR1LPushes%3]=raw;if(gR1LPushes<0xFFFF)++gR1LPushes;}
static void r1lBuild(const Result& r,const RxObs& o,uint8_t* out){
  AnchorBlock b{};b.flags=uint8_t((gR1LStore?1:0)|(r.fast_valid?2:0));b.prev_status=gR1LPrevStatus;b.resp_ok_bitmap=gR1LRespBitmap;
  b.ci_hist_count=r.ci_hist_count;const uint16_t k=(r.conf.ci_smooth_n>1)?gR1LPushes:0;b.valid_count=k;
  b.t_poll_tx=r.fast[0];b.t_resp_rx=r.fast[3];b.ci_raw=r.ci_resp_raw;
  b.ci_prev1=(k>=2)?gR1LRaw[(k-2)%3]:0;b.ci_prev2=(k>=3)?gR1LRaw[(k-3)%3]:0;
  b.fp_raw=o.fp_raw;b.rxpacc=o.nacc;b.cir_pwr=o.cir_pwr;b.f1=o.f1;b.f2=o.f2;b.f3=o.f3;b.std_noise=o.noise;b.sys_status=uint32_t(o.status);
  packAnchorBlock(b,out);
}
static void r1lAfterTxn(const Result& r,bool built,bool sent,const uint8_t* blk){
  uint8_t st=R1L_TXN_OTHER;
  if(r.ok)st=R1L_TXN_OK;
  else if(!r.ntx||!r.tx[0].ok)st=R1L_TXN_POLL_TX_FAIL;
  else if(!r.fast_valid&&strncmp(r.failure,"RX_TIMEOUT_RESP",15)==0)st=R1L_TXN_RESP_TIMEOUT;
  else if(!r.fast_valid)st=R1L_TXN_FAST_INVALID;
  else if(strncmp(r.failure,"TX_",3)==0)st=R1L_TXN_PROBE_TX_FAIL;
  gR1LPrevStatus=st;gR1LRespBitmap=uint8_t((gR1LRespBitmap<<1)|(r.fast_valid?1:0));
  if(sent){gR1LPointCrc=crcUpdate(gR1LPointCrc,blk,R1L_BLOCK_BYTES);++gR1LPointBlocks;++gR1LBlocksTotal;}
  else if(built)++gR1LBlockTxFail;
}
static void remoteCaptureResult(const Result& r);
static void remoteCommandReply(const char* json,size_t n);
static bool remoteCommand(uint32_t req,const char* op,char*& cur);
static void remoteServiceCommands();
static void remoteSetup();
static bool r9Command(uint32_t req,const char* op);
static void remoteLoop();
static void remoteOnRunDone();
static void remoteOnConfigApplied();
static void remoteOnArm();
static void remoteOnStop();
#endif
struct OutMsg {uint8_t kind=1,index=0;uint32_t baud=0;char text[1280]{};};
static void fail(Result& r,const char* why){if(!r.failure[0])snprintf(r.failure,sizeof(r.failure),"%s",why);r.ok=false;}

struct Json {
  char* p;size_t cap,n=0;bool ok=true;
  Json(char* b,size_t c):p(b),cap(c){if(c)p[0]=0;}
  void add(const char* fmt,...) __attribute__((format(printf,2,3))) {if(!ok)return;va_list a;va_start(a,fmt);int w=vsnprintf(p+n,cap-n,fmt,a);va_end(a);
    if(w<0||size_t(w)>=cap-n){ok=false;return;}n+=size_t(w);}
  void num(const char* key,double d,bool comma=true){add("\"%s\":",key);if(isfinite(d))add("%.12g",d);else add("null");if(comma)add(",");}
};
static void appendConfig(Json& j,const Config& c){
  uint32_t vals[CONFIG_FIELDS];memcpy(vals,&c,sizeof(c));j.add("{");
  for(unsigned i=0;i<CONFIG_FIELDS;i++)j.add("%s\"%s\":%lu",i?",":"",CONFIG_NAMES[i],(unsigned long)vals[i]);j.add("}");
}
static void event(const char* fmt,...) __attribute__((format(printf,1,2)));
static void event(const char* fmt,...){
  if(!outQ)return;OutMsg m;va_list a;va_start(a,fmt);int n=vsnprintf(m.text,sizeof(m.text),fmt,a);va_end(a);
  if(n<0||size_t(n)>=sizeof(m.text)){++outputDrops;return;}
#if BENCH_ANCHOR
  // TCP command replies use the loop task's connection directly, never the USB writer.
  remoteCommandReply(m.text,size_t(n));
#endif
  ++outputPending;
  if(xQueueSend(outQ,&m,0)!=pdTRUE){--outputPending;++outputDrops;}
}
static int reserveResult(uint32_t id){
  uint8_t i;if(xQueueReceive(freeQ,&i,0)!=pdTRUE){++resourceDrops;return -1;}
  // A completed slot is not reused until the writer has finished Serial.write.
  // Placement construction avoids a >12 kB Result temporary on the Arduino loop stack.
  new (&pool[i]) Result();auto& r=pool[i];r.conf=cfg;r.session=sessionId;r.txn=id;r.boot=bootId;r.peer=peerBoot;r.fast_route=(cfg.mode==4)?ROUTE_A:fastRouteForTxn(cfg,id);r.begin_us=nowUs();
  r.ant_tx_requested=r.ant_rx_requested=uint16_t(IS_ANCHOR?cfg.ant_anchor:cfg.ant_tag);r.ant_tx_readback=antTxReadback;r.ant_rx_readback=antRxReadback;r.ant_readback_ok=antReadbackOK;
  if(IS_ANCHOR){
    r.poll_atten_x2=R7_POLL_ATTEN_X2;r.probe_a_atten_x2=gR7ProbeAAttenX2;r.probe_b_atten_x2=gR7ProbeBAttenX2;
    r.tx_manual=gR7PollAudit.manual_bit;r.tx_power_plan_valid=gR7PowerPlanValid;
  }
  r.bad_start=badAir;r.unexpected_start=unexpected;r.rx_errors_start=rxErrors;return i;
}
static void submitResult(int i){
  auto& r=pool[i];r.end_us=nowUs();
#if BENCH_ANCHOR
  remoteCaptureResult(r);
#endif
  OutMsg m;m.kind=2;m.index=uint8_t(i);++outputPending;
  if(xQueueSend(outQ,&m,0)!=pdTRUE){--outputPending;++outputDrops;uint8_t b=uint8_t(i);xQueueSend(freeQ,&b,0);}
}
static uint16_t le16(const uint8_t* p){return uint16_t(p[0])|(uint16_t(p[1])<<8);}
static uint32_t le32(const uint8_t* p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
static uint64_t le40(const uint8_t* p){uint64_t x=0;for(unsigned i=0;i<5;i++)x|=uint64_t(p[i])<<(8*i);return x;}

// Same register-header encoding as the project library. Never read TX_BUFFER.
static void regRead(uint8_t reg,uint16_t off,uint8_t* dst,uint16_t n){
  byte h[3];uint8_t k=1;h[0]=reg&0x3F;
  if(off!=0xFFFF){h[0]|=0x40;h[k++]=off&0x7F;if(off>127){h[1]|=0x80;h[k++]=off>>7;}}
  SPIporting::readFromSPI(CS,k,h,n,dst);
}
static void regWrite(uint8_t reg,uint16_t off,uint8_t* src,uint16_t n){
  byte h[3];uint8_t k=1;h[0]=0x80|(reg&0x3F);
  if(off!=0xFFFF){h[0]|=0x40;h[k++]=off&0x7F;if(off>127){h[1]|=0x80;h[k++]=off>>7;}}
  SPIporting::writeToSPI(CS,k,h,n,src);
}
// Read the two physical DW1000 antenna-delay registers. TX_ANTD is register 0x18;
// LDE_RXANTD is subregister 0x1804 of LDE_IF (0x2E). Both are little-endian uint16.
static bool refreshAntennaDelayReadback(){
  uint8_t tx[2]{},rx[2]{};regRead(0x18,0xFFFF,tx,2);regRead(0x2E,0x1804,rx,2);
  antTxReadback=le16(tx);antRxReadback=le16(rx);
  const uint16_t requested=uint16_t(IS_ANCHOR?cfg.ant_anchor:cfg.ant_tag);
  antReadbackOK=(antTxReadback==requested&&antRxReadback==requested);return antReadbackOK;
}
static uint64_t status40(){uint8_t b[5];regRead(0x0F,0xFFFF,b,5);return le40(b);}
static uint64_t sysTime(){return DW1000Ng::getSystemTimestamp()&MASK40;}
static void idleRadio(){DW1000Ng::forceTRxOff();DW1000Ng::clearReceiveStatus();DW1000Ng::clearTransmitStatus();rxArmed=false;}
static void selectRoute(uint8_t r){
  if(IS_ANCHOR)return;
  r&=1U;
  if(r!=route){if(rxArmed){DW1000Ng::forceTRxOff();rxArmed=false;}
    digitalWrite(LS,LOW);digitalWrite(CTRL,r?LOW:HIGH);route=r;switchAt=nowUs();}
  else digitalWrite(LS,LOW);
}
static void waitSwitch(){if(IS_ANCHOR)return;while(nowUs()-switchAt<cfg.switch_us)delayMicroseconds(5);}
static bool ctrlCheckpointArming=false;
static bool ctrlStageCheckpoint(const char* stage);
static void armRx(){
  if(rxArmed)return;waitSwitch();
  DW1000Ng::forceTRxOff();if(ctrlCheckpointArming&&!ctrlStageCheckpoint("AFTER_ARM_TRXOFF"))return;
  DW1000Ng::clearReceiveStatus();if(ctrlCheckpointArming&&!ctrlStageCheckpoint("AFTER_RX_STATUS_CLEAR"))return;
  SPIporting::setSPIspeed(SPIClock::FAST);
  DW1000Ng::startReceive();if(ctrlCheckpointArming&&!ctrlStageCheckpoint("AFTER_RXENAB"))return;rxArmed=true;armedAt=nowUs();armedHw=sysTime();if(ctrlCheckpointArming&&!ctrlStageCheckpoint("AFTER_RX_FLAG_AND_TIME"))return;
}
static R7RegRead32 r7Read32Triple(uint8_t reg){
  R7RegRead32 out;
  SPIporting::setSPIspeed(SPIClock::FAST);
  for(unsigned i=0;i<3;i++){
    uint8_t b[4]{};regRead(reg,0xFFFF,b,4);out.samples[i]=le32(b);delayMicroseconds(4);
  }
  if(out.samples[0]==out.samples[1]||out.samples[0]==out.samples[2]){out.value=out.samples[0];out.stable=true;}
  else if(out.samples[1]==out.samples[2]){out.value=out.samples[1];out.stable=true;}
  else {out.value=out.samples[2];out.stable=false;}
  return out;
}
static bool healthy(){
  SPIporting::setSPIspeed(SPIClock::FAST);
  gR7Health=R7RadioHealth{};
  bool ids=true;uint32_t last=0;
  for(unsigned i=0;i<5;i++){uint8_t b[4]{};regRead(0,0xFFFF,b,4);last=le32(b);if(last!=0xDECA0130UL)ids=false;}
  gR7Health.dev_id=last;gR7Health.dev_id_ok=ids;
  uint64_t a=sysTime();delay(3);uint64_t d=delta40(sysTime(),a);
  gR7Health.clock_delta=d;gR7Health.clock_ok=d>usTicks(1000)&&d<usTicks(30000);
  gR7Health.ok=gR7Health.dev_id_ok&&gR7Health.clock_ok;return gR7Health.ok;
}
static bool r7AuditCurrent(uint8_t atten_x2,R7PowerAudit& audit){
  audit=R7PowerAudit{};audit.atten_x2=atten_x2;
  if(!IS_ANCHOR){audit.valid=true;return true;}
  if(atten_x2>=R7_TX_SETTING_COUNT)return false;
  audit.expected_reg=R7_TX_SETTINGS[atten_x2].reg;
  idleRadio();SPIporting::setSPIspeed(SPIClock::FAST);
  R7RegRead32 sys=r7Read32Triple(0x04);
  R7RegRead32 tx=r7Read32Triple(0x1E);
  audit.sys_cfg_read=sys.value;audit.tx_power_read=tx.value;
  audit.manual_bit=sys.stable&&((sys.value&R7_DIS_STXP_MASK)!=0);
  audit.readback_stable=sys.stable&&tx.stable;
  audit.tx_match=tx.stable&&(tx.value==audit.expected_reg);
  audit.valid=audit.manual_bit&&audit.readback_stable&&audit.tx_match;
  return audit.valid;
}
static bool r7ApplyAtten(uint8_t atten_x2,R7PowerAudit* audit=nullptr){
  if(!IS_ANCHOR)return true;
  if(atten_x2>=R7_TX_SETTING_COUNT)return false;
  idleRadio();SPIporting::setSPIspeed(SPIClock::FAST);
  DW1000Ng::setTXPower((int32_t)R7_TX_SETTINGS[atten_x2].reg);
  delayMicroseconds(40);
  R7PowerAudit local;bool ok=r7AuditCurrent(atten_x2,local);
  if(audit)*audit=local;
  return ok;
}
static bool r7VerifyConfiguredPlan(){
  if(!IS_ANCHOR){gR7PowerPlanValid=true;return true;}
  gR7PowerPlanValid=false;
  bool okPoll=r7ApplyAtten(R7_POLL_ATTEN_X2,&gR7PollAudit);
  bool okA=r7ApplyAtten(gR7ProbeAAttenX2,&gR7ProbeAAudit);
  bool okB=r7ApplyAtten(gR7ProbeBAttenX2,&gR7ProbeBAudit);
  // Always leave the radio at P0 so fast ranging starts from a known state.
  R7PowerAudit restore;bool okRestore=r7ApplyAtten(R7_POLL_ATTEN_X2,&restore);
  if(okRestore)gR7PollAudit=restore;
  gR7PowerPlanValid=okPoll&&okA&&okB&&okRestore;
  return gR7PowerPlanValid;
}
static bool r7SetProbeAttens(uint8_t a_x2,uint8_t b_x2){
  if(!IS_ANCHOR||a_x2>=R7_TX_SETTING_COUNT||b_x2>=R7_TX_SETTING_COUNT)return false;
  gR7ProbeAAttenX2=a_x2;gR7ProbeBAttenX2=b_x2;gR7PowerPlanValid=false;
  return r7VerifyConfiguredPlan();
}
static bool r7PreparePacketPower(uint8_t atten_x2,TxObs& o){
  if(!IS_ANCHOR)return true;
  uint64_t t0=nowUs();R7PowerAudit audit;bool ok=r7ApplyAtten(atten_x2,&audit);
  const auto& p=R7_TX_SETTINGS[atten_x2];
  o.power_atten_x2=atten_x2;o.power_octet=p.octet;o.power_reg=p.reg;o.power_read=audit.tx_power_read;
  o.power_ok=ok;o.power_apply_us=uint32_t(nowUs()-t0);
  if(!ok)gR7PowerPlanValid=false;
  return ok;
}
// ---------------------------------------------------------------------------
// R1F radio-integrity guard.
// Observed (R03 and R04): TX_ANTD=0, LDE_RXANTD=0, SYS_CFG.DIS_STXP=0 while DEV_ID and
// SYS_TIME stay valid and the ESP32 does not reboot -> the DW1000 itself was reset.
// applyRadio()/CONFIG restore those registers but NOT what only initialize() does:
// LDE microcode load, OTP XTAL trim (FS_XTALT) and OTP LDO tune. A CONFIG-"recovered"
// Anchor therefore passed SELFTEST while running untrimmed (tag saw ci_ppm 38.3 vs 5.5).
// Detection uses registers that a reset returns to default; repair is a full hard re-init.
// ---------------------------------------------------------------------------
static bool gRadioApplied=false;static uint8_t gXtalExpected=0;static bool gXtalExpectedValid=false;
static uint32_t gDwResetDetectCount=0,gDwFullInitCount=0,gDwLastDetectMs=0,gDwLastCheckMs=0;static uint8_t gDwLastReasons=0;
static uint8_t readXtalTrimReg(){uint8_t b[1]{};regRead(0x2B,0x0E,b,1);return b[0];}
static void captureXtalExpected(){gXtalExpected=readXtalTrimReg();gXtalExpectedValid=true;}
// reasons: bit0 TX_ANTD, bit1 LDE_RXANTD, bit2 SYS_CFG.DIS_STXP (anchor), bit3 FS_XTALT, bit4 DEV_ID
static uint8_t dwIntegrityReasons(){
  SPIporting::setSPIspeed(SPIClock::FAST);
  uint8_t tx[2]{},rx[2]{},sc[4]{},dev[4]{};regRead(0x18,0xFFFF,tx,2);regRead(0x2E,0x1804,rx,2);regRead(0x04,0xFFFF,sc,4);regRead(0x00,0xFFFF,dev,4);
  const uint16_t want=uint16_t(IS_ANCHOR?cfg.ant_anchor:cfg.ant_tag);uint8_t r=0;
  if(le16(tx)!=want)r|=0x01;if(le16(rx)!=want)r|=0x02;
  if(IS_ANCHOR&&!(le32(sc)&R7_DIS_STXP_MASK))r|=0x04;
  if(gXtalExpectedValid&&readXtalTrimReg()!=gXtalExpected)r|=0x08;
  if(le32(dev)!=0xDECA0130UL)r|=0x10;
  return r;
}
static bool applyRadio();
// Full hard re-init (RSTn pulse + DW1000Ng::initialize + applyRadio). No remote-state guards:
// callers decide when the radio may be taken down.
static bool dwFullInitCore(){
  rxArmed=false;++gDwFullInitCount;
  pinMode(CS,OUTPUT);digitalWrite(CS,HIGH);delay(2);
  digitalWrite(CS,LOW);delay(2);digitalWrite(CS,HIGH);delay(8);
  pinMode(RST,OUTPUT);digitalWrite(RST,LOW);delay(2);
  // DW1000 RSTn is open-drain: release it to input/high impedance, never drive high.
  pinMode(RST,INPUT);delay(10);
  SPI.end();delay(2);
  pinMode(CS,OUTPUT);digitalWrite(CS,HIGH);
  SPI.begin(18,19,23,CS);
  SPIporting::setSlowReadShiftFix(true);
  SPIporting::setSPIspeed(SPIClock::SLOW);
  DW1000Ng::initializeNoInterrupt(CS,RST);
  SPIporting::setSPIspeed(SPIClock::FAST);
  captureXtalExpected();
  return applyRadio()&&(!IS_ANCHOR||gR7PowerPlanValid);
}
// Returns true when the radio is (now) intact. Re-inits at most twice when a reset is confirmed.
static bool ctrlIntegrityStop(uint8_t reasons);
static bool dwEnsureIntegrity(const char* where){
  if(!gRadioApplied)return true;
  uint8_t r=dwIntegrityReasons();if(!r)return true;if(ctrlIntegrityStop(r))return false;
  delay(2);r=dwIntegrityReasons();if(!r)return true; // single SPI glitch is not a reset
  ++gDwResetDetectCount;gDwLastDetectMs=millis();gDwLastReasons=r;
  bool ok=false;for(uint8_t a=0;a<2&&!ok;a++)ok=dwFullInitCore();
  hwOK=ok;
  event("{\"event\":\"R1F_DW_RESET_DETECTED\",\"where\":\"%s\",\"reasons\":%u,\"detect_count\":%lu,\"full_init_count\":%lu,\"repaired\":%s,\"xtal_expected\":%u,\"xtal_now\":%u}",
    where,unsigned(r),(unsigned long)gDwResetDetectCount,(unsigned long)gDwFullInitCount,ok?"true":"false",unsigned(gXtalExpected),unsigned(readXtalTrimReg()));
  return ok;
}
static bool applyRadio(){
  idleRadio();
  device_configuration_t c={false,false,IS_ANCHOR?false:true,true,false,SFDMode::STANDARD_SFD,Channel::CHANNEL_5,
    cfg.rate_kbps==850?DataRate::RATE_850KBPS:DataRate::RATE_6800KBPS,
    PulseFrequency::FREQ_16MHZ,cfg.preamble==256?PreambleLength::LEN_256:PreambleLength::LEN_128,PreambleCode::CODE_3};
  DW1000Ng::applyConfiguration(c);DW1000Ng::setNetworkId(10);DW1000Ng::setDeviceAddress(IS_ANCHOR?5:6);
  DW1000Ng::setAntennaDelay(uint16_t(IS_ANCHOR?cfg.ant_anchor:cfg.ant_tag));
  DW1000Ng::setWait4Response(0);DW1000Ng::setDoubleBuffering(false);
  DW1000Ng::setReceiveFrameWaitTimeoutPeriod(0);SPIporting::setSPIspeed(SPIClock::FAST);
  // RAWCAL: audit the effective TX/RX antenna-delay registers immediately after programming.
  refreshAntennaDelayReadback();
  if(!IS_ANCHOR){pinMode(LS,OUTPUT);pinMode(CTRL,OUTPUT);digitalWrite(LS,LOW);digitalWrite(CTRL,HIGH);route=0;switchAt=nowUs();}
  const bool radioOK=healthy();
  if(IS_ANCHOR){if(radioOK)r7VerifyConfiguredPlan();else gR7PowerPlanValid=false;}
  const bool applied=radioOK&&antReadbackOK;if(applied)gRadioApplied=true;
  return applied;
}
static bool radioRecover();
static bool dwResetOnceReady();
static uint8_t radioRecoveryAttempts=0;
static bool applyRadioWithRecovery(){
  // R1F: a DW1000 that was reset must not be "repaired" by register re-application alone.
  if(gRadioApplied&&dwIntegrityReasons()){
    gDwLastReasons=dwIntegrityReasons();++gDwResetDetectCount;gDwLastDetectMs=millis();
    event("{\"event\":\"R1F_DW_RESET_DETECTED\",\"where\":\"apply_radio\",\"reasons\":%u,\"detect_count\":%lu}",unsigned(gDwLastReasons),(unsigned long)gDwResetDetectCount);
    if(dwFullInitCore())return true;
  }
  if(applyRadio()&&(!IS_ANCHOR||gR7PowerPlanValid))return true;
  return radioRecover();
}

#if !BENCH_ANCHOR
static uint8_t acc[CIR_BYTES+1],compareBuf[CIR_BYTES],repeatBuf[CIR_BYTES];
static uint32_t cirReadOnce(uint8_t* dst,bool block){
  uint64_t start=nowUs();uint8_t old[2],on[2];regRead(0x36,0,old,2);
  on[0]=(old[0]&0xB3)|0x48;on[1]=old[1]|0x80;regWrite(0x36,0,on,2);
  if(!block&&cfg.cir_hz==20000000){regRead(0x25,0xFFFF,acc,CIR_BYTES+1);}
  else {
    SPI.beginTransaction(SPISettings(cfg.cir_hz,MSBFIRST,SPI_MODE0));digitalWrite(CS,LOW);
    SPI.transfer(uint8_t(0x25));
    if(block)SPI.transferBytes(nullptr,acc,CIR_BYTES+1);
    else for(unsigned i=0;i<CIR_BYTES+1;i++)acc[i]=SPI.transfer(uint8_t(0));
    delayMicroseconds(5);digitalWrite(CS,HIGH);SPI.endTransaction();
  }
  regWrite(0x36,0,old,2);memcpy(dst,acc+1,CIR_BYTES);return uint32_t(nowUs()-start);
}
static int firstMismatch(const uint8_t* a,const uint8_t* b){for(unsigned i=0;i<CIR_BYTES;i++)if(a[i]!=b[i])return int(i);return -1;}
static void captureCir(Result& r,RxObs& o,unsigned slot){
  o.cir_slot=int8_t(slot);o.cir_us=cirReadOnce(r.cir[slot],cfg.cir_backend==1);
  if(cfg.verify_cir){
    // Same received accumulator, no new RX between the three reads.
    o.compare_us=cirReadOnce(compareBuf,cfg.cir_backend==0);
    o.verify_us=cirReadOnce(repeatBuf,cfg.cir_backend==1);
    o.verify_bad=firstMismatch(r.cir[slot],compareBuf);o.verify_repeat_bad=firstMismatch(r.cir[slot],repeatBuf);
    if(o.verify_bad>=0||o.verify_repeat_bad>=0)fail(r,"CIR_BYTE_BLOCK_COMPARE");
  }
  r.cir_mask|=uint8_t(1U<<slot);
}
#else
static void captureCir(Result& r,RxObs&,unsigned){fail(r,"ANCHOR_CIR_FORBIDDEN");}
#endif
static void diagnostics(RxObs& o){
  uint64_t start=nowUs();uint8_t fi[4],fq[8],rt[14],ci[3],cki[4],cko[5];
  if(!o.status)o.status=status40();regRead(0x10,0xFFFF,fi,4);regRead(0x12,0xFFFF,fq,8);
  regRead(0x15,0xFFFF,rt,14);regRead(0x27,0x28,ci,3);regRead(0x13,0xFFFF,cki,4);regRead(0x14,0xFFFF,cko,5);
  o.fp_raw=le16(rt+5);o.f1=le16(rt+7);o.noise=le16(fq);o.f2=le16(fq+2);o.f3=le16(fq+4);o.cir_pwr=le16(fq+6);
  o.nacc=uint16_t((fi[2]>>4)|(uint16_t(fi[3])<<4));
  uint32_t cr=uint32_t(ci[0])|(uint32_t(ci[1])<<8)|(uint32_t(ci[2])<<16);cr&=0x1FFFFFUL;
  o.ci_raw=(cr&0x100000UL)?int32_t(cr)-0x200000L:int32_t(cr);
  o.ttcki=le32(cki);o.ttcko=le40(cko);o.diag=true;o.diag_us=uint32_t(nowUs()-start);
}

// R1K6: fixed RAM trace; no serial output inside response receive wait.
struct ResponseStateRow { uint64_t begin,end,state,status; uint32_t dev; uint8_t phase,valid; };
static ResponseStateRow respRows[5]{};
static uint8_t respCount=0;static bool respTrace=false,respBad=false;
static uint64_t respExpected=0,respExpectedEsp=0;
static void responseSnapshot(uint8_t phase){
  if(!respTrace||respCount>=5)return;
  auto& x=respRows[respCount++];x=ResponseStateRow{};x.phase=phase;x.begin=nowUs();
  uint8_t dev[4]{};regRead(0x00,0xFFFF,dev,4);x.dev=le32(dev);x.valid=1;
  if(x.dev!=0xDECA0130UL){x.end=nowUs();respBad=true;return;}
  uint8_t a[5]{},b[5]{};regRead(0x19,0xFFFF,a,5);regRead(0x0F,0xFFFF,b,5);
  x.end=nowUs();x.state=le40(a);x.status=le40(b);x.valid=7;
  if(x.state==MASK40||x.status==MASK40)respBad=true;
}
static void responseTraceOutput(uint32_t id){
  respTrace=false;
  for(unsigned i=0;i<respCount;i++){auto& x=respRows[i];
    event("{\"event\":\"RESPONSE_STATE\",\"candidate\":\"R1K7_RESPONSE_ID\",\"role\":\"anchor\",\"boot\":%lu,\"session\":%lu,\"txn\":%lu,\"seq\":%u,\"phase\":%u,\"begin_us\":%llu,\"end_us\":%llu,\"expected_hw\":%llu,\"expected_esp_us\":%llu,\"dev_id\":%lu,\"valid_mask\":%u,\"state_hex\":\"%010llx\",\"status_hex\":\"%010llx\"}",
      (unsigned long)bootId,(unsigned long)sessionId,(unsigned long)id,i,x.phase,(unsigned long long)x.begin,(unsigned long long)x.end,(unsigned long long)respExpected,(unsigned long long)respExpectedEsp,(unsigned long)x.dev,x.valid,(unsigned long long)x.state,(unsigned long long)x.status);
  }
}

static void finishRx(){DW1000Ng::clearReceiveStatus();rxArmed=false;}
// Returns 1: valid app frame, 0: none, -1: event rejected/error and receiver needs rearming.
#include "ctrl_diag.hpp"
static int pollPacket(AirPacket& p,RxObs& o){
  if(ctrlFault)return -2;CtrlPollScope ctrlScope;
  if(!rxArmed)armRx();
  if(DW1000Ng::isReceiveDone()){
    responseSnapshot(3);
    uint64_t s=nowUs();o.esp_us=s;o.arm_us=armedAt;o.switch_us=switchAt;
    o.bytes=DW1000Ng::getReceivedDataLength();o.hw=DW1000Ng::getReceiveTimestamp()&MASK40;
    o.raw_hw=DW1000Ng::getReceiveTimestampRaw()&MASK40;o.lde_adjust_ticks=signed40(o.hw,o.raw_hw);
    o.status=status40();o.lde_done=(o.status&(1ULL<<10))!=0;
    // Always capture the 21-bit carrier integrator for the one-shot clock/timestamp audit.
    uint8_t ciAudit[3];regRead(0x27,0x28,ciAudit,3);
    uint32_t crAudit=uint32_t(ciAudit[0])|(uint32_t(ciAudit[1])<<8)|(uint32_t(ciAudit[2])<<16);crAudit&=0x1FFFFFUL;
    o.ci_raw=(crAudit&0x100000UL)?int32_t(crAudit)-0x200000L:int32_t(crAudit);
    o.detect_lag_us=int32_t(ticksUs(signed40(sysTime(),o.hw)));
    o.observed_arm_to_rmarker_us=int32_t(ticksUs(signed40(o.hw,armedHw)));
    o.ctrl=IS_ANCHOR?255:uint8_t(digitalRead(CTRL));o.ls=IS_ANCHOR?255:uint8_t(digitalRead(LS));
    o.settled=IS_ANCHOR||(armedAt>=switchAt&&armedAt-switchAt>=cfg.switch_us);
    if(o.bytes<36||o.bytes>52){ctrlNote("RX_LENGTH_REJECT",nullptr,&o);++badAir;finishRx();return -1;}
    DW1000Ng::getReceivedData(o.raw_air,o.bytes);
    if(!decodeAir(o.raw_air,o.bytes,p)){ctrlNote("RX_DECODE_REJECT",nullptr,&o);++badAir;finishRx();return -1;}
    if(p.h.session!=sessionId||p.h.config_id!=configId(cfg)||p.h.boot!=peerBoot){ctrlNote(p.h.session!=sessionId?"RX_SESSION_REJECT":(p.h.config_id!=configId(cfg)?"RX_CONFIG_REJECT":"RX_BOOT_REJECT"),&p,&o);++unexpected;finishRx();return -1;}
    ctrlNote("RX_VALID",&p,&o);o.received=true;o.type=p.h.type;o.tx_planned=p.ts[p.h.type==RESPONSE?1:0];o.source_boot=p.h.boot;
    o.port_match=IS_ANCHOR||(route==probeRouteForType(p.h.type));o.fetch_us=uint32_t(nowUs()-s);
    // Do not clear status until the caller has snapshotted diagnostics/CIR.
    return 1;
  }
  if(ctrlReceiveFailed()){ctrlNote("RX_PHY_FAILED");if(ctrlFault)return -2;responseSnapshot(4);if(respTrace)return -2;++rxErrors;DW1000Ng::clearReceiveFailedStatus();rxArmed=false;return -1;}
  if(DW1000Ng::isReceiveTimeout()){ctrlNote("RX_PHY_TIMEOUT");if(ctrlFault)return -2;responseSnapshot(5);if(respTrace)return -2;++rxErrors;DW1000Ng::clearReceiveTimeoutStatus();rxArmed=false;return -1;}
  return 0;
}
static bool waitPacket(uint8_t typ,uint32_t id,uint64_t expected,AirPacket& p,RxObs& o,Result& r){
  armRx();int64_t lead=signed40(expected,sysTime());
  uint64_t deadline=nowUs()+uint64_t(lead>0?ticksUs(lead):0)+cfg.rx_grace_us;
  int32_t armLead=int32_t(ticksUs(signed40(expected,armedHw)));
  respTrace=IS_ANCHOR&&typ==RESPONSE;respCount=0;respBad=false;
  respExpected=expected;respExpectedEsp=deadline-cfg.rx_grace_us;
  if(respTrace)responseSnapshot(0);
  bool preTaken=false,postTaken=false;

  while(nowUs()<deadline){
    if(respTrace){
      const uint64_t n=nowUs();
      if(!preTaken&&n+500>=respExpectedEsp){responseSnapshot(1);preTaken=true;}
      if(!postTaken&&n>=respExpectedEsp+500){responseSnapshot(2);postTaken=true;}
      if(respBad){idleRadio();fail(r,"RESP_TRACE_SPI_UNTRUSTED");return false;}
    }
    RxObs candidate{};AirPacket q{};int got=pollPacket(q,candidate);
    if(got==-2||respBad){idleRadio();fail(r,respBad?"RESP_TRACE_SPI_UNTRUSTED":"RESP_RX_ERROR_CAPTURED");return false;}

    if(got==1){
      if(q.h.type==typ&&q.h.txn==id){p=q;o=candidate;armLead=int32_t(ticksUs(signed40(expected,armedHw)));o.arm_lead_us=armLead;o.preamble_margin_us=armLead-int32_t(cfg.rx_guard_us);return true;}
      if(respTrace){responseSnapshot(7);idleRadio();fail(r,"RESP_UNEXPECTED_FRAME");return false;}
      ++unexpected;finishRx();armRx();
    } else if(got<0){if(respTrace){responseSnapshot(8);idleRadio();fail(r,"RESP_REJECTED_FRAME");return false;}armRx();}
    delayMicroseconds(10);
  }
  responseSnapshot(6);
  ++rxTimeouts;
  RxObs miss{};miss.type=typ;miss.esp_us=nowUs();miss.arm_us=armedAt;miss.switch_us=switchAt;miss.status=status40();
  miss.arm_lead_us=int32_t(ticksUs(signed40(expected,armedHw)));miss.preamble_margin_us=miss.arm_lead_us-int32_t(cfg.rx_guard_us);
  miss.ctrl=IS_ANCHOR?255:uint8_t(digitalRead(CTRL));miss.ls=IS_ANCHOR?255:uint8_t(digitalRead(LS));
  miss.settled=IS_ANCHOR||(armedAt>=switchAt&&armedAt-switchAt>=cfg.switch_us);
  miss.port_match=IS_ANCHOR||(route==probeRouteForType(typ));
  if(r.nrx<MAX_RX)r.rx[r.nrx++]=miss;
  idleRadio();char reason[40];snprintf(reason,sizeof(reason),"RX_TIMEOUT_%s",typeName(typ));fail(r,reason);return false;
}
static bool sendDelayedPacket(uint8_t typ,uint32_t id,uint64_t requested,TxObs& o,const uint64_t* responseTimes=nullptr,const uint8_t* blk=nullptr){
  o.type=typ;o.begin_us=nowUs();o.ctrl=IS_ANCHOR?255:uint8_t(digitalRead(CTRL));o.ls=IS_ANCHOR?255:uint8_t(digitalRead(LS));
  if(!IS_ANCHOR&&(route!=0||digitalRead(CTRL)!=HIGH||digitalRead(LS)!=LOW))return false;
  waitSwitch();uint16_t ant=uint16_t(IS_ANCHOR?cfg.ant_anchor:cfg.ant_tag);
  uint64_t dx=((requested-ant)&MASK40)&~511ULL;o.planned=(dx+ant)&MASK40;
  AirPacket p{};p.h.type=typ;p.h.session=sessionId;p.h.txn=id;p.h.config_id=configId(cfg);p.h.boot=bootId;
  if(typ==RESPONSE&&responseTimes){p.ts[0]=responseTimes[0];p.ts[1]=o.planned;}else p.ts[0]=o.planned;
  if(blk&&isProbeType(typ)){memcpy(p.blk,blk,R1L_BLOCK_BYTES);p.blk_len=R1L_BLOCK_BYTES;}
  byte data[AIR_MAX_LEN+4]{};o.bytes=uint16_t(encodeAir(p,data));
  idleRadio();uint8_t clr[5]{};clr[3]=0x08;clr[4]=0x04;regWrite(0x0F,0xFFFF,clr,5);
  uint64_t prep=nowUs();o.lead_start_us=int32_t(ticksUs(signed40(dx,sysTime())));
  if(o.lead_start_us<int32_t(cfg.min_lead_us)){o.status=status40();return false;}
  byte d[5];for(unsigned i=0;i<5;i++)d[i]=byte(dx>>(8*i));
  DW1000Ng::setDelayedTRX(d);DW1000Ng::setTransmitData(data,o.bytes);
  o.lead_arm_us=int32_t(ticksUs(signed40(dx,sysTime())));o.prepare_us=uint32_t(nowUs()-prep);
  if(o.lead_arm_us<int32_t(cfg.min_lead_us)){idleRadio();return false;}
  DW1000Ng::startTransmit(TransmitMode::DELAYED);
  uint64_t startWait=nowUs();uint64_t timeout=uint64_t(o.lead_arm_us)+cfg.rx_grace_us+2000;
  while(!DW1000Ng::isTransmitDone()){
    if(nowUs()-startWait>timeout){++txTimeouts;o.status=status40();idleRadio();o.wait_us=uint32_t(nowUs()-startWait);o.done_us=nowUs();return false;}
    delayMicroseconds(10);
  }
  o.actual=DW1000Ng::getTransmitTimestamp()&MASK40;o.status=status40();o.tx_delta_ticks=int32_t(signed40(o.actual,o.planned));
  o.done_us=nowUs();o.wait_us=uint32_t(nowUs()-startWait);DW1000Ng::clearTransmitStatus();
  o.ok=o.tx_delta_ticks==0&&!(o.status&(1ULL<<27))&&!(o.status&(1ULL<<34));return o.ok;
}
static bool sendImmediatePoll(uint32_t id,TxObs& o){
  o.type=POLL;o.begin_us=nowUs();o.ctrl=255;o.ls=255;if(!IS_ANCHOR)return false;
  AirPacket p{};p.h.type=POLL;p.h.session=sessionId;p.h.txn=id;p.h.config_id=configId(cfg);p.h.boot=bootId;p.ts[0]=0;
  byte data[54]{};o.bytes=uint16_t(encodeAir(p,data));idleRadio();DW1000Ng::setTransmitData(data,o.bytes);
  uint64_t startWait=nowUs();DW1000Ng::startTransmit(TransmitMode::IMMEDIATE);
  while(!DW1000Ng::isTransmitDone()){
    if(nowUs()-startWait>cfg.rx_grace_us+5000){++txTimeouts;o.status=status40();idleRadio();o.done_us=nowUs();return false;}
    delayMicroseconds(10);
  }
  o.actual=DW1000Ng::getTransmitTimestamp()&MASK40;o.planned=o.actual;o.tx_delta_ticks=0;o.status=status40();o.done_us=nowUs();o.wait_us=uint32_t(o.done_us-startWait);o.ok=true;DW1000Ng::clearTransmitStatus();return true;
}
static bool sendDelayedStep(Result& r,uint8_t typ,uint64_t target,const uint64_t* rt=nullptr){
  if(r.ntx>=MAX_TX){fail(r,"TX_ARRAY_BOUNDS");return false;}TxObs& o=r.tx[r.ntx++];
  if(sendDelayedPacket(typ,r.txn,target,o,rt))return true;char why[40];snprintf(why,sizeof(why),"TX_FAIL_%s",typeName(typ));fail(r,why);return false;
}
static bool sendDelayedStepAtAtten(Result& r,uint8_t typ,uint64_t target,uint8_t atten_x2,const uint64_t* rt=nullptr,const uint8_t* blk=nullptr){
  if(r.ntx>=MAX_TX){fail(r,"TX_ARRAY_BOUNDS");return false;}TxObs& o=r.tx[r.ntx++];
  if(!r7PreparePacketPower(atten_x2,o)){char why[40];snprintf(why,sizeof(why),"TX_POWER_FAIL_%s",typeName(typ));fail(r,why);return false;}
  if(sendDelayedPacket(typ,r.txn,target,o,rt,blk))return true;char why[40];snprintf(why,sizeof(why),"TX_FAIL_%s",typeName(typ));fail(r,why);return false;
}
static bool sendImmediatePollStep(Result& r){
  if(r.ntx>=MAX_TX){fail(r,"TX_ARRAY_BOUNDS");return false;}TxObs& o=r.tx[r.ntx++];
  if(IS_ANCHOR&&!r7PreparePacketPower(R7_POLL_ATTEN_X2,o)){fail(r,"TX_POWER_FAIL_POLL");return false;}
  if(sendImmediatePoll(r.txn,o))return true;fail(r,"TX_FAIL_POLL");return false;
}
static void storeRx(Result& r,RxObs& o){if(r.nrx<MAX_RX)r.rx[r.nrx++]=o;else fail(r,"RX_ARRAY_BOUNDS");}

static void runAnchor(uint32_t id){
  int slot=reserveResult(id);if(slot<0){event("{\"event\":\"RESOURCE_DROP\",\"role\":\"anchor\",\"session\":%lu,\"txn\":%lu}",(unsigned long)sessionId,(unsigned long)id);return;}
  Result& r=pool[slot];bool success=true;uint64_t burstBase=0;
  if(cfg.mode!=4){
    success=sendImmediatePollStep(r);
    if(success){
      AirPacket response{};RxObs obs{};uint64_t ta1=r.tx[0].actual;
      success=waitPacket(RESPONSE,id,(ta1+usTicks(cfg.reply_us))&MASK40,response,obs,r);
      responseTraceOutput(id);
      if(success){
        if(cfg.diag_fast)diagnostics(obs);
        finishRx();storeRx(r,obs);
        r.fast[0]=ta1;r.fast[1]=response.ts[0];r.fast[2]=response.ts[1];r.fast[3]=obs.hw;r.ci_resp_raw=obs.ci_raw;
        double ciBias=double(cfg.ci_bias_ppb)/1000.0;
        r.fast_valid=ciRange(r.fast[0],r.fast[1],r.fast[2],r.fast[3],r.ci_resp_raw,ciBias,r.range_ci_inst,r.tof_ci_inst,r.ci_corr_ppm);
        r.ci_resp_ppm=double(r.ci_resp_raw)*CI_PPM_PER_RAW;
        if(r.fast_valid){
          r.ci3_corr_ppm=pushCiHistory(r.ci_corr_ppm,cfg.ci_smooth_n);r.ci_hist_count=ciHistCount;
          if(IS_ANCHOR&&cfg.ci_smooth_n>1)r1lOnCiPush(r.ci_resp_raw);
          double q=r.ci3_corr_ppm*1e-6;double ra=double(delta40(r.fast[3],r.fast[0])),db=double(delta40(r.fast[2],r.fast[1]));
          r.tof_ci3=0.5*(ra-db*(1.0-q));r.range_ci3=r.tof_ci3*DTU_S*C_AIR_MPS;
        }else{success=false;fail(r,"INVALID_FAST_INTERVALS");}
        burstBase=(obs.hw+usTicks(cfg.burst_guard_us))&MASK40;
      }
    }
  }
  uint8_t blk[R1L_BLOCK_BYTES]{};bool blkBuilt=false,blkSent=false;
  if(success&&cfg.mode!=3){
    uint64_t base=cfg.mode==4?((sysTime()+usTicks(6000))&MASK40):burstBase;
    const bool bab=isBabTxn(cfg,id);
    const uint8_t probeTypes[3]={uint8_t(bab?B1:A1),uint8_t(bab?PROBE_A:B),uint8_t(bab?B2:A2)};
    const uint8_t probeRoutes[3]={uint8_t(bab?1:0),uint8_t(bab?0:1),uint8_t(bab?1:0)};
#if BENCH_ANCHOR
    // R1L: the same Anchor block rides on all three probes (RESP RX is already complete here).
    if(cfg.mode!=4&&r.fast_valid&&r.nrx&&gR1LBlockEnable){r1lBuild(r,r.rx[0],blk);blkBuilt=true;}
#endif
    for(unsigned i=0;i<3&&success;i++){
      const uint8_t atten=probeRoutes[i]?gR7ProbeBAttenX2:gR7ProbeAAttenX2;
      success=sendDelayedStepAtAtten(r,probeTypes[i],(base+usTicks(uint64_t(i)*cfg.probe_gap_us))&MASK40,atten,nullptr,blkBuilt?blk:nullptr);
      if(i==0&&success&&blkBuilt)blkSent=true;
    }
  }
  r.ok=success&&!r.failure[0];
#if BENCH_ANCHOR
  r1lAfterTxn(r,blkBuilt,blkSent,blk);
#else
  (void)blkSent;
#endif
  submitResult(slot);idleRadio();
}

static void runTag(AirPacket& first,RxObs& firstObs){
  int slot=reserveResult(first.h.txn);if(slot<0){finishRx();idleRadio();selectRoute(0);return;}
  Result& r=pool[slot];bool success=true;AirPacket p=first;RxObs o=firstObs;uint64_t burstExpected=0;
  if(cfg.mode!=4){
    r.fast[1]=o.hw;if(cfg.diag_fast)diagnostics(o);finishRx();storeRx(r,o);
    uint64_t rt[1]={r.fast[1]};success=sendDelayedStep(r,RESPONSE,(r.fast[1]+usTicks(cfg.reply_us))&MASK40,rt);
    if(success){r.fast[2]=r.tx[0].actual;burstExpected=(r.fast[2]+usTicks(cfg.burst_guard_us))&MASK40;}
  }
  if(success&&cfg.mode!=3){
    for(unsigned i=0;i<3&&success;i++){
      if(!(cfg.mode==4&&i==0)){
        o=RxObs{};selectRoute(i==1?1:0);
        uint64_t expected=i==0?burstExpected:(r.aba_rx[0]+usTicks(uint64_t(i)*cfg.probe_gap_us))&MASK40;
        success=waitPacket(uint8_t(A1+i),r.txn,expected,p,o,r);if(!success)break;
      }
      diagnostics(o);r.aba_tx[i]=p.ts[0];r.aba_rx[i]=o.hw;captureCir(r,o,i);finishRx();
      if(!o.port_match||!o.settled||o.ls!=0)fail(r,"PORT_OR_SETTLE_MISMATCH");storeRx(r,o);
      if(i<2){selectRoute(i==0?1:0);armRx();}
    }
    if(success){r.aba_valid=abaDelta(r.aba_tx,r.aba_rx,r.aba_clock_ppm,r.delta_ba);if(!r.aba_valid){success=false;fail(r,"INVALID_ABA_INTERVALS");}}
  }
  r.ok=success&&!r.failure[0];idleRadio();selectRoute(0);submitResult(slot);
}

static double pwrDb(const RxObs& o,bool fp,bool correction){
  if(!o.diag||!o.nacc)return NAN;
  double e=fp?(double(o.f1)*o.f1+double(o.f2)*o.f2+double(o.f3)*o.f3):(double(o.cir_pwr)*131072.0);
  if(e<=0)return NAN;double p=10*log10(e/(double(o.nacc)*o.nacc))-113.77;
  if(correction&&p>-88)p+=(p+88)*2.3334;return p;
}
static void resultJson(Result& r,Json& j){
  j.add("{\"event\":\"RESULT\",\"role\":\"%s\",\"firmware\":\"RF_PLATFORM_V1\",\"boot\":%lu,\"peer_boot\":%lu,\"session\":%lu,\"txn\":%lu,\"config_id\":%lu,\"config\":",role(),(unsigned long)r.boot,(unsigned long)r.peer,(unsigned long)r.session,(unsigned long)r.txn,(unsigned long)configId(r.conf));appendConfig(j,r.conf);
  {const bool bab=isBabTxn(r.conf,r.txn);const char* fast=(r.conf.mode==4)?"NONE":antRouteName(r.fast_route);const char* leg=(r.conf.mode==4)?"NONE":fast;
    j.add(",\"profile\":\"RAWCAL_PAIR_R04_ABDIRECT\",\"fast_route\":\"%s\",\"pair_leg\":\"%s\",\"ant_tx_requested\":%u,\"ant_rx_requested\":%u,\"ant_tx_readback\":%u,\"ant_rx_readback\":%u,\"ant_readback_ok\":%s",fast,leg,r.ant_tx_requested,r.ant_rx_requested,r.ant_tx_readback,r.ant_rx_readback,r.ant_readback_ok?"true":"false");
    j.add(",\"ok\":%s,\"failure\":\"%s\",\"begin_us\":%llu,\"end_us\":%llu,\"rf_work_us\":%llu,\"fast_valid\":%s,\"aba_valid\":%s,\"probe_valid\":%s,\"probe_sequence\":\"%s\",\"logical_sample_id\":%lu,\"cir_mask\":%u,",r.ok?"true":"false",r.failure,(unsigned long long)r.begin_us,(unsigned long long)r.end_us,(unsigned long long)(r.end_us-r.begin_us),r.fast_valid?"true":"false",r.aba_valid?"true":"false",r.aba_valid?"true":"false",bab?"BAB":"ABA",(unsigned long)logicalSampleId(r.conf,r.txn),r.cir_mask);}
  if(IS_ANCHOR)j.add("\"poll_atten_db\":%.1f,\"probe_a_atten_db\":%.1f,\"probe_b_atten_db\":%.1f,\"tx_manual\":%s,\"tx_power_plan_valid\":%s,",double(r.poll_atten_x2)/2.0,double(r.probe_a_atten_x2)/2.0,double(r.probe_b_atten_x2)/2.0,r.tx_manual?"true":"false",r.tx_power_plan_valid?"true":"false");
  j.num("range_ci_inst_m",r.range_ci_inst);j.num("range_ci3_m",r.range_ci3);j.num("tof_ci_inst_dtu",r.tof_ci_inst);j.num("tof_ci3_dtu",r.tof_ci3);
  j.num("ci_resp_ppm",r.ci_resp_ppm);j.num("ci_corrected_ppm",r.ci_corr_ppm);j.num("ci3_corrected_ppm",r.ci3_corr_ppm);
  j.add("\"ci_resp_raw\":%ld,\"ci_hist_count\":%u,",(long)r.ci_resp_raw,r.ci_hist_count);
  {const bool bab=isBabTxn(r.conf,r.txn);const double deltaBMinusA=r.aba_valid?(bab?-r.delta_ba:r.delta_ba):NAN;
    j.num("aba_clock_ppm",r.aba_clock_ppm);j.num("probe_delta_raw_m",r.delta_ba);j.num("delta_b_minus_a_m",deltaBMinusA);j.num("delta_ba_m",deltaBMinusA);j.num("probe_clock_ppm",r.aba_clock_ppm);j.num("probe_delta_m",r.delta_ba);
    j.num("range_direct_m",r.range_ci_inst);j.num("range_direct_ci3_m",r.range_ci3);
    j.num("range_a_direct_m",r.fast_route==ROUTE_A?r.range_ci_inst:NAN);j.num("range_a_direct_ci3_m",r.fast_route==ROUTE_A?r.range_ci3:NAN);
    j.num("range_b_direct_m",r.fast_route==ROUTE_B?r.range_ci_inst:NAN);j.num("range_b_direct_ci3_m",r.fast_route==ROUTE_B?r.range_ci3:NAN);
    if(r.fast_valid&&r.aba_valid&&r.fast_route==ROUTE_A){r.range_b_inst=r.range_ci_inst+deltaBMinusA;r.range_b_ci3=r.range_ci3+deltaBMinusA;}
  }
  j.num("range_b_inst_m",r.range_b_inst);j.num("range_b_ci3_m",r.range_b_ci3);
  j.add("\"fast_timestamps\":[");for(unsigned i=0;i<4;i++)j.add("%s%llu",i?",":"",(unsigned long long)r.fast[i]);j.add("],\"aba_tx\":[");
  for(unsigned i=0;i<3;i++)j.add("%s%llu",i?",":"",(unsigned long long)r.aba_tx[i]);j.add("],\"aba_rx\":[");
  for(unsigned i=0;i<3;i++)j.add("%s%llu",i?",":"",(unsigned long long)r.aba_rx[i]);j.add("],\"tx\":[");
  for(unsigned i=0;i<r.ntx;i++){auto& t=r.tx[i];j.add("%s{\"ctrl\":%u,\"ls\":%u,",i?",":"",t.ctrl,t.ls);j.add("\"type\":\"%s\",\"bytes\":%u,\"ok\":%s,\"planned\":%llu,\"actual\":%llu,\"delta_ticks\":%ld,\"lead_start_us\":%ld,\"lead_arm_us\":%ld,\"prepare_us\":%lu,\"wait_us\":%lu,\"begin_us\":%llu,\"done_us\":%llu,\"sys_status\":%llu,\"power_atten_db\":%.1f,\"power_octet\":%u,\"power_reg\":%lu,\"power_read\":%lu,\"power_ok\":%s,\"power_apply_us\":%lu}",typeName(t.type),t.bytes,t.ok?"true":"false",(unsigned long long)t.planned,(unsigned long long)t.actual,(long)t.tx_delta_ticks,(long)t.lead_start_us,(long)t.lead_arm_us,(unsigned long)t.prepare_us,(unsigned long)t.wait_us,(unsigned long long)t.begin_us,(unsigned long long)t.done_us,(unsigned long long)t.status,double(t.power_atten_x2)/2.0,t.power_octet,(unsigned long)t.power_reg,(unsigned long)t.power_read,t.power_ok?"true":"false",(unsigned long)t.power_apply_us);}
  j.add("],\"rx\":[");
  for(unsigned i=0;i<r.nrx;i++){auto& o=r.rx[i];j.add("%s{\"type\":\"%s\",\"bytes\":%u,\"rx_hw\":%llu,\"rx_raw_hw\":%llu,\"lde_adjust_ticks\":%lld,\"lde_done\":%s,\"tx_planned\":%llu,\"source_boot\":%lu,\"esp_us\":%llu,\"armed_us\":%llu,\"switch_us\":%llu,\"arm_lead_us\":%ld,\"preamble_margin_us\":%ld,\"ctrl\":%u,\"ls\":%u,\"settled\":%s,\"port_match\":%s,\"fetch_us\":%lu,\"diag_us\":%lu,\"cir_us\":%lu,\"compare_us\":%lu,\"verify_us\":%lu,\"verify_bad\":%ld,\"verify_repeat_bad\":%ld,\"cir_slot\":%d,\"diag\":%s,",i?",":"",typeName(o.type),o.bytes,(unsigned long long)o.hw,(unsigned long long)o.raw_hw,(long long)o.lde_adjust_ticks,o.lde_done?"true":"false",(unsigned long long)o.tx_planned,(unsigned long)o.source_boot,(unsigned long long)o.esp_us,(unsigned long long)o.arm_us,(unsigned long long)o.switch_us,(long)o.arm_lead_us,(long)o.preamble_margin_us,o.ctrl,o.ls,o.settled?"true":"false",o.port_match?"true":"false",(unsigned long)o.fetch_us,(unsigned long)o.diag_us,(unsigned long)o.cir_us,(unsigned long)o.compare_us,(unsigned long)o.verify_us,(long)o.verify_bad,(long)o.verify_repeat_bad,int(o.cir_slot),o.diag?"true":"false");
    j.add("\"received\":%s,",o.received?"true":"false");
    j.add("\"detect_lag_us\":%ld,\"observed_arm_to_rmarker_us\":%ld,",(long)o.detect_lag_us,(long)o.observed_arm_to_rmarker_us);
    j.num("rx_dbm",pwrDb(o,false,true));j.num("fp_dbm",pwrDb(o,true,true));j.num("rx_dbm_uncorrected",pwrDb(o,false,false));j.num("fp_dbm_uncorrected",pwrDb(o,true,false));
    j.add("\"fp_raw\":%u,\"fp_index\":%.6f,\"rxpacc\":%u,\"cir_pwr\":%u,\"f1\":%u,\"f2\":%u,\"f3\":%u,\"std_noise\":%u,\"ci_raw\":%ld,\"ci_ppm\":%.9f,\"ttcki\":%lu,\"ttcko\":%llu,\"sys_status\":%llu,\"air_hex\":\"",o.fp_raw,double(o.fp_raw)/64,o.nacc,o.cir_pwr,o.f1,o.f2,o.f3,o.noise,(long)o.ci_raw,double(o.ci_raw)*CI_PPM_PER_RAW,(unsigned long)o.ttcki,(unsigned long long)o.ttcko,(unsigned long long)o.status);
    for(unsigned k=0;k<o.bytes;k++)j.add("%02x",o.raw_air[k]);j.add("\"");
#if !BENCH_ANCHOR
    if(o.cir_slot>=0){auto raw=r.cir[unsigned(o.cir_slot)];uint64_t energy=0;unsigned nonzero=0,saturated=0;
      for(unsigned k=0;k<NSAMP;k++){int32_t re=int16_t(le16(raw+4*k)),im=int16_t(le16(raw+4*k+2));energy+=uint64_t(int64_t(re)*re+int64_t(im)*im);if(re||im)++nonzero;if(re==-32768||re==32767||im==-32768||im==32767)++saturated;}
      j.add(",\"cir_crc\":%lu,\"cir_samples\":%u,\"wire_cir_offset\":%u,\"cir_energy\":%llu,\"cir_nonzero_samples\":%u,\"cir_saturated_samples\":%u",(unsigned long)crc32(raw,CIR_BYTES),NSAMP,unsigned(o.cir_slot)*CIR_BYTES,(unsigned long long)energy,nonzero,saturated);
    }
#endif
    j.add("}");
  }
  j.add("],\"wire_cir_layout\":\"%s;missing_slots_zero_filled_do_not_use\"}",isBabTxn(r.conf,r.txn)?"B1,A,B2":"A1,B,A2");
}
static uint32_t wireSeq=0;
static void writeWire(uint8_t kind,const char* json,size_t n,const uint8_t* raw,size_t rawN){
  WireHeader h{WIRE_MAGIC,1,kind,sizeof(WireHeader),wireSeq++,uint32_t(n),uint32_t(rawN),0};
  uint32_t c=crcUpdate(0xFFFFFFFFUL,&h,offsetof(WireHeader,crc));c=crcUpdate(c,json,n);if(rawN)c=crcUpdate(c,raw,rawN);h.crc=c^0xFFFFFFFFUL;
  Serial.write((const uint8_t*)&h,sizeof(h));Serial.write((const uint8_t*)json,n);if(rawN)Serial.write(raw,rawN);
  // R1J5: USB writer never accesses WiFiClient. TCP owns its own request responses.
  // Unsolicited RF records/heartbeats remain available on USB, not mirrored to TCP.
  ++sentRecords;
}
static void writerTask(void*){
  static char json[10240];OutMsg m;
  for(;;){if(xQueueReceive(outQ,&m,portMAX_DELAY)!=pdTRUE)continue;writerBusy=true;
    if(m.kind==2){Result& r=pool[m.index];Json j(json,sizeof(json));resultJson(r,j);
      if(j.ok){
#if BENCH_ANCHOR
        writeWire(2,json,j.n,nullptr,0);
#else
        size_t n=r.conf.mode!=3?3*CIR_BYTES:0;writeWire(2,json,j.n,n?r.cir[0]:nullptr,n);
#endif
      }
      else {const char* e="{\"event\":\"JSON_OVERFLOW\"}";writeWire(1,e,strlen(e),nullptr,0);++outputDrops;}
      uint8_t i=m.index;xQueueSend(freeQ,&i,portMAX_DELAY);
    }else {writeWire(1,m.text,strlen(m.text),nullptr,0);
      if(m.kind==3){Serial.flush();vTaskDelay(pdMS_TO_TICKS(250));Serial.updateBaudRate(m.baud);currentBaud=m.baud;}
    }
    --outputPending;writerBusy=false;
  }
}

static bool parseU32(char*& cur,uint32_t& v){while(*cur&&isspace((unsigned char)*cur))++cur;if(!isdigit((unsigned char)*cur))return false;char* end;unsigned long long x=strtoull(cur,&end,10);if(end==cur||x>0xFFFFFFFFULL)return false;v=uint32_t(x);cur=end;return true;}
#include "platform_ext.hpp"
static void ack(uint32_t req,bool ok,const char* op,const char* why="OK"){
  event("{\"event\":\"ACK\",\"request\":%lu,\"role\":\"%s\",\"op\":\"%s\",\"ok\":%s,\"reason\":\"%s\",\"boot\":%lu,\"session\":%lu,\"config_id\":%lu}",(unsigned long)req,role(),op,ok?"true":"false",why,(unsigned long)bootId,(unsigned long)sessionId,(unsigned long)configId(cfg));
}
#if BENCH_ANCHOR
static std::atomic<bool> gR04CommandFromTcp{false};
static std::atomic<TaskHandle_t> gR05CommandTask{nullptr};
// OTA window opens only in idle maintenance. Keep all RF mutation commands disabled
// until it closes or the OTA installation finishes.
static bool r04OtaWindowActive();
#endif
static void command(char* line){
  char* cur=line;char op[20];size_t n=0;while(*cur&&!isspace((unsigned char)*cur)){if(n<19)op[n++]=*cur;cur++;}op[n]=0;
  uint32_t req;if(!parseU32(cur,req))return;
#if BENCH_ANCHOR
  if(r04OtaWindowActive() && strcmp(op,"OTASTATUS")!=0 && strcmp(op,"OTAWINDOW")!=0 &&
      strcmp(op,"HELLO")!=0 && strcmp(op,"CAPABILITIES")!=0 &&
      strcmp(op,"ROUTEINFO")!=0 && strcmp(op,"FSINFO")!=0 && strcmp(op,"WIFIINFO")!=0){
    ack(req,false,op,"OTA_WINDOW_ACTIVE");return;
  }
#endif
  if(platformCommand(req,op,cur))return;
  if(ctrlCommand(req,op,cur))return;
  if(strcmp(op,"COREDUMP")==0){
    // R1G2: summary of the last panic stored in the flash coredump partition (survives reboot and
    // app-only reflash). Decode PCs with host/coredump_tool.py and the ELF of the crashing build.
    r1gStackSample();
    if(esp_core_dump_image_check()!=ESP_OK){event("{\"event\":\"ACK\",\"request\":%lu,\"op\":\"COREDUMP\",\"ok\":true,\"role\":\"anchor\",\"present\":false}",(unsigned long)req);return;}
    esp_core_dump_summary_t* s=(esp_core_dump_summary_t*)malloc(sizeof(esp_core_dump_summary_t));
    if(!s||esp_core_dump_get_summary(s)!=ESP_OK){free(s);ack(req,false,op,"SUMMARY_FAILED");return;}
    char bt[16*11+1];int k=0;bt[0]=0;for(uint32_t i=0;i<s->exc_bt_info.depth&&i<16;i++)k+=snprintf(bt+k,sizeof(bt)-k,"%s%08lx",i?",":"",(unsigned long)s->exc_bt_info.bt[i]);
    char reason[96]={0};esp_core_dump_get_panic_reason(reason,sizeof(reason));for(char* c=reason;*c;++c)if(*c=='"'||*c=='\\'||*c<32)*c=' ';
    char task[17]={0};memcpy(task,s->exc_task,16);for(char* c=task;*c;++c)if(*c=='"'||*c=='\\'||*c<32)*c='?';
    char sha[APP_ELF_SHA256_SZ+1]={0};for(size_t i=0;i<sizeof(s->app_elf_sha256)&&s->app_elf_sha256[i];i++){char c=char(s->app_elf_sha256[i]);sha[i]=isxdigit((unsigned char)c)?c:'?';}
    event("{\"event\":\"ACK\",\"request\":%lu,\"op\":\"COREDUMP\",\"ok\":true,\"role\":\"anchor\",\"present\":true,\"task\":\"%s\",\"pc\":\"%08lx\",\"exc_cause\":%lu,\"exc_vaddr\":\"%08lx\",\"bt_depth\":%lu,\"bt_corrupted\":%s,\"bt\":\"%s\",\"panic_reason\":\"%s\",\"app_elf_sha256_prefix\":\"%s\"}",
      (unsigned long)req,task,(unsigned long)s->exc_pc,(unsigned long)s->ex_info.exc_cause,(unsigned long)s->ex_info.exc_vaddr,(unsigned long)s->exc_bt_info.depth,s->exc_bt_info.corrupted?"true":"false",bt,reason,sha);
    free(s);return;
  }
  if(strcmp(op,"COREDUMPCLR")==0){esp_err_t e=esp_core_dump_image_erase();ack(req,e==ESP_OK,op,e==ESP_OK?"OK":"ERASE_FAILED");return;}
  if(strcmp(op,"RESETINFO")==0){r1gStackSample();
    // R1G1: full forensic view. prev_* = final RTC trace of the previous boot (valid only if that boot was R1G1+).
    event("{\"event\":\"ACK\",\"request\":%lu,\"op\":\"RESETINFO\",\"ok\":true,\"role\":\"anchor\",\"diag_rev\":\"R1J\",\"boot\":%lu,\"esp_reset_reason\":%d,\"esp_reset_reason_name\":\"%s\",\"boot_count\":%lu,\"brownout_count\":%lu,\"panic_count\":%lu,\"wdt_count\":%lu,\"sw_reset_count\":%lu,\"other_reset_count\":%lu,\"prev_valid\":%s,\"prev_boot\":%lu,\"prev_op\":%lu,\"prev_route\":%lu,\"prev_point\":%lu,\"prev_offset\":%lu,\"prev_uptime_ms\":%lu,\"cur_op\":%lu,\"cur_offset\":%lu,\"uptime_ms\":%lu,\"heap\":%lu,\"min_heap\":%lu,\"loop_stack_size\":%lu,\"loop_stack_free_min\":%lu,\"writer_stack_free_min\":%lu,\"coredump_present\":%s}",
      (unsigned long)req,(unsigned long)bootId,int(gR1GResetReason),r1gResetReasonName(gR1GResetReason),(unsigned long)gR1GBootStats.boot_count,(unsigned long)gR1GBootStats.brownout_count,(unsigned long)gR1GBootStats.panic_count,(unsigned long)gR1GBootStats.wdt_count,(unsigned long)gR1GBootStats.sw_count,(unsigned long)gR1GBootStats.other_count,
      gR1GPrevValid?"true":"false",(unsigned long)gR1GPrev.boot_id,(unsigned long)gR1GPrev.op,(unsigned long)gR1GPrev.route,(unsigned long)gR1GPrev.point,(unsigned long)gR1GPrev.offset,(unsigned long)gR1GPrev.uptime_ms,
      (unsigned long)gR1GRtcTrace.op,(unsigned long)gR1GRtcTrace.offset,(unsigned long)millis(),(unsigned long)ESP.getFreeHeap(),(unsigned long)ESP.getMinFreeHeap(),(unsigned long)getArduinoLoopTaskStackSize(),(unsigned long)gR1GLoopStackMin,(unsigned long)gR1GWriterStackMin,esp_core_dump_image_check()==ESP_OK?"true":"false");return;
  }
  if(strcmp(op,"RADIOCHECK")==0){
    const uint8_t r=dwIntegrityReasons();const uint8_t x=readXtalTrimReg();refreshAntennaDelayReadback();uint8_t sc[4]{};regRead(0x04,0xFFFF,sc,4);
    event("{\"event\":\"ACK\",\"request\":%lu,\"op\":\"RADIOCHECK\",\"ok\":%s,\"role\":\"anchor\",\"reasons\":%u,\"radio_applied\":%s,\"ant_tx_readback\":%u,\"ant_rx_readback\":%u,\"manual_power\":%s,\"xtal_trim_reg\":%u,\"xtal_trim_expected\":%u,\"xtal_ok\":%s,\"dw_reset_detect_count\":%lu,\"dw_full_init_count\":%lu,\"dw_last_reasons\":%u,\"dw_last_detect_ms\":%lu,\"uptime_ms\":%lu,\"diag_rev\":\"R1J\",\"esp_reset_reason\":%d,\"esp_reset_reason_name\":\"%s\",\"boot_count\":%lu,\"brownout_count\":%lu,\"panic_count\":%lu,\"wdt_count\":%lu,\"sw_reset_count\":%lu,\"last_op\":%lu,\"last_route\":%lu,\"last_point\":%lu,\"last_getpoint_offset\":%lu}",
      (unsigned long)req,r?"false":"true",unsigned(r),gRadioApplied?"true":"false",unsigned(antTxReadback),unsigned(antRxReadback),(le32(sc)&R7_DIS_STXP_MASK)?"true":"false",unsigned(x),unsigned(gXtalExpected),(!gXtalExpectedValid||x==gXtalExpected)?"true":"false",
      (unsigned long)gDwResetDetectCount,(unsigned long)gDwFullInitCount,unsigned(gDwLastReasons),(unsigned long)gDwLastDetectMs,(unsigned long)millis(),int(gR1GResetReason),r1gResetReasonName(gR1GResetReason),(unsigned long)gR1GBootStats.boot_count,(unsigned long)gR1GBootStats.brownout_count,(unsigned long)gR1GBootStats.panic_count,(unsigned long)gR1GBootStats.wdt_count,(unsigned long)gR1GBootStats.sw_count,(unsigned long)gR1GPrev.op,(unsigned long)gR1GPrev.route,(unsigned long)gR1GPrev.point,(unsigned long)gR1GPrev.offset);return;
  }
  if(strcmp(op,"HELLO")==0){
    refreshAntennaDelayReadback();const uint16_t requested=uint16_t(cfg.ant_anchor);
    event("{\"event\":\"ACK\",\"request\":%lu,\"op\":\"HELLO\",\"ok\":true,\"role\":\"anchor\",\"boot\":%lu,\"firmware\":\"RF_PLATFORM_V1\",\"release\":\"R04\",\"profile\":\"RAWCAL_PAIR_R04_ABDIRECT\",\"diag_rev\":\"R1J\",\"hw_ok\":%s,\"radio_health_ok\":%s,\"dev_id\":%lu,\"clock_delta\":%llu,\"tx_power_plan_valid\":%s,\"poll_atten_x2\":%u,\"probe_a_atten_x2\":%u,\"probe_b_atten_x2\":%u,\"poll_power_read\":%lu,\"a_power_read\":%lu,\"b_power_read\":%lu,\"manual_power\":%s,\"ant_tx_requested\":%u,\"ant_rx_requested\":%u,\"ant_tx_readback\":%u,\"ant_rx_readback\":%u,\"ant_readback_ok\":%s,\"baud\":%lu,\"esp_reset_reason\":%d,\"esp_reset_reason_name\":\"%s\",\"boot_count\":%lu,\"brownout_count\":%lu,\"panic_count\":%lu,\"wdt_count\":%lu,\"sw_reset_count\":%lu,\"last_op\":%lu,\"last_route\":%lu,\"last_point\":%lu,\"last_getpoint_offset\":%lu}",
      (unsigned long)req,(unsigned long)bootId,hwOK?"true":"false",gR7Health.ok?"true":"false",(unsigned long)gR7Health.dev_id,(unsigned long long)gR7Health.clock_delta,(!IS_ANCHOR||gR7PowerPlanValid)?"true":"false",R7_POLL_ATTEN_X2,gR7ProbeAAttenX2,gR7ProbeBAttenX2,(unsigned long)gR7PollAudit.tx_power_read,(unsigned long)gR7ProbeAAudit.tx_power_read,(unsigned long)gR7ProbeBAudit.tx_power_read,gR7PollAudit.manual_bit?"true":"false",unsigned(requested),unsigned(requested),unsigned(antTxReadback),unsigned(antRxReadback),antReadbackOK?"true":"false",(unsigned long)currentBaud.load(),int(gR1GResetReason),r1gResetReasonName(gR1GResetReason),(unsigned long)gR1GBootStats.boot_count,(unsigned long)gR1GBootStats.brownout_count,(unsigned long)gR1GBootStats.panic_count,(unsigned long)gR1GBootStats.wdt_count,(unsigned long)gR1GBootStats.sw_count,(unsigned long)gR1GPrev.op,(unsigned long)gR1GPrev.route,(unsigned long)gR1GPrev.point,(unsigned long)gR1GPrev.offset);return;
  }
  if(strcmp(op,"STATUS")==0){
    const bool check=ctrlRecording&&ctrlWindow&&active;if(check&&!ctrlStageCheckpoint("BEFORE_STATUS")){ack(req,false,op,"DIAG_FAULT_LATCHED");return;}
    event("{\"event\":\"ACK\",\"request\":%lu,\"op\":\"STATUS\",\"ok\":true,\"role\":\"%s\",\"boot\":%lu,\"session\":%lu,\"active\":%s,\"hw_ok\":%s,\"radio_health_ok\":%s,\"tx_power_plan_valid\":%s,\"poll_atten_x2\":%u,\"probe_a_atten_x2\":%u,\"probe_b_atten_x2\":%u,\"poll_power_read\":%lu,\"a_power_read\":%lu,\"b_power_read\":%lu}",
      (unsigned long)req,role(),(unsigned long)bootId,(unsigned long)sessionId,active?"true":"false",hwOK?"true":"false",gR7Health.ok?"true":"false",(!IS_ANCHOR||gR7PowerPlanValid)?"true":"false",R7_POLL_ATTEN_X2,gR7ProbeAAttenX2,gR7ProbeBAttenX2,(unsigned long)gR7PollAudit.tx_power_read,(unsigned long)gR7ProbeAAudit.tx_power_read,(unsigned long)gR7ProbeBAudit.tx_power_read);if(check)ctrlStageCheckpoint("AFTER_STATUS");return;
  }
  if(strcmp(op,"STOP")==0){active=false;remaining=0;if(hwOK)idleRadio();if(!IS_ANCHOR)selectRoute(0);
#if BENCH_ANCHOR
    remoteOnStop();
#endif
    ack(req,true,op);return;}
  if(strcmp(op,"PROBEPOWER")==0){
    if(!IS_ANCHOR){ack(req,false,op,"ANCHOR_ONLY");return;}
    if(active){ack(req,false,op,"RUN_ACTIVE");return;}
    const bool radioOK=healthy();hwOK=radioOK;const bool powerOK=radioOK&&r7VerifyConfiguredPlan();
    event("{\"event\":\"ACK\",\"request\":%lu,\"op\":\"PROBEPOWER\",\"ok\":%s,\"reason\":\"%s\",\"tx_power_plan_valid\":%s,\"probe_a_atten_x2\":%u,\"probe_b_atten_x2\":%u,\"poll_reg\":%lu,\"a_reg\":%lu,\"b_reg\":%lu,\"poll_read\":%lu,\"a_read\":%lu,\"b_read\":%lu,\"manual_power\":%s}",
      (unsigned long)req,(radioOK&&powerOK)?"true":"false",!radioOK?"RADIO_HEALTH_FAILED":(!powerOK?"TX_POWER_VERIFY_FAILED":"OK"),gR7PowerPlanValid?"true":"false",gR7ProbeAAttenX2,gR7ProbeBAttenX2,
      (unsigned long)R7_TX_SETTINGS[R7_POLL_ATTEN_X2].reg,(unsigned long)R7_TX_SETTINGS[gR7ProbeAAttenX2].reg,(unsigned long)R7_TX_SETTINGS[gR7ProbeBAttenX2].reg,
      (unsigned long)gR7PollAudit.tx_power_read,(unsigned long)gR7ProbeAAudit.tx_power_read,(unsigned long)gR7ProbeBAudit.tx_power_read,gR7PollAudit.manual_bit?"true":"false");return;
  }
  if(strcmp(op,"PROBEATTEN")==0){
    if(!IS_ANCHOR){ack(req,false,op,"ANCHOR_ONLY");return;}
    if(active){ack(req,false,op,"RUN_ACTIVE");return;}
    uint32_t ax2,bx2;if(!parseU32(cur,ax2)||!parseU32(cur,bx2)||ax2>=R7_TX_SETTING_COUNT||bx2>=R7_TX_SETTING_COUNT){ack(req,false,op,"A_B_ATTEN_X2_0_TO_32");return;}
    while(*cur&&isspace((unsigned char)*cur))++cur;if(*cur){ack(req,false,op,"EXTRA_ARGUMENT");return;}
    const bool radioBefore=healthy();hwOK=radioBefore;bool powerOK=false;
    if(radioBefore)powerOK=r7SetProbeAttens(uint8_t(ax2),uint8_t(bx2));else gR7PowerPlanValid=false;
    const bool radioAfter=healthy();hwOK=radioAfter;if(!radioAfter)gR7PowerPlanValid=false;
    event("{\"event\":\"ACK\",\"request\":%lu,\"op\":\"PROBEATTEN\",\"ok\":%s,\"reason\":\"%s\",\"tx_power_plan_valid\":%s,\"probe_a_atten_x2\":%u,\"probe_b_atten_x2\":%u,\"probe_a_reg\":%lu,\"probe_b_reg\":%lu,\"a_power_read\":%lu,\"b_power_read\":%lu,\"manual_power\":%s,\"radio_health_ok\":%s}",
      (unsigned long)req,(powerOK&&radioAfter)?"true":"false",!radioBefore||!radioAfter?"RADIO_HEALTH_FAILED":(!powerOK?"TX_POWER_VERIFY_FAILED":"OK"),gR7PowerPlanValid?"true":"false",gR7ProbeAAttenX2,gR7ProbeBAttenX2,
      (unsigned long)R7_TX_SETTINGS[gR7ProbeAAttenX2].reg,(unsigned long)R7_TX_SETTINGS[gR7ProbeBAttenX2].reg,(unsigned long)gR7ProbeAAudit.tx_power_read,(unsigned long)gR7ProbeBAudit.tx_power_read,gR7PollAudit.manual_bit?"true":"false",radioAfter?"true":"false");return;
  }
  if(strcmp(op,"TXLEVEL")==0){
    // Compatibility command: P0..P8 maps to 0,2,...16 dB attenuation for BOTH A and B probes. Poll stays P0.
    uint32_t level;if(!IS_ANCHOR){ack(req,false,op,"ANCHOR_ONLY");return;}if(active){ack(req,false,op,"RUN_ACTIVE");return;}
    if(!parseU32(cur,level)||level>8){ack(req,false,op,"LEVEL_0_TO_8_ONLY");return;}uint8_t x2=uint8_t(level*4U);
    const bool radioOK=healthy();hwOK=radioOK;bool ok=radioOK&&r7SetProbeAttens(x2,x2);if(!radioOK)gR7PowerPlanValid=false;
    if(ok)event("{\"event\":\"ACK\",\"request\":%lu,\"op\":\"TXLEVEL\",\"ok\":true,\"tx_level\":%lu,\"probe_a_atten_x2\":%u,\"probe_b_atten_x2\":%u,\"tx_power_plan_valid\":true}",(unsigned long)req,(unsigned long)level,gR7ProbeAAttenX2,gR7ProbeBAttenX2);
    else ack(req,false,op,radioOK?"TX_POWER_VERIFY_FAILED":"RADIO_HEALTH_FAILED");return;
  }
  if(strcmp(op,"CONFIG")==0){
    if(active){ack(req,false,op,"STOP_FIRST");return;}uint32_t sid,vals[CONFIG_FIELDS];Config c;
    if(!parseU32(cur,sid)||!sid){ack(req,false,op,"SESSION_REQUIRED");return;}
    for(unsigned i=0;i<CONFIG_FIELDS;i++)if(!parseU32(cur,vals[i])){ack(req,false,op,"19_VALUES_REQUIRED");return;}
    while(*cur&&isspace((unsigned char)*cur))cur++;if(*cur){ack(req,false,op,"EXTRA_ARGUMENT");return;}
    memcpy(&c,vals,sizeof(c));const char* why;if(!validConfig(c,why)){ack(req,false,op,why);return;}
    cfg=c;sessionId=sid;peerBoot=0;txnId=0;resetCiHistory();const bool radioOK=applyRadioWithRecovery();hwOK=radioOK;
    if(!radioOK){ack(req,false,op,"RADIO_HEALTH_FAILED");return;}
    if(IS_ANCHOR&&!gR7PowerPlanValid){ack(req,false,op,"TX_POWER_PLAN_VERIFY_FAILED");return;}
#if BENCH_ANCHOR
    remoteOnConfigApplied();
#endif
    ack(req,true,op);return;
  }
  if(strcmp(op,"ARM")==0){
    uint32_t sid,peer;if(active||!hwOK){ack(req,false,op,"RADIO_NOT_READY");return;}
    if(IS_ANCHOR&&!gR7PowerPlanValid){ack(req,false,op,"TX_POWER_PLAN_NOT_VERIFIED");return;}
    if(!parseU32(cur,sid)||sid!=sessionId||!parseU32(cur,peer)||!peer){ack(req,false,op,"CONFIG_THEN_ARM_WITH_PEER_BOOT");return;}
    peerBoot=peer;txnId=0;resetCiHistory();active=true;remaining=0;if(!IS_ANCHOR){idleRadio();selectRoute(0);armRx();}
#if BENCH_ANCHOR
    remoteOnArm();
#endif
    ack(req,true,op);return;
  }
  if(strcmp(op,"START")==0){
    uint32_t sid,count;if(!IS_ANCHOR||!active||!peerBoot){ack(req,false,op,"ARM_THEN_START_COUNT");return;}
    if(!gR7PowerPlanValid){active=false;remaining=0;ack(req,false,op,"TX_POWER_PLAN_NOT_VERIFIED");return;}
    if(!parseU32(cur,sid)||sid!=sessionId||!parseU32(cur,count)||count<1||count>1000000){ack(req,false,op,"ARM_THEN_START_COUNT");return;}
    remaining=count;nextStart=nowUs()+200000;ack(req,true,op);return;
  }
  if(strcmp(op,"BAUD")==0){uint32_t baud;if(active||!parseU32(cur,baud)||(baud!=921600&&baud!=2000000&&baud!=3000000)){ack(req,false,op,"STOP_FIRST_BAUD_921600_2000000_3000000");return;}
    OutMsg m;m.kind=3;m.baud=baud;snprintf(m.text,sizeof(m.text),"{\"event\":\"ACK\",\"request\":%lu,\"op\":\"BAUD\",\"ok\":true,\"role\":\"%s\",\"new_baud\":%lu}",(unsigned long)req,role(),(unsigned long)baud);
    ++outputPending;if(xQueueSend(outQ,&m,0)!=pdTRUE){--outputPending;++outputDrops;}return;}
#if BENCH_ANCHOR
  if(r9Command(req,op))return;
  if(remoteCommand(req,op,cur))return;
#endif
  ack(req,false,op,"UNKNOWN_COMMAND");
}

#if BENCH_ANCHOR
#include "remote_v3.hpp"
#include "wifi_reconnect.hpp"
#include "recovery_r1j9.hpp"
#include "uwb_wifi_provision.hpp"
#endif

// Same maintenance guard as recovery, available to single-init diagnostic command.
static bool dwResetOnceReady(){
  if(!IS_ANCHOR||active||remaining||writerBusy.load()||outputPending.load()!=0)return false;
#if BENCH_ANCHOR
  if(remoteWaitingUwb||remoteCaptureEnabled||remotePrepared||remotePreparePending||remoteWaitPending||
     r03SessionActive||r03WaitPending||r03PointMeasuring||remoteState!=RemoteState::MAINTENANCE)return false;
#endif
  return true;
}
static bool radioRecover(){
  radioRecoveryAttempts=0;
  if(!IS_ANCHOR||active||remaining||writerBusy.load()||outputPending.load()!=0)return false;
#if BENCH_ANCHOR
  if(remoteWaitingUwb||remoteCaptureEnabled||remotePrepared||remotePreparePending||remoteWaitPending||
     r03SessionActive||r03WaitPending||r03PointMeasuring||remoteState!=RemoteState::MAINTENANCE)return false;
#endif
  rxArmed=false;
  for(uint8_t attempt=1;attempt<=2;attempt++){
    radioRecoveryAttempts=attempt;
    if(dwFullInitCore()){
      hwOK=true;
      return true;
    }
  }
  hwOK=false;
  gR7PowerPlanValid=false;
  return false;
}

static void serviceCommands(){
  static char buf[768];static unsigned pos=0;static bool overflow=false;
  while(Serial.available()){
    int c=Serial.read();if(c=='\r')continue;if(c=='\n'){if(!overflow){buf[pos]=0;command(buf);}pos=0;overflow=false;continue;}
    if(pos+1<sizeof(buf))buf[pos++]=char(c);else overflow=true;
  }
#if BENCH_ANCHOR
  remoteServiceCommands();
#endif
}
static void setupDevice(){
  Serial.setRxBufferSize(2048);Serial.setTxBufferSize(8192);Serial.begin(INITIAL_BAUD);delay(300);
  outQ=xQueueCreate(32,sizeof(OutMsg));freeQ=xQueueCreate(POOL_N,sizeof(uint8_t));
  if(!outQ||!freeQ){Serial.println("FATAL_QUEUE");while(true)delay(1000);}
  for(uint8_t i=0;i<POOL_N;i++)xQueueSend(freeQ,&i,0);
  if(xTaskCreatePinnedToCore(writerTask,"rf_writer",10000,nullptr,1,&gR1GWriterTask,0)!=pdPASS){Serial.println("FATAL_WRITER");while(true)delay(1000);}
#if BENCH_ANCHOR
  r1gBootStatsLoadAndBump();
#endif
  bootId=esp_random();if(!bootId)bootId=1;
  pinMode(IRQ,INPUT);pinMode(CS,OUTPUT);digitalWrite(CS,HIGH);
  // Anchor wiring: DW1000 RSTn is physically connected to GPIO5 (D5).
  // Use native SPI pins with CS on GPIO21, and enable only the proven 2 MHz
  // read-shift workaround before DW1000Ng performs its low-speed init reads.
  SPI.begin(18,19,23,CS);
  SPIporting::setSlowReadShiftFix(true);
  DW1000Ng::initializeNoInterrupt(CS,RST);
  SPIporting::setSPIspeed(SPIClock::FAST);
  captureXtalExpected();
  platformLoadPersisted(false);
  hwOK=applyRadioWithRecovery();
  event("{\"event\":\"BOOT\",\"role\":\"anchor\",\"boot\":%lu,\"hw_ok\":%s,\"radio_health_ok\":%s,\"firmware\":\"RF_PLATFORM_V1\",\"release\":\"R04\",\"diag_rev\":\"R1J\",\"auto_start\":false,\"esp_reset_reason\":%d,\"esp_reset_reason_name\":\"%s\",\"boot_count\":%lu,\"brownout_count\":%lu,\"panic_count\":%lu,\"wdt_count\":%lu,\"sw_reset_count\":%lu,\"last_op\":%lu,\"last_route\":%lu,\"last_point\":%lu,\"last_getpoint_offset\":%lu,\"dev_id\":%lu,\"clock_delta\":%llu,\"xtal_trim_reg\":%u,\"otp_ldo_tune\":%lu,\"coredump_present\":%s,\"loop_stack_size\":%lu}",
    (unsigned long)bootId,hwOK?"true":"false",gR7Health.ok?"true":"false",int(gR1GResetReason),r1gResetReasonName(gR1GResetReason),(unsigned long)gR1GBootStats.boot_count,(unsigned long)gR1GBootStats.brownout_count,(unsigned long)gR1GBootStats.panic_count,(unsigned long)gR1GBootStats.wdt_count,(unsigned long)gR1GBootStats.sw_count,(unsigned long)gR1GPrev.op,(unsigned long)gR1GPrev.route,(unsigned long)gR1GPrev.point,(unsigned long)gR1GPrev.offset,(unsigned long)gR7Health.dev_id,(unsigned long long)gR7Health.clock_delta,unsigned(readXtalTrimReg()),(unsigned long)DW1000Ng::getInitOtpLdoTune(),esp_core_dump_image_check()==ESP_OK?"true":"false",(unsigned long)getArduinoLoopTaskStackSize());
  remoteSetup();
  r9Setup();

}
static void loopDevice(){
  if(DW1000Ng::resetTraceFault()&&!ctrlFault)ctrlCheckFault("PMSC_RELEASE_READBACK");
  ctrlLoopTick();
#if BENCH_ANCHOR
  ctrlRxSample(r03SessionActive,r03WaitingPoint,r03PointMeasuring);
#endif
  serviceCommands();
#if BENCH_ANCHOR
  wifiReconnectLoop();
  remoteLoop();
  if(!gR04OtaWindow)uwbProvisionAnchorLoop();
  r9RecoveryLoop();
  // R1F: 1 Hz integrity watchdog outside timing-critical RF work (includes the idle
  // gap of an R03 route while waiting for POINT_START/QUERY/ROUTE_END).
  // Note: during an R03 route the Anchor keeps active=true with remaining=0 between points;
  // that idle gap is exactly where a mid-route reset must be caught, so gate on remaining.
  if(!ctrlFault&&!gR04OtaWindow&&hwOK&&!remaining&&!remoteCaptureEnabled&&!remoteWaitingUwb&&!r03PointMeasuring&&!r1mWaitingReady&&!r1mEndReplaySinceMs&&!remotePreparePending&&!remoteWaitPending&&!r03WaitPending&&
     millis()-gDwLastCheckMs>=1000){
    gDwLastCheckMs=millis();r1gStackSample();
    if(!dwEnsureIntegrity("watchdog")){remoteState=RemoteState::FAULT;snprintf(remoteLastError,sizeof(remoteLastError),"R1F_DW_REINIT_FAILED");}
  }
#endif
  if(active&&hwOK){
    if(IS_ANCHOR){
      if(remaining&&nowUs()>=nextStart){
        runAnchor(txnId++);--remaining;uint32_t nextGap=cfg.period_us;nextStart+=nextGap;
        if(nowUs()>=nextStart){++periodMiss;nextStart=nowUs()+nextGap;}
        if(!remaining){active=false;event("{\"event\":\"RUN_DONE\",\"role\":\"anchor\",\"session\":%lu,\"attempts\":%lu}",(unsigned long)sessionId,(unsigned long)txnId);
#if BENCH_ANCHOR
          remoteOnRunDone();
#endif
        }
      }
    }else {
      AirPacket p{};RxObs o{};int got=pollPacket(p,o);
      if(got==1){
        uint8_t first=cfg.mode==4?A1:POLL;
        if(p.h.type==first&&p.h.txn>=txnId){txnId=p.h.txn+1;runTag(p,o);}
        else {++unexpected;finishRx();}
      }
      if(rxArmed&&nowUs()-armedAt>2000000){idleRadio();selectRoute(0);}
    }
  }
  if(!remoteGetpointStreaming&&millis()-lastHeartbeat>=1000){lastHeartbeat=millis();event("{\"event\":\"HEARTBEAT\",\"role\":\"%s\",\"boot\":%lu,\"session\":%lu,\"active\":%s,\"hw_ok\":%s,\"tx_power_plan_valid\":%s,\"probe_a_atten_x2\":%u,\"probe_b_atten_x2\":%u,\"tx_timeout\":%lu,\"rx_timeout\":%lu,\"rx_error\":%lu,\"bad_air\":%lu,\"unexpected\":%lu,\"period_miss\":%lu,\"output_drop\":%lu,\"resource_drop\":%lu,\"out_pending\":%lu,\"heap\":%lu}",role(),(unsigned long)bootId,(unsigned long)sessionId,active?"true":"false",hwOK?"true":"false",(!IS_ANCHOR||gR7PowerPlanValid)?"true":"false",gR7ProbeAAttenX2,gR7ProbeBAttenX2,(unsigned long)txTimeouts,(unsigned long)rxTimeouts,(unsigned long)rxErrors,(unsigned long)badAir,(unsigned long)unexpected,(unsigned long)periodMiss,(unsigned long)outputDrops.load(),(unsigned long)resourceDrops.load(),(unsigned long)outputPending.load(),(unsigned long)ESP.getFreeHeap());}
  // Let the idle task run without a 1 ms sleep in each critical RX polling iteration.
  // Transaction functions are bounded. This yield is outside their timing-critical exchange.
  static uint32_t lastIdleYield=0;
  if(!active || millis()-lastIdleYield>=250){lastIdleYield=millis();delay(1);}else delayMicroseconds(20);
}
} // namespace bench
