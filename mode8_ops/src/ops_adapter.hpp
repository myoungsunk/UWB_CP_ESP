#pragma once
// Included INSIDE dual8app after application globals and emit(). SPI remains
// owned by loopTask; callbacks and the journal never access DW1000 registers.
static void command(char*);
static void idleAll();
static void releaseRecord(uint8_t);
namespace ops {
using m8ops::State;
static m8ops::Gate gate;
static NetworkServer server(29310);
static NetworkClient client;
static bool netWanted=false,netSuspended=false,serverUp=false,authenticated=false;
static bool fromTcp=false,internal=false,fsReady=false,otaOpen=false,otaReceiving=false;
static bool journalBad=false,abortRequested=false,acknowledged=false;
static uint64_t clientUntil=0,otaUntil=0,reconnectAt=0;
static uint8_t challenge[16]{};
static char input[512]{};static size_t inputUsed=0;
static uint8_t* snapshots=nullptr;
static uint32_t captured=0;static uint32_t snapshotSession=0;
static char fileAck[65]{};
static constexpr size_t SNAP=offsetof(Record,cir);
static_assert(std::is_trivially_copyable<Record>::value,"RAM snapshot requires POD");
static_assert(std::is_standard_layout<Record>::value,"RAM snapshot requires standard layout");
static constexpr char DATA[]="/mode8.rfb8",PART[]="/mode8.part",INTENT[]="/mode8.intent";
static constexpr uint32_t HEAP_RESERVE=32768;
struct Credentials {uint32_t version=1;char ssid[33]{};char pass[65]{};uint8_t key[32]{};char ota[65]{};uint32_t crc=0;};
static Credentials creds;static bool configured=false;
static uint64_t now(){return uint64_t(esp_timer_get_time());}
static bool outputBusy(){return pending.load()!=0||awaitingAck.load()!=0;}
static bool anyFiles(){
 if(!fsReady)return false;
 File root=LittleFS.open("/");if(!root||!root.isDirectory())return true;
 File entry=root.openNextFile();bool present=bool(entry);if(entry)entry.close();root.close();return present;
}
static bool dataPresent(){return snapshots||anyFiles();}
static bool quiet(){return !active&&!outputBusy()&&gate.state==State::Idle&&!otaOpen&&!dataPresent();}
static void reply(bool ok,const char* reason,const char* extra=""){
 char text[768];
 const int n=snprintf(text,sizeof text,
  "{\"event\":\"OPS\",\"ok\":%s,\"reason\":\"%s\",\"role\":\"%s\",\"boot\":%lu,\"state\":\"%s\",\"session\":%lu%s}",
  ok?"true":"false",reason,role(),(unsigned long)boot,m8ops::name(gate.state),(unsigned long)gate.session,extra);
 if(n<0||size_t(n)>=sizeof text)return;
 if(fromTcp){
  // Maintenance only. A stalled peer is disconnected rather than blocking RF.
  const uint64_t end=now()+250000;size_t pos=0;
  text[n]='\n';
  while(pos<size_t(n+1)&&client.connected()&&now()<end){
   size_t wrote=client.write(reinterpret_cast<uint8_t*>(text)+pos,size_t(n+1)-pos);
   pos+=wrote;if(!wrote)delay(1);
  }
  if(pos!=size_t(n+1))client.stop();
 }else emit("%s",text);
}
static void closeClient(){client.stop();authenticated=false;inputUsed=0;clientUntil=0;}
static void networkOff(){
 closeClient();if(serverUp){server.end();serverUp=false;}
 WiFi.setAutoReconnect(false);WiFi.disconnect(true,false);WiFi.mode(WIFI_OFF);
}
static void networkOn(){
 if(!configured||active||otaReceiving)return;
 WiFi.persistent(false);WiFi.mode(WIFI_STA);
 WiFi.setHostname(DUAL8_ANCHOR?"mode8-anchor":"mode8-tag");
 WiFi.setAutoReconnect(false);WiFi.begin(creds.ssid,creds.pass);
 reconnectAt=now()+45000000ULL;
}
static void suspendNetwork(){
 netSuspended=netWanted;networkOff();
}
static bool digestFile(char hash[65],size_t& bytes){
 if(!fsReady||!LittleFS.exists(DATA))return false;
 File f=LittleFS.open(DATA,"r");if(!f)return false;
 bytes=f.size();uint8_t buf[512],d[32];mbedtls_sha256_context ctx;mbedtls_sha256_init(&ctx);
 bool ok=mbedtls_sha256_starts(&ctx,0)==0;size_t read=0;
 while(ok&&read<bytes){size_t n=f.read(buf,sizeof buf);if(!n){ok=false;break;}read+=n;ok=mbedtls_sha256_update(&ctx,buf,n)==0;}
 ok=ok&&read==bytes&&mbedtls_sha256_finish(&ctx,d)==0;
 mbedtls_sha256_free(&ctx);f.close();if(ok)m8ops::hex(d,32,hash);return ok;
}
static uint32_t capacity(){
 size_t h=ESP.getFreeHeap(),b=heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
 if(h<=HEAP_RESERVE)return 0;
 size_t n=(h-HEAP_RESERVE)/SNAP;if(n>b/SNAP)n=b/SNAP;if(n>32)n=32;
 return uint32_t(n);
}
static bool putFrame(File& f,uint32_t seq,uint8_t kind,const char* json,size_t n){
 uint8_t h[24]{};putLE(h,WIRE_MAGIC,4);h[4]=1;h[5]=kind;putLE(h+6,24,2);
 putLE(h+8,seq,4);putLE(h+12,n,4);putLE(h+20,crc32(json,n),4);
 return f.write(h,sizeof h)==sizeof h&&f.write(reinterpret_cast<const uint8_t*>(json),n)==n;
}
static void finishJournal(){
 if(!snapshots||active||outputBusy())return;
 if(!fsReady||LittleFS.exists(DATA)||LittleFS.exists(PART)){gate.state=State::Fault;return;}
 uint8_t poolIndex=0;
 if(xQueueReceive(freeQ,&poolIndex,0)!=pdTRUE)return;
 const int idx=int(poolIndex);
 char* json=static_cast<char*>(malloc(JSON_MAX+1));
 if(!json){releaseRecord(uint8_t(idx));gate.state=State::Fault;return;}
 File f=LittleFS.open(PART,"w");
 bool ok=bool(f);
 if(ok){
  int n=snprintf(json,JSON_MAX+1,
   "{\"event\":\"OPS_CAPTURE\",\"schema\":\"mode8.ops.v1\",\"role\":\"anchor\",\"boot\":%lu,\"session\":%lu,\"config\":%lu,\"expected\":%lu,\"captured\":%lu,\"aborted\":%s,\"journal_order_ok\":%s,\"hardware_tested\":false}",
   (unsigned long)boot,(unsigned long)gate.session,(unsigned long)gate.config,
   (unsigned long)gate.count,(unsigned long)captured,abortRequested?"true":"false",journalBad?"false":"true");
  ok=n>0&&size_t(n)<=JSON_MAX&&putFrame(f,0,1,json,size_t(n));
 }
 Record& scratch=recordAt(unsigned(idx));
 for(uint32_t i=0;ok&&i<captured;++i){
  new(&scratch) Record();memcpy(&scratch,snapshots+size_t(i)*SNAP,SNAP);
  size_t n=0;ok=serializeRecord(scratch,json,JSON_MAX+1,n)&&putFrame(f,i+1,2,json,n);
 }
 if(f){f.flush();f.close();}
 free(json);releaseRecord(uint8_t(idx));
 if(ok)ok=LittleFS.rename(PART,DATA);
 if(ok){free(snapshots);snapshots=nullptr;gate.state=State::Stored;acknowledged=false;}
 else gate.state=State::Fault; // Intent/partial file and RAM are retained; never auto-format/overwrite.
}
static bool consume(const Record&r){
#if DUAL8_ANCHOR
 if(!snapshots)return false;
 if(r.session!=snapshotSession)journalBad=true;
 if(captured>=gate.count){journalBad=true;return true;}
 if(r.txn!=captured||r.config!=gate.config||r.boot!=boot)journalBad=true;
 memcpy(snapshots+size_t(captured)*SNAP,&r,SNAP);++captured;
 return true;
#else
 (void)r;return false;
#endif
}
static void status(){
 char hash[65]="";size_t bytes=0;bool has=!active&&!outputBusy()&&!otaReceiving&&digestFile(hash,bytes);
 char extra[600];
 snprintf(extra,sizeof extra,
  ",\"ops_version\":1,\"baseline\":\"ce8f986\",\"native_active\":%s,\"output_pending\":%lu,\"awaiting_ack\":%u,\"hw_ok\":%s,\"fault_latched\":%s,\"config_id\":%lu,\"stage\":%u,\"period_us\":%lu,\"baud\":%lu,\"wifi_connected\":%s,\"fs_ready\":%s,\"file_bytes\":%u,\"file_sha256\":\"%s\",\"data_pending\":%s,\"max_batch\":%lu,\"ota_open\":%s",
  active?"true":"false",(unsigned long)pending.load(),awaitingAck.load(),hwOk?"true":"false",port.faulted()?"true":"false",
  (unsigned long)configId(config),unsigned(config.stage),(unsigned long)config.period_us,(unsigned long)config.baud,
  WiFi.status()==WL_CONNECTED?"true":"false",fsReady?"true":"false",unsigned(bytes),has?hash:"",
  (snapshots||(!active&&anyFiles()))?"true":"false",(unsigned long)capacity(),otaOpen?"true":"false");
 reply(true,"STATUS",extra);
}
static bool sameHex(const char*a,const char*b){
 uint8_t x[32],y[32];return m8ops::unhex(a,x,32)&&m8ops::unhex(b,y,32)&&m8ops::secureEqual(x,y,32);
}
static void configureOta(){
 ArduinoOTA.setPort(3232);ArduinoOTA.setHostname(DUAL8_ANCHOR?"mode8-anchor":"mode8-tag");
 ArduinoOTA.setPasswordHash(creds.ota);ArduinoOTA.setMdnsEnabled(false);ArduinoOTA.setRebootOnSuccess(true);
 ArduinoOTA.setTimeout(5000);
 ArduinoOTA.onStart([](){
  if(ArduinoOTA.getCommand()!=U_FLASH||active||outputBusy()||dataPresent()||gate.state!=State::Idle){
   Update.abort();otaReceiving=false;return;
  }
  otaReceiving=true;
 });
 ArduinoOTA.onEnd([](){otaReceiving=false;});
 ArduinoOTA.onError([](ota_error_t){otaReceiving=false;});
 ArduinoOTA.begin();
}
static bool textFromHex(const char* in,char*out,size_t cap){
 if(!in)return false;
 if(!strcmp(in,"-")){out[0]=0;return true;}
 size_t n=strlen(in);if(n%2||n/2>=cap)return false;
 if(!m8ops::unhex(in,reinterpret_cast<uint8_t*>(out),n/2))return false;
 if(memchr(out,0,n/2))return false;
 out[n/2]=0;return true;
}
static void runNative(const char* line){
 char b[192];if(strlen(line)>=sizeof b)return;
 strcpy(b,line);internal=true;command(b);internal=false;
}
static void process(char* line){
 char* save=nullptr;char* w[9]{};unsigned n=0;
 for(char*p=strtok_r(line," \t\r",&save);p;p=strtok_r(nullptr," \t\r",&save)){
  if(n>=9){reply(false,"TOO_MANY_TOKENS");return;}w[n++]=p;
 }
 if(n<2||strcmp(w[0],"OPS")){reply(false,"OPS_COMMAND_REQUIRED");return;}
 const char*op=w[1];
 if(!strcmp(op,"STATUS")&&n==2){status();return;}
 if(active||gate.state==State::Running||gate.state==State::Draining||otaReceiving){reply(false,"BUSY");return;}
 if(!strcmp(op,"PROVISION")&&n==6){
  if(fromTcp||!quiet()){reply(false,"USB_MAINTENANCE_ONLY");return;}
  Credentials next;
  if(!textFromHex(w[2],next.ssid,sizeof next.ssid)||!next.ssid[0]||
     !textFromHex(w[3],next.pass,sizeof next.pass)||!m8ops::unhex(w[4],next.key,32)||
     strlen(w[5])!=64){reply(false,"BAD_CREDENTIAL_FORMAT");return;}
  uint8_t tmp[32];if(!m8ops::unhex(w[5],tmp,32)){reply(false,"BAD_OTA_HASH");return;}
  bool nonzero=false;for(auto v:next.key)nonzero|=v!=0;
  if(!nonzero){reply(false,"ZERO_KEY");return;}
  strcpy(next.ota,w[5]);next.crc=crc32(&next,offsetof(Credentials,crc));
  Preferences p;bool ok=p.begin("m8ops",false);if(ok){ok=p.putBytes("cred",&next,sizeof next)==sizeof next;p.end();}
  if(ok){creds=next;configured=true;}memset(&next,0,sizeof next);reply(ok,ok?"PROVISIONED":"NVS_ERROR");return;
 }
 if(!strcmp(op,"INIT_FS")&&n==3&&!strcmp(w[2],"EMPTY")){
  if(fromTcp||active||outputBusy()||fsReady||snapshots||gate.state!=State::Idle){
   reply(false,"USB_BLANK_PARTITION_ONLY");return;
  }
  const auto* part=esp_partition_find_first(ESP_PARTITION_TYPE_DATA,ESP_PARTITION_SUBTYPE_DATA_SPIFFS,"spiffs");
  if(!part){reply(false,"DEFAULT_FS_PARTITION_MISSING");return;}
  uint8_t buf[256];bool blank=true;
  for(size_t off=0;off<part->size&&blank;off+=sizeof buf){
   size_t len=part->size-off<sizeof buf?part->size-off:sizeof buf;
   if(esp_partition_read(part,off,buf,len)!=ESP_OK){blank=false;break;}
   for(size_t j=0;j<len;++j)if(buf[j]!=0xff){blank=false;break;}
  }
  if(!blank){reply(false,"NONEMPTY_PARTITION_NOT_FORMATTED");return;}
  fsReady=LittleFS.begin(true);
  reply(fsReady,fsReady?"EMPTY_FS_INITIALIZED":"FS_INIT_FAILED");return;
 }
 if(!strcmp(op,"NET")&&n==3){
  if(!quiet()){reply(false,"NOT_QUIESCENT");return;}
  if(!strcmp(w[2],"ON")&&configured){netWanted=true;networkOn();reply(true,"CONNECTING");}
  else if(!strcmp(w[2],"OFF")){reply(true,"DISCONNECTING");netWanted=false;networkOff();}
  else reply(false,"NET_ON_OFF_OR_NOT_PROVISIONED");return;
 }
 if(!strcmp(op,"CONFIG")&&n==4){
  uint32_t period,baud;if(!quiet()||!m8ops::u32(w[2],period)||!m8ops::u32(w[3],baud)){reply(false,"CONFIG_GUARD");return;}
  Config next=config;next.stage=6;next.period_us=period;next.baud=baud;
  if(const char*why=validate(next)){reply(false,why);return;}
  // Existing command path owns UART transitions. Change baud first, then stage/period.
  char cmd[64];snprintf(cmd,sizeof cmd,"BAUD %lu",(unsigned long)baud);runNative(cmd);
  if(config.baud!=baud){reply(false,"BAUD_REJECTED");return;}
  runNative("SET STAGE 6");snprintf(cmd,sizeof cmd,"SET PERIOD_US %lu",(unsigned long)period);runNative(cmd);
  bool ok=config.stage==6&&config.period_us==period&&config.baud==baud;reply(ok,ok?"CONFIGURED":"CONFIG_REJECTED");return;
 }
 if(!strcmp(op,"SELFTEST")&&n==2){
  if(!quiet()){reply(false,"NOT_QUIESCENT");return;}
  runNative("SELFTEST");reply(hwOk&&!port.faulted(),hwOk&&!port.faulted()?"SELFTEST_OK":"SELFTEST_FAILED");return;
 }
 if(!strcmp(op,"CAL")&&n==7){
  if(!quiet()){reply(false,"NOT_QUIESCENT");return;}
  char cmd[160];int z=snprintf(cmd,sizeof cmd,"ANT %s %s %s %s %s",w[2],w[3],w[4],w[5],w[6]);
  uint32_t radio,tx,rx,id;char* end=nullptr;double sigma=strtod(w[6],&end);
  if(z<=0||size_t(z)>=sizeof cmd||!m8ops::u32(w[2],radio)||radio>=(DUAL8_ANCHOR?1U:2U)||
     !m8ops::u32(w[3],tx)||tx>65535||!m8ops::u32(w[4],rx)||rx>65535||!m8ops::u32(w[5],id)||
     !((!id&&!strcmp(w[6],"none"))||(end!=w[6]&&!*end&&isfinite(sigma)&&sigma>0&&sigma<=100))){
   reply(false,"BAD_CALIBRATION");return;
  }
  runNative(cmd);const auto&cal=port.calibration(radio);
  bool ok=hwOk&&!port.faulted()&&cal.tx==tx&&cal.rx==rx&&cal.id==id;
  reply(ok,ok?"CAL_APPLIED_RAM_ONLY":"CAL_FAILED");return;
 }
 if(!strcmp(op,"PREPARE")&&n==5){
#if DUAL8_ANCHOR
  uint32_t s,count,c;
  if(!m8ops::u32(w[2],s)||!m8ops::u32(w[3],count)||!m8ops::u32(w[4],c)){reply(false,"BAD_PREPARE");return;}
  if(gate.state==State::Prepared&&s==gate.session&&count==gate.count&&c==gate.config){reply(true,"ALREADY_PREPARED");return;}
  if(!quiet()||!fsReady||!hwOk||port.faulted()||config.stage!=6||c!=configId(config)||
     !s||!count||count>capacity()||validate(config)){reply(false,"PREPARE_GUARD");return;}
  const uint64_t need=uint64_t(count)*(JSON_MAX+24)+2048;
  if(LittleFS.totalBytes()<LittleFS.usedBytes()||LittleFS.totalBytes()-LittleFS.usedBytes()<need){
   reply(false,"FS_CAPACITY");return;
  }
  uint8_t* buf=static_cast<uint8_t*>(malloc(size_t(count)*SNAP));
  if(!buf){reply(false,"NO_RAM");return;}
  File intent=LittleFS.open(INTENT,"w");
  char text[192];int z=snprintf(text,sizeof text,"{\"schema\":1,\"session\":%lu,\"boot\":%lu,\"count\":%lu,\"config\":%lu}",
    (unsigned long)s,(unsigned long)boot,(unsigned long)count,(unsigned long)c);
  bool saved=bool(intent)&&z>0&&size_t(z)<sizeof text&&intent.write(reinterpret_cast<const uint8_t*>(text),z)==size_t(z);
  if(intent){intent.flush();intent.close();}
  if(!saved||!gate.prepare(s,count,c,now())){free(buf);reply(false,"INTENT_NOT_COMMITTED");return;}
  snapshots=buf;snapshotSession=s;captured=0;journalBad=false;abortRequested=false;acknowledged=false;
  reply(true,"PREPARED");return;
#else
  reply(false,"ANCHOR_ONLY_USE_TAG_USB");return;
#endif
 }
 if(!strcmp(op,"COMMIT")&&n==4){
  uint32_t s,c;
  if(!m8ops::u32(w[2],s)||!m8ops::u32(w[3],c)||active||outputBusy()||
     !hwOk||port.faulted()||c!=configId(config)||!gate.arm(s,c,now())){reply(false,"COMMIT_GUARD");return;}
  reply(true,"ARMED_START_IN_1S");suspendNetwork();return;
 }
 if(!strcmp(op,"ABORT")&&n==3){
  uint32_t s;if(!m8ops::u32(w[2],s)||s!=gate.session||
     (gate.state!=State::Prepared&&gate.state!=State::Armed)){reply(false,"ABORT_GUARD");return;}
  abortRequested=true;gate.state=State::Draining;reply(true,"ABORT_RECORDED");return;
 }
 if(!strcmp(op,"FETCH")&&n==4){
  uint32_t offset,len;
  if(active||outputBusy()||snapshots||!fsReady||!m8ops::u32(w[2],offset)||!m8ops::u32(w[3],len)||!len||len>256){reply(false,"FETCH_GUARD");return;}
  File f=LittleFS.open(DATA,"r");if(!f||offset>f.size()||len>f.size()-offset||!f.seek(offset)){reply(false,"FETCH_RANGE");return;}
  uint8_t data[256];size_t got=f.read(data,len);f.close();
  if(got!=len){reply(false,"READ_FAILED");return;}
  char h[513],extra[640];m8ops::hex(data,len,h);
  snprintf(extra,sizeof extra,",\"offset\":%lu,\"bytes\":%lu,\"crc32\":%lu,\"hex\":\"%s\"",
   (unsigned long)offset,(unsigned long)len,(unsigned long)crc32(data,len),h);
  reply(true,"CHUNK",extra);return;
 }
 if((!strcmp(op,"ACK_FILE")||!strcmp(op,"DELETE"))&&n==3){
  char hash[65];size_t bytes;
  if(active||outputBusy()||snapshots||!digestFile(hash,bytes)||!sameHex(w[2],hash)){reply(false,"FILE_HASH_MISMATCH");return;}
  if(!strcmp(op,"ACK_FILE")){strcpy(fileAck,hash);acknowledged=true;reply(true,"FILE_ACKNOWLEDGED");return;}
  if(!acknowledged||!sameHex(fileAck,hash)){reply(false,"ACK_REQUIRED");return;}
  bool ok=LittleFS.remove(DATA);if(ok&&LittleFS.exists(INTENT))ok=LittleFS.remove(INTENT);
  if(ok){gate=m8ops::Gate{};acknowledged=false;fileAck[0]=0;}
  reply(ok,ok?"DELETED":"DELETE_FAILED");return;
 }
 if(!strcmp(op,"OTA")&&n==3){
  uint32_t seconds;
  if(!m8ops::u32(w[2],seconds)||seconds<60||seconds>600||!quiet()||!configured||WiFi.status()!=WL_CONNECTED){
   reply(false,"OTA_GUARD");return;
  }
  const auto*running=esp_ota_get_running_partition();const auto*next=esp_ota_get_next_update_partition(nullptr);
  if(!running||!next||running==next||next->size<ESP.getSketchSize()){reply(false,"OTA_PARTITION_GUARD");return;}
  otaOpen=true;otaUntil=now()+uint64_t(seconds)*1000000;configureOta();reply(true,"OTA_WINDOW_OPEN");return;
 }
 if(!strcmp(op,"OTA_CLOSE")&&n==2&&!otaReceiving){
  if(otaOpen)ArduinoOTA.end();otaOpen=false;reply(true,"OTA_CLOSED");return;
 }
 reply(false,"UNKNOWN_OR_BLOCKED_OPERATION");
}
static bool filter(char* line){
 if(internal)return false;
 if(!strncmp(line,"OPS ",4)){process(line);return true;}
 if(!strncmp(line,"STOP",4)){
  if(gate.state==State::Prepared||gate.state==State::Armed){abortRequested=true;gate.state=State::Draining;}
  return false;
 }
 if(!strncmp(line,"REC_ACK ",8)||!strncmp(line,"REC_NACK ",9)||
    !strcmp(line,"HELLO")||!strcmp(line,"STATUS")||!strcmp(line,"HELP"))return false;
 // No reconfiguration or start may race an old record or a pending OTA/journal.
 if(active||otaOpen||gate.state!=State::Idle||outputBusy()||dataPresent()){
  reply(false,"OPS_STATE_OR_DRAIN_GUARD");return true;
 }
 if(!strncmp(line,"START ",6))suspendNetwork();
 return false;
}
static void setup(){
 fsReady=LittleFS.begin(false); // NEVER format an existing partition automatically.
 Preferences p;
 if(p.begin("m8ops",true)){
  Credentials c;
  bool ok=p.getBytesLength("cred")==sizeof c&&p.getBytes("cred",&c,sizeof c)==sizeof c;
  p.end();
  uint8_t tmp[32];
  if(ok&&c.version==1&&c.ssid[32]==0&&c.pass[64]==0&&c.ota[64]==0&&c.ssid[0]&&
     c.crc==crc32(&c,offsetof(Credentials,crc))&&m8ops::unhex(c.ota,tmp,32)){
   creds=c;configured=true;
  }
 }
 if(fsReady&&LittleFS.exists(DATA))gate.state=State::Stored;
 else if(fsReady&&(LittleFS.exists(INTENT)||LittleFS.exists(PART)))gate.state=State::Fault;
 // Provisioned devices reconnect in maintenance. Never restart a previous RF session.
 netWanted=configured;if(netWanted)networkOn();
 emit("{\"event\":\"OPS_BOOT\",\"ops_version\":1,\"role\":\"%s\",\"fs_ready\":%s,\"state\":\"%s\",\"legacy_route_api\":false,\"measurement_wifi\":\"OFF\",\"bench_uart_preserved\":true}",
  role(),fsReady?"true":"false",m8ops::name(gate.state));
}
static void tcpTick(){
 if(!netWanted||netSuspended||!configured||active||outputBusy()||gate.state==State::Armed||gate.state==State::Running||gate.state==State::Draining)return;
 if(WiFi.status()!=WL_CONNECTED){
  if(now()>=reconnectAt){closeClient();if(serverUp){server.end();serverUp=false;}networkOn();}
  return;
 }
 if(!serverUp){server.begin();server.setNoDelay(true);serverUp=true;}
 if(!client.connected()){
  closeClient();NetworkClient next=server.accept();if(!next)return;
  client=next;client.setTimeout(250);client.setNoDelay(true);
  esp_fill_random(challenge,sizeof challenge);clientUntil=now()+10000000ULL;
  char h[33],greeting[160];m8ops::hex(challenge,sizeof challenge,h);
  snprintf(greeting,sizeof greeting,"{\"event\":\"CHALLENGE\",\"nonce\":\"%s\",\"role\":\"%s\",\"boot\":%lu,\"protocol\":1}\n",h,role(),(unsigned long)boot);
  client.print(greeting);
 }
 if(now()>=clientUntil){closeClient();return;}
 unsigned budget=128;
 while(budget--&&client.available()){
  int c=client.read();if(c<0)break;if(c=='\r')continue;
  if(c=='\n'){
   input[inputUsed]=0;inputUsed=0;
   if(!authenticated){
    uint8_t provided[32],wanted[32];
    const auto*info=mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    bool ok=!strncmp(input,"AUTH ",5)&&m8ops::unhex(input+5,provided,32)&&info&&
      mbedtls_md_hmac(info,creds.key,32,challenge,sizeof challenge,wanted)==0&&m8ops::secureEqual(provided,wanted,32);
    memset(challenge,0,sizeof challenge);fromTcp=true;reply(ok,ok?"AUTHENTICATED":"AUTH_FAILED");fromTcp=false;
    if(!ok){closeClient();return;}authenticated=true;
   }else{
    fromTcp=true;process(input);fromTcp=false;
   }
   clientUntil=now()+30000000ULL;
   if(!client.connected())return;
  }else if(c<32||c>126||inputUsed+1>=sizeof input){closeClient();return;}
  else input[inputUsed++]=char(c);
 }
}
static void tick(){
 if(gate.expire(now())){
  free(snapshots);snapshots=nullptr;captured=0;
  if(fsReady)LittleFS.remove(INTENT);
 }
 if(gate.due(now())){
  char cmd[64];snprintf(cmd,sizeof cmd,"START %lu %lu",(unsigned long)gate.session,(unsigned long)gate.count);
  runNative(cmd);gate.state=active?State::Running:State::Draining;
  if(!active)journalBad=true;
 }
 if(gate.state==State::Running&&!active)gate.state=State::Draining;
 if(gate.state==State::Draining&&!outputBusy())finishJournal();
 if(!active&&!outputBusy()&&gate.state!=State::Armed&&gate.state!=State::Running&&gate.state!=State::Draining&&netSuspended){
  netSuspended=false;if(netWanted)networkOn();
 }
 if(otaOpen){
  if(active||dataPresent()||outputBusy()||now()>=otaUntil||WiFi.status()!=WL_CONNECTED){
   if(!otaReceiving){ArduinoOTA.end();otaOpen=false;}
  }else ArduinoOTA.handle();
 }
 if(!active)tcpTick();
}
} // namespace ops
