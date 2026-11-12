#pragma once
#include "esp32_port.hpp"
#include "record_json.hpp"
#include "uart_fragment.hpp"
#include <esp_system.h>
#include <driver/uart.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <atomic>
#include <new>
#include <stdlib.h>
namespace dual8app {
using namespace dual8;
static Esp32Port port;static Config config;
// Two extra tag slots cover transient write/ACK latency at the RF cadence.
static constexpr unsigned POOL=DUAL8_ANCHOR?3:5;
static constexpr unsigned BASE_POOL=3;
static Record basePool[BASE_POOL];
static Record* extraPool[2]{};
static Record& recordAt(unsigned i){return i<BASE_POOL?basePool[i]:*extraPool[i-BASE_POOL];}
static QueueHandle_t freeQ=nullptr,outQ=nullptr;
constexpr unsigned wireStride=UART_FRAGMENT_HEADER+UART_FRAGMENT_BYTES;
constexpr unsigned maxRawBytes=DUAL8_ANCHOR?0:4*CIR_BYTES;
constexpr unsigned maxWireData=(24+JSON_MAX+maxRawBytes+UART_FRAGMENT_BYTES-1)/UART_FRAGMENT_BYTES;
constexpr unsigned wireBytes=(maxWireData+uartParityCount(maxWireData))*wireStride+3*UART_FRAGMENT_HEADER;
static_assert(maxWireData<=UART_FRAGMENT_MAX_DATA,"RFB8 fragment capacity");
static uint8_t* uartWire=nullptr;
#if !DUAL8_ANCHOR
static Probe2RepeatCapture* probe2Repeat=nullptr;
#endif
enum class WriterAction:uint8_t { RecordOrEvent, Ack, Nack };
struct Out {
 uint8_t index=255;
 bool fragmented=false;
 WriterAction action=WriterAction::RecordOrEvent;
 uint32_t sequence=0, session=0;
 char text[768]{};
};
struct ReplaySlot { bool occupied=false; uint32_t sequence=0; uint8_t attempts=0; uint64_t expires=0; };
constexpr uint64_t REPLAY_TIMEOUT_US=1500000;
constexpr unsigned REPLAY_LIMIT=2;
static std::atomic<unsigned> awaitingAck{0},retransmissions{0};
static std::atomic<unsigned> ackQueued{0},ackRetired{0},peakAwaitingAck{0};
static std::atomic<uint32_t> pending{0},drops{0},sent{0},writerMaxUs{0};
static uint32_t boot=0,session=0,txn=0,peerBoot=0,remaining=0,periodMiss=0;static uint64_t nextStart=0;
static uint64_t runExpire=0;static uint32_t targetCount=0;
static bool active=false,hwOk=false;static uint32_t recordSequence=0;
static const char*role(){return DUAL8_ANCHOR?"anchor":"tag";}
static void emit(const char*fmt,...) __attribute__((format(printf,1,2)));
static void emit(const char*fmt,...){
 if(!outQ){return;}
 Out o;o.fragmented=config.stage==6&&config.baud==2000000;va_list a;va_start(a,fmt);int n=vsnprintf(o.text,sizeof o.text,fmt,a);va_end(a);
 if(n<0||size_t(n)>=sizeof o.text){++drops;return;}++pending;if(xQueueSend(outQ,&o,0)!=pdTRUE){--pending;++drops;}
}
static bool prepareUartWire(const Config& next){
 if(next.stage!=6||next.baud!=2000000||uartWire)return true;
 uartWire=static_cast<uint8_t*>(malloc(wireBytes+maxRawBytes));
 if(uartWire)return true;
 emit("{\"event\":\"ERROR\",\"reason\":\"UART_RECORD_BUFFER_MEMORY\"}");
 return false;
}
static bool writeAll(const uint8_t*p,size_t n,bool bounded){
 constexpr size_t chunk=128;
 const uint64_t deadline=uint64_t(esp_timer_get_time())+REPLAY_TIMEOUT_US;
 while(n){
  if(uint64_t(esp_timer_get_time())>=deadline)return false;
  const size_t limit=bounded&&n>chunk?chunk:n;
  const size_t w=Serial.write(p,limit);
  if(!w){vTaskDelay(1);continue;}
  p+=w;n-=w;
 }
 return uint64_t(esp_timer_get_time())<deadline;
}
static void encodeFragment(uint8_t*h,uint32_t seq,unsigned index,unsigned dataCount,unsigned size,bool data){
 h[0]='R';h[1]='8';h[2]='F';h[3]='6';
 putLE(h+4,seq,4);h[8]=uint8_t(index);h[9]=uint8_t(dataCount);putLE(h+10,size,2);
 uint32_t crc=crcStep(0xffffffff,h,12);
 if(data)crc=crcStep(crc,h+UART_FRAGMENT_HEADER,UART_FRAGMENT_BYTES);
 putLE(h+12,crc^0xffffffff,4);
}
static bool sendFragments(uint32_t seq,const uint8_t*header,const uint8_t*meta,size_t nmeta,const uint8_t*raw,size_t nraw){
 static const UartField field;
 constexpr unsigned stride=wireStride;
 uint8_t* const wire=uartWire;
 if(!wire||nraw>maxRawBytes)return false;
 const size_t packedBytes=nraw?packUartIq(raw,nraw,wire+wireBytes):0;
 if(packedBytes){raw=wire+wireBytes;nraw=packedBytes;}
 const unsigned bytes=24+nmeta+nraw,dataCount=(bytes+UART_FRAGMENT_BYTES-1)/UART_FRAGMENT_BYTES;
 const unsigned sizeField=bytes|(packedBytes?UART_PACKED_IQ_FLAG:0);
 if(dataCount>maxWireData)return false;
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
  encodeFragment(fragment,seq,i,dataCount,sizeField,true);
 }
 for(unsigned j=dataCount;j<count;++j)encodeFragment(wire+j*stride,seq,j,dataCount,sizeField,true);
 // Submit the whole record to UART once, including the redundant end markers.
 for(unsigned j=0;j<3;++j)encodeFragment(wire+count*stride+j*UART_FRAGMENT_HEADER,seq,UART_FRAGMENT_END,dataCount,sizeField,false);
 return writeAll(wire,count*stride+3*UART_FRAGMENT_HEADER,false);
}
static bool wireAt(uint32_t sequence,uint8_t kind,const char*json,size_t size,const uint8_t*raw,size_t nraw,bool bounded,bool fragmented){
 uint8_t h[24]{};putLE(h,WIRE_MAGIC,4);h[4]=1;h[5]=kind;putLE(h+6,24,2);putLE(h+8,sequence,4);putLE(h+12,size,4);putLE(h+16,nraw,4);
 uint32_t c=crcStep(0xffffffff,json,size);if(nraw)c=crcStep(c,raw,nraw);putLE(h+20,c^0xffffffff,4);
 if(fragmented)return sendFragments(sequence,h,reinterpret_cast<const uint8_t*>(json),size,raw,nraw);
 return writeAll(h,24,bounded)&&
  writeAll(reinterpret_cast<const uint8_t*>(json),size,bounded)&&
  (!nraw||writeAll(raw,nraw,bounded));
}
static bool wire(uint8_t kind,const char*json,size_t size,const uint8_t*raw,size_t nraw,bool bounded=false,bool fragmented=false){
 return wireAt(recordSequence++,kind,json,size,raw,nraw,bounded,fragmented);
}
static bool writeRecord(Record& r,char* json,uint32_t sequence){
 size_t n=0;
 return serializeRecord(r,json,JSON_MAX+1,n)&&
  wireAt(sequence,2,json,n,r.anchor?nullptr:&r.cir[0][0],r.anchor?0:4*CIR_BYTES,
   r.cfg.stage==6,r.cfg.stage==6&&r.cfg.baud==2000000);
}
static void releaseRecord(uint8_t i){xQueueSend(freeQ,&i,portMAX_DELAY);}
static void retireReplay(ReplaySlot& slot,uint8_t i){
 slot.occupied=false;--awaitingAck;releaseRecord(i);
}
static void writerError(const char* reason,bool fragmented,uint32_t seq,uint32_t txn){
 ++drops;
 char event[152];
 const int n=snprintf(event,sizeof event,"{\"event\":\"ERROR\",\"reason\":\"%s\",\"sequence\":%lu,\"txn\":%lu}",
  reason,(unsigned long)seq,(unsigned long)txn);
 if(n>0&&size_t(n)<sizeof event)wire(1,event,size_t(n),nullptr,0,false,fragmented);
}
static void writer(void*){
 static char json[JSON_MAX+1];Out o;ReplaySlot cache[POOL]{};
 for(;;){
  const bool queued=xQueueReceive(outQ,&o,pdMS_TO_TICKS(10))==pdTRUE;
  const uint64_t now=uint64_t(esp_timer_get_time());
  for(uint8_t i=0;i<POOL;++i)if(cache[i].occupied&&now>=cache[i].expires){
   const uint32_t seq=cache[i].sequence,txn=recordAt(i).txn;
   retireReplay(cache[i],i);
   writerError("RECORD_ACK_TIMEOUT",true,seq,txn);
  }
  if(!queued)continue;
  if(o.action!=WriterAction::RecordOrEvent){
   for(uint8_t i=0;i<POOL;++i){
    auto& slot=cache[i];
    if(!slot.occupied||slot.sequence!=o.sequence||recordAt(i).session!=o.session)continue;
    if(o.action==WriterAction::Ack){retireReplay(slot,i);++ackRetired;}
    else if(slot.attempts>=REPLAY_LIMIT||!writeRecord(recordAt(i),json,slot.sequence)){
     const uint32_t txn=recordAt(i).txn;
     retireReplay(slot,i);writerError("RECORD_REPLAY_EXHAUSTED",true,o.sequence,txn);
    }else {++slot.attempts;++retransmissions;}
    break;
   }
  }else if(o.index==255){
   if(!wire(1,o.text,strlen(o.text),nullptr,0,false,o.fragmented))++drops;
  }else{
   const uint8_t i=o.index;Record& r=recordAt(i);
   const uint32_t started=uint32_t(esp_timer_get_time()),sequence=recordSequence++;
   if(writeRecord(r,json,sequence)){
    ++sent;
    if(r.cfg.stage==6&&r.cfg.baud==2000000){
     auto& slot=cache[i];
     slot.occupied=true;slot.sequence=sequence;slot.attempts=0;
     slot.expires=uint64_t(esp_timer_get_time())+REPLAY_TIMEOUT_US;
     const unsigned count=++awaitingAck;
     if(count>peakAwaitingAck.load())peakAwaitingAck=count;
    }else releaseRecord(i);
   }else{
    const bool fragmented=r.cfg.stage==6&&r.cfg.baud==2000000;
    const uint32_t txn=r.txn;
    releaseRecord(i);writerError("SERIALIZE_OVERFLOW",fragmented,sequence,txn);
   }
   const uint32_t elapsed=uint32_t(esp_timer_get_time())-started;
   if(elapsed>writerMaxUs)writerMaxUs=elapsed;
  }
  --pending;
 }
}
static int reserve(){uint8_t i;if(xQueueReceive(freeQ,&i,0)!=pdTRUE){++drops;return -1;}new(&recordAt(i))Record();return i;}
static void submit(uint8_t i){
 const unsigned n=DUAL8_ANCHOR?1:2;for(unsigned j=0;j<n;++j)recordAt(i).cal[j]=port.calibration(j);
 Out o;o.index=i;++pending;if(xQueueSend(outQ,&o,0)!=pdTRUE){--pending;++drops;xQueueSend(freeQ,&i,0);}
}
static void idleAll(){port.idle(0);if(!DUAL8_ANCHOR)port.idle(1);}
static bool number(const char*s,uint32_t&v){
 if(!s||!*s||*s=='-'||*s=='+'){return false;}
 char*end;unsigned long long n=strtoull(s,&end,10);if(*end||n>0xffffffffULL)return false;v=uint32_t(n);return true;
}
static void hello(){emit("{\"event\":\"HELLO\",\"role\":\"%s\",\"firmware\":\"%s\",\"mode\":8,\"wire\":\"RFB8\",\"record_replay\":1,\"record_retransmissions\":%lu,\"hardware_profile\":\"%s\",\"hw_ok\":%s,\"fault_latched\":%s,\"boot\":%lu,\"config_id\":%lu,\"stage\":%u,\"period_us\":%lu,\"baud\":%lu,\"active\":%s,\"wifi_ota\":false,\"cal_default_is_uncalibrated\":true,\"drops\":%lu,\"sent\":%lu,\"period_miss\":%lu,\"writer_max_us\":%lu,\"queue_pool\":%u,\"queue_ack_queued\":%u,\"queue_ack_retired\":%u,\"queue_peak_awaiting_ack\":%u}",role(),firmwareId(DUAL8_ANCHOR),(unsigned long)retransmissions.load(),DUAL8_ANCHOR?"esp32-dw1000-anchor-21-22-5":"esp32-dw1000-tag-ab-21-22-17-25-26-27",hwOk?"true":"false",port.faulted()?"true":"false",(unsigned long)boot,(unsigned long)configId(config),config.stage,(unsigned long)config.period_us,(unsigned long)config.baud,active?"true":"false",(unsigned long)drops.load(),(unsigned long)sent.load(),(unsigned long)periodMiss,(unsigned long)writerMaxUs.load(),POOL,ackQueued.load(),ackRetired.load(),peakAwaitingAck.load());}
static void formatFault(uint8_t radio,char (&out)[256]){
 const auto& f=port.registerFault(radio);
 if(!f.check){strcpy(out,"null");return;}
 snprintf(out,sizeof out,"{\"check\":\"%s\",\"reg\":%u,\"offset\":%d,\"bytes\":%u,\"txn\":%ld,\"at_us\":%llu,\"expected\":%llu,\"first\":%llu,\"reread\":%llu,\"reread_2mhz\":%llu}",
          f.check,unsigned(f.reg),f.offset==0xffff?-1:int(f.offset),unsigned(f.bytes),(long)f.txn,
          (unsigned long long)f.at_us,(unsigned long long)f.expected,(unsigned long long)f.first,
          (unsigned long long)f.reread,(unsigned long long)f.reread_2mhz);
}
#if !DUAL8_ANCHOR
static const char* clockInitTrigger="boot";
static const char* const clockStepNames[]={
 "before_reset","reset_released","xti_selected","detector_enabled","trim_written",
 "ldo_loaded","lde_loaded","lde_auto","lde_done","before_final_auto","final_auto","final_settled"
};
static_assert(sizeof clockStepNames/sizeof clockStepNames[0]==DW1000Ng::ClockInitStepCount,"clock step labels");
#endif
static void emitAntennaInit(const char* trigger){
#if !DUAL8_ANCHOR
 clockInitTrigger=trigger;
 const auto& trace=port.antennaInitA();
 const auto& cal=port.calibration(0);
 emit("{\"event\":\"ANT_INIT\",\"role\":\"tag\",\"boot\":%lu,\"radio\":0,\"trigger\":\"%s\",\"spi_hz\":20000000,"
      "\"set_tx\":{\"tx_written\":%u,\"tx_read\":%u,\"rx_written\":%u,\"rx_read\":%u},"
      "\"set_rx\":{\"tx_written\":%u,\"tx_read\":%u,\"rx_written\":%u,\"rx_read\":%u},"
      "\"first_guard\":{\"tx_expected\":%u,\"tx_read\":%u,\"rx_expected\":%u,\"rx_read\":%u}}",
      (unsigned long)boot,trigger,
      unsigned(trace.set_tx.tx_written),unsigned(trace.set_tx.tx_read),unsigned(trace.set_tx.rx_written),unsigned(trace.set_tx.rx_read),
      unsigned(trace.set_rx.tx_written),unsigned(trace.set_rx.tx_read),unsigned(trace.set_rx.rx_written),unsigned(trace.set_rx.rx_read),
      unsigned(cal.tx),unsigned(trace.first_guard.tx),unsigned(cal.rx),unsigned(trace.first_guard.rx));
 for(uint8_t i=0;i<2;++i){
  const auto& vendor=port.vendorInitClock(i);
  const auto& before=port.preAckClock(i);
  const auto& after=port.settledClock(i);
  const auto& configured=port.configuredClock(i);
  emit("{\"event\":\"PLL_INIT\",\"boot\":%lu,\"trigger\":\"%s\",\"radio\":%u,"
       "\"vendor_init\":[%llu,%u,%lu],\"pre_ack\":[%llu,%u,%lu],"
       "\"acknowledged\":%s,\"settled\":[%llu,%u,%lu],\"configured\":[%llu,%u,%lu]}",
       (unsigned long)boot,trigger,i,
       (unsigned long long)vendor.status,vendor.rf_status,(unsigned long)vendor.detector,
       (unsigned long long)before.status,before.rf_status,(unsigned long)before.detector,
       port.startupClockAcknowledged(i)?"true":"false",
       (unsigned long long)after.status,after.rf_status,(unsigned long)after.detector,
       (unsigned long long)configured.status,configured.rf_status,(unsigned long)configured.detector);
 }
#else
 (void)trigger;
#endif
}
static void stop(uint32_t request=0){
 active=false;remaining=0;idleAll();
 if(!DUAL8_ANCHOR&&port.faulted()){
  char a[256],b[256];formatFault(0,a);formatFault(1,b);
  emit("{\"event\":\"STOPPED\",\"role\":\"tag\",\"session\":%lu,\"stop_request\":%lu,\"physical_rf_off_confirmed\":false,\"fault_a\":%s,\"fault_b\":%s}",
       (unsigned long)session,(unsigned long)request,a,b);
 }else emit("{\"event\":\"STOPPED\",\"role\":\"%s\",\"session\":%lu,\"stop_request\":%lu,\"physical_rf_off_confirmed\":false}",
            role(),(unsigned long)session,(unsigned long)request);
}
static void command(char*line){
 char*save=nullptr;char*w[9]{};unsigned n=0;for(char*p=strtok_r(line," \t\r",&save);p;p=strtok_r(nullptr," \t\r",&save)){if(n==9){emit("{\"event\":\"ERROR\",\"reason\":\"TOO_MANY_TOKENS\"}");return;}w[n++]=p;}if(!n)return;
 if(!strcmp(w[0],"REC_ACK")||!strcmp(w[0],"REC_NACK")){
  uint32_t b,s,seq;
  if(n!=4||!number(w[1],b)||!number(w[2],s)||!number(w[3],seq)||
     b!=boot||!s||s!=session||config.stage!=6||config.baud!=2000000)return;
  Out o;o.action=!strcmp(w[0],"REC_ACK")?WriterAction::Ack:WriterAction::Nack;
  o.session=s;o.sequence=seq;++pending;
  // A durable ACK releases a record slot. Handle it before queued serial writes.
  const bool ack=o.action==WriterAction::Ack;
  const auto accepted=ack?xQueueSendToFront(outQ,&o,0):xQueueSend(outQ,&o,0);
  if(accepted!=pdTRUE){--pending;++drops;}
  else if(ack)++ackQueued;
  return;
 }
 if(!strcmp(w[0],"HELLO")||!strcmp(w[0],"STATUS")){hello();return;}
 if(!strcmp(w[0],"STOP")){
  uint32_t request=0;
  if(n==1||(n==2&&number(w[1],request)&&request))stop(request);
  else emit("{\"event\":\"ERROR\",\"reason\":\"BAD_STOP_REQUEST\"}");
  return;
 }
 if(!strcmp(w[0],"HELP")){emit("{\"event\":\"HELP\",\"commands\":\"HELLO; SELFTEST; SET STAGE|PERIOD_US|CIR_HZ|VERIFY_CIR value; BAUD 921600|2000000; ANT radio tx rx cal_id sigma_m; START session count; STOP [request]; RESET%s\"}",DUAL8_ANCHOR?"":"; DEVID 0|1; PLLSTEP radio index");return;}
 if(active){emit("{\"event\":\"ERROR\",\"reason\":\"STOP_REQUIRED\"}");return;}
#if !DUAL8_ANCHOR
 if(!strcmp(w[0],"PLLSTEP")){
  uint32_t i,k;
  if(n!=3||!number(w[1],i)||i>=2||!number(w[2],k)||k>=DW1000Ng::ClockInitStepCount){
   emit("{\"event\":\"ERROR\",\"reason\":\"BAD_PLL_STEP\"}");return;
  }
  const auto& s=port.clockInitStep(i,k);
  const auto& ldo=port.clockInitLdo(i);
  emit("{\"event\":\"PLL_INIT_STEP\",\"boot\":%lu,\"trigger\":\"%s\",\"radio\":%u,\"index\":%u,\"step\":\"%s\","
       "\"status\":%llu,\"rf_status\":%u,\"ec_ctrl\":%lu,\"pmsc_ctrl0\":%lu,\"xtal_trim\":%u,\"dev_id\":%lu,"
       "\"ldo_otp\":%lu,\"ldo_before\":%lu,\"ldo_after\":%lu,\"ldo_kicked\":%s}",
       (unsigned long)boot,clockInitTrigger,unsigned(i),unsigned(k),clockStepNames[k],
       (unsigned long long)s.status,unsigned(s.rf_status),(unsigned long)s.ec_ctrl,
       (unsigned long)s.pmsc_ctrl0,unsigned(s.xtal_trim),(unsigned long)s.dev_id,
       (unsigned long)ldo.otp,(unsigned long)ldo.before,(unsigned long)ldo.after,ldo.kicked?"true":"false");
  return;
 }
 if(!strcmp(w[0],"DEVID")){
  uint32_t i;if(n!=2||!number(w[1],i)||i>=2){emit("{\"event\":\"ERROR\",\"reason\":\"BAD_RADIO\"}");return;}
  const uint32_t id=port.readDeviceId(uint8_t(i));
  emit("{\"event\":\"DEVID\",\"role\":\"tag\",\"radio\":%u,\"boot\":%lu,\"spi_hz\":20000000,\"dev_id\":%lu,\"matches\":%s}",
       unsigned(i),(unsigned long)boot,(unsigned long)id,id==0xdeca0130UL?"true":"false");return;
 }
 if(!strcmp(w[0],"RXSTATUS")&&n==2){
  uint32_t i;if(!number(w[1],i)||i>=2){emit("{\"event\":\"ERROR\",\"reason\":\"BAD_RADIO\"}");return;}
  const auto& e=port.rxEvidence[i];
  emit("{\"event\":\"RXSTATUS\",\"role\":\"tag\",\"radio\":%u,\"boot\":%lu,\"session\":%lu,"
       "\"arms\":%lu,\"first_state\":%llu,\"status_or\":%llu,\"last_error_status\":%llu,"
       "\"polls\":%lu,\"probes\":%lu,\"failures\":%lu,\"bad_length\":%lu,\"bad_frame\":%lu,\"last_length\":%u,"
       "\"delay_rechecks\":%lu,\"delay_recovered\":%lu,\"last_ant_rx\":%u,\"fault_status\":%llu,"
       "\"sys_cfg\":%lu,\"chan_ctrl\":%lu,\"tx_fctrl\":%lu,\"drx_tune0b\":%u,\"drx_tune1a\":%u,"
       "\"drx_tune1b\":%u,\"drx_tune2\":%lu,\"lde_repc\":%u}",
       unsigned(i),(unsigned long)boot,(unsigned long)session,(unsigned long)e.arms,
       (unsigned long long)e.first_state,(unsigned long long)e.status_or,(unsigned long long)e.last_error_status,
       (unsigned long)e.polls,(unsigned long)e.probes,(unsigned long)e.failures,(unsigned long)e.bad_length,
       (unsigned long)e.bad_frame,unsigned(e.last_length),(unsigned long)e.delay_rechecks,
       (unsigned long)e.delay_recovered,unsigned(port.last_ant_rx[i]),(unsigned long long)port.fault_status[i],
       (unsigned long)port.rxRegister(i,0x04,0xffff,4),
       (unsigned long)port.rxRegister(i,0x1f,0xffff,4),(unsigned long)port.rxRegister(i,0x08,0xffff,4),
       unsigned(port.rxRegister(i,0x27,2,2)),unsigned(port.rxRegister(i,0x27,4,2)),unsigned(port.rxRegister(i,0x27,6,2)),
       (unsigned long)port.rxRegister(i,0x27,8,4),unsigned(port.rxRegister(i,0x2e,0x2804,2)));
  return;
 }
#endif
 if(!strcmp(w[0],"RESET")&&n==1){idleAll();hwOk=port.initialize();hello();emitAntennaInit("reset");return;}
 if(!strcmp(w[0],"SELFTEST")&&n==1){hwOk=port.selftest();emit("{\"event\":\"SELFTEST\",\"role\":\"%s\",\"pass\":%s,\"faults\":%lu,\"dev_a\":%lu,\"dev_b\":%lu,\"ant_a_tx\":%u,\"ant_a_rx\":%u,\"ant_b_tx\":%u,\"ant_b_rx\":%u}",role(),hwOk?"true":"false",(unsigned long)port.faults,(unsigned long)port.last_dev[0],(unsigned long)port.last_dev[1],port.last_ant_tx[0],port.last_ant_rx[0],port.last_ant_tx[1],port.last_ant_rx[1]);return;}
 if(!strcmp(w[0],"SET")&&n==3){uint32_t v;if(!number(w[2],v)){emit("{\"event\":\"ERROR\",\"reason\":\"BAD_INTEGER\"}");return;}Config next=config;
  if(!strcmp(w[1],"STAGE")){if(v<1||v>6)return;next.stage=uint8_t(v);}
  else if(!strcmp(w[1],"PERIOD_US"))next.period_us=v;
  else if(!strcmp(w[1],"CIR_HZ"))next.cir_hz=v;
  else if(!strcmp(w[1],"VERIFY_CIR")){if(v>1)return;next.verify_cir=v!=0;}
  else {emit("{\"event\":\"ERROR\",\"reason\":\"UNKNOWN_SET_KEY\"}");return;}
  const char*err=validate(next);if(err){emit("{\"event\":\"ERROR\",\"reason\":\"%s\"}",err);return;}
  if(!prepareUartWire(next))return;
  config=next;port.configure(config);hello();return;
 }
 if(!strcmp(w[0],"BAUD")&&n==2){uint32_t v;if(!number(w[1],v)||(v!=921600&&v!=2000000)){emit("{\"event\":\"ERROR\",\"reason\":\"BAD_BAUD\"}");return;}Config next=config;next.baud=v;if(validate(next)){emit("{\"event\":\"ERROR\",\"reason\":\"PERIOD_UART_BUDGET\"}");return;}
  if(!prepareUartWire(next))return;
  const uint64_t timeout=port.nowUs()+2000000;
  while(pending.load()&&port.nowUs()<timeout)delay(1);
  if(pending.load()||awaitingAck.load()){emit("{\"event\":\"ERROR\",\"reason\":\"RECORD_DRAIN_PENDING\"}");return;}
  Serial.flush();Serial.end();
  Serial.setTxBufferSize(8192);Serial.begin(v);config=next;port.configure(config);hello();return;
 }
 if(!strcmp(w[0],"ANT")&&n==6){uint32_t i,tx,rx,id;if(!number(w[1],i)||!number(w[2],tx)||!number(w[3],rx)||!number(w[4],id)||i>=(DUAL8_ANCHOR?1U:2U)||tx>65535||rx>65535){emit("{\"event\":\"ERROR\",\"reason\":\"BAD_CALIBRATION\"}");return;}
  char*e=nullptr;double sigma=NAN;
  if(id||strcmp(w[5],"none")){
   sigma=strtod(w[5],&e);
   if(e==w[5]||*e||!isfinite(sigma)||sigma<=0||sigma>100){emit("{\"event\":\"ERROR\",\"reason\":\"BAD_SIGMA\"}");return;}
  }
  Calibration c;c.tx=uint16_t(tx);c.rx=uint16_t(rx);c.id=id;c.range_sigma_m=sigma;hwOk=port.setCalibration(i,c);hello();return;
 }
 if(!strcmp(w[0],"START")&&n==3){uint32_t s,count;if(!number(w[1],s)||!number(w[2],count)||!s||count<1||count>1000000||config.stage<2||validate(config)||!hwOk||port.faulted()||awaitingAck.load()){
   emit("{\"event\":\"ERROR\",\"reason\":\"START_GUARD\"}");return;}
  session=s;remaining=count;targetCount=count;txn=0;peerBoot=0;periodMiss=0;writerMaxUs=0;nextStart=port.nowUs()+20000;runExpire=port.nowUs()+uint64_t(count)*config.period_us+10000000ULL;active=true;
#if !DUAL8_ANCHOR
  port.clearRxEvidence();
#endif
#if !DUAL8_ANCHOR
  StartClockTrace clock;
  const bool armed=port.beginStartClockTrace(clock)&&port.arm(0)&&port.arm(1);
  port.endStartClockTrace();
  if(!armed||!port.checkStartClocks(clock.after_arms)){active=false;hwOk=false;idleAll();}
  for(uint8_t i=0;i<2;++i){
   const auto& vendor=port.vendorInitClock(i);
   const auto& configured=port.configuredClock(i);
   const auto& pre=clock.before_start[i];
   const auto& rx0=clock.before_rx[i];
   const auto& rx1=clock.after_rx[i];
   const auto& post=clock.after_arms[i];
   emit("{\"event\":\"START_PLL_TRACE\",\"boot\":%lu,\"session\":%lu,\"radio\":%u,"
        "\"vendor_init\":[%llu,%u,%lu],\"configured\":[%llu,%u,%lu],"
        "\"before_start\":[%llu,%u,%lu],\"before_rx\":[%llu,%u,%lu],"
        "\"after_rx\":[%llu,%u,%lu],\"after_arms\":[%llu,%u,%lu]}",
        (unsigned long)boot,(unsigned long)session,i,
        (unsigned long long)vendor.status,vendor.rf_status,(unsigned long)vendor.detector,
        (unsigned long long)configured.status,configured.rf_status,(unsigned long)configured.detector,
        (unsigned long long)pre.status,pre.rf_status,(unsigned long)pre.detector,
        (unsigned long long)rx0.status,rx0.rf_status,(unsigned long)rx0.detector,
        (unsigned long long)rx1.status,rx1.rf_status,(unsigned long)rx1.detector,
        (unsigned long long)post.status,post.rf_status,(unsigned long)post.detector);
  }
#endif
  hello();return;
 }
 emit("{\"event\":\"ERROR\",\"reason\":\"UNKNOWN_COMMAND\"}");
}
static void service(){
 static char b[192];static size_t n=0;static bool overflow=false;
 uint8_t incoming[192];
 const int count=uart_read_bytes(UART_NUM_0,incoming,sizeof incoming,0);
 for(int i=0;i<count;++i){
  const char c=char(incoming[i]);
  if(c=='\n'){
   if(!overflow){b[n]=0;command(b);}else emit("{\"event\":\"ERROR\",\"reason\":\"COMMAND_TOO_LONG\"}");
   n=0;overflow=false;
  }else if(c!='\r'){
   if(n+1<sizeof b)b[n++]=c;else overflow=true;
  }
 }
}
static void setup(){
 Serial.setRxBufferSize(2048);Serial.setTxBufferSize(8192);Serial.begin(config.baud);delay(300);
 freeQ=xQueueCreate(POOL,sizeof(uint8_t));outQ=xQueueCreate(12,sizeof(Out));
 if(!freeQ||!outQ){Serial.println("FATAL_QUEUE");while(true)delay(1000);}
 // Allocate the extra records before RF starts. The ESP32 static DRAM
 // segment cannot hold five full records.
 for(unsigned i=BASE_POOL;i<POOL;++i){
  auto*& slot=extraPool[i-BASE_POOL];
  slot=new(std::nothrow) Record();
  if(!slot){Serial.println("FATAL_RECORD_POOL");while(true)delay(1000);}
 }
#if !DUAL8_ANCHOR
 probe2Repeat=new(std::nothrow) Probe2RepeatCapture;
 if(!probe2Repeat){Serial.println("FATAL_P2_REPEAT_SCRATCH");while(true)delay(1000);}
#endif
 for(uint8_t i=0;i<POOL;++i)xQueueSend(freeQ,&i,0);
 if(xTaskCreatePinnedToCore(writer,"dual8_writer",8192,nullptr,1,nullptr,0)!=pdPASS){Serial.println("FATAL_WRITER");while(true)delay(1000);}
 boot=esp_random();if(!boot)boot=1;port.configure(config);hwOk=port.initialize();hello();emitAntennaInit("boot");
}
static void loop(){
 service();if(!active){delay(1);return;}
 if(port.nowUs()>runExpire){
#if DUAL8_ANCHOR
  emit("{\"event\":\"SESSION_DEADLINE\"}");
#else
  emit("{\"event\":\"SESSION_DEADLINE\",\"role\":\"tag\",\"boot\":%lu,\"session\":%lu,\"next_txn\":%lu,\"target\":%lu,\"polls_a\":%lu,\"polls_b\":%lu}",
       (unsigned long)boot,(unsigned long)session,(unsigned long)txn,(unsigned long)targetCount,
       (unsigned long)port.rxEvidence[0].polls,(unsigned long)port.rxEvidence[1].polls);
#endif
  stop();return;
 }
#if DUAL8_ANCHOR
 if(port.nowUs()<nextStart){port.relax();return;}
 int idx=reserve();
 if(idx<0){nextStart=port.nowUs()+config.period_us;emit("{\"event\":\"BACKPRESSURE_NO_RF\"}");return;}
 runAnchor(port,config,session,txn++,boot,peerBoot,recordAt(idx));if(recordAt(idx).peer_boot)peerBoot=recordAt(idx).peer_boot;submit(uint8_t(idx));
 nextStart+=config.period_us;if(port.nowUs()>=nextStart){++periodMiss;nextStart=port.nowUs()+config.period_us;}
 if(--remaining==0)stop();
#else
 for(uint8_t i=0;i<2;++i){Frame f;Rx o;const int rc=port.receive(i,f,o);
  if(rc==1){
   if(f.type==Type::Poll&&f.session==session&&f.config==configId(config)&&f.stage==config.stage&&f.txn>=txn&&f.txn<targetCount&&(!peerBoot||peerBoot==f.boot)&&(!isRetryPoll(f)||!f.peer_boot||f.peer_boot==boot)){
    peerBoot=f.boot;txn=f.txn+1;int idx=reserve();if(idx<0){
     const unsigned held=awaitingAck.load(),queued=ackQueued.load(),retired=ackRetired.load();
     const uint32_t backlog=pending.load();
     idleAll();
     emit("{\"event\":\"BACKPRESSURE_DROP_POLL\",\"txn\":%lu,\"pool\":%u,\"awaiting_ack\":%u,\"writer_pending\":%lu,\"ack_queued\":%u,\"ack_retired\":%u}",
          (unsigned long)f.txn,POOL,held,(unsigned long)backlog,queued,retired);
    }
    else {
     if(config.stage==6)port.beginRegisterTrace(f.txn);
     runTag(port,config,boot,i,f,o,recordAt(idx),probe2Repeat);
     if(config.stage==6)port.finishRegisterTrace(recordAt(idx));
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
  char a[256],b[256];formatFault(0,a);formatFault(1,b);
  emit("{\"event\":\"HARDWARE_FAULT_LATCHED\",\"session\":%lu,\"boot\":%lu,\"next_txn\":%lu,\"status_a\":%llu,\"status_b\":%llu,\"fault_a\":%s,\"fault_b\":%s}",
       (unsigned long)session,(unsigned long)boot,(unsigned long)txn,
       (unsigned long long)port.fault_status[0],(unsigned long long)port.fault_status[1],a,b);
 }
 port.relax();
}
} // namespace dual8app
void setup(){dual8app::setup();}
void loop(){dual8app::loop();}
