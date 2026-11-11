#line 1 "C:\\UWB\\UWB_R04_R1J6_FIELD_FIX_20261010\\R1M_CTRL_V2_RC1\\built\\sketches\\ANCHOR_RF_PLATFORM_V1\\platform_ext.hpp"
#pragma once
// Included from bench_impl.hpp *inside namespace bench* after parseU32().
// No external headers here; Preferences.h is included by bench_impl.hpp.

constexpr uint32_t PLATFORM_PERSIST_MAGIC = 0x31504652UL; // RFP1
constexpr uint16_t PLATFORM_PERSIST_VERSION = 1;
constexpr uint32_t PLATFORM_UNLOCK_XOR = 0xA55A5AA5UL;
static uint64_t platformWriteUnlockUntilUs = 0;

#pragma pack(push,1)
struct PlatformPersistBlob {
  uint32_t magic = PLATFORM_PERSIST_MAGIC;
  uint16_t version = PLATFORM_PERSIST_VERSION;
  uint16_t bytes = sizeof(PlatformPersistBlob);
  Config config{};
  uint8_t probe_a_atten_x2 = 0;
  uint8_t probe_b_atten_x2 = 0;
  uint8_t reserved[6]{};
  uint32_t crc = 0;
};
#pragma pack(pop)

static uint32_t platformBlobCrc(const PlatformPersistBlob& in){
  PlatformPersistBlob b=in;b.crc=0;return crc32(&b,sizeof(b));
}
static void platformAck(uint32_t req,const char* op,bool ok,const char* why="OK"){
  event("{\"event\":\"ACK\",\"request\":%lu,\"role\":\"%s\",\"op\":\"%s\",\"ok\":%s,\"reason\":\"%s\",\"boot\":%lu,\"session\":%lu,\"config_id\":%lu}",
    (unsigned long)req,role(),op,ok?"true":"false",why,(unsigned long)bootId,(unsigned long)sessionId,(unsigned long)configId(cfg));
}
static bool platformReadPersisted(PlatformPersistBlob& b){
  Preferences p;if(!p.begin("rfplat",true))return false;
  size_t n=p.getBytesLength("state");
  if(n!=sizeof(b)){p.end();return false;}
  size_t got=p.getBytes("state",&b,sizeof(b));p.end();
  if(got!=sizeof(b)||b.magic!=PLATFORM_PERSIST_MAGIC||b.version!=PLATFORM_PERSIST_VERSION||b.bytes!=sizeof(b))return false;
  if(b.crc!=platformBlobCrc(b))return false;
  const char* why=nullptr;if(!validConfig(b.config,why))return false;
#if BENCH_ANCHOR
  if(b.probe_a_atten_x2>=R7_TX_SETTING_COUNT||b.probe_b_atten_x2>=R7_TX_SETTING_COUNT)return false;
#endif
  return true;
}
static bool platformSavePersisted(){
  PlatformPersistBlob b;b.config=cfg;
#if BENCH_ANCHOR
  b.probe_a_atten_x2=gR7ProbeAAttenX2;b.probe_b_atten_x2=gR7ProbeBAttenX2;
#endif
  b.crc=platformBlobCrc(b);
  Preferences p;if(!p.begin("rfplat",false))return false;
  size_t n=p.putBytes("state",&b,sizeof(b));p.end();return n==sizeof(b);
}
static bool platformLoadPersisted(bool applyNow){
  PlatformPersistBlob b;if(!platformReadPersisted(b))return false;
  cfg=b.config;
#if BENCH_ANCHOR
  gR7ProbeAAttenX2=b.probe_a_atten_x2;gR7ProbeBAttenX2=b.probe_b_atten_x2;gR7PowerPlanValid=false;
#endif
  if(applyNow){sessionId=0;peerBoot=0;txnId=0;resetCiHistory();hwOK=applyRadio();return hwOK;}
  return true;
}
static bool platformErasePersisted(){Preferences p;if(!p.begin("rfplat",false))return false;bool ok=p.remove("state");p.end();return ok;}
static void platformSetDefaults(){cfg=Config{};
#if BENCH_ANCHOR
  gR7ProbeAAttenX2=0;gR7ProbeBAttenX2=0;gR7PowerPlanValid=false;
#endif
  sessionId=0;peerBoot=0;txnId=0;resetCiHistory();
}
static void platformEmitConfig(uint32_t req,const char* op){
  refreshAntennaDelayReadback();
  char b[1280];Json j(b,sizeof(b));j.add("{\"event\":\"ACK\",\"request\":%lu,\"op\":\"%s\",\"ok\":true,\"role\":\"%s\",\"boot\":%lu,\"profile\":\"RAWCAL_PAIR_R04_ABDIRECT\",\"config_id\":%lu,\"config\":",
    (unsigned long)req,op,role(),(unsigned long)bootId,(unsigned long)configId(cfg));appendConfig(j,cfg);
  const uint16_t reqAnt=uint16_t(IS_ANCHOR?cfg.ant_anchor:cfg.ant_tag);
  j.add(",\"ant_tx_requested\":%u,\"ant_rx_requested\":%u,\"ant_tx_readback\":%u,\"ant_rx_readback\":%u,\"ant_readback_ok\":%s",
    unsigned(reqAnt),unsigned(reqAnt),unsigned(antTxReadback),unsigned(antRxReadback),antReadbackOK?"true":"false");
#if BENCH_ANCHOR
  j.add(",\"probe_a_atten_x2\":%u,\"probe_b_atten_x2\":%u,\"probe_a_atten_db\":%.1f,\"probe_b_atten_db\":%.1f,\"tx_power_plan_valid\":%s",
    gR7ProbeAAttenX2,gR7ProbeBAttenX2,double(gR7ProbeAAttenX2)/2.0,double(gR7ProbeBAttenX2)/2.0,gR7PowerPlanValid?"true":"false");
#endif
  j.add("}");if(j.ok)event("%s",b);else platformAck(req,op,false,"JSON_OVERFLOW");
}
static void platformEmitAntennaDelay(uint32_t req){
  refreshAntennaDelayReadback();const uint16_t requested=uint16_t(IS_ANCHOR?cfg.ant_anchor:cfg.ant_tag);
  event("{\"event\":\"ACK\",\"request\":%lu,\"op\":\"ANTDELAY\",\"ok\":%s,\"role\":\"%s\",\"requested_tx\":%u,\"requested_rx\":%u,\"tx_readback\":%u,\"rx_readback\":%u,\"readback_ok\":%s}",
    (unsigned long)req,antReadbackOK?"true":"false",role(),unsigned(requested),unsigned(requested),unsigned(antTxReadback),unsigned(antRxReadback),antReadbackOK?"true":"false");
}
static void platformEmitCapabilities(uint32_t req){
  event("{\"event\":\"ACK\",\"request\":%lu,\"op\":\"CAPABILITIES\",\"ok\":true,\"firmware\":\"RF_PLATFORM_V1\",\"release\":\"R04\",\"profile\":\"RAWCAL_PAIR_R04_ABDIRECT\",\"diag_rev\":\"R1J\",\"protocol\":5,\"role\":\"%s\",\"modes\":[3,4,5,6,7],\"full_cir_samples\":992,\"full_cir_bytes\":3968,\"probe_cir_types\":[\"A1\",\"B\",\"A2\",\"B1\",\"A\",\"B2\"],\"fast_rx_diagnostics_required\":true,\"antenna_delay_register_readback\":true,\"all_rx_roles_saved_by_host\":true,\"per_probe_tx_power\":%s,\"tx_power_step_db\":0.5,\"tx_power_min_atten_db\":0.0,\"tx_power_max_atten_db\":16.0,\"persistent_config\":true,\"register_read\":true,\"guarded_register_write\":true,\"self_test\":true,\"baud\":[921600,2000000,3000000],\"route_ready_ping\":true,\"transport_api\":2,\"route_session\":true,\"mode5_legacy_aba\":true,\"mode6_paired_aba_bab\":true,\"mode7_bab_standalone\":true,\"ab_direct_ranging\":true,\"delta_b_minus_a\":true,\"pair_gap_us\":300000,\"mode6_max_logical_samples_per_point\":32,\"power_diag\":true,\"route_point_max_records\":64,\"point_commit_query\":true,\"timesync\":true,\"getpoint_resume\":true,\"wifi_measurement_dependency\":false,\"probe_block\":1,\"probe_block_bytes\":44,\"anchor_store_flag\":true,\"ota\":%s,\"auto_start\":false}",
    (unsigned long)req,role(),IS_ANCHOR?"true":"false",IS_ANCHOR?"true":"false");
}
static void platformEmitCounters(uint32_t req){
  event("{\"event\":\"ACK\",\"request\":%lu,\"op\":\"COUNTERS\",\"ok\":true,\"tx_timeout\":%lu,\"rx_timeout\":%lu,\"rx_error\":%lu,\"bad_air\":%lu,\"unexpected\":%lu,\"period_miss\":%lu,\"output_drop\":%lu,\"resource_drop\":%lu,\"sent_records\":%lu,\"out_pending\":%lu,\"heap\":%lu}",
    (unsigned long)req,(unsigned long)txTimeouts,(unsigned long)rxTimeouts,(unsigned long)rxErrors,(unsigned long)badAir,(unsigned long)unexpected,(unsigned long)periodMiss,
    (unsigned long)outputDrops.load(),(unsigned long)resourceDrops.load(),(unsigned long)sentRecords.load(),(unsigned long)outputPending.load(),(unsigned long)ESP.getFreeHeap());
}
static void platformResetCounters(){txTimeouts=rxTimeouts=rxErrors=badAir=unexpected=periodMiss=0;outputDrops=0;resourceDrops=0;sentRecords=0;}
static void platformSelfTest(uint32_t req){
  bool radio=healthy();hwOK=radio;
#if BENCH_ANCHOR
  bool power=radio&&r7VerifyConfiguredPlan();
  bool clockOK=gR7Health.clock_ok;uint64_t clockDelta=gR7Health.clock_delta;
#else
  bool power=true;
  uint64_t ta=sysTime();delay(3);uint64_t clockDelta=delta40(sysTime(),ta);bool clockOK=clockDelta>usTicks(1000)&&clockDelta<usTicks(30000);
#endif
  uint8_t dev[4]{};regRead(0x00,0xFFFF,dev,4);uint32_t devid=le32(dev);refreshAntennaDelayReadback();hwOK=radio&&antReadbackOK;
#if BENCH_ANCHOR
  // R1F: XTAL trim must equal the value written by initialize(); a CONFIG-only recovery fails here.
  const uint8_t xtalNow=readXtalTrimReg();const bool xtalOK=!gXtalExpectedValid||xtalNow==gXtalExpected;
#else
  const uint8_t xtalNow=0;const bool xtalOK=true;
#endif
  if(!xtalOK)hwOK=false;
  const uint16_t requested=uint16_t(IS_ANCHOR?cfg.ant_anchor:cfg.ant_tag);
  event("{\"event\":\"ACK\",\"request\":%lu,\"op\":\"SELFTEST\",\"ok\":%s,\"role\":\"%s\",\"dev_id\":%lu,\"dev_id_ok\":%s,\"sys_clock_ok\":%s,\"clock_delta\":%llu,\"power_plan_ok\":%s,\"ant_tx_requested\":%u,\"ant_rx_requested\":%u,\"ant_tx_readback\":%u,\"ant_rx_readback\":%u,\"ant_readback_ok\":%s,\"config_valid\":true,\"xtal_trim_reg\":%u,\"xtal_ok\":%s,\"heap\":%lu}",
    (unsigned long)req,(radio&&power&&clockOK&&antReadbackOK&&xtalOK)?"true":"false",role(),(unsigned long)devid,(devid==0xDECA0130UL)?"true":"false",clockOK?"true":"false",(unsigned long long)clockDelta,power?"true":"false",unsigned(requested),unsigned(requested),unsigned(antTxReadback),unsigned(antRxReadback),antReadbackOK?"true":"false",unsigned(xtalNow),xtalOK?"true":"false",(unsigned long)ESP.getFreeHeap());
}
static bool platformCommand(uint32_t req,const char* op,char*& cur){
  if(strcmp(op,"CAPABILITIES")==0||strcmp(op,"CAPS")==0){platformEmitCapabilities(req);return true;}
  if(strcmp(op,"GETCONFIG")==0){platformEmitConfig(req,op);return true;}
  if(strcmp(op,"ANTDELAY")==0){if(active){platformAck(req,op,false,"STOP_FIRST");return true;}platformEmitAntennaDelay(req);return true;}
  if(strcmp(op,"COUNTERS")==0){platformEmitCounters(req);return true;}
  if(strcmp(op,"RESETCOUNTERS")==0){if(active){platformAck(req,op,false,"STOP_FIRST");return true;}platformResetCounters();platformAck(req,op,true);return true;}
  if(strcmp(op,"SELFTEST")==0){if(active){platformAck(req,op,false,"STOP_FIRST");return true;}platformSelfTest(req);return true;}
  // E1 diagnostic: exactly one full DW reset/init, never radioRecover retries.
  if(strcmp(op,"DWRESETONCE")==0){
    if(!IS_ANCHOR){platformAck(req,op,false,"ANCHOR_ONLY");return true;}
    if(active||remaining||writerBusy.load()||outputPending.load()!=0){platformAck(req,op,false,"NOT_QUIESCENT");return true;}
    if(!dwResetOnceReady()){platformAck(req,op,false,"NOT_MAINTENANCE");return true;}
    const uint32_t countBefore=gDwFullInitCount;
    const bool ok=dwFullInitCore();
    hwOK=ok;
    if(!ok)gR7PowerPlanValid=false;
    // No further SPI reads here, including after failure.
    event("{\"event\":\"ACK\",\"request\":%lu,\"role\":\"%s\",\"op\":\"DWRESETONCE\",\"ok\":%s,\"boot\":%lu,\"attempts\":1,\"full_init_before\":%lu,\"full_init_after\":%lu,\"hw_ok\":%s,\"ant_readback_ok\":%s,\"tx_power_plan_valid\":%s}",
      (unsigned long)req,role(),ok?"true":"false",(unsigned long)bootId,(unsigned long)countBefore,(unsigned long)gDwFullInitCount,hwOK?"true":"false",antReadbackOK?"true":"false",gR7PowerPlanValid?"true":"false");
    return true;
  }
  if(strcmp(op,"RADIORECOVER")==0){
    if(!IS_ANCHOR){platformAck(req,op,false,"ANCHOR_ONLY");return true;}
    if(active){platformAck(req,op,false,"STOP_FIRST");return true;}
    bool ok=radioRecover();uint8_t dev[4]{};regRead(0x00,0xFFFF,dev,4);uint32_t devId=le32(dev);
    event("{\"event\":\"ACK\",\"request\":%lu,\"role\":\"%s\",\"op\":\"RADIORECOVER\",\"ok\":%s,\"reason\":\"%s\",\"attempts\":%u,\"dev_id\":%lu,\"hw_ok\":%s,\"radio_health_ok\":%s,\"tx_power_plan_valid\":%s,\"ant_readback_ok\":%s}",
      (unsigned long)req,role(),ok?"true":"false",ok?"OK":"RECOVERY_BLOCKED_OR_FAILED",unsigned(radioRecoveryAttempts),(unsigned long)devId,hwOK?"true":"false",gR7Health.ok?"true":"false",gR7PowerPlanValid?"true":"false",antReadbackOK?"true":"false");return true;
  }
  if(strcmp(op,"SAVE")==0){if(active){platformAck(req,op,false,"STOP_FIRST");return true;}bool ok=platformSavePersisted();platformAck(req,op,ok,ok?"OK":"NVS_WRITE_FAILED");return true;}
  if(strcmp(op,"LOAD")==0){if(active){platformAck(req,op,false,"STOP_FIRST");return true;}bool ok=platformLoadPersisted(true);platformAck(req,op,ok,ok?"OK":"NVS_STATE_INVALID_OR_RADIO_FAILED");return true;}
  if(strcmp(op,"DEFAULTS")==0){if(active){platformAck(req,op,false,"STOP_FIRST");return true;}platformSetDefaults();hwOK=applyRadio();platformAck(req,op,hwOK,hwOK?"OK":"RADIO_HEALTH_FAILED");return true;}
  if(strcmp(op,"ERASESTATE")==0){if(active){platformAck(req,op,false,"STOP_FIRST");return true;}bool ok=platformErasePersisted();platformAck(req,op,ok,ok?"OK":"NVS_REMOVE_FAILED");return true;}
  if(strcmp(op,"READREG")==0){
    if(active){platformAck(req,op,false,"STOP_FIRST");return true;}uint32_t reg,off,len;if(!parseU32(cur,reg)||!parseU32(cur,off)||!parseU32(cur,len)||reg>63||off>65535||len<1||len>32){platformAck(req,op,false,"REG_0_63_OFF_0_65535_LEN_1_32");return true;}
    uint8_t data[32]{};regRead(uint8_t(reg),uint16_t(off),data,uint16_t(len));char hex[65];for(uint32_t i=0;i<len;i++)snprintf(hex+2*i,3,"%02x",data[i]);
    event("{\"event\":\"ACK\",\"request\":%lu,\"op\":\"READREG\",\"ok\":true,\"reg\":%lu,\"off\":%lu,\"len\":%lu,\"hex\":\"%s\"}",(unsigned long)req,(unsigned long)reg,(unsigned long)off,(unsigned long)len,hex);return true;
  }
  if(strcmp(op,"UNLOCKREG")==0){uint32_t token;if(active||!parseU32(cur,token)){platformAck(req,op,false,"STOP_FIRST_TOKEN_REQUIRED");return true;}uint32_t expected=bootId^PLATFORM_UNLOCK_XOR;if(token!=expected){platformAck(req,op,false,"BAD_TOKEN");return true;}platformWriteUnlockUntilUs=nowUs()+30000000ULL;platformAck(req,op,true);return true;}
  if(strcmp(op,"WRITEREG")==0){
    if(active){platformAck(req,op,false,"STOP_FIRST");return true;}if(nowUs()>platformWriteUnlockUntilUs){platformAck(req,op,false,"UNLOCKREG_FIRST");return true;}
    uint32_t reg,off,len;if(!parseU32(cur,reg)||!parseU32(cur,off)||!parseU32(cur,len)||reg>63||off>65535||len<1||len>8){platformAck(req,op,false,"REG_0_63_OFF_0_65535_LEN_1_8");return true;}
    if(reg==0x09||reg==0x25){platformAck(req,op,false,"TX_BUFFER_AND_ACC_MEM_WRITE_BLOCKED");return true;}uint8_t data[8]{};for(uint32_t i=0;i<len;i++){uint32_t v;if(!parseU32(cur,v)||v>255){platformAck(req,op,false,"BYTE_VALUES_0_255_REQUIRED");return true;}data[i]=uint8_t(v);}regWrite(uint8_t(reg),uint16_t(off),data,uint16_t(len));platformWriteUnlockUntilUs=0;platformAck(req,op,true);return true;
  }
  if(strcmp(op,"REBOOT")==0){if(active){platformAck(req,op,false,"STOP_FIRST");return true;}platformAck(req,op,true);delay(100);Serial.flush();ESP.restart();return true;}
  return false;
}
