#line 1 "D:\\SLAM_bot\\artifacts\\UWB_MODE8_R5_TARGET_20261111\\UWB_DUAL_DW1000_MODE8_R5_20261111\\firmware\\ANCHOR_DUAL_DS_CIR\\app.hpp"
#pragma once
#include "esp32_port.hpp"
#include "record_json.hpp"
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <atomic>
#include <new>
#include <stdlib.h>
namespace dual8app {
using namespace dual8;
static Esp32Port port;static Config config;static constexpr unsigned POOL=3;
static Record pool[POOL];static QueueHandle_t freeQ=nullptr,outQ=nullptr;
struct Out {uint8_t index=255;char text[768]{};};
static std::atomic<uint32_t> pending{0},drops{0},sent{0};
static uint32_t boot=0,session=0,txn=0,peerBoot=0,remaining=0,periodMiss=0;static uint64_t nextStart=0;
static uint64_t runExpire=0;static uint32_t targetCount=0;
static bool active=false,hwOk=false;static uint32_t recordSequence=0;
static const char*role(){return DUAL8_ANCHOR?"anchor":"tag";}
static void emit(const char*fmt,...) __attribute__((format(printf,1,2)));
static void emit(const char*fmt,...){
 if(!outQ){return;}
 Out o;va_list a;va_start(a,fmt);int n=vsnprintf(o.text,sizeof o.text,fmt,a);va_end(a);
 if(n<0||size_t(n)>=sizeof o.text){++drops;return;}++pending;if(xQueueSend(outQ,&o,0)!=pdTRUE){--pending;++drops;}
}
static void writeAll(const uint8_t*p,size_t n){while(n){size_t w=Serial.write(p,n);if(!w){vTaskDelay(1);continue;}p+=w;n-=w;}}
static void wire(uint8_t kind,const char*json,size_t size,const uint8_t*raw,size_t nraw){
 uint8_t h[24]{};putLE(h,WIRE_MAGIC,4);h[4]=1;h[5]=kind;putLE(h+6,24,2);putLE(h+8,recordSequence++,4);putLE(h+12,size,4);putLE(h+16,nraw,4);
 uint32_t c=crcStep(0xffffffff,json,size);if(nraw)c=crcStep(c,raw,nraw);putLE(h+20,c^0xffffffff,4);
 writeAll(h,24);writeAll(reinterpret_cast<const uint8_t*>(json),size);if(nraw)writeAll(raw,nraw);
}
static void writer(void*){
 static char json[JSON_MAX+1];Out o;
 for(;;){if(xQueueReceive(outQ,&o,portMAX_DELAY)!=pdTRUE)continue;
  if(o.index==255)wire(1,o.text,strlen(o.text),nullptr,0);
  else {Record&r=pool[o.index];size_t n=0;
   if(serializeRecord(r,json,sizeof json,n)){wire(2,json,n,r.anchor?nullptr:&r.cir[0][0],r.anchor?0:4*CIR_BYTES);++sent;}
   else {++drops;const char e[]="{\"event\":\"SERIALIZE_OVERFLOW\"}";wire(1,e,sizeof e-1,nullptr,0);}
   const uint8_t i=o.index;xQueueSend(freeQ,&i,portMAX_DELAY);
  }--pending;
 }
}
static int reserve(){uint8_t i;if(xQueueReceive(freeQ,&i,0)!=pdTRUE){++drops;return -1;}new(&pool[i])Record();return i;}
static void submit(uint8_t i){
 const unsigned n=DUAL8_ANCHOR?1:2;for(unsigned j=0;j<n;++j)pool[i].cal[j]=port.calibration(j);
 Out o;o.index=i;++pending;if(xQueueSend(outQ,&o,0)!=pdTRUE){--pending;++drops;xQueueSend(freeQ,&i,0);}
}
static void idleAll(){port.idle(0);if(!DUAL8_ANCHOR)port.idle(1);}
static bool number(const char*s,uint32_t&v){
 if(!s||!*s||*s=='-'||*s=='+'){return false;}
 char*end;unsigned long long n=strtoull(s,&end,10);if(*end||n>0xffffffffULL)return false;v=uint32_t(n);return true;
}
static void hello(){emit("{\"event\":\"HELLO\",\"role\":\"%s\",\"firmware\":\"D8_R4_SOURCE_20261111\",\"mode\":8,\"wire\":\"RFB8\",\"hw_ok\":%s,\"fault_latched\":%s,\"boot\":%lu,\"config_id\":%lu,\"stage\":%u,\"period_us\":%lu,\"baud\":%lu,\"active\":%s,\"wifi_ota\":false,\"cal_default_is_uncalibrated\":true,\"drops\":%lu,\"sent\":%lu,\"period_miss\":%lu}",role(),hwOk?"true":"false",port.faulted()?"true":"false",(unsigned long)boot,(unsigned long)configId(config),config.stage,(unsigned long)config.period_us,(unsigned long)config.baud,active?"true":"false",(unsigned long)drops.load(),(unsigned long)sent.load(),(unsigned long)periodMiss);}
static void stop(uint32_t request=0){active=false;remaining=0;idleAll();emit("{\"event\":\"STOPPED\",\"role\":\"%s\",\"session\":%lu,\"stop_request\":%lu,\"physical_rf_off_confirmed\":false}",role(),(unsigned long)session,(unsigned long)request);}
static void command(char*line){
 char*save=nullptr;char*w[9]{};unsigned n=0;for(char*p=strtok_r(line," \t\r",&save);p;p=strtok_r(nullptr," \t\r",&save)){if(n==9){emit("{\"event\":\"ERROR\",\"reason\":\"TOO_MANY_TOKENS\"}");return;}w[n++]=p;}if(!n)return;
 if(!strcmp(w[0],"HELLO")||!strcmp(w[0],"STATUS")){hello();return;}
 if(!strcmp(w[0],"STOP")){
  uint32_t request=0;
  if(n==1||(n==2&&number(w[1],request)&&request))stop(request);
  else emit("{\"event\":\"ERROR\",\"reason\":\"BAD_STOP_REQUEST\"}");
  return;
 }
 if(!strcmp(w[0],"HELP")){emit("{\"event\":\"HELP\",\"commands\":\"HELLO; SELFTEST; SET STAGE|PERIOD_US|CIR_HZ|VERIFY_CIR value; BAUD 921600|2000000; ANT radio tx rx cal_id sigma_m; START session count; STOP [request]; RESET\"}");return;}
 if(active){emit("{\"event\":\"ERROR\",\"reason\":\"STOP_REQUIRED\"}");return;}
 if(!strcmp(w[0],"RESET")&&n==1){idleAll();hwOk=port.initialize();hello();return;}
 if(!strcmp(w[0],"SELFTEST")&&n==1){hwOk=port.selftest();emit("{\"event\":\"SELFTEST\",\"role\":\"%s\",\"pass\":%s,\"faults\":%lu,\"dev_a\":%lu,\"dev_b\":%lu,\"ant_a_tx\":%u,\"ant_a_rx\":%u,\"ant_b_tx\":%u,\"ant_b_rx\":%u}",role(),hwOk?"true":"false",(unsigned long)port.faults,(unsigned long)port.last_dev[0],(unsigned long)port.last_dev[1],port.last_ant_tx[0],port.last_ant_rx[0],port.last_ant_tx[1],port.last_ant_rx[1]);return;}
 if(!strcmp(w[0],"SET")&&n==3){uint32_t v;if(!number(w[2],v)){emit("{\"event\":\"ERROR\",\"reason\":\"BAD_INTEGER\"}");return;}Config next=config;
  if(!strcmp(w[1],"STAGE")){if(v<1||v>6)return;next.stage=uint8_t(v);}
  else if(!strcmp(w[1],"PERIOD_US"))next.period_us=v;
  else if(!strcmp(w[1],"CIR_HZ"))next.cir_hz=v;
  else if(!strcmp(w[1],"VERIFY_CIR")){if(v>1)return;next.verify_cir=v!=0;}
  else {emit("{\"event\":\"ERROR\",\"reason\":\"UNKNOWN_SET_KEY\"}");return;}
  const char*err=validate(next);if(err){emit("{\"event\":\"ERROR\",\"reason\":\"%s\"}",err);return;}
  config=next;port.configure(config);hello();return;
 }
 if(!strcmp(w[0],"BAUD")&&n==2){uint32_t v;if(!number(w[1],v)||(v!=921600&&v!=2000000)){emit("{\"event\":\"ERROR\",\"reason\":\"BAD_BAUD\"}");return;}Config next=config;next.baud=v;if(validate(next)){emit("{\"event\":\"ERROR\",\"reason\":\"PERIOD_UART_BUDGET\"}");return;}
  while(pending.load()){delay(1);}
  Serial.flush();Serial.end();Serial.begin(v);config=next;port.configure(config);hello();return;
 }
 if(!strcmp(w[0],"ANT")&&n==6){uint32_t i,tx,rx,id;if(!number(w[1],i)||!number(w[2],tx)||!number(w[3],rx)||!number(w[4],id)||i>=(DUAL8_ANCHOR?1U:2U)||tx>65535||rx>65535){emit("{\"event\":\"ERROR\",\"reason\":\"BAD_CALIBRATION\"}");return;}
  char*e=nullptr;double sigma=strtod(w[5],&e);if(*e||!isfinite(sigma)||sigma<=0||sigma>100){emit("{\"event\":\"ERROR\",\"reason\":\"BAD_SIGMA\"}");return;}
  Calibration c;c.tx=uint16_t(tx);c.rx=uint16_t(rx);c.id=id;c.range_sigma_m=sigma;hwOk=port.setCalibration(i,c);hello();return;
 }
 if(!strcmp(w[0],"START")&&n==3){uint32_t s,count;if(!number(w[1],s)||!number(w[2],count)||!s||count<1||count>1000000||config.stage<2||validate(config)||!hwOk||port.faulted()){
   emit("{\"event\":\"ERROR\",\"reason\":\"START_GUARD\"}");return;}
  session=s;remaining=count;targetCount=count;txn=0;peerBoot=0;periodMiss=0;nextStart=port.nowUs()+20000;runExpire=port.nowUs()+uint64_t(count)*config.period_us+10000000ULL;active=true;
  if(!DUAL8_ANCHOR){if(!port.arm(0)||!port.arm(1)){active=false;hwOk=false;idleAll();}}
  hello();return;
 }
 emit("{\"event\":\"ERROR\",\"reason\":\"UNKNOWN_COMMAND\"}");
}
static void service(){static char b[192];static size_t n=0;static bool overflow=false;
 while(Serial.available()){const int c=Serial.read();if(c=='\n'){if(!overflow){b[n]=0;command(b);}else emit("{\"event\":\"ERROR\",\"reason\":\"COMMAND_TOO_LONG\"}");n=0;overflow=false;}else if(c!='\r'){if(n+1<sizeof b)b[n++]=char(c);else overflow=true;}}
}
static void setup(){
 Serial.setRxBufferSize(2048);Serial.setTxBufferSize(8192);Serial.begin(config.baud);delay(300);
 freeQ=xQueueCreate(POOL,sizeof(uint8_t));outQ=xQueueCreate(12,sizeof(Out));
 if(!freeQ||!outQ){Serial.println("FATAL_QUEUE");while(true)delay(1000);}
 for(uint8_t i=0;i<POOL;++i)xQueueSend(freeQ,&i,0);
 if(xTaskCreatePinnedToCore(writer,"dual8_writer",8192,nullptr,1,nullptr,0)!=pdPASS){Serial.println("FATAL_WRITER");while(true)delay(1000);}
 boot=esp_random();if(!boot)boot=1;port.configure(config);hwOk=port.initialize();hello();
}
static void loop(){
 service();if(!active){delay(1);return;}
 if(port.nowUs()>runExpire){emit("{\"event\":\"SESSION_DEADLINE\"}");stop();return;}
#if DUAL8_ANCHOR
 if(port.nowUs()<nextStart){port.relax();return;}
 int idx=reserve();
 if(idx<0){nextStart=port.nowUs()+config.period_us;emit("{\"event\":\"BACKPRESSURE_NO_RF\"}");return;}
 runAnchor(port,config,session,txn++,boot,peerBoot,pool[idx]);if(pool[idx].peer_boot)peerBoot=pool[idx].peer_boot;submit(uint8_t(idx));
 nextStart+=config.period_us;if(port.nowUs()>=nextStart){++periodMiss;nextStart=port.nowUs()+config.period_us;}
 if(--remaining==0)stop();
#else
 for(uint8_t i=0;i<2;++i){Frame f;Rx o;const int rc=port.receive(i,f,o);
  if(rc==1){
   if(f.type==Type::Poll&&f.session==session&&f.config==configId(config)&&f.stage==config.stage&&f.txn>=txn&&f.txn<targetCount&&(!peerBoot||peerBoot==f.boot)){
    peerBoot=f.boot;txn=f.txn+1;int idx=reserve();if(idx<0){idleAll();emit("{\"event\":\"BACKPRESSURE_DROP_POLL\",\"txn\":%lu}",(unsigned long)f.txn);}
    else {runTag(port,config,boot,i,f,o,pool[idx]);submit(uint8_t(idx));}
    remaining=targetCount-txn;if(remaining==0){stop();break;}if(port.faulted()){active=false;hwOk=false;idleAll();break;}
    if(!port.arm(0)||!port.arm(1)){active=false;hwOk=false;idleAll();}break;
   }else if(!port.arm(i)){active=false;hwOk=false;idleAll();break;}
  }else if(rc==-1){if(!port.arm(i)){active=false;hwOk=false;idleAll();break;}}
  else if(rc==-2){active=false;hwOk=false;idleAll();break;}
 }
#endif
 if(port.faulted()){active=false;hwOk=false;idleAll();emit("{\"event\":\"HARDWARE_FAULT_LATCHED\",\"status_a\":%llu,\"status_b\":%llu}",(unsigned long long)port.fault_status[0],(unsigned long long)port.fault_status[1]);}
 port.relax();
}
} // namespace dual8app
void setup(){dual8app::setup();}
void loop(){dual8app::loop();}
