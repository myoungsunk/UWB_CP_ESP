#line 1 "C:\\UWB\\UWB_R04_R1J6_FIELD_FIX_20261010\\R1M_CTRL_V2_RC1\\built\\sketches\\ANCHOR_RF_PLATFORM_V1\\wifi_reconnect.hpp"
#pragma once
// Network recovery runs only outside RF preparation and measurement.
static uint32_t wifiReconnectDueMs=0,wifiReconnectBackoffMs=5000;
static bool wifiNeedTcpTeardown=false;
static uint8_t wifiSoftAttempts=0;
static void wifiReconnectLoop(){
  if(gR04OtaWindow)return; // OTA owns the Wi-Fi link until transfer or window end.
  const bool connected=WiFi.status()==WL_CONNECTED&&uint32_t(WiFi.localIP())!=0;
  if(remoteWifiReady&&!connected){
    remoteWifiReady=false;remoteWifiStarting=false;
    wifiNeedTcpTeardown=true;
    snprintf(remoteWifiError,sizeof(remoteWifiError),"WIFI_LINK_LOST");
    wifiReconnectDueMs=millis()+5000;
    event("{\"event\":\"WIFI_LINK_LOST\",\"retry_ms\":5000}");
  }
  const bool blocked=active||remoteWaitingUwb||remoteCaptureEnabled||remotePreparePending||remoteWaitPending||r03WaitPending||r03SessionActive||
    remoteState==RemoteState::PREPARED||remoteState==RemoteState::ARMED||remoteState==RemoteState::ROUTE_PREPARED||
    remoteState==RemoteState::WAIT_UWB||remoteState==RemoteState::MEASURE||remoteState==RemoteState::ROUTE_WAIT_POINT||remoteState==RemoteState::POINT_MEASURE;
  if(blocked||!remoteNetValid){wifiReconnectDueMs=0;return;}
  if(wifiNeedTcpTeardown){if(remoteClient)remoteClient.stop();remoteServer.stop();MDNS.end();wifiNeedTcpTeardown=false;}
  if(connected){
    if(!remoteWifiReady)remoteWifiStarting=true;
    wifiReconnectDueMs=0;wifiReconnectBackoffMs=5000;wifiSoftAttempts=0;return;
  }
  if(remoteWifiStarting||remoteReconnectAtMs)return;
  if(!wifiReconnectDueMs)wifiReconnectDueMs=millis()+wifiReconnectBackoffMs;
  if(int32_t(millis()-wifiReconnectDueMs)<0)return;
  wifiReconnectDueMs=0;
  const bool wasOff=WiFi.getMode()==WIFI_OFF;
  bool issued=false;
  const bool lastResort=!wasOff&&wifiSoftAttempts>=2;  // R1J9: 2 same-AP retries, then full STA restart
  if(wasOff){remoteWifiStart();issued=remoteWifiStarting;wifiSoftAttempts=0;}
  else if(lastResort){
    remoteWifiStop();remoteWifiStart();issued=remoteWifiStarting;wifiSoftAttempts=0;
  }else {
    ++wifiSoftAttempts;++wifiReconnectAttemptCount;
    issued=WiFi.reconnect();
    if(issued){remoteWifiStartMs=millis();remoteWifiStarting=true;}
    else snprintf(remoteWifiError,sizeof(remoteWifiError),"WIFI_RECONNECT_CALL_FAILED");
  }
  event("{\"event\":\"WIFI_AUTO_RECONNECT\",\"backoff_ms\":%lu,\"mode\":\"%s\",\"issued\":%s,\"first_reason\":%d}",
    (unsigned long)wifiReconnectBackoffMs,wasOff?"STA_START":lastResort?"LAST_RESORT_RADIO_RESET":"STA_RECONNECT",issued?"true":"false",wifiFirstDisconnectReason);
  wifiReconnectBackoffMs=wifiReconnectBackoffMs<30000?wifiReconnectBackoffMs*2:60000;
}
