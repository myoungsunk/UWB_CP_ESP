#include "core.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

// Test against both DUAL8_ANCHOR=0/1, without ESP32 NVS or Wi-Fi.
namespace dual8app {
struct Preferences {
 static std::vector<uint8_t> saved;
 bool begin(const char*,bool){return true;}
 void end(){}
 size_t getBytesLength(const char*){return saved.size();}
 size_t getBytes(const char*,void*dest,size_t n){
  if(saved.size()!=n)return 0;
  std::memcpy(dest,saved.data(),n);return n;
 }
 size_t putBytes(const char*,const void*src,size_t n){
  const auto*p=static_cast<const uint8_t*>(src);
  saved.assign(p,p+n);return n;
 }
 bool remove(const char*){saved.clear();return true;}
};
std::vector<uint8_t> Preferences::saved;
#include "operational_profile.hpp"

static void verify(){
 assert(std::strcmp(profileName(),"MISSING")==0);
 dual8::Config config;
 assert(profileLoad(config)==ProfileStatus::Missing);
 assert(config.stage==5&&config.baud==921600);
 dual8::Config good=config;good.stage=6;good.period_us=200000;good.baud=2000000;
 assert(dual8::validate(good)==nullptr);
 assert(profileSave(good));
 assert(profileLoad(config)==ProfileStatus::Valid);
 assert(config.stage==6&&config.period_us==200000&&config.baud==2000000);
 ProfileRecord rec;
 std::memcpy(&rec,Preferences::saved.data(),sizeof rec);
 rec.role=DUAL8_ANCHOR?2:1;
 rec.crc32=profileCrc(rec);
 std::memcpy(Preferences::saved.data(),&rec,sizeof rec);
 dual8::Config scratch;
 assert(profileLoad(scratch)==ProfileStatus::Corrupt);
 assert(scratch.stage==5&&scratch.baud==921600);
 rec.role=DUAL8_ANCHOR?1:2;
 rec.crc32=profileCrc(rec);
 std::memcpy(Preferences::saved.data(),&rec,sizeof rec);
 Preferences::saved[7]^=0x1; // length/schema corruption
 assert(profileLoad(scratch)==ProfileStatus::Corrupt);
 Preferences::saved.resize(8);
 assert(profileLoad(scratch)==ProfileStatus::Corrupt);
 assert(profileClear());
 assert(profileLoad(scratch)==ProfileStatus::Missing);
 dual8::Config invalid=good;invalid.stage=5;
 assert(!profileSave(invalid));
}
}
int main(){
 dual8app::verify();
 std::puts("PASS profile CRC, cross-role and corruption fallback");
}
