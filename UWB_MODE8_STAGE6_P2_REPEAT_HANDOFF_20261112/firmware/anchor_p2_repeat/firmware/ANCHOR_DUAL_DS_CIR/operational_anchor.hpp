#pragma once
// Mode 8 operational anchor adapter. Include INSIDE namespace dual8app after
// command(); Arduino/WiFi/Preferences/LittleFS/ArduinoOTA/mbedtls headers must
// be included before entering the namespace in app.hpp.
// All DW1000 operations remain owned by the original Mode 8 RF loop.
// This module never touches SPI, CIR accumulators or RF timestamps.
#if DUAL8_ANCHOR
enum class OpsPhase : uint8_t { Idle, Armed, Measuring, Draining, Stored, Fault };
static OpsPhase opsPhase=OpsPhase::Idle;
static bool opsWifiUp=false,opsFsOk=false,opsOtaOpen=false;
static bool opsTcpAuthorized=false,opsTcpOverflow=false,opsArchiveFailure=false;
static bool opsWantNetwork=false;static uint32_t opsReconnectAtMs=0;
static uint32_t opsSession=0,opsCount=0,opsRows=0,opsNonce=0,opsLastAck=0;
static uint64_t opsStartAt=0;
static uint32_t opsOtaDeadline=0,opsTcpSince=0;
static char opsTcpLine[192]{};static size_t opsTcpUsed=0;
static constexpr uint32_t OPS_ARCHIVE_MAX=32,OPS_TCP_PORT=29310;
static constexpr uint32_t OPS_ARCHIVE_MAGIC=0x4f384d42UL; // "BM8O" LE
static constexpr uint16_t OPS_ARCHIVE_VERSION=1;
static const char *OPS_ARCHIVE_PATH="/m8_pending.bin";
static WiFiServer opsServer(OPS_TCP_PORT);
static WiFiClient opsClient;
struct OpsNet {
 char ssid[33]{},pass[65]{},otaHash[65]{};
 uint8_t authKey[32]{};
 bool authSet=false,otaSet=false;
};
static OpsNet opsNet{};

#pragma pack(push,1)
struct OpsTx {
 uint64_t planned=0,actual=0,status=0;
 uint8_t type=0,ok=0;
};
struct OpsRx {
 uint64_t hw=0,raw=0,status=0;
 int32_t ci=0;
 uint16_t fp=0,nacc=0;
 uint8_t type=0,received=0,lde=0;
 uint8_t air[dual8::AIR_BYTES]{};
};
struct OpsArchiveRow {
 uint32_t session=0,txn=0,boot=0,peer=0,config=0,errors=0;
 uint8_t complete=0,response_mask=0,response2_mask=0,flags=0;
 OpsTx tx[5]{};
 OpsRx rx[4]{};
 uint8_t frames[4][dual8::AIR_BYTES]{};
 uint32_t crc32=0;
};
struct OpsArchiveHeader {
 uint32_t magic=OPS_ARCHIVE_MAGIC;
 uint16_t version=OPS_ARCHIVE_VERSION;
 uint16_t row_bytes=sizeof(OpsArchiveRow);
 uint32_t session=0,boot=0,config=0,count=0,body_crc=0,header_crc=0;
};
#pragma pack(pop)
static_assert(sizeof(OpsArchiveRow)==1070,"Mode 8 archive row schema drift");
static_assert(sizeof(OpsArchiveHeader)==32,"Mode 8 archive header schema drift");
static OpsArchiveRow* opsArchive=nullptr;
static bool opsArchiveEnabled(){
 return opsPhase==OpsPhase::Armed||opsPhase==OpsPhase::Measuring||
        opsPhase==OpsPhase::Draining||opsPhase==OpsPhase::Stored||opsPhase==OpsPhase::Fault;
}
static bool opsArchivedMode(){return opsArchiveEnabled();}
static const char* opsPhaseText(){
 switch(opsPhase){
 case OpsPhase::Idle:return "IDLE";
 case OpsPhase::Armed:return "ARMED";
 case OpsPhase::Measuring:return "MEASURING";
 case OpsPhase::Draining:return "DRAINING";
 case OpsPhase::Stored:return "STORED";
 default:return "FAULT";
 }
}
static void opsCopyTx(OpsTx& d,const dual8::Tx& s){
 d.planned=s.planned;d.actual=s.actual;d.status=s.status;
 d.type=s.type;d.ok=s.ok?1:0;
}
static void opsCopyRx(OpsRx& d,const dual8::Rx& s){
 d.hw=s.hw;d.raw=s.raw;d.status=s.status;d.ci=s.ci;
 d.fp=s.fp_raw;d.nacc=s.nacc;d.type=s.type;
 d.received=s.received?1:0;d.lde=s.lde?1:0;
 if(s.received)memcpy(d.air,s.air,dual8::AIR_BYTES);
}
static bool opsArchiveRecord(const dual8::Record& r){
 if(opsPhase!=OpsPhase::Measuring)return true; // bench USB mode untouched
 if(!opsArchive||r.session!=opsSession||r.txn!=opsRows||opsRows>=opsCount||
    opsRows>=OPS_ARCHIVE_MAX){opsArchiveFailure=true;opsPhase=OpsPhase::Fault;return false;}
 OpsArchiveRow& d=opsArchive[opsRows];
 memset(&d,0,sizeof d);
 d.session=r.session;d.txn=r.txn;d.boot=r.boot;d.peer=r.peer_boot;
 d.config=r.config;d.errors=r.errors;d.complete=r.complete?1:0;
 d.response_mask=r.response_mask;d.response2_mask=r.response2_mask;
 d.flags=(r.p1_retry?1:0)|(r.p2_retry?2:0)|(r.p2_repeat_used?4:0);
 opsCopyTx(d.tx[0],r.tx[0]);opsCopyTx(d.tx[1],r.tx[1]);
 opsCopyTx(d.tx[2],r.poll2_tx);opsCopyTx(d.tx[3],r.tx[2]);
 opsCopyTx(d.tx[4],r.p2_repeat_tx);
 opsCopyRx(d.rx[0],r.responses[0]);opsCopyRx(d.rx[1],r.responses[1]);
 opsCopyRx(d.rx[2],r.response2_rx[0]);opsCopyRx(d.rx[3],r.response2_rx[1]);
 dual8::encode(r.poll_frame,d.frames[0]);
 if(r.have_probe_frame[0])dual8::encode(r.probe_frame[0],d.frames[1]);
 if(r.have_poll2_frame)dual8::encode(r.poll2_frame,d.frames[2]);
 if(r.have_probe_frame[1])dual8::encode(r.probe_frame[1],d.frames[3]);
 d.crc32=dual8::crc32(&d,offsetof(OpsArchiveRow,crc32));
 ++opsRows;
 return true;
}
static bool opsFilePending(){return opsFsOk&&LittleFS.exists(OPS_ARCHIVE_PATH);}
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
static bool opsReadHeader(OpsArchiveHeader& h){
 if(!opsFilePending())return false;
 File f=LittleFS.open(OPS_ARCHIVE_PATH,"r");
 if(!f)return false;
 const bool ok=f.size()>=sizeof h&&f.read(reinterpret_cast<uint8_t*>(&h),sizeof h)==sizeof h;
 f.close();
 if(!ok||h.magic!=OPS_ARCHIVE_MAGIC||h.version!=OPS_ARCHIVE_VERSION||
    h.row_bytes!=sizeof(OpsArchiveRow)||h.count>OPS_ARCHIVE_MAX)return false;
 const uint32_t found=h.header_crc;h.header_crc=0;
 const bool valid=dual8::crc32(&h,sizeof h)==found;
 h.header_crc=found;
 return valid;
}
static bool opsVerifyArchive(const OpsArchiveHeader& h){
 File f=LittleFS.open(OPS_ARCHIVE_PATH,"r");
 if(!f)return false;
 const size_t expected=sizeof h+size_t(h.count)*h.row_bytes;
 if(size_t(f.size())!=expected){f.close();return false;}
 f.seek(sizeof h,SeekSet);
 uint8_t buffer[256];uint32_t crc=0xffffffffUL;
 size_t remaining=expected-sizeof h;
 while(remaining){
  const size_t n=remaining>sizeof buffer?sizeof buffer:remaining;
  if(f.read(buffer,n)!=int(n)){f.close();return false;}
  crc=dual8::crcStep(crc,buffer,n);remaining-=n;
 }
 f.close();return (crc^0xffffffffUL)==h.body_crc;
}
static bool opsPersistArchive(){
 if(!opsArchive||!opsFsOk||opsFilePending()||opsRows!=opsCount||
    opsArchiveFailure||opsCount>OPS_ARCHIVE_MAX)return false;
 OpsArchiveHeader h;
 h.session=opsSession;h.boot=boot;h.config=configId(config);h.count=opsRows;
 h.body_crc=dual8::crc32(opsArchive,size_t(opsRows)*sizeof(OpsArchiveRow));
 h.header_crc=0;h.header_crc=dual8::crc32(&h,sizeof h);
 File f=LittleFS.open(OPS_ARCHIVE_PATH,"w");
 if(!f)return false;
 const size_t bytes=size_t(opsRows)*sizeof(OpsArchiveRow);
 const bool ok=f.write(reinterpret_cast<const uint8_t*>(&h),sizeof h)==sizeof h
              &&f.write(reinterpret_cast<const uint8_t*>(opsArchive),bytes)==bytes;
 f.flush();f.close();
 if(!ok||!opsVerifyArchive(h))return false;
 return true;
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
 p.getString("otahash",opsNet.otaHash,sizeof opsNet.otaHash);
 opsNet.otaSet=strlen(opsNet.otaHash)==64;
 opsLastAck=p.getUInt("lastack",0);
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
 }else if(!strcmp(name,"OTAHASH")&&n==32){
  // OTA uses the hex text of a 32-byte SHA-256 password digest.
  ok=p.putString("otahash",hex)==64;
  if(ok){strncpy(opsNet.otaHash,hex,64);opsNet.otaHash[64]=0;opsNet.otaSet=true;}
 }
 p.end();memset(bytes,0,sizeof bytes);return ok;
}
static void opsWifiDown(){
 opsWantNetwork=false;
 if(opsOtaOpen){ArduinoOTA.end();opsOtaOpen=false;}
 if(opsClient)opsClient.stop();
 opsServer.end();WiFi.disconnect(true,false);WiFi.mode(WIFI_OFF);
 opsWifiUp=false;opsTcpAuthorized=false;opsTcpUsed=0;
}
static bool opsWifiStart(){
 if(active||!opsRecordsDrained()||opsPhase==OpsPhase::Measuring||
    !opsNet.ssid[0]||!opsNet.pass[0])return false;
 WiFi.mode(WIFI_STA);WiFi.setAutoReconnect(false);
 WiFi.begin(opsNet.ssid,opsNet.pass);
 opsWifiUp=false;opsWantNetwork=true;opsReconnectAtMs=millis()+15000UL;return true;
}
static void opsTcpReply(const char*line){
 if(opsClient&&opsClient.connected()){opsClient.print(line);opsClient.print('\n');}
}
static void opsTcpStatus(){
 if(!opsClient)return;
 char out[400];
 OpsArchiveHeader h;const bool stored=opsReadHeader(h)&&opsVerifyArchive(h);
 snprintf(out,sizeof out,
  "{\"event\":\"OPS_STATUS\",\"phase\":\"%s\",\"boot\":%lu,\"session\":%lu,\"records\":%lu,"
  "\"config\":%lu,\"wifi\":%s,\"file\":%s,\"file_session\":%lu,\"file_count\":%lu,"
  "\"file_acked\":%s,\"pending\":%lu,\"awaiting_ack\":%u,\"free_slots\":%u}",
  opsPhaseText(),(unsigned long)boot,(unsigned long)opsSession,(unsigned long)opsRows,
  (unsigned long)configId(config),WiFi.status()==WL_CONNECTED?"true":"false",
  stored?"true":"false",(unsigned long)(stored?h.session:0),(unsigned long)(stored?h.count:0),
  stored&&opsLastAck==h.session?"true":"false",(unsigned long)pending.load(),
  awaitingAck.load(),freeQ?unsigned(uxQueueMessagesWaiting(freeQ)):0);
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
static bool opsDownload(){
 OpsArchiveHeader h;
 if(!opsReadHeader(h)||!opsVerifyArchive(h))return false;
 File f=LittleFS.open(OPS_ARCHIVE_PATH,"r");if(!f)return false;
 char line[90];
 snprintf(line,sizeof line,"DATA %lu %lu %lu",(unsigned long)h.session,
          (unsigned long)f.size(),(unsigned long)h.body_crc);
 opsTcpReply(line);
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
 if(ok)opsTcpReply("END");
 return ok;
}
static bool opsEraseAcked(){
 OpsArchiveHeader h;
 if(!opsReadHeader(h)||!opsVerifyArchive(h)||h.session!=opsLastAck)return false;
 const bool ok=LittleFS.remove(OPS_ARCHIVE_PATH);
 if(ok){opsPhase=OpsPhase::Idle;opsSession=0;opsCount=0;opsRows=0;}
 return ok;
}
static bool opsOtaStart(uint32_t seconds){
 if(seconds<60||seconds>600||!opsNet.otaSet||opsAnyStoredFile()||
    !opsRecordsDrained()||WiFi.status()!=WL_CONNECTED)return false;
 ArduinoOTA.setPort(3232);
 ArduinoOTA.setHostname("mode8-anchor");
 ArduinoOTA.setPasswordHash(opsNet.otaHash);
 ArduinoOTA.setMdnsEnabled(false);
 ArduinoOTA.setRebootOnSuccess(true);
 ArduinoOTA.onStart([](){
  if(ArduinoOTA.getCommand()!=U_FLASH){Update.abort();return;}
  // The runtime was checked before opening the OTA window.
 });
 ArduinoOTA.onEnd([](){opsOtaOpen=false;});
 ArduinoOTA.onError([](ota_error_t){opsOtaOpen=false;});
 ArduinoOTA.begin();opsOtaOpen=true;opsOtaDeadline=millis()+seconds*1000UL;
 return true;
}
static bool opsPlan(uint32_t sessionId,uint32_t count){
 if(!opsFsOk||opsFilePending()||opsOtaOpen||opsPhase!=OpsPhase::Idle||
    !opsRecordsDrained()||!hwOk||port.faulted()||
    config.stage!=6||count==0||count>OPS_ARCHIVE_MAX||sessionId==0||
    validate(config))return false;
 if(!opsArchive){
  opsArchive=new(std::nothrow) OpsArchiveRow[OPS_ARCHIVE_MAX];
  if(!opsArchive)return false;
  if(ESP.getFreeHeap()<32768){delete[] opsArchive;opsArchive=nullptr;return false;}
 }
 memset(opsArchive,0,size_t(count)*sizeof(OpsArchiveRow));
 opsSession=sessionId;opsCount=count;opsRows=0;opsArchiveFailure=false;
 opsPhase=OpsPhase::Armed;
 return true;
}
static void opsTcpCommand(char *line){
 char *save=nullptr,*op=strtok_r(line," \r\t",&save);
 if(!op)return;
 if(!opsTcpAuthorized){
  if(!strcmp(op,"AUTH")){
   char*key=strtok_r(nullptr," \r\t",&save);
   if(key&&opsHmac(key)){opsTcpAuthorized=true;opsTcpReply("AUTH_OK");}
   else {opsTcpReply("AUTH_FAIL");opsClient.stop();}
  }else opsTcpReply("AUTH_REQUIRED");
  return;
 }
 if(!strcmp(op,"STATUS")){opsTcpStatus();return;}
 if(!strcmp(op,"GET")){
  if(!opsDownload())opsTcpReply("ERROR GET_FAILED");
  return;
 }
 if(!strcmp(op,"ACKFILE")){
  char*raw=strtok_r(nullptr," \r\t",&save),*end=nullptr;
  const unsigned long id=raw?strtoul(raw,&end,10):0;
  OpsArchiveHeader h;
  if(!raw||!*raw||!end||*end||!id||!opsReadHeader(h)||
     !opsVerifyArchive(h)||h.session!=id){opsTcpReply("ERROR ACKFILE");return;}
  Preferences p;
  if(!p.begin("mode8ops",false)){opsTcpReply("ERROR NVS");return;}
  const bool ok=p.putUInt("lastack",h.session)==sizeof(uint32_t);
  p.end();
  if(ok){opsLastAck=h.session;opsTcpReply("ACKFILE_OK");}
  else opsTcpReply("ERROR NVS");
  return;
 }
 if(!strcmp(op,"PURGE")){opsTcpReply(opsEraseAcked()?"PURGE_OK":"ERROR UNACKED_FILE");return;}
 if(!strcmp(op,"START")){
  char*a=strtok_r(nullptr," \r\t",&save),*b=strtok_r(nullptr," \r\t",&save),*end=nullptr;
  const unsigned long sid=a?strtoul(a,&end,10):0;
  if(!a||!end||*end||!sid||sid>0xffffffffUL){opsTcpReply("ERROR BAD_SESSION");return;}
  const unsigned long count=b?strtoul(b,&end,10):0;
  if(!b||!end||*end||!count||count>OPS_ARCHIVE_MAX||
     !opsPlan(uint32_t(sid),uint32_t(count))){opsTcpReply("ERROR START_GUARD");return;}
  opsTcpReply("START_ACCEPTED_WIFI_OFF");
  opsClient.flush();delay(15);
  opsWifiDown();
  opsStartAt=uint64_t(esp_timer_get_time())+250000ULL;
  return;
 }
 if(!strcmp(op,"OTA")){
  char*a=strtok_r(nullptr," \r\t",&save),*end=nullptr;
  const unsigned long s=a?strtoul(a,&end,10):0;
  opsTcpReply(a&&end&&!*end&&opsOtaStart(uint32_t(s))?"OTA_WINDOW_OPEN":"ERROR OTA_GUARD");
  return;
 }
 opsTcpReply("ERROR UNKNOWN_COMMAND");
}
static void opsTcpPoll(){
 if(!opsWifiUp)return;
 if(!opsClient||!opsClient.connected()){
  if(opsClient)opsClient.stop();
  opsClient=opsServer.available();
  opsTcpAuthorized=false;opsTcpUsed=0;opsTcpOverflow=false;
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
 opsLoadNet();opsPhase=opsFilePending()?OpsPhase::Stored:OpsPhase::Idle;
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
  emit("{\"event\":\"OPS_STATUS\",\"phase\":\"%s\",\"wifi\":%s,\"fs\":%s,\"pending_file\":%s,\"session\":%lu,\"rows\":%lu}",
   opsPhaseText(),WiFi.status()==WL_CONNECTED?"true":"false",opsFsOk?"true":"false",
   opsFilePending()?"true":"false",(unsigned long)opsSession,(unsigned long)opsRows);
  return true;
 }
 if((opsOtaOpen&&strcmp(op,"STATUS")!=0)||active||!opsRecordsDrained()||opsPhase==OpsPhase::Armed||
    opsPhase==OpsPhase::Measuring||opsPhase==OpsPhase::Draining){
  emit("{\"event\":\"ERROR\",\"reason\":\"OPS_NOT_QUIESCENT\"}");return true;
 }
 if(!strcmp(op,"NET")){
  char*sub=strtok_r(nullptr," \r\t",&save);
  if(sub&&!strcmp(sub,"ON")){emit("{\"event\":\"OPS_NET\",\"ok\":%s}",opsWifiStart()?"true":"false");return true;}
  if(sub&&!strcmp(sub,"OFF")){opsWifiDown();emit("{\"event\":\"OPS_NET\",\"ok\":true}");return true;}
 }
 if(!strcmp(op,"SSID")||!strcmp(op,"PASS")||!strcmp(op,"AUTH")||!strcmp(op,"OTAHASH")){
  char*value=strtok_r(nullptr," \r\t",&save);
  // Credentials are not printed or echoed on serial.
  emit("{\"event\":\"OPS_PROVISION\",\"field\":\"%s\",\"ok\":%s}",
       op,value&&opsSaveHexField(op,value)?"true":"false");
  return true;
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
  if(!opsRecordsDrained()){opsPhase=OpsPhase::Fault;return;}
  char line[70];snprintf(line,sizeof line,"START %lu %lu",
                        (unsigned long)opsSession,(unsigned long)opsCount);
  opsInternalStart=true;command(line);opsInternalStart=false;
  opsPhase=active?OpsPhase::Measuring:OpsPhase::Fault;
 }
 if(opsPhase==OpsPhase::Measuring&&!active)opsPhase=OpsPhase::Draining;
 if(opsPhase==OpsPhase::Draining&&opsRecordsDrained()){
  if(opsPersistArchive()){opsPhase=OpsPhase::Stored;opsWifiStart();}
  else opsPhase=OpsPhase::Fault;
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
