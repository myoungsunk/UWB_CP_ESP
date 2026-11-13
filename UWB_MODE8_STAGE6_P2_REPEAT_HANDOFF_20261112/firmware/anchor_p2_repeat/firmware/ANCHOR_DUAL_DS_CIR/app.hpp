#pragma once
#include "esp32_port.hpp"
#include "record_json.hpp"
#include "uart_fragment.hpp"
#include "transport_guard.hpp"
#include <esp_system.h>
#include <driver/uart.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <atomic>
#include <new>
#include <stdlib.h>
#if DUAL8_ANCHOR
#include <WiFi.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <ArduinoOTA.h>
#include <Update.h>
#include <mbedtls/md.h>
#endif
namespace dual8app {
using namespace dual8;
static Esp32Port port;static Config config;
static constexpr unsigned BASE_POOL=3;
static constexpr unsigned POOL=DUAL8_ANCHOR?5:BASE_POOL;
// Wi-Fi increases static DRAM usage on ESP32. Reserve the same record pool
// from heap during setup, before enabling the operational network.
static Record* basePool[BASE_POOL]{};
static Record* extraPool[2]{};
static Record& recordAt(unsigned index){
 return index<BASE_POOL?*basePool[index]:*extraPool[index-BASE_POOL];
}
static QueueHandle_t freeQ=nullptr,outQ=nullptr;
constexpr unsigned wireStride=UART_FRAGMENT_HEADER+UART_FRAGMENT_BYTES;
constexpr unsigned maxWireData=(24+JSON_MAX+(DUAL8_ANCHOR?0:4*CIR_BYTES)+UART_FRAGMENT_BYTES-1)/UART_FRAGMENT_BYTES;
static_assert(maxWireData<=UART_FRAGMENT_MAX_DATA,"RFB8 fragment capacity");
static uint8_t* uartWire=nullptr;
struct Out {
 uint8_t index=255;
 bool fragmented=false;
 char text[768]{};
};
static ReplaySlot cache[POOL]{};
static portMUX_TYPE replayMux=portMUX_INITIALIZER_UNLOCKED;
constexpr uint64_t REPLAY_TIMEOUT_US=1500000;
constexpr unsigned REPLAY_LIMIT=2;
static std::atomic<unsigned> awaitingAck{0},retransmissions{0};
static std::atomic<unsigned> ackQueued{0},ackRetired{0},peakAwaitingAck{0};
static unsigned queueScheduleSkips=0;
static std::atomic<uint32_t> pending{0},drops{0},sent{0},writerMaxUs{0};
static uint32_t boot=0,session=0,txn=0,peerBoot=0,remaining=0,periodMiss=0;static uint64_t nextStart=0;
static uint64_t runExpire=0;static uint32_t targetCount=0;
static bool active=false,hwOk=false;static uint32_t recordSequence=0;
#if DUAL8_ANCHOR
static bool opsInternalStart=false;
static bool opsArchiveEnabled();
static bool opsUsbCommand(const char*);
static void opsSetup();
static void opsLoop();
static void opsCancelScheduledStart();
#endif
static const char*role(){return DUAL8_ANCHOR?"anchor":"tag";}
static void emit(const char*fmt,...) __attribute__((format(printf,1,2)));
static void emit(const char*fmt,...){
 if(!outQ){return;}
 Out o;o.fragmented=config.stage==6&&config.baud==2000000;va_list a;va_start(a,fmt);int n=vsnprintf(o.text,sizeof o.text,fmt,a);va_end(a);
 if(n<0||size_t(n)>=sizeof o.text){++drops;return;}++pending;if(xQueueSend(outQ,&o,0)!=pdTRUE){--pending;++drops;}
}
#if DUAL8_ANCHOR
static void optionalNumber(char (&text)[21],uint64_t value,bool present){
 if(present)snprintf(text,sizeof text,"%llu",(unsigned long long)value);
 else strcpy(text,"null");
}
static void formatRxEdge(char (&text)[128],const AnchorRxEdgeSample& sample){
 if(!sample.sampled){strcpy(text,"null");return;}
 snprintf(text,sizeof text,
          "{\"at_us\":%llu,\"sys_status\":%llu,\"sys_valid\":%s,\"rf_status\":%u,\"rf_valid\":%s}",
          (unsigned long long)sample.at_us,(unsigned long long)sample.sys_status,
          sample.sys_status!=MASK40?"true":"false",unsigned(sample.rf_status),
          sample.rf_status!=0xff?"true":"false");
}
static void emitAnchorRxEdge(uint8_t index){
 const auto& edges=port.anchorRxEdges();
 const auto& cycle=edges.cycles[index];
 const bool response=edges.session!=0;
 char sessionText[21],txnText[21],onBefore[128],onAfter[128],offBefore[128],offAfter[128];
 optionalNumber(sessionText,edges.session,response);optionalNumber(txnText,cycle.context.txn,response);
 formatRxEdge(onBefore,cycle.on_before);formatRxEdge(onAfter,cycle.on_after);
 formatRxEdge(offBefore,cycle.off_before);formatRxEdge(offAfter,cycle.off_after);
 emit("{\"event\":\"ANCHOR_RX_EDGE\",\"boot\":%lu,\"scope\":\"%s\",\"session\":%s,\"txn\":%s,\"index\":%u,"
      "\"exchange\":%s,\"retry\":%s,\"window_on\":%s,\"window_off\":%s,\"off_reason\":%s%s%s,"
      "\"on_before\":%s,\"on_after\":%s,\"off_before\":%s,\"off_after\":%s}",
      (unsigned long)boot,edges.scope,sessionText,txnText,unsigned(index),
      response?(cycle.context.second?"2":"1"):"null",response?(cycle.context.retry?"true":"false"):"null",
      response?(cycle.window_on?"\"B\"":"\"A\""):"null",
      response&&cycle.off_before.sampled?(cycle.window_off?"\"B\"":"\"A\""):"null",
      cycle.off_reason?"\"":"",cycle.off_reason?cycle.off_reason:"null",cycle.off_reason?"\"":"",
      onBefore,onAfter,offBefore,offAfter);
}
static void emitAnchorRxEdges(){
 const auto& edges=port.anchorRxEdges();
 char sessionText[21];optionalNumber(sessionText,edges.session,edges.session!=0);
 emit("{\"event\":\"ANCHOR_RX_EDGES\",\"boot\":%lu,\"scope\":%s%s%s,\"session\":%s,\"captured\":%s,"
      "\"count\":%u,\"dropped\":%u,\"spi_hz\":2000000}",
      (unsigned long)boot,edges.scope?"\"":"",edges.scope?edges.scope:"null",edges.scope?"\"":"",
      sessionText,edges.count?"true":"false",unsigned(edges.count),edges.dropped);
 for(uint8_t i=0;i<edges.count;++i)emitAnchorRxEdge(i);
}
static void emitAnchorRxFault(){
 const auto& f=port.anchorRxFault();
 if(!f.phase){
  emit("{\"event\":\"ANCHOR_RX_FIRST_FAULT\",\"captured\":false,\"boot\":%lu}",(unsigned long)boot);
  return;
 }
 char sessionText[21],txnText[21],pollDw[21],pollEnd[21],armBegin[21],armEnd[21],rfFirst[21],ecFirst[21];
 optionalNumber(sessionText,f.context.session,!f.selftest);optionalNumber(txnText,f.context.txn,!f.selftest);
 optionalNumber(pollDw,f.context.poll_tx_dw,!f.selftest);optionalNumber(pollEnd,f.context.poll_tx_end_us,!f.selftest);
 optionalNumber(armBegin,f.arm_begin_us,f.arm_begin_us!=0);optionalNumber(armEnd,f.arm_end_us,f.arm_end_us!=0);
 optionalNumber(rfFirst,f.rf_first,f.first_clock_sampled);optionalNumber(ecFirst,f.ec_first,f.first_clock_sampled);
 emit("{\"event\":\"ANCHOR_RX_FIRST_FAULT\",\"captured\":true,\"boot\":%lu,\"scope\":\"%s\",\"session\":%s,\"txn\":%s,"
      "\"window\":%s,\"exchange\":%s,\"retry\":%s,\"radio\":%u,\"phase\":\"%s\",\"spi_hz\":2000000,"
      "\"at_us\":%llu,\"poll_tx_dw\":%s,\"poll_tx_end_us\":%s,\"arm_begin_us\":%s,\"arm_end_us\":%s,"
      "\"status_first\":%llu,\"status_first_read_valid\":%s,\"status_reread\":%llu,\"status_reread_read_valid\":%s,"
      "\"dev_id\":%lu,\"dev_id_matches\":%s,\"rf_status\":%u,\"rf_status_read_valid\":%s,\"ec_ctrl\":%u,\"ec_ctrl_read_valid\":%s,"
      "\"rf_status_first\":%s,\"ec_ctrl_first\":%s}",
      (unsigned long)boot,f.selftest?"selftest":"response",sessionText,txnText,
      f.selftest?"null":(f.window?"\"B\"":"\"A\""),f.selftest?"null":(f.context.second?"2":"1"),
      f.selftest?"null":(f.context.retry?"true":"false"),unsigned(f.radio),f.phase,
      (unsigned long long)f.at_us,pollDw,pollEnd,armBegin,armEnd,
      (unsigned long long)f.first,f.first!=MASK40?"true":"false",
      (unsigned long long)f.reread,f.reread!=MASK40?"true":"false",
      (unsigned long)f.dev_id,f.dev_id==0xdeca0130U?"true":"false",
      unsigned(f.rf_status),f.rf_status!=0xff?"true":"false",
      unsigned(f.ec_ctrl),f.ec_ctrl!=0xff?"true":"false",rfFirst,ecFirst);
}
#endif
static bool prepareUartWire(const Config& next){
 if(next.stage!=6||next.baud!=2000000||uartWire)return true;
 const unsigned bytes=(maxWireData+uartParityCount(maxWireData))*wireStride+3*UART_FRAGMENT_HEADER;
 uartWire=static_cast<uint8_t*>(malloc(bytes));
 if(uartWire)return true;
 emit("{\"event\":\"ERROR\",\"reason\":\"UART_RECORD_BUFFER_MEMORY\"}");
 return false;
}
// UART0 has no TX ring. Only the writer submits bytes; the command task
// changes baud only when the writer and physical UART are both idle.
struct UartTx {
 uint64_t nextYield=now()+4000;
 static uint64_t now(){return esp_timer_get_time();}
 static int write(const uint8_t* p,size_t n){
  return uart_tx_chars(UART_NUM_0,reinterpret_cast<const char*>(p),n);
 }
 static int idle(){
  const auto result=uart_wait_tx_done(UART_NUM_0,0);
  return result==ESP_OK?1:result==ESP_ERR_TIMEOUT?0:-1;
 }
 void pause(){
  if(now()>=nextYield){vTaskDelay(1);nextYield=now()+4000;}
  else delayMicroseconds(100);
 }
};
static constexpr uint64_t UART_FRAME_US=350000;
static void encodeFragment(uint8_t*h,uint32_t seq,unsigned index,unsigned dataCount,unsigned size,bool data){
 h[0]='R';h[1]='8';h[2]='F';h[3]='6';
 putLE(h+4,seq,4);h[8]=uint8_t(index);h[9]=uint8_t(dataCount);putLE(h+10,size,2);
 uint32_t crc=crcStep(0xffffffff,h,12);
 if(data)crc=crcStep(crc,h+UART_FRAGMENT_HEADER,UART_FRAGMENT_BYTES);
 putLE(h+12,crc^0xffffffff,4);
}
static bool sendFragments(BoundedWire<UartTx>& output,uint32_t seq,const uint8_t*header,const uint8_t*meta,size_t nmeta,const uint8_t*raw,size_t nraw){
 static const UartField field;
 constexpr unsigned stride=wireStride;
 uint8_t* const wire=uartWire;
 const unsigned bytes=24+nmeta+nraw,dataCount=(bytes+UART_FRAGMENT_BYTES-1)/UART_FRAGMENT_BYTES;
 if(!wire||dataCount>maxWireData)return false;
 const unsigned count=dataCount+uartParityCount(dataCount);
 memset(wire,0,count*stride);
 const uint8_t* parts[]={header,meta,raw};
 const size_t lengths[]={24,nmeta,nraw};
 unsigned part=0;size_t offset=0;
 for(unsigned i=0;i<dataCount;++i){
  uint8_t* fragment=wire+i*stride;
  uint8_t* data=fragment+UART_FRAGMENT_HEADER;
  unsigned used=0;
  while(used<UART_FRAGMENT_BYTES&&part<3){
   size_t copy=lengths[part]-offset;
   if(copy>UART_FRAGMENT_BYTES-used)copy=UART_FRAGMENT_BYTES-used;
   if(copy)memcpy(data+used,parts[part]+offset,copy);
   used+=copy;offset+=copy;
   if(offset==lengths[part]){++part;offset=0;}
  }
  for(unsigned j=dataCount;j<count;++j)field.add(wire+j*stride+UART_FRAGMENT_HEADER,data,used,i,j);
  encodeFragment(fragment,seq,i,dataCount,bytes,true);
 }
 for(unsigned j=dataCount;j<count;++j)encodeFragment(wire+j*stride,seq,j,dataCount,bytes,true);
 // Submit the whole record to UART once, including the redundant end markers.
 for(unsigned j=0;j<3;++j)encodeFragment(wire+count*stride+j*UART_FRAGMENT_HEADER,seq,UART_FRAGMENT_END,dataCount,bytes,false);
 return output.send(wire,count*stride+3*UART_FRAGMENT_HEADER);
}
// A failed frame may leave a FIFO tail. Resume on the same UART only
// after observing the transmitter idle within the next frame budget.
static bool uartRecoveryRequired=false;
static WireResult wireAt(uint32_t sequence,uint8_t kind,const char*json,size_t size,const uint8_t*raw,size_t nraw,bool fragmented){
 UartTx uart;
 BoundedWire<UartTx> output(uart,UART_FRAME_US);
 if(uartRecoveryRequired&&output.waitIdle()!=WireResult::Ok)return output.finish();
 uartRecoveryRequired=true;
 uint8_t h[24]{};putLE(h,WIRE_MAGIC,4);h[4]=1;h[5]=kind;putLE(h+6,24,2);putLE(h+8,sequence,4);putLE(h+12,size,4);putLE(h+16,nraw,4);
 uint32_t c=crcStep(0xffffffff,json,size);if(nraw)c=crcStep(c,raw,nraw);putLE(h+20,c^0xffffffff,4);
 if(fragmented){
  if(!sendFragments(output,sequence,h,reinterpret_cast<const uint8_t*>(json),size,raw,nraw)){
   const auto result=output.finish();
   uartRecoveryRequired=result!=WireResult::Ok;
   return result==WireResult::Ok?WireResult::Encoding:result;
  }
 }else{
  if(!output.send(h,24)||!output.send(reinterpret_cast<const uint8_t*>(json),size)||
     (nraw&&!output.send(raw,nraw)))return output.finish();
 }
 const auto result=output.finish();
 uartRecoveryRequired=result!=WireResult::Ok;
 return result;
}
static WireResult wire(uint8_t kind,const char*json,size_t size,const uint8_t*raw,size_t nraw,bool fragmented){
 return wireAt(recordSequence++,kind,json,size,raw,nraw,fragmented);
}
static WireResult writeRecord(Record& r,char* json,uint32_t sequence){
 size_t n=0;
 if(!serializeRecord(r,json,JSON_MAX+1,n))return WireResult::Encoding;
 return wireAt(sequence,2,json,n,r.anchor?nullptr:&r.cir[0][0],r.anchor?0:4*CIR_BYTES,
   r.cfg.stage==6&&r.cfg.baud==2000000);
}
static void releaseRecord(uint8_t i){
 if(xQueueSend(freeQ,&i,0)!=pdTRUE)++drops;
}
static void retireReplay(uint8_t i){
 releaseRecord(i);
 --awaitingAck;
}
static const char* wireError(WireResult result){
 return result==WireResult::Timeout?"UART_FRAME_TIMEOUT":
        result==WireResult::Driver?"UART_DRIVER_ERROR":"SERIALIZE_OVERFLOW";
}
static void writerError(const char* reason,bool fragmented,uint32_t seq,uint32_t txn){
 ++drops;
 // Wait only within the next frame's own budget for any previous FIFO tail.
 // A partial frame remains visible in the raw stream and makes DRAINED fail.
 char event[200];
 const int n=snprintf(event,sizeof event,"{\"event\":\"ERROR\",\"reason\":\"%s\",\"sequence\":%lu,\"txn\":%lu}",
  reason,(unsigned long)seq,(unsigned long)txn);
 if(n>0&&size_t(n)<sizeof event)wire(1,event,size_t(n),nullptr,0,fragmented);
}
static void noticeReplay(const RecordKey& key,bool ack){
 const uint64_t now=esp_timer_get_time();
 portENTER_CRITICAL(&replayMux);
 for(auto& slot:cache){
  if(!slot.offer(key,ack,now))continue;
  if(ack)++ackQueued;
  break;
 }
 portEXIT_CRITICAL(&replayMux);
}
static void serviceReplay(char* json){
 for(uint8_t i=0;i<POOL;++i){
  auto& slot=cache[i];
  const uint64_t now=esp_timer_get_time();
  portENTER_CRITICAL(&replayMux);
  const auto decision=slot.take(now);
  portEXIT_CRITICAL(&replayMux);
  if(decision==ReplayDecision::None)continue;
  if(decision==ReplayDecision::Ack){++ackRetired;retireReplay(i);continue;}
  const uint32_t sequence=slot.key.sequence, txn=recordAt(i).txn;
  if(decision==ReplayDecision::Expired){
   writerError("RECORD_ACK_TIMEOUT",true,sequence,txn);
   retireReplay(i);continue;
  }
  const bool exhausted=slot.attempts>=REPLAY_LIMIT;
  const auto result=exhausted?WireResult::Encoding:writeRecord(recordAt(i),json,sequence);
  if(result==WireResult::Ok){++slot.attempts;++retransmissions;continue;}
  portENTER_CRITICAL(&replayMux);
  const bool alreadyAcked=slot.ack;
  slot.occupied=false;
  portEXIT_CRITICAL(&replayMux);
  if(alreadyAcked)++ackRetired;
  if(!exhausted||!alreadyAcked)
   writerError(exhausted?"RECORD_REPLAY_EXHAUSTED":wireError(result),true,sequence,txn);
  retireReplay(i);
 }
}
static void writer(void*){
 static char json[JSON_MAX+1];Out o;
 for(;;){
  const bool queued=xQueueReceive(outQ,&o,1)==pdTRUE;
  serviceReplay(json);
  if(!queued)continue;
  if(o.index==255){
   if(wire(1,o.text,strlen(o.text),nullptr,0,o.fragmented)!=WireResult::Ok)++drops;
  }else{
   const uint8_t i=o.index;Record& r=recordAt(i);
   const uint32_t started=uint32_t(esp_timer_get_time()),sequence=recordSequence++;
   const bool fragmented=r.cfg.stage==6&&r.cfg.baud==2000000;
   if(fragmented){
    const RecordKey key(boot,r.session,configId(r.cfg),sequence);
    portENTER_CRITICAL(&replayMux);
    cache[i]=ReplaySlot{};
    cache[i].key=key;
    cache[i].occupied=true;
    portEXIT_CRITICAL(&replayMux);
    const unsigned held=++awaitingAck;
    if(held>peakAwaitingAck.load())peakAwaitingAck=held;
   }
   const auto result=writeRecord(r,json,sequence);
   if(result==WireResult::Ok){
    ++sent;
    if(fragmented){
     const uint64_t expires=uint64_t(esp_timer_get_time())+REPLAY_TIMEOUT_US;
     portENTER_CRITICAL(&replayMux);
     cache[i].expires=expires;
     portEXIT_CRITICAL(&replayMux);
    }else releaseRecord(i);
   }else{
    const uint32_t txn=r.txn;
    if(fragmented){
     portENTER_CRITICAL(&replayMux);
     cache[i].occupied=false;
     portEXIT_CRITICAL(&replayMux);
    }
    writerError(wireError(result),fragmented,sequence,txn);
    if(fragmented)retireReplay(i);else releaseRecord(i);
   }
   const uint32_t elapsed=uint32_t(esp_timer_get_time())-started;
   if(elapsed>writerMaxUs)writerMaxUs=elapsed;
  }
  --pending;
 }
}
static int reserve(bool countDrop=true){
 uint8_t index;
 if(xQueueReceive(freeQ,&index,0)!=pdTRUE){
  if(countDrop)++drops;
  return -1;
 }
 new(&recordAt(index))Record();
 return index;
}
#if DUAL8_ANCHOR
// A writer can retire an ACK while the anchor enters the scheduled slot.
static int reserveAnchor(uint64_t scheduled){
 constexpr uint64_t ACK_GRACE_US=1000;
 int index=reserve(false);
 while(index<0&&port.nowUs()<scheduled+ACK_GRACE_US){
  port.relaxRx(true);
  index=reserve(false);
 }
 return index;
}
#endif
static void submit(uint8_t i){
 const unsigned n=DUAL8_ANCHOR?1:2;for(unsigned j=0;j<n;++j)recordAt(i).cal[j]=port.calibration(j);
 Out o;o.index=i;++pending;if(xQueueSend(outQ,&o,0)!=pdTRUE){--pending;++drops;xQueueSend(freeQ,&i,0);}
}
static void idleAll(){port.idle(0);if(!DUAL8_ANCHOR)port.idle(1);}
static bool number(const char*s,uint32_t&v){
 if(!s||!*s||*s=='-'||*s=='+'){return false;}
 char*end;unsigned long long n=strtoull(s,&end,10);if(*end||n>0xffffffffULL)return false;v=uint32_t(n);return true;
}
static uint32_t drainRequest=0, dropsAtStart=0;
static uint64_t drainDeadline=0;
static bool drainComplete=true, drainClean=true;
static uint32_t sessionFailures(){return drops.load()-dropsAtStart;}
static bool transportQuiet(){
 // Read pending first: the writer publishes replay state before dropping pending.
 const auto queued=pending.load();
 const auto held=awaitingAck.load();
 return writerQuiet(queued,held,!queued&&!held&&UartTx::idle()>0);
}
static bool transportChangeAllowed(){
 return canChangeTransport(active,drainRequest!=0,drainComplete,!active&&transportQuiet());
}
static void emitDrained(uint32_t request,bool ok,const char* reason){
 emit("{\"event\":\"DRAINED\",\"role\":\"%s\",\"boot\":%lu,\"session\":%lu,\"request\":%lu,"
      "\"ok\":%s,\"rf_stopped\":%s,\"pending\":%lu,\"awaiting_ack\":%u,\"failures\":%lu,\"reason\":\"%s\"}",
      role(),(unsigned long)boot,(unsigned long)session,(unsigned long)request,
      ok?"true":"false",active?"false":"true",(unsigned long)pending.load(),awaitingAck.load(),
      (unsigned long)sessionFailures(),reason);
}
static void serviceDrain(){
 if(!drainRequest)return;
 const bool quiet=transportQuiet();
 if(!quiet&&UartTx::now()<drainDeadline)return;
 const uint32_t request=drainRequest;
 drainRequest=0;
 drainComplete=quiet;
 drainClean=quiet&&!sessionFailures();
 emitDrained(request,drainClean,quiet?(drainClean?"OK":"TRANSPORT_FAILURE"):"TIMEOUT");
}
static void hello(uint32_t request=0){emit("{\"event\":\"HELLO\",\"role\":\"%s\",\"firmware\":\"%s\",\"mode\":8,\"wire\":\"RFB8\",\"record_replay\":1,\"drain_protocol\":1,\"session\":%lu,\"hello_request\":%lu,\"record_retransmissions\":%lu,\"hardware_profile\":\"%s\",\"hw_ok\":%s,\"fault_latched\":%s,\"boot\":%lu,\"config_id\":%lu,\"stage\":%u,\"period_us\":%lu,\"baud\":%lu,\"active\":%s,\"wifi_ota\":false,\"cal_default_is_uncalibrated\":true,\"drops\":%lu,\"sent\":%lu,\"period_miss\":%lu,\"writer_max_us\":%lu,\"queue_pool\":%u,\"queue_ack_queued\":%u,\"queue_ack_retired\":%u,\"queue_peak_awaiting_ack\":%u,\"queue_schedule_skips\":%u}",role(),firmwareId(DUAL8_ANCHOR),(unsigned long)session,(unsigned long)request,(unsigned long)retransmissions.load(),DUAL8_ANCHOR?"esp32-dw1000-anchor-21-22-5":"esp32-dw1000-tag-ab-21-22-17-25-26-27",hwOk?"true":"false",port.faulted()?"true":"false",(unsigned long)boot,(unsigned long)configId(config),config.stage,(unsigned long)config.period_us,(unsigned long)config.baud,active?"true":"false",(unsigned long)drops.load(),(unsigned long)sent.load(),(unsigned long)periodMiss,(unsigned long)writerMaxUs.load(),POOL,ackQueued.load(),ackRetired.load(),peakAwaitingAck.load(),queueScheduleSkips);}
static void stop(uint32_t request=0){
#if DUAL8_ANCHOR
 if(!opsInternalStart)opsCancelScheduledStart();
#endif
 active=false;remaining=0;idleAll();
 emit("{\"event\":\"STOPPED\",\"role\":\"%s\",\"session\":%lu,\"boot\":%lu,\"stop_request\":%lu,\"physical_rf_off_confirmed\":false}",role(),(unsigned long)session,(unsigned long)boot,(unsigned long)request);
#if DUAL8_ANCHOR
 if(port.takeAnchorEdgeReport())emitAnchorRxEdges();
#endif
}
static void command(char*line){
 char*save=nullptr;char*w[9]{};unsigned n=0;for(char*p=strtok_r(line," \t\r",&save);p;p=strtok_r(nullptr," \t\r",&save)){if(n==9){emit("{\"event\":\"ERROR\",\"reason\":\"TOO_MANY_TOKENS\"}");return;}w[n++]=p;}if(!n)return;
 if(!strcmp(w[0],"REC_ACK")||!strcmp(w[0],"REC_NACK")){
  uint32_t b,s,seq;
  if(n!=4||!number(w[1],b)||!number(w[2],s)||!number(w[3],seq)||
     b!=boot||!s||s!=session||config.stage!=6||config.baud!=2000000)return;
  // Configuration cannot change while this session has cached records.
  noticeReplay({b,s,configId(config),seq},!strcmp(w[0],"REC_ACK"));
  return;
 }
 if(!strcmp(w[0],"HELLO")||!strcmp(w[0],"STATUS")){
  uint32_t request=0;
  if(n==1||(n==2&&number(w[1],request)&&request))hello(request);
  else emit("{\"event\":\"ERROR\",\"reason\":\"BAD_HELLO_REQUEST\"}");
  return;
 }
 if(!strcmp(w[0],"DRAIN")){
  uint32_t s,b,request;
  if(n!=4||!number(w[1],s)||!number(w[2],b)||!number(w[3],request)||!request){
   emit("{\"event\":\"ERROR\",\"reason\":\"BAD_DRAIN_REQUEST\"}");return;
  }
  if(s!=session||b!=boot){emitDrained(request,false,"DRAIN_IDENTITY");return;}
  if(active){emitDrained(request,false,"RF_ACTIVE");return;}
  if(drainRequest){if(drainRequest!=request)emitDrained(request,false,"DRAIN_BUSY");return;}
  drainRequest=request;drainDeadline=UartTx::now()+5000000;
  return;
 }
#if DUAL8_ANCHOR
 if(!strcmp(w[0],"RXFAULT")&&n==1&&!active){emitAnchorRxFault();return;}
 if(!strcmp(w[0],"RXEDGES")&&!active){
  uint32_t index=0;
  if(n==1)emitAnchorRxEdges();
  else if(n==2&&number(w[1],index)&&index<port.anchorRxEdges().count)emitAnchorRxEdge(uint8_t(index));
  else emit("{\"event\":\"ERROR\",\"reason\":\"BAD_RXEDGE_INDEX\"}");
  return;
 }
 if(!strcmp(w[0],"PLLSTATUS")&&n==1&&!active){
  const auto&p=port.pll_startup;
  const auto live=port.pllLive();
  emit("{\"event\":\"PLLSTATUS\",\"role\":\"anchor\",\"boot\":%lu,\"before\":%llu,\"after\":%llu,\"first_poll_pre_tx\":%llu,\"rf_before\":%u,\"rf_after\":%u,\"ec_ctrl\":%u,\"acknowledged\":%s,\"startup_ready\":%s,\"ready\":%s,\"first_poll_checked\":%s,"
    "\"first_poll_entry\":%llu,\"first_poll_after_idle\":%llu,\"first_poll_after_clear\":%llu,\"first_poll_after_schedule\":%llu,\"first_poll_after_buffer\":%llu,"
    "\"live_status_fast\":%llu,\"live_status_slow\":%llu,\"live_rf_fast\":%u,\"live_rf_slow\":%u,\"live_ec_ctrl\":%u,\"fast_sampled\":%s,"
    "\"attempts\":%u,\"retry_status\":%llu,\"retry_rf\":%u,\"clock_fault_recorded\":%s,\"fault_rf\":%u,\"fault_ec_ctrl\":%u}",
    (unsigned long)boot,(unsigned long long)p.before,(unsigned long long)p.after,(unsigned long long)p.first_poll_pre_tx,
    p.rf_before,p.rf_after,p.ec_ctrl,p.acknowledged?"true":"false",p.ready?"true":"false",live.ready?"true":"false",p.first_poll_checked?"true":"false",
    (unsigned long long)p.first_poll_entry,(unsigned long long)p.first_poll_after_idle,
    (unsigned long long)p.first_poll_after_clear,(unsigned long long)p.first_poll_after_schedule,
    (unsigned long long)p.first_poll_after_buffer,(unsigned long long)live.fast,(unsigned long long)live.slow,live.rf_fast,live.rf_slow,live.detector,live.fast_sampled?"true":"false",
    p.attempts,(unsigned long long)p.retry_status,p.retry_rf,
    port.clockFaultRecorded()?"true":"false",port.faultRf(),port.faultDetector());
  return;
 }
#endif
 if(!strcmp(w[0],"STOP")){
  uint32_t request=0;
  if(n==1||(n==2&&number(w[1],request)&&request))stop(request);
  else emit("{\"event\":\"ERROR\",\"reason\":\"BAD_STOP_REQUEST\"}");
  return;
 }
 if(!strcmp(w[0],"HELP")){emit("{\"event\":\"HELP\",\"commands\":\"HELLO; SELFTEST; "
#if DUAL8_ANCHOR
  "PLLSTATUS; RXFAULT; RXEDGES [index]; "
#endif
  "SET STAGE|PERIOD_US|CIR_HZ|VERIFY_CIR value; BAUD 921600|2000000; ANT radio tx rx cal_id sigma_m; START session count; STOP [request]; DRAIN session boot request; RESET\"}");return;}
#if DUAL8_ANCHOR
 if(!strcmp(w[0],"START")||!strcmp(w[0],"SET")||!strcmp(w[0],"BAUD")||
    !strcmp(w[0],"ANT")||!strcmp(w[0],"RESET")||!strcmp(w[0],"SELFTEST")){
  if(!opsInternalStart&&opsArchiveEnabled()){
   emit("{\"event\":\"ERROR\",\"reason\":\"OPS_ARCHIVE_LOCK\"}");return;
  }
  if(WiFi.getMode()!=WIFI_OFF){
   emit("{\"event\":\"ERROR\",\"reason\":\"OPS_WIFI_MAINTENANCE\"}");return;
  }
 }
#endif
 if(active){emit("{\"event\":\"ERROR\",\"reason\":\"STOP_REQUIRED\"}");return;}
 if(!strcmp(w[0],"RESET")&&n==1){
  if(!transportChangeAllowed()){
   emit("{\"event\":\"ERROR\",\"reason\":\"RECORD_DRAIN_PENDING\"}");return;
  }
  idleAll();hwOk=port.initialize();
  if(hwOk&&!drainClean){
   // Retire the failed run identity. Its existing DRAINED failure stays valid.
   session=0;dropsAtStart=drops.load();drainClean=true;
  }
  hello();return;
 }
 if(!strcmp(w[0],"SELFTEST")&&n==1){
#if DUAL8_ANCHOR
  const bool hadRxFault=port.anchorRxFault().phase!=nullptr;
  if(!port.faulted())port.beginAnchorEdgeReport("selftest",0);
  port.beginAnchorSelftest();
#endif
  hwOk=port.selftest();
#if DUAL8_ANCHOR
  port.endAnchorRx();
  if(port.takeAnchorEdgeReport())emitAnchorRxEdges();
  if(!hadRxFault&&port.anchorRxFault().phase)emitAnchorRxFault();
  char detail[320]=",\"init_failure\":null";
  const auto& f=port.startupFailure();
  if(f.step){
   char offset[16]="null";
   if(f.offset!=0xffff)snprintf(offset,sizeof offset,"\"0x%x\"",unsigned(f.offset));
   snprintf(detail,sizeof detail,",\"init_failure\":{\"step\":\"%s\",\"reg\":\"0x%02x\",\"offset\":%s,"
            "\"comparison\":\"%s\",\"mask\":\"0x%llx\",\"expected\":\"0x%llx\",\"actual\":\"0x%llx\"}",
            f.step,unsigned(f.reg),offset,f.comparison,(unsigned long long)f.mask,
            (unsigned long long)f.expected,(unsigned long long)f.actual);
  }
#else
  const char*detail="";
#endif
  emit("{\"event\":\"SELFTEST\",\"role\":\"%s\",\"pass\":%s,\"faults\":%lu,\"dev_a\":%lu,\"dev_b\":%lu,\"ant_a_tx\":%u,\"ant_a_rx\":%u,\"ant_b_tx\":%u,\"ant_b_rx\":%u%s}",
       role(),hwOk?"true":"false",(unsigned long)port.faults,(unsigned long)port.last_dev[0],
       (unsigned long)port.last_dev[1],port.last_ant_tx[0],port.last_ant_rx[0],port.last_ant_tx[1],port.last_ant_rx[1],detail);
  return;
 }
 if((!strcmp(w[0],"SET")||!strcmp(w[0],"BAUD")||!strcmp(w[0],"START")||
     !strcmp(w[0],"ANT"))&&!transportChangeAllowed()){
  emit("{\"event\":\"ERROR\",\"reason\":\"RECORD_DRAIN_PENDING\"}");return;
 }
 if(!strcmp(w[0],"SET")&&n==3){uint32_t v;if(!number(w[2],v)){emit("{\"event\":\"ERROR\",\"reason\":\"BAD_INTEGER\"}");return;}Config next=config;
  if(!strcmp(w[1],"STAGE")){if(v<1||v>6){emit("{\"event\":\"ERROR\",\"reason\":\"BAD_STAGE\"}");return;}next.stage=uint8_t(v);}
  else if(!strcmp(w[1],"PERIOD_US"))next.period_us=v;
  else if(!strcmp(w[1],"CIR_HZ"))next.cir_hz=v;
  else if(!strcmp(w[1],"VERIFY_CIR")){if(v>1){emit("{\"event\":\"ERROR\",\"reason\":\"BAD_VERIFY_CIR\"}");return;}next.verify_cir=v!=0;}
  else {emit("{\"event\":\"ERROR\",\"reason\":\"UNKNOWN_SET_KEY\"}");return;}
  const char*err=validate(next);if(err){emit("{\"event\":\"ERROR\",\"reason\":\"%s\"}",err);return;}
  if(!prepareUartWire(next))return;
  config=next;port.configure(config);hello();return;
 }
 if(!strcmp(w[0],"BAUD")&&n==2){uint32_t v;if(!number(w[1],v)||(v!=921600&&v!=2000000)){emit("{\"event\":\"ERROR\",\"reason\":\"BAD_BAUD\"}");return;}Config next=config;next.baud=v;if(validate(next)){emit("{\"event\":\"ERROR\",\"reason\":\"PERIOD_UART_BUDGET\"}");return;}
  if(!prepareUartWire(next))return;
  if(uart_set_baudrate(UART_NUM_0,v)!=ESP_OK){
   emit("{\"event\":\"ERROR\",\"reason\":\"UART_BAUD_FAILED\"}");return;
  }
  config=next;port.configure(config);hello();return;
 }
 if(!strcmp(w[0],"ANT")&&n==6){uint32_t i,tx,rx,id;if(!number(w[1],i)||!number(w[2],tx)||!number(w[3],rx)||!number(w[4],id)||i>=(DUAL8_ANCHOR?1U:2U)||tx>65535||rx>65535){emit("{\"event\":\"ERROR\",\"reason\":\"BAD_CALIBRATION\"}");return;}
  char*e=nullptr;double sigma=NAN;
  if(id||strcmp(w[5],"none")){
   sigma=strtod(w[5],&e);
   if(e==w[5]||*e||!isfinite(sigma)||sigma<=0||sigma>100){emit("{\"event\":\"ERROR\",\"reason\":\"BAD_SIGMA\"}");return;}
  }
  Calibration c;c.tx=uint16_t(tx);c.rx=uint16_t(rx);c.id=id;c.range_sigma_m=sigma;hwOk=port.setCalibration(i,c);hello();return;
 }
 if(!strcmp(w[0],"START")&&n==3){uint32_t s,count;if(!number(w[1],s)||!number(w[2],count)||!s||count<1||count>1000000||config.stage<2||validate(config)||!hwOk||port.faulted()||!drainClean){
   emit("{\"event\":\"ERROR\",\"reason\":\"START_GUARD\"}");return;}
#if DUAL8_ANCHOR
  if(const char*reason=port.verifyStart()){
   hwOk=false;emit("{\"event\":\"ERROR\",\"reason\":\"%s\"}",reason);return;
  }
  port.beginAnchorEdgeReport("response",s);
#endif
  dropsAtStart=drops.load();
  drainComplete=config.stage!=6||config.baud!=2000000;drainClean=drainComplete;
  session=s;remaining=count;targetCount=count;txn=0;peerBoot=0;periodMiss=0;queueScheduleSkips=0;writerMaxUs=0;nextStart=port.nowUs()+20000;runExpire=port.nowUs()+uint64_t(count)*config.period_us+10000000ULL;active=true;
  if(!DUAL8_ANCHOR){if(!port.arm(0)||!port.arm(1)){active=false;hwOk=false;idleAll();}}
  hello();return;
 }
 emit("{\"event\":\"ERROR\",\"reason\":\"UNKNOWN_COMMAND\"}");
}
#if DUAL8_ANCHOR
#include "operational_anchor.hpp"
#endif
static void service(){
 static char b[192];static size_t n=0;static bool overflow=false;
 uint8_t incoming[192];
 const int count=uart_read_bytes(UART_NUM_0,incoming,sizeof incoming,0);
 for(int i=0;i<count;++i){
  const char c=char(incoming[i]);
  if(c=='\n'){
   if(!overflow){
    b[n]=0;
#if DUAL8_ANCHOR
    if(!opsUsbCommand(b))
#endif
     command(b);
   }else emit("{\"event\":\"ERROR\",\"reason\":\"COMMAND_TOO_LONG\"}");
   n=0;overflow=false;
  }else if(c!='\r'){
   if(n+1<sizeof b)b[n++]=c;else overflow=true;
  }
 }
}
static void setup(){
 Serial.setRxBufferSize(2048);Serial.setTxBufferSize(0);Serial.begin(config.baud);delay(300);
 freeQ=xQueueCreate(POOL,sizeof(uint8_t));outQ=xQueueCreate(12,sizeof(Out));
 if(!freeQ||!outQ){Serial.println("FATAL_QUEUE");while(true)delay(1000);}
 for(unsigned i=0;i<POOL;++i){
  auto*& slot=i<BASE_POOL?basePool[i]:extraPool[i-BASE_POOL];
  slot=new(std::nothrow) Record();
  if(!slot){Serial.println("FATAL_RECORD_POOL");while(true)delay(1000);}
 }
 for(uint8_t i=0;i<POOL;++i)xQueueSend(freeQ,&i,0);
 if(xTaskCreatePinnedToCore(writer,"dual8_writer",8192,nullptr,1,nullptr,0)!=pdPASS){Serial.println("FATAL_WRITER");while(true)delay(1000);}
 boot=esp_random();if(!boot)boot=1;port.configure(config);hwOk=port.initialize();
#if DUAL8_ANCHOR
 opsSetup();
#endif
 hello();
}
static void loop(){
 service();serviceDrain();
#if DUAL8_ANCHOR
 opsLoop();
#endif
 if(!active){delay(1);return;}
 if(port.nowUs()>runExpire){emit("{\"event\":\"SESSION_DEADLINE\"}");stop();return;}
#if DUAL8_ANCHOR
 if(port.nowUs()<nextStart){port.relax();return;}
 const int idx=reserveAnchor(nextStart);
 if(idx<0){
  ++drops;++periodMiss;++queueScheduleSkips;
  emit("{\"event\":\"BACKPRESSURE_NO_RF\",\"session\":%lu,\"txn\":%lu,\"scheduled_us\":%llu,"
       "\"pool\":%u,\"awaiting_ack\":%u,\"writer_pending\":%lu,\"ack_queued\":%u,\"ack_retired\":%u}",
       (unsigned long)session,(unsigned long)txn,(unsigned long long)nextStart,POOL,awaitingAck.load(),
       (unsigned long)pending.load(),ackQueued.load(),ackRetired.load());
  nextStart+=config.period_us;
  return;
 }
 auto& record=recordAt(idx);
 runAnchor(port,config,session,txn++,boot,peerBoot,record);
 if(record.peer_boot)peerBoot=record.peer_boot;
 if(opsArchiveEnabled()){
  const bool archived=opsArchiveRecord(record);
  releaseRecord(uint8_t(idx));
  if(!archived){stop();return;}
  ++sent;
 }else submit(uint8_t(idx));
 if(port.faulted()&&port.anchorRxFault().phase)emitAnchorRxFault();
 nextStart+=config.period_us;if(port.nowUs()>=nextStart){++periodMiss;nextStart=port.nowUs()+config.period_us;}
 if(--remaining==0)stop();
#else
 for(uint8_t i=0;i<2;++i){Frame f;Rx o;const int rc=port.receive(i,f,o);
  if(rc==1){
   if(f.type==Type::Poll&&f.session==session&&f.config==configId(config)&&f.stage==config.stage&&f.txn>=txn&&f.txn<targetCount&&(!peerBoot||peerBoot==f.boot)){
    peerBoot=f.boot;txn=f.txn+1;int idx=reserve();if(idx<0){idleAll();emit("{\"event\":\"BACKPRESSURE_DROP_POLL\",\"txn\":%lu}",(unsigned long)f.txn);}
    else {
     runTag(port,config,boot,i,f,o,recordAt(idx));
     const uint64_t nextPoll=(o.hw+usTicks(config.period_us-(isRetryPoll(f)?config.probe1DelayUs():0)))&MASK40;
     if(config.stage==6&&!port.faulted()&&signed40(port.system(i),nextPoll)>=0)++periodMiss;
     submit(uint8_t(idx));
    }
    remaining=targetCount-txn;if(remaining==0){stop();break;}if(port.faulted()){active=false;hwOk=false;idleAll();break;}
    if(!port.arm(0)||!port.arm(1)){active=false;hwOk=false;idleAll();}break;
   }else if(!port.arm(i)){active=false;hwOk=false;idleAll();break;}
  }else if(rc==-1){if(!port.arm(i)){active=false;hwOk=false;idleAll();break;}}
  else if(rc==-2){active=false;hwOk=false;idleAll();break;}
 }
#endif
 if(port.faulted()){
  active=false;hwOk=false;idleAll();
#if DUAL8_ANCHOR
  if(port.takeAnchorEdgeReport())emitAnchorRxEdges();
  emit("{\"event\":\"HARDWARE_FAULT_LATCHED\",\"status_a\":%llu,\"status_b\":0,\"clock_fault_recorded\":%s,\"fault_rf\":%u,\"fault_ec_ctrl\":%u}",
       (unsigned long long)port.fault_status[0],port.clockFaultRecorded()?"true":"false",port.faultRf(),port.faultDetector());
#else
  emit("{\"event\":\"HARDWARE_FAULT_LATCHED\",\"status_a\":%llu,\"status_b\":%llu}",
       (unsigned long long)port.fault_status[0],(unsigned long long)port.fault_status[1]);
#endif
 }
 port.relax();
}
} // namespace dual8app
void setup(){dual8app::setup();}
void loop(){dual8app::loop();}
