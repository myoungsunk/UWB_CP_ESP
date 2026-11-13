#include "core.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>
#include <new>
#include <string>
#include <vector>

namespace dual8app {
using namespace dual8;
enum { WIFI_OFF, WIFI_STA, WL_CONNECTED, SeekSet, U_FLASH };
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

struct File {
 std::vector<uint8_t>* data=nullptr;
 size_t pos=0;
 explicit File(std::vector<uint8_t>* p=nullptr):data(p){}
 explicit operator bool() const{return data;}
 bool isDirectory() const{return false;}
 File openNextFile(){return File();}
 size_t size() const{return data->size();}
 int read(uint8_t* p,size_t n){
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
 bool begin(bool format){assert(!format);return true;}
 bool exists(const char* p){return files.count(p);}
 File open(const char* p,const char* mode="r"){
  if(*mode=='w'){files[p].clear();return File(&files[p]);}
  return exists(p)?File(&files[p]):File();
 }
 bool rename(const char* from,const char* to){
  if(!exists(from)||exists(to))return false;
  files[to]=files[from];files.erase(from);return true;
 }
 bool remove(const char* path){return files.erase(path)==1;}
} LittleFS;
static std::map<std::string,std::vector<uint8_t>> nvs;
struct Preferences {
 bool begin(const char*,bool){return true;}
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
 bool connectedFlag=false;
 std::string written;
 explicit operator bool() const{return connectedFlag;}
 bool connected() const{return connectedFlag;}
 void stop(){connectedFlag=false;}
 size_t print(const char* s){written+=s;return std::strlen(s);}
 size_t print(char c){written+=c;return 1;}
 size_t write(const uint8_t* p,size_t size){written.append(reinterpret_cast<const char*>(p),size);return size;}
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
 int getCommand(){return U_FLASH;}
 void begin(){}
 void end(){}
 void handle(){}
} ArduinoOTA;
struct Updater {void abort(){}} Update;
struct Esp {unsigned getFreeHeap(){return 100000;}} ESP;
using ota_error_t=int;
struct mbedtls_md_info_t {};
enum { MBEDTLS_MD_SHA256 };
static const mbedtls_md_info_t* mbedtls_md_info_from_type(int){return nullptr;}
static int mbedtls_md_hmac(const mbedtls_md_info_t*,const uint8_t*,size_t,
                           const uint8_t*,size_t,uint8_t*){return 1;}
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
 opsClient.connectedFlag=true;opsTcpAuthorized=true;opsDownloaded={};
}
static void ack(const OpsArchiveHeader& h){
 char cmd[64];std::snprintf(cmd,sizeof cmd,"ACKFILE %u %u",h.session,h.body_crc);
 tcp(cmd);
}
static void testApproval(){
 makeArchive(OPS_ARCHIVE_PATH,41,1);
 OpsArchiveHeader h;assert(opsReadHeader(h));
 connect();ack(h);assert(opsClient.written=="ERROR ACKFILE\n");
 tcp("GET");assert(opsClient.written.find("END\n")!=std::string::npos);
 ack(h);assert(opsClient.written=="ACKFILE_OK\n");
 ack(h);assert(opsClient.written=="ACKFILE_OK\n");
 opsLastAck={};opsLoadNet();assert(opsLastAck==opsIdentity(h));
 makeArchive(OPS_ARCHIVE_PATH,41,2);
 assert(!opsEraseAcked());
 makeArchive(OPS_ARCHIVE_PATH,42,1);
 assert(!opsEraseAcked());
 makeArchive(OPS_ARCHIVE_PATH,41,1);
 assert(opsEraseAcked());
 assert(!nvs.count("file_ack_v2"));
 makeArchive(OPS_ARCHIVE_PATH,41,1);
 assert(!opsEraseAcked());
 LittleFS.remove(OPS_ARCHIVE_PATH);
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
 assert(opsUsbCommand("OPS QUARANTINE"));
 assert(opsPhase==OpsPhase::Idle);
 assert(LittleFS.files[OPS_ARCHIVE_QUARANTINE]==bytes);
}
static void testWifiStart(){
 config.stage=6;config.period_us=200000;config.baud=2000000;
 WiFi.selected=WIFI_STA;WiFi.rejectOff=true;
 connect();tcp("START 80 1");
 assert(opsPhase==OpsPhase::Fault&&std::string(opsFailure)=="WIFI_OFF_FAILED");
 nowUs+=1000000;opsLoop();assert(starts==0);
 opsPhase=OpsPhase::Idle;WiFi.rejectOff=false;connect();tcp("START 81 1");
 assert(opsPhase==OpsPhase::Armed);assert(WiFi.selected==WIFI_OFF);
 WiFi.selected=WIFI_STA;nowUs+=1000000;opsLoop();
 assert(starts==0&&std::string(opsFailure)=="WIFI_OFF_LOST");
 opsPhase=OpsPhase::Idle;connect();tcp("START 82 1");
 opsCancelScheduledStart();nowUs+=1000000;opsLoop();assert(starts==0);
 connect();tcp("START 83 1");
 nowUs+=1000000;opsLoop();assert(starts==1&&opsPhase==OpsPhase::Measuring);
 delete[] opsArchive;opsArchive=nullptr;
}
}

int main(){
 using namespace dual8app;
 opsFsOk=true;
 testApproval();testRecovery();testWifiStart();
 std::puts("PASS operational firmware mock: ACK reuse, journal recovery, Wi-Fi start guards");
}
