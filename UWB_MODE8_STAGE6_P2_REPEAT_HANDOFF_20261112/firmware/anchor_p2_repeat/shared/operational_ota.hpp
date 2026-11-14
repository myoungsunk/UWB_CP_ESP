#pragma once

// Included from operational_anchor.hpp inside namespace dual8app.
static const char* opsOtaLast="NONE";
static bool opsOtaReady(){
 const auto* running=esp_ota_get_running_partition();
 const auto* selected=esp_ota_get_boot_partition();
 const auto* next=esp_ota_get_next_update_partition(nullptr);
 esp_ota_img_states_t state=ESP_OTA_IMG_UNDEFINED;
 // A custody HMAC authenticates the operator. A device-side publisher signature is required for activation.
 constexpr bool imageSignatureVerifierProvisioned=false;
 return imageSignatureVerifierProvisioned&&opsFsOk&&!opsAnyStoredFile()&&!opsFilePending()&&opsPhase==OpsPhase::Idle&&!active&&opsRecordsDrained()&&
        hwOk&&!port.faulted()&&opsCustodyKeyReady()&&WiFi.status()==WL_CONNECTED&&
        running&&selected&&next&&selected->address==running->address&&next->address!=running->address&&
        (esp_ota_get_state_partition(running,&state)!=ESP_OK||state!=ESP_OTA_IMG_PENDING_VERIFY);
}

static void opsOtaStatus(char* out,size_t size){
 const auto* running=esp_ota_get_running_partition();
 const auto* next=esp_ota_get_next_update_partition(nullptr);
 const auto* selected=esp_ota_get_boot_partition();
 esp_ota_img_states_t state=ESP_OTA_IMG_UNDEFINED,other=ESP_OTA_IMG_UNDEFINED;
 const int currentState=running&&esp_ota_get_state_partition(running,&state)==ESP_OK?int(state):-1;
 const int nextState=next&&esp_ota_get_state_partition(next,&other)==ESP_OK?int(other):-1;
 uint8_t digest[32]{};
 char sha[65]{};
 if(running&&esp_partition_get_sha256(running,digest)==ESP_OK)
  for(unsigned i=0;i<sizeof digest;++i)snprintf(sha+2*i,3,"%02x",unsigned(digest[i]));
 snprintf(out,size,
   "{\"event\":\"OPS_OTA_STATUS\",\"boot\":%lu,\"running\":\"%s\",\"next\":\"%s\","
   "\"selected\":\"%s\",\"state\":%d,\"next_state\":%d,\"last\":\"%s\",\"partition_sha256\":\"%s\",\"ready\":%s}",
   (unsigned long)boot,running?running->label:"",next?next->label:"",selected?selected->label:"",
   currentState,nextState,opsOtaLast,sha,opsOtaReady()?"true":"false");
}

static bool opsOtaManifest(const char* hash,const char* proof,uint32_t size,uint8_t (&expected)[32]){
 if(!opsCustodyKeyReady()||!hash||!proof||strlen(hash)!=64||strlen(proof)!=64)return false;
 for(size_t i=0;i<64;++i)
  if(!((hash[i]>='0'&&hash[i]<='9')||(hash[i]>='a'&&hash[i]<='f')))return false;
 size_t decoded=0;
 uint8_t claimed[32]{},actual[32]{};
 if(!opsHex(hash,expected,sizeof expected,decoded)||decoded!=32||
    !opsHex(proof,claimed,sizeof claimed,decoded)||decoded!=32)return false;
 char message[128];
 const int length=snprintf(message,sizeof message,"MODE8-OTA:ANCHOR:%08lX:%08lX:%08lX:%s",
    (unsigned long)boot,(unsigned long)opsNonce,(unsigned long)size,hash);
 const auto* info=mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
 if(length<0||size_t(length)>=sizeof message||!info||
    mbedtls_md_hmac(info,opsNet.deleteKey,32,reinterpret_cast<const uint8_t*>(message),size_t(length),actual)!=0)
  return false;
 unsigned diff=0;
 for(size_t i=0;i<sizeof actual;++i)diff|=actual[i]^claimed[i];
 return diff==0;
}

static bool opsOtaReadExact(uint8_t* out,size_t size,uint32_t started){
 uint32_t progress=millis();
 while(size){
  const int available=opsClient.available();
  if(available>0){
   const size_t wanted=size<size_t(available)?size:size_t(available);
   const int n=opsClient.read(out,wanted);
   if(n>0){out+=n;size-=size_t(n);progress=millis();}
  }else if(!opsClient.connected())return false;
  if(uint32_t(millis()-progress)>15000U||uint32_t(millis()-started)>180000U)return false;
  delay(1);
 }
 return true;
}

static bool opsOtaMatchesRole(const uint8_t* prefix){
 // ESP32 image header, first segment header, then the fixed ESP-IDF app descriptor.
 // The anchor descriptor is the first custom-app descriptor in that segment.
 constexpr size_t descOffset=sizeof(esp_image_header_t)+sizeof(esp_image_segment_header_t);
 constexpr size_t roleOffset=descOffset+sizeof(esp_app_desc_t);
 uint32_t segmentSize=0,magic=0;
 memcpy(&segmentSize,prefix+sizeof(esp_image_header_t)+4,sizeof segmentSize);
 memcpy(&magic,prefix+descOffset,sizeof magic);
 return prefix[0]==ESP_IMAGE_HEADER_MAGIC&&magic==ESP_APP_DESC_MAGIC_WORD&&
        segmentSize>=sizeof(esp_app_desc_t)+sizeof mode8OtaRole&&
        memcmp(prefix+roleOffset,mode8OtaRole,sizeof mode8OtaRole)==0;
}

static bool opsOtaReadbackMatches(const esp_partition_t* partition,size_t bytes,const uint8_t* expected){
 uint8_t buffer[1024],digest[32]{};
 mbedtls_sha256_context sha;
 mbedtls_sha256_init(&sha);
 bool ok=mbedtls_sha256_starts(&sha,0)==0;
 for(size_t offset=0;ok&&offset<bytes;){
  const size_t count=bytes-offset<sizeof buffer?bytes-offset:sizeof buffer;
  ok=esp_partition_read(partition,offset,buffer,count)==ESP_OK&&mbedtls_sha256_update(&sha,buffer,count)==0;
  offset+=count;
  delay(1);
 }
 ok=ok&&mbedtls_sha256_finish(&sha,digest)==0;
 mbedtls_sha256_free(&sha);
 return ok&&memcmp(digest,expected,sizeof digest)==0;
}

static const char* opsOtaReceive(const esp_partition_t* next,uint32_t size,const uint8_t* expected){
 static uint8_t buffer[4096];
 constexpr char trailer[]="\nMODE8_OTA_END\n";
 const uint32_t started=millis();
 size_t count=size<sizeof buffer?size:sizeof buffer;
 if(!opsOtaReadExact(buffer,count,started))return "OTA_INCOMPLETE";
 if(!opsOtaMatchesRole(buffer))return "OTA_ROLE";
 esp_ota_handle_t handle=0;
 if(esp_ota_begin(next,OTA_WITH_SEQUENTIAL_WRITES,&handle)!=ESP_OK)return "OTA_BEGIN";
 size_t remaining=size;
 const char* error=nullptr;
 for(;;){
  if(esp_ota_write(handle,buffer,count)!=ESP_OK){error="OTA_WRITE";break;}
  remaining-=count;
  if(!remaining)break;
  count=remaining<sizeof buffer?remaining:sizeof buffer;
  if(!opsOtaReadExact(buffer,count,started)){error="OTA_INCOMPLETE";break;}
 }
 if(!error&&!opsOtaReadExact(buffer,sizeof trailer-1,started))error="OTA_INCOMPLETE";
 if(!error&&(memcmp(buffer,trailer,sizeof trailer-1)!=0||opsClient.available()>0))error="OTA_FORMAT";
 if(error){esp_ota_abort(handle);return error;}
 if(esp_ota_end(handle)!=ESP_OK)return "OTA_IMAGE";
 if(!opsOtaReadbackMatches(next,size,expected))return "OTA_SHA256";
 if(esp_ota_set_boot_partition(next)!=ESP_OK)return "OTA_ACTIVATE";
 return nullptr;
}

static void opsOtaCommand(char* save){
 const char* target=strtok_r(nullptr," \r\t",&save);
 const char* rawSize=strtok_r(nullptr," \r\t",&save);
 const char* hash=strtok_r(nullptr," \r\t",&save);
 const char* proof=strtok_r(nullptr," \r\t",&save);
 uint32_t size=0;
 constexpr size_t minimum=sizeof(esp_image_header_t)+sizeof(esp_image_segment_header_t)
                         +sizeof(esp_app_desc_t)+sizeof mode8OtaRole;
 const auto* next=esp_ota_get_next_update_partition(nullptr);
 uint8_t expected[32]{};
 if(!target||strcmp(target,"U_FLASH")){opsOtaLast="OTA_TARGET";opsTcpReply("ERROR OTA_TARGET");return;}
 if(strtok_r(nullptr," \r\t",&save)||!parseDecimalU32(rawSize,size)||size<minimum||
    !next||size>next->size){opsOtaLast="OTA_FORMAT";opsTcpReply("ERROR OTA_FORMAT");return;}
 if(!opsOtaManifest(hash,proof,size,expected)){opsOtaLast="OTA_MANIFEST";opsTcpReply("ERROR OTA_MANIFEST");return;}
 if(!opsOtaReady()){opsOtaLast="OTA_GUARD";opsTcpReply("ERROR OTA_GUARD");return;}
 opsOtaLast="OTA_STREAMING";
 if(!opsTcpReply("OTA_READY")){opsOtaLast="OTA_INCOMPLETE";opsClient.stop();return;}
 const char* error=opsOtaReceive(next,size,expected);
 if(error){
  opsOtaLast=error;
  char reply[48];snprintf(reply,sizeof reply,"ERROR %s",error);opsTcpReply(reply);
  opsClient.stop();return;
 }
 opsOtaLast="OTA_ACTIVATED";
 opsTcpReply("OTA_ACTIVATED");
 // Allow the success line to leave TCP before reboot. The new slot is authoritative.
 delay(400);
 esp_restart();
}

static void opsOtaBootAttest(){
#if defined(CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE) && CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
 const auto* running=esp_ota_get_running_partition();
 esp_ota_img_states_t state=ESP_OTA_IMG_UNDEFINED;
 if(!running||esp_ota_get_state_partition(running,&state)!=ESP_OK||state!=ESP_OTA_IMG_PENDING_VERIFY)return;
 const bool pass=hwOk&&opsFsOk&&port.selftest()&&opsPhase!=OpsPhase::Recovery;
 if(pass&&esp_ota_mark_app_valid_cancel_rollback()==ESP_OK){
  opsOtaLast="OTA_BOOT_VALID";
  return;
 }
 Serial.printf("{\"event\":\"OTA_BOOT_REJECT\",\"slot\":\"%s\",\"boot\":%lu,\"pending_verify\":true,\"injected\":false}\n",
               running->label,(unsigned long)boot);
 Serial.flush();
 delay(100);
 if(esp_ota_mark_app_invalid_rollback_and_reboot()!=ESP_OK)opsFail("OTA_ROLLBACK_FAILED");
#endif
}
