#line 1 "C:\\UWB\\UWB_R04_R1J6_FIELD_FIX_20261010\\R1M_CTRL_V2_RC1\\built\\sketches\\ANCHOR_RF_PLATFORM_V1\\route_v1.hpp"
#pragma once

// R03 route-session orchestration for the Anchor.
// RF transaction primitives (runAnchor/runTag and POLL/RESP/A1/B/A2 timing) are untouched.
// Default route keeps Wi-Fi off. R1J.3 adds an explicitly opt-in, one-route-at-a-time
// Wi-Fi KEEP experiment; control/measurement timing and LittleFS layout are unchanged.

constexpr uint32_t R03_ROUTE_MAGIC = 0x33545252UL;      // RRT3
constexpr uint32_t R03_POINT_MAGIC = 0x33545052UL;      // RPT3
constexpr uint16_t R03_ROUTE_VERSION = 3;
constexpr uint32_t R03_POINT_MAX_RECORDS_HARD = 64;
constexpr uint32_t R04_PAIR_MAX_LOGICAL_SAMPLES = 32;
constexpr uint32_t R03_CTRL_TIMEOUT_MS = 2500;
constexpr uint32_t R03_CTRL_REPLY_GUARD_US = 2000;
static const char* R03_ROUTE_MANIFEST_FILE = "/r03_route.bin";
static const char* R03_ROUTE_MANIFEST_TMP  = "/r03_route.tmp";

#pragma pack(push,1)
struct R03RouteManifest {
  uint32_t magic=R03_ROUTE_MAGIC;
  uint16_t version=R03_ROUTE_VERSION;
  uint16_t bytes=sizeof(R03RouteManifest);
  uint32_t route_id=0;
  uint32_t config_id=0;
  uint32_t anchor_boot=0;
  uint32_t tag_boot=0;
  uint32_t max_point_records=0;
  uint32_t next_txn=0;
  uint32_t committed_points=0;
  uint32_t total_records=0;
  uint32_t last_point=0;
  uint32_t last_attempt=0;
  uint32_t last_count=0;
  uint32_t last_records_crc32=0;
  uint8_t finalized=0;
  uint8_t reserved[3]{};
  uint32_t crc32=0;
};

struct R03PointFileHeader {
  uint32_t magic=R03_POINT_MAGIC;
  uint16_t version=R03_ROUTE_VERSION;
  uint16_t bytes=sizeof(R03PointFileHeader);
  uint32_t route_id=0;
  uint32_t point_id=0;
  uint32_t attempt_id=0;
  uint32_t first_txn=0;
  uint32_t count=0;
  uint32_t config_id=0;
  uint32_t anchor_boot=0;
  uint32_t tag_boot=0;
  uint32_t record_bytes=REMOTE_COMPACT_BYTES;
  uint32_t records_crc32=0;
  uint32_t wifi_measure_violation_count=0;
  RemoteRxCompact point_start_rx{};
  uint32_t header_crc32=0;
};
#pragma pack(pop)

static_assert(REMOTE_COMPACT_BYTES==479,"R03 expects R02 compact schema v2 / 479 bytes");

static R03RouteManifest r03Manifest{};
static bool r03ManifestValid=false;
static bool r03Prepared=false;
static bool r03SessionActive=false;
static bool r03WaitPending=false;
static bool r03WaitingPoint=false;
static bool r03PointMeasuring=false;
static uint32_t r03WaitDeadlineMs=0;
static uint32_t r03PointId=0,r03AttemptId=0,r03PointCount=0,r03PointFirstTxn=0;
static uint32_t r03LastCommitCrc=0;
// R1M: a POINT_ACCEPT TX is NOT proof that the Tag received it. Never start
// RF measurement before a matching POINT_READY comes back over the air.
static bool r1mWaitingReady=false;
static uint32_t r1mReadySinceMs=0;
static constexpr uint32_t R1M_READY_DEADLINE_MS=9000;
static constexpr uint32_t R1M_START_AFTER_READY_US=350000;
// Bounded post-finalize receiver window: replay the same END_ACK on duplicate END.
static uint32_t r1mEndReplaySinceMs=0;
static constexpr uint32_t R1M_END_REPLAY_MS=15000;
static uint32_t r1mPointReadyAccepted=0,r1mPointStartDuplicates=0;
static uint32_t r1mReadyTimeouts=0,r1mEndAckReplays=0;

// R1G1: idle auto-finalize is configurable (ROUTEIDLE <s>, 0=off, 30..3600). R1G fixed 120 s could
// finalize a manual route while the operator was repositioning the robot.
static uint32_t gR1GRouteIdleTimeoutMs=300000;
static uint32_t r1gRouteLastActivityMs=0;
// R1J.3 experiment: RAM-only (reboot => OFF), settable only with no prepared/active
// route. Persist the selected policy in the existing manifest reserved[0] for audit.
static bool gR03WifiKeepDuringRoute=false;
static const char* r03WifiPolicyName(){return gR03WifiKeepDuringRoute?"KEEP":"OFF";}

static uint32_t r03ManifestCrc(const R03RouteManifest& in){R03RouteManifest x=in;x.crc32=0;return crc32(&x,sizeof(x));}
static uint32_t r03PointHeaderCrc(const R03PointFileHeader& in){R03PointFileHeader x=in;x.header_crc32=0;return crc32(&x,sizeof(x));}

static void r03PointPath(char* out,size_t cap,uint32_t routeId,uint32_t pointId,uint32_t attemptId,bool tmp=false){
  snprintf(out,cap,"/r03_%lu_%lu_%lu.%s",(unsigned long)routeId,(unsigned long)pointId,(unsigned long)attemptId,tmp?"tmp":"bin");
}

static bool r03SaveManifest(){
  if(!remoteFsReady)return false;
  r03Manifest.magic=R03_ROUTE_MAGIC;r03Manifest.version=R03_ROUTE_VERSION;r03Manifest.bytes=sizeof(r03Manifest);r03Manifest.crc32=r03ManifestCrc(r03Manifest);
  File f=LittleFS.open(R03_ROUTE_MANIFEST_TMP,"w");if(!f)return false;
  bool ok=f.write((const uint8_t*)&r03Manifest,sizeof(r03Manifest))==sizeof(r03Manifest);f.flush();f.close();
  if(!ok){LittleFS.remove(R03_ROUTE_MANIFEST_TMP);return false;}
  File v=LittleFS.open(R03_ROUTE_MANIFEST_TMP,"r");R03RouteManifest chk{};ok=v&&v.read((uint8_t*)&chk,sizeof(chk))==sizeof(chk);if(v)v.close();
  ok=ok&&chk.magic==R03_ROUTE_MAGIC&&chk.version==R03_ROUTE_VERSION&&chk.bytes==sizeof(chk)&&chk.crc32==r03ManifestCrc(chk);
  if(!ok){LittleFS.remove(R03_ROUTE_MANIFEST_TMP);return false;}
  if(LittleFS.exists(R03_ROUTE_MANIFEST_FILE))LittleFS.remove(R03_ROUTE_MANIFEST_FILE);
  ok=LittleFS.rename(R03_ROUTE_MANIFEST_TMP,R03_ROUTE_MANIFEST_FILE);r03ManifestValid=ok;return ok;
}

static bool r03LoadManifest(){
  r03ManifestValid=false;if(!remoteFsReady||!LittleFS.exists(R03_ROUTE_MANIFEST_FILE))return false;
  File f=LittleFS.open(R03_ROUTE_MANIFEST_FILE,"r");if(!f)return false;R03RouteManifest x{};
  bool ok=f.read((uint8_t*)&x,sizeof(x))==sizeof(x);f.close();
  ok=ok&&x.magic==R03_ROUTE_MAGIC&&x.version==R03_ROUTE_VERSION&&x.bytes==sizeof(x)&&x.crc32==r03ManifestCrc(x);
  if(ok){r03Manifest=x;r03ManifestValid=true;}return ok;
}

static bool r03ValidatePoint(uint32_t routeId,uint32_t pointId,uint32_t attemptId,R03PointFileHeader& h){
  if(!remoteFsReady)return false;char path[64];r03PointPath(path,sizeof(path),routeId,pointId,attemptId,false);
  File f=LittleFS.open(path,"r");if(!f)return false;
  bool ok=f.read((uint8_t*)&h,sizeof(h))==sizeof(h);
  ok=ok&&h.magic==R03_POINT_MAGIC&&h.version==R03_ROUTE_VERSION&&h.bytes==sizeof(h)&&h.route_id==routeId&&h.point_id==pointId&&h.attempt_id==attemptId&&h.record_bytes==REMOTE_COMPACT_BYTES&&h.header_crc32==r03PointHeaderCrc(h);
  if(ok)ok=f.size()==size_t(sizeof(h))+size_t(h.count)*REMOTE_COMPACT_BYTES;
  uint32_t crc=0xffffffffUL;RemoteCompactRecord rec{};
  for(uint32_t i=0;i<h.count&&ok;i++){if(f.read(rec.data,sizeof(rec))!=sizeof(rec)){ok=false;break;}crc=crcUpdate(crc,&rec,sizeof(rec));}
  f.close();return ok&&((crc^0xffffffffUL)==h.records_crc32);
}

static bool r03SendControl(uint8_t type,uint32_t pointId,uint64_t ts0=0,uint64_t ts1=0,uint64_t ts2=0){
  if(!IS_ANCHOR||!sessionId||!peerBoot){ctrlNote("CTRL_TX_PRECONDITION",nullptr,nullptr,pointId);return false;}
  // Control frames are outside the measurement schedule; force the validated P0 level.
  R7PowerAudit audit;if(!r7ApplyAtten(R7_POLL_ATTEN_X2,&audit)){ctrlNote("CTRL_TX_POWER_FAILED",nullptr,nullptr,pointId);snprintf(remoteLastError,sizeof(remoteLastError),"R03_CTRL_POWER_FAIL");return false;}
  AirPacket p{};p.h.type=type;p.h.session=sessionId;p.h.txn=pointId;p.h.config_id=configId(cfg);p.h.boot=bootId;p.ts[0]=ts0;p.ts[1]=ts1;p.ts[2]=ts2;
  byte data[54]{};uint16_t n=uint16_t(encodeAir(p,data));idleRadio();
  // R1E snapshot: Tag pre-arms route-A RX after control TX; allow a small
  // turnaround guard before Anchor sends POINT_ACCEPT / COMMIT / END_ACK.
  delayMicroseconds(R03_CTRL_REPLY_GUARD_US);
  DW1000Ng::setTransmitData(data,n);uint64_t t0=nowUs();ctrlNote("CTRL_TX_START",&p);DW1000Ng::startTransmit(TransmitMode::IMMEDIATE);
  while(!DW1000Ng::isTransmitDone()){
    if(nowUs()-t0>uint64_t(cfg.rx_grace_us)+8000ULL){ctrlNote("CTRL_TX_TIMEOUT",&p);++txTimeouts;idleRadio();event("{\"event\":\"R03_CTRL_TX\",\"role\":\"anchor\",\"type\":\"%s\",\"point\":%lu,\"ok\":false}",typeName(type),(unsigned long)pointId);return false;}
    delayMicroseconds(10);
  }
  uint64_t hw=DW1000Ng::getTransmitTimestamp()&MASK40;DW1000Ng::clearTransmitStatus();idleRadio();ctrlNote("CTRL_TX_DONE",&p);
  event("{\"event\":\"R03_CTRL_TX\",\"role\":\"anchor\",\"type\":\"%s\",\"route\":%lu,\"point\":%lu,\"tx_hw\":%llu,\"bytes\":%u,\"ok\":true}",typeName(type),(unsigned long)sessionId,(unsigned long)pointId,(unsigned long long)hw,unsigned(n));return true;
}

// R1L: ts[1] low 32 = records_crc32 (store=1: point file CRC as R1J9; store=0: CRC32 of the probe blocks sent),
// bits 32..47 = probe blocks sent in this point, bit 63 = no-store. Old Tag firmware reads only the low 32 bits.
static bool r03SendCommit(const R03PointFileHeader& h,uint32_t blocks=0,bool nostore=false){
  uint64_t a=(uint64_t(h.attempt_id)<<32)|uint64_t(h.count);
  uint64_t c=uint64_t(h.records_crc32)|(uint64_t(blocks&0xFFFFU)<<32)|(nostore?(1ULL<<63):0ULL);
  return r03SendControl(POINT_COMMIT,h.point_id,a,c,uint64_t(h.first_txn));
}
// R1L store=0: the last committed point lives in RAM only (duplicate POINT_START / POINT_QUERY replay).
static R03PointFileHeader gR1LLastH{};static bool gR1LLastValid=false;static uint32_t gR1LLastBlocks=0;
static bool r1lLastMatches(uint32_t route,uint32_t point,uint32_t attempt){return gR1LLastValid&&gR1LLastH.route_id==route&&gR1LLastH.point_id==point&&gR1LLastH.attempt_id==attempt;}

static bool r03PersistCurrentPoint(R03PointFileHeader& h){
  if(!remoteFsReady||!remoteRecords||remoteRecordCount!=r03PointCount||remoteRoundtripCount!=r03PointCount)return false;
  h=R03PointFileHeader{};h.route_id=r03Manifest.route_id;h.point_id=r03PointId;h.attempt_id=r03AttemptId;h.first_txn=r03PointFirstTxn;h.count=r03PointCount;h.config_id=configId(remoteRunCfg);h.anchor_boot=bootId;h.tag_boot=peerBoot;h.wifi_measure_violation_count=remoteWifiMeasureViolations;h.point_start_rx=remoteSessionStartRx;
  uint32_t crc=0xffffffffUL;for(uint32_t i=0;i<remoteRecordCount;i++)crc=crcUpdate(crc,&remoteRecordAt(i),sizeof(RemoteCompactRecord));h.records_crc32=crc^0xffffffffUL;h.header_crc32=r03PointHeaderCrc(h);
  char tmp[64],path[64];r03PointPath(tmp,sizeof(tmp),h.route_id,h.point_id,h.attempt_id,true);r03PointPath(path,sizeof(path),h.route_id,h.point_id,h.attempt_id,false);LittleFS.remove(tmp);
  File f=LittleFS.open(tmp,"w");if(!f)return false;bool ok=f.write((const uint8_t*)&h,sizeof(h))==sizeof(h);
  for(uint32_t i=0;i<remoteRecordCount&&ok;i++)ok=f.write(remoteRecordAt(i).data,sizeof(RemoteCompactRecord))==sizeof(RemoteCompactRecord);
  f.flush();f.close();if(!ok){LittleFS.remove(tmp);return false;}
  // Verify the temporary file before making it the committed filename.
  File v=LittleFS.open(tmp,"r");R03PointFileHeader vh{};ok=v&&v.read((uint8_t*)&vh,sizeof(vh))==sizeof(vh);uint32_t vc=0xffffffffUL;RemoteCompactRecord rec{};
  if(ok)ok=vh.header_crc32==r03PointHeaderCrc(vh)&&vh.count==h.count&&v.size()==size_t(sizeof(vh))+size_t(vh.count)*REMOTE_COMPACT_BYTES;
  for(uint32_t i=0;i<vh.count&&ok;i++){if(v.read(rec.data,sizeof(rec))!=sizeof(rec)){ok=false;break;}vc=crcUpdate(vc,&rec,sizeof(rec));}
  if(v)v.close();ok=ok&&((vc^0xffffffffUL)==vh.records_crc32)&&vh.records_crc32==h.records_crc32;
  if(!ok){LittleFS.remove(tmp);return false;}if(LittleFS.exists(path))LittleFS.remove(path);if(!LittleFS.rename(tmp,path))return false;
  R03PointFileHeader finalH{};return r03ValidatePoint(h.route_id,h.point_id,h.attempt_id,finalH)&&finalH.records_crc32==h.records_crc32;
}

static void r03ArmForControl(){ctrlCheckpointArming=ctrlRecording&&!ctrlFault&&ctrlCheckN==0;if(ctrlCheckpointArming&&!ctrlStageCheckpoint("BEFORE_CONTROL_IDLE")){ctrlCheckpointArming=false;return;}idleRadio();if(ctrlCheckpointArming&&!ctrlStageCheckpoint("AFTER_CONTROL_IDLE")){ctrlCheckpointArming=false;return;}armRx();ctrlCheckpointArming=false;if(ctrlFault)return;r03WaitingPoint=true;remoteState=RemoteState::ROUTE_WAIT_POINT;}

// ---------------------------------------------------------------------------------------------
// R1H LittleFS management.
// R1G2 ROUTECLEAR/ROUTEPREP never freed space: File::name() in arduino-esp32 3.x returns the base
// name ("r03_1_1_1.bin") and LittleFS.remove() needs "/r03_1_1_1.bin", the remove() result was
// ignored, and the directory iterator was advanced while files were being removed. Old route point
// files therefore accumulated until the 1.44 MB partition had 12 KB free (J3: R03_POINT_PERSIST_FAILED).
// Now: collect absolute paths with the directory closed, remove, verify, report counts.
// ---------------------------------------------------------------------------------------------
static constexpr size_t R03_FS_RESERVE_BYTES=16384;
static constexpr uint32_t R03_FS_BATCH=48;
static char gR03FsPaths[R03_FS_BATCH][48];
static String r03AbsPath(File& f){
  String n=String(f.path());if(n.length()==0)n=String(f.name());if(!n.startsWith("/"))n=String("/")+n;return n;
}
// "/r03_<route>_<point>_<attempt>.bin|tmp" -> true and fields; manifest/tmp names -> false
static bool r03ParsePointPath(const String& n,uint32_t& route,uint32_t& point,uint32_t& attempt,bool& tmp){
  unsigned long r=0,pt=0,a=0;char ext[4]={0};
  if(sscanf(n.c_str(),"/r03_%lu_%lu_%lu.%3s",&r,&pt,&a,ext)!=4)return false;
  route=uint32_t(r);point=uint32_t(pt);attempt=uint32_t(a);tmp=(strcmp(ext,"tmp")==0);return true;
}
struct R03FsStats{size_t total=0,used=0,free_bytes=0,r03_bytes=0,other_bytes=0;uint32_t files=0,r03_files=0,other_files=0,routes=0;uint32_t route_ids[8]={0};bool root_ok=false;};
static R03FsStats r03FsStats(){
  R03FsStats s{};if(!remoteFsReady)return s;
  s.total=LittleFS.totalBytes();s.used=LittleFS.usedBytes();s.free_bytes=s.total>s.used?s.total-s.used:0;
  File root=LittleFS.open("/");if(!root)return s;s.root_ok=true;
  File f=root.openNextFile();
  while(f){
    String n=r03AbsPath(f);size_t z=f.size();++s.files;uint32_t r,pt,a;bool tmp;
    if(n.startsWith("/r03_")){++s.r03_files;s.r03_bytes+=z;
      if(r03ParsePointPath(n,r,pt,a,tmp)){bool seen=false;for(uint32_t i=0;i<s.routes&&i<8;i++)if(s.route_ids[i]==r)seen=true;if(!seen){if(s.routes<8)s.route_ids[s.routes]=r;++s.routes;}}}
    else{++s.other_files;s.other_bytes+=z;}
    f.close();f=root.openNextFile();
  }
  root.close();return s;
}
static uint32_t r03CapacityRecords(size_t freeBytes){return uint32_t((freeBytes>R03_FS_RESERVE_BYTES?freeBytes-R03_FS_RESERVE_BYTES:0)/REMOTE_COMPACT_BYTES);}
// routeFilter: 0 = every r03 point file; otherwise only that route. keepRoute: never delete this route.
// withManifest: also delete /r03_route.bin and /r03_route.tmp.
static bool r03DeleteFiles(uint32_t routeFilter,uint32_t keepRoute,bool withManifest,uint32_t& removed,uint32_t& failed){
  removed=0;failed=0;if(!remoteFsReady)return false;
  for(uint32_t pass=0;pass<64;pass++){
    uint32_t n=0;File root=LittleFS.open("/");if(!root)return false;
    File f=root.openNextFile();
    while(f&&n<R03_FS_BATCH){
      String p=r03AbsPath(f);f.close();uint32_t r,pt,a;bool tmp;bool take=false;
      if(r03ParsePointPath(p,r,pt,a,tmp))take=(routeFilter==0||r==routeFilter)&&(keepRoute==0||r!=keepRoute);
      else if(withManifest&&(p==String(R03_ROUTE_MANIFEST_FILE)||p==String(R03_ROUTE_MANIFEST_TMP)))take=true;
      if(take&&p.length()<sizeof(gR03FsPaths[0])){strcpy(gR03FsPaths[n++],p.c_str());}
      f=root.openNextFile();
    }
    if(f)f.close();root.close();
    if(n==0)break;
    uint32_t progress=0;
    for(uint32_t i=0;i<n;i++){if(LittleFS.remove(gR03FsPaths[i])){++removed;++progress;}else ++failed;}
    if(progress==0)break;   // nothing removable left in this selection
  }
  return failed==0;
}
static void r03ResetRouteState(){
  r03Manifest=R03RouteManifest{};r03ManifestValid=false;r03Prepared=false;r03SessionActive=false;r03WaitPending=false;
  r03WaitingPoint=false;r03PointMeasuring=false;r1mWaitingReady=false;r1mEndReplaySinceMs=0;r03WaitDeadlineMs=0;r03PointId=0;r03AttemptId=0;r03PointCount=0;r03PointFirstTxn=0;
  r1gRouteLastActivityMs=0;
}
static bool r03ClearFiles(uint32_t* removedOut=nullptr,uint32_t* failedOut=nullptr){
  uint32_t removed=0,failed=0;bool ok=r03DeleteFiles(0,0,true,removed,failed);
  R03FsStats st=r03FsStats();ok=ok&&st.root_ok&&st.r03_files==0;
  r03ResetRouteState();
  if(ok){remoteState=RemoteState::MAINTENANCE;snprintf(remoteLastError,sizeof(remoteLastError),"NONE");}
  else{remoteState=RemoteState::FAULT;snprintf(remoteLastError,sizeof(remoteLastError),"R03_CLEAR_INCOMPLETE");}
  if(removedOut)*removedOut=removed;if(failedOut)*failedOut=failed;return ok;
}

static void r03EmitInfo(uint32_t req,const char* op){
  if(!r03ManifestValid)r03LoadManifest();
  event("{\"event\":\"ACK\",\"request\":%lu,\"op\":\"%s\",\"ok\":true,\"role\":\"anchor\",\"r03\":true,\"prepared\":%s,\"session_active\":%s,\"waiting_point\":%s,\"point_measuring\":%s,\"manifest_valid\":%s,\"route_id\":%lu,\"max_point_records\":%lu,\"next_txn\":%lu,\"committed_points\":%lu,\"total_records\":%lu,\"last_point\":%lu,\"last_attempt\":%lu,\"last_count\":%lu,\"last_crc32\":%lu,\"finalized\":%s,\"wifi_policy\":\"%s\",\"anchor_store\":%u,\"manifest_anchor_store\":%u,\"probe_block\":%u,\"heap\":%lu}",
    (unsigned long)req,op,r03Prepared?"true":"false",r03SessionActive?"true":"false",r03WaitingPoint?"true":"false",r03PointMeasuring?"true":"false",r03ManifestValid?"true":"false",(unsigned long)r03Manifest.route_id,(unsigned long)r03Manifest.max_point_records,(unsigned long)r03Manifest.next_txn,(unsigned long)r03Manifest.committed_points,(unsigned long)r03Manifest.total_records,(unsigned long)r03Manifest.last_point,(unsigned long)r03Manifest.last_attempt,(unsigned long)r03Manifest.last_count,(unsigned long)r03Manifest.last_records_crc32,r03Manifest.finalized?"true":"false",(r03ManifestValid&&r03Manifest.reserved[0]==1)?"KEEP":"OFF",gR1LStore?1U:0U,(r03ManifestValid&&r03Manifest.reserved[1]==1)?0U:1U,gR1LBlockEnable?1U:0U,(unsigned long)ESP.getFreeHeap());
}

// R1G2: GETPOINT buffers are static (single caller: loopTask command path) to keep the
// loopTask frame small; r1gTrace() stage codes: 7 VALIDATE, 8 FILE_CRC, 9 FIRST_WRITE, 1 STREAM.
static uint8_t gGpRaw[256];static char gGpMsg[760];static uint8_t gGpCrcBuf[256];
static uint32_t r1gFileCrc32(File& f){
  const size_t pos=f.position();f.seek(0,SeekSet);uint8_t* b=gGpCrcBuf;uint32_t c=0xffffffffUL;
  while(f.available()){size_t n=f.read(b,sizeof(gGpCrcBuf));if(!n)break;c=crcUpdate(c,b,n);}f.seek(pos,SeekSet);return c^0xffffffffUL;
}
static bool r03DumpPointTcp(uint32_t req,uint32_t routeId,uint32_t pointId,uint32_t attemptId,uint32_t startOffset=0,uint32_t maxBytes=0){
  // R1J6: USB serial GETPOINT is independent of campus Wi-Fi/TCP.
  // Apply the TCP-only readiness guard ONLY to TCP-originated commands.
  // USB replies use the bounded OutMsg writer queue via remoteWriteReplyOnly.
  if(gR04CommandFromTcp&&(!remoteWifiReady||!remoteClient||!remoteClient.connected())){
    remoteDirectAck(req,false,"GETPOINT","TCP_CLIENT_REQUIRED");return true;
  }
  r1gStackSample();
  if(r1gStackFreeNow()<1536){remoteDirectAck(req,false,"GETPOINT","LOOP_STACK_LOW");return true;}
  r1gTrace(7,routeId,pointId,startOffset);
  R03PointFileHeader h{};if(!r03ValidatePoint(routeId,pointId,attemptId,h)){remoteDirectAck(req,false,"GETPOINT","POINT_NOT_FOUND_OR_CRC_FAILED");return true;}
  char path[64];r03PointPath(path,sizeof(path),routeId,pointId,attemptId,false);File f=LittleFS.open(path,"r");if(!f){remoteDirectAck(req,false,"GETPOINT","POINT_OPEN_FAILED");return true;}
  const uint32_t total=f.size();if(startOffset>total){f.close();remoteDirectAck(req,false,"GETPOINT","OFFSET_BEYOND_EOF");return true;}
  r1gTrace(8,routeId,pointId,startOffset);const uint32_t fileCrc=r1gFileCrc32(f);if(!f.seek(startOffset,SeekSet)){f.close();remoteDirectAck(req,false,"GETPOINT","SEEK_FAILED");return true;}
  r1gTrace(9,routeId,pointId,startOffset);
  uint8_t* raw=gGpRaw;char* msg=gGpMsg;const size_t rawCap=sizeof(gGpRaw),msgCap=sizeof(gGpMsg);uint32_t off=startOffset,tc=0xffffffffUL;
  // R1I: GETPOINT frames are the only TCP producer during this short transfer window.
  // Heartbeat remains available outside the window; the host still uses resumable offsets/CRC.
  remoteGetpointStreaming=true;
  while(f.available()&&(maxBytes==0||off-startOffset<maxBytes)){
    size_t want=rawCap;if(maxBytes&&maxBytes-(off-startOffset)<want)want=maxBytes-(off-startOffset);
    size_t n=f.read(raw,want);if(!n){remoteGetpointStreaming=false;f.close();remoteDirectAck(req,false,"GETPOINT","POINT_READ_FAILED");return true;}
    int k=snprintf(msg,msgCap,"{\"event\":\"R03_POINT_CHUNK\",\"route\":%lu,\"point\":%lu,\"attempt\":%lu,\"offset\":%lu,\"hex\":\"",(unsigned long)routeId,(unsigned long)pointId,(unsigned long)attemptId,(unsigned long)off);
    static const char hx[]="0123456789abcdef";for(size_t i=0;i<n;i++){msg[k++]=hx[raw[i]>>4];msg[k++]=hx[raw[i]&15];}msg[k++]='"';msg[k++]='}';msg[k]=0;
    if(!remoteWriteReplyOnly(1,msg,k,nullptr,0)){remoteGetpointStreaming=false;f.close();event("{\"event\":\"GETPOINT_SEND_FAILED\",\"route\":%lu,\"point\":%lu,\"offset\":%lu}",(unsigned long)routeId,(unsigned long)pointId,(unsigned long)off);return true;}
    tc=crcUpdate(tc,raw,n);off+=n;r1gTrace(1,routeId,pointId,off);r1gStackSample();delay(1);
  }
  f.close();r1gTrace(off>=total?2:1,routeId,pointId,off);
  const uint32_t sentBytes=off-startOffset;
  int k=snprintf(msg,msgCap,"{\"event\":\"ACK\",\"request\":%lu,\"op\":\"GETPOINT\",\"ok\":true,\"route\":%lu,\"point\":%lu,\"attempt\":%lu,\"offset_start\":%lu,\"bytes\":%lu,\"total_bytes\":%lu,\"chunk_crc32\":%lu,\"file_crc32\":%lu,\"records_crc32\":%lu,\"next_offset\":%lu,\"eof\":%s}",(unsigned long)req,(unsigned long)routeId,(unsigned long)pointId,(unsigned long)attemptId,(unsigned long)startOffset,(unsigned long)sentBytes,(unsigned long)total,(unsigned long)(tc^0xffffffffUL),(unsigned long)fileCrc,(unsigned long)h.records_crc32,(unsigned long)off,off>=total?"true":"false");
  if(!remoteWriteReplyOnly(1,msg,k,nullptr,0))event("{\"event\":\"GETPOINT_ACK_SEND_FAILED\",\"route\":%lu,\"point\":%lu,\"next_offset\":%lu}",(unsigned long)routeId,(unsigned long)pointId,(unsigned long)off);remoteGetpointStreaming=false;return true;
}

// R1J8 GETSUMMARY v1: READ ONLY, streamed from CRC-verified 479-B LittleFS records.
// Summary projection matches r1j7_summary_offline_trial.py byte-for-byte.
// The 64-B envelope binds each 57-B record to route/point/config/boot and source CRCs.
constexpr uint32_t R03_SUMMARY_MAGIC = 0x31534d52UL; // 'RMS1' little endian
constexpr uint16_t R03_SUMMARY_VERSION = 1;
constexpr size_t R03_SUMMARY_RECORD_BYTES = 57;
#pragma pack(push,1)
struct R03SummaryHeader {
  uint32_t magic = R03_SUMMARY_MAGIC;
  uint16_t version = R03_SUMMARY_VERSION;
  uint16_t bytes = sizeof(R03SummaryHeader);
  uint32_t route_id = 0, point_id = 0, attempt_id = 0, first_txn = 0;
  uint32_t count = 0, config_id = 0, anchor_boot = 0, tag_boot = 0;
  uint32_t source_record_bytes = REMOTE_COMPACT_BYTES, summary_record_bytes = R03_SUMMARY_RECORD_BYTES;
  uint32_t source_records_crc32 = 0, summary_records_crc32 = 0;
  uint32_t source_header_crc32 = 0, header_crc32 = 0;
};
#pragma pack(pop)
static_assert(sizeof(R03SummaryHeader)==64,"R1J8 summary header must be exactly 64 bytes");
static_assert(REMOTE_COMPACT_BYTES==479,"R1J8 projection requires compact v2");
// Offsets are from the production RemoteCodec layout, not the C++ in-memory object.
static const uint16_t r03SummarySpans[7][2] = {
  {0,12}, {365,1}, {426,1}, {406,20}, {327,4}, {298,5}, {339,14}
};
static RemoteCompactRecord gSmSourceRecord{};
static uint8_t gSmProjected[R03_SUMMARY_RECORD_BYTES]{};
static R03SummaryHeader gSmHeader{};

static void r03SummaryProject(const RemoteCompactRecord& source, uint8_t* out){
  size_t at=0;
  for(unsigned i=0;i<7;i++){
    const uint16_t offset=r03SummarySpans[i][0], length=r03SummarySpans[i][1];
    memcpy(out+at,source.data+offset,length);at+=length;
  }
}
static uint32_t r03SummaryHeaderCrc(const R03SummaryHeader& source){
  R03SummaryHeader copy=source;copy.header_crc32=0;
  return crc32(&copy,sizeof(copy));
}
static bool r03SummaryPrepare(File& f,const R03PointFileHeader& src,R03SummaryHeader& dst,uint32_t& fileCrc){
  if(src.count>R03_POINT_MAX_RECORDS_HARD)return false;
  dst=R03SummaryHeader{};
  dst.route_id=src.route_id;dst.point_id=src.point_id;dst.attempt_id=src.attempt_id;
  dst.first_txn=src.first_txn;dst.count=src.count;dst.config_id=src.config_id;
  dst.anchor_boot=src.anchor_boot;dst.tag_boot=src.tag_boot;
  dst.source_records_crc32=src.records_crc32;dst.source_header_crc32=src.header_crc32;
  uint32_t recordsCrc=0xffffffffUL;
  if(!f.seek(sizeof(R03PointFileHeader),SeekSet))return false;
  for(uint32_t i=0;i<src.count;i++){
    if(f.read(gSmSourceRecord.data,REMOTE_COMPACT_BYTES)!=REMOTE_COMPACT_BYTES)return false;
    r03SummaryProject(gSmSourceRecord,gSmProjected);
    recordsCrc=crcUpdate(recordsCrc,gSmProjected,R03_SUMMARY_RECORD_BYTES);
  }
  dst.summary_records_crc32=recordsCrc^0xffffffffUL;
  dst.header_crc32=r03SummaryHeaderCrc(dst);
  uint32_t combined=crcUpdate(0xffffffffUL,&dst,sizeof(dst));
  if(!f.seek(sizeof(R03PointFileHeader),SeekSet))return false;
  for(uint32_t i=0;i<src.count;i++){
    if(f.read(gSmSourceRecord.data,REMOTE_COMPACT_BYTES)!=REMOTE_COMPACT_BYTES)return false;
    r03SummaryProject(gSmSourceRecord,gSmProjected);
    combined=crcUpdate(combined,gSmProjected,R03_SUMMARY_RECORD_BYTES);
  }
  fileCrc=combined^0xffffffffUL;return true;
}
// Bounded read of the *virtual* [64-B header + N*57-B summary] file.
// Arbitrary byte offsets support resumed reads even midway through a record.
static bool r03SummaryReadAt(File& f,const R03SummaryHeader& h,uint32_t at,uint8_t* out,size_t n){
  size_t written=0;
  const uint8_t* hb=(const uint8_t*)&h;
  while(written<n){
    if(at<sizeof(h)){
      const size_t take=min(n-written,sizeof(h)-size_t(at));
      memcpy(out+written,hb+at,take);at+=take;written+=take;continue;
    }
    const uint32_t rel=at-sizeof(h), idx=rel/R03_SUMMARY_RECORD_BYTES;
    if(idx>=h.count)return false;
    const size_t inRecord=rel%R03_SUMMARY_RECORD_BYTES;
    if(!f.seek(sizeof(R03PointFileHeader)+size_t(idx)*REMOTE_COMPACT_BYTES,SeekSet))return false;
    if(f.read(gSmSourceRecord.data,REMOTE_COMPACT_BYTES)!=REMOTE_COMPACT_BYTES)return false;
    r03SummaryProject(gSmSourceRecord,gSmProjected);
    const size_t take=min(n-written,R03_SUMMARY_RECORD_BYTES-inRecord);
    memcpy(out+written,gSmProjected+inRecord,take);at+=take;written+=take;
  }
  return true;
}
static bool r03DumpSummary(uint32_t req,uint32_t routeId,uint32_t pointId,uint32_t attemptId,uint32_t startOffset,uint32_t maxBytes){
  // GETSUMMARY is read only. Never run a lengthy filesystem scan while collecting RF.
  if(r03SessionActive||r03PointMeasuring||remoteCaptureEnabled){remoteDirectAck(req,false,"GETSUMMARY","RF_ACTIVE");return true;}
  if(gR04CommandFromTcp&&(!remoteWifiReady||!remoteClient||!remoteClient.connected())){
    remoteDirectAck(req,false,"GETSUMMARY","TCP_CLIENT_REQUIRED");return true;
  }
  if(r1gStackFreeNow()<1536){remoteDirectAck(req,false,"GETSUMMARY","LOOP_STACK_LOW");return true;}
  R03PointFileHeader sourceHeader{};
  if(!r03ValidatePoint(routeId,pointId,attemptId,sourceHeader)){
    remoteDirectAck(req,false,"GETSUMMARY","SOURCE_POINT_CRC_OR_ID_FAILED");return true;
  }
  // The current route must be finalized; old already-committed route files may still be fetched.
  if(r03ManifestValid&&r03Manifest.route_id==routeId&&!r03Manifest.finalized){
    remoteDirectAck(req,false,"GETSUMMARY","ROUTE_NOT_FINALIZED");return true;
  }
  char path[64];r03PointPath(path,sizeof(path),routeId,pointId,attemptId,false);
  File f=LittleFS.open(path,"r");if(!f){remoteDirectAck(req,false,"GETSUMMARY","POINT_OPEN_FAILED");return true;}
  uint32_t fileCrc=0;
  if(!r03SummaryPrepare(f,sourceHeader,gSmHeader,fileCrc)){
    f.close();remoteDirectAck(req,false,"GETSUMMARY","SUMMARY_BUILD_FAILED");return true;
  }
  const uint32_t total=sizeof(gSmHeader)+sourceHeader.count*R03_SUMMARY_RECORD_BYTES;
  if(startOffset>total){f.close();remoteDirectAck(req,false,"GETSUMMARY","OFFSET_BEYOND_EOF");return true;}
  uint32_t off=startOffset,tc=0xffffffffUL;
  remoteGetpointStreaming=true;
  uint8_t* bytes=gGpRaw;char* msg=gGpMsg;
  while(off<total&&(maxBytes==0||off-startOffset<maxBytes)){
    size_t n=min(size_t(total-off),sizeof(gGpRaw));
    if(maxBytes)n=min(n,size_t(maxBytes-(off-startOffset)));
    if(!r03SummaryReadAt(f,gSmHeader,off,bytes,n)){
      remoteGetpointStreaming=false;f.close();remoteDirectAck(req,false,"GETSUMMARY","SUMMARY_READ_FAILED");return true;
    }
    int k=snprintf(msg,sizeof(gGpMsg),"{\"event\":\"R03_SUMMARY_CHUNK\",\"route\":%lu,\"point\":%lu,\"attempt\":%lu,\"offset\":%lu,\"hex\":\"",
      (unsigned long)routeId,(unsigned long)pointId,(unsigned long)attemptId,(unsigned long)off);
    if(k<=0||size_t(k)+n*2+4>=sizeof(gGpMsg)){
      remoteGetpointStreaming=false;f.close();remoteDirectAck(req,false,"GETSUMMARY","FRAME_BUFFER_TOO_SMALL");return true;
    }
    static const char hx[]="0123456789abcdef";
    for(size_t i=0;i<n;i++){msg[k++]=hx[bytes[i]>>4];msg[k++]=hx[bytes[i]&15];}
    msg[k++]='"';msg[k++]='}';msg[k]=0;
    if(!remoteWriteReplyOnly(1,msg,k,nullptr,0)){
      remoteGetpointStreaming=false;f.close();event("{\"event\":\"GETSUMMARY_SEND_FAILED\",\"route\":%lu,\"point\":%lu,\"offset\":%lu}",
        (unsigned long)routeId,(unsigned long)pointId,(unsigned long)off);return true;
    }
    tc=crcUpdate(tc,bytes,n);off+=uint32_t(n);delay(1);
  }
  f.close();
  int k=snprintf(msg,sizeof(gGpMsg),"{\"event\":\"ACK\",\"request\":%lu,\"op\":\"GETSUMMARY\",\"ok\":true,\"version\":1,\"record_bytes\":57,\"header_bytes\":64,\"route\":%lu,\"point\":%lu,\"attempt\":%lu,\"offset_start\":%lu,\"bytes\":%lu,\"total_bytes\":%lu,\"chunk_crc32\":%lu,\"file_crc32\":%lu,\"summary_records_crc32\":%lu,\"source_records_crc32\":%lu,\"next_offset\":%lu,\"eof\":%s}",
    (unsigned long)req,(unsigned long)routeId,(unsigned long)pointId,(unsigned long)attemptId,
    (unsigned long)startOffset,(unsigned long)(off-startOffset),(unsigned long)total,
    (unsigned long)(tc^0xffffffffUL),(unsigned long)fileCrc,
    (unsigned long)gSmHeader.summary_records_crc32,(unsigned long)gSmHeader.source_records_crc32,
    (unsigned long)off,off==total?"true":"false");
  if(k<=0||size_t(k)>=sizeof(gGpMsg)||!remoteWriteReplyOnly(1,msg,k,nullptr,0))
    event("{\"event\":\"GETSUMMARY_ACK_SEND_FAILED\",\"route\":%lu,\"point\":%lu,\"offset\":%lu}",
      (unsigned long)routeId,(unsigned long)pointId,(unsigned long)off);
  remoteGetpointStreaming=false;return true;
}

static void r03BeginRouteWait(){
  r03WaitPending=false;
  if(outputPending.load()!=0||writerBusy.load()){
    active=false;r03WaitingPoint=false;remoteState=RemoteState::FAULT;
    snprintf(remoteLastError,sizeof(remoteLastError),"R03_OUTPUT_DRAIN_TIMEOUT");return;
  }
  wifiRecoveryAttempts=0;wifiDiagPinned=false;
  // OFF is byte-for-byte equivalent to the old route's Wi-Fi transition.
  // KEEP refuses to arm if association/IP isn't already ready: no silent fallback.
  if(!gR03WifiKeepDuringRoute)remoteWifiStop();
  remoteMeasureWifiOffConfirmed=(!remoteWifiReady&&WiFi.getMode()==WIFI_OFF);
  const bool wifiPolicyReady=gR03WifiKeepDuringRoute
      ? (remoteWifiReady&&WiFi.status()==WL_CONNECTED&&uint32_t(WiFi.localIP())!=0)
      : remoteMeasureWifiOffConfirmed;
  if(!wifiPolicyReady){r03SessionActive=false;r03WaitingPoint=false;active=false;remoteState=RemoteState::FAULT;snprintf(remoteLastError,sizeof(remoteLastError),gR03WifiKeepDuringRoute?"R03_WIFI_KEEP_NOT_READY":"R03_WIFI_NOT_OFF");remoteReconnectAtMs=millis()+250;return;}
  r03Manifest.tag_boot=peerBoot;if(!r03SaveManifest()){r03SessionActive=false;active=false;remoteState=RemoteState::FAULT;snprintf(remoteLastError,sizeof(remoteLastError),"R03_MANIFEST_SAVE_FAILED");remoteReconnectAtMs=millis()+250;return;}
  r03SessionActive=true;r03WaitingPoint=true;r1gRouteLastActivityMs=millis();r03PointMeasuring=false;remoteWifiMeasureViolations=0;remoteRunCfg=cfg;remoteRunSession=sessionId;remoteRunPeer=peerBoot;remoteRunAntTx=antTxReadback;remoteRunAntRx=antRxReadback;remoteRunAntOK=antReadbackOK;remoteRunAatten=gR7ProbeAAttenX2;remoteRunBatten=gR7ProbeBAttenX2;remoteRunManual=gR7PollAudit.manual_bit;remoteRunPowerOK=gR7PowerPlanValid;snprintf(remoteLastError,sizeof(remoteLastError),"NONE");r03ArmForControl();ctrlNote("ROUTE_RX_ARMED");
  event("{\"event\":\"R03_ROUTE_READY\",\"role\":\"anchor\",\"route\":%lu,\"tag_boot\":%lu,\"max_point_records\":%lu,\"wifi_off\":%s,\"wifi_policy\":\"%s\"}",(unsigned long)sessionId,(unsigned long)peerBoot,(unsigned long)r03Manifest.max_point_records,gR03WifiKeepDuringRoute?"false":"true",r03WifiPolicyName());
}

static void r03HandlePointStart(const AirPacket& p,RxObs& o){
  ctrlNote("POINT_START_DISPATCH",&p,&o);
  r1gRouteLastActivityMs=millis();
  uint32_t attempt=uint32_t(p.ts[0]>>32),logicalCount=uint32_t(p.ts[0]&0xffffffffULL),point=p.h.txn;const uint32_t count=(cfg.mode==6)?logicalCount*2U:logicalCount;
  diagnostics(o);finishRx();
  if(r1mWaitingReady){
    // Compare the original logical point identity, not the Tag's next-txn guess.
    // After an actual lost POLL, Anchor may legitimately be ahead in next_txn.
    if(point==r03PointId&&attempt==r03AttemptId&&count==r03PointCount){
      ctrlNote("POINT_START_REPLAY",&p);
      ++r1mPointStartDuplicates;
      uint64_t a=(uint64_t(attempt)<<32)|uint64_t(logicalCount);
      bool tx=r03SendControl(POINT_ACCEPT,point,a,uint64_t(r03PointFirstTxn),uint64_t(r03Manifest.committed_points));
      event("{\"event\":\"R1M_POINT_ACCEPT_REPLAY\",\"point\":%lu,\"attempt\":%lu,\"ok\":%s}",
        (unsigned long)point,(unsigned long)attempt,tx?"true":"false");
    } else {ctrlNote("POINT_START_CONFLICT",&p);event("{\"event\":\"R1M_POINT_PENDING_CONFLICT\",\"point\":%lu,\"attempt\":%lu}",
      (unsigned long)point,(unsigned long)attempt);}
    r03ArmForControl();return;
  }
  remoteSessionStartRx=RemoteRxCompact{};remoteCopyRx(o,remoteSessionStartRx);
  if(!attempt||!logicalCount||count>r03Manifest.max_point_records||count>R03_POINT_MAX_RECORDS_HARD){ctrlNote("POINT_START_COUNT_REJECT",&p);event("{\"event\":\"R03_POINT_REJECT\",\"reason\":\"COUNT_OR_ATTEMPT_INVALID\",\"point\":%lu,\"attempt\":%lu,\"count\":%lu}",(unsigned long)point,(unsigned long)attempt,(unsigned long)count);r03ArmForControl();return;}
  R03PointFileHeader existing{};if(r03ValidatePoint(r03Manifest.route_id,point,attempt,existing)){
    event("{\"event\":\"R03_POINT_DUPLICATE\",\"point\":%lu,\"attempt\":%lu,\"records\":%lu,\"crc32\":%lu}",(unsigned long)point,(unsigned long)attempt,(unsigned long)existing.count,(unsigned long)existing.records_crc32);
    uint32_t acceptedLogical=(cfg.mode==6)?(existing.count/2U):existing.count;uint64_t a=(uint64_t(existing.attempt_id)<<32)|uint64_t(acceptedLogical);r03SendControl(POINT_ACCEPT,point,a,uint64_t(existing.first_txn),uint64_t(r03Manifest.committed_points));delay(180);r03SendCommit(existing);r03ArmForControl();return;
  }
  if(r1lLastMatches(r03Manifest.route_id,point,attempt)){
    const R03PointFileHeader& e=gR1LLastH;
    event("{\"event\":\"R03_POINT_DUPLICATE\",\"point\":%lu,\"attempt\":%lu,\"records\":%lu,\"crc32\":%lu,\"anchor_store\":0}",(unsigned long)point,(unsigned long)attempt,(unsigned long)e.count,(unsigned long)e.records_crc32);
    uint32_t acceptedLogical=(cfg.mode==6)?(e.count/2U):e.count;uint64_t a=(uint64_t(e.attempt_id)<<32)|uint64_t(acceptedLogical);r03SendControl(POINT_ACCEPT,point,a,uint64_t(e.first_txn),uint64_t(r03Manifest.committed_points));delay(180);r03SendCommit(e,gR1LLastBlocks,true);r03ArmForControl();return;
  }
  if(gR1LStore){const size_t fsT=LittleFS.totalBytes(),fsU=LittleFS.usedBytes(),fsF=fsT>fsU?fsT-fsU:0;
   if(r03CapacityRecords(fsF)<count){ctrlNote("POINT_START_FS_REJECT",&p);snprintf(remoteLastError,sizeof(remoteLastError),"R03_POINT_FS_CAPACITY");
     event("{\"event\":\"R03_POINT_REJECT\",\"reason\":\"FS_CAPACITY\",\"point\":%lu,\"count\":%lu,\"fs_free_bytes\":%lu}",(unsigned long)point,(unsigned long)count,(unsigned long)fsF);
     r03ArmForControl();return;}}   // R1H: refuse before measuring instead of failing at persist time
  remoteFreeRam();remoteExpected=count;remoteRequiredBytes=size_t(count)*sizeof(RemoteCompactRecord);remoteRecordCount=0;remoteRoundtripCount=0;remoteBufferReady=false;remoteCaptureEnabled=false;
  if(!remoteAllocate()){ctrlNote("POINT_START_ALLOC_REJECT",&p);snprintf(remoteLastError,sizeof(remoteLastError),"R03_POINT_BUFFER_ALLOC_FAILED");event("{\"event\":\"R03_POINT_REJECT\",\"reason\":\"BUFFER_ALLOC_FAILED\",\"point\":%lu,\"count\":%lu}",(unsigned long)point,(unsigned long)count);r03ArmForControl();return;}
  remoteBufferReady=true;r03PointId=point;r03AttemptId=attempt;r03PointCount=count;r03PointFirstTxn=r03Manifest.next_txn;
  uint64_t a=(uint64_t(attempt)<<32)|uint64_t(logicalCount);
  if(!r03SendControl(POINT_ACCEPT,point,a,uint64_t(r03PointFirstTxn),uint64_t(r03Manifest.committed_points))){remoteFreeRam();remoteBufferReady=false;snprintf(remoteLastError,sizeof(remoteLastError),"R03_POINT_ACCEPT_TX_FAILED");r03ArmForControl();return;}
  // Do NOT arm the measurement here: accept may be lost at the Tag.
  r1mWaitingReady=true;r1mReadySinceMs=millis();remoteCaptureEnabled=false;
  remaining=0;r03WaitingPoint=true;r03PointMeasuring=false;
  active=true;remoteState=RemoteState::ROUTE_WAIT_POINT;
  snprintf(remoteLastError,sizeof(remoteLastError),"NONE");r03ArmForControl();
  event("{\"event\":\"R1M_POINT_WAIT_READY\",\"route\":%lu,\"point\":%lu,\"attempt\":%lu,\"count\":%lu,\"first_txn\":%lu}",
    (unsigned long)r03Manifest.route_id,(unsigned long)point,(unsigned long)attempt,(unsigned long)count,(unsigned long)r03PointFirstTxn);
}

static void r1mHandlePointReady(const AirPacket& p,RxObs& o){
  const uint32_t attempt=uint32_t(p.ts[0]>>32),logicalCount=uint32_t(p.ts[0]);
  const uint32_t count=(cfg.mode==6)?logicalCount*2U:logicalCount;
  const bool match=r1mWaitingReady && r03SessionActive && r03Manifest.route_id==p.h.session &&
    p.h.txn==r03PointId && attempt==r03AttemptId && count==r03PointCount &&
    uint32_t(p.ts[1])==r03PointFirstTxn && p.h.boot==peerBoot &&
    p.h.config_id==configId(cfg);
  diagnostics(o);finishRx();
  if(!match){
    event("{\"event\":\"R1M_POINT_READY_REJECT\",\"point\":%lu,\"attempt\":%lu}",
      (unsigned long)p.h.txn,(unsigned long)attempt);
    r03ArmForControl();return;
  }
  ++r1mPointReadyAccepted;r1mWaitingReady=false;
  resetCiHistory();r1lPointReset();txnId=r03PointFirstTxn;
  remaining=r03PointCount;nextStart=nowUs()+R1M_START_AFTER_READY_US;
  remoteCaptureEnabled=true;r03WaitingPoint=false;r03PointMeasuring=true;
  remoteState=RemoteState::POINT_MEASURE;active=true;r1gRouteLastActivityMs=millis();
  event("{\"event\":\"R1M_POINT_READY_ACCEPTED\",\"route\":%lu,\"point\":%lu,\"attempt\":%lu,\"count\":%lu,\"start_delay_us\":%lu}",
    (unsigned long)p.h.session,(unsigned long)p.h.txn,(unsigned long)attempt,(unsigned long)count,
    (unsigned long)R1M_START_AFTER_READY_US);
}

static void r03HandlePointQuery(const AirPacket& p){
  r1gRouteLastActivityMs=millis();
  uint32_t attempt=uint32_t(p.ts[0]&0xffffffffULL);finishRx();R03PointFileHeader h{};
  if(p.h.txn==0&&attempt==0){
    // Reserved read-only readiness probe. Never creates/commits a measurement.
    r03SendControl(POINT_COMMIT,0,0,uint64_t(r03Manifest.next_txn),1);r03ArmForControl();return;
  }
  if(r03ValidatePoint(r03Manifest.route_id,p.h.txn,attempt,h))r03SendCommit(h);
  else if(r1lLastMatches(r03Manifest.route_id,p.h.txn,attempt))r03SendCommit(gR1LLastH,gR1LLastBlocks,true);
  else event("{\"event\":\"R03_POINT_QUERY_MISS\",\"point\":%lu,\"attempt\":%lu}",(unsigned long)p.h.txn,(unsigned long)attempt);r03ArmForControl();
}

static void r03HandleRouteEnd(const AirPacket& p){
  r1gRouteLastActivityMs=millis();
  (void)p;r1gTrace(3,r03Manifest.route_id,0,0);finishRx();r1mWaitingReady=false;r03Manifest.finalized=1;bool saved=r03SaveManifest();uint64_t a=uint64_t(r03Manifest.committed_points),b=uint64_t(r03Manifest.total_records);bool tx=r03SendControl(ROUTE_END_ACK,0,a,b,0);
  // R1F: R1E re-applied registers unconditionally here, before the Wi-Fi restart that is the
  // suspected reset trigger, and without LDE/XTAL re-init. Now: check integrity and do a full
  // hard re-init only on a confirmed reset; the 1 Hz watchdog covers the later Wi-Fi window.
  bool radioRestored=dwEnsureIntegrity("route_end");
  event("{\"event\":\"R03_ROUTE_END\",\"role\":\"anchor\",\"route\":%lu,\"committed_points\":%lu,\"total_records\":%lu,\"manifest_saved\":%s,\"ack_tx\":%s,\"radio_restored\":%s}",(unsigned long)r03Manifest.route_id,(unsigned long)r03Manifest.committed_points,(unsigned long)r03Manifest.total_records,saved?"true":"false",tx?"true":"false",radioRestored?"true":"false");
  r03SessionActive=false;r03WaitingPoint=false;r03PointMeasuring=false;r03Prepared=false;active=false;remaining=0;remoteCaptureEnabled=false;remoteBufferReady=false;remoteFreeRam();remoteState=(saved&&radioRestored)?RemoteState::TRANSFER:RemoteState::FAULT;
  if(!saved)snprintf(remoteLastError,sizeof(remoteLastError),"R03_FINAL_MANIFEST_SAVE_FAILED");
  else if(!radioRestored)snprintf(remoteLastError,sizeof(remoteLastError),"R03_ROUTE_END_RADIO_REAPPLY_FAILED");
  // KEEP must not trigger an unnecessary reconnect immediately after route end.
  remoteReconnectAtMs=(gR03WifiKeepDuringRoute&&remoteWifiReady)?0:(millis()+500);
  if(saved&&radioRestored&&tx){
    // Repeated Tag ROUTE_END after a lost air ACK receives identical result.
    r1mEndReplaySinceMs=millis();selectRoute(0);armRx();
  }
}

static void r03ListenControl(){
  if(ctrlFault)return;if(ctrlRecording)++ctrlListenCount;
  if(!r03SessionActive||!r03WaitingPoint||r03PointMeasuring)return;AirPacket p{};RxObs o{};int got=pollPacket(p,o);
  if(got==1){
    // R1M: ignore late controls from an old route, mismatched Tag reboot or
    // previous RF configuration. Never let a stale ROUTE_END finalize a new run.
    if(p.h.session!=r03Manifest.route_id || p.h.boot!=r03Manifest.tag_boot ||
       p.h.config_id!=r03Manifest.config_id){
      ++unexpected;diagnostics(o);finishRx();r03ArmForControl();
      event("{\"event\":\"R1M_STALE_CONTROL_REJECT\",\"type\":\"%s\",\"session\":%lu,\"expected_session\":%lu}",
        typeName(p.h.type),(unsigned long)p.h.session,(unsigned long)r03Manifest.route_id);
      return;
    }
    if(p.h.type==POINT_START)r03HandlePointStart(p,o);
    else if(p.h.type==POINT_READY)r1mHandlePointReady(p,o);
    else if(p.h.type==POINT_QUERY)r03HandlePointQuery(p);
    else if(p.h.type==ROUTE_END)r03HandleRouteEnd(p);
    else {++unexpected;finishRx();r03ArmForControl();}
  } else if(got<0&&!ctrlFault)r03ArmForControl();
}

static void r03OnPointRunDone(){
  r1gRouteLastActivityMs=millis();
  remoteCaptureEnabled=false;r03PointMeasuring=false;r1mWaitingReady=false;R03PointFileHeader h{};bool countOK=(remoteRecordCount==r03PointCount&&remoteRoundtripCount==r03PointCount);
  const bool nostore=!gR1LStore;bool persisted=false;
  if(nostore&&countOK){
    // R1L store=0: no point file. The header is kept in RAM; records_crc32 = CRC32 of the probe blocks sent.
    h=R03PointFileHeader{};h.route_id=r03Manifest.route_id;h.point_id=r03PointId;h.attempt_id=r03AttemptId;h.first_txn=r03PointFirstTxn;h.count=r03PointCount;h.config_id=configId(remoteRunCfg);h.anchor_boot=bootId;h.tag_boot=peerBoot;h.wifi_measure_violation_count=remoteWifiMeasureViolations;h.point_start_rx=remoteSessionStartRx;
    h.records_crc32=r1lPointCrc();h.header_crc32=r03PointHeaderCrc(h);gR1LLastH=h;gR1LLastBlocks=gR1LPointBlocks;gR1LLastValid=true;persisted=true;
  } else persisted=countOK&&r03PersistCurrentPoint(h);
  if(persisted){
    r03Manifest.next_txn=r03PointFirstTxn+r03PointCount;++r03Manifest.committed_points;r03Manifest.total_records+=r03PointCount;r03Manifest.last_point=r03PointId;r03Manifest.last_attempt=r03AttemptId;r03Manifest.last_count=r03PointCount;r03Manifest.last_records_crc32=h.records_crc32;r03Manifest.finalized=0;persisted=r03SaveManifest();
  }
  remoteFreeRam();remoteBufferReady=false;remoteExpected=0;remoteRequiredBytes=0;remoteRecordCount=0;
  if(!persisted){
    // R1H: keep already committed points downloadable: finalize the manifest instead of leaving
    // an unfinalized route that `fetch` must refuse (J3: 6 points / 120 records stranded).
    LittleFS.remove(String("/r03_")+String((unsigned long)r03Manifest.route_id)+"_"+String((unsigned long)r03PointId)+"_"+String((unsigned long)r03AttemptId)+".tmp");
    r03Manifest.finalized=1;const bool fin=r03SaveManifest();
    remoteState=fin?RemoteState::TRANSFER:RemoteState::FAULT;snprintf(remoteLastError,sizeof(remoteLastError),countOK?"R03_POINT_PERSIST_FAILED":"R03_POINT_RECORD_COUNT_MISMATCH");
    event("{\"event\":\"R03_POINT_COMMIT_FAIL\",\"point\":%lu,\"attempt\":%lu,\"count_ok\":%s,\"route_finalized\":%s,\"committed_points\":%lu}",(unsigned long)r03PointId,(unsigned long)r03AttemptId,countOK?"true":"false",fin?"true":"false",(unsigned long)r03Manifest.committed_points);
    r03SessionActive=false;r03WaitingPoint=false;active=false;remaining=0;remoteReconnectAtMs=millis()+500;return;}
  r03LastCommitCrc=h.records_crc32;bool tx=r03SendCommit(h,gR1LPointBlocks,nostore);
  event("{\"event\":\"R1L_POINT_BLOCKS\",\"role\":\"anchor\",\"route\":%lu,\"point\":%lu,\"attempt\":%lu,\"records\":%lu,\"blocks_sent\":%lu,\"block_crc32\":%lu,\"anchor_store\":%u,\"block_tx_fail_total\":%lu}",(unsigned long)h.route_id,(unsigned long)h.point_id,(unsigned long)h.attempt_id,(unsigned long)h.count,(unsigned long)gR1LPointBlocks,(unsigned long)r1lPointCrc(),nostore?0U:1U,(unsigned long)gR1LBlockTxFail);event("{\"event\":\"R03_POINT_COMMITTED\",\"role\":\"anchor\",\"route\":%lu,\"point\":%lu,\"attempt\":%lu,\"records\":%lu,\"first_txn\":%lu,\"crc32\":%lu,\"commit_tx\":%s}",(unsigned long)h.route_id,(unsigned long)h.point_id,(unsigned long)h.attempt_id,(unsigned long)h.count,(unsigned long)h.first_txn,(unsigned long)h.records_crc32,tx?"true":"false");
  active=true;remaining=0;r03WaitingPoint=true;r03PointMeasuring=false;r03ArmForControl();
}

static void r1lEmitInfo(uint32_t req,const char* op){
  event("{\"event\":\"ACK\",\"request\":%lu,\"op\":\"%s\",\"ok\":true,\"role\":\"anchor\",\"r1l\":1,\"block_version\":%u,\"block_bytes\":%u,\"probe_block\":%u,\"anchor_store\":%u,\"manifest_anchor_store\":%u,\"route_active\":%s,\"point_blocks\":%lu,\"point_block_crc32\":%lu,\"blocks_total\":%lu,\"block_tx_fail_total\":%lu,\"ci_pushes\":%u,\"last_nostore_point\":%lu,\"last_nostore_attempt\":%lu,\"last_nostore_valid\":%s,\"ctrl_handshake\":2,\"point_ready_count\":%lu,\"point_start_replay_count\":%lu,\"point_ready_timeout_count\":%lu,\"end_ack_replay_count\":%lu}",
    (unsigned long)req,op,unsigned(R1L_BLOCK_VERSION),unsigned(R1L_BLOCK_BYTES),gR1LBlockEnable?1U:0U,gR1LStore?1U:0U,(r03ManifestValid&&r03Manifest.reserved[1]==1)?0U:1U,(r03SessionActive||r03Prepared)?"true":"false",
    (unsigned long)gR1LPointBlocks,(unsigned long)r1lPointCrc(),(unsigned long)gR1LBlocksTotal,(unsigned long)gR1LBlockTxFail,unsigned(gR1LPushes),
    (unsigned long)gR1LLastH.point_id,(unsigned long)gR1LLastH.attempt_id,gR1LLastValid?"true":"false",(unsigned long)r1mPointReadyAccepted,(unsigned long)r1mPointStartDuplicates,(unsigned long)r1mReadyTimeouts,(unsigned long)r1mEndAckReplays);
}
static bool r1lCommand(uint32_t req,const char* op,char*& cur){
  // R1LINFO | R1LSTORE [GET|0|1] | R1LBLOCK [GET|0|1]. RAM only: every boot starts with store=1, block=1.
  const bool store=strcmp(op,"R1LSTORE")==0,blk=strcmp(op,"R1LBLOCK")==0;
  if(strcmp(op,"R1LINFO")==0){r1lEmitInfo(req,op);return true;}
  if(!store&&!blk)return false;
  char v[8]{};if(!remoteToken(cur,v,sizeof(v)))snprintf(v,sizeof(v),"GET");
  if(strcmp(v,"GET")!=0){
    if(strcmp(v,"0")!=0&&strcmp(v,"1")!=0){ack(req,false,op,"GET_0_1");return true;}
    if(active||r03SessionActive||r03WaitPending||r03Prepared||r03PointMeasuring||remoteCaptureEnabled){ack(req,false,op,"ROUTE_ACTIVE_OR_PREPARED");return true;}
    if(store)gR1LStore=(v[0]=='1');else gR1LBlockEnable=(v[0]=='1');
  }
  r1lEmitInfo(req,op);return true;
}

static bool r03AnchorCommand(uint32_t req,const char* op,char*& cur){
  if(r1lCommand(req,op,cur))return true;
  // WIFIPOLICY GET | OFF | KEEP; KEEP is an opt-in bench experiment and is
  // deliberately not saved to Wi-Fi credentials/NVS. No policy changes mid-route.
  if(strcmp(op,"WIFIPOLICY")==0){
    char requested[12]{};if(!remoteToken(cur,requested,sizeof(requested)))snprintf(requested,sizeof(requested),"GET");
    if(strcmp(requested,"GET")!=0){
      if(strcmp(requested,"OFF")!=0&&strcmp(requested,"KEEP")!=0){ack(req,false,op,"POLICY_GET_OFF_KEEP");return true;}
      if(active||r03SessionActive||r03WaitPending||r03Prepared||r03PointMeasuring||remoteCaptureEnabled){ack(req,false,op,"ROUTE_ACTIVE_OR_PREPARED");return true;}
      if(strcmp(requested,"KEEP")==0&&(!remoteNetValid||!remoteWifiReady||WiFi.status()!=WL_CONNECTED||uint32_t(WiFi.localIP())==0)){
        ack(req,false,op,"WIFI_ASSOCIATION_REQUIRED");return true;
      }
      gR03WifiKeepDuringRoute=(strcmp(requested,"KEEP")==0);
    }
    event("{\"event\":\"ACK\",\"request\":%lu,\"op\":\"WIFIPOLICY\",\"ok\":true,\"role\":\"anchor\",\"policy\":\"%s\",\"wifi_ready\":%s,\"active\":%s,\"manifest_policy\":\"%s\"}",
      (unsigned long)req,r03WifiPolicyName(),remoteWifiReady?"true":"false",r03SessionActive?"true":"false",(r03ManifestValid&&r03Manifest.reserved[0]==1)?"KEEP":"OFF");return true;
  }
  if(strcmp(op,"ROUTEINFO")==0){r03EmitInfo(req,op);return true;}
  if(strcmp(op,"ROUTECLEAR")==0){uint32_t confirm=0;if(active||r03SessionActive||!parseU32(cur,confirm)||confirm!=1){ack(req,false,op,"STOP_FIRST_CONFIRM_ONE");return true;}
    const size_t before=r03FsStats().free_bytes;uint32_t removed=0,failed=0;bool ok=r03ClearFiles(&removed,&failed);R03FsStats st=r03FsStats();
    event("{\"event\":\"ACK\",\"request\":%lu,\"op\":\"ROUTECLEAR\",\"ok\":%s,\"role\":\"anchor\",\"reason\":\"%s\",\"removed\":%lu,\"failed\":%lu,\"r03_files_left\":%lu,\"fs_free_before\":%lu,\"fs_free_after\":%lu,\"capacity_records\":%lu}",
      (unsigned long)req,ok?"true":"false",ok?"OK":"R03_CLEAR_INCOMPLETE",(unsigned long)removed,(unsigned long)failed,(unsigned long)st.r03_files,(unsigned long)before,(unsigned long)st.free_bytes,(unsigned long)r03CapacityRecords(st.free_bytes));return true;}
  if(strcmp(op,"FSINFO")==0){
    R03FsStats st=r03FsStats();if(!r03ManifestValid)r03LoadManifest();
    char ids[8*11+1];int k=0;ids[0]=0;for(uint32_t i=0;i<st.routes&&i<8;i++)k+=snprintf(ids+k,sizeof(ids)-k,"%s%lu",i?",":"",(unsigned long)st.route_ids[i]);
    event("{\"event\":\"ACK\",\"request\":%lu,\"op\":\"FSINFO\",\"ok\":%s,\"role\":\"anchor\",\"fs_total_bytes\":%lu,\"fs_used_bytes\":%lu,\"fs_free_bytes\":%lu,\"capacity_records\":%lu,\"reserve_bytes\":%lu,\"record_bytes\":%u,\"files\":%lu,\"r03_files\":%lu,\"r03_bytes\":%lu,\"r03_routes\":%lu,\"r03_route_ids\":\"%s\",\"other_files\":%lu,\"other_bytes\":%lu,\"manifest_valid\":%s,\"manifest_route\":%lu,\"manifest_finalized\":%s,\"legacy_run_exists\":%s}",
      (unsigned long)req,st.root_ok?"true":"false",(unsigned long)st.total,(unsigned long)st.used,(unsigned long)st.free_bytes,(unsigned long)r03CapacityRecords(st.free_bytes),(unsigned long)R03_FS_RESERVE_BYTES,unsigned(REMOTE_COMPACT_BYTES),
      (unsigned long)st.files,(unsigned long)st.r03_files,(unsigned long)st.r03_bytes,(unsigned long)st.routes,ids,(unsigned long)st.other_files,(unsigned long)st.other_bytes,
      r03ManifestValid?"true":"false",(unsigned long)(r03ManifestValid?r03Manifest.route_id:0),(r03ManifestValid&&r03Manifest.finalized)?"true":"false",LittleFS.exists(REMOTE_RUN_FILE)?"true":"false");return true;
  }
  if(strcmp(op,"FSLIST")==0){
    // One R03_FILE event per file (max 256), throttled so the 32-slot output queue never drops.
    uint32_t n=0;File root=LittleFS.open("/");if(!root){ack(req,false,op,"ROOT_OPEN_FAILED");return true;}
    File f=root.openNextFile();
    while(f&&n<256){String p=r03AbsPath(f);size_t z=f.size();f.close();uint32_t r=0,pt=0,a=0;bool tmp=false;bool isPoint=r03ParsePointPath(p,r,pt,a,tmp);
      while(outputPending.load()>20)delay(2);
      event("{\"event\":\"R03_FILE\",\"request\":%lu,\"path\":\"%s\",\"bytes\":%lu,\"point_file\":%s,\"route\":%lu,\"point\":%lu,\"attempt\":%lu,\"tmp\":%s}",
        (unsigned long)req,p.c_str(),(unsigned long)z,isPoint?"true":"false",(unsigned long)r,(unsigned long)pt,(unsigned long)a,tmp?"true":"false");
      ++n;f=root.openNextFile();}
    if(f)f.close();root.close();while(outputPending.load()>20)delay(2);
    event("{\"event\":\"ACK\",\"request\":%lu,\"op\":\"FSLIST\",\"ok\":true,\"role\":\"anchor\",\"files\":%lu}",(unsigned long)req,(unsigned long)n);return true;
  }
  if(strcmp(op,"ROUTEDEL")==0){
    // ROUTEDEL <route_id> 1 : delete one route's point files (e.g. after a CRC-verified download).
    uint32_t rid=0,confirm=0;if(!parseU32(cur,rid)||rid==0||!parseU32(cur,confirm)||confirm!=1){ack(req,false,op,"ROUTE_ID_AND_CONFIRM_ONE");return true;}
    if((active||r03SessionActive||r03WaitPending||r03PointMeasuring)&&r03Manifest.route_id==rid){ack(req,false,op,"ROUTE_ACTIVE");return true;}
    if(!r03ManifestValid)r03LoadManifest();const bool isManifestRoute=r03ManifestValid&&r03Manifest.route_id==rid;
    const size_t before=r03FsStats().free_bytes;uint32_t removed=0,failed=0;bool ok=r03DeleteFiles(rid,0,false,removed,failed);
    if(ok&&isManifestRoute){LittleFS.remove(R03_ROUTE_MANIFEST_FILE);LittleFS.remove(R03_ROUTE_MANIFEST_TMP);r03ResetRouteState();if(remoteState!=RemoteState::FAULT)remoteState=RemoteState::MAINTENANCE;}
    R03FsStats st=r03FsStats();
    event("{\"event\":\"ACK\",\"request\":%lu,\"op\":\"ROUTEDEL\",\"ok\":%s,\"role\":\"anchor\",\"route\":%lu,\"removed\":%lu,\"failed\":%lu,\"manifest_removed\":%s,\"fs_free_before\":%lu,\"fs_free_after\":%lu,\"capacity_records\":%lu}",
      (unsigned long)req,ok?"true":"false",(unsigned long)rid,(unsigned long)removed,(unsigned long)failed,(ok&&isManifestRoute)?"true":"false",(unsigned long)before,(unsigned long)st.free_bytes,(unsigned long)r03CapacityRecords(st.free_bytes));return true;
  }
  if(strcmp(op,"ROUTEPREP")==0){
    uint32_t sid,maxPoint;if(active||r03SessionActive){ack(req,false,op,"STOP_FIRST");return true;}if(!remoteFsReady){ack(req,false,op,"LITTLEFS_NOT_READY");return true;}if(!parseU32(cur,sid)||sid!=sessionId||!parseU32(cur,maxPoint)||maxPoint<1||maxPoint>R03_POINT_MAX_RECORDS_HARD){ack(req,false,op,"CONFIG_THEN_ROUTE_MAX_POINT_1_64");return true;}
    if(r03ManifestValid&&r03Manifest.route_id==sid){ack(req,false,op,"ROUTE_ID_ALREADY_PRESENT_USE_NEW_ID_OR_ROUTECLEAR");return true;}
    // R1H: optional 3rd arg purge=1 deletes every older route's point files before preparing.
    uint32_t purge=0;{char* save=cur;if(!parseU32(cur,purge)){cur=save;purge=0;}}
    uint32_t purgedFiles=0,purgeFailed=0;if(purge==1)r03DeleteFiles(0,0,true,purgedFiles,purgeFailed);
    r03Manifest=R03RouteManifest{};r03Manifest.route_id=sid;r03Manifest.config_id=configId(cfg);r03Manifest.anchor_boot=bootId;r03Manifest.max_point_records=maxPoint;r03Manifest.next_txn=0;r03Manifest.finalized=0;
    r1mEndReplaySinceMs=0;idleRadio();
    r03Manifest.reserved[0]=gR03WifiKeepDuringRoute?1:0;r03Manifest.reserved[1]=gR1LStore?0:1;gR1LLastValid=false;
    bool ok=r03SaveManifest();r03Prepared=ok;r03SessionActive=false;r03WaitingPoint=false;r03PointMeasuring=false;remoteState=ok?RemoteState::ROUTE_PREPARED:RemoteState::FAULT;if(!ok)snprintf(remoteLastError,sizeof(remoteLastError),"R03_ROUTE_PREP_SAVE_FAILED");
    const size_t fsTotal=LittleFS.totalBytes(),fsUsed=LittleFS.usedBytes(),fsFree=fsTotal>fsUsed?fsTotal-fsUsed:0;const uint32_t estRecords=gR1LStore?r03CapacityRecords(fsFree):65535UL; // R1L store=0 writes no point files
    event("{\"event\":\"ACK\",\"request\":%lu,\"role\":\"anchor\",\"op\":\"ROUTEPREP\",\"ok\":%s,\"reason\":\"%s\",\"boot\":%lu,\"session\":%lu,\"config_id\":%lu,\"fs_total_bytes\":%lu,\"fs_used_bytes\":%lu,\"fs_free_bytes\":%lu,\"estimated_records_capacity\":%lu,\"record_bytes\":%u,\"route_idle_timeout_s\":%lu,\"purged_files\":%lu,\"purge_failed\":%lu}",(unsigned long)req,ok?"true":"false",ok?"OK":"MANIFEST_SAVE_FAILED",(unsigned long)bootId,(unsigned long)sessionId,(unsigned long)configId(cfg),(unsigned long)fsTotal,(unsigned long)fsUsed,(unsigned long)fsFree,(unsigned long)estRecords,unsigned(REMOTE_COMPACT_BYTES),(unsigned long)(gR1GRouteIdleTimeoutMs/1000UL),(unsigned long)purgedFiles,(unsigned long)purgeFailed);return true;
  }
  if(strcmp(op,"ROUTEABORTEMPTY")==0){
    uint32_t sid;if(!parseU32(cur,sid)||!r03ManifestValid||sid!=r03Manifest.route_id){ack(req,false,op,"ROUTE_ID_MISMATCH");return true;}
    // R1L: a store=0 route (manifest reserved[1]==1) never has point files; its data is on the Pi only.
    const bool nostoreRoute=(r03Manifest.reserved[1]==1);
    const bool hasPoints=(r03Manifest.total_records||r03Manifest.committed_points||r03Manifest.next_txn);
    if(remaining||r03PointMeasuring||remoteCaptureEnabled||(hasPoints&&(!nostoreRoute||r03SessionActive))){ack(req,false,op,"NOT_EMPTY_OR_MEASURING");return true;}
    R03FsStats fs=r03FsStats();if(!fs.root_ok){ack(req,false,op,"FS_SCAN_FAILED");return true;}
    // Check all files (not the 8-route summary list) before declaring this route empty.
    File root=LittleFS.open("/");File f=root.openNextFile();bool found=false;
    while(f){String path=r03AbsPath(f);uint32_t r,p,a;bool tmp;if(r03ParsePointPath(path,r,p,a,tmp)&&r==sid)found=true;f.close();f=root.openNextFile();}root.close();
    if(found){ack(req,false,op,"ROUTE_FILES_PRESENT");return true;}
    r03Manifest.finalized=1;if(!r03SaveManifest()){ack(req,false,op,"MANIFEST_SAVE_FAILED");return true;}
    idleRadio();active=false;remaining=0;r03SessionActive=false;r03WaitPending=false;r03WaitingPoint=false;r03Prepared=false;
    remoteState=RemoteState::TRANSFER;remoteReconnectAtMs=remoteWifiReady?0:millis()+500;
    ack(req,true,op,nostoreRoute?"NOSTORE_ROUTE_FINALIZED_NO_FLASH_DATA":"EMPTY_ROUTE_FINALIZED_NO_DATA_DELETED");return true;
  }
  if(strcmp(op,"ROUTEWAIT")==0){
    uint32_t sid;if(!active||!peerBoot){ack(req,false,op,"ARM_FIRST");return true;}if(!r03Prepared||!r03ManifestValid){ack(req,false,op,"ROUTEPREP_FIRST");return true;}if(!parseU32(cur,sid)||sid!=sessionId||sid!=r03Manifest.route_id){ack(req,false,op,"ROUTE_ID_MISMATCH");return true;}
    if(r03SessionActive){ack(req,true,op,"ALREADY_ROUTE_READY");return true;}
    r03WaitPending=true;r03WaitDeadlineMs=millis()+300;remoteState=RemoteState::ARMED;ack(req,true,op,gR03WifiKeepDuringRoute?"EXPERIMENT_KEEP_WIFI_ASSOCIATED":"WIFI_WILL_TURN_OFF_ROUTE_UWB_ONLY");return true;
  }
  if(strcmp(op,"ROUTEIDLE")==0){
    uint32_t sec;if(r03SessionActive||r03WaitPending){remoteDirectAck(req,false,op,"ROUTE_ACTIVE");return true;}
    if(!parseU32(cur,sec)||(sec!=0&&(sec<30||sec>3600))){remoteDirectAck(req,false,op,"SECONDS_0_OR_30_3600");return true;}
    gR1GRouteIdleTimeoutMs=sec*1000UL;
    event("{\"event\":\"ACK\",\"request\":%lu,\"op\":\"ROUTEIDLE\",\"ok\":true,\"role\":\"anchor\",\"route_idle_timeout_s\":%lu}",(unsigned long)req,(unsigned long)(gR1GRouteIdleTimeoutMs/1000UL));return true;
  }
  if(strcmp(op,"GETPOINT")==0){uint32_t rid,pid,aid,off=0;if(!parseU32(cur,rid)||!parseU32(cur,pid)||!parseU32(cur,aid)){remoteDirectAck(req,false,op,"ROUTE_POINT_ATTEMPT_REQUIRED");return true;}uint32_t win=0;char* save=cur;if(!parseU32(cur,off)){cur=save;off=0;}else{save=cur;if(!parseU32(cur,win)){cur=save;win=0;}}return r03DumpPointTcp(req,rid,pid,aid,off,win);}
  if(strcmp(op,"GETSUMMARY")==0){
    uint32_t rid,pid,aid,off=0,win=0;
    if(!parseU32(cur,rid)||!parseU32(cur,pid)||!parseU32(cur,aid)){
      remoteDirectAck(req,false,op,"ROUTE_POINT_ATTEMPT_REQUIRED");return true;
    }
    char* saved=cur;
    if(!parseU32(cur,off)){cur=saved;off=0;}
    else{saved=cur;if(!parseU32(cur,win)){cur=saved;win=0;}}
    return r03DumpSummary(req,rid,pid,aid,off,win);
  }
  return false;
}

static void r03AnchorLoop(){
  if(ctrlFault)return;
  if(r1mEndReplaySinceMs && !r03SessionActive && !r03Prepared){
    if(uint32_t(millis()-r1mEndReplaySinceMs)>R1M_END_REPLAY_MS){
      r1mEndReplaySinceMs=0;idleRadio();
    } else if(hwOK){
      AirPacket p{};RxObs o{};int got=pollPacket(p,o);
      if(got==1){
        const bool replay=p.h.type==ROUTE_END && p.h.txn==0 &&
          r03ManifestValid && r03Manifest.finalized &&
          p.h.session==r03Manifest.route_id && p.h.config_id==r03Manifest.config_id &&
          p.h.boot==r03Manifest.tag_boot;
        finishRx();
        if(replay){
          bool tx=r03SendControl(ROUTE_END_ACK,0,
            uint64_t(r03Manifest.committed_points),uint64_t(r03Manifest.total_records),0);
          if(tx)++r1mEndAckReplays;
          event("{\"event\":\"R1M_ROUTE_END_ACK_REPLAY\",\"route\":%lu,\"ok\":%s}",
            (unsigned long)r03Manifest.route_id,tx?"true":"false");
        }
        selectRoute(0);armRx();
      } else if(got<0){selectRoute(0);armRx();}
    }
  }
  if(r1mWaitingReady && uint32_t(millis()-r1mReadySinceMs)>R1M_READY_DEADLINE_MS){
    ++r1mReadyTimeouts;r1mWaitingReady=false;
    remoteCaptureEnabled=false;remoteBufferReady=false;remoteFreeRam();
    remaining=0;active=true;r03WaitingPoint=true;r03PointMeasuring=false;
    remoteState=RemoteState::ROUTE_WAIT_POINT;
    event("{\"event\":\"R1M_POINT_READY_TIMEOUT\",\"route\":%lu,\"point\":%lu,\"attempt\":%lu,\"no_measurement_started\":true}",
      (unsigned long)r03Manifest.route_id,(unsigned long)r03PointId,(unsigned long)r03AttemptId);
    r03ArmForControl();
  }
  if(r03WaitPending&&int32_t(millis()-r03WaitDeadlineMs)>=0&&((outputPending.load()==0&&!writerBusy.load())||millis()-r03WaitDeadlineMs>2000))r03BeginRouteWait();
  if(r03SessionActive&&r03WaitingPoint&&!r03PointMeasuring){
    if(r1gRouteLastActivityMs&&gR1GRouteIdleTimeoutMs&&millis()-r1gRouteLastActivityMs>gR1GRouteIdleTimeoutMs){
      r1gTrace(5,r03Manifest.route_id,0,0);finishRx();r03Manifest.finalized=1;bool saved=r03SaveManifest();r03SessionActive=false;r03WaitingPoint=false;r03PointMeasuring=false;r03Prepared=false;active=false;remaining=0;remoteCaptureEnabled=false;remoteBufferReady=false;remoteFreeRam();remoteState=saved?RemoteState::TRANSFER:RemoteState::FAULT;snprintf(remoteLastError,sizeof(remoteLastError),saved?"R1G_IDLE_FINALIZED":"R1G_IDLE_FINALIZE_SAVE_FAILED");remoteReconnectAtMs=(gR03WifiKeepDuringRoute&&remoteWifiReady)?0:(millis()+500);
      event("{\"event\":\"R1G_ROUTE_IDLE_FINALIZE\",\"route\":%lu,\"idle_ms\":%lu,\"saved\":%s}",(unsigned long)r03Manifest.route_id,(unsigned long)(millis()-r1gRouteLastActivityMs),saved?"true":"false");
    } else r03ListenControl();
  }
}

static void r03RecoverAfterFsMount(){r03LoadManifest();}
