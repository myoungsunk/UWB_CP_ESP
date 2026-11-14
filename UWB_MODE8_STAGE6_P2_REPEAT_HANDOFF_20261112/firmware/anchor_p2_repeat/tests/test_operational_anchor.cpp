#include "core.hpp"
#include "transport_guard.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>
#include <new>
#include <string>
#include <vector>

namespace dual8app {
using namespace dual8;
enum { WIFI_OFF, WIFI_STA, WL_CONNECTED, SeekSet, U_FLASH, WPA2_AUTH_PEAP };
static uint64_t nowUs;
static bool active=false,hwOk=true,drainComplete=true,drainClean=true;
static uint32_t boot=41,periodMiss=0;
static Config config;
static bool opsInternalStart=false;
static unsigned starts=0;
static void command(char*){++starts;active=true;}
static void emit(const char*,...){}
static bool transportQuiet(){return true;}
static bool transportChangeAllowed(){return true;}
static unsigned sessionFailures(){return 0;}
static uint32_t millis(){return nowUs/1000;}
static uint64_t esp_timer_get_time(){return nowUs;}
static uint32_t esp_random(){return 17;}
struct Port {bool faulted(){return false;}} port;
struct Count {unsigned load(){return 0;}} pending,awaitingAck;
static const unsigned POOL=5;
static void* freeQ=&pending;
static unsigned uxQueueMessagesWaiting(void*){return POOL;}
static const char* firmwareId(bool){return "test-anchor";}

static bool failRead=false;
struct File {
 std::vector<uint8_t>* data=nullptr;
 size_t pos=0;
 bool directory=false;
 explicit File(std::vector<uint8_t>* p=nullptr):data(p){}
 explicit operator bool() const{return data||directory;}
 bool isDirectory() const{return directory;}
 File openNextFile(){return File();}
 size_t size() const{return data->size();}
 int read(uint8_t* p,size_t n){
  if(failRead){failRead=false;return -1;}
  if(n>size()-pos)n=size()-pos;
  std::memcpy(p,data->data()+pos,n);pos+=n;return int(n);
 }
 size_t write(const uint8_t* p,size_t n){data->insert(data->end(),p,p+n);return n;}
 bool available() const{return pos<size();}
 bool seek(size_t offset,int){pos=offset;return offset<=size();}
 void close(){}
 void flush(){}
};
struct Filesystem {
 std::map<std::string,std::vector<uint8_t>> files;
 size_t capacity=1024*1024;
 size_t totalBytes() const{return capacity;}
 size_t usedBytes() const{size_t n=0;for(const auto& row:files)n+=row.second.size();return n;}
 bool corruptRenameHeader=false;
 bool begin(bool format){assert(!format);return true;}
 bool exists(const char* p){return files.count(p);}
 File open(const char* p,const char* mode="r"){
  if(std::strcmp(p,"/")==0){File root;root.directory=true;return root;}
  if(*mode=='w'){files[p].clear();return File(&files[p]);}
  return exists(p)?File(&files[p]):File();
 }
 bool rename(const char* from,const char* to){
  if(!exists(from)||exists(to))return false;
  files[to]=files[from];files.erase(from);
  if(corruptRenameHeader)files[to][0]^=1;
  return true;
 }
 bool remove(const char* path){return files.erase(path)==1;}
} LittleFS;
static std::map<std::string,std::vector<uint8_t>> nvs;
struct Preferences {
 bool begin(const char*,bool){return true;}
 bool getBool(const char* k,bool defaultValue){return nvs.count(k)?nvs[k][0]!=0:defaultValue;}
 size_t putBool(const char* k,bool v){nvs[k]={uint8_t(v?1:0)};return 1;}
 void end(){}
 size_t getString(const char*,char*,size_t){return 0;}
 size_t getBytesLength(const char* k){return nvs.count(k)?nvs[k].size():0;}
 size_t getBytes(const char* k,void* dest,size_t size){
  if(getBytesLength(k)!=size)return 0;
  std::memcpy(dest,nvs[k].data(),size);return size;
 }
 size_t putBytes(const char* k,const void* src,size_t size){
  const auto* p=static_cast<const uint8_t*>(src);
  nvs[k].assign(p,p+size);return size;
 }
 size_t putString(const char*,const char* v){return std::strlen(v);}
 bool remove(const char* k){return nvs.erase(k)==1;}
};
struct WiFiClient {
 bool connectedFlag=false,failDataRead=false,corruptData=false,oversizedWrite=false;
 std::string written;
 explicit operator bool() const{return connectedFlag;}
 bool connected() const{return connectedFlag;}
 void stop(){connectedFlag=false;}
 size_t print(const char* s){
  if(std::strncmp(s,"DATA ",5)==0){
   if(failDataRead)failRead=true;
   if(corruptData)LittleFS.files["/m8_pending.bin"].back()^=1;
  }
  if(connectedFlag){written+=s;return std::strlen(s);}
  return 0;
 }
 size_t print(char c){written+=c;return 1;}
 size_t write(const uint8_t* p,size_t size){
  if(size>512){oversizedWrite=true;return size;}
  written.append(reinterpret_cast<const char*>(p),size);return size;
 }
 bool available(){return false;}
 int read(){return -1;}
};
struct WiFiServer {
 explicit WiFiServer(unsigned){}
 WiFiClient accept(){return {};}
 void begin(){}
 void end(){}
};
struct Wifi {
 int selected=WIFI_OFF;
 bool rejectOff=false;
 bool mode(int m){if(m==WIFI_OFF&&rejectOff)return false;selected=m;return true;}
 int getMode() const{return selected;}
 int status() const{return selected==WIFI_STA?WL_CONNECTED:0;}
 void disconnect(bool,bool){}
 void setAutoReconnect(bool){}
 void begin(const char*,const char*){}
 void begin(const char*,int,const char*,const char*,const char*,const char*){}
} WiFi;
struct Ota {
 void setPort(int){}
 void setHostname(const char*){}
 void setPasswordHash(const char*){}
 void setMdnsEnabled(bool){}
 void setRebootOnSuccess(bool){}
 template<class Fn> void onStart(Fn){}
 template<class Fn> void onEnd(Fn){}
 template<class Fn> void onError(Fn){}
 unsigned beginCalls=0;
 int getCommand(){return U_FLASH;}
 void begin(){++beginCalls;}
 void end(){}
 void handle(){}
} ArduinoOTA;
struct Updater {void abort(){}} Update;
struct Esp {unsigned getFreeHeap(){return 100000;}} ESP;
using ota_error_t=int;
struct mbedtls_md_info_t {};
enum { MBEDTLS_MD_SHA256 };
static mbedtls_md_info_t mockMdInfo;
static const mbedtls_md_info_t* mbedtls_md_info_from_type(int){return &mockMdInfo;}
static int mbedtls_md_hmac(const mbedtls_md_info_t*,const uint8_t*key,size_t kn,
                           const uint8_t*msg,size_t mn,uint8_t*out){
 uint32_t h=2166136261U;
 for(size_t i=0;i<kn;++i)h=(h^key[i])*16777619U;
 for(size_t i=0;i<mn;++i)h=(h^msg[i])*16777619U;
 for(unsigned i=0;i<32;++i)out[i]=uint8_t((h>>(8*(i%4)))^i);
 return 0;
}
static void delay(unsigned ms){nowUs+=uint64_t(ms)*1000;}

#define DUAL8_ANCHOR 1
#include "operational_anchor.hpp"

static void makeArchive(const char* path,uint32_t bootId,uint8_t value){
 OpsArchiveHeader h;
 h.session=23;h.boot=bootId;h.config=71;h.count=1;
 std::vector<uint8_t> row(sizeof(OpsArchiveRow),value);
 h.body_crc=crc32(row.data(),row.size());h.header_crc=crc32(&h,sizeof h);
 File f=LittleFS.open(path,"w");
 f.write(reinterpret_cast<uint8_t*>(&h),sizeof h);f.write(row.data(),row.size());
}
static void tcp(const char* op){
 char line[96];std::snprintf(line,sizeof line,"%s",op);
 opsClient.written.clear();opsTcpCommand(line);
}
static void connect(){
 opsClient.connectedFlag=true;opsTcpAuthorized=true;opsDownloaded={};opsNonce=17;
}
static std::string custodyToken(const char* kind,const OpsArchiveHeader& h){
 char text[128];
 std::snprintf(text,sizeof text,"MODE8-CUSTODY:%s:%08X:%08X:%08X:%08X",
               kind,h.boot,h.session,h.body_crc,opsNonce);
 uint8_t digest[32]{};
 mbedtls_md_hmac(&mockMdInfo,opsNet.deleteKey,32,
    reinterpret_cast<const uint8_t*>(text),std::strlen(text),digest);
 char hex[65]{};
 for(unsigned i=0;i<32;++i)std::snprintf(hex+2*i,3,"%02x",unsigned(digest[i]));
 return std::string(hex);
}
static void ack(const OpsArchiveHeader& h){
 char cmd[144];std::snprintf(cmd,sizeof cmd,"ACKFILE %u %u %s",
                            h.session,h.body_crc,custodyToken("ACK",h).c_str());
 tcp(cmd);
}
static void testApproval(){
 opsNet.deleteSet=true;
 for(unsigned i=0;i<32;++i)opsNet.deleteKey[i]=uint8_t(i);
 Preferences p;assert(p.putBytes("deletekey",opsNet.deleteKey,32)==32);
 makeArchive(OPS_ARCHIVE_PATH,41,1);
 OpsArchiveHeader h;assert(opsReadHeader(h));
 connect();ack(h);assert(opsClient.written=="ERROR ACKFILE\n");
 tcp("GET");assert(opsClient.written.find("END\n")!=std::string::npos);
 ack(h);assert(opsClient.written=="ACKFILE_OK\n");
 ack(h);assert(opsClient.written=="ACKFILE_OK\n");
 assert(opsUsbCommand("OPS PURGE"));
 assert(opsUsbCommand("OPS PURGE EXTRA"));
 assert(LittleFS.exists(OPS_ARCHIVE_PATH)&&opsLastAck==opsIdentity(h));
 opsLastAck={};opsLoadNet();assert(opsLastAck==opsIdentity(h));
 makeArchive(OPS_ARCHIVE_PATH,41,2);
 assert(!opsEraseAcked());
 makeArchive(OPS_ARCHIVE_PATH,42,1);
 assert(!opsEraseAcked());
 makeArchive(OPS_ARCHIVE_PATH,41,1);
 assert(opsEraseAcked());
 assert(!nvs.count("file_ack_v3"));
 makeArchive(OPS_ARCHIVE_PATH,41,1);
 assert(!opsEraseAcked());
 LittleFS.remove(OPS_ARCHIVE_PATH);
}
static void testDownloadReadFailure(){
 makeArchive(OPS_ARCHIVE_PATH,41,5);
 connect();opsClient.failDataRead=true;
 tcp("GET");
 assert(!opsClient.oversizedWrite);
 assert(!opsClient.connected()&&opsDownloaded.boot==0);
 assert(opsClient.written.find("END\n")==std::string::npos);
 opsClient.failDataRead=false;
 connect();opsClient.corruptData=true;
 tcp("GET");
 assert(!opsClient.connected()&&opsDownloaded.boot==0);
 assert(opsClient.written.find("END\n")==std::string::npos);
 opsClient.corruptData=false;
 assert(LittleFS.remove(OPS_ARCHIVE_PATH));
}
static void testPersistedHeader(){
 opsArchive=new OpsArchiveRow[1]{};
 opsCount=opsRows=1;opsSession=12;
 LittleFS.corruptRenameHeader=true;
 assert(!opsPersistArchive());
 LittleFS.corruptRenameHeader=false;
 assert(opsArchive!=nullptr&&LittleFS.exists(OPS_ARCHIVE_PATH));
 OpsArchiveHeader h;assert(!opsReadHeader(h));
 assert(LittleFS.remove(OPS_ARCHIVE_PATH));
 delete[] opsArchive;opsArchive=nullptr;
 opsCount=opsRows=opsSession=0;
}
static void testDistinctKeys(){
 const char* hex="000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
 assert(!opsSaveHexField("AUTH",hex)&&!nvs.count("auth"));
 // Reproduce equal keys loaded from a previously permissive firmware.
 Preferences p;assert(p.putBytes("auth",opsNet.deleteKey,32)==32);
 opsLoadNet();assert(opsNet.authSet&&opsNet.deleteSet);
 makeArchive(OPS_ARCHIVE_PATH,41,6);
 OpsArchiveHeader h;assert(opsReadHeader(h));
 connect();tcp("GET");assert(opsDownloaded==opsIdentity(h));
 ack(h);assert(opsClient.written=="ERROR ACKFILE\n");
 assert(!nvs.count("file_ack_v3")&&LittleFS.remove(OPS_ARCHIVE_PATH));
 assert(p.remove("auth"));opsLoadNet();
 // An approval issued by the old firmware cannot survive the upgrade.
 const auto identity=opsIdentity(h);
 assert(p.putBytes("file_ack_v2",&identity,sizeof identity)==sizeof identity);
 opsLoadNet();assert(opsLastAck.boot==0);
 assert(p.remove("file_ack_v2"));
 const char* distinct="ffeeddccbbaa99887766554433221100ffeeddccbbaa99887766554433221100";
 connect();assert(opsSaveHexField("AUTH",distinct));
 assert(!opsClient.connected()&&!opsTcpAuthorized);
 assert(p.remove("auth"));opsLoadNet();
}
static void testOtaImageBinding(){
 opsNet.otaSet=true;WiFi.selected=WIFI_STA;
 connect();tcp("OTA 60");
 assert(opsClient.written=="ERROR OTA_IMAGE_ATTESTATION_REQUIRED\n");
 assert(!opsOtaOpen&&ArduinoOTA.beginCalls==0);
 assert(opsUsbCommand("OPS OTA 60"));
 assert(!opsOtaOpen&&ArduinoOTA.beginCalls==0);
 opsNet.otaSet=false;
}
static void testRecovery(){
 makeArchive(OPS_ARCHIVE_TMP,41,3);
 opsLoadArchiveState();assert(opsPhase==OpsPhase::Stored);
 assert(!LittleFS.exists(OPS_ARCHIVE_TMP));
 LittleFS.remove(OPS_ARCHIVE_PATH);
 makeArchive(OPS_ARCHIVE_TMP,41,4);
 LittleFS.files[OPS_ARCHIVE_TMP].back()^=1;
 auto bytes=LittleFS.files[OPS_ARCHIVE_TMP];
 opsLoadArchiveState();assert(opsPhase==OpsPhase::Recovery);
 assert(LittleFS.files[OPS_ARCHIVE_TMP]==bytes);
 assert(opsUsbCommand("OPS QUARANTINE EXTRA"));
 assert(opsPhase==OpsPhase::Recovery&&LittleFS.files[OPS_ARCHIVE_TMP]==bytes);
 assert(opsUsbCommand("OPS QUARANTINE"));
 assert(opsPhase==OpsPhase::Idle);
 assert(LittleFS.files[OPS_ARCHIVE_QUARANTINE]==bytes);
}
static void testInvalidTcpNumbers(){
 config.stage=6;config.period_us=200000;config.baud=2000000;
 opsPhase=OpsPhase::Idle;active=false;WiFi.selected=WIFI_STA;
 connect();
 for(const char* cmd:{"START -1 1","START +1 1","START 4294967296 1",
                       "START 12 1 EXTRA","START 13 -1","START 1 4294967296"}){
  tcp(cmd);assert(opsPhase==OpsPhase::Idle&&starts==0);
 }
 // Supply a real checksum-valid archive and a completed GET; malformed
 // inputs must be rejected even when every other ACKFILE condition is true.
 makeArchive(OPS_ARCHIVE_PATH,41,9);
 OpsArchiveHeader h;assert(opsReadHeader(h));
 tcp("GET");assert(opsClient.written.find("END\n")!=std::string::npos);
 assert(opsDownloaded==opsIdentity(h));
 for(const char* cmd:{"ACKFILE -1 0","ACKFILE 4294967296 0",
                      "ACKFILE 23 -1","ACKFILE 23 4294967296",
                      "ACKFILE 23 0 EXTRA"}){
  tcp(cmd);assert(opsClient.written=="ERROR ACKFILE\n");
 }
 tcp("GET EXTRA");assert(opsClient.written=="ERROR EXTRA_ARGS\n");
 tcp("PURGE EXTRA");assert(opsClient.written=="ERROR PURGE_GUARD\n");
 tcp("STATUS EXTRA");assert(opsClient.written=="ERROR EXTRA_ARGS\n");
 tcp("OTA +60");assert(opsClient.written=="ERROR OTA_GUARD\n");
 tcp("OTA 60 EXTRA");assert(opsClient.written=="ERROR OTA_GUARD\n");
 assert(opsLastAck.boot==0&&opsLastAck.session==0);
 assert(!nvs.count("file_ack_v3"));
 assert(LittleFS.remove(OPS_ARCHIVE_PATH));
 opsDownloaded={};
}

static void testArchiveCapacityGuards(){
 opsFsOk=true;opsPhase=OpsPhase::Idle;active=false;hwOk=true;
 config.stage=6;config.period_us=200000;config.baud=2000000;
 const size_t old=LittleFS.capacity;
 LittleFS.capacity=12000;
 assert(!opsArchiveSpaceReady(32));
 assert(!opsPlan(90,32));assert(opsArchive==nullptr);
 LittleFS.capacity=old;
 assert(opsArchiveSpaceReady(32));
 assert(opsPlan(91,32));assert(opsArchive!=nullptr);
 delete[] opsArchive;opsArchive=nullptr;
 opsPhase=OpsPhase::Idle;opsSession=opsCount=opsRows=0;
}

static void testWifiAutostartIsPersistentAndExplicit(){
 opsPhase=OpsPhase::Idle;active=false;opsWifiAutoStart=true;
 WiFi.selected=WIFI_STA;
 assert(opsUsbCommand("OPS NET AUTOSTART 0"));
 assert(!opsWifiAutoStart&&WiFi.selected==WIFI_OFF&&!opsWantNetwork);
 opsWifiAutoStart=true;opsLoadNet();
 assert(!opsWifiAutoStart);
 assert(opsUsbCommand("OPS NET AUTOSTART 1"));
 assert(opsWifiAutoStart);
 assert(opsUsbCommand("OPS NET AUTOSTART -1"));
 assert(opsWifiAutoStart); // malformed input cannot silently change policy
}

static void testEnterpriseRequiresCA(){
 opsNet.eapEnabled=false;
 strcpy(opsNet.ssid,"EAP_TEST");
 strcpy(opsNet.eapIdentity,"id");strcpy(opsNet.eapUser,"user");strcpy(opsNet.eapPassword,"pass");
 assert(!opsEapReady());
 assert(opsUsbCommand("OPS NET EAP 1"));
 assert(!opsNet.eapEnabled);
 assert(opsUsbCommand("OPS NET EAP +1"));
 assert(!opsNet.eapEnabled);
 assert(opsUsbCommand("OPS NET EAP 0"));
 assert(!opsNet.eapEnabled);
}
static void testWifiStart(){
 config.stage=6;config.period_us=200000;config.baud=2000000;
 WiFi.selected=WIFI_STA;opsWifiUp=true;
 connect();tcp("START 78 1");
 assert(opsClient.written=="START_READY 78 00000011\n");
 assert(opsPhase==OpsPhase::Armed&&opsStartAt==0&&WiFi.selected==WIFI_STA);
 nowUs+=1000000;opsLoop();assert(starts==0);
 for(const char* wrong:{"START_ACK 77 00000011","START_ACK 78 00000012",
                         "START_ACK 78 00000011 EXTRA"}){
  tcp(wrong);assert(opsClient.written=="ERROR START_ACK_GUARD\n");
  assert(WiFi.selected==WIFI_STA&&opsStartAt==0);
 }
 nowUs+=uint64_t(OPS_START_ACK_TIMEOUT_MS)*1000;
 tcp("START_ACK 78 00000011");
 assert(opsClient.written=="ERROR START_ACK_GUARD\n");
 opsLoop();
 assert(opsPhase==OpsPhase::Idle&&opsArchive==nullptr&&starts==0&&WiFi.selected==WIFI_STA);
 assert(std::string(opsFailure)=="START_ACK_TIMEOUT");
 connect();tcp("START 79 1");
 opsClient.stop();opsLoop();
 assert(opsPhase==OpsPhase::Idle&&std::string(opsFailure)=="START_ACK_DISCONNECTED");
 connect();tcp("START_ACK 79 00000011");
 assert(opsClient.written=="ERROR START_ACK_GUARD\n"&&WiFi.selected==WIFI_STA);
 WiFi.rejectOff=true;
 tcp("START 80 1");tcp("START_ACK 80 00000011");
 assert(opsPhase==OpsPhase::Fault&&std::string(opsFailure)=="WIFI_OFF_FAILED");
 assert(opsArchive==nullptr);
 nowUs+=1000000;opsLoop();assert(starts==0);
 opsPhase=OpsPhase::Idle;WiFi.rejectOff=false;connect();tcp("START 81 1");
 tcp("START_ACK 81 00000011");
 assert(opsPhase==OpsPhase::Armed);assert(WiFi.selected==WIFI_OFF);
 WiFi.selected=WIFI_STA;nowUs+=1000000;opsLoop();
 assert(starts==0&&std::string(opsFailure)=="WIFI_OFF_LOST");
 opsPhase=OpsPhase::Idle;connect();tcp("START 82 1");
 opsCancelScheduledStart();assert(opsArchive==nullptr);
 nowUs+=1000000;opsLoop();assert(starts==0);
 connect();tcp("START 83 1");
 nowUs+=1000000;opsLoop();assert(starts==0&&WiFi.selected==WIFI_STA);
 opsTcpAuthorized=false;tcp("START_ACK 83 00000011");
 assert(opsClient.written=="AUTH_REQUIRED\n"&&WiFi.selected==WIFI_STA);
 opsTcpAuthorized=true;tcp("START_ACK 83 00000011");
 assert(WiFi.selected==WIFI_OFF&&opsStartAckSession==83);
 nowUs+=249999;opsLoop();assert(starts==0);
 ++nowUs;opsLoop();assert(starts==1&&opsPhase==OpsPhase::Measuring);
 delete[] opsArchive;opsArchive=nullptr;
}
}

int main(){
 using namespace dual8app;
 opsFsOk=true;
 testDownloadReadFailure();testPersistedHeader();testApproval();testDistinctKeys();
 testOtaImageBinding();testRecovery();testInvalidTcpNumbers();testArchiveCapacityGuards();testWifiAutostartIsPersistentAndExplicit();testEnterpriseRequiresCA();testWifiStart();
 std::puts("PASS operational firmware mock: archive integrity, custody, OTA and Wi-Fi guards");
}
