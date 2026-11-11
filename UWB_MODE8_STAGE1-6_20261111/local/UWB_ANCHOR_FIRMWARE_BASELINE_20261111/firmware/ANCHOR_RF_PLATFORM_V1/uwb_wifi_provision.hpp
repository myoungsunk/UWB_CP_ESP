#line 1 "C:\\UWB\\UWB_R04_R1J6_FIELD_FIX_20261010\\R1M_CTRL_V2_RC1\\built\\sketches\\ANCHOR_RF_PLATFORM_V1\\uwb_wifi_provision.hpp"
#pragma once

// UWB Wi-Fi provisioning transport for RF_PLATFORM_V1.
// Included INSIDE namespace bench.
// Measurement AirPacket schema/timing is intentionally untouched.

#include "uwb_provision_key.hpp"

constexpr uint32_t UWB_PROV_MAGIC = 0x31505755UL; // "UWP1" in little-endian memory
constexpr uint8_t UWB_PROV_VERSION = 1;
constexpr uint8_t UWB_PROV_FLAG_ENCRYPTED = 0x01;
constexpr size_t UWB_PROV_MAX_PAYLOAD = 60;
constexpr size_t UWB_PROV_MAX_FRAME = 120;
constexpr uint32_t UWB_PROV_RX_TIMEOUT_MS = 1200;
constexpr uint32_t UWB_PROV_CONFIG_TIMEOUT_MS = 60000;
constexpr uint8_t UWB_PROV_RETRIES = 3;

enum UwbProvType : uint8_t {
  UWB_PROV_INFO_REQ   = 1,
  UWB_PROV_STATUS     = 2,
  UWB_PROV_CFG_FRAG   = 3,
  UWB_PROV_FRAG_ACK   = 4,
  UWB_PROV_CFG_COMMIT = 5,
  UWB_PROV_CLEAR_REQ  = 6,
  UWB_PROV_RECONNECT_REQ = 7 // R1F: restart Anchor STA with stored credentials (no secrets on air)
};

enum UwbProvStatusCode : uint8_t {
  UWB_PROV_OK = 0,
  UWB_PROV_BUSY = 1,
  UWB_PROV_BAD_FRAME = 2,
  UWB_PROV_AUTH_FAIL = 3,
  UWB_PROV_REASSEMBLY_FAIL = 4,
  UWB_PROV_CONFIG_INVALID = 5,
  UWB_PROV_CONNECT_FAILED = 6,
  UWB_PROV_NVS_FAILED = 7,
  UWB_PROV_CLEAR_FAILED = 8,
  UWB_PROV_TIMEOUT = 9,
  UWB_PROV_UNSUPPORTED = 10
};

static const char* uwbProvStatusName(uint8_t c){
  switch(c){
    case UWB_PROV_OK:return "OK";
    case UWB_PROV_BUSY:return "BUSY";
    case UWB_PROV_BAD_FRAME:return "BAD_FRAME";
    case UWB_PROV_AUTH_FAIL:return "AUTH_FAIL";
    case UWB_PROV_REASSEMBLY_FAIL:return "REASSEMBLY_FAIL";
    case UWB_PROV_CONFIG_INVALID:return "CONFIG_INVALID";
    case UWB_PROV_CONNECT_FAILED:return "CONNECT_FAILED";
    case UWB_PROV_NVS_FAILED:return "NVS_FAILED";
    case UWB_PROV_CLEAR_FAILED:return "CLEAR_FAILED";
    case UWB_PROV_TIMEOUT:return "TIMEOUT";
    default:return "UNSUPPORTED";
  }
}

#pragma pack(push,1)
struct UwbProvHeader {
  uint32_t magic=UWB_PROV_MAGIC;
  uint8_t version=UWB_PROV_VERSION;
  uint8_t type=0;
  uint8_t flags=0;
  uint8_t frag_index=0;
  uint8_t frag_count=1;
  uint8_t reserved0[3]{};
  uint32_t request=0;
  uint32_t sender_boot=0;
  uint16_t total_bytes=0;
  uint16_t offset=0;
  uint16_t payload_bytes=0;
  uint16_t reserved1=0;
  uint8_t nonce[12]{};
  uint8_t tag[16]{};
};
struct UwbProvWifiConfigV1 {
  uint8_t enterprise=0; // 0=open/PSK, 1=PEAP using existing RemoteNetConfig semantics
  char ssid[33]{};
  char password[65]{};
  char hostname[33]{};
  char identity[65]{};
  char username[65]{};
  uint32_t crc32=0;
};
struct UwbProvWifiStatusV1 {
  uint8_t ok=0;
  uint8_t wifi_ready=0;
  uint8_t enterprise=0;
  uint8_t wifi_status=0;
  uint8_t code=UWB_PROV_OK;
  int8_t rssi=0;
  uint16_t tcp_port=29310;
  uint8_t ip[4]{};
  uint32_t config_crc32=0;
  char ssid[33]{};
  char hostname[33]{};
};
#pragma pack(pop)

static_assert(sizeof(UwbProvHeader)==56,"UWB provisioning header layout changed");
static_assert(sizeof(UwbProvWifiConfigV1)==266,"UWB provisioning config layout changed");
static_assert(sizeof(UwbProvWifiStatusV1)==82,"UWB provisioning status layout changed");

static uint32_t uwbProvConfigCrc(const UwbProvWifiConfigV1& in){
  UwbProvWifiConfigV1 x=in;x.crc32=0;return crc32(&x,sizeof(x));
}
static bool uwbProvConfigValid(const UwbProvWifiConfigV1& c){
  if(c.crc32!=uwbProvConfigCrc(c))return false;
  if(!c.ssid[0]||!c.hostname[0])return false;
  if(c.ssid[sizeof(c.ssid)-1]||c.password[sizeof(c.password)-1]||c.hostname[sizeof(c.hostname)-1]||c.identity[sizeof(c.identity)-1]||c.username[sizeof(c.username)-1])return false;
  if(c.enterprise && (!c.identity[0]||!c.username[0]||!c.password[0]))return false;
  return true;
}

static bool uwbProvHexDecode(const char* in,char* out,size_t cap){
  if(strcmp(in,"-")==0){if(cap)out[0]=0;return true;}
  size_t n=strlen(in);if((n&1)||n/2>=cap)return false;
  auto cv=[](char c)->int{if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1;};
  for(size_t i=0;i<n/2;i++){int a=cv(in[2*i]),b=cv(in[2*i+1]);if(a<0||b<0)return false;out[i]=char((a<<4)|b);}out[n/2]=0;return true;
}
static bool uwbProvToken(char*& cur,char* out,size_t cap){
  while(*cur&&isspace((unsigned char)*cur))++cur;if(!*cur)return false;size_t n=0;
  while(*cur&&!isspace((unsigned char)*cur)){if(n+1>=cap)return false;out[n++]=*cur++;}out[n]=0;return n>0;
}

static bool uwbProvGcmCrypt(bool encrypt,const UwbProvHeader& h,const uint8_t* in,uint8_t* out,size_t n,uint8_t tag[16]){
  mbedtls_gcm_context ctx;mbedtls_gcm_init(&ctx);
  int rc=mbedtls_gcm_setkey(&ctx,MBEDTLS_CIPHER_ID_AES,UWB_PROVISION_KEY,256);
  if(rc==0){
    const uint8_t* aad=reinterpret_cast<const uint8_t*>(&h);
    const size_t aadN=offsetof(UwbProvHeader,tag);
    if(encrypt)rc=mbedtls_gcm_crypt_and_tag(&ctx,MBEDTLS_GCM_ENCRYPT,n,h.nonce,12,aad,aadN,in,out,16,tag);
    else rc=mbedtls_gcm_auth_decrypt(&ctx,n,h.nonce,12,aad,aadN,tag,16,in,out);
  }
  mbedtls_gcm_free(&ctx);return rc==0;
}

static size_t uwbProvEncode(uint8_t type,uint8_t flags,uint32_t req,uint8_t fragIndex,uint8_t fragCount,
                            uint16_t total,uint16_t offset,const uint8_t* payload,uint16_t payloadN,uint8_t* out){
  if(payloadN>UWB_PROV_MAX_PAYLOAD||fragCount<1||fragIndex>=fragCount)return 0;
  UwbProvHeader h{};h.type=type;h.flags=flags;h.frag_index=fragIndex;h.frag_count=fragCount;h.request=req;h.sender_boot=bootId;h.total_bytes=total;h.offset=offset;h.payload_bytes=payloadN;
  uint8_t body[UWB_PROV_MAX_PAYLOAD]{};
  if(flags&UWB_PROV_FLAG_ENCRYPTED){
    uint32_t r0=esp_random(),r1=esp_random(),r2=esp_random();memcpy(h.nonce,&r0,4);memcpy(h.nonce+4,&r1,4);memcpy(h.nonce+8,&r2,4);
    uint8_t dummy=0;const uint8_t* src=payloadN?payload:&dummy;uint8_t* dst=payloadN?body:&dummy;
    if(!uwbProvGcmCrypt(true,h,src,dst,payloadN,h.tag))return 0;
  }else if(payloadN)memcpy(body,payload,payloadN);
  const size_t n=sizeof(h)+payloadN+4;if(n>UWB_PROV_MAX_FRAME)return 0;
  memcpy(out,&h,sizeof(h));if(payloadN)memcpy(out+sizeof(h),body,payloadN);
  uint32_t c=crc32(out,sizeof(h)+payloadN);memcpy(out+sizeof(h)+payloadN,&c,4);return n;
}

static bool uwbProvDecode(const uint8_t* in,size_t n,UwbProvHeader& h,uint8_t* payload,size_t cap){
  if(n<sizeof(UwbProvHeader)+4||n>UWB_PROV_MAX_FRAME)return false;memcpy(&h,in,sizeof(h));
  if(h.magic!=UWB_PROV_MAGIC||h.version!=UWB_PROV_VERSION||h.frag_count<1||h.frag_index>=h.frag_count||h.payload_bytes>UWB_PROV_MAX_PAYLOAD)return false;
  if(n!=sizeof(h)+h.payload_bytes+4||h.payload_bytes>cap)return false;
  uint32_t got=0;memcpy(&got,in+n-4,4);if(got!=crc32(in,n-4))return false;
  if(h.flags&UWB_PROV_FLAG_ENCRYPTED){
    uint8_t dummy=0;const uint8_t* src=h.payload_bytes?in+sizeof(h):&dummy;uint8_t* dst=h.payload_bytes?payload:&dummy;
    if(!uwbProvGcmCrypt(false,h,src,dst,h.payload_bytes,const_cast<uint8_t*>(h.tag)))return false;
  }else if(h.payload_bytes)memcpy(payload,in+sizeof(h),h.payload_bytes);
  return true;
}

static bool uwbProvSendRaw(uint8_t type,uint8_t flags,uint32_t req,uint8_t fragIndex,uint8_t fragCount,
                           uint16_t total,uint16_t offset,const uint8_t* payload,uint16_t payloadN){
  uint8_t frame[UWB_PROV_MAX_FRAME]{};size_t n=uwbProvEncode(type,flags,req,fragIndex,fragCount,total,offset,payload,payloadN,frame);if(!n)return false;
  idleRadio();if(!IS_ANCHOR){selectRoute(0);waitSwitch();}
#if BENCH_ANCHOR
  if(!r7ApplyAtten(R7_POLL_ATTEN_X2,nullptr))return false;
#endif
  DW1000Ng::setTransmitData(frame,uint16_t(n));uint64_t start=nowUs();DW1000Ng::startTransmit(TransmitMode::IMMEDIATE);
  while(!DW1000Ng::isTransmitDone()){if(nowUs()-start>20000){++txTimeouts;idleRadio();return false;}delayMicroseconds(20);}
  DW1000Ng::clearTransmitStatus();idleRadio();return true;
}

// 1=valid provisioning frame, 0=none, -1=non-provision/error frame consumed.
static int uwbProvPollRaw(UwbProvHeader& h,uint8_t* payload,size_t cap){
  if(!rxArmed)armRx();
  if(DW1000Ng::isReceiveDone()){
    uint16_t n=DW1000Ng::getReceivedDataLength();uint8_t raw[UWB_PROV_MAX_FRAME]{};
    if(n>UWB_PROV_MAX_FRAME||n<sizeof(UwbProvHeader)+4){finishRx();return -1;}
    DW1000Ng::getReceivedData(raw,n);finishRx();
    if(!uwbProvDecode(raw,n,h,payload,cap))return -1;return 1;
  }
  if(DW1000Ng::isReceiveFailed()){DW1000Ng::clearReceiveFailedStatus();rxArmed=false;return -1;}
  if(DW1000Ng::isReceiveTimeout()){DW1000Ng::clearReceiveTimeoutStatus();rxArmed=false;return -1;}
  return 0;
}

static bool uwbProvWaitFrame(uint8_t type,uint32_t req,uint32_t timeoutMs,UwbProvHeader& h,uint8_t* payload,size_t cap){
  uint32_t start=millis();while(millis()-start<timeoutMs){int g=uwbProvPollRaw(h,payload,cap);if(g==1){if(h.type==type&&h.request==req)return true;}if(g!=0)armRx();delay(1);}idleRadio();return false;
}

#if !BENCH_ANCHOR
static void uwbProvEmitTagStatus(uint32_t req,const char* op,bool transportOK,const UwbProvWifiStatusV1* s,uint8_t fallbackCode){
  if(!transportOK||!s){event("{\"event\":\"ACK\",\"request\":%lu,\"op\":\"%s\",\"ok\":false,\"reason\":\"%s\",\"role\":\"tag\",\"prov_key_id\":\"%s\"}",(unsigned long)req,op,uwbProvStatusName(fallbackCode),UWB_PROVISION_KEY_ID);return;}
  char sh[67]{};size_t sl=strnlen(s->ssid,sizeof(s->ssid));for(size_t i=0;i<sl;i++)snprintf(sh+2*i,3,"%02x",uint8_t(s->ssid[i]));
  event("{\"event\":\"ACK\",\"request\":%lu,\"op\":\"%s\",\"ok\":%s,\"reason\":\"%s\",\"role\":\"tag\",\"wifi_ready\":%s,\"enterprise\":%s,\"wifi_status\":%u,\"ip\":\"%u.%u.%u.%u\",\"rssi\":%d,\"tcp_port\":%u,\"config_crc32\":%lu,\"ssid_hex\":\"%s\",\"hostname\":\"%s\",\"prov_key_id\":\"%s\"}",
    (unsigned long)req,op,s->ok?"true":"false",uwbProvStatusName(s->code),s->wifi_ready?"true":"false",s->enterprise?"true":"false",unsigned(s->wifi_status),unsigned(s->ip[0]),unsigned(s->ip[1]),unsigned(s->ip[2]),unsigned(s->ip[3]),int(s->rssi),unsigned(s->tcp_port),(unsigned long)s->config_crc32,sh,s->hostname,UWB_PROVISION_KEY_ID);
}

static bool uwbProvTagReceiveStatus(uint32_t req,uint32_t timeoutMs,UwbProvWifiStatusV1& out){
  uint8_t buf[sizeof(UwbProvWifiStatusV1)]{};uint32_t mask=0;uint8_t fragCount=0;uint16_t total=0;uint32_t start=millis();
  while(millis()-start<timeoutMs){UwbProvHeader h{};uint8_t payload[UWB_PROV_MAX_PAYLOAD]{};int g=uwbProvPollRaw(h,payload,sizeof(payload));
    if(g==1&&h.type==UWB_PROV_STATUS&&h.request==req){
      if(!fragCount){fragCount=h.frag_count;total=h.total_bytes;if(total!=sizeof(UwbProvWifiStatusV1)||fragCount>8)return false;}
      if(h.frag_count!=fragCount||h.total_bytes!=total||uint32_t(h.offset)+h.payload_bytes>sizeof(buf))return false;
      memcpy(buf+h.offset,payload,h.payload_bytes);mask|=(1UL<<h.frag_index);
      uint32_t want=(1UL<<fragCount)-1;if(mask==want){memcpy(&out,buf,sizeof(out));idleRadio();return true;}
    }
    if(g!=0)armRx();delay(1);
  }idleRadio();return false;
}

static bool uwbProvTagInfo(uint32_t req,UwbProvWifiStatusV1& status){
  for(uint8_t a=0;a<UWB_PROV_RETRIES;a++){
    if(!uwbProvSendRaw(UWB_PROV_INFO_REQ,0,req,0,1,0,0,nullptr,0))continue;
    if(uwbProvTagReceiveStatus(req,3000,status))return true;
  }return false;
}

static bool uwbProvTagSendConfig(uint32_t req,const UwbProvWifiConfigV1& cfgIn,UwbProvWifiStatusV1& status){
  const uint8_t* bytes=reinterpret_cast<const uint8_t*>(&cfgIn);const uint16_t total=sizeof(cfgIn);const uint8_t fragCount=uint8_t((total+UWB_PROV_MAX_PAYLOAD-1)/UWB_PROV_MAX_PAYLOAD);
  for(uint8_t i=0;i<fragCount;i++){
    uint16_t off=uint16_t(i*UWB_PROV_MAX_PAYLOAD),n=uint16_t(total-off);if(n>UWB_PROV_MAX_PAYLOAD)n=UWB_PROV_MAX_PAYLOAD;
    bool acked=false;for(uint8_t a=0;a<UWB_PROV_RETRIES&&!acked;a++){
      if(!uwbProvSendRaw(UWB_PROV_CFG_FRAG,UWB_PROV_FLAG_ENCRYPTED,req,i,fragCount,total,off,bytes+off,n))continue;
      UwbProvHeader h{};uint8_t p[UWB_PROV_MAX_PAYLOAD]{};if(uwbProvWaitFrame(UWB_PROV_FRAG_ACK,req,UWB_PROV_RX_TIMEOUT_MS,h,p,sizeof(p))&&h.payload_bytes>=2&&p[0]==i&&p[1]==UWB_PROV_OK)acked=true;
    }if(!acked){idleRadio();return false;}
  }
  const uint8_t marker=0xA5;
  for(uint8_t a=0;a<UWB_PROV_RETRIES;a++){
    if(!uwbProvSendRaw(UWB_PROV_CFG_COMMIT,UWB_PROV_FLAG_ENCRYPTED,req,0,1,1,0,&marker,1))continue;
    if(uwbProvTagReceiveStatus(req,UWB_PROV_CONFIG_TIMEOUT_MS,status))return true;
  }return false;
}

static bool uwbProvTagClear(uint32_t req,UwbProvWifiStatusV1& status){
  const uint8_t marker=0x5A;for(uint8_t a=0;a<UWB_PROV_RETRIES;a++){
    if(!uwbProvSendRaw(UWB_PROV_CLEAR_REQ,UWB_PROV_FLAG_ENCRYPTED,req,0,1,1,0,&marker,1))continue;
    if(uwbProvTagReceiveStatus(req,5000,status))return true;
  }return false;
}

static bool uwbProvTagReconnect(uint32_t req,UwbProvWifiStatusV1& status){
  // One request id for every retry: the Anchor de-duplicates and replays its last status.
  const uint8_t marker=0xC3;for(uint8_t a=0;a<2;a++){
    if(!uwbProvSendRaw(UWB_PROV_RECONNECT_REQ,UWB_PROV_FLAG_ENCRYPTED,req,0,1,1,0,&marker,1))continue;
    if(uwbProvTagReceiveStatus(req,UWB_PROV_CONFIG_TIMEOUT_MS,status))return true;
  }return false;
}

static bool uwbProvisionTagCommand(uint32_t req,const char* op,char*& cur){
  if(strcmp(op,"UWBWIFIRECON")==0){
    if(active){ack(req,false,op,"STOP_FIRST");return true;}while(*cur&&isspace((unsigned char)*cur))++cur;if(*cur){ack(req,false,op,"EXTRA_ARGUMENT");return true;}
    UwbProvWifiStatusV1 s{};bool ok=uwbProvTagReconnect(req,s);uwbProvEmitTagStatus(req,op,ok,ok?&s:nullptr,UWB_PROV_TIMEOUT);return true;
  }
  if(strcmp(op,"UWBWIFIINFO")==0){
    if(active){ack(req,false,op,"STOP_FIRST");return true;}while(*cur&&isspace((unsigned char)*cur))++cur;if(*cur){ack(req,false,op,"EXTRA_ARGUMENT");return true;}
    UwbProvWifiStatusV1 s{};bool ok=uwbProvTagInfo(req,s);uwbProvEmitTagStatus(req,op,ok,ok?&s:nullptr,UWB_PROV_TIMEOUT);return true;
  }
  if(strcmp(op,"UWBWIFICFG")==0){
    if(active){ack(req,false,op,"STOP_FIRST");return true;}char sh[67],ph[131],host[33];
    if(!uwbProvToken(cur,sh,sizeof(sh))||!uwbProvToken(cur,ph,sizeof(ph))||!uwbProvToken(cur,host,sizeof(host))){ack(req,false,op,"SSIDHEX_PASSHEX_HOST_REQUIRED");return true;}
    while(*cur&&isspace((unsigned char)*cur))++cur;if(*cur){ack(req,false,op,"EXTRA_ARGUMENT");return true;}
    UwbProvWifiConfigV1 c{};c.enterprise=0;if(!uwbProvHexDecode(sh,c.ssid,sizeof(c.ssid))||!uwbProvHexDecode(ph,c.password,sizeof(c.password))||strlen(host)>=sizeof(c.hostname)){ack(req,false,op,"INVALID_WIFI_ENCODING_OR_LENGTH");return true;}snprintf(c.hostname,sizeof(c.hostname),"%s",host);c.crc32=uwbProvConfigCrc(c);
    UwbProvWifiStatusV1 s{};bool ok=uwbProvTagSendConfig(req,c,s);uwbProvEmitTagStatus(req,op,ok,ok?&s:nullptr,UWB_PROV_TIMEOUT);return true;
  }
  if(strcmp(op,"UWBWIFIEAP")==0){
    if(active){ack(req,false,op,"STOP_FIRST");return true;}char sh[67],ih[131],uh[131],ph[131],host[33];
    if(!uwbProvToken(cur,sh,sizeof(sh))||!uwbProvToken(cur,ih,sizeof(ih))||!uwbProvToken(cur,uh,sizeof(uh))||!uwbProvToken(cur,ph,sizeof(ph))||!uwbProvToken(cur,host,sizeof(host))){ack(req,false,op,"EAP_FIELDS_REQUIRED");return true;}
    while(*cur&&isspace((unsigned char)*cur))++cur;if(*cur){ack(req,false,op,"EXTRA_ARGUMENT");return true;}
    UwbProvWifiConfigV1 c{};c.enterprise=1;
    if(!uwbProvHexDecode(sh,c.ssid,sizeof(c.ssid))||!uwbProvHexDecode(ih,c.identity,sizeof(c.identity))||!uwbProvHexDecode(uh,c.username,sizeof(c.username))||!uwbProvHexDecode(ph,c.password,sizeof(c.password))||!c.identity[0]||!c.username[0]||!c.password[0]||strlen(host)>=sizeof(c.hostname)){ack(req,false,op,"EAP_FIELDS_INVALID");return true;}
    snprintf(c.hostname,sizeof(c.hostname),"%s",host);c.crc32=uwbProvConfigCrc(c);UwbProvWifiStatusV1 s{};bool ok=uwbProvTagSendConfig(req,c,s);uwbProvEmitTagStatus(req,op,ok,ok?&s:nullptr,UWB_PROV_TIMEOUT);return true;
  }
  if(strcmp(op,"UWBWIFICLEAR")==0){
    if(active){ack(req,false,op,"STOP_FIRST");return true;}while(*cur&&isspace((unsigned char)*cur))++cur;if(*cur){ack(req,false,op,"EXTRA_ARGUMENT");return true;}
    UwbProvWifiStatusV1 s{};bool ok=uwbProvTagClear(req,s);uwbProvEmitTagStatus(req,op,ok,ok?&s:nullptr,UWB_PROV_TIMEOUT);return true;
  }
  return false;
}
#else

static UwbProvWifiConfigV1 uwbProvAnchorBuf{};static uint32_t uwbProvAnchorReq=0;static uint8_t uwbProvAnchorFragCount=0;static uint32_t uwbProvAnchorMask=0;static uint32_t uwbProvAnchorLastMs=0;
static UwbProvWifiStatusV1 uwbProvLastStatus{};static uint32_t uwbProvLastStatusReq=0;static bool uwbProvLastStatusValid=false;

// R1F: read-only INFO and RECONNECT are also served in TRANSFER/FINALIZE/CONFIGURED while the
// Anchor Wi-Fi is down, so a power-only Anchor stays diagnosable through the Tag after a route.
static bool uwbProvAnchorRecoveryAllowed(){
  if(active||remotePreparePending||remoteWaitPending||remoteWaitingUwb||remoteCaptureEnabled||r03SessionActive||r03WaitPending)return false;
  switch(remoteState){
    case RemoteState::PREPARED:case RemoteState::ARMED:case RemoteState::WAIT_UWB:case RemoteState::MEASURE:
    case RemoteState::ROUTE_PREPARED:case RemoteState::ROUTE_WAIT_POINT:case RemoteState::POINT_MEASURE:return false;
    default:return true;
  }
}
static uint32_t uwbProvReconLastReq=0;static UwbProvWifiStatusV1 uwbProvReconLastStatus{};static bool uwbProvReconLastValid=false;
static bool uwbProvAnchorAllowed(){
  if(active||remotePreparePending||remoteWaitPending||remoteWaitingUwb||remoteCaptureEnabled)return false;
  if(remoteState==RemoteState::PREPARED||remoteState==RemoteState::ARMED||remoteState==RemoteState::WAIT_UWB||remoteState==RemoteState::MEASURE||remoteState==RemoteState::TRANSFER)return false;
  return remoteState==RemoteState::MAINTENANCE||remoteState==RemoteState::FAULT||!remoteWifiReady;
}
static void uwbProvFillStatus(UwbProvWifiStatusV1& s,bool ok,uint8_t code){
  s=UwbProvWifiStatusV1{};s.ok=ok?1:0;s.wifi_ready=remoteWifiReady?1:0;s.enterprise=remoteNetValid&&remoteNet.enterprise?1:0;s.wifi_status=uint8_t(WiFi.status());s.code=code;s.rssi=remoteWifiReady?int8_t(WiFi.RSSI()):0;s.tcp_port=REMOTE_TCP_PORT;
  IPAddress ip=WiFi.localIP();for(unsigned i=0;i<4;i++)s.ip[i]=ip[i];s.config_crc32=remoteNetValid?remoteNet.crc:0;if(remoteNetValid){snprintf(s.ssid,sizeof(s.ssid),"%s",remoteNet.ssid);snprintf(s.hostname,sizeof(s.hostname),"%s",remoteNet.hostname);}
}
static bool uwbProvSendStatus(uint32_t req,const UwbProvWifiStatusV1& s){
  delay(8);const uint8_t* p=reinterpret_cast<const uint8_t*>(&s);uint16_t total=sizeof(s);uint8_t fc=uint8_t((total+UWB_PROV_MAX_PAYLOAD-1)/UWB_PROV_MAX_PAYLOAD);
  for(uint8_t i=0;i<fc;i++){uint16_t off=uint16_t(i*UWB_PROV_MAX_PAYLOAD),n=uint16_t(total-off);if(n>UWB_PROV_MAX_PAYLOAD)n=UWB_PROV_MAX_PAYLOAD;if(!uwbProvSendRaw(UWB_PROV_STATUS,0,req,i,fc,total,off,p+off,n))return false;delay(8);}return true;
}
static void uwbProvSendFragAck(uint32_t req,uint8_t idx,uint8_t code){delay(8);uint8_t p[2]={idx,code};uwbProvSendRaw(UWB_PROV_FRAG_ACK,0,req,0,1,2,0,p,2);armRx();}

static bool uwbProvTryCandidate(const UwbProvWifiConfigV1& c,uint8_t& code){
  if(!uwbProvConfigValid(c)){code=UWB_PROV_CONFIG_INVALID;return false;}
  RemoteNetConfig old=remoteNet;bool oldValid=remoteNetValid;RemoteState oldState=remoteState;
  RemoteNetConfig candidate{};candidate.enterprise=c.enterprise!=0;snprintf(candidate.ssid,sizeof(candidate.ssid),"%s",c.ssid);snprintf(candidate.password,sizeof(candidate.password),"%s",c.password);snprintf(candidate.hostname,sizeof(candidate.hostname),"%s",c.hostname);snprintf(candidate.identity,sizeof(candidate.identity),"%s",c.identity);snprintf(candidate.username,sizeof(candidate.username),"%s",c.username);
  wifiDiagPinned=false;wifiRecoveryAttempts=0;remoteWifiStop();remoteNet=candidate;remoteNetValid=true;remoteState=RemoteState::MAINTENANCE;remoteWifiStart();
  uint32_t start=millis();bool connected=false;while(millis()-start<REMOTE_WIFI_CONNECT_TIMEOUT_MS){
    if(WiFi.status()==WL_CONNECTED){remoteWifiPollState();if(remoteWifiReady){connected=true;break;}}
    delay(50);
  }
  if(!connected){remoteWifiStop();remoteNet=old;remoteNetValid=oldValid;remoteState=oldState;if(oldValid){wifiDiagPinned=false;wifiRecoveryAttempts=0;remoteWifiStart();}code=UWB_PROV_CONNECT_FAILED;return false;}
  if(!remoteSaveNet()){
    remoteWifiStop();remoteNet=old;remoteNetValid=oldValid;remoteState=oldState;if(oldValid){wifiDiagPinned=false;wifiRecoveryAttempts=0;remoteWifiStart();}code=UWB_PROV_NVS_FAILED;return false;
  }
  remoteNetValid=true;remoteState=RemoteState::MAINTENANCE;code=UWB_PROV_OK;return true;
}

static void uwbProvisionAnchorLoop(){
  // R03 owns the radio throughout the route, including control-frame waits.
  // Provisioning must not stop or consume its receiver.
  // R1M: route-end ACK replay owns the DW1000 during its bounded window.
  // Otherwise the provisioning listener can consume or clear repeated ROUTE_END.
  if(r03SessionActive || r1mEndReplaySinceMs)return;
  const bool fullAllowed=uwbProvAnchorAllowed();
  const bool recoveryListen=!fullAllowed&&uwbProvAnchorRecoveryAllowed();  // R1J9: also while Wi-Fi is up (TCP may be dead)
  if(!fullAllowed&&!recoveryListen){if(rxArmed&&!remoteWaitingUwb)idleRadio();return;}
  if(uwbProvAnchorReq&&millis()-uwbProvAnchorLastMs>10000){uwbProvAnchorReq=0;uwbProvAnchorFragCount=0;uwbProvAnchorMask=0;uwbProvAnchorBuf=UwbProvWifiConfigV1{};}
  UwbProvHeader h{};uint8_t payload[UWB_PROV_MAX_PAYLOAD]{};int g=uwbProvPollRaw(h,payload,sizeof(payload));if(g<=0){if(g<0)armRx();return;}
  if(h.type==UWB_PROV_INFO_REQ){UwbProvWifiStatusV1 s{};uwbProvFillStatus(s,true,UWB_PROV_OK);uwbProvSendStatus(h.request,s);return;}
  if(h.type==UWB_PROV_RECONNECT_REQ){
    UwbProvWifiStatusV1 s{};
    if(!(h.flags&UWB_PROV_FLAG_ENCRYPTED)||h.payload_bytes!=1||payload[0]!=0xC3){uwbProvFillStatus(s,false,UWB_PROV_AUTH_FAIL);uwbProvSendStatus(h.request,s);return;}
    if(uwbProvReconLastValid&&uwbProvReconLastReq==h.request){uwbProvSendStatus(h.request,uwbProvReconLastStatus);return;}
    if(!remoteNetValid){uwbProvFillStatus(s,false,UWB_PROV_CONFIG_INVALID);uwbProvSendStatus(h.request,s);return;}
    event("{\"event\":\"R1I_UWB_WIFI_RECONNECT_ACCEPTED\",\"request\":%lu,\"state\":\"%s\"}",(unsigned long)h.request,remoteStateName(remoteState));
    if(r9OnUwbRecon()){   // R1J9: 3rd RECON without any served TCP request -> reboot (idle only)
      uwbProvFillStatus(s,true,UWB_PROV_OK);s.wifi_status=R9_WIFI_STATUS_REBOOTING;
      uwbProvReconLastReq=h.request;uwbProvReconLastStatus=s;uwbProvReconLastValid=true;
      uwbProvSendStatus(h.request,s);return;
    }
    // R1I: ACK the UWB request while the Anchor is still listening on UWB.  R1H waited for
    // Wi-Fi association first; once Wi-Fi became ready the recovery listener stopped, so the
    // success status itself could be missed and the Tag reported TIMEOUT even though Wi-Fi worked.
    uwbProvFillStatus(s,true,UWB_PROV_OK);
    uwbProvReconLastReq=h.request;uwbProvReconLastStatus=s;uwbProvReconLastValid=true;
    uwbProvSendStatus(h.request,s);
    // Preserve remoteState / LittleFS route, then restart STA asynchronously on the next loop.
    wifiDiagPinned=false;wifiRecoveryAttempts=0;wifiReconnectDueMs=0;wifiReconnectBackoffMs=5000;
    remoteWifiStop();remoteReconnectAtMs=millis()+250;return;
  }
  if(!fullAllowed){UwbProvWifiStatusV1 s{};uwbProvFillStatus(s,false,UWB_PROV_BUSY);uwbProvSendStatus(h.request,s);return;}
  if(h.type==UWB_PROV_CFG_FRAG){
    if(!(h.flags&UWB_PROV_FLAG_ENCRYPTED)){uwbProvSendFragAck(h.request,h.frag_index,UWB_PROV_AUTH_FAIL);return;}
    if(h.total_bytes!=sizeof(UwbProvWifiConfigV1)||h.frag_count>8||uint32_t(h.offset)+h.payload_bytes>sizeof(UwbProvWifiConfigV1)){uwbProvSendFragAck(h.request,h.frag_index,UWB_PROV_BAD_FRAME);return;}
    if(uwbProvAnchorReq!=h.request){uwbProvAnchorReq=h.request;uwbProvAnchorFragCount=h.frag_count;uwbProvAnchorMask=0;uwbProvAnchorBuf=UwbProvWifiConfigV1{};}
    if(h.frag_count!=uwbProvAnchorFragCount){uwbProvSendFragAck(h.request,h.frag_index,UWB_PROV_REASSEMBLY_FAIL);return;}
    memcpy(reinterpret_cast<uint8_t*>(&uwbProvAnchorBuf)+h.offset,payload,h.payload_bytes);uwbProvAnchorMask|=(1UL<<h.frag_index);uwbProvAnchorLastMs=millis();uwbProvSendFragAck(h.request,h.frag_index,UWB_PROV_OK);return;
  }
  if(h.type==UWB_PROV_CFG_COMMIT){
    if(!(h.flags&UWB_PROV_FLAG_ENCRYPTED)||h.payload_bytes!=1||payload[0]!=0xA5){UwbProvWifiStatusV1 s{};uwbProvFillStatus(s,false,UWB_PROV_AUTH_FAIL);uwbProvSendStatus(h.request,s);return;}
    if(uwbProvLastStatusValid&&uwbProvLastStatusReq==h.request){uwbProvSendStatus(h.request,uwbProvLastStatus);return;}
    uint32_t want=uwbProvAnchorFragCount?(1UL<<uwbProvAnchorFragCount)-1:0;uint8_t code=UWB_PROV_REASSEMBLY_FAIL;bool ok=(h.request==uwbProvAnchorReq&&want&&uwbProvAnchorMask==want)&&uwbProvTryCandidate(uwbProvAnchorBuf,code);
    UwbProvWifiStatusV1 s{};uwbProvFillStatus(s,ok,code);uwbProvLastStatus=s;uwbProvLastStatusReq=h.request;uwbProvLastStatusValid=true;uwbProvAnchorReq=0;uwbProvAnchorFragCount=0;uwbProvAnchorMask=0;uwbProvAnchorBuf=UwbProvWifiConfigV1{};uwbProvSendStatus(h.request,s);return;
  }
  if(h.type==UWB_PROV_CLEAR_REQ){
    UwbProvWifiStatusV1 s{};if(!(h.flags&UWB_PROV_FLAG_ENCRYPTED)||h.payload_bytes!=1||payload[0]!=0x5A){uwbProvFillStatus(s,false,UWB_PROV_AUTH_FAIL);uwbProvSendStatus(h.request,s);return;}
    bool ok=remoteEraseNet();if(ok){remoteNetValid=false;remoteNet=RemoteNetConfig{};remoteWifiStop();remoteState=RemoteState::MAINTENANCE;}uwbProvFillStatus(s,ok,ok?UWB_PROV_OK:UWB_PROV_CLEAR_FAILED);uwbProvSendStatus(h.request,s);return;
  }
}
#endif
