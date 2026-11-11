#line 1 "C:\\UWB\\UWB_R04_R1J6_FIELD_FIX_20261010\\R1M_CTRL_V2_RC1\\built\\sketches\\ANCHOR_RF_PLATFORM_V1\\recovery_r1j9.hpp"
#pragma once
// R1J9 (Anchor only): self-recovery when the Anchor is reachable over UWB but not over Wi-Fi/TCP.
// Included INSIDE namespace bench, after remote_v3.hpp / route_v1.hpp / ota_manager.hpp / wifi_reconnect.hpp
// and BEFORE uwb_wifi_provision.hpp (its RECONNECT handler calls r9OnUwbRecon()).
// Air protocol, measurement timing, LittleFS schema, TCP framing and the Tag firmware are unchanged.
//
//  1 UWB listener in every idle state (also while Wi-Fi is "ready" but TCP is dead)      -> uwb_wifi_provision.hpp
//  2 3 x UWBWIFIRECON within 10 min with no TCP request served in between -> reboot (idle only)
//  3 Wi-Fi not ready for 10/20/40/80 min while idle -> reboot (back-off kept across reboots in RTC RAM)
//  4 TCP: a newer client waits while the held client is silent > 10 s -> drop the held client;
//    clients keep waiting but no request is served for 120 s -> reboot (idle only)
//  5 ROUTEPREP/ARM left without a session for 10 min and the route is empty -> finalize it (no data exists)
//  6 loopTask silent for 180 s while idle -> reboot from an esp_timer (catches a hung loop)
// Reboot = ESP.restart(): LittleFS routes/manifest stay intact. Never while a route is prepared/armed/active,
// a point is measuring, GETPOINT/GETSUMMARY is streaming or OTA is open.

constexpr uint32_t R9_MAGIC=0x39394A31UL;            // "1J99"
constexpr uint32_t R9_RECON_WINDOW_MS=600000UL;      // 10 min
constexpr uint8_t  R9_RECON_REBOOT_COUNT=3;
constexpr uint32_t R9_WIFI_DOWN_BASE_MS=600000UL;    // 10 min, doubles per consecutive watchdog reboot (max x8)
constexpr uint32_t R9_TCP_STALE_CLIENT_MS=10000UL;
constexpr uint32_t R9_TCP_WEDGE_MS=120000UL;
constexpr uint32_t R9_STALE_PREP_MS=600000UL;
constexpr uint32_t R9_LOOP_STALL_MS=180000UL;
constexpr uint8_t  R9_WIFI_STATUS_REBOOTING=238;     // UWB status marker: Anchor reboots ~0.5 s after this ACK

enum R9Cause : uint8_t { R9_NONE=0, R9_UWB_RECON=1, R9_WIFI_DOWN=2, R9_TCP_WEDGED=3, R9_LOOP_STALL=4 };
static const char* r9CauseName(uint8_t c){
  switch(c){case R9_UWB_RECON:return "UWB_RECON_ESCALATION";case R9_WIFI_DOWN:return "WIFI_DOWN_WATCHDOG";
    case R9_TCP_WEDGED:return "TCP_WEDGED";case R9_LOOP_STALL:return "LOOP_STALL";default:return "NONE";}
}
struct R9Rtc { uint32_t magic; uint8_t cause; uint8_t wifi_streak; uint16_t reboots; uint32_t prev_uptime_s; uint32_t check; };
RTC_NOINIT_ATTR static R9Rtc gR9Rtc;
static uint8_t gR9PrevCause=R9_NONE;static uint32_t gR9PrevUptimeS=0;
static uint32_t gR9ReconFirstMs=0,gR9ReconTcpReq=0;static uint8_t gR9ReconCount=0;
static uint32_t gR9WifiDownSinceMs=0,gR9PendingSinceMs=0,gR9LastTcpReq=0,gR9LastTcpReqMs=0,gR9StalePrepSinceMs=0;
static uint32_t gR9StaleDrops=0,gR9StalePrepFinalized=0,gR9RebootAtMs=0;static uint8_t gR9RebootCause=R9_NONE;
static volatile uint32_t gR9LoopBeatMs=0;static volatile bool gR9IdleBeat=false;static esp_timer_handle_t gR9Timer=nullptr;

static uint32_t r9Check(const R9Rtc& r){return r.magic^(uint32_t(r.cause)<<8)^(uint32_t(r.wifi_streak)<<16)^(uint32_t(r.reboots)<<1)^r.prev_uptime_s^0xA5A5A5A5UL;}
static bool r9RtcValid(){return gR9Rtc.magic==R9_MAGIC&&gR9Rtc.check==r9Check(gR9Rtc);}
static void r9RtcSave(){gR9Rtc.magic=R9_MAGIC;gR9Rtc.check=r9Check(gR9Rtc);}

// Route-idle = no RF route/run state that a reboot could corrupt or interrupt (stable, used for timers).
static bool r9RouteIdle(){
  return !active&&!remaining&&!remoteWaitingUwb&&!remoteCaptureEnabled&&!remoteWaitPending&&!remotePreparePending&&!remotePrepared&&
         !r03SessionActive&&!r03WaitPending&&!r03Prepared&&!r03PointMeasuring&&!r03WaitingPoint&&!gR04OtaWindow&&!gR04OtaReceiving;
}
static void r9ScheduleReboot(uint8_t cause,uint32_t delayMs){
  if(gR9RebootAtMs)return;
  gR9RebootCause=cause;gR9RebootAtMs=millis()+delayMs;if(!gR9RebootAtMs)gR9RebootAtMs=1;
  event("{\"event\":\"R1J9_REBOOT_SCHEDULED\",\"cause\":\"%s\",\"in_ms\":%lu,\"uptime_s\":%lu}",r9CauseName(cause),(unsigned long)delayMs,(unsigned long)(millis()/1000));
}
static void r9TimerCb(void*){
  // Runs in the esp_timer task: only reads volatile snapshots written by the loop.
  if(gR9IdleBeat&&!remoteGetpointStreaming&&gR9LoopBeatMs&&uint32_t(millis()-gR9LoopBeatMs)>R9_LOOP_STALL_MS){
    gR9Rtc.cause=R9_LOOP_STALL;gR9Rtc.reboots=uint16_t(gR9Rtc.reboots+1);gR9Rtc.prev_uptime_s=millis()/1000;r9RtcSave();esp_restart();
  }
}
static void r9Setup(){
  if(r9RtcValid()){gR9PrevCause=gR9Rtc.cause;gR9PrevUptimeS=gR9Rtc.cause!=R9_NONE?gR9Rtc.prev_uptime_s:0;}
  else{gR9Rtc=R9Rtc{};gR9Rtc.magic=R9_MAGIC;}
  gR9Rtc.cause=R9_NONE;r9RtcSave();
  gR9LoopBeatMs=millis();gR9LastTcpReq=remoteTcpRequests;gR9LastTcpReqMs=millis();
  esp_timer_create_args_t a{};a.callback=&r9TimerCb;a.name="r9wd";
  if(esp_timer_create(&a,&gR9Timer)==ESP_OK)esp_timer_start_periodic(gR9Timer,5ULL*1000000ULL);
  if(gR9PrevCause!=R9_NONE)event("{\"event\":\"R1J9_RECOVERED_FROM\",\"cause\":\"%s\",\"prev_uptime_s\":%lu,\"wifi_streak\":%u}",
                                 r9CauseName(gR9PrevCause),(unsigned long)gR9PrevUptimeS,unsigned(gR9Rtc.wifi_streak));
}
// Called from the UWB RECONNECT handler (new request id only). true = reboot instead of STA restart.
static bool r9OnUwbRecon(){
  const uint32_t now=millis();
  if(!gR9ReconCount||uint32_t(now-gR9ReconFirstMs)>R9_RECON_WINDOW_MS||remoteTcpRequests!=gR9ReconTcpReq){
    gR9ReconCount=0;gR9ReconFirstMs=now;gR9ReconTcpReq=remoteTcpRequests;
  }
  ++gR9ReconCount;
  if(gR9ReconCount>=R9_RECON_REBOOT_COUNT&&r9RouteIdle()){r9ScheduleReboot(R9_UWB_RECON,500);return true;}
  return false;
}
// ROUTEPREP (and maybe ARM) done, route never started, nothing recorded: same checks as ROUTEABORTEMPTY.
static bool r9FinalizeStalePrep(){
  if(remaining||r03PointMeasuring||remoteCaptureEnabled||r03SessionActive||r03WaitPending||!r03ManifestValid)return false;
  if(r03Manifest.finalized||r03Manifest.total_records||r03Manifest.committed_points||r03Manifest.next_txn)return false;
  const uint32_t sid=r03Manifest.route_id;
  R03FsStats fs=r03FsStats();if(!fs.root_ok)return false;
  File root=LittleFS.open("/");File f=root.openNextFile();bool found=false;
  while(f){String path=r03AbsPath(f);uint32_t r,p,a;bool tmp;if(r03ParsePointPath(path,r,p,a,tmp)&&r==sid)found=true;f.close();f=root.openNextFile();}root.close();
  if(found)return false;
  r03Manifest.finalized=1;if(!r03SaveManifest())return false;
  idleRadio();active=false;remaining=0;r03SessionActive=false;r03WaitPending=false;r03WaitingPoint=false;r03Prepared=false;
  remoteState=RemoteState::TRANSFER;remoteReconnectAtMs=remoteWifiReady?0:millis()+500;++gR9StalePrepFinalized;
  event("{\"event\":\"R1J9_STALE_PREP_FINALIZED\",\"route\":%lu,\"no_data\":true}",(unsigned long)sid);
  return true;
}
// 4) TCP: a newer client is waiting while the held one is silent -> drop the held one;
//    clients waiting continuously and no request served for R9_TCP_WEDGE_MS -> reboot. true = reboot scheduled.
static bool r9TcpCheck(uint32_t now,bool idle){
  static uint32_t lastPoll=0;
  if(remoteTcpRequests!=gR9LastTcpReq){gR9LastTcpReq=remoteTcpRequests;gR9LastTcpReqMs=now;gR9PendingSinceMs=0;}
  if(!remoteWifiReady||remoteGetpointStreaming){gR9PendingSinceMs=0;return false;}
  if(uint32_t(now-lastPoll)<200)return false;   // lwip_accept() at most 5 Hz
  lastPoll=now;
  if(!remoteServer.hasClient()){gR9PendingSinceMs=0;return false;}
  if(!gR9PendingSinceMs)gR9PendingSinceMs=now?now:1;
  if(remoteClient&&remoteClient.connected()&&uint32_t(now-remoteTcpLastRxMs)>R9_TCP_STALE_CLIENT_MS){remoteClient.stop();++gR9StaleDrops;}
  if(idle&&uint32_t(now-gR9PendingSinceMs)>R9_TCP_WEDGE_MS&&uint32_t(now-gR9LastTcpReqMs)>R9_TCP_WEDGE_MS){r9ScheduleReboot(R9_TCP_WEDGED,200);return true;}
  return false;
}
static void r9RecoveryLoop(){
  const uint32_t now=millis();const bool idle=r9RouteIdle();
  gR9LoopBeatMs=now;gR9IdleBeat=idle;
  if(gR9RebootAtMs){
    if(!r9RouteIdle()){gR9RebootAtMs=0;gR9RebootCause=R9_NONE;return;}   // a route started meanwhile: cancel
    if(int32_t(now-gR9RebootAtMs)>=0&&!remoteGetpointStreaming&&!writerBusy.load()&&(outputPending.load()==0||int32_t(now-gR9RebootAtMs)>1500)){
      gR9Rtc.cause=gR9RebootCause;gR9Rtc.reboots=uint16_t(gR9Rtc.reboots+1);gR9Rtc.prev_uptime_s=now/1000;r9RtcSave();
      delay(50);ESP.restart();
    }
    return;
  }
  // 3) Wi-Fi watchdog (only counts while idle; a route with WIFIPOLICY OFF never triggers it)
  if(remoteWifiReady){gR9WifiDownSinceMs=0;if(gR9Rtc.wifi_streak){gR9Rtc.wifi_streak=0;r9RtcSave();}}
  else if(remoteNetValid&&idle){
    if(!gR9WifiDownSinceMs)gR9WifiDownSinceMs=now?now:1;
    const uint8_t s=gR9Rtc.wifi_streak>3?3:gR9Rtc.wifi_streak;
    if(uint32_t(now-gR9WifiDownSinceMs)>(R9_WIFI_DOWN_BASE_MS<<s)){
      if(gR9Rtc.wifi_streak<250){++gR9Rtc.wifi_streak;}
      r9RtcSave();r9ScheduleReboot(R9_WIFI_DOWN,200);return;
    }
  }else gR9WifiDownSinceMs=0;
  // 4) TCP: stale held client / wedged server
  if(r9TcpCheck(now,idle))return;
  // 5) prepared/armed route that never started
  if(r03Prepared&&!r03SessionActive&&!r03WaitPending&&!r03PointMeasuring&&!remaining&&!remoteGetpointStreaming){
    if(!gR9StalePrepSinceMs)gR9StalePrepSinceMs=now?now:1;
    if(uint32_t(now-gR9StalePrepSinceMs)>R9_STALE_PREP_MS){gR9StalePrepSinceMs=0;r9FinalizeStalePrep();}
  }else gR9StalePrepSinceMs=0;
}
static bool r9Command(uint32_t req,const char* op){
  if(strcmp(op,"R1J9INFO")!=0)return false;
  const uint32_t now=millis();
  event("{\"event\":\"ACK\",\"request\":%lu,\"op\":\"R1J9INFO\",\"ok\":true,\"role\":\"anchor\",\"recovery_rev\":\"R1J9\",\"prev_reboot_cause\":\"%s\","
        "\"prev_uptime_s\":%lu,\"app_reboots\":%u,\"wifi_streak\":%u,\"idle\":%s,\"recon_count\":%u,\"wifi_down_s\":%lu,\"tcp_pending_s\":%lu,"
        "\"tcp_last_req_s\":%lu,\"stale_drops\":%lu,\"stale_prep_finalized\":%lu,\"uptime_s\":%lu}",
        (unsigned long)req,r9CauseName(gR9PrevCause),(unsigned long)gR9PrevUptimeS,unsigned(gR9Rtc.reboots),unsigned(gR9Rtc.wifi_streak),
        r9RouteIdle()?"true":"false",unsigned(gR9ReconCount),(unsigned long)(gR9WifiDownSinceMs?(now-gR9WifiDownSinceMs)/1000:0),
        (unsigned long)(gR9PendingSinceMs?(now-gR9PendingSinceMs)/1000:0),(unsigned long)((now-gR9LastTcpReqMs)/1000),
        (unsigned long)gR9StaleDrops,(unsigned long)gR9StalePrepFinalized,(unsigned long)(now/1000));
  return true;
}
