#line 1 "C:\\UWB\\UWB_R04_R1J6_FIELD_FIX_20261010\\R1M_CTRL_V2_RC1\\built\\sketches\\ANCHOR_RF_PLATFORM_V1\\remote_v3.hpp"
#pragma once

// RF_PLATFORM_V1_RAWCAL TEST3 remote-control layer.
// This header is included inside namespace bench, after command()/writeWire()/resultJson().
// It intentionally does NOT alter the validated RF transaction schedule.

constexpr uint16_t REMOTE_TCP_PORT = 29310;
constexpr uint32_t REMOTE_NET_MAGIC = 0x33574652UL; // RFW3
constexpr uint16_t REMOTE_NET_VERSION = 2;
constexpr uint32_t REMOTE_MAX_TRANSACTIONS = 256;
constexpr uint32_t REMOTE_WIFI_CONNECT_TIMEOUT_MS = 45000;
constexpr uint32_t REMOTE_SESSION_START_TIMEOUT_MS = 30000;

enum class RemoteState : uint8_t {
  MAINTENANCE=0,
  CONFIGURED=1,
  PREPARED=2,
  ARMED=3,
  WAIT_UWB=4,
  MEASURE=5,
  TRANSFER=6,
  ROUTE_PREPARED=7,
  ROUTE_WAIT_POINT=8,
  POINT_MEASURE=9,
  ROUTE_FINALIZE=10,
  FAULT=11
};
static const char* remoteStateName(RemoteState s){
  switch(s){
    case RemoteState::MAINTENANCE:return "MAINTENANCE";
    case RemoteState::CONFIGURED:return "CONFIGURED";
    case RemoteState::PREPARED:return "PREPARED";
    case RemoteState::ARMED:return "ARMED";
    case RemoteState::WAIT_UWB:return "WAIT_UWB";
    case RemoteState::MEASURE:return "MEASURE";
    case RemoteState::TRANSFER:return "TRANSFER";
    case RemoteState::ROUTE_PREPARED:return "ROUTE_PREPARED";
    case RemoteState::ROUTE_WAIT_POINT:return "ROUTE_WAIT_POINT";
    case RemoteState::POINT_MEASURE:return "POINT_MEASURE";
    case RemoteState::ROUTE_FINALIZE:return "ROUTE_FINALIZE";
    default:return "FAULT";
  }
}

#pragma pack(push,1)
struct RemoteNetConfig {
  uint32_t magic=REMOTE_NET_MAGIC;
  uint16_t version=REMOTE_NET_VERSION;
  uint16_t bytes=sizeof(RemoteNetConfig);
  char ssid[33]{};
  char password[65]{};
  char hostname[33]="rf-anchor";
  bool enterprise=false;
  char identity[65]{};
  char username[65]{};
  uint32_t crc=0;
};
struct RemoteRxCompact {
  uint16_t bytes=0;
  uint8_t received=0,diag=0,lde_done=0;
  // Preserve the exact RESPONSE air bytes received by the Anchor.
  // Do not reconstruct these during GETRUN: TEST3 keeps the same raw-data contract as TEST2E.
  uint8_t raw_air[52]{};
  uint64_t hw=0,raw_hw=0,esp_us=0,arm_us=0,switch_us=0,status=0;
  int32_t arm_lead_us=-1000000000,preamble_margin_us=-1000000000;
  uint32_t fetch_us=0,diag_us=0;
  int32_t detect_lag_us=0,observed_arm_to_rmarker_us=0,ci_raw=0;
  int64_t lde_adjust_ticks=0;
  uint16_t fp_raw=0,nacc=0,cir_pwr=0,f1=0,f2=0,f3=0,noise=0;
  uint32_t ttcki=0;
  uint64_t ttcko=0;
};
struct RemoteAnchorRecord {
  uint32_t txn=0;
  uint64_t begin_us=0,end_us=0;
  uint8_t ok=0,fast_valid=0,ntx=0,nrx=0;
  char failure[40]{};
  uint64_t fast[4]{};
  TxObs tx[4]{}; // Anchor Mode5: POLL,A1,B,A2. Full TxObs preserves every emitted TX diagnostic.
  RemoteRxCompact response{};
};
#pragma pack(pop)
#include "remote_compact.hpp"
#pragma pack(push,1)
struct RemoteRunFileHeader {
  uint32_t magic=0x33524652UL; // RFR3
  uint16_t version=3;
  uint16_t record_bytes=REMOTE_COMPACT_BYTES;
  uint16_t record_schema_version=2,block_records=16;
  uint32_t block_crc32[16]{};
  uint32_t count=0,session=0,config_id=0,records_crc32=0;
  uint32_t wifi_measure_violation_count=0;
  RemoteRxCompact session_start_rx{};
};
#pragma pack(pop)

static_assert(sizeof(RemoteAnchorRecord)<700,"remote record unexpectedly large");

static RemoteNetConfig remoteNet{};
static WiFiServer remoteServer(REMOTE_TCP_PORT);
static WiFiClient remoteClient;
static SemaphoreHandle_t remoteTxMutex=nullptr;
static volatile bool remoteGetpointStreaming=false; // R1I: direct GETPOINT owns TCP writer window
static uint32_t remoteTcpSeq=0,remoteWaitDeadlineMs=0;
// R1J5: all remoteClient access is owned by loopTask. USB writer only writes UART.
static uint32_t remoteTcpConnections=0,remoteTcpRequests=0,remoteTcpFramesOk=0,remoteTcpFramesFailed=0;
static uint32_t remoteTcpLastRxMs=0,remoteTcpLastReq=0,remoteTcpLastWriteMs=0;
static char remoteTcpLastOp[32]="NONE";
static bool remoteCyclePending=false;static uint32_t remoteCycleDueMs=0;
static bool remoteNetValid=false,remoteWifiStarting=false,remoteWifiReady=false;
static uint32_t remoteWifiStartMs=0;
static char remoteLine[512]{};static size_t remoteLinePos=0;static bool remoteLineOverflow=false;

constexpr uint32_t REMOTE_BLOCK_RECORDS=16,REMOTE_MIN_FREE=30000;
static RemoteCompactRecord* remoteBlocks[16]{};
static bool remoteRecords=false;
static uint32_t remoteAllocatedBytes=0,remoteBlockCount=0,remoteFreeAfter=0,remoteLargestBefore=0,remoteRoundtripCount=0;
static char remoteMemoryError[48]="NONE",remoteWifiError[48]="NONE";
static RemoteCompactRecord& remoteRecordAt(uint32_t i){return remoteBlocks[i/16][i%16];}
static uint32_t remoteCapacity=0,remoteRecordCount=0,remoteExpected=0;
static size_t remoteRequiredBytes=0;
static bool remoteBufferReady=false,remotePreparePending=false,remotePrepared=false,remoteFsReady=false;
static const char* REMOTE_RUN_FILE="/rf3run.bin";
static bool remoteWaitPending=false,remoteWaitingUwb=false,remoteCaptureEnabled=false,remoteRunReady=false;
static uint32_t remoteWaitStartedMs=0,remoteReconnectAtMs=0,remoteWifiMeasureViolations=0;
static RemoteRxCompact remoteSessionStartRx{};
static char remoteLastError[64]="NONE";
static RemoteState remoteState=RemoteState::MAINTENANCE;
static bool remoteMeasureWifiOffConfirmed=false;
static Config remoteRunCfg{};static uint32_t remoteRunSession=0,remoteRunPeer=0;
static uint16_t remoteRunAntTx=0,remoteRunAntRx=0;static bool remoteRunAntOK=false;
static uint8_t remoteRunAatten=0,remoteRunBatten=0;static bool remoteRunManual=false,remoteRunPowerOK=false;

static void remoteOnConfigApplied(){
  if(!remoteWaitingUwb&&!remoteCaptureEnabled&&!remoteRunReady)remoteState=RemoteState::CONFIGURED;
}
static void remoteOnArm(){
  if(remoteState==RemoteState::CONFIGURED||remoteState==RemoteState::PREPARED||remoteState==RemoteState::ROUTE_PREPARED)remoteState=RemoteState::ARMED;
}

static uint32_t remoteNetCrc(const RemoteNetConfig& in){RemoteNetConfig x=in;x.crc=0;return crc32(&x,sizeof(x));}
static bool remoteLoadNet(){
  Preferences p;if(!p.begin("rfnet3",true))return false;size_t n=p.getBytesLength("cfg");
  if(n!=sizeof(remoteNet)){p.end();return false;}size_t got=p.getBytes("cfg",&remoteNet,sizeof(remoteNet));p.end();
  if(got!=sizeof(remoteNet)||remoteNet.magic!=REMOTE_NET_MAGIC||remoteNet.version!=REMOTE_NET_VERSION||remoteNet.bytes!=sizeof(remoteNet))return false;
  if(remoteNet.crc!=remoteNetCrc(remoteNet)||!remoteNet.ssid[0]||!remoteNet.hostname[0])return false;return true;
}
static bool remoteSaveNet(){remoteNet.magic=REMOTE_NET_MAGIC;remoteNet.version=REMOTE_NET_VERSION;remoteNet.bytes=sizeof(remoteNet);remoteNet.crc=remoteNetCrc(remoteNet);Preferences p;if(!p.begin("rfnet3",false))return false;size_t n=p.putBytes("cfg",&remoteNet,sizeof(remoteNet));p.end();return n==sizeof(remoteNet);}
static bool remoteEraseNet(){Preferences p;if(!p.begin("rfnet3",false))return false;bool ok=p.remove("cfg");p.end();return ok;}

static bool remoteHexDecode(const char* in,char* out,size_t cap){
  size_t n=strlen(in);if((n&1)||n/2>=cap)return false;for(size_t i=0;i<n/2;i++){
    auto cv=[](char c)->int{if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1;};
    int a=cv(in[2*i]),b=cv(in[2*i+1]);if(a<0||b<0)return false;out[i]=char((a<<4)|b);
  }out[n/2]=0;return true;
}
static bool remoteToken(char*& cur,char* out,size_t cap){while(*cur&&isspace((unsigned char)*cur))++cur;if(!*cur)return false;size_t n=0;while(*cur&&!isspace((unsigned char)*cur)){if(n+1>=cap)return false;out[n++]=*cur++;}out[n]=0;return n>0;}

static uint32_t wifiDiagCycle=0;
static bool wifiTestFixedAp=false;
static uint8_t wifiRecoveryAttempts=0;
static uint8_t wifiDiagBssid[6]{};static int wifiDiagChannel=0;static bool wifiDiagPinned=false;
static uint32_t wifiDisconnectCount=0,wifiConnectCount=0,wifiReconnectAttemptCount=0;
static int wifiLastDisconnectReason=-1,wifiLastDisconnectRssi=0;
static uint32_t wifiLastDisconnectMs=0;static uint8_t wifiLastDisconnectBssid[6]{};
static bool wifiOutageCauseLatched=false;
static uint32_t wifiIntentionalStopUntilMs=0,wifiFirstDisconnectMs=0;
static int wifiFirstDisconnectReason=-1,wifiFirstDisconnectRssi=0;
static void wifiDiagEvent(WiFiEvent_t id, WiFiEventInfo_t info){
  int reason=-1,channel=-1,rssi=0;uint8_t mac[6]{};
  if(id==ARDUINO_EVENT_WIFI_STA_DISCONNECTED){memcpy(mac,info.wifi_sta_disconnected.bssid,6);rssi=info.wifi_sta_disconnected.rssi;wifiLastDisconnectReason=info.wifi_sta_disconnected.reason;wifiLastDisconnectRssi=rssi;wifiLastDisconnectMs=millis();memcpy(wifiLastDisconnectBssid,mac,6);++wifiDisconnectCount;
    if(!wifiOutageCauseLatched&&int32_t(millis()-wifiIntentionalStopUntilMs)>=0){wifiOutageCauseLatched=true;wifiFirstDisconnectReason=wifiLastDisconnectReason;wifiFirstDisconnectRssi=rssi;wifiFirstDisconnectMs=millis();}}
  if(id==ARDUINO_EVENT_WIFI_STA_GOT_IP)wifiOutageCauseLatched=false;
  if(id==ARDUINO_EVENT_WIFI_STA_CONNECTED){memcpy(mac,info.wifi_sta_connected.bssid,6);memcpy(wifiDiagBssid,mac,6);wifiDiagChannel=info.wifi_sta_connected.channel;++wifiConnectCount;}
  event("{\"event\":\"WIFI_DIAG\",\"kind\":\"ap_detail\",\"ms\":%lu,\"cycle\":%lu,\"event_id\":%d,\"bssid\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"rssi\":%d,\"pinned\":%s}",(unsigned long)millis(),(unsigned long)wifiDiagCycle,int(id),mac[0],mac[1],mac[2],mac[3],mac[4],mac[5],rssi,wifiDiagPinned?"true":"false");
  if(id==ARDUINO_EVENT_WIFI_STA_DISCONNECTED)reason=info.wifi_sta_disconnected.reason;
  if(id==ARDUINO_EVENT_WIFI_STA_CONNECTED)channel=info.wifi_sta_connected.channel;
  event("{\"event\":\"WIFI_DIAG\",\"kind\":\"event\",\"ms\":%lu,\"cycle\":%lu,\"event_id\":%d,\"reason\":%d,\"channel\":%d,\"heap\":%lu}",(unsigned long)millis(),(unsigned long)wifiDiagCycle,int(id),reason,channel,(unsigned long)ESP.getFreeHeap());
}
static void remoteWifiStop(){
  if(remoteClient)remoteClient.stop();remoteServer.stop();MDNS.end();
  wifiIntentionalStopUntilMs=millis()+3000;bool dr=WiFi.disconnect(true,false);bool mr=WiFi.mode(WIFI_OFF);
  event("{\"event\":\"WIFI_DIAG\",\"kind\":\"stop_return\",\"ms\":%lu,\"cycle\":%lu,\"disconnect_ok\":%s,\"mode_off_ok\":%s}",(unsigned long)millis(),(unsigned long)wifiDiagCycle,dr?"true":"false",mr?"true":"false");remoteWifiStarting=false;remoteWifiReady=false;remoteLinePos=0;remoteLineOverflow=false;
  delay(120);
}
// R1G1: optional Wi-Fi TX power cap (WIFITXPWR <quarter-dBm>, 0 = driver default), persisted in NVS.
// Purpose: test the supply-dip hypothesis behind GETPOINT-time resets without reflashing.
static int8_t gR1GWifiTxQ=0;static bool gR1GWifiTxLoaded=false;
static void r1gWifiTxLoad(){if(gR1GWifiTxLoaded)return;gR1GWifiTxLoaded=true;Preferences p;if(p.begin("r1gwifi",true)){gR1GWifiTxQ=int8_t(p.getChar("txq",0));p.end();}}
static bool r1gWifiTxValid(int q){static const int8_t ok[]={8,20,28,34,44,52,56,60,66,72,78,80,84};for(int8_t v:ok)if(v==q)return true;return false;}
static void r1gWifiTxApply(){r1gWifiTxLoad();if(gR1GWifiTxQ>0&&r1gWifiTxValid(gR1GWifiTxQ))WiFi.setTxPower(wifi_power_t(gR1GWifiTxQ));}
static void remoteWifiStart(){
  if(!remoteNetValid)return;++wifiReconnectAttemptCount;

  if(wifiTestFixedAp){
    static const uint8_t target[6]={0x50,0x5c,0x88,0x76,0xfb,0x81};
    memcpy(wifiDiagBssid,target,6);wifiDiagChannel=11;wifiDiagPinned=true;
    event("{\"event\":\"WIFI_FIXED_AP\",\"enabled\":true,\"target_bssid\":\"50:5c:88:76:fb:81\",\"channel\":11,\"cycle\":%lu}",(unsigned long)wifiDiagCycle);
  }
  WiFi.persistent(false);WiFi.setAutoReconnect(false);bool modeOK=WiFi.mode(WIFI_STA);bool sleepOK=WiFi.setSleep(false);bool hostOK=WiFi.setHostname(remoteNet.hostname);int beginResult;
  // R1F: default WIFI_FAST_SCAN joins the FIRST AP matching the SSID. On a campus ESS (many
  // 'postech' APs) that can be a -85..-93 dBm AP while a -60 dBm AP is reachable (Pi saw -60).
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
  r1gTrace(4,0,0,0);r1gWifiTxApply();
  if(remoteNet.enterprise)beginResult=WiFi.begin(remoteNet.ssid,WPA2_AUTH_PEAP,remoteNet.identity,remoteNet.username,remoteNet.password,nullptr,nullptr,nullptr,-1,wifiDiagPinned?wifiDiagChannel:0,wifiDiagPinned?wifiDiagBssid:nullptr);
  else { esp_err_t eapOff=esp_wifi_sta_enterprise_disable(); event("{\"event\":\"WIFI_DIAG\",\"kind\":\"eap_disable\",\"rc\":%d}",int(eapOff)); beginResult=WiFi.begin(remoteNet.ssid,remoteNet.password); }
  event("{\"event\":\"WIFI_DIAG\",\"kind\":\"start_return\",\"ms\":%lu,\"cycle\":%lu,\"mode_sta_ok\":%s,\"sleep_ok\":%s,\"hostname_ok\":%s,\"begin_status\":%d,\"heap\":%lu}",(unsigned long)millis(),(unsigned long)wifiDiagCycle,modeOK?"true":"false",sleepOK?"true":"false",hostOK?"true":"false",beginResult,(unsigned long)ESP.getFreeHeap());remoteWifiStartMs=millis();remoteWifiStarting=true;remoteWifiReady=false;
}
static void remoteWifiPollState(){
  if(remoteWifiStarting&&WiFi.status()==WL_CONNECTED&&uint32_t(WiFi.localIP())!=0){
    remoteServer.begin();remoteServer.setNoDelay(true);MDNS.begin(remoteNet.hostname);remoteWifiStarting=false;remoteWifiReady=true;wifiRecoveryAttempts=0;r1gWifiTxApply();snprintf(remoteWifiError,sizeof(remoteWifiError),"NONE");
  }
  // R1J7: one retry owner, no WIFI_OFF from poller.
  // R1J9: autoReconnect is off, so a CONNECT_FAILED / NO_SSID_AVAIL start never recovers by itself: stop waiting.
  if(remoteWifiStarting&&millis()-remoteWifiStartMs>15000&&(WiFi.status()==WL_CONNECT_FAILED||WiFi.status()==WL_NO_SSID_AVAIL)){
    remoteWifiStarting=false;remoteWifiReady=false;snprintf(remoteWifiError,sizeof(remoteWifiError),"WIFI_CONNECT_FAILED_EARLY");
  }
  if(remoteWifiStarting&&millis()-remoteWifiStartMs>REMOTE_WIFI_CONNECT_TIMEOUT_MS){
    remoteWifiStarting=false;remoteWifiReady=false;snprintf(remoteWifiError,sizeof(remoteWifiError),"WIFI_CONNECT_TIMEOUT");
  }
  if(remoteWifiReady&&(!remoteClient||!remoteClient.connected())){
    WiFiClient c=remoteServer.available();if(c){if(remoteClient)remoteClient.stop();remoteClient=c;remoteClient.setNoDelay(true);remoteClient.setTimeout(2);++remoteTcpConnections;remoteTcpLastRxMs=millis();remoteLinePos=0;remoteLineOverflow=false;}
  }
}

// A whole TCP frame has one writer; partial writes must never be ignored.
static bool remoteSendAll(const uint8_t* data,size_t bytes){
  size_t sent=0;uint32_t last=millis();const uint32_t began=last;
  while(sent<bytes){
    // A trickle of single bytes must not keep the command task here forever.
    if(millis()-began>5000||!remoteClient.connected())return false;
    size_t n=remoteClient.write(data+sent,bytes-sent);
    if(n){sent+=n;last=millis();}
    else {if(millis()-last>2000)return false;delay(1);}
  }return true;
}
static bool remoteWriteWireOnly(uint8_t kind,const char* json,size_t n,const uint8_t* raw,size_t rawN){
  if(!remoteTxMutex)return false;
  if(xSemaphoreTake(remoteTxMutex,pdMS_TO_TICKS(2500))!=pdTRUE){++remoteTcpFramesFailed;return false;}
  bool ok=false;
  if(remoteWifiReady&&remoteClient&&remoteClient.connected()){
    WireHeader h{WIRE_MAGIC,1,kind,sizeof(WireHeader),remoteTcpSeq++,uint32_t(n),uint32_t(rawN),0};
    uint32_t c=crcUpdate(0xFFFFFFFFUL,&h,offsetof(WireHeader,crc));c=crcUpdate(c,json,n);if(rawN)c=crcUpdate(c,raw,rawN);h.crc=c^0xFFFFFFFFUL;
    // Coalesce small metadata frames to avoid header/body TCP latency.
    // R1H: R1G2 put the clamp below inside a trailing comment -> frames >1000 B (HELLO, CAPABILITIES,
    // WIFIINFO) overran this static buffer into the Wi-Fi globals that follow it in .bss.
    static uint8_t first[1024];  // static; serialized by remoteTxMutex
    size_t j=n;if(j>sizeof(first)-sizeof(h))j=sizeof(first)-sizeof(h);
    memcpy(first,&h,sizeof(h));memcpy(first+sizeof(h),json,j);
    ok=remoteSendAll(first,sizeof(h)+j);
    if(ok&&j<n)ok=remoteSendAll((const uint8_t*)json+j,n-j);
    if(ok&&rawN)ok=remoteSendAll(raw,rawN);
    if(!ok){snprintf(remoteWifiError,sizeof(remoteWifiError),"TCP_FRAME_WRITE_FAILED");remoteClient.stop();}
  }
  if(ok)++remoteTcpFramesOk;else ++remoteTcpFramesFailed;remoteTcpLastWriteMs=millis();
  xSemaphoreGive(remoteTxMutex);return ok;
}
static void remoteCommandReply(const char* json,size_t n){
  // Wi-Fi callbacks also call event(): only the actual command task may touch TCP.
  if(gR04CommandFromTcp&&gR05CommandTask==xTaskGetCurrentTaskHandle())
    remoteWriteWireOnly(1,json,n,nullptr,0);
}
static bool remoteWriteReplyOnly(uint8_t kind,const char* json,size_t n,const uint8_t* raw,size_t rawN){
  if(gR04CommandFromTcp)return remoteWriteWireOnly(kind,json,n,raw,rawN);
  // GETPOINT USB chunks/ACKs are metadata-only and fit OutMsg. No second UART writer.
  if(kind!=1||rawN||n>=sizeof(OutMsg::text))return false;
  OutMsg m;memcpy(m.text,json,n);m.text[n]=0;++outputPending;
  if(xQueueSend(outQ,&m,pdMS_TO_TICKS(2000))!=pdTRUE){--outputPending;++outputDrops;return false;}
  return true;
}
static void remoteDirectAck(uint32_t req,bool ok,const char* op,const char* why="OK"){
  char b[512];int n=snprintf(b,sizeof(b),"{\"event\":\"ACK\",\"request\":%lu,\"role\":\"anchor\",\"op\":\"%s\",\"ok\":%s,\"reason\":\"%s\",\"boot\":%lu,\"session\":%lu,\"config_id\":%lu}",(unsigned long)req,op,ok?"true":"false",why,(unsigned long)bootId,(unsigned long)sessionId,(unsigned long)configId(cfg));
  if(n>0&&size_t(n)<sizeof(b))remoteWriteReplyOnly(1,b,size_t(n),nullptr,0);
}

static void remoteFreeRam(){for(auto& block:remoteBlocks){if(block)free(block);block=nullptr;}remoteRecords=false;remoteCapacity=0;}
static bool remoteAllocate(){
 remoteFreeRam();remoteAllocatedBytes=0;remoteBlockCount=0;remoteFreeAfter=0;
 remoteLargestBefore=heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
 snprintf(remoteMemoryError,sizeof(remoteMemoryError),"NONE");
 for(uint32_t offset=0;offset<remoteExpected;offset+=16){uint32_t n=remoteExpected-offset;if(n>16)n=16;
  auto* block=(RemoteCompactRecord*)malloc(n*sizeof(RemoteCompactRecord));
  if(!block){snprintf(remoteMemoryError,sizeof(remoteMemoryError),"BUFFER_ALLOC_FAILED");remoteFreeRam();return false;}
  memset(block,0,n*sizeof(RemoteCompactRecord));remoteBlocks[offset/16]=block;remoteAllocatedBytes+=n*sizeof(RemoteCompactRecord);++remoteBlockCount;
 }
 remoteFreeAfter=heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
 if(remoteFreeAfter<REMOTE_MIN_FREE){snprintf(remoteMemoryError,sizeof(remoteMemoryError),"BUFFER_HEADROOM_BELOW_30000");remoteFreeRam();return false;}
 remoteRecords=true;remoteCapacity=remoteExpected;return true;
}
static void remoteHealth(uint32_t req,const char* op){
 event("{\"event\":\"ACK\",\"request\":%lu,\"op\":\"%s\",\"ok\":true,\"record_schema_version\":2,\"compact_record_bytes\":479,\"expected_records\":%lu,\"block_records\":16,\"block_count\":%lu,\"allocated_bytes\":%lu,\"free_heap_after\":%lu,\"largest_free_before\":%lu,\"roundtrip_records\":%lu,\"wifi_error\":\"%s\",\"memory_error\":\"%s\",\"run_error\":\"%s\"}",(unsigned long)req,op,(unsigned long)remoteExpected,(unsigned long)remoteBlockCount,(unsigned long)remoteAllocatedBytes,(unsigned long)remoteFreeAfter,(unsigned long)remoteLargestBefore,(unsigned long)remoteRoundtripCount,remoteWifiError,remoteMemoryError,remoteLastError);
}
static void remoteClearRunState(){remoteFreeRam();remoteRecordCount=0;remoteExpected=0;remoteRequiredBytes=0;remoteBufferReady=false;remotePrepared=false;remoteRunReady=false;remoteMeasureWifiOffConfirmed=false;remoteState=RemoteState::MAINTENANCE;if(remoteFsReady&&LittleFS.exists(REMOTE_RUN_FILE))LittleFS.remove(REMOTE_RUN_FILE);}
static void remoteCopyRx(const RxObs& o,RemoteRxCompact& d){
  d.bytes=o.bytes;d.received=o.received;d.diag=o.diag;d.lde_done=o.lde_done;d.hw=o.hw;d.raw_hw=o.raw_hw;d.esp_us=o.esp_us;d.arm_us=o.arm_us;d.switch_us=o.switch_us;d.status=o.status;
  d.arm_lead_us=o.arm_lead_us;d.preamble_margin_us=o.preamble_margin_us;d.fetch_us=o.fetch_us;d.diag_us=o.diag_us;d.detect_lag_us=o.detect_lag_us;d.observed_arm_to_rmarker_us=o.observed_arm_to_rmarker_us;d.ci_raw=o.ci_raw;d.lde_adjust_ticks=o.lde_adjust_ticks;
  d.fp_raw=o.fp_raw;d.nacc=o.nacc;d.cir_pwr=o.cir_pwr;d.f1=o.f1;d.f2=o.f2;d.f3=o.f3;d.noise=o.noise;d.ttcki=o.ttcki;d.ttcko=o.ttcko;
  if(o.bytes<=sizeof(d.raw_air))memcpy(d.raw_air,o.raw_air,o.bytes);
}
static void remoteRestoreRx(const RemoteAnchorRecord& s,RxObs& o){
  const auto& d=s.response;o=RxObs{};o.type=RESPONSE;o.ctrl=255;o.ls=255;o.bytes=d.bytes;o.cir_slot=-1;o.received=d.received;o.diag=d.diag;o.settled=true;o.port_match=true;
  o.hw=d.hw;o.raw_hw=d.raw_hw;o.esp_us=d.esp_us;o.arm_us=d.arm_us;o.switch_us=d.switch_us;o.status=d.status;o.arm_lead_us=d.arm_lead_us;o.preamble_margin_us=d.preamble_margin_us;o.fetch_us=d.fetch_us;o.diag_us=d.diag_us;
  o.detect_lag_us=d.detect_lag_us;o.observed_arm_to_rmarker_us=d.observed_arm_to_rmarker_us;o.verify_bad=-1;o.verify_repeat_bad=-1;o.ci_raw=d.ci_raw;o.lde_adjust_ticks=d.lde_adjust_ticks;o.lde_done=d.lde_done;
  o.fp_raw=d.fp_raw;o.nacc=d.nacc;o.cir_pwr=d.cir_pwr;o.f1=d.f1;o.f2=d.f2;o.f3=d.f3;o.noise=d.noise;o.ttcki=d.ttcki;o.ttcko=d.ttcko;o.tx_planned=s.fast[2];o.source_boot=remoteRunPeer;
  if(d.bytes<=sizeof(o.raw_air))memcpy(o.raw_air,d.raw_air,d.bytes);
}
static void remoteCaptureResult(const Result& r){
  if(!IS_ANCHOR||!remoteCaptureEnabled||!remoteBufferReady||!remoteRecords)return;
  if(remoteWifiReady||WiFi.getMode()!=WIFI_OFF)++remoteWifiMeasureViolations;
  if(remoteRecordCount>=remoteCapacity){snprintf(remoteLastError,sizeof(remoteLastError),"REMOTE_BUFFER_OVERFLOW");remoteCaptureEnabled=false;return;}
  RemoteAnchorRecord d{};d.txn=r.txn;d.begin_us=r.begin_us;d.end_us=r.end_us;d.ok=r.ok;d.fast_valid=r.fast_valid;d.ntx=(r.ntx<4?r.ntx:4);d.nrx=(r.nrx<1?r.nrx:1);snprintf(d.failure,sizeof(d.failure),"%s",r.failure);memcpy(d.fast,r.fast,sizeof(d.fast));
  for(unsigned i=0;i<d.ntx;i++)d.tx[i]=r.tx[i];if(d.nrx)remoteCopyRx(r.rx[0],d.response);
  RemoteAnchorRecord restored{};auto& packed=remoteRecordAt(remoteRecordCount);
  if(!remoteEncode(d,packed)||!remoteDecode(packed,restored)||!remoteEqual(d,restored)){
    snprintf(remoteLastError,sizeof(remoteLastError),"COMPACT_ROUNDTRIP_FAILED");remoteCaptureEnabled=false;return;
  }
  ++remoteRoundtripCount;++remoteRecordCount;
}

static void remoteBuildResult(const RemoteAnchorRecord& s,Result& r){
  new (&r) Result();r.conf=remoteRunCfg;r.session=remoteRunSession;r.txn=s.txn;r.boot=bootId;r.peer=remoteRunPeer;r.fast_route=(remoteRunCfg.mode==4)?ROUTE_A:fastRouteForTxn(remoteRunCfg,s.txn);r.begin_us=s.begin_us;r.end_us=s.end_us;r.ok=s.ok;r.fast_valid=s.fast_valid;snprintf(r.failure,sizeof(r.failure),"%s",s.failure);
  r.ant_tx_requested=r.ant_rx_requested=uint16_t(remoteRunCfg.ant_anchor);r.ant_tx_readback=remoteRunAntTx;r.ant_rx_readback=remoteRunAntRx;r.ant_readback_ok=remoteRunAntOK;
  r.poll_atten_x2=R7_POLL_ATTEN_X2;r.probe_a_atten_x2=remoteRunAatten;r.probe_b_atten_x2=remoteRunBatten;r.tx_manual=remoteRunManual;r.tx_power_plan_valid=remoteRunPowerOK;
  memcpy(r.fast,s.fast,sizeof(r.fast));r.ntx=s.ntx;for(unsigned i=0;i<r.ntx&&i<4;i++)r.tx[i]=s.tx[i];r.nrx=s.nrx;if(r.nrx)remoteRestoreRx(s,r.rx[0]);r.ci_resp_raw=r.nrx?r.rx[0].ci_raw:0;
  if(r.fast_valid){double bias=double(remoteRunCfg.ci_bias_ppb)/1000.0;r.fast_valid=ciRange(r.fast[0],r.fast[1],r.fast[2],r.fast[3],r.ci_resp_raw,bias,r.range_ci_inst,r.tof_ci_inst,r.ci_corr_ppm);r.ci_resp_ppm=double(r.ci_resp_raw)*CI_PPM_PER_RAW;
    if(r.fast_valid){r.ci3_corr_ppm=pushCiHistory(r.ci_corr_ppm,remoteRunCfg.ci_smooth_n);r.ci_hist_count=ciHistCount;double q=r.ci3_corr_ppm*1e-6;double ra=double(delta40(r.fast[3],r.fast[0])),db=double(delta40(r.fast[2],r.fast[1]));r.tof_ci3=0.5*(ra-db*(1.0-q));r.range_ci3=r.tof_ci3*DTU_S*C_AIR_MPS;}}
}

#include "route_v1.hpp"
#include "ota_manager.hpp"

static void remoteOnStop(){
  if(r03SessionActive||r03WaitPending||r03PointMeasuring){r03SessionActive=false;r03WaitPending=false;r03WaitingPoint=false;r03PointMeasuring=false;r03Prepared=false;remoteCaptureEnabled=false;remoteBufferReady=false;remoteFreeRam();remoteState=RemoteState::MAINTENANCE;snprintf(remoteLastError,sizeof(remoteLastError),"R03_STOPPED");remoteReconnectAtMs=millis()+250;}
}

static bool remoteDumpRun(uint32_t req){
  if(!remoteRunReady||!remoteFsReady||!LittleFS.exists(REMOTE_RUN_FILE)){remoteDirectAck(req,false,"GETRUN","RUN_NOT_READY");return true;}
  if(outputPending.load()!=0||writerBusy.load()){remoteDirectAck(req,false,"GETRUN","WRITER_BUSY_RETRY");return true;}
  File f=LittleFS.open(REMOTE_RUN_FILE,"r");if(!f){remoteDirectAck(req,false,"GETRUN","RUN_FILE_OPEN_FAILED");return true;}
  RemoteRunFileHeader h{};if(f.read((uint8_t*)&h,sizeof(h))!=sizeof(h)||h.magic!=0x33524652UL||h.version!=3||h.record_bytes!=REMOTE_COMPACT_BYTES||h.record_schema_version!=2||h.block_records!=16||h.count!=remoteRecordCount||h.session!=remoteRunSession){f.close();remoteDirectAck(req,false,"GETRUN","RUN_FILE_HEADER_INVALID");return true;}
  if(f.size()!=sizeof(h)+h.count*REMOTE_COMPACT_BYTES){f.close();remoteDirectAck(req,false,"GETRUN","RUN_FILE_SIZE_INVALID");return true;}
  uint32_t fileCRC=0xffffffffUL,blockCRC=0xffffffffUL;
  for(uint32_t i=0;i<h.count;i++){RemoteCompactRecord packed{};
    if(f.read(packed.data,sizeof(packed))!=sizeof(packed)){f.close();remoteDirectAck(req,false,"GETRUN","RUN_FILE_TRUNCATED");return true;}
    fileCRC=crcUpdate(fileCRC,&packed,sizeof(packed));blockCRC=crcUpdate(blockCRC,&packed,sizeof(packed));
    if(i%16==15||i+1==h.count){if((blockCRC^0xffffffffUL)!=h.block_crc32[i/16]){f.close();remoteDirectAck(req,false,"GETRUN","BLOCK_CRC_MISMATCH");return true;}blockCRC=0xffffffffUL;}
  }
  if((fileCRC^0xffffffffUL)!=h.records_crc32){f.close();remoteDirectAck(req,false,"GETRUN","RUN_FILE_CRC_MISMATCH");return true;}
  f.seek(sizeof(h));
  {
    char ctrl[2048];int n=snprintf(ctrl,sizeof(ctrl),"{\"event\":\"REMOTE_SESSION_START_RX\",\"role\":\"anchor\",\"session\":%lu,\"rx_hw\":%llu,\"rx_raw_hw\":%llu,\"lde_adjust_ticks\":%lld,\"lde_done\":%s,\"diag\":%s,\"ci_raw\":%ld,\"fp_raw\":%u,\"rxpacc\":%u,\"cir_pwr\":%u,\"f1\":%u,\"f2\":%u,\"f3\":%u,\"std_noise\":%u,\"ttcki\":%lu,\"ttcko\":%llu,\"sys_status\":%llu,\"bytes\":%u,\"wifi_measure_violation_count\":%lu,\"air_hex\":\"",
      (unsigned long)remoteRunSession,(unsigned long long)h.session_start_rx.hw,(unsigned long long)h.session_start_rx.raw_hw,(long long)h.session_start_rx.lde_adjust_ticks,h.session_start_rx.lde_done?"true":"false",h.session_start_rx.diag?"true":"false",(long)h.session_start_rx.ci_raw,unsigned(h.session_start_rx.fp_raw),unsigned(h.session_start_rx.nacc),unsigned(h.session_start_rx.cir_pwr),unsigned(h.session_start_rx.f1),unsigned(h.session_start_rx.f2),unsigned(h.session_start_rx.f3),unsigned(h.session_start_rx.noise),(unsigned long)h.session_start_rx.ttcki,(unsigned long long)h.session_start_rx.ttcko,(unsigned long long)h.session_start_rx.status,unsigned(h.session_start_rx.bytes),(unsigned long)h.wifi_measure_violation_count);
    if(n>0&&size_t(n)+2U*h.session_start_rx.bytes+3U<sizeof(ctrl)){
      size_t k=size_t(n);for(unsigned i=0;i<h.session_start_rx.bytes;i++)k+=size_t(snprintf(ctrl+k,sizeof(ctrl)-k,"%02x",h.session_start_rx.raw_air[i]));
      ctrl[k++]='\"';ctrl[k++]='}';ctrl[k]=0;remoteWriteWireOnly(1,ctrl,k,nullptr,0);
    }
  }
  static char json[10240];resetCiHistory();uint32_t crc=0xFFFFFFFFUL;
  for(uint32_t i=0;i<h.count;i++){RemoteAnchorRecord rec{};RemoteCompactRecord packed{};if(f.read(packed.data,sizeof(packed))!=sizeof(packed)){f.close();remoteDirectAck(req,false,"GETRUN","RUN_FILE_TRUNCATED");return true;}crc=crcUpdate(crc,&packed,sizeof(packed));if(!remoteDecode(packed,rec)){f.close();remoteDirectAck(req,false,"GETRUN","COMPACT_DECODE_FAILED");return true;}Result& r=pool[0];remoteBuildResult(rec,r);Json j(json,sizeof(json));resultJson(r,j);if(!j.ok){f.close();remoteDirectAck(req,false,"GETRUN","JSON_OVERFLOW");return true;}remoteWriteWireOnly(2,json,j.n,nullptr,0);if((i&15U)==15U)delay(1);}
  f.close();crc^=0xFFFFFFFFUL;if(crc!=h.records_crc32){remoteDirectAck(req,false,"GETRUN","RUN_FILE_CRC_MISMATCH");return true;}remoteDirectAck(req,true,"GETRUN","OK");return true;
}

static void remoteEmitInfo(uint32_t req,const char* op){
  IPAddress ip=WiFi.localIP();
  event("{\"event\":\"ACK\",\"request\":%lu,\"op\":\"%s\",\"ok\":true,\"role\":\"anchor\",\"wifi_configured\":%s,\"wifi_ready\":%s,\"wifi_status\":%d,\"ip\":\"%u.%u.%u.%u\",\"hostname\":\"%s\",\"rssi\":%ld,\"tcp_port\":%u,\"buffer_ready\":%s,\"buffer_capacity\":%lu,\"buffer_records\":%lu,\"buffer_bytes\":%lu,\"expected\":%lu,\"prepared\":%s,\"waiting_uwb\":%s,\"capture_enabled\":%s,\"run_ready\":%s,\"fs_ready\":%s,\"ram_buffer_allocated\":%s,\"state\":\"%s\",\"measure_wifi_off_confirmed\":%s,\"wifi_measure_violation_count\":%lu,\"wifi_disconnect_count\":%lu,\"wifi_connect_count\":%lu,\"wifi_reconnect_attempt_count\":%lu,\"wifi_last_disconnect_reason\":%d,\"wifi_last_disconnect_rssi\":%d,\"wifi_last_disconnect_ms\":%lu,\"wifi_pinned\":%s,\"last_error\":\"%s\",\"heap\":%lu}",
    (unsigned long)req,op,remoteNetValid?"true":"false",remoteWifiReady?"true":"false",int(WiFi.status()),ip[0],ip[1],ip[2],ip[3],remoteNet.hostname,(long)(remoteWifiReady?WiFi.RSSI():0),unsigned(REMOTE_TCP_PORT),remoteBufferReady?"true":"false",(unsigned long)remoteCapacity,(unsigned long)remoteRecordCount,(unsigned long)remoteRequiredBytes,(unsigned long)remoteExpected,remotePrepared?"true":"false",remoteWaitingUwb?"true":"false",remoteCaptureEnabled?"true":"false",remoteRunReady?"true":"false",remoteFsReady?"true":"false",remoteRecords?"true":"false",remoteStateName(remoteState),remoteMeasureWifiOffConfirmed?"true":"false",(unsigned long)remoteWifiMeasureViolations,(unsigned long)wifiDisconnectCount,(unsigned long)wifiConnectCount,(unsigned long)wifiReconnectAttemptCount,wifiLastDisconnectReason,wifiLastDisconnectRssi,(unsigned long)wifiLastDisconnectMs,wifiDiagPinned?"true":"false",remoteLastError,(unsigned long)ESP.getFreeHeap());
}

static bool remoteCommand(uint32_t req,const char* op,char*& cur){
  if(r04OtaCommand(req,op,cur))return true;
  if(strcmp(op,"TRANSPORTINFO")==0){
    event("{\"event\":\"ACK\",\"request\":%lu,\"op\":\"TRANSPORTINFO\",\"role\":\"anchor\",\"ok\":true,\"transport_api\":2,\"build\":\"R1J8_GETSUMMARY\",\"tcp_owner\":\"loopTask\",\"tcp_unsolicited\":false,\"connections\":%lu,\"requests\":%lu,\"frames_ok\":%lu,\"frames_failed\":%lu,\"last_request\":%lu,\"last_op\":\"%s\",\"last_rx_ms\":%lu,\"last_write_ms\":%lu,\"wifi_error\":\"%s\"}",
      (unsigned long)req,(unsigned long)remoteTcpConnections,(unsigned long)remoteTcpRequests,
      (unsigned long)remoteTcpFramesOk,(unsigned long)remoteTcpFramesFailed,(unsigned long)remoteTcpLastReq,
      remoteTcpLastOp,(unsigned long)remoteTcpLastRxMs,(unsigned long)remoteTcpLastWriteMs,remoteWifiError);return true;
  }
  if(IS_ANCHOR&&strcmp(op,"WIFIRECOVERYTEST")==0){
    uint32_t confirm=0;
    if(active||remoteWaitingUwb||remoteCaptureEnabled||remotePreparePending||remoteWaitPending||remoteState!=RemoteState::MAINTENANCE){ack(req,false,op,"MAINTENANCE_ONLY");return true;}
    if(!parseU32(cur,confirm)||confirm!=1){ack(req,false,op,"CONFIRM_ONE_REQUIRED");return true;}
    if(WiFi.status()!=WL_CONNECTED||uint32_t(WiFi.localIP())==0){ack(req,false,op,"CONNECT_FIRST");return true;}
    // Disable driver retries so the independent recovery loop is exercised.
    if(!WiFi.setAutoReconnect(false)){ack(req,false,op,"AUTORECONNECT_DISABLE_FAILED");return true;}
    ack(req,true,op,"DROP_LINK_TEST");
    const bool disconnected=WiFi.disconnect(false,false);
    event("{\"event\":\"WIFI_RECOVERY_TEST_DROP\",\"ok\":%s,\"boot\":%lu}",disconnected?"true":"false",(unsigned long)bootId);
    if(!disconnected)WiFi.setAutoReconnect(true);
    return true;
  }
  if(IS_ANCHOR&&strcmp(op,"REMOTEHEALTH")==0){remoteHealth(req,op);return true;}
  if(IS_ANCHOR&&strcmp(op,"COMPACTSELFTEST")==0){if(active){ack(req,false,op,"STOP_FIRST");return true;}bool ok=remoteCodecSelfTest();ack(req,ok,op,ok?"1000_ROUNDTRIPS_AND_BOUNDS_PASS":"CODEC_FAILED");return true;}

  // Same test-only RAM intervention in both variants. No Preferences/NVS writes.
  if(IS_ANCHOR && strcmp(op,"WIFITESTAP")==0){
    char setting[8]{};if(!remoteToken(cur,setting,sizeof(setting)))snprintf(setting,sizeof(setting),"GET");
    if(strcmp(setting,"GET")!=0){
      if(active||remoteWaitingUwb||remoteCaptureEnabled){ack(req,false,op,"STOP_FIRST");return true;}
      if(strcmp(setting,"ON")==0)wifiTestFixedAp=true;
      else if(strcmp(setting,"OFF")==0){wifiTestFixedAp=false;wifiDiagPinned=false;}
      else {ack(req,false,op,"ON_OFF_GET_REQUIRED");return true;}
    }
    event("{\"event\":\"WIFI_SPLIT_STATE\",\"recording\":%s,\"rx_rows\":%u,\"route_active\":%s,\"waiting_uwb\":%s,\"capture_enabled\":%s,\"extra_static_bytes\":%u,\"lazy_allocated\":%s}",ctrlRecording?"true":"false",ctrlRxN,active?"true":"false",remoteWaitingUwb?"true":"false",remoteCaptureEnabled?"true":"false",0,ctrlRxRows?"true":"false");
    wifiKeepReserve();
    event("{\"event\":\"ACK\",\"request\":%lu,\"op\":\"WIFITESTAP\",\"ok\":true,\"enabled\":%s,\"target_bssid\":\"50:5c:88:76:fb:81\",\"target_channel\":11,\"heap\":%lu,\"min_heap\":%lu,\"largest_default\":%lu,\"largest_internal_8bit\":%lu}",(unsigned long)req,wifiTestFixedAp?"true":"false",(unsigned long)ESP.getFreeHeap(),(unsigned long)ESP.getMinFreeHeap(),(unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT),(unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
    return true;
  }

  if(IS_ANCHOR && strcmp(op,"WIFIPIN")==0){
    if(active){ack(req,false,op,"STOP_FIRST");return true;}
    uint32_t v;if(!parseU32(cur,v)||v>1){ack(req,false,op,"ZERO_OR_ONE");return true;}
    if(v){
      if(WiFi.status()!=WL_CONNECTED){ack(req,false,op,"CONNECT_FIRST");return true;}
      memcpy(wifiDiagBssid,WiFi.BSSID(),6);wifiDiagChannel=WiFi.channel();
    }
    wifiDiagPinned=v;ack(req,true,op);return true;
  }
  if(IS_ANCHOR && strcmp(op,"WIFICYCLE")==0){
    if(active||remoteWaitingUwb||remoteCaptureEnabled){ack(req,false,op,"STOP_FIRST");return true;}
    ++wifiDiagCycle;wifiRecoveryAttempts=0;wifiDiagPinned=false;
    ack(req,true,op,"WIFI_CYCLE_SCHEDULED");remoteCyclePending=true;remoteCycleDueMs=millis()+300;return true;
  }
  if(!IS_ANCHOR)return false;
  if(strcmp(op,"WIFIINFO")==0||strcmp(op,"REMOTEINFO")==0){remoteEmitInfo(req,op);return true;}
  if(strcmp(op,"WIFITXPWR")==0){
    uint32_t q;static const uint8_t allowed[]={8,20,28,34,44,52,56,60,66,72,78,80,84};bool okv=false;
    if(!parseU32(cur,q)){remoteDirectAck(req,false,op,"QUARTER_DBM_REQUIRED");return true;}
    if(q==0)okv=true;for(uint8_t v:allowed)if(q==v)okv=true;
    if(!okv){remoteDirectAck(req,false,op,"ALLOWED_0_8_20_28_34_44_52_56_60_66_72_78_80_84");return true;}
    r1gWifiTxLoad();gR1GWifiTxQ=int8_t(q);Preferences p;bool saved=false;if(p.begin("r1gwifi",false)){saved=p.putChar("txq",int8_t(q))==1;p.end();}
    if(remoteWifiReady&&q>0)WiFi.setTxPower(wifi_power_t(q));
    event("{\"event\":\"ACK\",\"request\":%lu,\"op\":\"WIFITXPWR\",\"ok\":true,\"role\":\"anchor\",\"tx_power_q\":%u,\"saved\":%s,\"note\":\"0 applies on next Wi-Fi start\"}",(unsigned long)req,unsigned(q),saved?"true":"false");return true;
  }
  if(strcmp(op,"WIFILINK")==0){
    // R1F: compact link view (WIFIINFO output is close to the 1024-byte event limit).
    uint8_t* b=WiFi.BSSID();char bs[18]="00:00:00:00:00:00";if(remoteWifiReady&&b)snprintf(bs,sizeof(bs),"%02x:%02x:%02x:%02x:%02x:%02x",b[0],b[1],b[2],b[3],b[4],b[5]);
    event("{\"event\":\"ACK\",\"request\":%lu,\"op\":\"WIFILINK\",\"ok\":true,\"role\":\"anchor\",\"wifi_ready\":%s,\"rssi\":%ld,\"bssid\":\"%s\",\"channel\":%ld,\"scan\":\"ALL_CHANNEL_BY_SIGNAL\",\"tx_power_q\":%d,\"tx_power_cap_q\":%d,\"disconnect_count\":%lu,\"connect_count\":%lu,\"reconnect_attempt_count\":%lu,\"last_disconnect_reason\":%d,\"last_disconnect_rssi\":%d,\"first_disconnect_reason\":%d,\"first_disconnect_rssi\":%d,\"first_disconnect_ms\":%lu,\"uptime_ms\":%lu,\"heap\":%lu}",
      (unsigned long)req,remoteWifiReady?"true":"false",(long)(remoteWifiReady?WiFi.RSSI():0),bs,(long)(remoteWifiReady?WiFi.channel():0),int(remoteWifiReady?WiFi.getTxPower():0),(r1gWifiTxLoad(),int(gR1GWifiTxQ)),(unsigned long)wifiDisconnectCount,(unsigned long)wifiConnectCount,(unsigned long)wifiReconnectAttemptCount,wifiLastDisconnectReason,wifiLastDisconnectRssi,wifiFirstDisconnectReason,wifiFirstDisconnectRssi,(unsigned long)wifiFirstDisconnectMs,(unsigned long)millis(),(unsigned long)ESP.getFreeHeap());return true;
  }
  if(strcmp(op,"WIFISCAN")==0){
    if(active){ack(req,false,op,"STOP_FIRST");return true;}
    int n=WiFi.scanNetworks();int found=0;
    for(int i=0;i<n;i++)if(WiFi.SSID(i)==String(remoteNet.ssid))++found;
    WiFi.scanDelete();event("{\"event\":\"ACK\",\"request\":%lu,\"op\":\"WIFISCAN\",\"ok\":true,\"matching_24ghz_aps\":%d}",(unsigned long)req,found);return true;
  }
  if(strcmp(op,"WIFIEAPHEX")==0){
    if(active){ack(req,false,op,"STOP_FIRST");return true;}
    char sh[67],ih[131],uh[131],ph[131],host[33];
    if(!remoteToken(cur,sh,sizeof(sh))||!remoteToken(cur,ih,sizeof(ih))||!remoteToken(cur,uh,sizeof(uh))||!remoteToken(cur,ph,sizeof(ph))||!remoteToken(cur,host,sizeof(host))){ack(req,false,op,"EAP_FIELDS_REQUIRED");return true;}
    RemoteNetConfig n{};n.enterprise=true;
    if(!remoteHexDecode(sh,n.ssid,sizeof(n.ssid))||!remoteHexDecode(ih,n.identity,sizeof(n.identity))||!remoteHexDecode(uh,n.username,sizeof(n.username))||!remoteHexDecode(ph,n.password,sizeof(n.password))||!n.identity[0]||!n.username[0]||!n.password[0]){ack(req,false,op,"EAP_FIELDS_INVALID");return true;}
    snprintf(n.hostname,sizeof(n.hostname),"%s",host);remoteNet=n;wifiDiagPinned=false;wifiRecoveryAttempts=0;remoteNetValid=remoteSaveNet();
    if(!remoteNetValid){ack(req,false,op,"NVS_WRITE_FAILED");return true;}
    remoteState=RemoteState::MAINTENANCE;ack(req,true,op);remoteWifiStop();remoteWifiStart();return true;
  }
  if(strcmp(op,"WIFICONFIGHEX")==0){
    if(active){ack(req,false,op,"STOP_FIRST");return true;}char sh[67],ph[131],host[33];if(!remoteToken(cur,sh,sizeof(sh))||!remoteToken(cur,ph,sizeof(ph))||!remoteToken(cur,host,sizeof(host))){ack(req,false,op,"SSIDHEX_PASSHEX_HOST_REQUIRED");return true;}
    RemoteNetConfig n{};bool passOK=(strcmp(ph,"-")==0)?(n.password[0]=0,true):remoteHexDecode(ph,n.password,sizeof(n.password));if(!remoteHexDecode(sh,n.ssid,sizeof(n.ssid))||!passOK||strlen(host)>=sizeof(n.hostname)){ack(req,false,op,"INVALID_WIFI_ENCODING_OR_LENGTH");return true;}snprintf(n.hostname,sizeof(n.hostname),"%s",host);remoteNet=n;wifiDiagPinned=false;wifiRecoveryAttempts=0;remoteNetValid=remoteSaveNet();if(!remoteNetValid){ack(req,false,op,"NVS_WRITE_FAILED");return true;}remoteState=RemoteState::MAINTENANCE;ack(req,true,op);remoteWifiStop();remoteWifiStart();return true;
  }
  if(strcmp(op,"WIFICLEAR")==0){if(active){ack(req,false,op,"STOP_FIRST");return true;}bool ok=remoteEraseNet();remoteNetValid=false;remoteWifiStop();remoteState=RemoteState::MAINTENANCE;ack(req,ok,op,ok?"OK":"NVS_REMOVE_FAILED");return true;}
  if(strcmp(op,"REMOTEPREP")==0){
    uint32_t sid,count;if(active){ack(req,false,op,"STOP_FIRST");return true;}if(!parseU32(cur,sid)||sid!=sessionId||!parseU32(cur,count)||count<1||count>REMOTE_MAX_TRANSACTIONS){ack(req,false,op,"CONFIG_THEN_SESSION_COUNT_1_256");return true;}
    remoteRoundtripCount=0;remoteExpected=count;remoteRequiredBytes=size_t(count)*sizeof(RemoteCompactRecord);remoteRecordCount=0;remoteRunReady=false;remotePrepared=false;remoteBufferReady=false;remoteMeasureWifiOffConfirmed=false;if(remoteFsReady&&LittleFS.exists(REMOTE_RUN_FILE))LittleFS.remove(REMOTE_RUN_FILE);remotePreparePending=true;snprintf(remoteLastError,sizeof(remoteLastError),"NONE");ack(req,true,op,"WIFI_WILL_CYCLE");return true;
  }
  if(strcmp(op,"REMOTEWAIT")==0){
    uint32_t sid,count;if(!active||!peerBoot){ack(req,false,op,"ARM_FIRST");return true;}if(!remotePrepared||!remoteBufferReady){ack(req,false,op,"REMOTEPREP_FIRST");return true;}if(!parseU32(cur,sid)||sid!=sessionId||!parseU32(cur,count)||count!=remoteExpected){ack(req,false,op,"SESSION_COUNT_MISMATCH");return true;}
    remoteRunCfg=cfg;remoteRunSession=sessionId;remoteRunPeer=peerBoot;remoteRunAntTx=antTxReadback;remoteRunAntRx=antRxReadback;remoteRunAntOK=antReadbackOK;remoteRunAatten=gR7ProbeAAttenX2;remoteRunBatten=gR7ProbeBAttenX2;remoteRunManual=gR7PollAudit.manual_bit;remoteRunPowerOK=gR7PowerPlanValid;
    remoteRecordCount=0;remoteWifiMeasureViolations=0;remoteSessionStartRx=RemoteRxCompact{};remoteWaitPending=true;remoteWaitDeadlineMs=millis()+300;remoteRunReady=false;remoteState=RemoteState::ARMED;ack(req,true,op,"WIFI_WILL_TURN_OFF_WAIT_UWB");return true;
  }
  if(r03AnchorCommand(req,op,cur))return true;
  if(strcmp(op,"GETCOMPACT")==0){
    uint32_t sid;if(!parseU32(cur,sid)||sid!=remoteRunSession||!remoteRunReady){remoteDirectAck(req,false,op,"RUN_NOT_READY");return true;}
    File f=LittleFS.open(REMOTE_RUN_FILE,"r");if(!f){remoteDirectAck(req,false,op,"FILE_OPEN_FAILED");return true;}
    uint8_t raw[256];char msg[680];uint32_t offset=0;const uint32_t total=f.size();uint32_t transferCrc=0xffffffffUL;
    while(f.available()){size_t n=f.read(raw,sizeof(raw));if(!n){f.close();remoteDirectAck(req,false,op,"FILE_READ_FAILED");return true;}
      int k=snprintf(msg,sizeof(msg),"{\"event\":\"COMPACT_CHUNK\",\"session\":%lu,\"offset\":%lu,\"hex\":\"",(unsigned long)sid,(unsigned long)offset);
      for(size_t i=0;i<n;i++)k+=snprintf(msg+k,sizeof(msg)-k,"%02x",raw[i]);msg[k++]='"';msg[k++]='}';msg[k]=0;
      if(!remoteWriteWireOnly(1,msg,k,nullptr,0)){f.close();return true;}transferCrc=crcUpdate(transferCrc,raw,n);offset+=n;delay(1);
    }f.close();
    int k=snprintf(msg,sizeof(msg),"{\"event\":\"ACK\",\"request\":%lu,\"op\":\"GETCOMPACT\",\"ok\":true,\"session\":%lu,\"bytes\":%lu,\"total_bytes\":%lu,\"crc32\":%lu}",(unsigned long)req,(unsigned long)sid,(unsigned long)offset,(unsigned long)total,(unsigned long)(transferCrc^0xffffffffUL));
    remoteWriteWireOnly(1,msg,k,nullptr,0);return true;
  }
  if(strcmp(op,"GETRUN")==0){uint32_t sid;if(!parseU32(cur,sid)||sid!=remoteRunSession){remoteDirectAck(req,false,op,"SESSION_MISMATCH");return true;}return remoteDumpRun(req);}
  if(strcmp(op,"CLEARRUN")==0){if(active||remoteWaitingUwb||remoteCaptureEnabled){ack(req,false,op,"RUN_ACTIVE");return true;}remoteClearRunState();ack(req,true,op);return true;}
  return false;
}

static void remoteServiceCommands(){
  remoteWifiPollState();if(!remoteWifiReady||!remoteClient||!remoteClient.connected())return;
  // A forgotten maintenance client cannot monopolize the one-client server forever.
  if(millis()-remoteTcpLastRxMs>30000&&!remoteGetpointStreaming){remoteClient.stop();return;}
  while(remoteClient.available()){
    int c=remoteClient.read();if(c<0)break;remoteTcpLastRxMs=millis();
    if(c=='\r')continue;
    if(c=='\n'){
      if(!remoteLineOverflow){
        remoteLine[remoteLinePos]=0;
        unsigned long request=0;char op[32]{};
        if(sscanf(remoteLine,"%31s %lu",op,&request)==2){
          remoteTcpLastReq=uint32_t(request);snprintf(remoteTcpLastOp,sizeof(remoteTcpLastOp),"%s",op);++remoteTcpRequests;
        }
        gR05CommandTask=xTaskGetCurrentTaskHandle();gR04CommandFromTcp=true;
        command(remoteLine);
        gR04CommandFromTcp=false;gR05CommandTask=nullptr;
      }else snprintf(remoteWifiError,sizeof(remoteWifiError),"TCP_COMMAND_LINE_OVERFLOW");
      remoteLinePos=0;remoteLineOverflow=false;continue;
    }
    if(remoteLinePos+1<sizeof(remoteLine))remoteLine[remoteLinePos++]=char(c);else remoteLineOverflow=true;
  }
}

static void remoteSetup(){remoteTxMutex=xSemaphoreCreateMutex();WiFi.onEvent(wifiDiagEvent);remoteState=RemoteState::MAINTENANCE;remoteFsReady=LittleFS.begin(false);if(!remoteFsReady){remoteState=RemoteState::FAULT;snprintf(remoteLastError,sizeof(remoteLastError),"LITTLEFS_MOUNT_FAILED");}if(remoteFsReady)r03RecoverAfterFsMount();r04OtaLoadKey();remoteNetValid=remoteLoadNet();if(remoteNetValid)remoteWifiStart();}

static void remoteDoPrepare(){
  remotePreparePending=false;delay(300);wifiRecoveryAttempts=0;remoteWifiStop();remoteFreeRam();delay(120);size_t need=remoteRequiredBytes;
  size_t total=heap_caps_get_free_size(MALLOC_CAP_DEFAULT),largest=heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
  bool allocated=remoteAllocate();
  event("{\"event\":\"MEM_PREP_DIAG\",\"count\":%lu,\"record_bytes\":479,\"required_bytes\":%lu,\"free_default\":%lu,\"largest_default\":%lu,\"free_after\":%lu,\"block_count\":%lu,\"result_size\":%u,\"pool_bytes\":%u,\"allocated\":%s,\"fs_ready\":%s}",(unsigned long)remoteExpected,(unsigned long)need,(unsigned long)total,(unsigned long)largest,(unsigned long)remoteFreeAfter,(unsigned long)remoteBlockCount,unsigned(sizeof(Result)),unsigned(sizeof(pool)),allocated?"true":"false",remoteFsReady?"true":"false");
  remoteFreeRam();
  if(allocated&&remoteFsReady){remoteBufferReady=true;remotePrepared=true;remoteCapacity=remoteExpected;remoteState=RemoteState::PREPARED;snprintf(remoteLastError,sizeof(remoteLastError),"NONE");}
  else {remoteBufferReady=false;remotePrepared=false;remoteState=RemoteState::FAULT;snprintf(remoteLastError,sizeof(remoteLastError),remoteFsReady?"BUFFER_PREP_FAILED":"LITTLEFS_NOT_READY");}
  remoteReconnectAtMs=millis()+250;
}
static void remoteBeginWait(){
  remoteWaitPending=false;while(outputPending.load()!=0||writerBusy.load())delay(1);remoteWifiStop();
  remoteMeasureWifiOffConfirmed=(!remoteWifiReady&&WiFi.getMode()==WIFI_OFF);
  if(!remoteMeasureWifiOffConfirmed){remoteWaitingUwb=false;remoteCaptureEnabled=false;active=false;remaining=0;remoteState=RemoteState::FAULT;snprintf(remoteLastError,sizeof(remoteLastError),"WIFI_NOT_OFF_BEFORE_MEASURE");remoteReconnectAtMs=millis()+250;return;}
  remoteFreeRam();delay(100);bool allocated=remoteAllocate();
  if(!allocated){remoteWaitingUwb=false;remoteCaptureEnabled=false;active=false;remaining=0;remotePrepared=false;remoteBufferReady=false;remoteState=RemoteState::FAULT;snprintf(remoteLastError,sizeof(remoteLastError),"BUFFER_ALLOC_FINAL_FAILED_%lu",(unsigned long)remoteRequiredBytes);remoteReconnectAtMs=millis()+250;return;}
  remoteCapacity=remoteExpected;remoteBufferReady=true;remoteWaitingUwb=true;remoteCaptureEnabled=false;remoteState=RemoteState::WAIT_UWB;remoteWaitStartedMs=millis();idleRadio();armRx();
}
static void remoteListenSessionStart(){
  if(!remoteWaitingUwb)return;AirPacket p{};RxObs o{};int got=pollPacket(p,o);
  if(got==1){
    if(p.h.type==SESSION_START&&p.h.txn==0&&p.ts[0]==remoteExpected){diagnostics(o);remoteCopyRx(o,remoteSessionStartRx);finishRx();remoteWaitingUwb=false;remoteCaptureEnabled=true;remoteState=RemoteState::MEASURE;txnId=0;remaining=remoteExpected;nextStart=nowUs()+200000;snprintf(remoteLastError,sizeof(remoteLastError),"NONE");}
    else {++unexpected;finishRx();armRx();}
  } else if(got<0)armRx();
  if(remoteWaitingUwb&&millis()-remoteWaitStartedMs>REMOTE_SESSION_START_TIMEOUT_MS){idleRadio();remoteWaitingUwb=false;remoteCaptureEnabled=false;active=false;remaining=0;remoteState=RemoteState::FAULT;snprintf(remoteLastError,sizeof(remoteLastError),"SESSION_START_TIMEOUT");remoteReconnectAtMs=millis()+250;}
}
static bool remotePersistRun(){
 if(!remoteFsReady||!remoteRecords||remoteRecordCount!=remoteExpected||remoteRoundtripCount!=remoteExpected)return false;
 File f=LittleFS.open(REMOTE_RUN_FILE,"w");if(!f)return false;
 RemoteRunFileHeader h{};h.count=remoteRecordCount;h.session=remoteRunSession;h.config_id=configId(remoteRunCfg);h.wifi_measure_violation_count=remoteWifiMeasureViolations;h.session_start_rx=remoteSessionStartRx;
 uint32_t crc=0xffffffffUL,block=0xffffffffUL;
 for(uint32_t i=0;i<remoteRecordCount;i++){const auto& rec=remoteRecordAt(i);crc=crcUpdate(crc,&rec,sizeof(rec));block=crcUpdate(block,&rec,sizeof(rec));if(i%16==15||i+1==remoteRecordCount){h.block_crc32[i/16]=block^0xffffffffUL;block=0xffffffffUL;}}
 h.records_crc32=crc^0xffffffffUL;bool ok=f.write((const uint8_t*)&h,sizeof(h))==sizeof(h);
 for(uint32_t i=0;i<remoteRecordCount&&ok;i++){const auto& rec=remoteRecordAt(i);ok=f.write(rec.data,sizeof(rec))==sizeof(rec);}
 f.flush();f.close();return ok;
}
static void remoteOnRunDone(){if(r03SessionActive){r03OnPointRunDone();return;}remoteCaptureEnabled=false;bool countOK=(remoteRecordCount==remoteExpected);bool persisted=countOK&&remotePersistRun();remoteRunReady=countOK&&persisted;remoteState=remoteRunReady?RemoteState::TRANSFER:RemoteState::FAULT;if(!countOK)snprintf(remoteLastError,sizeof(remoteLastError),"RUN_RECORD_COUNT_%lu_EXPECT_%lu",(unsigned long)remoteRecordCount,(unsigned long)remoteExpected);else if(!persisted)snprintf(remoteLastError,sizeof(remoteLastError),"RUN_FILE_PERSIST_FAILED");remoteFreeRam();remoteReconnectAtMs=millis()+500;}
static void remoteLoop(){
  remoteServiceCommands();
  if(remoteCyclePending&&int32_t(millis()-remoteCycleDueMs)>=0){
    if(active||remoteWaitingUwb||remoteCaptureEnabled){remoteCyclePending=false;}
    else if(outputPending.load()==0&&!writerBusy.load()){
      remoteCyclePending=false;remoteWifiStop();remoteWifiStart();
    }else if(millis()-remoteCycleDueMs>2000){
      remoteCyclePending=false;snprintf(remoteWifiError,sizeof(remoteWifiError),"WIFI_CYCLE_OUTPUT_DRAIN_TIMEOUT");
    }
  }
  if(remotePreparePending&&outputPending.load()==0&&!writerBusy.load())remoteDoPrepare();
  if(remoteWaitPending&&int32_t(millis()-remoteWaitDeadlineMs)>=0&&outputPending.load()==0&&!writerBusy.load())remoteBeginWait();
  if(remoteWaitingUwb)remoteListenSessionStart();
  r03AnchorLoop();
  r04OtaLoop();
  if(remoteReconnectAtMs&&int32_t(millis()-remoteReconnectAtMs)>=0&&!active&&!remoteWaitingUwb&&!r03SessionActive){remoteReconnectAtMs=0;if(remoteNetValid&&!remoteWifiReady&&WiFi.getMode()==WIFI_OFF&&!remoteWifiStarting)remoteWifiStart();}
}
