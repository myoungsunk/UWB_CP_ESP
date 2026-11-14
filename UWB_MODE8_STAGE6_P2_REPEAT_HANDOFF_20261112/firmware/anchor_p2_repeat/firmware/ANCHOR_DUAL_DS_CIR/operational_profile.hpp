#pragma once
// Explicitly opted-in, CRC-verified Mode 8 Stage 6 operating profile.
// Neither RF antenna calibration nor autostart is implied by this profile.
// Included inside dual8app after Preferences.h and core.hpp.
enum class ProfileStatus:uint8_t { Missing, Valid, Corrupt };
static ProfileStatus profileStatus=ProfileStatus::Missing;
static constexpr uint32_t PROFILE_MAGIC=0x3846504dUL;
#pragma pack(push,1)
struct ProfileRecord {
 uint32_t magic=PROFILE_MAGIC;
 uint16_t version=1,bytes=sizeof(ProfileRecord);
 uint8_t role=DUAL8_ANCHOR?1:2,stage=6;
 uint16_t reserved=0;
 uint32_t period_us=200000,baud=2000000,crc32=0;
};
#pragma pack(pop)
static_assert(sizeof(ProfileRecord)==24,"profile layout drift");
static uint32_t profileCrc(const ProfileRecord& r){
 ProfileRecord x=r;x.crc32=0;
 return dual8::crc32(&x,sizeof x);
}
static bool profileValid(const ProfileRecord& r){
 return r.magic==PROFILE_MAGIC&&r.version==1&&r.bytes==sizeof r&&
        r.role==(DUAL8_ANCHOR?1:2)&&r.stage==6&&
        r.period_us==200000&&r.baud==2000000&&r.reserved==0&&
        r.crc32==profileCrc(r);
}
static ProfileStatus profileLoad(dual8::Config& cfg){
 Preferences p;
 if(!p.begin("m8profile",true))return ProfileStatus::Missing;
 const size_t len=p.getBytesLength("operating");
 if(!len){p.end();return ProfileStatus::Missing;}
 ProfileRecord r;
 const size_t read=len==sizeof r?p.getBytes("operating",&r,sizeof r):0;
 p.end();
 if(read!=sizeof r||!profileValid(r))return ProfileStatus::Corrupt;
 dual8::Config next=cfg;
 next.stage=r.stage;next.period_us=r.period_us;next.baud=r.baud;
 if(dual8::validate(next))return ProfileStatus::Corrupt;
 cfg=next;return ProfileStatus::Valid;
}
static bool profileSave(const dual8::Config& cfg){
 if(cfg.stage!=6||cfg.period_us!=200000||cfg.baud!=2000000||
    dual8::validate(cfg))return false;
 ProfileRecord r;r.crc32=profileCrc(r);
 Preferences p;if(!p.begin("m8profile",false))return false;
 const bool ok=p.putBytes("operating",&r,sizeof r)==sizeof r;
 p.end();if(ok)profileStatus=ProfileStatus::Valid;
 return ok;
}
static bool profileClear(){
 Preferences p;if(!p.begin("m8profile",false))return false;
 const bool ok=p.getBytesLength("operating")==0||p.remove("operating");
 p.end();if(ok)profileStatus=ProfileStatus::Missing;
 return ok;
}
static const char*profileName(){
 return profileStatus==ProfileStatus::Valid?"VALID":
        profileStatus==ProfileStatus::Corrupt?"CORRUPT":"MISSING";
}
