#line 1 "C:\\UWB\\UWB_R04_R1J6_FIELD_FIX_20261010\\R1M_CTRL_V2_RC1\\built\\sketches\\ANCHOR_RF_PLATFORM_V1\\ota_manager.hpp"
#pragma once
// R1J.4: guarded Anchor application OTA over ArduinoOTA/espota (UDP 3232).
// Included after route_v1.hpp INSIDE namespace bench; ArduinoOTA.h must be
// included outside the namespace in bench_impl.hpp.
//
// Design:
// - OTA is disabled until an SHA-256 password hash has been provisioned via physical USB Serial.
// - OTA UDP service runs only in an explicit 60..600 s maintenance window.
// - No active/prepared RF route, no flash RF files and no legacy run file may be present.
// - While window open, command() blocks all RF/control state mutations.
// - File-system OTA is explicitly aborted in onStart() BEFORE the first flash write.
// - Dual OTA app slots are runtime-checked (no partition table rewrite).
// - Only Anchor is modified. Tag/RF air packet timing are untouched.

static constexpr uint16_t R04_OTA_PORT=3232;
static constexpr const char* R04_OTA_BUILD="R1M_CTRL_V2";
static char gR04OtaHash[65]{};
static bool gR04OtaHasKey=false;
static bool gR04OtaWindow=false;
static bool gR04OtaReceiving=false;
static uint32_t gR04OtaDeadlineMs=0;
static uint32_t gR04OtaLastError=0;

static bool r04OtaHashValid(const char* s){
  if(strlen(s)!=64)return false;
  for(size_t i=0;i<64;i++)if(!isxdigit(static_cast<unsigned char>(s[i])))return false;
  return true;
}
static void r04OtaLoadKey(){
  gR04OtaHasKey=false;gR04OtaHash[0]=0;
  Preferences p;if(!p.begin("rfota",true))return;
  String stored=p.getString("sha256","");p.end();
  if(!r04OtaHashValid(stored.c_str()))return;
  stored.toLowerCase();memcpy(gR04OtaHash,stored.c_str(),64);gR04OtaHash[64]=0;gR04OtaHasKey=true;
}
static bool r04OtaWindowActive(){return gR04OtaWindow;}
static void r04OtaCloseWindow(){
  if(gR04OtaWindow&&!gR04OtaReceiving)ArduinoOTA.end();
  gR04OtaWindow=false;gR04OtaDeadlineMs=0;
}
static const char* r04OtaSafeReason(){
  if(!gR04OtaHasKey)return "OTA_KEY_NOT_CONFIGURED";
  if(!remoteNetValid||!remoteWifiReady||WiFi.status()!=WL_CONNECTED||uint32_t(WiFi.localIP())==0)return "WIFI_NOT_READY";
  if(active||remaining||remoteWaitingUwb||remoteCaptureEnabled||remoteWaitPending||remotePreparePending||remotePrepared||
     r03SessionActive||r03WaitPending||r03Prepared||r03PointMeasuring||r03WaitingPoint)return "RF_ACTIVE_OR_PREPARED";
  if(remoteGetpointStreaming||writerBusy.load()||outputPending.load()>10)return "OUTPUT_BUSY";
  if(!remoteFsReady)return "FS_NOT_READY";
  const R03FsStats fs=r03FsStats();
  if(!fs.root_ok)return "FS_SCAN_FAILED";
  if(fs.r03_files!=0||r03ManifestValid||LittleFS.exists(REMOTE_RUN_FILE))return "RF_FLASH_DATA_PRESENT";
  const esp_partition_t* running=esp_ota_get_running_partition();
  const esp_partition_t* next=esp_ota_get_next_update_partition(nullptr);
  if(!running||!next||next==running||next->size<0x100000)return "OTA_PARTITION_MISSING";
  return "OK";
}
static void r04OtaStatus(uint32_t req,const char* op){
  const esp_partition_t* running=esp_ota_get_running_partition();
  const esp_partition_t* next=esp_ota_get_next_update_partition(nullptr);
  const char* safe=r04OtaSafeReason();
  const uint32_t left=gR04OtaWindow&&!gR04OtaReceiving&&int32_t(gR04OtaDeadlineMs-millis())>0
      ? uint32_t(gR04OtaDeadlineMs-millis())/1000:0;
  event("{\"event\":\"ACK\",\"request\":%lu,\"op\":\"%s\",\"ok\":true,\"role\":\"anchor\",\"ota_api\":1,\"ota_build\":\"%s\",\"configured\":%s,\"window_open\":%s,\"updating\":%s,\"seconds_left\":%lu,\"safe_reason\":\"%s\",\"running_offset\":%lu,\"next_offset\":%lu,\"next_size\":%lu,\"ota_port\":%u,\"last_error\":%lu}",
    (unsigned long)req,op,R04_OTA_BUILD,gR04OtaHasKey?"true":"false",gR04OtaWindow?"true":"false",gR04OtaReceiving?"true":"false",
    (unsigned long)left,safe,(unsigned long)(running?running->address:0),(unsigned long)(next?next->address:0),
    (unsigned long)(next?next->size:0),unsigned(R04_OTA_PORT),(unsigned long)gR04OtaLastError);
}
static bool r04OtaStartWindow(uint32_t seconds,const char** reason){
  if(gR04OtaReceiving){*reason="OTA_IN_PROGRESS";return false;}
  if(gR04OtaWindow)r04OtaCloseWindow();
  const char* safe=r04OtaSafeReason();
  if(strcmp(safe,"OK")!=0){*reason=safe;return false;}
  // ESP32 Core 3.3.7 Updater.cpp ignores partition_label!
  // onStart() below MUST abort filesystem-image requests before any write.
  ArduinoOTA.setPort(R04_OTA_PORT);
  ArduinoOTA.setHostname(remoteNet.hostname);
  ArduinoOTA.setPasswordHash(gR04OtaHash);
  ArduinoOTA.setMdnsEnabled(false);
  ArduinoOTA.setRebootOnSuccess(true);
  ArduinoOTA.onStart([](){
    // ArduinoOTA calls onStart AFTER Update.begin but BEFORE write. Reject SPIFFS
    // so neither LittleFS nor any other data partition can be touched.
    if(ArduinoOTA.getCommand()!=U_FLASH){
      Update.abort();gR04OtaLastError=101;gR04OtaReceiving=false;return;
    }
    gR04OtaReceiving=true;
  });
  ArduinoOTA.onEnd([](){gR04OtaReceiving=false;});
  ArduinoOTA.onError([](ota_error_t err){gR04OtaLastError=uint32_t(err)+1;gR04OtaReceiving=false;});
  ArduinoOTA.begin();
  gR04OtaDeadlineMs=millis()+seconds*1000UL;
  gR04OtaWindow=true;
  *reason="OK";return true;
}
static bool r04OtaCommand(uint32_t req,const char* op,char*& cur){
  if(strcmp(op,"OTASTATUS")==0){r04OtaStatus(req,op);return true;}
  if(strcmp(op,"OTASET")==0){
    if(gR04CommandFromTcp){ack(req,false,op,"SERIAL_ONLY");return true;}
    if(active||r03SessionActive||r03WaitPending||r03Prepared||gR04OtaWindow){ack(req,false,op,"STOP_FIRST");return true;}
    char hash[70]{};if(!remoteToken(cur,hash,sizeof(hash))||!r04OtaHashValid(hash)){
      ack(req,false,op,"SHA256_64_HEX_REQUIRED");return true;
    }
    while(*cur&&isspace(static_cast<unsigned char>(*cur)))++cur;
    if(*cur){ack(req,false,op,"EXTRA_ARGUMENT");return true;}
    for(size_t i=0;i<64;i++)hash[i]=char(tolower(static_cast<unsigned char>(hash[i])));
    Preferences p;bool saved=false;
    if(p.begin("rfota",false)){saved=p.putString("sha256",hash)==64;p.end();}
    if(saved){memcpy(gR04OtaHash,hash,65);gR04OtaHasKey=true;}
    memset(hash,0,sizeof(hash));ack(req,saved,op,saved?"OK":"NVS_WRITE_FAILED");return true;
  }
  if(strcmp(op,"OTACLEAR")==0){
    if(gR04CommandFromTcp){ack(req,false,op,"SERIAL_ONLY");return true;}
    if(active||r03SessionActive||r03WaitPending||gR04OtaWindow){ack(req,false,op,"STOP_FIRST");return true;}
    uint32_t confirm=0;
    if(!parseU32(cur,confirm)||confirm!=1){ack(req,false,op,"CONFIRM_ONE_REQUIRED");return true;}
    Preferences p;bool done=false;
    if(p.begin("rfota",false)){done=p.remove("sha256");p.end();}
    if(done){memset(gR04OtaHash,0,sizeof(gR04OtaHash));gR04OtaHasKey=false;}
    ack(req,done,op,done?"OK":"NVS_REMOVE_FAILED");return true;
  }
  if(strcmp(op,"OTAWINDOW")==0){
    uint32_t seconds=0;if(!parseU32(cur,seconds)||seconds>600||(seconds!=0&&seconds<60)){
      ack(req,false,op,"SECONDS_0_OR_60_TO_600");return true;
    }
    if(seconds==0){if(gR04OtaReceiving)ack(req,false,op,"OTA_IN_PROGRESS");
      else{r04OtaCloseWindow();ack(req,true,op,"CLOSED");}return true;}
    const char* reason="UNKNOWN";bool ok=r04OtaStartWindow(seconds,&reason);
    ack(req,ok,op,reason);return true;
  }
  return false;
}
static void r04OtaLoop(){
  if(!gR04OtaWindow)return;
  if(!gR04OtaReceiving && (int32_t(millis()-gR04OtaDeadlineMs)>=0 ||
                            !remoteWifiReady || WiFi.status()!=WL_CONNECTED)){
    r04OtaCloseWindow();return;
  }
  ArduinoOTA.handle();
}
