#pragma once
// Mode 8 operational anchor adapter. Include INSIDE namespace dual8app after
// command(); Arduino/WiFi/Preferences/LittleFS/ArduinoOTA/mbedtls headers must
// be included before entering the namespace in app.hpp.
// All DW1000 operations remain owned by the original Mode 8 RF loop.
// This module never touches SPI, CIR accumulators or RF timestamps.
#if DUAL8_ANCHOR
enum class OpsPhase : uint8_t { Idle, Armed, Measuring, Draining, Stored, Recovery, Fault };
static OpsPhase opsPhase=OpsPhase::Idle;
static bool opsWifiUp=false,opsFsOk=false,opsOtaOpen=false;
static bool opsTcpAuthorized=false,opsTcpOverflow=false,opsArchiveFailure=false;
static bool opsWantNetwork=false;static uint32_t opsReconnectAtMs=0;
static uint32_t opsSession=0,opsCount=0,opsRows=0,opsNonce=0;
// An ACKFILE is only permitted after a complete GET on this authenticated
// TCP connection, and it must bind to the exact archived body CRC.
struct OpsFileIdentity {
 uint32_t boot=0,session=0,crc=0;
 bool operator==(const OpsFileIdentity& other) const {
  return boot==other.boot&&session==other.session&&crc==other.crc;
 }
};
static OpsFileIdentity opsLastAck{},opsDownloaded{};
static const char* opsFailure=nullptr;
static uint64_t opsStartAt=0;
static uint32_t opsOtaDeadline=0,opsTcpSince=0;
static char opsTcpLine[192]{};static size_t opsTcpUsed=0;
static constexpr uint32_t OPS_ARCHIVE_MAX=32,OPS_TCP_PORT=29310;
static constexpr uint32_t OPS_ARCHIVE_MAGIC=0x4f384d42UL; // "BM8O" LE
static constexpr uint16_t OPS_ARCHIVE_VERSION=2;
static constexpr uint16_t OPS_LEGACY_ROW_BYTES=1070;
static const char *OPS_ARCHIVE_PATH="/m8_pending.bin";
static const char *OPS_ARCHIVE_TMP="/m8_pending.tmp";
static const char *OPS_ARCHIVE_QUARANTINE="/m8_quarantine.bin";
static WiFiServer opsServer(OPS_TCP_PORT);
static WiFiClient opsClient;
struct OpsNet {
 char ssid[33]{},pass[65]{},otaHash[65]{};
 uint8_t authKey[32]{},deleteKey[32]{};
 bool authSet=false,deleteSet=false,otaSet=false;
};
static OpsNet opsNet{};

#pragma pack(push,1)
struct OpsTx {
 uint64_t planned=0,actual=0,status=0;
 uint8_t type=0,ok=0,radio=255,attempted=0;
};
struct OpsRx {
 uint64_t hw=0,raw=0,status=0;
 int32_t ci=0;
 uint16_t fp=0,nacc=0;
 uint8_t type=0,received=0,lde=0,radio=255;
 uint8_t air[dual8::AIR_BYTES]{};
};
struct OpsArchiveRow {
 uint32_t session=0,txn=0,boot=0,peer=0,config=0,errors=0;
 uint8_t complete=0,response_mask=0,response2_mask=0,flags=0;
 uint64_t initial_poll_tx=0;
 OpsTx tx[6]{};
 OpsRx rx[4]{};
 uint8_t frames[6][dual8::AIR_BYTES]{};
 uint32_t crc32=0;
};
struct OpsArchiveHeader {
 uint32_t magic=OPS_ARCHIVE_MAGIC;
 uint16_t version=OPS_ARCHIVE_VERSION;
 uint16_t row_bytes=sizeof(OpsArchiveRow);
 uint32_t session=0,boot=0,config=0,count=0,body_crc=0,header_crc=0;
};
#pragma pack(pop)
static_assert(sizeof(OpsArchiveRow)==1312,"Mode 8 archive row schema drift");
static_assert(sizeof(OpsArchiveHeader)==32,"Mode 8 archive header schema drift");
static OpsArchiveRow* opsArchive=nullptr;
static OpsFileIdentity opsIdentity(const OpsArchiveHeader& h){
 OpsFileIdentity id;id.boot=h.boot;id.session=h.session;id.crc=h.body_crc;return id;
}
static bool opsRecordsDrained(){
 return transportQuiet()&&freeQ&&uxQueueMessagesWaiting(freeQ)==POOL;
}
static void opsRequestNetwork(){
 opsWantNetwork=opsNet.ssid[0]!=0&&opsNet.pass[0]!=0;
 opsReconnectAtMs=millis()+1000UL;
}
static void opsFail(const char* reason){
 // An unstarted reservation has no acquired RF data. Release its RAM
 // if the Wi-Fi-off/start gate fails, retaining any committed files.
 if(opsPhase==OpsPhase::Armed&&!active){delete[] opsArchive;opsArchive=nullptr;}
 opsPhase=OpsPhase::Fault;opsFailure=reason;opsRequestNetwork();
}
static void opsCancelScheduledStart(){
 if(opsPhase!=OpsPhase::Armed)return;
 // Explicit USB STOP cancels an accepted TCP START before the RF deadline.
 // No RF measurement was started, so no archive evidence can be lost.
 delete[] opsArchive;opsArchive=nullptr;
 opsPhase=OpsPhase::Idle;opsSession=0;opsCount=0;opsRows=0;opsStartAt=0;
 opsRequestNetwork();
}
static bool opsArchiveEnabled(){
 return opsPhase==OpsPhase::Armed||opsPhase==OpsPhase::Measuring||
        opsPhase==OpsPhase::Draining||opsPhase==OpsPhase::Stored||
        opsPhase==OpsPhase::Recovery||opsPhase==OpsPhase::Fault;
}
static const char* opsPhaseText(){
 switch(opsPhase){
 case OpsPhase::Idle:return "IDLE";
 case OpsPhase::Armed:return "ARMED";
 case OpsPhase::Measuring:return "MEASURING";
 case OpsPhase::Draining:return "DRAINING";
 case OpsPhase::Stored:return "STORED";
 case OpsPhase::Recovery:return "RECOVERY_REQUIRED";
 default:return "FAULT";
 }
}
static void opsCopyTx(OpsTx& d,const dual8::Tx& s){
 d.planned=s.planned;d.actual=s.actual;d.status=s.status;
 d.type=s.type;d.ok=s.ok?1:0;d.radio=s.radio;d.attempted=s.attempted?1:0;
}
static void opsCopyRx(OpsRx& d,const dual8::Rx& s){
 d.hw=s.hw;d.raw=s.raw;d.status=s.status;d.ci=s.ci;
 d.fp=s.fp_raw;d.nacc=s.nacc;d.type=s.type;
 d.received=s.received?1:0;d.lde=s.lde?1:0;d.radio=s.radio;
 if(s.received)memcpy(d.air,s.air,dual8::AIR_BYTES);
}
static bool opsArchiveRecord(const dual8::Record& r){
 if(opsPhase!=OpsPhase::Measuring)return true; // bench USB mode untouched
 if(!opsArchive||r.session!=opsSession||r.txn!=opsRows||opsRows>=opsCount||
    opsRows>=OPS_ARCHIVE_MAX){opsArchiveFailure=true;opsFail("ARCHIVE_RECORD");return false;}
 OpsArchiveRow& d=opsArchive[opsRows];
 d=OpsArchiveRow{};
 d.session=r.session;d.txn=r.txn;d.boot=r.boot;d.peer=r.peer_boot;
 d.config=r.config;d.errors=r.errors;d.complete=r.complete?1:0;
 d.response_mask=r.response_mask;d.response2_mask=r.response2_mask;
 d.flags=(r.p1_retry?1:0)|(r.p2_retry?2:0);
 d.initial_poll_tx=r.initial_poll_tx;
 opsCopyTx(d.tx[0],r.tx[0]);opsCopyTx(d.tx[1],r.tx[1]);
 opsCopyTx(d.tx[2],r.poll2_tx);opsCopyTx(d.tx[3],r.tx[2]);
 opsCopyTx(d.tx[4],r.p2_repeat_tx);opsCopyTx(d.tx[5],r.p1_repeat_tx);
 opsCopyRx(d.rx[0],r.responses[0]);opsCopyRx(d.rx[1],r.responses[1]);
 opsCopyRx(d.rx[2],r.response2_rx[0]);opsCopyRx(d.rx[3],r.response2_rx[1]);
 const bool pollOk=r.tx[0].ok&&dual8::encode(r.poll_frame,d.frames[0]);
 const bool p1Ok=r.have_probe_frame[0]&&dual8::encode(r.probe_frame[0],d.frames[1]);
 const bool poll2Ok=r.have_poll2_frame&&dual8::encode(r.poll2_frame,d.frames[2]);
 const bool p2Ok=r.have_probe_frame[1]&&dual8::encode(r.probe_frame[1],d.frames[3]);
 // Never report an archive row as complete when raw RF evidence was
 // omitted, corrupted or not serializable.
 const bool p2RepeatOk=r.p2_repeat_tx.ok&&dual8::encode(r.p2_repeat_frame,d.frames[4]);
 const bool p1RepeatOk=r.p1_repeat_tx.ok&&dual8::encode(r.p1_repeat_frame,d.frames[5]);
 if((r.complete&&(!pollOk||!p1Ok||!poll2Ok||!p2Ok||!p2RepeatOk))||
    (r.p1_repeat_tx.ok&&!p1RepeatOk)){
  opsArchiveFailure=true;opsFail("ARCHIVE_FRAME");return false;
 }
 d.crc32=dual8::crc32(&d,offsetof(OpsArchiveRow,crc32));
 ++opsRows;
 return true;
}
static bool opsFilePending(){
 return opsFsOk&&(LittleFS.exists(OPS_ARCHIVE_PATH)||LittleFS.exists(OPS_ARCHIVE_TMP));
}
static bool opsAnyStoredFile(){
 if(!opsFsOk)return true;
 File root=LittleFS.open("/");
 if(!root||!root.isDirectory())return true;
 File item=root.openNextFile();
 const bool exists=bool(item);
 if(item)item.close();
 root.close();
 return exists;
}
static bool opsReadHeader(OpsArchiveHeader& h,const char* path=OPS_ARCHIVE_PATH){
 if(!opsFsOk)return false;
 File f=LittleFS.open(path,"r");
 if(!f)return false;
 const bool ok=f.size()>=sizeof h&&f.read(reinterpret_cast<uint8_t*>(&h),sizeof h)==sizeof h;
 f.close();
 if(!ok||h.magic!=OPS_ARCHIVE_MAGIC||!h.session||!h.boot||!h.config||!h.count||
    h.count>OPS_ARCHIVE_MAX)return false;
 if(!((h.version==OPS_ARCHIVE_VERSION&&h.row_bytes==sizeof(OpsArchiveRow))||
      (h.version==1&&h.row_bytes==OPS_LEGACY_ROW_BYTES)))return false;
 const uint32_t found=h.header_crc;h.header_crc=0;
 const bool valid=dual8::crc32(&h,sizeof h)==found;
 h.header_crc=found;
 return valid;
}
static bool opsVerifyArchive(const OpsArchiveHeader& h,const char* path=OPS_ARCHIVE_PATH){
 File f=LittleFS.open(path,"r");
 if(!f)return false;
 const size_t expected=sizeof h+size_t(h.count)*h.row_bytes;
 if(size_t(f.size())!=expected){f.close();return false;}
 if(!f.seek(sizeof h,SeekSet)){f.close();return false;}
 uint8_t buffer[256];uint32_t crc=0xffffffffUL;
 size_t remaining=expected-sizeof h;
 while(remaining){
  const size_t n=remaining>sizeof buffer?sizeof buffer:remaining;
  if(f.read(buffer,n)!=int(n)){f.close();return false;}
  crc=dual8::crcStep(crc,buffer,n);remaining-=n;
 }
 f.close();return (crc^0xffffffffUL)==h.body_crc;
}
// Only promote a complete, checksum-verified journal. A damaged journal
// stays on flash and requires an explicit operator recovery decision.
static bool opsRecoverJournal(){
 if(!opsFsOk||LittleFS.exists(OPS_ARCHIVE_PATH))return false;
 OpsArchiveHeader h;
 return opsReadHeader(h,OPS_ARCHIVE_TMP)&&opsVerifyArchive(h,OPS_ARCHIVE_TMP)&&
        LittleFS.rename(OPS_ARCHIVE_TMP,OPS_ARCHIVE_PATH);
}
static void opsLoadArchiveState(){
 if(LittleFS.exists(OPS_ARCHIVE_TMP)&&!LittleFS.exists(OPS_ARCHIVE_PATH))
  opsRecoverJournal();
 OpsArchiveHeader h;
 if(opsReadHeader(h)&&opsVerifyArchive(h)){
  opsPhase=OpsPhase::Stored;opsSession=h.session;opsCount=opsRows=h.count;
 }else if(opsFilePending()){
  opsPhase=OpsPhase::Recovery;opsFailure="ARCHIVE_RECOVERY_REQUIRED";
 }
}
static bool opsPersistArchive(){
 if(!opsArchive||!opsFsOk||opsFilePending()||opsRows!=opsCount||
    opsArchiveFailure||opsCount>OPS_ARCHIVE_MAX)return false;
 OpsArchiveHeader h;
 h.session=opsSession;h.boot=boot;h.config=configId(config);h.count=opsRows;
 h.body_crc=dual8::crc32(opsArchive,size_t(opsRows)*sizeof(OpsArchiveRow));
 h.header_crc=0;h.header_crc=dual8::crc32(&h,sizeof h);
 File f=LittleFS.open(OPS_ARCHIVE_TMP,"w");
 if(!f)return false;
 const size_t bytes=size_t(opsRows)*sizeof(OpsArchiveRow);
 const bool ok=f.write(reinterpret_cast<const uint8_t*>(&h),sizeof h)==sizeof h
              &&f.write(reinterpret_cast<const uint8_t*>(opsArchive),bytes)==bytes;
 f.flush();f.close();
 if(!ok||!opsVerifyArchive(h,OPS_ARCHIVE_TMP))return false;
 if(!LittleFS.rename(OPS_ARCHIVE_TMP,OPS_ARCHIVE_PATH))return false;
 // A reboot sees either a verified committed file or a pending .tmp;
 // it must never silently overwrite an interrupted journal.
 return opsVerifyArchive(h,OPS_ARCHIVE_PATH);
}
static bool opsHex(const char*hex,uint8_t*out,size_t capacity,size_t&length){
 length=0;if(!hex)return false;
 const size_t n=strlen(hex);if(n==0||n%2||n/2>capacity)return false;
 auto digit=[](char c)->int{
  if(c>='0'&&c<='9')return c-'0';
  if(c>='a'&&c<='f')return c-'a'+10;
  if(c>='A'&&c<='F')return c-'A'+10;
  return -1;
 };
 for(size_t i=0;i<n;i+=2){
  const int a=digit(hex[i]),b=digit(hex[i+1]);
  if(a<0||b<0)return false;
  out[length++]=uint8_t(a*16+b);
 }
 return true;
}
static void opsLoadNet(){
 Preferences p;if(!p.begin("mode8ops",true))return;
 p.getString("ssid",opsNet.ssid,sizeof opsNet.ssid);
 p.getString("pass",opsNet.pass,sizeof opsNet.pass);
 opsNet.authSet=p.getBytesLength("auth")==sizeof opsNet.authKey
     &&p.getBytes("auth",opsNet.authKey,sizeof opsNet.authKey)==sizeof opsNet.authKey;
 opsNet.deleteSet=p.getBytesLength("deletekey")==sizeof opsNet.deleteKey
     &&p.getBytes("deletekey",opsNet.deleteKey,sizeof opsNet.deleteKey)==sizeof opsNet.deleteKey;
 p.getString("otahash",opsNet.otaHash,sizeof opsNet.otaHash);
 opsNet.otaSet=strlen(opsNet.otaHash)==64;
 // Ignore the old session-only approval. A legacy file must be fetched again.
 if(p.getBytesLength("file_ack_v2")==sizeof opsLastAck&&
    p.getBytes("file_ack_v2",&opsLastAck,sizeof opsLastAck)!=sizeof opsLastAck)
  opsLastAck={};
 p.end();
}
static bool opsSaveHexField(const char*name,const char*hex){
 uint8_t bytes[65]{};size_t n=0;
 if(!opsHex(hex,bytes,sizeof bytes,n))return false;
 Preferences p;if(!p.begin("mode8ops",false))return false;
 bool ok=false;
 if(!strcmp(name,"SSID")&&n>=1&&n<=32){
  bytes[n]=0;ok=p.putString("ssid",reinterpret_cast<const char*>(bytes))==n;
  if(ok)memcpy(opsNet.ssid,bytes,n+1);
 }else if(!strcmp(name,"PASS")&&n>=8&&n<=63){
  bytes[n]=0;ok=p.putString("pass",reinterpret_cast<const char*>(bytes))==n;
  if(ok)memcpy(opsNet.pass,bytes,n+1);
 }else if(!strcmp(name,"AUTH")&&n==32){
  ok=p.putBytes("auth",bytes,32)==32;
  if(ok){memcpy(opsNet.authKey,bytes,32);opsNet.authSet=true;}
 }else if(!strcmp(name,"DELETEKEY")&&n==32){
  // Deletion custody uses a separate physical USB provisioned secret.
  if(!opsNet.authSet||memcmp(bytes,opsNet.authKey,32)!=0){
   ok=p.putBytes("deletekey",bytes,32)==32;
   if(ok){memcpy(opsNet.deleteKey,bytes,32);opsNet.deleteSet=true;}
  }
 }else if(!strcmp(name,"OTAHASH")&&n==32){
  // OTA uses the hex text of a 32-byte SHA-256 password digest.
  ok=p.putString("otahash",hex)==64;
  if(ok){strncpy(opsNet.otaHash,hex,64);opsNet.otaHash[64]=0;opsNet.otaSet=true;}
 }
 p.end();memset(bytes,0,sizeof bytes);return ok;
}
static bool opsWifiDown(){
 opsWantNetwork=false;
 if(opsOtaOpen){ArduinoOTA.end();opsOtaOpen=false;}
 if(opsClient)opsClient.stop();
 opsServer.end();WiFi.disconnect(false,false);
 const bool modeSet=WiFi.mode(WIFI_OFF);
 opsWifiUp=false;opsTcpAuthorized=false;opsTcpUsed=0;opsDownloaded={};
 return modeSet&&WiFi.getMode()==WIFI_OFF;
}
static bool opsWifiStart(){
 if(active||!opsRecordsDrained()||opsPhase==OpsPhase::Measuring||
    !opsNet.ssid[0]||!opsNet.pass[0])return false;
 WiFi.mode(WIFI_STA);WiFi.setAutoReconnect(false);
 WiFi.begin(opsNet.ssid,opsNet.pass);
 opsWifiUp=false;opsWantNetwork=true;opsReconnectAtMs=millis()+15000UL;return true;
}
static bool opsTcpReply(const char*line){
 return opsClient&&opsClient.connected()&&opsClient.print(line)==strlen(line)&&
        opsClient.print('\n')==1;
}
static void opsTcpStatus(){
 if(!opsClient)return;
 char out[576];
 OpsArchiveHeader h;const bool stored=opsReadHeader(h)&&opsVerifyArchive(h);
 snprintf(out,sizeof out,
  "{\"event\":\"OPS_STATUS\",\"phase\":\"%s\",\"boot\":%lu,\"session\":%lu,\"records\":%lu,"
  "\"config\":%lu,\"wifi\":%s,\"file\":%s,\"file_session\":%lu,\"file_count\":%lu,"
  "\"file_acked\":%s,\"file_crc\":%lu,\"file_boot\":%lu,\"deletion_key_ready\":%s,\"pending\":%lu,\"awaiting_ack\":%u,\"free_slots\":%u,"
  "\"firmware\":\"%s\",\"stage\":%u,\"period_us\":%lu,\"baud\":%lu,"
  "\"hw_ok\":%s,\"fault_latched\":%s,\"period_miss\":%lu,\"drained\":%s,\"failure\":\"%s\"}",
  opsPhaseText(),(unsigned long)boot,(unsigned long)opsSession,(unsigned long)opsRows,
  (unsigned long)configId(config),WiFi.status()==WL_CONNECTED?"true":"false",
  stored?"true":"false",(unsigned long)(stored?h.session:0),(unsigned long)(stored?h.count:0),
  stored&&opsLastAck==opsIdentity(h)?"true":"false",
  (unsigned long)(stored?h.body_crc:0),(unsigned long)(stored?h.boot:0),
  opsNet.deleteSet?"true":"false",(unsigned long)pending.load(),
  awaitingAck.load(),freeQ?unsigned(uxQueueMessagesWaiting(freeQ)):0,
  firmwareId(true),config.stage,(unsigned long)config.period_us,(unsigned long)config.baud,
  hwOk?"true":"false",port.faulted()?"true":"false",(unsigned long)periodMiss,
  drainComplete&&drainClean&&opsRecordsDrained()?"true":"false",opsFailure?opsFailure:"");
 opsTcpReply(out);
}
static bool opsHmac(const char*hex){
 if(!opsNet.authSet||strlen(hex)!=64)return false;
 uint8_t expected[32]{};size_t n=0;
 if(!opsHex(hex,expected,sizeof expected,n)||n!=32)return false;
 char msg[64];
 snprintf(msg,sizeof msg,"MODE8:%08lX:%08lX",(unsigned long)opsNonce,(unsigned long)boot);
 uint8_t digest[32]{};
 const mbedtls_md_info_t*info=mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
 if(!info||mbedtls_md_hmac(info,opsNet.authKey,32,reinterpret_cast<const unsigned char*>(msg),
                            strlen(msg),digest)!=0)return false;
 uint8_t diff=0;for(unsigned i=0;i<32;++i)diff|=expected[i]^digest[i];
 return diff==0;
}
// The control-login key alone never grants archival approval/deletion.
// Every proof is bound to the current TCP nonce, boot, session and CRC.
static bool opsCustodyProof(const char* token,const char* verb,const OpsFileIdentity& id){
 if(!opsNet.deleteSet||!token||strlen(token)!=64)return false;
 uint8_t claimed[32]{};size_t n=0;
 if(!opsHex(token,claimed,sizeof claimed,n)||n!=32)return false;
 char message[120];
 const int length=snprintf(message,sizeof message,
   "MODE8-CUSTODY:%s:%08lX:%08lX:%08lX:%08lX",verb,
   (unsigned long)id.boot,(unsigned long)id.session,
   (unsigned long)id.crc,(unsigned long)opsNonce);
 if(length<0||size_t(length)>=sizeof message)return false;
 uint8_t digest[32]{};
 const auto* md=mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
 if(!md||mbedtls_md_hmac(md,opsNet.deleteKey,32,
   reinterpret_cast<const unsigned char*>(message),size_t(length),digest)!=0)return false;
 uint8_t diff=0;
 for(unsigned i=0;i<32;++i)diff|=claimed[i]^digest[i];
 return diff==0;
}
static bool opsDownload(){
 opsDownloaded={};
 OpsArchiveHeader h;
 if(!opsReadHeader(h)||!opsVerifyArchive(h))return false;
 File f=LittleFS.open(OPS_ARCHIVE_PATH,"r");if(!f)return false;
 char line[90];
 snprintf(line,sizeof line,"DATA %lu %lu %lu",(unsigned long)h.session,
          (unsigned long)f.size(),(unsigned long)h.body_crc);
 if(!opsTcpReply(line)){f.close();return false;}
 uint8_t bytes[512];uint32_t idleSince=millis();bool ok=true;
 while(f.available()){
  const size_t n=f.read(bytes,sizeof bytes);if(n==0){ok=false;break;}
  size_t off=0;
  while(off<n){
   if(!opsClient.connected()||uint32_t(millis()-idleSince)>5000){ok=false;break;}
   const size_t sent=opsClient.write(bytes+off,n-off);
   if(sent){off+=sent;idleSince=millis();}else delay(1);
  }
  if(!ok)break;
 }
 f.close();
 if(!ok||!opsTcpReply("END"))return false;
 opsDownloaded=opsIdentity(h);
 return true;
}
static bool opsEraseAcked(){
 OpsArchiveHeader h;
 if(!opsReadHeader(h)||!opsVerifyArchive(h)||!(opsIdentity(h)==opsLastAck))return false;
 // Clear persistent approval before removing the file. An interruption can
 // require another GET/ACK, but cannot grant approval to a later file.
 Preferences p;
 if(!p.begin("mode8ops",false))return false;
 const bool cleared=p.remove("file_ack_v2");
 p.end();
 if(!cleared)return false;
 opsLastAck={};
 const bool ok=LittleFS.remove(OPS_ARCHIVE_PATH);
 if(ok){
  opsPhase=OpsPhase::Idle;opsSession=0;opsCount=0;opsRows=0;opsDownloaded={};
  opsFailure=nullptr;opsLoadArchiveState();
 }
 return ok;
}
// Device-only OTA partition gate. Does not enable the rollback bootloader.
static bool opsOtaPartitionsReady(){
#if defined(ESP_PLATFORM)
 const auto*running=esp_ota_get_running_partition();
 const auto*next=esp_ota_get_next_update_partition(nullptr);
 if(!running||!next||next==running||next->size<0x100000UL)return false;
 esp_ota_img_states_t state=ESP_OTA_IMG_UNDEFINED;
 if(esp_ota_get_state_partition(running,&state)==ESP_OK&&
    state==ESP_OTA_IMG_PENDING_VERIFY)return false;
 return true;
#else
 return true;
#endif
}
static void opsOtaBootAttest(){
#if defined(CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE) && CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
 const auto*running=esp_ota_get_running_partition();
 esp_ota_img_states_t state=ESP_OTA_IMG_UNDEFINED;
 if(!running||esp_ota_get_state_partition(running,&state)!=ESP_OK||
    state!=ESP_OTA_IMG_PENDING_VERIFY)return;
 if(hwOk&&opsFsOk&&port.selftest()&&opsPhase!=OpsPhase::Recovery){
  if(esp_ota_mark_app_valid_cancel_rollback()!=ESP_OK)opsFail("OTA_VALIDATION_FAILED");
 }else{
  esp_ota_mark_app_invalid_rollback_and_reboot();
  opsFail("OTA_FIRST_BOOT_SELFTEST");
 }
#endif
}
static bool opsOtaStart(uint32_t seconds){
 if(seconds<60||seconds>600||!opsNet.otaSet||!opsOtaPartitionsReady()||opsAnyStoredFile()||
    opsPhase!=OpsPhase::Idle||active||!opsRecordsDrained()||
    WiFi.status()!=WL_CONNECTED)return false;
 ArduinoOTA.setPort(3232);
 ArduinoOTA.setHostname("mode8-anchor");
 ArduinoOTA.setPasswordHash(opsNet.otaHash);
 ArduinoOTA.setMdnsEnabled(false);
 ArduinoOTA.setRebootOnSuccess(true);
 ArduinoOTA.onStart([](){
  if(ArduinoOTA.getCommand()!=U_FLASH||active||opsPhase!=OpsPhase::Idle||
     !opsRecordsDrained()||opsAnyStoredFile()){
   Update.abort();return;
  }
 });
 ArduinoOTA.onEnd([](){opsOtaOpen=false;});
 ArduinoOTA.onError([](ota_error_t){opsOtaOpen=false;});
 ArduinoOTA.begin();opsOtaOpen=true;opsOtaDeadline=millis()+seconds*1000UL;
 return true;
}
static bool opsPlan(uint32_t sessionId,uint32_t count){
 if(!opsFsOk||opsFilePending()||opsOtaOpen||opsPhase!=OpsPhase::Idle||active||
    !transportChangeAllowed()||!drainClean||!opsRecordsDrained()||!hwOk||port.faulted()||
    config.stage!=6||config.period_us!=200000||config.baud!=2000000||
    count==0||count>OPS_ARCHIVE_MAX||sessionId==0||
    validate(config))return false;
 if(!opsArchive){
  opsArchive=new(std::nothrow) OpsArchiveRow[OPS_ARCHIVE_MAX];
  if(!opsArchive)return false;
  if(ESP.getFreeHeap()<32768){delete[] opsArchive;opsArchive=nullptr;return false;}
 }
 opsSession=sessionId;opsCount=count;opsRows=0;opsArchiveFailure=false;
 opsFailure=nullptr;opsPhase=OpsPhase::Armed;
 return true;
}
static void opsTcpCommand(char *line){
 char *save=nullptr,*op=strtok_r(line," \r\t",&save);
 if(!op)return;
 if(!opsTcpAuthorized){
  if(!strcmp(op,"AUTH")){
   char*key=strtok_r(nullptr," \r\t",&save);
   const bool extra=strtok_r(nullptr," \r\t",&save)!=nullptr;
   if(!extra&&key&&opsHmac(key)){opsTcpAuthorized=true;opsTcpReply("AUTH_OK");}
   else {opsTcpReply("AUTH_FAIL");opsClient.stop();}
  }else opsTcpReply("AUTH_REQUIRED");
  return;
 }
 if(!strcmp(op,"STATUS")){
  if(strtok_r(nullptr," \r\t",&save))opsTcpReply("ERROR EXTRA_ARGS");
  else opsTcpStatus();
  return;
 }
 if(!strcmp(op,"GET")){
  if(strtok_r(nullptr," \r\t",&save)){opsTcpReply("ERROR EXTRA_ARGS");return;}
  if(!opsDownload())opsTcpReply("ERROR GET_FAILED");
  return;
 }
 if(!strcmp(op,"ACKFILE")){
  char*raw=strtok_r(nullptr," \r\t",&save);
  char*rawCrc=strtok_r(nullptr," \r\t",&save);
  char*proof=strtok_r(nullptr," \r\t",&save);
  char*extra=strtok_r(nullptr," \r\t",&save);
  uint32_t id=0,crc=0;
  OpsArchiveHeader h;
  if(!parseDecimalU32(raw,id)||!id||!parseDecimalU32(rawCrc,crc)||extra||!opsReadHeader(h)||
     !opsVerifyArchive(h)||h.session!=id||h.body_crc!=crc||
     !(opsDownloaded==opsIdentity(h))||
     !opsCustodyProof(proof,"ACK",opsIdentity(h))){
    opsTcpReply("ERROR ACKFILE");return;
  }
  const auto identity=opsIdentity(h);
  if(opsLastAck==identity){opsTcpReply("ACKFILE_OK");return;}
  Preferences p;
  if(!p.begin("mode8ops",false)){opsTcpReply("ERROR NVS");return;}
  const bool ok=p.putBytes("file_ack_v2",&identity,sizeof identity)==sizeof identity;
  p.end();
  if(ok){
   opsLastAck=identity;
   opsTcpReply("ACKFILE_OK");
  }
  else opsTcpReply("ERROR NVS");
  return;
 }
 if(!strcmp(op,"PURGE")){
  char*raw=strtok_r(nullptr," \r\t",&save);
  char*rawCrc=strtok_r(nullptr," \r\t",&save);
  char*proof=strtok_r(nullptr," \r\t",&save);
  char*extra=strtok_r(nullptr," \r\t",&save);
  uint32_t id=0,crc=0;
  OpsArchiveHeader h;
  if(extra||!parseDecimalU32(raw,id)||!id||!parseDecimalU32(rawCrc,crc)||
     !opsReadHeader(h)||!opsVerifyArchive(h)||h.session!=id||h.body_crc!=crc||
     !(opsLastAck==opsIdentity(h))||
     !opsCustodyProof(proof,"PURGE",opsIdentity(h))){
   opsTcpReply("ERROR PURGE_GUARD");return;
  }
  opsTcpReply(opsEraseAcked()?"PURGE_OK":"ERROR UNACKED_FILE");return;
 }
 if(!strcmp(op,"START")){
  char*a=strtok_r(nullptr," \r\t",&save),*b=strtok_r(nullptr," \r\t",&save);
  char*extra=strtok_r(nullptr," \r\t",&save);
  uint32_t sid=0,count=0;
  if(!parseDecimalU32(a,sid)||!sid){opsTcpReply("ERROR BAD_SESSION");return;}
  if(!parseDecimalU32(b,count)||!count||count>OPS_ARCHIVE_MAX||extra||
     !opsPlan(sid,count)){opsTcpReply("ERROR START_GUARD");return;}
  if(!opsTcpReply("START_ACCEPTED_WIFI_OFF")){opsCancelScheduledStart();return;}
  delay(15);
  if(!opsWifiDown()){opsFail("WIFI_OFF_FAILED");return;}
  opsStartAt=uint64_t(esp_timer_get_time())+250000ULL;
  return;
 }
 if(!strcmp(op,"OTA")){
  char*a=strtok_r(nullptr," \r\t",&save);
  const bool extra=strtok_r(nullptr," \r\t",&save)!=nullptr;
  uint32_t seconds=0;
  opsTcpReply(!extra&&parseDecimalU32(a,seconds)&&opsOtaStart(seconds)?"OTA_WINDOW_OPEN":"ERROR OTA_GUARD");
  return;
 }
 opsTcpReply("ERROR UNKNOWN_COMMAND");
}
static void opsTcpPoll(){
 if(!opsWifiUp)return;
 if(!opsClient||!opsClient.connected()){
  if(opsClient)opsClient.stop();
  opsClient=opsServer.accept();
  opsTcpAuthorized=false;opsTcpUsed=0;opsTcpOverflow=false;
  opsDownloaded={};
  if(opsClient){
   opsNonce=esp_random();if(!opsNonce)opsNonce=1;
   opsTcpSince=millis();
   char msg[96];snprintf(msg,sizeof msg,"CHALLENGE %08lX %08lX",
     (unsigned long)opsNonce,(unsigned long)boot);opsTcpReply(msg);
  }
 }
 if(!opsClient)return;
 if(uint32_t(millis()-opsTcpSince)>90000){opsClient.stop();return;}
 for(unsigned k=0;k<128&&opsClient.available();++k){
  const int n=opsClient.read();if(n<0)break;const char c=char(n);
  if(c=='\n'){
   if(!opsTcpOverflow){opsTcpLine[opsTcpUsed]=0;opsTcpCommand(opsTcpLine);}
   else opsTcpReply("ERROR LINE_TOO_LONG");
   opsTcpUsed=0;opsTcpOverflow=false;
  }else if(c!='\r'){
   if(opsTcpUsed+1<sizeof opsTcpLine)opsTcpLine[opsTcpUsed++]=c;
   else opsTcpOverflow=true;
  }
 }
}
static void opsSetup(){
 opsFsOk=LittleFS.begin(false); // NO autoformat; preserve existing flash data.
 opsLoadNet();
 if(opsFsOk)opsLoadArchiveState();
 opsOtaBootAttest();
 WiFi.mode(WIFI_OFF);
 opsWantNetwork=opsNet.ssid[0]!=0&&opsNet.pass[0]!=0;
 opsReconnectAtMs=millis()+2000UL;
}
static bool opsUsbCommand(const char*line){
 if(strncmp(line,"OPS",3)||!(line[3]==' '||line[3]==0))return false;
 char buffer[192];strncpy(buffer,line,sizeof buffer-1);buffer[sizeof buffer-1]=0;
 char *save=nullptr,*root=strtok_r(buffer," \r\t",&save);
 (void)root;
 char *op=strtok_r(nullptr," \r\t",&save);
 if(!op){emit("{\"event\":\"ERROR\",\"reason\":\"OPS_COMMAND_REQUIRED\"}");return true;}
 if(!strcmp(op,"STATUS")){
  emit("{\"event\":\"OPS_STATUS\",\"phase\":\"%s\",\"wifi\":%s,\"fs\":%s,\"pending_file\":%s,\"session\":%lu,\"rows\":%lu,\"failure\":\"%s\"}",
   opsPhaseText(),WiFi.status()==WL_CONNECTED?"true":"false",opsFsOk?"true":"false",
   opsFilePending()?"true":"false",(unsigned long)opsSession,(unsigned long)opsRows,opsFailure?opsFailure:"");
  return true;
 }
 if((opsOtaOpen&&strcmp(op,"STATUS")!=0)||active||!opsRecordsDrained()||opsPhase==OpsPhase::Armed||
    opsPhase==OpsPhase::Measuring||opsPhase==OpsPhase::Draining){
  emit("{\"event\":\"ERROR\",\"reason\":\"OPS_NOT_QUIESCENT\"}");return true;
 }
 if(!strcmp(op,"NET")){
  char*sub=strtok_r(nullptr," \r\t",&save);
  if(sub&&!strcmp(sub,"ON")){emit("{\"event\":\"OPS_NET\",\"ok\":%s}",opsWifiStart()?"true":"false");return true;}
  if(sub&&!strcmp(sub,"OFF")){emit("{\"event\":\"OPS_NET\",\"ok\":%s}",opsWifiDown()?"true":"false");return true;}
 }
 if(!strcmp(op,"SSID")||!strcmp(op,"PASS")||!strcmp(op,"AUTH")||
    !strcmp(op,"DELETEKEY")||!strcmp(op,"OTAHASH")){
  char*value=strtok_r(nullptr," \r\t",&save);
  // Credentials are not printed or echoed on serial.
  emit("{\"event\":\"OPS_PROVISION\",\"field\":\"%s\",\"ok\":%s}",
       op,value&&opsSaveHexField(op,value)?"true":"false");
  return true;
 }
 if(!strcmp(op,"RECOVER")){
  const bool ok=opsPhase==OpsPhase::Recovery&&opsRecoverJournal();
  if(ok){opsFailure=nullptr;opsLoadArchiveState();}
  emit("{\"event\":\"OPS_RECOVER\",\"ok\":%s}",ok?"true":"false");return true;
 }
 if(!strcmp(op,"QUARANTINE")){
  // Only move an uncommitted journal, and never overwrite a previous one.
  const bool ok=opsPhase==OpsPhase::Recovery&&!LittleFS.exists(OPS_ARCHIVE_PATH)&&
      !LittleFS.exists(OPS_ARCHIVE_QUARANTINE)&&LittleFS.rename(OPS_ARCHIVE_TMP,OPS_ARCHIVE_QUARANTINE);
  if(ok){opsPhase=OpsPhase::Idle;opsFailure=nullptr;}
  emit("{\"event\":\"OPS_QUARANTINE\",\"ok\":%s}",ok?"true":"false");return true;
 }
 if(!strcmp(op,"PURGE")){
  emit("{\"event\":\"OPS_PURGE\",\"ok\":%s}",opsEraseAcked()?"true":"false");return true;
 }
 if(!strcmp(op,"OTA")){
  char*value=strtok_r(nullptr," \r\t",&save),*end=nullptr;
  const unsigned long s=value?strtoul(value,&end,10):0;
  emit("{\"event\":\"OPS_OTA\",\"ok\":%s}",
       value&&end&&!*end&&opsOtaStart(uint32_t(s))?"true":"false");return true;
 }
 emit("{\"event\":\"ERROR\",\"reason\":\"UNKNOWN_OPS_COMMAND\"}");return true;
}
static void opsLoop(){
 if(opsPhase==OpsPhase::Armed&&!active&&uint64_t(esp_timer_get_time())>=opsStartAt){
  if(WiFi.getMode()!=WIFI_OFF){opsFail("WIFI_OFF_LOST");return;}
  // An incidental USB status reply can still be draining at the deadline.
  // Wait briefly for it before failing the armed session.
  if(!opsRecordsDrained()){
   if(uint64_t(esp_timer_get_time())>=opsStartAt+250000ULL)opsFail("RECORD_DRAIN_PENDING");
   return;
  }
  char line[70];snprintf(line,sizeof line,"START %lu %lu",
                        (unsigned long)opsSession,(unsigned long)opsCount);
  opsInternalStart=true;command(line);opsInternalStart=false;
  if(active)opsPhase=OpsPhase::Measuring;
  else opsFail("RF_START_FAILED");
 }
 if(opsPhase==OpsPhase::Measuring&&!active)opsPhase=OpsPhase::Draining;
 if(opsPhase==OpsPhase::Draining&&opsRecordsDrained()){
  drainComplete=true;drainClean=sessionFailures()==0;
  if(opsPersistArchive()){
   delete[] opsArchive;opsArchive=nullptr; // release RF archival RAM before Wi-Fi TCP maintenance
   if(!drainClean)opsFailure="TRANSPORT_FAILURE";
   opsPhase=OpsPhase::Stored;opsWifiStart();
  }else if(opsFilePending()){
   opsPhase=OpsPhase::Recovery;opsFailure="ARCHIVE_RECOVERY_REQUIRED";opsRequestNetwork();
  }else opsFail("ARCHIVE_INCOMPLETE");
 }
 if(opsOtaOpen){
  if(active||opsFilePending()||int32_t(millis()-opsOtaDeadline)>=0){
   ArduinoOTA.end();opsOtaOpen=false;
  }else ArduinoOTA.handle();
 }
 // Reconnect only in maintenance after boot or completed archival commit.
 // Never retry Wi-Fi while RF is active or an offline run is armed.
 if(opsWantNetwork&&!active&&opsPhase!=OpsPhase::Armed&&
    opsPhase!=OpsPhase::Measuring&&opsPhase!=OpsPhase::Draining&&
    WiFi.status()!=WL_CONNECTED&&
    int32_t(millis()-opsReconnectAtMs)>=0&&opsRecordsDrained()){
   opsWifiStart();
 }
 if(WiFi.status()==WL_CONNECTED&&!opsWifiUp&&!active){
  opsServer.begin();opsWifiUp=true;
 }
 if(opsWifiUp&&WiFi.status()!=WL_CONNECTED){
  opsWifiUp=false;opsClient.stop();opsServer.end();
 }
 if(!active)opsTcpPoll();
}
#endif // DUAL8_ANCHOR
